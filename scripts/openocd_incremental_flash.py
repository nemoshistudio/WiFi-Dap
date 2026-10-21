#!/usr/bin/env python3
"""Incremental OpenOCD flashing helper (STM32H7 friendly).

Only erases/programs the flash sectors whose bytes differ from the previous
image (kept in a per-target cache below ``build/openocd-flash-cache``).  The
first run (or a ``--force-full`` run) does a normal full flash.  Contiguous
changed sectors are merged into one ``flash write_image erase`` call and, by
default, only the changed ranges are verified.

Intended for iterative development where the firmware changes a little between
builds; STM32H7 main-flash sectors are 128 KiB.

Quick start (USBIP device must already be attached; OpenOCD usually needs root):

    python3 scripts/openocd_incremental_flash.py --bin build/app.bin

All defaults point at the primary tree in this repository:
``build/openocd-master-install/{bin/openocd,share/openocd/scripts}``.  Use
``--candidate`` for the pipelining speed build
(``build/openocd-master-speed/install``) or pass ``--openocd``/``--scripts``
explicitly.

Offline self-tests (no device, no OpenOCD execution):

    python3 scripts/openocd_incremental_flash.py --self-test
    python3 scripts/openocd_incremental_flash.py --dry-run --bin <image>

Hardening measures (see docs/h7-flash-speedup.md §6):

  L1  defaults on this host instead of the old non-existent /tmp path;
  L2  any failed attempt invalidates the cache and triggers exactly ONE
      automatic force-full retry; the cache is only updated after success;
  L3  ``--verify/--no-verify`` is a real switch (default: verify on);
      ``--verify-scope changed|full`` selects changed-ranges-only (default)
      vs whole-image verification;
  L4  shrinking images: with ``--erase-trailing`` the comparison/write range is
      extended with 0xFF up to the previous image size, so stale trailing
      sectors are erased; without it the old size is documented as untouched;
  L6  contiguous changed sectors are merged into a single write;
  L7  only changed ranges are verified by default;
  L8  one-line/JSON timing summary for repeat-flash comparisons.

Exit codes: 0 ok (also: up to date), 1 OpenOCD/flash failure, 2 usage error.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_OPENOCD = REPO_ROOT / "build/openocd-master-install/bin/openocd"
DEFAULT_SCRIPTS = REPO_ROOT / "build/openocd-master-install/share/openocd/scripts"
CANDIDATE_OPENOCD = REPO_ROOT / "build/openocd-master-speed/install/bin/openocd"
CANDIDATE_SCRIPTS = REPO_ROOT / "build/openocd-master-speed/install/share/openocd/scripts"
DEFAULT_CACHE_DIR = REPO_ROOT / "build/openocd-flash-cache"

DEFAULT_BASE_ADDR = 0x08000000
DEFAULT_SECTOR_SIZE = 0x20000  # STM32H7 main flash sector = 128 KiB
DEFAULT_FLASH_SIZE = 0x100000  # H723/H725/H730 1 MiB variant
DEFAULT_VID_PID = "0xc251 0xf00a"
CACHE_SCHEMA = 1

CHUNK_FF = b"\xff"

# Output markers emitted by a failing OpenOCD run (see target.c verify_image).
VERIFY_FAILURE_MARKERS = (
    "checksum mismatch",
    "diff ",
    "No more differences found",
)
# Any LOG_ERROR line also aborts, but only these classify a *verify* failure.
GENERIC_FAILURE_MARKERS = ("Error:", "Timeout", "timed out")

VERIFY_TIME_RE = re.compile(r"verified\s+(\d+)\s+bytes\s+in\s+([0-9.]+)\s*s")


def log(msg: str) -> None:
    print(f"[inc-flash] {msg}", flush=True)


def die(msg: str, code: int = 2) -> "NoReturn":  # noqa: F821 (py<3.11)
    print(f"[inc-flash] ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def human_bytes(n: int) -> str:
    for unit, scale in (("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= scale:
            return f"{n / scale:.1f} {unit}"
    return f"{n} B"


# ---------------------------------------------------------------------------
# sector / changed-range logic (also imported by scripts/h7_flash_loop.py)
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Sector:
    index: int
    offset: int
    addr: int
    length: int


@dataclass(frozen=True)
class Run:
    """A contiguous byte range that has to be written (and verified)."""

    addr: int
    offset: int
    data: bytes
    sectors: tuple = ()

    @property
    def length(self) -> int:
        return len(self.data)


def image_sector_count(image_size: int, sector_size: int) -> int:
    if image_size <= 0:
        return 0
    return (image_size + sector_size - 1) // sector_size


def image_sectors(image_size: int, sector_size: int, base_addr: int) -> list:
    """Yield Sector covering [base_addr, base_addr+image_size)."""
    if sector_size <= 0:
        raise ValueError("sector size must be positive")
    out = []
    off = 0
    idx = 0
    while off < image_size:
        size = min(sector_size, image_size - off)
        out.append(Sector(index=idx, offset=off, addr=base_addr + off,
                          length=size))
        idx += 1
        off += size
    return out


def effective_image(img: bytes, old: bytes | None,
                    erase_trailing: bool) -> bytes:
    """Comparison/write image, optionally padded with 0xFF up to the old size.

    Only used for images that *shrank* and ``--erase-trailing``; padding to the
    old length makes the trailing sectors count as changed (old != 0xFF) and
    therefore erased/rewritten with 0xFF.
    """
    if old is not None and erase_trailing and len(old) > len(img):
        return img + CHUNK_FF * (len(old) - len(img))
    return img


def _old_slice(old: bytes | None, off: int, length: int) -> bytes:
    if old is None:
        return CHUNK_FF * length
    part = old[off:off + length]
    if len(part) < length:
        part += CHUNK_FF * (length - len(part))
    return part


def changed_sectors(img: bytes, old: bytes | None, sector_size: int,
                    base_addr: int, erase_trailing: bool = False) -> list:
    """Return [(Sector, changed: bool)] for the effective image."""
    eff = effective_image(img, old, erase_trailing)
    out = []
    for sec in image_sectors(len(eff), sector_size, base_addr):
        new_part = eff[sec.offset:sec.offset + sec.length]
        old_part = _old_slice(old, sec.offset, sec.length)
        out.append((sec, new_part != old_part))
    return out


def merge_contiguous(changed: list, eff: bytes) -> list:
    """Merge consecutive changed sectors into contiguous Run objects."""
    runs = []
    current = None  # [first_sector, last_sector]
    for sec in changed:
        if current is not None and sec.index == current[1].index + 1:
            current[1] = sec
            continue
        if current is not None:
            runs.append(current)
        current = [sec, sec]
    if current is not None:
        runs.append(current)
    out = []
    for first, last in runs:
        start_off = first.offset
        end_off = last.offset + last.length
        out.append(Run(addr=first.addr, offset=start_off,
                       data=eff[start_off:end_off],
                       sectors=tuple(range(first.index, last.index + 1))))
    return out


def full_run(img: bytes, old: bytes | None, sector_size: int, base_addr: int,
             erase_trailing: bool = False) -> Run:
    eff = effective_image(img, old, erase_trailing)
    return Run(addr=base_addr, offset=0, data=eff,
               sectors=tuple(range(image_sector_count(len(eff), sector_size))))


def changed_runs(img: bytes, old: bytes | None, sector_size: int,
                 base_addr: int, erase_trailing: bool = False) -> list:
    """Contiguous Runs covering only the changed sectors of the image."""
    marked = changed_sectors(img, old, sector_size, base_addr, erase_trailing)
    eff = effective_image(img, old, erase_trailing)
    return merge_contiguous([sec for sec, changed in marked if changed], eff)


# ---------------------------------------------------------------------------
# cache (image of the last successfully flashed content + metadata sidecar)
# ---------------------------------------------------------------------------

@dataclass
class FlashCache:
    cache_dir: Path
    base_addr: int
    sector_size: int
    image: bytes | None = None
    reason: str = "missing"

    def _stem(self) -> str:
        return f"base{self.base_addr:08x}-sector{self.sector_size:x}"

    @property
    def data_path(self) -> Path:
        return self.cache_dir / f"{self._stem()}.bin"

    @property
    def meta_path(self) -> Path:
        return self.cache_dir / f"{self._stem()}.json"

    def load(self, image_size: int) -> bytes | None:
        """Load the last flashed image; None (with reason) if unusable.

        A cache is only trusted when both files exist, the sidecar metadata
        matches the payload (sha256/size/layout) and the payload is at least as
        large as the new image - a shorter/truncated file must not be padded
        with 0xFF silently (that is exactly the L2 stale-sector trap).
        """
        self.image = None
        if not self.data_path.is_file():
            self.reason = "missing"
            return None
        if not self.meta_path.is_file():
            self.reason = "missing metadata sidecar (legacy cache ignored)"
            return None
        try:
            meta = json.loads(self.meta_path.read_text(encoding="utf-8"))
            data = self.data_path.read_bytes()
        except (OSError, json.JSONDecodeError) as exc:
            self.reason = f"unreadable ({exc})"
            return None
        if meta.get("schema") != CACHE_SCHEMA:
            self.reason = f"schema {meta.get('schema')} != {CACHE_SCHEMA}"
            return None
        if int(meta.get("base_addr", -1)) != self.base_addr or \
                int(meta.get("sector_size", -1)) != self.sector_size:
            self.reason = "layout mismatch (base/sector size)"
            return None
        if int(meta.get("image_size", -1)) != len(data) or \
                meta.get("sha256") != sha256_bytes(data):
            self.reason = "metadata/payload mismatch (corrupt or truncated)"
            return None
        if len(data) < image_size:
            self.reason = ("cached image shorter than the new image; "
                           "refusing to pad it with 0xFF")
            return None
        self.image = data
        self.reason = "ok"
        return data

    def save(self, image: bytes, origin: str) -> None:
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        tmp = self.data_path.with_suffix(".bin.tmp")
        tmp.write_bytes(image)
        os.replace(tmp, self.data_path)
        meta = {
            "schema": CACHE_SCHEMA,
            "base_addr": self.base_addr,
            "sector_size": self.sector_size,
            "image_size": len(image),
            "sha256": sha256_bytes(image),
            "written_at": utc_now(),
            "origin": origin,
        }
        meta_tmp = self.meta_path.with_suffix(".json.tmp")
        meta_tmp.write_text(json.dumps(meta, indent=2) + "\n",
                            encoding="utf-8")
        os.replace(meta_tmp, self.meta_path)

    def invalidate(self) -> None:
        for path in (self.data_path, self.meta_path):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        self.image = None
        self.reason = "invalidated"


# ---------------------------------------------------------------------------
# plan
# ---------------------------------------------------------------------------

@dataclass
class FlashPlan:
    mode: str                       # "full" | "incremental" | "up-to-date"
    image: bytes
    image_sha256: str
    base_addr: int
    sector_size: int
    runs: list = field(default_factory=list)
    verify_targets: list = field(default_factory=list)
    verify_enabled: bool = True
    verify_scope: str = "changed"    # "changed" | "full"
    changed_sector_indexes: list = field(default_factory=list)
    total_sectors: int = 0
    old_present: bool = False
    effective_size: int = 0
    reason: str = ""

    @property
    def bytes_to_write(self) -> int:
        return sum(len(r.data) for r in self.runs)


def build_plan(img: bytes, cache: FlashCache, sector_size: int,
               base_addr: int, verify: bool, verify_scope: str,
               force_full: bool, erase_trailing: bool) -> FlashPlan:
    old = None if force_full else cache.image
    total = image_sector_count(len(img), sector_size)
    marked = changed_sectors(img, None if old is None else old, sector_size,
                             base_addr, erase_trailing)
    changed_idx = [sec.index for sec, changed in marked if changed]
    eff_size = len(effective_image(img, old, erase_trailing))
    image_sha = sha256_bytes(img)

    if old is None:
        mode, reason = "full", ("--force-full" if force_full
                                else "no valid cache")
    elif not changed_idx:
        mode, reason = "up-to-date", "cache matches the image"
    elif len(changed_idx) == len(marked):
        mode, reason = "full", "all sectors changed"
    else:
        mode, reason = "incremental", (
            f"{len(changed_idx)}/{len(marked)} sector(s) changed")

    plan = FlashPlan(mode=mode, image=img, image_sha256=image_sha,
                     base_addr=base_addr, sector_size=sector_size,
                     verify_enabled=verify, verify_scope=verify_scope,
                     changed_sector_indexes=changed_idx,
                     total_sectors=total, old_present=old is not None,
                     effective_size=eff_size, reason=reason)

    if mode == "full":
        run = full_run(img, old, sector_size, base_addr, erase_trailing)
        plan.runs = [run]
        plan.verify_targets = [run] if verify else []
    elif mode == "incremental":
        plan.runs = changed_runs(img, old, sector_size, base_addr,
                                 erase_trailing)
        if verify:
            if verify_scope == "full":
                plan.verify_targets = [full_run(img, old, sector_size,
                                                base_addr, erase_trailing)]
            else:
                plan.verify_targets = list(plan.runs)
    else:  # up-to-date: nothing to write/verify
        plan.runs = []
        plan.verify_targets = []
    return plan


# ---------------------------------------------------------------------------
# OpenOCD invocation
# ---------------------------------------------------------------------------

def tcl_quote(text: str) -> str:
    """Quote a path/string for use as one TCL word (braces handle spaces)."""
    if "}" in text or "{" in text:
        raise ValueError(f"path not TCL-safe: {text!r}")
    return "{" + text + "}"


@dataclass
class RunResult:
    rc: int
    seconds: float
    output: str = ""
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.rc == 0 and not self.timed_out

    def verify_failure(self) -> bool:
        return any(m in self.output for m in VERIFY_FAILURE_MARKERS)

    def generic_failure(self) -> bool:
        return any(m in self.output for m in GENERIC_FAILURE_MARKERS)


def build_openocd_argv(args, tcl_path: Path) -> list:
    argv = [
        str(args.openocd),
        "-f", args.interface_cfg,
        "-c", f"cmsis_dap_vid_pid {args.vid_pid}",
        "-c", "transport select swd",
        "-f", args.target_cfg,
        # adapter speed after the target cfg: the target script may set its own.
        "-c", f"adapter speed {args.adapter_speed}",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
    ]
    for command in args.openocd_config:
        argv += ["-c", command]
    argv += ["-c", "init", "-c", "reset halt", "-f", str(tcl_path)]
    return argv


def default_runner(argv: list, env: dict, timeout_s: int) -> RunResult:
    started = time.monotonic()
    try:
        proc = subprocess.run(argv, env=env, capture_output=True, text=True,
                              timeout=timeout_s)
        return RunResult(rc=proc.returncode,
                         seconds=time.monotonic() - started,
                         output=(proc.stdout or "") + (proc.stderr or ""))
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        output += f"\n[inc-flash] TIMEOUT after {timeout_s}s\n"
        return RunResult(rc=124, seconds=time.monotonic() - started,
                         output=output, timed_out=True)


@dataclass
class Attempt:
    label: str
    result: RunResult
    plan: FlashPlan
    verify_internal_seconds: float | None = None


def _parse_verify_seconds(output: str) -> float | None:
    found = VERIFY_TIME_RE.findall(output)
    if not found:
        return None
    try:
        return sum(float(secs) for _bytes, secs in found)
    except ValueError:
        return None


def run_attempt(plan: FlashPlan, args, label: str,
                runner=default_runner) -> Attempt:
    """Materialise the plan in a temp dir and run one OpenOCD process."""
    env = os.environ.copy()
    env["OPENOCD_SCRIPTS"] = str(args.scripts)
    with tempfile.TemporaryDirectory(prefix="inc-flash-") as td:
        tdir = Path(td)
        cmds = []
        for i, run in enumerate(plan.runs):
            path = tdir / f"write-{i}.bin"
            path.write_bytes(run.data)
            cmds.append(f"flash write_image erase {tcl_quote(str(path))} "
                        f"0x{run.addr:08x}")
        for i, run in enumerate(plan.verify_targets):
            path = tdir / f"verify-{i}.bin"
            path.write_bytes(run.data)
            cmds.append(f"verify_image {tcl_quote(str(path))} "
                        f"0x{run.addr:08x}")
        cmds += ["reset run", "shutdown"]
        tcl = tdir / "flash.tcl"
        tcl.write_text("\n".join(cmds) + "\n", encoding="utf-8")
        argv = build_openocd_argv(args, tcl)
        if args.verbose:
            log(f"[{label}] {' '.join(argv)}")
            log(f"[{label}] tcl:\n" + "\n".join("  " + c for c in cmds))
        result = runner(argv, env, args.timeout)
    return Attempt(label=label, result=result, plan=plan,
                   verify_internal_seconds=_parse_verify_seconds(
                       result.output))


# ---------------------------------------------------------------------------
# orchestration (L2: invalidate + one force-full retry)
# ---------------------------------------------------------------------------

def _save_cache(cache: FlashCache, plan: FlashPlan, args) -> None:
    if args.no_cache_update:
        log("cache update disabled (--no-cache-update)")
        return
    if not plan.verify_enabled:
        log("WARNING: cache updated without verification (--no-verify); "
            "run a verified flash if this image must be trusted")
    cache.save(plan.image, origin=f"{plan.mode} flash via {args.openocd}")


def execute_plan(plan: FlashPlan, cache: FlashCache, args,
                 runner=default_runner) -> int:
    started = time.monotonic()
    summary = {
        "schema": 1,
        "tool": "openocd_incremental_flash",
        "timestamp": utc_now(),
        "image": {
            "path": str(args.bin) if args.bin else "<synthetic>",
            "size": len(plan.image),
            "sha256": plan.image_sha256,
            "base_addr": f"0x{plan.base_addr:08x}",
            "sector_size": plan.sector_size,
        },
        "cache": {"dir": str(cache.cache_dir), "state": cache.reason},
        "plan": {
            "mode": plan.mode,
            "reason": plan.reason,
            "changed_sectors": plan.changed_sector_indexes,
            "total_sectors": plan.total_sectors,
            "bytes_to_write": plan.bytes_to_write,
            "verify": plan.verify_enabled,
            "verify_scope": plan.verify_scope,
            "verify_targets": [
                {"addr": f"0x{r.addr:08x}", "size": r.length}
                for r in plan.verify_targets
            ],
        },
        "attempts": [],
        "result": "pending",
    }

    def record(attempt: Attempt) -> None:
        summary["attempts"].append({
            "label": attempt.label,
            "rc": attempt.result.rc,
            "timed_out": attempt.result.timed_out,
            "seconds": round(attempt.result.seconds, 3),
            "verify_internal_seconds": attempt.verify_internal_seconds,
            "verify_failure": attempt.result.verify_failure(),
        })

    def finish(code: int, result: str) -> int:
        summary["result"] = result
        summary["wall_seconds"] = round(time.monotonic() - started, 3)
        quiet = getattr(args, "quiet", False)
        if not quiet:
            print_plan(plan, cache, args)
            print_attempts(summary["attempts"])
            log(f"result: {result.upper()} "
                f"wall={summary['wall_seconds']:.3f} s")
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(summary, indent=2) + "\n",
                                     encoding="utf-8")
            log(f"timing JSON: {args.json_out}")
        elif not quiet:
            log("summary: " + json.dumps({
                "mode": plan.mode,
                "changed_sectors": len(plan.changed_sector_indexes),
                "bytes_to_write": plan.bytes_to_write,
                "attempts": len(summary["attempts"]),
                "wall_seconds": summary["wall_seconds"],
                "result": result,
            }, separators=(",", ":")))
        return code

    # Up-to-date path: nothing to program.  Optionally verify the target
    # against the whole image first (catches a stale external writer).
    if plan.mode == "up-to-date":
        if args.verify_on_up_to_date:
            check = FlashPlan(mode="verify-only", image=plan.image,
                              image_sha256=plan.image_sha256,
                              base_addr=plan.base_addr,
                              sector_size=plan.sector_size,
                              verify_enabled=True, verify_scope="full",
                              total_sectors=plan.total_sectors,
                              old_present=True,
                              effective_size=plan.effective_size,
                              verify_targets=[full_run(
                                  plan.image, cache.image, plan.sector_size,
                                  plan.base_addr, args.erase_trailing)])
            attempt = run_attempt(check, args, "verify-only", runner)
            record(attempt)
            if attempt.result.ok:
                return finish(0, "ok")
            log("up-to-date verification FAILED; invalidating cache and "
                "flashing the full image")
            cache.invalidate()
            full = build_plan(plan.image, cache, plan.sector_size,
                              plan.base_addr, True, "full", True,
                              args.erase_trailing)
            retry = run_attempt(full, args, "force-full (recovery)", runner)
            record(retry)
            if retry.result.ok:
                _save_cache(cache, full, args)
                return finish(0, "recovered")
            cache.invalidate()
            return finish(1, "fail")
        log("up to date, nothing to flash")
        return finish(0, "ok")

    first = run_attempt(plan, args, plan.mode, runner)
    record(first)
    if first.result.ok:
        _save_cache(cache, plan, args)
        return finish(0, "ok")

    if first.result.verify_failure():
        log("verify failed on the changed range")
    elif first.result.timed_out:
        log("OpenOCD timed out")
    else:
        log("OpenOCD reported an error")
    # L2: never leave a cache that says "nothing changed" after a failure.
    cache.invalidate()
    log("cache invalidated; retrying ONCE with a full-image write+verify")
    full = build_plan(plan.image, cache, plan.sector_size, plan.base_addr,
                      True, "full", True, args.erase_trailing)
    retry = run_attempt(full, args, "force-full (recovery)", runner)
    record(retry)
    if retry.result.ok:
        _save_cache(cache, full, args)
        return finish(0, "recovered")
    cache.invalidate()
    log("full retry failed as well; cache stays invalid, target may be in an "
        "unknown state")
    return finish(1, "fail")


def print_plan(plan: FlashPlan, cache: FlashCache, args) -> None:
    log(f"image: {args.bin if args.bin else '<synthetic>'} "
        f"size={human_bytes(len(plan.image))} "
        f"sha256={plan.image_sha256[:12]} base=0x{plan.base_addr:08x}")
    log(f"cache: {cache.cache_dir} ({cache.reason})")
    log(f"plan: {plan.mode} - {plan.reason}; "
        f"changed sectors {plan.changed_sector_indexes}/"
        f"{plan.total_sectors}; write {human_bytes(plan.bytes_to_write)}; "
        f"verify={'on' if plan.verify_enabled else 'off'}"
        f"({plan.verify_scope})")
    for i, run in enumerate(plan.runs):
        log(f"  write[{i}] addr=0x{run.addr:08x} size={run.length} "
            f"sectors={list(run.sectors)}")
    for i, run in enumerate(plan.verify_targets):
        log(f"  verify[{i}] addr=0x{run.addr:08x} size={run.length}")


def print_attempts(attempts: list) -> None:
    if not attempts:
        return
    print(f"{'attempt':<28}{'rc':>4}{'wall_s':>10}  note")
    for att in attempts:
        note = []
        if att["timed_out"]:
            note.append("TIMEOUT")
        if att["verify_failure"]:
            note.append("verify mismatch")
        if att["verify_internal_seconds"] is not None:
            note.append(f"verify {att['verify_internal_seconds']:.3f} s on target")
        print(f"{att['label']:<28}{att['rc']:>4}{att['seconds']:>10.3f}  "
              f"{', '.join(note)}")


# ---------------------------------------------------------------------------
# offline self-test / dry-run
# ---------------------------------------------------------------------------

def _self_test_run(runner, plan, args):
    return run_attempt(plan, args, "selftest", runner)


class _FakeRunner:
    """Deterministic OpenOCD stub: records TCL scripts, returns canned runs."""

    def __init__(self, results: list):
        self.results = list(results)
        self.tcls = []
        self.calls = 0

    def __call__(self, argv, env, timeout_s):
        self.calls += 1
        self.tcls.append(Path(argv[-1]).read_text(encoding="utf-8"))
        rc, output = self.results.pop(0)
        return RunResult(rc=rc, seconds=0.01, output=output)


class _SelfTestArgs:
    """Minimal args namespace for the offline tests."""

    def __init__(self, tmp: Path):
        self.openocd = tmp / "fake-openocd"
        self.scripts = tmp
        self.interface_cfg = "interface/cmsis-dap.cfg"
        self.target_cfg = "target/stm32h7x.cfg"
        self.adapter_speed = 1900
        self.vid_pid = DEFAULT_VID_PID
        self.openocd_config = []
        self.timeout = 30
        self.verbose = False
        self.no_cache_update = False
        self.verify_on_up_to_date = False
        self.erase_trailing = False
        self.bin = None
        self.json_out = None
        self.quiet = True


def self_test() -> int:
    """Offline checks of the sector/merge/cache/verify/retry logic."""
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

    print("self-test: sector / merge / cache / verify / retry logic")
    base = 0x08000000
    sector = 0x20000

    def image(nbytes: int, seed: int = 0) -> bytes:
        return bytes((seed + i * 7 + (i >> 8)) % 256 for i in range(nbytes))

    with tempfile.TemporaryDirectory(prefix="inc-flash-selftest-") as td:
        tdir = Path(td)
        args = _SelfTestArgs(tdir)

        # 1) sector math on a non-sector-multiple image
        secs = image_sectors(3 * sector - 1000, sector, base)
        check(len(secs) == 3 and secs[-1].length == sector - 1000
              and secs[-1].addr == base + 2 * sector,
              "sector layout for partial last sector")
        check(len(image_sectors(0, sector, base)) == 0,
              "empty image has no sectors")

        # 2) change detection + contiguous merge
        old = image(3 * sector)
        new = bytearray(old)
        new[sector + 10] ^= 0xFF                     # only sector 1 changed
        runs = changed_runs(bytes(new), old, sector, base)
        check(len(runs) == 1 and runs[0].addr == base + sector
              and runs[0].length == sector
              and runs[0].data == bytes(new[sector:2 * sector]),
              "single changed sector -> one run")
        check(runs[0].sectors == (1,), "single run sector indexes")

        new2 = bytearray(old)                        # sector 0 and 1 changed
        new2[5] ^= 0x0F
        new2[2 * sector - 1] ^= 0x0F
        runs2 = changed_runs(bytes(new2), old, sector, base)
        check(len(runs2) == 1 and runs2[0].length == 2 * sector
              and runs2[0].addr == base and runs2[0].sectors == (0, 1),
              "contiguous changed sectors merged into one run")

        new3 = bytearray(old)                        # sectors 0 and 2 changed
        new3[1] ^= 0x0F
        new3[2 * sector + 1] ^= 0x0F
        runs3 = changed_runs(bytes(new3), old, sector, base)
        check(len(runs3) == 2 and runs3[0].sectors == (0,)
              and runs3[1].sectors == (2,),
              "non-contiguous changed sectors stay separate")

        # 3) no cache -> full plan, one run, verify target = run
        cache = FlashCache(tdir / "cache", base, sector)
        plan = build_plan(bytes(new), cache, sector, base, True, "changed",
                          False, False)
        check(plan.mode == "full" and len(plan.runs) == 1
              and plan.runs[0].length == 3 * sector,
              "no cache -> full plan")
        check(len(plan.verify_targets) == 1 and plan.verify_enabled,
              "full plan verifies the whole image")

        # 4) --no-verify produces no verify commands
        plan_nv = build_plan(bytes(new), cache, sector, base, False, "changed",
                             False, False)
        check(plan_nv.verify_targets == [], "--no-verify removes verify")

        # 5) cache round-trip, corruption handling
        cache.save(bytes(old), origin="self-test")
        check(cache.load(len(old)) == old, "cache save/load round-trip")
        check(cache.data_path.is_file() and cache.meta_path.is_file(),
              "cache payload + metadata sidecar written")
        meta_backup = cache.meta_path.read_text(encoding="utf-8")
        cache.meta_path.write_text(meta_backup.replace(
            '"sha256": "', '"sha256": "dead'), encoding="utf-8")
        cache.image = None
        check(cache.load(len(old)) is None, "corrupt metadata -> cache ignored")
        cache.meta_path.write_text(meta_backup, encoding="utf-8")
        payload = cache.data_path.read_bytes()
        cache.data_path.write_bytes(payload[:-100])
        check(cache.load(len(old)) is None,
              "truncated payload -> cache ignored (L2)")
        cache.data_path.write_bytes(payload)
        check(cache.load(len(old) + 1) is None,
              "cache shorter than new image -> ignored")
        cache.invalidate()
        check(not cache.data_path.exists() and not cache.meta_path.exists(),
              "cache invalidate removes both files")

        # 6) incremental plan: changed ranges only, verify scope
        cache.save(bytes(old), origin="self-test")
        cache.load(len(old))
        inc = build_plan(bytes(new), cache, sector, base, True, "changed",
                         False, False)
        check(inc.mode == "incremental" and len(inc.runs) == 1
              and inc.runs[0].addr == base + sector,
              "incremental plan selects the changed sector only")
        check(len(inc.verify_targets) == 1
              and inc.verify_targets[0].addr == base + sector,
              "verify scope changed by default (L7)")
        full_verify = build_plan(bytes(new), cache, sector, base, True, "full",
                                 False, False)
        check(full_verify.verify_targets[0].length == 3 * sector,
              "verify scope full verifies the whole image")

        # 7) up-to-date plan
        cache2 = FlashCache(tdir / "cache2", base, sector)
        cache2.save(bytes(new), origin="self-test")
        cache2.load(len(new))
        uptodate = build_plan(bytes(new), cache2, sector, base, True,
                              "changed", False, False)
        check(uptodate.mode == "up-to-date" and uptodate.runs == [],
              "identical image -> up-to-date plan")

        # 8) shrinking image with --erase-trailing covers the old tail
        small = bytes(new[:sector])
        cache3 = FlashCache(tdir / "cache3", base, sector)
        cache3.save(bytes(new), origin="self-test")
        cache3.load(len(small))
        shrink = build_plan(small, cache3, sector, base, True, "changed",
                            False, True)
        check(shrink.mode == "incremental"
              and sum(r.length for r in shrink.runs) == 2 * sector
              and shrink.runs[0].data[-sector:] == CHUNK_FF * sector,
              "--erase-trailing pads the shrunk image with 0xFF")

        # 9) L2 retry: first attempt fails verification, force-full succeeds
        fake = _FakeRunner([
            (1, "Error: checksum mismatch - attempting binary compare\n"
                "Error: verify_image failed\n"),
            (0, "verified 65536 bytes in 0.100000s (640.0 KiB/s)\n"),
        ])
        cache4 = FlashCache(tdir / "cache4", base, sector)
        cache4.save(bytes(old), origin="self-test")
        cache4.load(len(old))
        plan4 = build_plan(bytes(new), cache4, sector, base, True, "changed",
                           False, False)
        rc = execute_plan(plan4, cache4, args, runner=fake)
        check(rc == 0, "retry: recovered exit code 0")
        check(fake.calls == 2, "retry: exactly one extra attempt")
        check("verify_image" in fake.tcls[0]
              and "write-0.bin" in fake.tcls[0],
              "retry: first attempt programmed/verified changed range")
        check(fake.tcls[1].count("flash write_image erase") == 1
              and "verify_image" in fake.tcls[1],
              "retry: second attempt is a full write+verify")
        check(cache4.data_path.is_file()
              and cache4.load(len(new)) == bytes(new),
              "retry: cache updated after the successful recovery")

        # 10) both attempts fail -> exit 1 and cache stays invalid
        fake2 = _FakeRunner([
            (1, "Error: checksum mismatch\n"),
            (1, "Error: unexpected failure\n"),
        ])
        cache5 = FlashCache(tdir / "cache5", base, sector)
        cache5.save(bytes(old), origin="self-test")
        cache5.load(len(old))
        plan5 = build_plan(bytes(new), cache5, sector, base, True, "changed",
                           False, False)
        rc = execute_plan(plan5, cache5, args, runner=fake2)
        check(rc == 1, "retry: double failure exit code 1")
        check(not cache5.data_path.exists() and not cache5.meta_path.exists(),
              "retry: cache invalid after double failure (L2)")

        # 11) verify-failure markers
        check(RunResult(0, 0.0, "verified 4 bytes in 0.1s").ok
              and not RunResult(0, 0.0, "verified 4 bytes in 0.1s")
              .verify_failure(), "success output is not a verify failure")
        check(RunResult(1, 0.0, "diff 0 address 0x08000010. Was 0x00 "
                                "instead of 0xff").verify_failure(),
              "binary-compare diff classified as verify failure")
        check(RunResult(124, 0.0, "", timed_out=True).ok is False,
              "timeout is never ok")

        # 12) dry-run plan output smoke
        dry = build_plan(bytes(new3), cache, sector, base, True, "changed",
                         False, False)
        check(len(dry.runs) == 2 and len(dry.verify_targets) == 2,
              "dry-run-style plan has 2 write and 2 verify runs")

    if failures:
        print(f"self-test: FAIL ({len(failures)} of {checks} checks failed: "
              f"{', '.join(failures)})")
        return 1
    print(f"self-test OK ({checks} checks)")
    return 0


def _synthetic_dry_run_json() -> dict:
    """Show full / incremental / up-to-date plans with synthetic images."""
    base = 0x08000000
    sector = 0x20000
    with tempfile.TemporaryDirectory(prefix="inc-flash-synth-") as td:
        tdir = Path(td)
        old_img = bytes((i * 13 + (i >> 7)) % 256
                        for i in range(2 * sector + 12345))
        changed = bytearray(old_img)
        changed[sector + 123] ^= 0xFF
        new_img = bytes(changed)
        cases = (
            ("no-cache -> full flash", None),
            ("1 changed sector -> incremental", old_img),
            ("identical image -> up-to-date", new_img),
        )
        cache = FlashCache(tdir / "cache", base, sector)
        out = {}
        for label, old in cases:
            cache.invalidate()
            if old is not None:
                cache.save(old, origin="synthetic")
                cache.load(len(new_img))
            plan = build_plan(new_img, cache, sector, base, True, "changed",
                              False, False)
            out[label] = {
                "mode": plan.mode,
                "changed_sectors": plan.changed_sector_indexes,
                "bytes_to_write": plan.bytes_to_write,
                "runs": [{"addr": f"0x{r.addr:08x}", "size": r.length}
                         for r in plan.runs],
                "verify_targets": [
                    {"addr": f"0x{r.addr:08x}", "size": r.length}
                    for r in plan.verify_targets],
            }
        return out


def dry_run(args) -> int:
    log("DRY RUN: no OpenOCD executed, no hardware touched")
    if args.bin is None:
        log("no --bin given -> synthetic plan demo")
        demo = _synthetic_dry_run_json()
        print(json.dumps(demo, indent=2))
        return 0 if self_test() == 0 else 1

    img = args.bin.read_bytes()
    cache = FlashCache(args.cache_dir, args.base_addr, args.sector_size)
    old = None if args.force_full else cache.load(len(img))
    plan = build_plan(img, cache, args.sector_size, args.base_addr,
                      args.verify, args.verify_scope, args.force_full,
                      args.erase_trailing)
    print_plan(plan, cache, args)
    with tempfile.TemporaryDirectory(prefix="inc-flash-dry-") as td:
        tcl = Path(td) / "flash.tcl"
        tcl.write_text("# reason: " + plan.reason + "\n", encoding="utf-8")
        print(f"openocd argv: {' '.join(build_openocd_argv(args, tcl))}")
    log(f"openocd exists={args.openocd.is_file()} "
        f"scripts exists={args.scripts.is_dir()}")
    print()
    return 0 if self_test() == 0 else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv: list):
    ap = argparse.ArgumentParser(
        description="Incremental OpenOCD flashing helper (changed H7 sectors "
                    "only, cache invalidation + force-full retry, changed-range "
                    "verify). Defaults point at build/openocd-master-install.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Quick start", 1)[-1])
    ap.add_argument("--bin", type=Path, default=None,
                    help="new firmware .bin (required unless --dry-run/"
                         "--self-test)")
    ap.add_argument("--base-addr", type=lambda v: int(v, 0),
                    default=DEFAULT_BASE_ADDR,
                    help="flash base address (default 0x08000000)")
    ap.add_argument("--sector-size", type=lambda s: int(s, 0),
                    default=DEFAULT_SECTOR_SIZE,
                    help="flash sector size in bytes (H7=0x20000, default)")
    ap.add_argument("--flash-size", type=lambda s: int(s, 0),
                    default=DEFAULT_FLASH_SIZE,
                    help="target flash size in bytes (default 0x100000)")
    ap.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR,
                    help=f"cache directory (default: {DEFAULT_CACHE_DIR})")
    ap.add_argument("--openocd", type=Path, default=DEFAULT_OPENOCD,
                    help=f"OpenOCD binary (default: {DEFAULT_OPENOCD})")
    ap.add_argument("--scripts", type=Path, default=DEFAULT_SCRIPTS,
                    help=f"OPENOCD_SCRIPTS directory (default: {DEFAULT_SCRIPTS})")
    ap.add_argument("--candidate", action="store_true",
                    help="use the pipelining speed build "
                         f"({CANDIDATE_OPENOCD})")
    ap.add_argument("--interface-cfg", default="interface/cmsis-dap.cfg")
    ap.add_argument("--target-cfg", default="target/stm32h7x.cfg")
    ap.add_argument("--adapter-speed", default="1900",
                    help="SWD clock in kHz (default: 1900)")
    ap.add_argument("--vid-pid", default=DEFAULT_VID_PID,
                    help="CMSIS-DAP USB VID/PID (default: 0xc251 0xf00a)")
    ap.add_argument("--openocd-config", action="append", default=[],
                    metavar="TCL",
                    help="extra -c config command before init (repeatable), "
                         'e.g. "cmsis-dap pending_requests 4"')
    ap.add_argument("--timeout", type=int, default=180,
                    help="per-OpenOCD-invocation timeout in seconds "
                         "(default: 180)")
    verify = ap.add_mutually_exclusive_group()
    verify.add_argument("--verify", dest="verify", action="store_true",
                        default=True,
                        help="verify after writing (default; L3)")
    verify.add_argument("--no-verify", dest="verify", action="store_false",
                        help="skip verification (cache is then updated "
                             "unverified - not recommended)")
    ap.add_argument("--verify-scope", choices=("changed", "full"),
                    default="changed",
                    help="verify only changed ranges (default, L7) or the "
                         "whole image")
    ap.add_argument("--verify-on-up-to-date", action="store_true",
                    help="when the cache matches the image, still verify the "
                         "target (costs one OpenOCD startup); on mismatch the "
                         "cache is invalidated and a full flash is retried")
    ap.add_argument("--force-full", action="store_true",
                    help="ignore the cache and flash the whole image")
    ap.add_argument("--erase-trailing", action="store_true",
                    help="when the image shrank, also erase the old trailing "
                         "sectors (pad with 0xFF up to the old size, L4)")
    ap.add_argument("--no-cache-update", action="store_true",
                    help="do not update the incremental cache after success")
    ap.add_argument("--json-out", type=Path, default=None,
                    help="write a machine-readable timing JSON here")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan + run offline self-tests; never "
                         "executes OpenOCD")
    ap.add_argument("--self-test", action="store_true",
                    help="run the offline logic self-test and exit")
    ap.add_argument("--verbose", action="store_true",
                    help="print the full OpenOCD argv/TCL")
    args = ap.parse_args(argv)

    if args.candidate:
        args.openocd = CANDIDATE_OPENOCD
        args.scripts = CANDIDATE_SCRIPTS
    if args.self_test and args.bin is None and not args.dry_run:
        return args
    if args.bin is None:
        return args
    if args.base_addr % args.sector_size:
        die("--base-addr must be sector aligned")
    if args.sector_size <= 0:
        die("--sector-size must be positive")
    try:
        args.adapter_speed = int(str(args.adapter_speed), 0)
    except ValueError:
        die(f"invalid --adapter-speed: {args.adapter_speed}")
    return args


def main(argv: list) -> int:
    args = parse_args(argv)
    if args.self_test and not args.dry_run:
        return self_test()
    if args.dry_run:
        if args.bin is not None:
            if not args.bin.is_file():
                die(f"image not found: {args.bin}")
            try:
                img = args.bin.read_bytes()
            except OSError as exc:
                die(f"cannot read image: {exc}")
            if not img:
                die(f"empty image: {args.bin}")
            size = len(img)
        else:
            size = 0
        if size and size > args.flash_size:
            die(f"image does not fit the configured flash size "
                f"(0x{args.flash_size:x})")
        return dry_run(args)

    if args.bin is None:
        die("--bin is required unless --dry-run/--self-test")
    if not args.bin.is_file():
        die(f"image not found: {args.bin}")
    if not args.openocd.is_file():
        die(f"OpenOCD binary not found: {args.openocd}")
    if not args.scripts.is_dir():
        die(f"OpenOCD scripts directory not found: {args.scripts}")
    if not os.access(args.openocd, os.X_OK):
        die(f"OpenOCD binary is not executable: {args.openocd}")

    img = args.bin.read_bytes()
    if not img:
        die(f"empty image: {args.bin}")
    if len(img) > args.flash_size:
        die(f"image {human_bytes(len(img))} does not fit flash "
            f"{human_bytes(args.flash_size)}")
    cache = FlashCache(args.cache_dir, args.base_addr, args.sector_size)
    old = cache.load(len(img))
    plan = build_plan(img, cache, args.sector_size, args.base_addr,
                      args.verify, args.verify_scope, args.force_full,
                      args.erase_trailing)
    log(f"image: {args.bin} size={len(img)} "
        f"sha256={sha256_bytes(img)[:16]}...")
    return execute_plan(plan, cache, args, runner=default_runner)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        print("\n[inc-flash] interrupted", file=sys.stderr)
        sys.exit(130)
