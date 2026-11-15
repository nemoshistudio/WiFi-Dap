#!/usr/bin/env python3
"""Persistent-session OpenOCD flash loop over the TCL RPC port.

Starts **one** OpenOCD process (``-c "tcl port <port>"``, gdb/telnet disabled),
keeps it alive and sends ``reset halt`` / ``flash write_image erase`` /
``verify_image`` / ``reset run`` per iteration over the Tcl RPC socket instead
of paying a full OpenOCD probe/init/startup for every flash.  Iteration times
are measured per phase and summarised (min/median/mean/total), with a JSON
report written next to the session log.

The RPC protocol is the OpenOCD TCL server one: a command is sent as text
terminated by ``0x1a`` and the response is returned up to the next ``0x1a``.
Each command is wrapped in ``catch`` so failures are visible in-band (the raw
protocol returns no status code).  Per-command socket timeouts, a startup
timeout and kill-on-exit child cleanup make a hung session impossible to leak.

Typical use (USBIP device already attached; OpenOCD usually needs root, e.g.
run the whole command through ``/tmp/hostroot.sh '' or sudo):

    # 5 full flashes in one session
    python3 scripts/h7_flash_loop.py --image <firmware.bin> --iterations 5

    # incremental: 1st iteration full, later iterations only changed sectors
    python3 scripts/h7_flash_loop.py --image <firmware.bin> --iterations 3 \\
        --incremental

    # alternate between two images: exercises the incremental path every run
    python3 scripts/h7_flash_loop.py --image a.bin --alternate-image b.bin \\
        --iterations 4 --incremental

    # offline self-test / plan only (no hardware, no OpenOCD process)
    python3 scripts/h7_flash_loop.py --self-test
    python3 scripts/h7_flash_loop.py --dry-run --image <firmware.bin>

Notes
-----
* ``--incremental`` uses the cache of the last successfully flashed image
  (``build/openocd-flash-cache``, shared with
  ``scripts/openocd_incremental_flash.py``).  Without a changed image an
  iteration is a no-op ("up-to-date") and only measures session overhead.
* The target is left **halted only during programming** and ``reset run`` is
  executed at the end of every iteration.
* Defaults point at ``build/openocd-master-install``; ``--candidate`` selects
  the pipelining speed build (``build/openocd-master-speed/install``, v2
  defaults pending_requests=4 + eager_reads enabled).

Exit codes: 0 all iterations ok, 1 iteration/session failure, 2 usage error.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

try:
    from openocd_incremental_flash import (  # noqa: E402
        DEFAULT_CACHE_DIR,
        FlashCache,
        build_plan,
        human_bytes,
        sha256_bytes,
        tcl_quote,
    )
except ImportError as exc:  # pragma: no cover - shipped side by side
    print(f"[h7-loop] ERROR: cannot import openocd_incremental_flash.py "
          f"(keep both scripts in scripts/): {exc}", file=sys.stderr)
    raise SystemExit(2)

DEFAULT_IMAGE = Path("~/firmware/reference/firmware.bin")
DEFAULT_OPENOCD = REPO_ROOT / "build/openocd-master-install/bin/openocd"
DEFAULT_SCRIPTS = REPO_ROOT / "build/openocd-master-install/share/openocd/scripts"
CANDIDATE_OPENOCD = REPO_ROOT / "build/openocd-master-speed/install/bin/openocd"
CANDIDATE_SCRIPTS = REPO_ROOT / "build/openocd-master-speed/install/share/openocd/scripts"

DEFAULT_BASE_ADDR = 0x08000000
DEFAULT_SECTOR_SIZE = 0x20000
DEFAULT_VID_PID = "0xc251 0xf00a"
DEFAULT_OUT_DIR = Path("/tmp/h7-flash-loop")

TCL_EOM = b"\x1a"
RPC_TOKEN = "DSH_BEGIN"
STARTUP_PING_TIMEOUT = 15.0  # socket timeout while waiting for the first RPC reply
VERIFY_TIME_RE = re.compile(r"verified\s+(\d+)\s+bytes\s+in\s+([0-9.]+)\s*s")


def log(msg: str) -> None:
    print(f"[h7-loop] {msg}", flush=True)


def die(msg: str, code: int = 2) -> "NoReturn":  # noqa: F821 (py<3.11)
    print(f"[h7-loop] ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def kib_per_s(nbytes: int, seconds: float) -> float:
    return 0.0 if seconds <= 0 else nbytes / 1024.0 / seconds


def pick_free_port(host: str = "127.0.0.1") -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


# ---------------------------------------------------------------------------
# TCL RPC client
# ---------------------------------------------------------------------------

class RpcError(RuntimeError):
    pass


class TclRpcClient:
    """Minimal OpenOCD TCL-server (tcl_port) RPC client."""

    def __init__(self, host: str = "127.0.0.1", port: int = 0,
                 command_timeout: float = 180.0):
        self.host = host
        self.port = port
        self.command_timeout = command_timeout
        self._sock = None
        self._buf = b""

    # -- low level ---------------------------------------------------------
    def connect(self, timeout: float = 5.0) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._sock = sock
        self._buf = b""

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        self._buf = b""

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def _recv_until_eom(self, timeout: float) -> str:
        assert self._sock is not None
        self._sock.settimeout(timeout)
        deadline = time.monotonic() + timeout
        while True:
            idx = self._buf.find(TCL_EOM)
            if idx >= 0:
                payload = self._buf[:idx]
                self._buf = self._buf[idx + 1:]
                return payload.decode("utf-8", "replace")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RpcError(f"RPC response timeout after {timeout:.1f}s")
            self._sock.settimeout(remaining)
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                raise RpcError(
                    f"RPC response timeout after {timeout:.1f}s") from None
            if not chunk:
                raise RpcError("OpenOCD closed the TCL RPC connection")
            self._buf += chunk

    def _send(self, text: str, timeout: float) -> str:
        if self._sock is None:
            raise RpcError("not connected")
        self._sock.settimeout(timeout)
        self._sock.sendall(text.encode("utf-8") + TCL_EOM)
        return self._recv_until_eom(timeout)

    # -- command level -----------------------------------------------------
    @staticmethod
    def wrap(command: str) -> str:
        """Wrap one OpenOCD command so its status/output comes back in-band."""
        return ("set __dsh_rc [catch {" + command + "} __dsh_out]; "
                'set __dsh_res "' + RPC_TOKEN + '\\n$__dsh_rc\\n$__dsh_out"; '
                "set __dsh_res")

    @staticmethod
    def parse_wrapped(response: str):
        marker = RPC_TOKEN + "\n"
        idx = response.find(marker)
        if idx < 0:
            return None
        rest = response[idx + len(marker):]
        rc_text, _, output = rest.partition("\n")
        try:
            return int(rc_text), output
        except ValueError:
            return None

    def command(self, command: str, timeout: float | None = None):
        """Run one command; return (rc, output)."""
        timeout = self.command_timeout if timeout is None else timeout
        raw = self._send(self.wrap(command), timeout)
        parsed = self.parse_wrapped(raw)
        if parsed is None:
            raise RpcError(
                f"unexpected RPC response for {command!r}: {raw[:200]!r}")
        return parsed

    def command_checked(self, command: str, timeout: float | None = None):
        rc, output = self.command(command, timeout)
        if rc != 0:
            raise RpcError(f"OpenOCD command failed (rc={rc}): {command}\n"
                           f"{output.strip()}")
        return output

    def ping(self, timeout: float | None = None) -> str:
        """Version string; used to wait for the server loop to be ready."""
        return self.command_checked("version", timeout).strip()

    def shutdown(self, timeout: float = 10.0) -> None:
        """Send the raw shutdown command (the server sends an empty reply and
        closes the connection)."""
        if self._sock is None:
            return
        try:
            self._send("shutdown", timeout)
        except (RpcError, OSError):
            pass
        finally:
            self.close()


# ---------------------------------------------------------------------------
# OpenOCD process / session management
# ---------------------------------------------------------------------------

@dataclass
class OpenOcdSession:
    client: TclRpcClient
    process: subprocess.Popen | None
    tcl_port: int
    log_path: Path
    log_handle: object = None
    argv: list = field(default_factory=list)
    startup_seconds: float = 0.0
    version: str = ""

    @property
    def pid(self) -> int | None:
        return None if self.process is None else self.process.pid

    def log_tail(self, lines: int = 15) -> str:
        try:
            text = self.log_path.read_text(encoding="utf-8",
                                           errors="replace")
        except OSError:
            return "<no session log>"
        return "\n".join(text.strip().splitlines()[-lines:])

    def alive(self) -> bool:
        if self.process is None:
            return self.client.connected
        return self.process.poll() is None


def build_openocd_argv(cfg, tcl_port: int) -> list:
    argv = [
        str(cfg.openocd),
        "-f", cfg.interface_cfg,
        "-c", f"cmsis_dap_vid_pid {cfg.vid_pid}",
        "-c", "transport select swd",
        "-f", cfg.target_cfg,
        "-c", f"adapter speed {cfg.adapter_speed}",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
    ]
    for command in cfg.extra_config:
        argv += ["-c", command]
    # Disable the other servers so only the TCL RPC endpoint is exposed; the
    # TCL port must be configured before the automatic init of the session.
    argv += ["-c", "gdb port disabled",
             "-c", "telnet port disabled",
             "-c", f"tcl port {tcl_port}"]
    return argv


def start_session(cfg, out_dir: Path) -> OpenOcdSession:
    if not cfg.openocd.is_file():
        die(f"OpenOCD binary not found: {cfg.openocd}")
    if not os.access(cfg.openocd, os.X_OK):
        die(f"OpenOCD binary is not executable: {cfg.openocd}")
    if not cfg.scripts.is_dir():
        die(f"OpenOCD scripts directory not found: {cfg.scripts}")

    out_dir.mkdir(parents=True, exist_ok=True)
    tcl_port = cfg.tcl_port or pick_free_port(cfg.tcl_host)
    log_path = out_dir / "openocd-session.log"
    log_handle = log_path.open("w", encoding="utf-8", errors="replace")
    argv = build_openocd_argv(cfg, tcl_port)
    env = os.environ.copy()
    env["OPENOCD_SCRIPTS"] = str(cfg.scripts)
    if cfg.verbose:
        log(f"starting: {' '.join(argv)}")
    started = time.monotonic()
    try:
        process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                   stdout=log_handle,
                                   stderr=subprocess.STDOUT,
                                   start_new_session=True)
    except OSError as exc:
        log_handle.close()
        die(f"cannot start OpenOCD: {exc}")

    client = TclRpcClient(cfg.tcl_host, tcl_port, cfg.command_timeout)
    deadline = time.monotonic() + cfg.startup_timeout
    last_error = "no connection"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            tail = _tail_file(log_path)
            log_handle.close()
            die(f"OpenOCD exited during startup (rc={process.returncode}); "
                f"log tail:\n{tail}")
        try:
            client.connect(timeout=min(2.0, cfg.startup_timeout))
        except OSError as exc:
            last_error = str(exc)
            time.sleep(0.1)
            continue
        try:
            version = client.ping(
                timeout=min(STARTUP_PING_TIMEOUT, cfg.startup_timeout))
            startup = time.monotonic() - started
            log(f"OpenOCD ready in {startup:.3f} s: {version} "
                f"(pid={process.pid}, tcl_port={tcl_port})")
            return OpenOcdSession(client=client, process=process,
                                  tcl_port=tcl_port, log_path=log_path,
                                  log_handle=log_handle, argv=argv,
                                  startup_seconds=startup,
                                  version=version)
        except RpcError as exc:
            last_error = str(exc)
            client.close()
            time.sleep(0.1)
    tail = _tail_file(log_path)
    log_handle.close()
    _kill_process(process)
    die(f"OpenOCD TCL RPC not ready after {cfg.startup_timeout:.0f}s "
        f"({last_error}); log tail:\n{tail}")


def _tail_file(path: Path, lines: int = 15) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "<no log>"
    return "\n".join(text.strip().splitlines()[-lines:])


def _kill_process(process: subprocess.Popen | None,
                  grace: float = 3.0) -> None:
    if process is None:
        return
    if process.poll() is not None:
        return
    try:
        process.terminate()
    except OSError:
        pass
    try:
        process.wait(timeout=grace)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        process.kill()
    except OSError:
        return
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass


def stop_session(session: OpenOcdSession | None, cfg) -> None:
    if session is None:
        return
    try:
        if cfg.keep_running and session.process is not None:
            if session.process.poll() is None:
                log(f"--keep-running: leaving OpenOCD alive "
                    f"(pid={session.process.pid}, "
                    f"tcl_port={session.tcl_port}, log={session.log_path})")
                session.client.close()
                if session.log_handle is not None:
                    session.log_handle.close()
                return
        try:
            session.client.shutdown(timeout=5.0)
        except Exception:  # noqa: BLE001 - best effort teardown
            pass
        _kill_process(session.process)
        try:
            session.client.close()
        except Exception:  # noqa: BLE001
            pass
        if session.log_handle is not None:
            try:
                session.log_handle.close()
            except Exception:  # noqa: BLE001
                pass
    finally:
        pass


# ---------------------------------------------------------------------------
# planning / iteration execution
# ---------------------------------------------------------------------------

@dataclass
class IterationResult:
    index: int                 # 1-based measured index
    loop_index: int            # 0-based position in the whole loop
    warmup: bool
    mode: str
    image_sha256: str
    write_seconds: float = 0.0
    verify_seconds: float = 0.0
    total_seconds: float = 0.0
    target_verify_seconds: float | None = None
    verified: bool = False
    ok: bool = False
    error: str = ""
    commands: list = field(default_factory=list)

    def as_dict(self) -> dict:
        return {
            "iteration": self.index,
            "loop_index": self.loop_index,
            "warmup": self.warmup,
            "mode": self.mode,
            "image_sha256": self.image_sha256,
            "write_seconds": round(self.write_seconds, 3),
            "verify_seconds": round(self.verify_seconds, 3),
            "total_seconds": round(self.total_seconds, 3),
            "target_verify_seconds": self.target_verify_seconds,
            "verified": self.verified,
            "ok": self.ok,
            "error": self.error,
            "commands": self.commands,
        }


class IterationFailure(RuntimeError):
    pass


def _parse_target_verify_seconds(outputs: list) -> float | None:
    total = 0.0
    found = False
    for output in outputs:
        for _bytes, seconds in VERIFY_TIME_RE.findall(output or ""):
            total += float(seconds)
            found = True
    return total if found else None


def _write_iteration_files(cfg, img: bytes, plan, iter_token: str,
                           tmpdir: Path):
    """Materialise plan runs/verify targets as files; return command lists."""
    writes, verifies = [], []
    for i, run in enumerate(plan.runs):
        path = tmpdir / f"{iter_token}-w{i}.bin"
        path.write_bytes(run.data)
        command = (f"flash write_image erase {tcl_quote(str(path))} "
                   f"0x{run.addr:08x}")
        writes.append((command, run.length))
    for i, run in enumerate(plan.verify_targets):
        path = tmpdir / f"{iter_token}-v{i}.bin"
        path.write_bytes(run.data)
        command = (f"verify_image {tcl_quote(str(path))} "
                   f"0x{run.addr:08x}")
        verifies.append(command)
    return writes, verifies


def run_iteration(cfg, client, cache, img: bytes, loop_index: int,
                  measured_index: int, warmup: bool, tmpdir: Path,
                  iter_token: str) -> IterationResult:
    image_sha = sha256_bytes(img)
    if cache is not None:
        # Refresh cache.image for this image length (validated sidecar read).
        cache.load(len(img))
        plan = build_plan(img, cache, cfg.sector_size, cfg.base_addr,
                          cfg.verify, cfg.verify_scope, False,
                          cfg.erase_trailing)
    else:
        # Non-incremental: fresh empty cache forced to "full" semantics.
        scratch = FlashCache(tmpdir / f"nocache-{iter_token}",
                             cfg.base_addr, cfg.sector_size)
        plan = build_plan(img, scratch, cfg.sector_size, cfg.base_addr,
                          cfg.verify, cfg.verify_scope, True, False)

    result = IterationResult(index=measured_index, loop_index=loop_index,
                             warmup=warmup, mode=plan.mode,
                             image_sha256=image_sha)
    writes, verifies = _write_iteration_files(cfg, img, plan, iter_token,
                                              tmpdir)
    result.commands = (["reset halt"] + [c for c, _n in writes]
                       + verifies + ["reset run"])
    started = time.perf_counter()
    try:
        client.command_checked("reset halt")
        for command, _size in writes:
            t0 = time.perf_counter()
            out = client.command_checked(command)
            result.write_seconds += time.perf_counter() - t0
            if cfg.verbose and out.strip():
                log(f"  write: {out.strip()}")
        verify_outputs = []
        for command in verifies:
            t0 = time.perf_counter()
            out = client.command_checked(command)
            result.verify_seconds += time.perf_counter() - t0
            verify_outputs.append(out)
        result.verified = bool(verifies)
        result.target_verify_seconds = _parse_target_verify_seconds(
            verify_outputs)
        if verifies and result.target_verify_seconds is None and cfg.verbose:
            log("  note: verify command output had no 'verified N bytes in "
                "Xs' line (timing absent)")
        client.command_checked("reset run")
        result.ok = True
    except (RpcError, OSError) as exc:
        result.error = str(exc)
        log(f"iteration {measured_index} FAILED: {exc}")
    finally:
        result.total_seconds = time.perf_counter() - started
    return result


def run_loop(cfg, images: list, client, startup_seconds: float = 0.0,
             session: OpenOcdSession | None = None, out_dir: Path | None = None,
             tmpdir: Path | None = None) -> dict:
    """Run cfg.warmup + cfg.iterations iterations over one RPC client."""

    def emit(msg: str) -> None:
        if not getattr(cfg, "quiet", False):
            log(msg)

    if out_dir is None:
        out_dir = cfg.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp_holder = None
    if tmpdir is None:
        tmp_holder = tempfile.TemporaryDirectory(prefix="h7-loop-")
        tmpdir = Path(tmp_holder.name)

    cache = None
    if cfg.incremental:
        cache = FlashCache(cfg.cache_dir, cfg.base_addr, cfg.sector_size)
        cached = cache.load(len(images[0]))
        emit(f"incremental mode; cache {cache.cache_dir}: {cache.reason}"
             + (f" ({len(cached)} B cached)" if cached is not None else ""))

    if session is not None:
        startup_seconds = session.startup_seconds
    started = time.monotonic()
    total_runs = cfg.warmup + cfg.iterations
    results = []
    try:
        for loop_index in range(total_runs):
            warmup = loop_index < cfg.warmup
            measured_index = loop_index - cfg.warmup + 1
            img = images[loop_index % len(images)]
            token = f"iter{loop_index:03d}"
            rec = run_iteration(cfg, client, cache, img, loop_index,
                                measured_index, warmup, tmpdir, token)
            results.append(rec)
            if rec.ok and cache is not None:
                cache.save(img, origin=f"h7_flash_loop iteration "
                                       f"{measured_index}")
            if not rec.ok:
                if not cfg.keep_going:
                    emit("aborting the loop after the failed iteration "
                 "(use --keep-going to continue)")
                    break
                continue
            speed = kib_per_s(len(img), rec.write_seconds)
            emit(f"iter {measured_index}/{cfg.iterations}"
                f"{' (warmup)' if warmup else ''}: {rec.mode:<12} "
                f"write={rec.write_seconds:7.3f}s verify={rec.verify_seconds:6.3f}s"
                f" total={rec.total_seconds:7.3f}s "
                f"{speed:6.1f} KiB/s")
    finally:
        if tmp_holder is not None:
            tmp_holder.cleanup()

    measured = [r for r in results if not r.warmup]
    ok_runs = [r for r in measured if r.ok]
    totals = [r.total_seconds for r in ok_runs]
    writes = [r.write_seconds for r in ok_runs]
    verifies = [r.verify_seconds for r in ok_runs]

    def stats(values: list) -> dict:
        if not values:
            return {"runs": 0}
        return {
            "runs": len(values),
            "min_s": min(values),
            "median_s": statistics.median(values),
            "mean_s": statistics.mean(values),
            "stdev_s": statistics.stdev(values) if len(values) > 1 else 0.0,
        }

    first = images[0]
    summary = {
        "schema": 1,
        "tool": "h7_flash_loop",
        "timestamp": utc_now(),
        "label": cfg.label,
        "transport": {
            "tcl_rpc": f"{cfg.tcl_host}:"
                       f"{session.tcl_port if session else 0}",
            "startup_seconds": round(startup_seconds, 3),
            "openocd": str(cfg.openocd),
            "openocd_version": session.version if session else "",
            "scripts": str(cfg.scripts),
            "adapter_speed_khz": cfg.adapter_speed,
        },
        "image": {
            "path": str(cfg.image) if cfg.image else "<synthetic>",
            "size": len(first),
            "sha256": sha256_bytes(first),
            "base_addr": f"0x{cfg.base_addr:08x}",
            "sector_size": cfg.sector_size,
            "alternate_image": (str(cfg.alternate_image)
                                if cfg.alternate_image else ""),
        },
        "incremental": cfg.incremental,
        "verify": cfg.verify,
        "verify_scope": cfg.verify_scope,
        "iterations": [r.as_dict() for r in results],
        "measured": stats(totals),
        "write": stats(writes),
        "verify_phase": stats(verifies),
        "wall_seconds": round(time.monotonic() - started, 3),
        "ok": len(ok_runs) == cfg.iterations and len(measured) == cfg.iterations,
    }
    if not getattr(cfg, "quiet", False):
        _print_summary(cfg, measured, summary)
    return summary


def _print_summary(cfg, measured: list, summary: dict) -> None:
    print()
    print(f"{'iter':>4}  {'mode':<12}{'write_s':>10}{'verify_s':>10}"
          f"{'total_s':>10}  {'KiB/s':>8}  note")
    for rec in measured:
        note = "ok" if rec.ok else "FAILED"
        if rec.mode == "up-to-date":
            note = "up to date"
        speed = 0.0
        if rec.write_seconds > 0:
            speed = kib_per_s(summary["image"]["size"], rec.write_seconds)
        print(f"{rec.index:>4}  {rec.mode:<12}{rec.write_seconds:>10.3f}"
              f"{rec.verify_seconds:>10.3f}{rec.total_seconds:>10.3f}"
              f"  {speed:>8.1f}  {note}")
        if not rec.ok and rec.error:
            print(f"      error: {rec.error.splitlines()[0]}")
    stats = summary["measured"]
    if stats.get("runs"):
        print(f"\ntotal measured: n={stats['runs']} "
              f"min={stats['min_s']:.3f}s median={stats['median_s']:.3f}s "
              f"mean={stats['mean_s']:.3f}s stdev={stats['stdev_s']:.3f}s "
              f"wall={summary['wall_seconds']:.3f}s")
    if not summary["ok"]:
        print("\nresult: FAIL")


def write_outputs(cfg, summary: dict, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = cfg.json_out or (out_dir / "result.json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    log(f"report: {path}")
    return path


# ---------------------------------------------------------------------------
# fake TCL RPC server (offline tests / --fake-rpc)
# ---------------------------------------------------------------------------

class FakeTclServer(threading.Thread):
    """Tiny TCL-server emulator used for offline end-to-end tests."""

    def __init__(self, delay: float = 0.005, fail_verify: int = 0,
                 fail_command: str = ""):
        super().__init__(daemon=True)
        self.delay = delay
        self.fail_verify = fail_verify          # fail this many verify cmds
        self.fail_command = fail_command        # substring -> one failure
        self.commands = []
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(1)
        self.port = self._srv.getsockname()[1]
        self._stop_evt = threading.Event()
        self._conn = None

    def run(self) -> None:
        try:
            self._srv.settimeout(0.2)
            while not self._stop_evt.is_set():
                try:
                    conn, _addr = self._srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                self._conn = conn
                self._serve(conn)
                break
        finally:
            try:
                self._srv.close()
            except OSError:
                pass

    def _serve(self, conn: socket.socket) -> None:
        buf = b""
        conn.settimeout(5.0)
        try:
            while not self._stop_evt.is_set():
                chunk = conn.recv(65536)
                if not chunk:
                    return
                buf += chunk
                while TCL_EOM in buf:
                    idx = buf.find(TCL_EOM)
                    raw = buf[:idx].decode("utf-8", "replace")
                    buf = buf[idx + 1:]
                    if self.delay:
                        time.sleep(self.delay)
                    self.commands.append(raw)
                    if raw.strip() == "shutdown":
                        conn.sendall(TCL_EOM)
                        return
                    response = self._respond(raw)
                    conn.sendall(response.encode("utf-8") + TCL_EOM)
        except OSError:
            return
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _respond(self, raw: str) -> str:
        if "verify_image" in raw and self.fail_verify > 0:
            self.fail_verify -= 1
            return (f"{RPC_TOKEN}\n1\n"
                    "checksum mismatch - attempting binary compare\n"
                    "diff 0 address 0x08000010. Was 0xff instead of 0x00")
        if self.fail_command and self.fail_command in raw:
            self.fail_command = None
            return f"{RPC_TOKEN}\n1\nsimulated command failure"
        if "verify_image" in raw:
            return (f"{RPC_TOKEN}\n0\n"
                    "verified 345572 bytes in 0.100000s (3374.7 KiB/s)")
        return f"{RPC_TOKEN}\n0\n"

    def stop(self) -> None:
        self._stop_evt.set()
        try:
            if self._conn is not None:
                self._conn.close()
        except OSError:
            pass
        try:
            self._srv.close()
        except OSError:
            pass


def _synthetic_image(size: int, seed: int) -> bytes:
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(size))


# ---------------------------------------------------------------------------
# offline self-test
# ---------------------------------------------------------------------------

class _SelfTestConfig:
    """LoopConfig-compatible minimal config for offline tests."""

    def __init__(self, tmp: Path):
        self.image = None
        self.alternate_image = None
        self.iterations = 2
        self.warmup = 0
        self.base_addr = DEFAULT_BASE_ADDR
        self.sector_size = DEFAULT_SECTOR_SIZE
        self.cache_dir = tmp / "cache"
        self.openocd = tmp / "fake-openocd"
        self.scripts = tmp
        self.interface_cfg = "interface/cmsis-dap.cfg"
        self.target_cfg = "target/stm32h7x.cfg"
        self.adapter_speed = 1900
        self.vid_pid = DEFAULT_VID_PID
        self.extra_config = []
        self.tcl_host = "127.0.0.1"
        self.tcl_port = 0
        self.startup_timeout = 10.0
        self.command_timeout = 30.0
        self.keep_running = False
        self.incremental = False
        self.verify = True
        self.verify_scope = "changed"
        self.erase_trailing = False
        self.keep_going = False
        self.label = "self-test"
        self.json_out = None
        self.out_dir = tmp / "out"
        self.verbose = False
        self.quiet = True


def _connect_fake(server: FakeTclServer,
                  timeout: float = 5.0) -> TclRpcClient:
    client = TclRpcClient("127.0.0.1", server.port, command_timeout=10.0)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            client.connect(timeout=1.0)
            client.ping(timeout=3.0)
            return client
        except (OSError, RpcError):
            client.close()
            time.sleep(0.02)
    raise AssertionError("fake TCL server not reachable")


def self_test() -> int:
    checks = 0
    failures = []

    def check(cond: bool, name: str) -> None:
        nonlocal checks
        checks += 1
        if cond:
            print(f"  ok   {name}")
        else:
            failures.append(name)
            print(f"  FAIL {name}")

    print("self-test: TCL RPC client / loop / incremental / cleanup")

    # 1) in-band catch wrapper round-trip
    wrapped = TclRpcClient.wrap("flash write_image erase {/tmp/a b.bin} "
                                "0x08000000")
    check("catch {" in wrapped and RPC_TOKEN in wrapped,
          "catch wrapper is built")
    parsed = TclRpcClient.parse_wrapped(f"{RPC_TOKEN}\n0\nhello\nworld")
    check(parsed == (0, "hello\nworld"), "wrapped response parsed")
    check(TclRpcClient.parse_wrapped("garbage") is None,
          "bad response rejected")

    with tempfile.TemporaryDirectory(prefix="h7-loop-selftest-") as td:
        tdir = Path(td)
        cfg = _SelfTestConfig(tdir)

        # 2) end-to-end loop over a fake TCL server (non-incremental)
        image = _synthetic_image(2 * cfg.sector_size + 5000, seed=1)
        server = FakeTclServer(delay=0.001)
        server.start()
        client = _connect_fake(server)
        summary = run_loop(cfg, [image], client, out_dir=tdir / "out")
        check(summary["ok"] and len(summary["iterations"]) == 2,
              "fake RPC: 2 loop iterations completed")
        check(summary["measured"]["median_s"] > 0
              and summary["write"]["runs"] == 2,
              "fake RPC: per-iteration timings recorded")
        check(summary["iterations"][0]["mode"] == "full"
              and len(summary["iterations"][0]["commands"]) == 4,
              "fake RPC: full mode = reset halt/write/verify/reset run")
        check(any("flash write_image erase" in c
                  for c in server.commands),
              "fake RPC: write command sent over the socket")
        check(any("verify_image" in c for c in server.commands),
              "fake RPC: verify command sent over the socket")
        check(any("version" in c for c in server.commands),
              "fake RPC: startup version ping sent")
        client.shutdown()
        server.stop()
        server.join(timeout=2)

        # 3) fake RPC incremental: 1st full, 2nd up-to-date
        cfg2 = _SelfTestConfig(tdir / "inc")
        cfg2.incremental = True
        cfg2.out_dir = tdir / "inc-out"
        server2 = FakeTclServer(delay=0.001)
        server2.start()
        client2 = _connect_fake(server2)
        summary2 = run_loop(cfg2, [image], client2,
                            out_dir=tdir / "inc-out")
        check([r["mode"] for r in summary2["iterations"]]
              == ["full", "up-to-date"],
              "incremental: full then up-to-date")
        cache = FlashCache(cfg2.cache_dir, cfg2.base_addr, cfg2.sector_size)
        check(cache.load(len(image)) == image,
              "incremental: cache updated after the successful iteration")
        client2.shutdown()
        server2.stop()
        server2.join(timeout=2)

        # 4) fake RPC two-image alternation exercises changed-sector path
        cfg3 = _SelfTestConfig(tdir / "alt")
        cfg3.incremental = True
        cfg3.out_dir = tdir / "alt-out"
        img_b = bytearray(image)
        img_b[cfg3.sector_size + 7] ^= 0xFF
        img_b = bytes(img_b)
        server3 = FakeTclServer(delay=0.001)
        server3.start()
        client3 = _connect_fake(server3)
        summary3 = run_loop(cfg3, [image, img_b], client3,
                            out_dir=tdir / "alt-out")
        modes = [r["mode"] for r in summary3["iterations"]]
        check(modes[0] == "full"
              and all(m in ("incremental", "full") for m in modes[1:]),
              "alternate images: incremental changed-sector iterations")
        write_cmds = [c for c in server3.commands
                      if "flash write_image erase" in c]
        check(modes[1] == "incremental" and len(write_cmds) == 2,
              "alternate: iteration 2 writes only the changed sector")
        client3.shutdown()
        server3.stop()
        server3.join(timeout=2)

        # 5) failure handling: verify fails -> iteration failed, loop aborts,
        #    child/socket cleanup completes (no hang)
        cfg4 = _SelfTestConfig(tdir / "fail")
        cfg4.out_dir = tdir / "fail-out"
        server4 = FakeTclServer(delay=0.001, fail_verify=1)
        server4.start()
        client4 = _connect_fake(server4)
        started = time.monotonic()
        summary4 = run_loop(cfg4, [image], client4,
                            out_dir=tdir / "fail-out")
        check(not summary4["ok"] and summary4["iterations"][0]["ok"] is False,
              "failure: failed verify reported and loop aborted")
        check("checksum mismatch" in summary4["iterations"][0]["error"],
              "failure: verify output attached to the error")
        check(time.monotonic() - started < 10,
              "failure: loop aborts quickly (no hang)")
        client4.shutdown()
        server4.stop()
        server4.join(timeout=2)

        # 6) output JSON + summary helpers
        cfg5 = _SelfTestConfig(tdir / "json")
        cfg5.out_dir = tdir / "json-out"
        summary5 = {"image": {"size": len(image)}, "measured": {"runs": 1}}
        path = write_outputs(cfg5, summary5, cfg5.out_dir)
        check(path.is_file()
              and json.loads(path.read_text())["image"]["size"] == len(image),
              "report JSON written")
        stats = {"runs": 0}
        check(stats == {"runs": 0}, "empty stats helper sanity")

        # 7) OpenOCD argv construction/port selection
        cfg6 = _SelfTestConfig(tdir)
        argv = build_openocd_argv(cfg6, 6666)
        check(argv[0].endswith("fake-openocd")
              and "tcl port 6666" in argv
              and "gdb port disabled" in argv
              and "telnet port disabled" in argv,
              "OpenOCD argv: tcl port configured, gdb/telnet disabled")
        check(f"adapter speed {cfg6.adapter_speed}" in argv,
              "OpenOCD argv: adapter speed present")
        port = pick_free_port()
        check(1 <= port <= 65535, "free TCP port picked")

    if failures:
        print(f"self-test: FAIL ({len(failures)} of {checks} checks failed: "
              f"{', '.join(failures)})")
        return 1
    print(f"self-test OK ({checks} checks)")
    return 0


# ---------------------------------------------------------------------------
# CLI / main
# ---------------------------------------------------------------------------

def parse_args(argv: list):
    ap = argparse.ArgumentParser(
        description="Persistent-session OpenOCD flash loop over the TCL RPC "
                    "port (one OpenOCD process, many flash iterations).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Typical use", 1)[-1])
    ap.add_argument("--image", type=Path, default=DEFAULT_IMAGE,
                    help=f"firmware image (default: {DEFAULT_IMAGE})")
    ap.add_argument("--alternate-image", type=Path, default=None,
                    help="optional second image; iterations alternate "
                         "between the two (exercises the incremental path)")
    ap.add_argument("--iterations", type=int, default=5,
                    help="measured flash iterations (default: 5)")
    ap.add_argument("--warmup", type=int, default=0,
                    help="discarded warm-up iterations (default: 0)")
    ap.add_argument("--base-addr", type=lambda v: int(v, 0),
                    default=DEFAULT_BASE_ADDR,
                    help="flash base address (default 0x08000000)")
    ap.add_argument("--sector-size", type=lambda v: int(v, 0),
                    default=DEFAULT_SECTOR_SIZE,
                    help="flash sector size in bytes (default 0x20000)")
    ap.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR,
                    help=f"incremental cache dir (default: {DEFAULT_CACHE_DIR})")
    ap.add_argument("--incremental", action="store_true",
                    help="reuse the changed-sector cache in the persistent "
                         "session (shared with openocd_incremental_flash.py)")
    ap.add_argument("--openocd", type=Path, default=DEFAULT_OPENOCD,
                    help=f"OpenOCD binary (default: {DEFAULT_OPENOCD})")
    ap.add_argument("--scripts", type=Path, default=DEFAULT_SCRIPTS,
                    help=f"OPENOCD_SCRIPTS directory (default: {DEFAULT_SCRIPTS})")
    ap.add_argument("--candidate", action="store_true",
                    help=f"use the speed build ({CANDIDATE_OPENOCD})")
    ap.add_argument("--interface-cfg", default="interface/cmsis-dap.cfg")
    ap.add_argument("--target-cfg", default="target/stm32h7x.cfg")
    ap.add_argument("--adapter-speed", type=int, default=1900,
                    help="SWD clock in kHz (default: 1900)")
    ap.add_argument("--vid-pid", default=DEFAULT_VID_PID,
                    help="CMSIS-DAP USB VID/PID (default: 0xc251 0xf00a)")
    ap.add_argument("--openocd-config", action="append", default=[],
                    metavar="TCL",
                    help="extra -c config command before init (repeatable), "
                         'e.g. "cmsis-dap pending_requests 4"')
    ap.add_argument("--tcl-host", default="127.0.0.1",
                    help="host of the TCL RPC socket (default: 127.0.0.1)")
    ap.add_argument("--tcl-port", type=int, default=0,
                    help="TCL RPC port (default: 0 = pick a free port)")
    ap.add_argument("--startup-timeout", type=float, default=60.0,
                    help="max seconds to wait for probe/init/TCL RPC "
                         "(default: 60)")
    ap.add_argument("--command-timeout", type=float, default=180.0,
                    help="per-RPC-command timeout seconds (default: 180)")
    verify = ap.add_mutually_exclusive_group()
    verify.add_argument("--verify", dest="verify", action="store_true",
                        default=True, help="verify after each write (default)")
    verify.add_argument("--no-verify", dest="verify", action="store_false",
                        help="skip verify_image")
    ap.add_argument("--verify-scope", choices=("changed", "full"),
                    default="changed",
                    help="verify changed ranges only (default) or the whole "
                         "image; non-incremental always verifies the image")
    ap.add_argument("--erase-trailing", action="store_true",
                    help="with --incremental, erase old trailing sectors when "
                         "the image shrank (0xFF padding)")
    ap.add_argument("--keep-running", action="store_true",
                    help="leave the OpenOCD process running after the loop "
                         "(prints pid/tcl_port); default: shut it down")
    ap.add_argument("--keep-going", action="store_true",
                    help="continue after a failed iteration")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT_DIR,
                    help=f"output directory for result.json + session log "
                         f"(default: {DEFAULT_OUT_DIR})")
    ap.add_argument("--json-out", type=Path, default=None,
                    help="explicit result JSON path (overrides --out)")
    ap.add_argument("--label", default="run", help="label for the report")
    ap.add_argument("--expect-sha256", default="",
                    help="abort if the image sha256 does not match")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan + run offline self-tests; never "
                         "starts OpenOCD or touches hardware")
    ap.add_argument("--self-test", action="store_true",
                    help="run the offline self-test (incl. fake TCL RPC "
                         "end-to-end) and exit")
    ap.add_argument("--fake-rpc", action="store_true",
                    help="run the loop against the internal fake TCL RPC "
                         "server (offline end-to-end test, no hardware)")
    ap.add_argument("--verbose", action="store_true",
                    help="print OpenOCD argv and per-command output")
    args = ap.parse_args(argv)

    if args.candidate:
        args.openocd = CANDIDATE_OPENOCD
        args.scripts = CANDIDATE_SCRIPTS
    args.extra_config = list(args.openocd_config)
    if args.iterations < 1:
        die("--iterations must be >= 1")
    if args.warmup < 0:
        die("--warmup must be >= 0")
    if args.startup_timeout < 5:
        die("--startup-timeout must be >= 5 s")
    if args.command_timeout < 5:
        die("--command-timeout must be >= 5 s")
    if args.base_addr % args.sector_size:
        die("--base-addr must be sector aligned")
    return args


def _load_images(args, synthetic_size: int | None = None) -> list:
    if synthetic_size is not None:
        return [_synthetic_image(synthetic_size, seed=42)]
    if not args.image.is_file():
        die(f"image not found: {args.image}")
    data = args.image.read_bytes()
    if not data:
        die(f"empty image: {args.image}")
    images = [data]
    if args.alternate_image is not None:
        if not args.alternate_image.is_file():
            die(f"alternate image not found: {args.alternate_image}")
        alt = args.alternate_image.read_bytes()
        if not alt:
            die(f"empty alternate image: {args.alternate_image}")
        images.append(alt)
    if args.expect_sha256:
        for image in images:
            if sha256_bytes(image) != args.expect_sha256.lower():
                die(f"image sha256 mismatch: got {sha256_bytes(image)}, "
                    f"expected {args.expect_sha256}")
    return images


def dry_run(args) -> int:
    log("DRY RUN: no OpenOCD started, no hardware touched")
    images = _load_images(args)
    image = images[0]
    size = len(image)
    log(f"image: {args.image} size={size} B sha256={sha256_bytes(image)[:16]}"
        f"... base=0x{args.base_addr:08x} sector=0x{args.sector_size:x}")
    tcl_port = args.tcl_port or pick_free_port(args.tcl_host)
    argv = build_openocd_argv(args, tcl_port)
    log(f"openocd: {args.openocd} (exists={args.openocd.is_file()})")
    log(f"scripts: {args.scripts} (exists={args.scripts.is_dir()})")
    log(f"session: one process, tcl_port={tcl_port}, "
        f"iterations={args.iterations} warmup={args.warmup} "
        f"incremental={args.incremental} verify={args.verify}"
        f"({args.verify_scope})")
    print(f"\nsession argv: {' '.join(argv)}\n")

    sim_cache_dir = Path(tempfile.mkdtemp(prefix="h7-loop-dry-cache-"))
    cache = FlashCache(sim_cache_dir, args.base_addr, args.sector_size)
    cached = cache.load(size) if args.incremental else None
    if args.incremental:
        log(f"incremental cache (simulated, real cache untouched): "
            f"{cache.reason}"
            + (f" ({len(cached)} B)" if cached is not None else ""))
    for loop_index in range(min(args.warmup + args.iterations, 3)):
        img = images[loop_index % len(images)]
        if args.incremental:
            plan = build_plan(img, cache, args.sector_size, args.base_addr,
                              args.verify, args.verify_scope, False,
                              args.erase_trailing)
        else:
            scratch = FlashCache(Path("/tmp/h7-loop-dry-nocache"),
                                 args.base_addr, args.sector_size)
            plan = build_plan(img, scratch, args.sector_size, args.base_addr,
                              args.verify, args.verify_scope, True, False)
        log(f"iteration {loop_index + 1}: mode={plan.mode} "
            f"write={human_bytes(plan.bytes_to_write)} "
            f"verify_targets={len(plan.verify_targets)}")
        commands = (["reset halt"]
                    + [f"flash write_image erase <run {i} {human_bytes(len(r.data))}"
                       f" @0x{r.addr:08x}>" for i, r in enumerate(plan.runs)]
                    + [f"verify_image <run {i} {human_bytes(len(r.data))}"
                       f" @0x{r.addr:08x}>"
                       for i, r in enumerate(plan.verify_targets)]
                    + ["reset run"])
        for command in commands:
            print(f"  rpc: {command}")
        # update the simulated cache so iteration 2 shows the next-mode plan
        if args.incremental:
            cache.save(img, origin="dry-run")
            cache.load(size)
    print()
    return 0 if self_test() == 0 else 1


def main(argv: list) -> int:
    args = parse_args(argv)
    if args.self_test and not args.dry_run and not args.fake_rpc:
        return self_test()
    if args.dry_run:
        return dry_run(args)

    if args.fake_rpc:
        # Use the real image when present, a synthetic one otherwise; the
        # fake server never talks to hardware.
        if args.image.is_file():
            images = _load_images(args)
        else:
            images = _load_images(args, synthetic_size=3 * args.sector_size)
        if args.cache_dir == DEFAULT_CACHE_DIR:
            # Never pollute the real incremental cache from a fake run.
            args.cache_dir = Path(tempfile.mkdtemp(prefix="h7-loop-fake-cache-"))
        out_dir = args.out
        server = FakeTclServer(delay=0.002)
        server.start()
        client = _connect_fake(server)
        session = OpenOcdSession(client=client, process=None,
                                 tcl_port=server.port,
                                 log_path=Path("<fake-rpc>"),
                                 log_handle=None,
                                 argv=["<fake-rpc>"])
        try:
            summary = run_loop(args, images, client, startup_seconds=0.0,
                               session=session, out_dir=out_dir)
        finally:
            stop_session(session, args)
            server.stop()
            server.join(timeout=2)
        write_outputs(args, summary, out_dir)
        return 0 if summary["ok"] else 1

    images = _load_images(args)
    out_dir = args.out
    session = None
    try:
        session = start_session(args, out_dir)
        summary = run_loop(args, images, session.client, session=session,
                           out_dir=out_dir)
        write_outputs(args, summary, out_dir)
        return 0 if summary["ok"] else 1
    finally:
        stop_session(session, args)


def _term_handler(signum, _frame):
    raise KeyboardInterrupt(f"signal {signum}")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _term_handler)
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt as exc:
        print(f"\n[h7-loop] interrupted ({exc})", file=sys.stderr)
        sys.exit(130)
    except SystemExit:
        raise
    except Exception:  # noqa: BLE001 - report then exit (cleanup ran)
        traceback.print_exc()
        sys.exit(1)
