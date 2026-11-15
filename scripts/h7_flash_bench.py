#!/usr/bin/env python3
"""Repeatable STM32H7 flash/debug benchmark harness (WiFi USBIP CMSIS-DAP).

Measures the phases that matter for the H7 flashing workflow documented in
docs/h7-flash-profile.md and docs/h7-flash-speedup.md:

  startup  : OpenOCD probe/init + reset halt only (no flash operation)
  erase    : `flash erase_sector 0 <first> <last>` covering the image
  program  : `flash write_image <bin>` on an already erased flash (no erase)
  verify   : `verify_image <bin>` (read-back comparison)
  full     : `flash write_image erase <bin>` + `verify_image <bin>` (wall time)
  read     : `dump_image` of <read-size> bytes (default 1 MiB) from flash base
  step     : N single-instruction `step` operations (DAP small transactions)
  smallread: N single-word `mdw` reads from RAM (DAP small transactions)
  smallwrite: N single-word `mww` writes to RAM (DAP small transactions)

The step/small* phases isolate the per-transaction cost of the DAP transport
(one CMSIS-DAP command per operation) from the bulk flash phases, which is what
the ESP32-side DAP copy optimisation (docs/dap-copy-budget.md) targets.

Each phase runs as a *fresh* OpenOCD process so phase times include OpenOCD
startup (measured separately by the `startup` phase; subtract it if you need
the pure on-target time).  The target must already be reachable: the harness
never touches usbip, it assumes the USBIP device is attached and appears as a
CMSIS-DAP v2 bulk device (e.g. `sudo usbip attach -r <device-ip> -b 1-1`).

SAFETY
  * No hardware access at all unless --yes is given.
  * Without --yes the harness refuses to run when a destructive phase
    (erase/program/full) is selected.  Read-only phases do not need --yes.
  * --dry-run never executes OpenOCD; it prints the plan and self-tests the
    harness offline (works with no device, no OpenOCD binary, no network).
  * The default image is the reference firmware reference-board reference firmware.bin; pass
    --expect-sha256 to make the harness abort on a wrong/renamed image.

Examples
--------
Offline self-test (safe, no device):

  python3 scripts/h7_flash_bench.py --dry-run

Benchmark the existing baseline build (3 repeats):

  sudo usbip attach -r <device-ip> -b 1-1          # once, by hand
  python3 scripts/h7_flash_bench.py --yes --label baseline --repeats 3 \\
      --openocd build/openocd-master-install/bin/openocd \\
      --scripts build/openocd-master-install/share/openocd/scripts

Benchmark the pipelining candidate build and A/B against the baseline:

  python3 scripts/h7_flash_bench.py --yes --label speed-v1 --repeats 3 \\
      --openocd build/openocd-master-speed/install/bin/openocd \\
      --scripts build/openocd-master-speed/install/share/openocd/scripts \\
      --compare /tmp/speed/bench-baseline/result.json

Reproduce upstream behavior with the candidate binary (same binary A/B):

  ... --openocd-config "cmsis-dap pending_requests 4" \\
      --openocd-config "cmsis-dap eager_reads disable"

Decide whether the program phase is link-bound or flash-bound (adapter speed
sweep; program+erase only, no full/read):

  python3 scripts/h7_flash_bench.py --yes --label h7-sweep \\
      --speed-sweep 1000,1900,2400,4000 --repeats 2

Exit codes: 0 ok, 1 phase/OpenOCD failure, 2 usage/config error,
            3 destructive phases requested without --yes.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_BIN = Path("~/firmware/reference/firmware.bin")
DEFAULT_OPENOCD = REPO_ROOT / "build/openocd-master-install/bin/openocd"
DEFAULT_SCRIPTS = REPO_ROOT / "build/openocd-master-install/share/openocd/scripts"
DEFAULT_CANDIDATE_OPENOCD = REPO_ROOT / "build/openocd-master-speed/install/bin/openocd"
DEFAULT_CANDIDATE_SCRIPTS = REPO_ROOT / "build/openocd-master-speed/install/share/openocd/scripts"

DEFAULT_BASE_ADDR = 0x08000000
DEFAULT_FLASH_SIZE = 0x100000  # STM32H723/H725/H730 1 MiB variant
DEFAULT_SECTOR_SIZE = 0x20000  # H7 main-flash sector = 128 KiB
DEFAULT_READ_SIZE = 0x100000
DEFAULT_VID_PID = "0xc251 0xf00a"
# sha256 of the current reference firmware reference-board reference image; only enforced when the
# caller passes --expect-sha256.
REFERENCE_SHA256 = "0a91d59dbe857f055d13a81380af235b277637f25202285e4ccfe210b45081c0"

ALL_PHASES = ("startup", "erase", "program", "verify", "full", "read",
              "step", "smallread", "smallwrite")
#: Default phase set (historical behaviour; the small-transaction phases
#: are opt-in through --phases).
DEFAULT_PHASES = ("startup", "erase", "program", "verify", "full", "read")
DESTRUCTIVE_PHASES = {"erase", "program", "full"}
#: Phases that count DAP operations instead of bytes.
OPERATION_PHASES = {"step": "step_count", "smallread": "small_count",
                    "smallwrite": "small_count"}
# Phases whose throughput can be derived from a known byte count.
PHASE_BYTES = {"program": "image", "verify": "image", "full": "image", "read": "read"}


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[h7-bench] {msg}", flush=True)


def die(msg: str, code: int = 2) -> "NoReturn":  # noqa: F821 (py3.7 compat)
    print(f"[h7-bench] ERROR: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def human_bytes(n: int) -> str:
    for unit, scale in (("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= scale:
            return f"{n / scale:.1f} {unit}"
    return f"{n} B"


def kib_per_s(nbytes: int, seconds: float) -> float:
    if seconds <= 0:
        return 0.0
    return nbytes / 1024.0 / seconds


def tcl_quote(text: str) -> str:
    """Quote a path/string for use as one TCL word (braces handle spaces)."""
    if "}" in text or "{" in text:
        raise ValueError(f"path not TCL-safe: {text!r}")
    return "{" + text + "}"


@dataclass
class BenchConfig:
    bin_path: Path
    base_addr: int = DEFAULT_BASE_ADDR
    flash_size: int = DEFAULT_FLASH_SIZE
    sector_size: int = DEFAULT_SECTOR_SIZE
    openocd: Path = DEFAULT_OPENOCD
    scripts: Path = DEFAULT_SCRIPTS
    interface_cfg: str = "interface/cmsis-dap.cfg"
    target_cfg: str = "target/stm32h7x.cfg"
    adapter_speed: int = 1900
    vid_pid: str = DEFAULT_VID_PID
    extra_config: list = field(default_factory=list)
    read_addr: int = DEFAULT_BASE_ADDR
    read_size: int = DEFAULT_READ_SIZE
    repeats: int = 3
    warmup: int = 0
    timeout_s: int = 120
    out_dir: Path = Path("/tmp/h7-flash-bench")
    label: str = "run"
    yes: bool = False
    dry_run: bool = False
    keep_going: bool = False
    verbose: bool = False
    expect_sha256: str = ""
    phases: list = field(default_factory=lambda: list(ALL_PHASES))
    compare: Path = None
    check_usbip: bool = False
    speed_sweep: list = field(default_factory=list)  # kHz values
    step_count: int = 200
    small_count: int = 400
    small_addr: int = 0x20000000  # target RAM (H7 DTCM by default)


# ---------------------------------------------------------------------------
# image / plan validation
# ---------------------------------------------------------------------------

@dataclass
class ImageInfo:
    path: Path
    size: int
    sha256: str
    first_sector: int
    last_sector: int


def load_image(cfg: BenchConfig) -> ImageInfo:
    path = cfg.bin_path
    if not path.is_file():
        die(f"image not found: {path}")
    size = path.stat().st_size
    if size <= 0:
        die(f"image is empty: {path}")
    if size > cfg.flash_size:
        die(f"image {human_bytes(size)} does not fit flash "
            f"{human_bytes(cfg.flash_size)}")
    if cfg.base_addr % cfg.sector_size:
        die("--base-addr must be sector aligned")
    sha = sha256_file(path)
    if cfg.expect_sha256 and sha.lower() != cfg.expect_sha256.lower():
        die(f"image sha256 mismatch: got {sha}, expected {cfg.expect_sha256}")
    last = (size + cfg.sector_size - 1) // cfg.sector_size - 1
    if (last + 1) * cfg.sector_size > cfg.flash_size:
        die("image sectors exceed the configured flash size")
    return ImageInfo(path=path, size=size, sha256=sha,
                    first_sector=0, last_sector=last)


# ---------------------------------------------------------------------------
# OpenOCD command construction
# ---------------------------------------------------------------------------

def openocd_argv(cfg: BenchConfig, script_path: Path | None = None) -> list:
    argv = [
        str(cfg.openocd),
        "-f", cfg.interface_cfg,
        "-c", f"adapter usb vid_pid {cfg.vid_pid}",
        "-c", "transport select swd",
        "-f", cfg.target_cfg,
        # adapter speed must come after the target config (the target script
        # may set its own default, e.g. 2000 kHz).
        "-c", f"adapter speed {cfg.adapter_speed}",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
    ]
    for command in cfg.extra_config:
        argv += ["-c", command]
    argv += ["-c", "init", "-c", "reset halt"]
    if script_path is not None:
        argv += ["-f", str(script_path)]
    argv += ["-c", "shutdown"]
    return argv


def phase_tcl(cfg: BenchConfig, image: ImageInfo, phase: str,
              read_tmp: Path) -> list:
    """Return the TCL lines for one phase (already-quoted arguments)."""
    bin_q = tcl_quote(str(image.path.resolve()))
    base = f"0x{cfg.base_addr:08x}"
    if phase == "startup":
        return []
    if phase == "erase":
        return [f"flash erase_sector 0 {image.first_sector} {image.last_sector}"]
    if phase == "program":
        return [f"flash write_image {bin_q} {base}"]
    if phase == "verify":
        return [f"verify_image {bin_q} {base}"]
    if phase == "full":
        return [f"flash write_image erase {bin_q} {base}",
                f"verify_image {bin_q} {base}"]
    if phase == "read":
        return [f"dump_image {tcl_quote(str(read_tmp))} "
                f"0x{cfg.read_addr:08x} 0x{cfg.read_size:08x}"]
    if phase == "step":
        # One DAP transaction (+ one halt/resume) per instruction.
        return [f"for {{set i 0}} {{$i < {cfg.step_count}}} {{incr i}} {{ step }}"]
    if phase in ("smallread", "smallwrite"):
        # One word per CMSIS-DAP Transfer transaction; `mdw`/`mww` each map to
        # exactly one DAP_Transfer, which is the granularity the ESP32-side
        # copy optimisation changes.
        addr = f"0x{cfg.small_addr:08x}"
        if phase == "smallread":
            command = f"mdw {addr}"
        else:
            command = f"mww {addr} 0x12345678"
        return [f"for {{set i 0}} {{$i < {cfg.small_count}}} {{incr i}} "
                f"{{ {command} }}"]
    raise ValueError(f"unknown phase {phase}")


# ---------------------------------------------------------------------------
# execution
# ---------------------------------------------------------------------------

def run_openocd(cfg: BenchConfig, argv: list, log_path: Path) -> tuple:
    """Run one OpenOCD invocation; return (rc, seconds, tail_of_log)."""
    env = os.environ.copy()
    env["OPENOCD_SCRIPTS"] = str(cfg.scripts)
    started = time.monotonic()
    try:
        proc = subprocess.run(argv, env=env, capture_output=True, text=True,
                              timeout=cfg.timeout_s)
        seconds = time.monotonic() - started
        output = (proc.stdout or "") + (proc.stderr or "")
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        seconds = time.monotonic() - started
        output = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        output += f"\n[h7-bench] TIMEOUT after {cfg.timeout_s}s\n"
        rc = 124
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(
        f"$ {' '.join(argv)}\n\n{output}", encoding="utf-8")
    tail = "\n".join(output.strip().splitlines()[-12:])
    return rc, seconds, tail


def _run_phase(cfg: BenchConfig, image: ImageInfo, phase: str, repeat: int,
               out_dir: Path, script_dir: Path, log_dir: Path,
               execute: bool) -> dict:
    read_tmp = out_dir / f"read-r{repeat}.bin"
    lines = phase_tcl(cfg, image, phase, read_tmp)
    script_path = script_dir / f"{phase}-r{repeat}.tcl"
    script_text = "\n".join(lines) + ("\n" if lines else "")
    result = {
        "phase": phase,
        "repeat": repeat,
        "rc": None,
        "seconds": None,
        "log": str(log_dir / f"{phase}-r{repeat}.log"),
        "tcl": script_text,
    }
    if not execute:
        result["planned"] = True
        return result
    script_path.parent.mkdir(parents=True, exist_ok=True)
    script_path.write_text(script_text, encoding="utf-8")
    argv = openocd_argv(cfg, script_path)
    if cfg.verbose:
        log(f"run {phase} #{repeat}: {' '.join(argv)}")
    rc, seconds, tail = run_openocd(cfg, argv, Path(result["log"]))
    result["rc"] = rc
    result["seconds"] = seconds
    if phase == "read" and read_tmp.exists():
        try:
            result["read_bytes"] = read_tmp.stat().st_size
        finally:
            read_tmp.unlink()
    if rc != 0:
        result["error_tail"] = tail
        log(f"FAILED {phase} #{repeat} rc={rc}\n{tail}")
    else:
        log(f"{phase:<8} #{repeat}: {seconds:8.3f} s")
    return result


def execute_plan(cfg: BenchConfig, image: ImageInfo) -> dict:
    out_dir = cfg.out_dir
    script_dir = out_dir / "scripts"
    log_dir = out_dir / "logs"
    script_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    results = []
    aborted = False
    total_runs = cfg.warmup + cfg.repeats
    for repeat in range(total_runs):
        warmup = repeat < cfg.warmup
        for phase in cfg.phases:
            if aborted:
                break
            rec = _run_phase(cfg, image, phase, repeat, out_dir,
                             script_dir, log_dir, execute=True)
            rec["warmup"] = warmup
            results.append(rec)
            if rec["rc"] not in (0, None) and not warmup:
                if not cfg.keep_going:
                    aborted = True
                    break
        if aborted:
            log("aborting remaining repeats (use --keep-going to continue)")
            break
    return summarize(cfg, image, results)


# ---------------------------------------------------------------------------
# summarising / reporting
# ---------------------------------------------------------------------------

def summarize(cfg: BenchConfig, image: ImageInfo, results: list) -> dict:
    measured = [r for r in results if not r.get("warmup")]
    phases = {}
    for phase in cfg.phases:
        runs = [r for r in measured if r["phase"] == phase]
        times = [r["seconds"] for r in runs
                 if r["rc"] == 0 and r["seconds"] is not None]
        entry = {
            "runs": len(runs),
            "failures": sum(1 for r in runs if r["rc"] not in (0, None)),
            "times_s": times,
        }
        if times:
            entry.update({
                "min_s": min(times),
                "median_s": statistics.median(times),
                "mean_s": statistics.mean(times),
                "stdev_s": statistics.stdev(times) if len(times) > 1 else 0.0,
            })
            byte_key = PHASE_BYTES.get(phase)
            if byte_key == "image":
                entry["bytes"] = image.size
                entry["throughput_kib_s"] = kib_per_s(image.size,
                                                     entry["min_s"])
            elif byte_key == "read":
                sizes = [r.get("read_bytes", cfg.read_size) for r in runs
                         if r["rc"] == 0]
                nbytes = sizes[0] if sizes else cfg.read_size
                entry["bytes"] = nbytes
                entry["throughput_kib_s"] = kib_per_s(nbytes, entry["min_s"])
            elif phase in OPERATION_PHASES:
                ops = getattr(cfg, OPERATION_PHASES[phase])
                entry["ops"] = ops
                entry["ops_per_s"] = ops / entry["min_s"] if entry["min_s"] else 0.0
                entry["us_per_op"] = entry["min_s"] * 1e6 / ops if ops else 0.0
        phases[phase] = entry
    return {
        "schema": 1,
        "label": cfg.label,
        "timestamp": utc_now(),
        "image": {
            "path": str(image.path),
            "size": image.size,
            "sha256": image.sha256,
            "base_addr": f"0x{cfg.base_addr:08x}",
            "first_sector": image.first_sector,
            "last_sector": image.last_sector,
        },
        "target": {
            "interface_cfg": cfg.interface_cfg,
            "target_cfg": cfg.target_cfg,
            "adapter_speed_khz": cfg.adapter_speed,
            "vid_pid": cfg.vid_pid,
            "extra_config": list(cfg.extra_config),
        },
        "host": {
            "openocd": str(cfg.openocd),
            "scripts": str(cfg.scripts),
            "python": platform.python_version(),
            "uname": platform.uname().release,
        },
        "phases": phases,
        "results": results,
    }


def print_summary(summary: dict) -> None:
    log(f"label={summary['label']} image={summary['image']['size']} B "
        f"sha256={summary['image']['sha256'][:12]} "
        f"speed={summary['target']['adapter_speed_khz']} kHz "
        f"openocd={summary['host']['openocd']}")
    print()
    print(f"{'phase':<9}{'n':>3}{'min_s':>10}{'median_s':>10}"
          f"{'KiB/s':>10}  note")
    for phase, stats in summary["phases"].items():
        if phase == "startup":
            note = "includes OpenOCD startup"
        elif phase == "read":
            note = f"{stats.get('bytes', 0)} B read"
        elif "bytes" in stats:
            note = f"{stats['bytes']} B"
        elif "ops" in stats:
            note = (f"{stats['ops']} DAP ops, "
                    f"{stats.get('us_per_op', 0.0):.0f} us/op")
        else:
            note = ""
        if stats.get("times_s"):
            print(f"{phase:<9}{stats['runs']:>3}{stats['min_s']:>10.3f}"
                  f"{stats['median_s']:>10.3f}"
                  f"{stats.get('throughput_kib_s', 0.0):>10.1f}  {note}")
        else:
            print(f"{phase:<9}{stats['runs']:>3}{'-':>10}{'-':>10}{'-':>10}"
                  f"  no successful run ({stats['failures']} failures)")
    print()


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        die(f"cannot read baseline JSON {path}: {exc}")


def print_comparison(current: dict, baseline: dict) -> list:
    rows = []
    base_phases = baseline.get("phases", {})
    for phase, stats in current.get("phases", {}).items():
        if not stats.get("min_s"):
            continue
        base = base_phases.get(phase, {})
        if not base.get("min_s"):
            continue
        speedup = base["min_s"] / stats["min_s"]
        rows.append({
            "phase": phase,
            "baseline_min_s": base["min_s"],
            "current_min_s": stats["min_s"],
            "current_over_baseline": speedup,
        })
    if not rows:
        log("comparison: no overlapping measured phases")
        return rows
    print(f"comparison vs baseline label={baseline.get('label', '?')} "
          f"({baseline.get('timestamp', '?')})")
    print(f"{'phase':<9}{'baseline_s':>12}{'current_s':>12}{'speedup':>10}")
    for row in rows:
        print(f"{row['phase']:<9}{row['baseline_min_s']:>12.3f}"
              f"{row['current_min_s']:>12.3f}"
              f"{row['current_over_baseline']:>9.2f}x")
    print()
    return rows


def write_outputs(cfg: BenchConfig, summary: dict) -> tuple:
    out_dir = cfg.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / "result.json"
    csv_path = out_dir / "summary.csv"
    json_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["label", "phase", "repeat", "warmup", "rc",
                         "seconds", "adapter_speed_khz", "image_sha256"])
        for rec in summary["results"]:
            writer.writerow([
                summary["label"], rec["phase"], rec["repeat"],
                int(bool(rec.get("warmup"))), rec["rc"],
                "" if rec["seconds"] is None else f"{rec['seconds']:.6f}",
                summary["target"]["adapter_speed_khz"],
                summary["image"]["sha256"],
            ])
    return json_path, csv_path


# ---------------------------------------------------------------------------
# dry-run / self-test
# ---------------------------------------------------------------------------

def self_test(cfg: BenchConfig, image: ImageInfo) -> int:
    """Offline checks of plan construction, summary math and comparison."""
    checks = 0
    plan = {}
    for phase in ALL_PHASES:
        script = phase_tcl(cfg, image, phase, cfg.out_dir / "read.bin")
        plan[phase] = script
        checks += 1
    assert "flash erase_sector 0 0" in plan["erase"][0]
    assert "flash write_image erase" in plan["full"][0]
    assert "verify_image" in plan["full"][1]
    assert "dump_image" in plan["read"][0]
    assert plan["startup"] == []
    assert plan["step"] and f"$i < {cfg.step_count}" in plan["step"][0]
    assert plan["smallread"] and plan["smallread"][0].count("mdw 0x20000000") == 1
    assert plan["smallwrite"] and "mww 0x20000000 0x12345678" in plan["smallwrite"][0]
    checks += 3

    # operation-count phases produce per-op latency metrics
    fake_ops = [
        {"phase": "step", "repeat": 0, "rc": 0, "seconds": 2.0,
         "warmup": False, "log": ""},
        {"phase": "smallread", "repeat": 0, "rc": 0, "seconds": 1.0,
         "warmup": False, "log": ""},
    ]
    saved_phases = cfg.phases
    cfg.phases = ["step", "smallread"]
    ops_summary = summarize(cfg, image, fake_ops)
    cfg.phases = saved_phases
    assert ops_summary["phases"]["step"]["ops"] == cfg.step_count
    assert abs(ops_summary["phases"]["step"]["us_per_op"]
               - 2.0e6 / cfg.step_count) < 1e-6
    assert ops_summary["phases"]["smallread"]["ops"] == cfg.small_count
    checks += 2

    # summaries with synthetic runs
    fake = [
        {"phase": "program", "repeat": 0, "rc": 0, "seconds": 5.0,
         "warmup": False, "log": ""},
        {"phase": "program", "repeat": 1, "rc": 0, "seconds": 4.0,
         "warmup": False, "log": ""},
        {"phase": "program", "repeat": 2, "rc": 0, "seconds": 6.0,
         "warmup": False, "log": ""},
    ]
    saved_phases = cfg.phases
    cfg.phases = ["program"]
    summary = summarize(cfg, image, fake)
    cfg.phases = saved_phases
    stats = summary["phases"]["program"]
    assert stats["min_s"] == 4.0 and stats["median_s"] == 5.0
    expected_tp = image.size / 1024.0 / 4.0
    assert abs(stats["throughput_kib_s"] - expected_tp) < 1e-9
    checks += 3

    baseline = {"label": "selftest-baseline", "timestamp": utc_now(),
                "phases": {"program": {"min_s": 8.0}}}
    rows = print_comparison(summary, baseline)
    assert len(rows) == 1 and rows[0]["current_over_baseline"] == 2.0
    checks += 1

    # command construction sanity (no execution)
    argv = openocd_argv(cfg)
    assert argv[0] == str(cfg.openocd)
    assert f"adapter speed {cfg.adapter_speed}" in argv
    assert "reset_config none" in argv
    assert "shutdown" in argv
    checks += 1

    # write/read output round-trip
    cfg.out_dir.mkdir(parents=True, exist_ok=True)
    json_path, csv_path = write_outputs(cfg, summary)
    assert json_path.is_file() and csv_path.is_file()
    loaded = load_json(json_path)
    assert loaded["phases"]["program"]["min_s"] == 4.0
    checks += 2

    print(f"self-test OK ({checks} checks)")
    return checks


def dry_run(cfg: BenchConfig, image: ImageInfo) -> int:
    log("DRY RUN: no OpenOCD will be executed and no hardware is touched")
    log(f"image: {image.path} size={image.size} B "
        f"sha256={image.sha256}")
    log(f"sectors: {image.first_sector}..{image.last_sector} "
        f"(sector size {cfg.sector_size})")
    log(f"openocd: {cfg.openocd} (exists={cfg.openocd.is_file()})")
    log(f"scripts: {cfg.scripts} (exists={cfg.scripts.is_dir()})")
    log(f"phases: {', '.join(cfg.phases)} repeats={cfg.repeats} "
        f"warmup={cfg.warmup} adapter_speed={cfg.adapter_speed} kHz")
    print()

    out_dir = cfg.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    script_dir = out_dir / "scripts"
    script_dir.mkdir(parents=True, exist_ok=True)
    plan = {}
    for repeat in range(1):
        for phase in cfg.phases:
            read_tmp = out_dir / f"read-r{repeat}.bin"
            script = phase_tcl(cfg, image, phase, read_tmp)
            plan[phase] = script
            script_path = script_dir / f"{phase}-dryrun.tcl"
            script_path.write_text("\n".join(script) + ("\n" if script else ""),
                                   encoding="utf-8")
            print(f"--- {phase} ---")
            print(f"  tcl: {script if script else '# (init/reset halt only)'}")
            print(f"  cmd: {' '.join(openocd_argv(cfg, script_path))}")

    plan_json = {
        "schema": 1,
        "dry_run": True,
        "timestamp": utc_now(),
        "label": cfg.label,
        "image": {"path": str(image.path), "size": image.size,
                  "sha256": image.sha256, "first_sector": image.first_sector,
                  "last_sector": image.last_sector},
        "target": {"adapter_speed_khz": cfg.adapter_speed,
                   "vid_pid": cfg.vid_pid, "target_cfg": cfg.target_cfg,
                   "extra_config": list(cfg.extra_config)},
        "phases": plan,
        "openocd_exists": cfg.openocd.is_file(),
        "scripts_exist": cfg.scripts.is_dir(),
    }
    raw_path = out_dir / "dry-run.json"
    raw_path.write_text(json.dumps(plan_json, indent=2) + "\n",
                        encoding="utf-8")
    print()
    self_test(cfg, image)
    log(f"dry-run artefacts: {raw_path}")
    return 0


# ---------------------------------------------------------------------------
# safety / environment checks
# ---------------------------------------------------------------------------

def preflight(cfg: BenchConfig) -> None:
    destructive = [p for p in cfg.phases if p in DESTRUCTIVE_PHASES]
    if destructive and not cfg.yes:
        die("refusing to run destructive phase(s) "
            f"{', '.join(destructive)} without --yes "
            "(this command erases/programs the target flash)", 3)
    if cfg.phases and not cfg.yes:
        log("read-only phases only; pass --yes to allow erase/program")
    if not cfg.openocd.is_file():
        die(f"OpenOCD binary not found: {cfg.openocd}")
    if not os.access(cfg.openocd, os.X_OK):
        die(f"OpenOCD binary is not executable: {cfg.openocd}")
    if not cfg.scripts.is_dir():
        die(f"OpenOCD scripts directory not found: {cfg.scripts}")
    if cfg.check_usbip:
        try:
            out = subprocess.run(["usbip", "port"], capture_output=True,
                                 text=True, timeout=20).stdout
        except (OSError, subprocess.TimeoutExpired) as exc:
            log(f"WARNING: could not run 'usbip port': {exc}")
            return
        if "c251:f00a" in out.lower() or "cmsis-dap" in out.lower():
            log("usbip: CMSIS-DAP USBIP device is attached")
        else:
            log("WARNING: 'usbip port' does not show a c251:f00a CMSIS-DAP "
                "device; attach it first: sudo usbip attach -r "
                "<device-ip> -b 1-1")


def openocd_version(cfg: BenchConfig) -> str:
    try:
        proc = subprocess.run([str(cfg.openocd), "--version"],
                              capture_output=True, text=True, timeout=20)
        return (proc.stdout or proc.stderr).strip().splitlines()[0]
    except (OSError, subprocess.TimeoutExpired, IndexError):
        return "unknown"


def run_speed_sweep(cfg: BenchConfig, image: ImageInfo) -> int:
    """Program/erase timing at each adapter speed (link-vs-flash verdict)."""
    cfg.phases = ["startup", "erase", "program"]
    preflight(cfg)
    cfg.out_dir.mkdir(parents=True, exist_ok=True)
    (cfg.out_dir / "scripts").mkdir(parents=True, exist_ok=True)
    (cfg.out_dir / "logs").mkdir(parents=True, exist_ok=True)
    log(f"adapter-speed sweep: {cfg.speed_sweep} kHz, phases "
        f"startup+erase+program x{cfg.repeats}")
    sweep = {}
    for speed in cfg.speed_sweep:
        cfg.adapter_speed = speed
        log(f"--- adapter speed {speed} kHz ---")
        results = []
        for repeat in range(cfg.repeats):
            for phase in ("startup", "erase", "program"):
                rec = _run_phase(cfg, image, phase, repeat, cfg.out_dir,
                                 cfg.out_dir / "scripts", cfg.out_dir / "logs",
                                 execute=True)
                rec["warmup"] = False
                results.append(rec)
                if rec["rc"] != 0 and not cfg.keep_going:
                    die(f"phase {phase} failed at {speed} kHz", 1)
        saved = cfg.phases
        cfg.phases = ["startup", "erase", "program"]
        summary = summarize(cfg, image, results)
        cfg.phases = saved
        sweep[str(speed)] = summary
    cfg.out_dir.mkdir(parents=True, exist_ok=True)
    path = cfg.out_dir / "sweep.json"
    path.write_text(json.dumps({"schema": 1, "label": cfg.label,
                                "timestamp": utc_now(),
                                "image": {"size": image.size,
                                          "sha256": image.sha256},
                                "speeds": sweep}, indent=2) + "\n",
                    encoding="utf-8")
    print(f"{'speed_khz':>10}{'erase_min_s':>13}{'program_min_s':>15}"
          f"{'program_KiB/s':>15}")
    for speed, summary in sweep.items():
        erase = summary["phases"].get("erase", {})
        program = summary["phases"].get("program", {})
        print(f"{speed:>10}"
              f"{erase.get('min_s', float('nan')):>13.3f}"
              f"{program.get('min_s', float('nan')):>15.3f}"
              f"{program.get('throughput_kib_s', float('nan')):>15.1f}")
    log(f"wrote {path}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv: list) -> BenchConfig:
    parser = argparse.ArgumentParser(
        description="Repeatable STM32H7 (WiFi USBIP CMSIS-DAP) flash/debug "
                    "benchmark harness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples\n--------\n", 1)[-1],
    )
    parser.add_argument("--bin", type=Path, default=DEFAULT_BIN,
                        help=f"firmware image (default: {DEFAULT_BIN})")
    parser.add_argument("--base-addr", default=f"0x{DEFAULT_BASE_ADDR:08x}",
                        help="flash base address (default: 0x08000000)")
    parser.add_argument("--flash-size", default=f"0x{DEFAULT_FLASH_SIZE:x}",
                        help="target flash size (default: 0x100000 = 1 MiB)")
    parser.add_argument("--sector-size", default=f"0x{DEFAULT_SECTOR_SIZE:x}",
                        help="flash sector size (default: 0x20000 = 128 KiB)")
    parser.add_argument("--openocd", type=Path, default=DEFAULT_OPENOCD,
                        help="OpenOCD binary (default: build/openocd-master-install/bin/openocd)")
    parser.add_argument("--scripts", type=Path, default=DEFAULT_SCRIPTS,
                        help="OPENOCD_SCRIPTS directory (default: build/openocd-master-install/share/openocd/scripts)")
    parser.add_argument("--candidate", action="store_true",
                        help=f"use the pipelining candidate tree "
                             f"({DEFAULT_CANDIDATE_OPENOCD})")
    parser.add_argument("--interface-cfg", default="interface/cmsis-dap.cfg")
    parser.add_argument("--target-cfg", default="target/stm32h7x.cfg")
    parser.add_argument("--adapter-speed", type=int, default=1900,
                        help="SWD clock in kHz (default: 1900)")
    parser.add_argument("--vid-pid", default=DEFAULT_VID_PID,
                        help="CMSIS-DAP USB VID/PID (default: 0xc251 0xf00a)")
    parser.add_argument("--openocd-config", action="append", default=[],
                        metavar="TCL",
                        help="extra -c config command inserted before init "
                             "(repeatable), e.g. "
                             "\"cmsis-dap pending_requests 4\"")
    parser.add_argument("--read-addr", default=f"0x{DEFAULT_BASE_ADDR:08x}",
                        help="dump_image address (default: flash base)")
    parser.add_argument("--read-size", default=f"0x{DEFAULT_READ_SIZE:x}",
                        help="dump_image size (default: 0x100000 = 1 MiB)")
    parser.add_argument("--step-count", type=int, default=200,
                        help="number of single-instruction steps for the "
                             "'step' phase (default: 200)")
    parser.add_argument("--small-count", type=int, default=400,
                        help="number of single-word transfers for the "
                             "'smallread'/'smallwrite' phases (default: 400)")
    parser.add_argument("--small-addr", default="0x20000000",
                        help="target RAM address for smallread/smallwrite "
                             "(default: 0x20000000, H7 DTCM)")
    parser.add_argument("--phases", default=",".join(DEFAULT_PHASES),
                        help="comma-separated phases (valid: "
                             f"{','.join(ALL_PHASES)}; default: "
                             f"{','.join(DEFAULT_PHASES)})")
    parser.add_argument("--repeats", type=int, default=3,
                        help="measured repeats per phase (default: 3)")
    parser.add_argument("--warmup", type=int, default=0,
                        help="discarded warm-up repeats per phase (default: 0)")
    parser.add_argument("--timeout", type=int, default=120,
                        help="per-OpenOCD-invocation timeout seconds (default: 120)")
    parser.add_argument("--out", type=Path, default=Path("/tmp/h7-flash-bench"),
                        help="output directory (default: /tmp/h7-flash-bench)")
    parser.add_argument("--label", default="run",
                        help="label for this run, used in JSON/CSV")
    parser.add_argument("--compare", type=Path, default=None,
                        help="baseline result.json to A/B compare against")
    parser.add_argument("--speed-sweep", default="",
                        help="comma-separated adapter speeds (kHz) for the "
                             "erase+program link-vs-flash sweep, e.g. "
                             "1000,1900,2400,4000")
    parser.add_argument("--expect-sha256", default="",
                        help="abort if the image sha256 does not match this value")
    parser.add_argument("--yes", action="store_true",
                        help="allow destructive phases (erase/program/full); "
                             "required, this is real flash")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the plan and self-test offline; never executes OpenOCD")
    parser.add_argument("--keep-going", action="store_true",
                        help="continue after a failed OpenOCD run")
    parser.add_argument("--check-usbip", action="store_true",
                        help="run 'usbip port' (read-only) to check the device is attached")
    parser.add_argument("--verbose", action="store_true",
                        help="print every OpenOCD command line")
    args = parser.parse_args(argv)

    if args.candidate:
        args.openocd = DEFAULT_CANDIDATE_OPENOCD
        args.scripts = DEFAULT_CANDIDATE_SCRIPTS

    phases = [p.strip() for p in args.phases.split(",") if p.strip()]
    bad = [p for p in phases if p not in ALL_PHASES]
    if bad:
        die(f"unknown phase(s): {', '.join(bad)}; valid: {', '.join(ALL_PHASES)}")
    if not phases:
        die("no phases selected")
    speeds = [int(s) for s in args.speed_sweep.replace(",", " ").split()]
    if any(s <= 0 for s in speeds):
        die("--speed-sweep values must be positive kHz")
    if args.repeats < 1 or args.warmup < 0:
        die("--repeats must be >= 1 and --warmup >= 0")
    if args.timeout < 5:
        die("--timeout must be >= 5 s")
    if not 1 <= args.step_count <= 10000:
        die("--step-count must be in 1..10000")
    if not 1 <= args.small_count <= 10000:
        die("--small-count must be in 1..10000")
    for value, name in ((args.base_addr, "--base-addr"),
                        (args.flash_size, "--flash-size"),
                        (args.sector_size, "--sector-size"),
                        (args.read_addr, "--read-addr"),
                        (args.read_size, "--read-size"),
                        (args.small_addr, "--small-addr")):
        try:
            int(value, 0)
        except ValueError:
            die(f"{name} is not a number: {value}")

    cfg = BenchConfig(
        bin_path=args.bin.expanduser(),
        base_addr=int(args.base_addr, 0),
        flash_size=int(args.flash_size, 0),
        sector_size=int(args.sector_size, 0),
        openocd=args.openocd.expanduser(),
        scripts=args.scripts.expanduser(),
        interface_cfg=args.interface_cfg,
        target_cfg=args.target_cfg,
        adapter_speed=args.adapter_speed,
        vid_pid=args.vid_pid,
        extra_config=list(args.openocd_config),
        read_addr=int(args.read_addr, 0),
        read_size=int(args.read_size, 0),
        repeats=args.repeats,
        warmup=args.warmup,
        timeout_s=args.timeout,
        out_dir=args.out.expanduser(),
        label=args.label,
        yes=args.yes,
        dry_run=args.dry_run,
        keep_going=args.keep_going,
        verbose=args.verbose,
        expect_sha256=args.expect_sha256,
        phases=phases,
        compare=args.compare,
        check_usbip=args.check_usbip,
        speed_sweep=speeds,
        step_count=args.step_count,
        small_count=args.small_count,
        small_addr=int(args.small_addr, 0),
    )
    return cfg


def main(argv: list) -> int:
    cfg = parse_args(argv)
    image = load_image(cfg)

    if cfg.dry_run:
        return dry_run(cfg, image)

    if cfg.speed_sweep:
        return run_speed_sweep(cfg, image)

    preflight(cfg)
    version = openocd_version(cfg)
    log(f"OpenOCD: {version}")
    log(f"label={cfg.label} image={image.path} "
        f"size={image.size} B sha256={image.sha256[:16]}...")
    log(f"phases={','.join(cfg.phases)} repeats={cfg.repeats} "
        f"warmup={cfg.warmup} speed={cfg.adapter_speed} kHz")
    cfg.out_dir.mkdir(parents=True, exist_ok=True)

    summary = execute_plan(cfg, image)
    summary["host"]["openocd_version"] = version
    json_path, csv_path = write_outputs(cfg, summary)
    print_summary(summary)

    comparison = []
    if cfg.compare is not None:
        comparison = print_comparison(summary, load_json(cfg.compare))
        summary["comparison"] = comparison
        # rewrite result.json with the comparison included
        json_path.write_text(json.dumps(summary, indent=2) + "\n",
                             encoding="utf-8")
    if any(stats.get("failures") for stats in summary["phases"].values()):
        log(f"result: FAIL (see {cfg.out_dir / 'logs'})")
        return 1
    log(f"result: OK  json={json_path} csv={csv_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        print("\n[h7-bench] interrupted", file=sys.stderr)
        sys.exit(130)
