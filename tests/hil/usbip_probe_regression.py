#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""HIL regression: HTTP /api/target/info probe -> first USBIP/OpenOCD session.

Task-66 regression (real hardware, H7 target, WiFi or USB-NCM transport).

Symptom that this script verifies is gone:

    GET /api/target/info            # Web dashboard target probe
    usbip attach -r <device> -b 1-1
    openocd ... -c init -c shutdown
    -> Error: CMSIS-DAP command CMD_INFO failed.

Root cause: the target-flash probe takes the DAP engine with DELETE_HANDLE and
the old DAP task freed both ring buffers for it; the following RESET_HANDLE
re-create needed a 20,672-byte contiguous block (response ring) but the
Wi-Fi+NCM heap only had a 20,480-byte largest free block, so it failed and the
next USBIP session had no DAP data path.  Fix: DELETE_HANDLE now only drains +
detaches (rings kept for the whole boot); RESET_HANDLE only drains + restores.

What this script does per iteration (identify-only, never halts/writes/flashes):

  1. GET /api/target/info                     (unless --control)
  2. GET /api/target/status -> dap_ring_active must be true and
     dap_session_active must be false
  3. usbip detach (best effort) + usbip attach -r <host> -b <busid>
  4. openocd identify: CMSIS-DAP init, H7 examination, shutdown
  5. usbip detach (always, also on failure)

Modes:

  default                probe-then-session, expects every iteration to PASS
  --control              attach-first (no HTTP probe); use as the reference
  --expect-known-bug     pre-fix reproduction: expects the session to FAIL with
                         "CMD_INFO failed" and dap_ring_active=false; exits 0
                         when the bug reproduces (used to capture before/after
                         evidence).  Requires --reboot-first-broken to be
                         meaningful on a fixed firmware; not used post-fix.

Host requirements: root helper for usbip (default /tmp/hostroot.sh when
present, else $HIL_ROOT_HELPER, else "sudo -n"), OpenOCD with CMSIS-DAP
support, and network access to the device HTTP API.

Examples:
  python3 tests/hil/usbip_probe_regression.py --iterations 3
  python3 tests/hil/usbip_probe_regression.py --control --iterations 3
  python3 tests/hil/usbip_probe_regression.py --expect-known-bug --iterations 1
  python3 tests/hil/usbip_probe_regression.py --dry-run    # offline self-test

Exit codes: 0 PASS (or known bug reproduced), 1 FAIL, 2 usage/environment.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OPENOCD = REPO_ROOT / "build/openocd-master-install/bin/openocd"
DEFAULT_SCRIPTS = REPO_ROOT / "build/openocd-master-install/share/openocd/scripts"
TARGET_INFO_PATH = "/api/target/info"
TARGET_STATUS_PATH = "/api/target/status"


def log(message: str) -> None:
    print("[usbip-probe] %s" % message, flush=True)


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib only)
# ---------------------------------------------------------------------------

def http_get_json(host: str, port: int, path: str, timeout: float) -> dict:
    url = "http://%s:%d%s" % (host, port, path)
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read()
        return json.loads(body.decode("utf-8", "replace"))


# ---------------------------------------------------------------------------
# Command helpers
# ---------------------------------------------------------------------------

def default_root_helper() -> str:
    explicit = os.environ.get("HIL_ROOT_HELPER", "").strip()
    if explicit:
        return explicit
    hostroot = Path("/tmp/hostroot.sh")
    if hostroot.is_file():
        return str(hostroot)
    return "sudo -n"


def run_command(argv: list[str], timeout: float) -> dict:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=timeout, check=False)
        output = completed.stdout.decode("utf-8", "replace")
        return {"rc": completed.returncode, "seconds": round(time.monotonic() - started, 2),
                "output": output}
    except subprocess.TimeoutExpired as exc:
        output = (exc.output or b"").decode("utf-8", "replace")
        return {"rc": 124, "seconds": round(time.monotonic() - started, 2),
                "output": output + "\n<timeout>"}
    except OSError as exc:
        return {"rc": 127, "seconds": round(time.monotonic() - started, 2),
                "output": str(exc)}


def openocd_identify_argv(openocd: str, scripts: str) -> list[str]:
    """Identify-only CMSIS-DAP session (same shape as scripts/hil_extended.py)."""
    return [
        openocd,
        "-s", scripts,
        "-f", "interface/cmsis-dap.cfg",
        "-c", "adapter usb vid_pid 0xc251 0xf00a",
        "-c", "transport select swd",
        "-f", "target/stm32h7x.cfg",
        "-c", "adapter speed 1000",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
        "-c", "init",
        "-c", "shutdown",
    ]


def session_verdict(result: dict, expect_bug: bool) -> tuple[bool, str]:
    output = result["output"]
    init_ok = "Interface Initialised (SWD)" in output
    target_ok = "Cortex-M7" in output or "STM32H7" in output
    cmd_info_failed = "CMD_INFO failed" in output
    if expect_bug:
        ok = cmd_info_failed and not init_ok
        detail = ("known bug reproduced (CMD_INFO failed)" if ok
                  else "expected the known CMD_INFO bug; output did not match")
        return ok, detail
    ok = result["rc"] == 0 and init_ok and target_ok and not cmd_info_failed
    if ok:
        return True, "OpenOCD identified the STM32H7 target (%.2f s)" % result["seconds"]
    return False, "OpenOCD session failed (rc=%d): %s" % (
        result["rc"], output.strip().splitlines()[-1] if output.strip() else "<no output>")


def tail(text: str, limit: int = 400) -> str:
    text = text.strip()
    return text if len(text) <= limit else "..." + text[-limit:]


# ---------------------------------------------------------------------------
# Main HIL sequence
# ---------------------------------------------------------------------------

def run_iteration(index: int, args: argparse.Namespace, helper: list[str]) -> dict:
    record: dict = {"iteration": index, "status": "fail"}
    helper = list(helper)

    def root(argv: list[str], timeout: float) -> dict:
        return run_command(helper + list(argv), timeout)

    if args.control:
        record["probe"] = {"status": "skipped-control"}
    else:
        try:
            probe = http_get_json(args.host, args.port, TARGET_INFO_PATH, args.timeout)
            record["probe"] = {
                "ok": bool(probe.get("ok")),
                "detected": bool(probe.get("detected")),
                "family": probe.get("family"),
                "device": probe.get("device"),
                "uid": probe.get("uid"),
            }
        except (urllib.error.URLError, OSError, ValueError) as exc:
            record["probe"] = {"ok": False, "error": str(exc)}
            record["error"] = "HTTP probe failed: %s" % exc
            return record

    try:
        status = http_get_json(args.host, args.port, TARGET_STATUS_PATH, args.timeout)
        record["device_status"] = {
            "dap_ring_active": status.get("dap_ring_active"),
            "dap_ring_preallocated": status.get("dap_ring_preallocated"),
            "dap_session_active": status.get("dap_session_active"),
            "heap_free": status.get("heap_free"),
            "heap_largest": status.get("heap_largest"),
            "message": status.get("message"),
        }
    except (urllib.error.URLError, OSError, ValueError) as exc:
        record["error"] = "GET %s failed: %s" % (TARGET_STATUS_PATH, exc)
        return record

    ring_active = record["device_status"].get("dap_ring_active")
    if args.expect_known_bug:
        if ring_active is not False and not args.control:
            record["error"] = ("expected dap_ring_active=false (broken pre-fix "
                               "state), got %r" % (ring_active,))
            return record
    elif ring_active is not True:
        record["error"] = ("dap_ring_active must stay true after the probe "
                           "(ring buffers are created once at boot, task-66); "
                           "got %r" % (ring_active,))
        return record

    session = None
    try:
        record["detach_before"] = root(["usbip", "detach", "-p", "00"], 60)
        attach = root(["usbip", "attach", "-r", args.host, "-b", args.busid], 60)
        record["attach"] = attach
        if attach["rc"] != 0:
            record["error"] = "usbip attach failed: %s" % tail(attach["output"], 200)
            return record
        time.sleep(args.attach_settle)
        session = root(openocd_identify_argv(args.openocd, args.scripts),
                       args.openocd_timeout)
        record["session"] = session
        ok, detail = session_verdict(session, args.expect_known_bug)
        record["session_ok"] = ok
        record["detail"] = detail
        if ok:
            record["status"] = "pass-known-bug" if args.expect_known_bug else "pass"
        else:
            record["error"] = detail
            if args.expect_known_bug:
                record["status"] = "no-known-bug"
        return record
    finally:
        detach = root(["usbip", "detach", "-p", "00"], 60)
        record["detach_after"] = detach
        # Always leave the local vhci port clean; a stale attachment would make
        # the next iteration fail for the wrong reason.
        if session is not None and record.get("status", "").startswith("pass"):
            session["output"] = tail(session["output"], 600)


def print_summary(results: list[dict], expect_bug: bool) -> bool:
    passed = sum(1 for item in results if item.get("status", "").startswith("pass"))
    total = len(results)
    log("summary: %d/%d iterations %s" % (
        passed, total, "reproduced the known bug" if expect_bug else "passed"))
    for item in results:
        status = item.get("status", "fail")
        line = "  [%s] iteration %d" % (status, item["iteration"])
        if "detail" in item:
            line += ": " + item["detail"]
        if "error" in item:
            line += " | " + str(item["error"])
        log(line)
        status_info = item.get("device_status") or {}
        if status_info:
            log("        ring_active=%r heap_largest=%r message=%r" % (
                status_info.get("dap_ring_active"),
                status_info.get("heap_largest"),
                status_info.get("message")))
    return passed == total


def dry_run() -> int:
    """Offline self-test: exercise the verdict logic with fake OpenOCD outputs."""
    log("dry-run: no device / usbip / OpenOCD access")
    good = {"rc": 0, "seconds": 3.5, "output": (
        "Info : CMSIS-DAP: Interface Initialised (SWD)\n"
        "Info : [stm32h7x.cpu0] Cortex-M7 r1p2 processor detected\n"
        "Info : Examination succeed\n")}
    broken = {"rc": 1, "seconds": 3.1, "output": (
        "Info : Using CMSIS-DAPv2 interface with VID:PID=0xc251:0xf00a\n"
        "Error: CMSIS-DAP command CMD_INFO failed.\n")}
    checks = [
        ("good session passes", session_verdict(good, False)[0] is True),
        ("good session is not a known bug", session_verdict(good, True)[0] is False),
        ("broken session fails", session_verdict(broken, False)[0] is False),
        ("broken session reproduces known bug",
         session_verdict(broken, True)[0] is True),
    ]
    failed = 0
    for name, passed in checks:
        log("  [%s] %s" % ("ok" if passed else "FAIL", name))
        failed += 0 if passed else 1
    # Command construction sanity (no hardware touched).
    root_argv = [default_root_helper()] + ["usbip", "attach", "-r", "192.168.7.1",
                                           "-b", "1-1"]
    log("  root helper command: %s" % " ".join(shlex.quote(part) for part in root_argv))
    return 0 if failed == 0 else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="HIL regression for the task-66 target/info -> USBIP CMD_INFO bug")
    parser.add_argument("--host", default="192.168.7.1",
                        help="device IP for usbip attach + HTTP (default: %(default)s)")
    parser.add_argument("--port", type=int, default=80, help="device HTTP port")
    parser.add_argument("--busid", default="1-1", help="usbip remote bus id")
    parser.add_argument("--iterations", type=int, default=3,
                        help="probe+attach+identify+detach rounds (default: 3)")
    parser.add_argument("--openocd", default=str(DEFAULT_OPENOCD),
                        help="OpenOCD binary (default: build/openocd-master-install)")
    parser.add_argument("--scripts", default=str(DEFAULT_SCRIPTS),
                        help="OpenOCD scripts directory")
    parser.add_argument("--root-helper", default="",
                        help="root command prefix for usbip/OpenOCD "
                             "(default: $HIL_ROOT_HELPER, /tmp/hostroot.sh, sudo -n)")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="HTTP timeout in seconds")
    parser.add_argument("--openocd-timeout", type=float, default=90.0,
                        help="OpenOCD session timeout in seconds")
    parser.add_argument("--attach-settle", type=float, default=1.0,
                        help="seconds between usbip attach and OpenOCD")
    parser.add_argument("--control", action="store_true",
                        help="attach-first reference: skip the HTTP target probe")
    parser.add_argument("--expect-known-bug", action="store_true",
                        help="pre-fix reproduction mode: expect CMD_INFO failure")
    parser.add_argument("--json", default="",
                        help="write the full result JSON to this path")
    parser.add_argument("--dry-run", action="store_true",
                        help="offline self-test (verdict logic only)")
    args = parser.parse_args(argv)

    if args.dry_run:
        return dry_run()
    if args.iterations < 1:
        parser.error("--iterations must be >= 1")
    if not Path(args.openocd).is_file():
        log("OpenOCD binary not found: %s" % args.openocd)
        return 2
    if not Path(args.scripts).is_dir():
        log("OpenOCD scripts directory not found: %s" % args.scripts)
        return 2

    helper_text = args.root_helper.strip() or default_root_helper()
    helper = shlex.split(helper_text)
    log("device=%s busid=%s iterations=%d mode=%s" % (
        args.host, args.busid, args.iterations,
        ("known-bug-repro" if args.expect_known_bug else
         ("control-attach-first" if args.control else "probe-then-session"))))
    log("root helper: %s" % helper_text)

    results: list[dict] = []
    for index in range(1, args.iterations + 1):
        record = run_iteration(index, args, helper)
        results.append(record)
        log("iteration %d/%d: %s%s" % (
            index, args.iterations, record.get("status", "fail"),
            (" | " + record["error"]) if record.get("error") else ""))
        if not record.get("status", "").startswith("pass"):
            # Do not mask a failure with more iterations: one decisive failure
            # is the regression signal.
            break

    passed = print_summary(results, args.expect_known_bug)
    payload = {
        "task": "task-66",
        "host": args.host,
        "busid": args.busid,
        "mode": ("known-bug-repro" if args.expect_known_bug else
                 ("control-attach-first" if args.control else
                  "probe-then-session")),
        "iterations": results,
        "passed": passed,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(payload, indent=2) + "\n",
                                   encoding="utf-8")
        log("result JSON: %s" % args.json)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
