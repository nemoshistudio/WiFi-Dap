#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""check_board_pins.py -- verify components/board against docs/board-pins.md.

The board pin table in docs/board-pins.md is the frozen hardware contract
(WP-006). This script:

  1. parses the per-board pin tables (§4) and the board overview table (§3) of
     docs/board-pins.md,
  2. dumps the macros that birdiebot_board.h actually defines for every board
     (gcc -E -dM with the Kconfig board symbol),
  3. fails on any mismatch (pin number, LED capability flags, UART instance,
     board identity/target id, project-wide defaults),
  4. runs the host syntax checks from docs/board-pins.md §8.1 (default board,
     each board, and components/board/src/birdiebot_board.c).

Usage:
  python3 scripts/check_board_pins.py            # full check, exit 0/1
  python3 scripts/check_board_pins.py -v         # also print every checked pin

No ESP-IDF or Docker needed (gcc only); this script only reads files.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
BOARD_DIR = ROOT / "components/board"
INCLUDE_DIR = BOARD_DIR / "include"
DOC = ROOT / "docs/board-pins.md"

# Kconfig symbol suffix -> board id used in the doc headings / board files.
BOARDS = {
    "ZXR_DAP_S3": "BOARD_ZXR_DAP_S3",
    "GENERIC_S3": "BOARD_GENERIC_S3",
    "GENERIC_C3": "BOARD_GENERIC_C3",
    "GENERIC_ESP32": "BOARD_GENERIC_ESP32",
}

# Expected board identity values (docs/board-pins.md §3/§4).
IDENTITY = {
    "ZXR_DAP_S3": dict(id="zxr_dap_s3", target="esp32s3", target_id=3),
    "GENERIC_S3": dict(id="generic_s3", target="esp32s3", target_id=3),
    "GENERIC_C3": dict(id="generic_c3", target="esp32c3", target_id=2),
    "GENERIC_ESP32": dict(id="generic_esp32", target="esp32", target_id=1),
}

ERRORS: list[str] = []


def fail(msg: str) -> None:
    ERRORS.append(msg)
    print(f"  [FAIL] {msg}")


def ok(msg: str) -> None:
    print(f"  [ ok ] {msg}")


def run(cmd: list[str], input_text: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, input=input_text, text=True,
                          capture_output=True, cwd=ROOT)


def gcc_macros(board: str) -> dict[str, str]:
    """Dump all macros visible when the given board is selected."""
    snippet = '#include "birdiebot_board.h"\n'
    proc = run(
        ["gcc", "-E", "-dM", "-x", "c",
         f"-DCONFIG_BIRDIEBOT_BOARD_{board}",
         f"-I{INCLUDE_DIR}", "-"],
        snippet,
    )
    if proc.returncode != 0:
        fail(f"gcc macro dump failed for {board}: {proc.stderr.strip()[:500]}")
        return {}
    macros: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        m = re.match(r"#define (\w+) (.*)$", line)
        if m:
            macros[m.group(1)] = m.group(2).strip()
    return macros


def parse_doc_pins() -> dict[str, dict[str, str]]:
    """Return {board: {macro: expected_value}} from docs/board-pins.md §4."""
    text = DOC.read_text(encoding="utf-8")
    boards: dict[str, dict[str, str]] = {}
    current: str | None = None
    in_led_note = False
    for raw in text.splitlines():
        line = raw.strip()
        m = re.match(r"### 4\.\d+ `?BOARD_(\w+)`?", line)
        if m:
            suffix = m.group(1)
            current = suffix if suffix in BOARDS else None
            if current:
                boards.setdefault(current, {})
            in_led_note = False
            continue
        if line.startswith("## "):
            current = None
            continue
        if line.startswith("### "):
            # Any other level-3 section (e.g. §8) ends the last table.
            if current is not None and not line.startswith("### 4."):
                current = None
            continue
        if current is None or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 3 or set(cells[0]) <= {"-"}:
            continue
        macro_m = re.search(r"`(PIN_[A-Za-z0-9_]+|UART_BRIDGE_[A-Za-z0-9_]+|"
                            r"BIRDIEBOT_[A-Za-z0-9_]+)`", cells[1])
        if not macro_m:
            continue
        macro = macro_m.group(1)
        gpio_m = re.search(r"GPIO(\d+)", cells[2])
        if gpio_m:
            boards[current][macro] = gpio_m.group(1)
        uart_m = re.search(r"UART_NUM_(\d)", line)
        if uart_m and macro == "UART_BRIDGE_TX_PIN":
            boards[current]["__uart_num"] = uart_m.group(1)
        if macro in ("PIN_LED_CONNECTED", "PIN_LED_RUNNING") and "HAS_STATUS_LEDS=1" in line:
            boards[current]["__has_status_leds"] = "1"
            pins = re.findall(r"GPIO(\d+)", cells[2])
            if macro == "PIN_LED_CONNECTED" and pins:
                boards[current]["PIN_LED_CONNECTED"] = pins[0]
            if macro == "PIN_LED_RUNNING" and pins:
                boards[current]["PIN_LED_RUNNING"] = pins[-1]
        if macro == "PIN_LED_WIFI_STATUS" and "HAS_WIFI_LED=1" in line:
            boards[current]["__has_wifi_led"] = "1"
        if macro in ("PIN_LED_CONNECTED", "PIN_LED_RUNNING", "PIN_LED_WIFI_STATUS") \
                and "无：" in line:
            boards[current]["__no_leds"] = "1"
    return boards


def parse_doc_overview() -> dict[str, dict[str, str]]:
    """Return {board: {target, uart, has_status, has_wifi}} from §3."""
    text = DOC.read_text(encoding="utf-8")
    overview: dict[str, dict[str, str]] = {}
    for line in text.splitlines():
        if not line.strip().startswith("| `BOARD_"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        m = re.search(r"BOARD_(\w+)", cells[0])
        if not m or m.group(1) not in BOARDS:
            continue
        info = {"target": cells[1].replace("ESP32-", "esp32").lower()}
        uart_m = re.search(r"UART(\d)", cells[2])
        if uart_m:
            info["uart"] = uart_m.group(1)
        info["has_status"] = "1" if "✅" in cells[3] else "0"
        info["has_wifi"] = "1" if "✅" in cells[4] else "0"
        overview[m.group(1)] = info
    return overview


def check_board(board: str, doc_pins: dict[str, dict[str, str]],
                doc_overview: dict[str, dict[str, str]], verbose: bool) -> None:
    print(f"\n[{board}]")
    macros = gcc_macros(board)
    if not macros:
        return
    expected = doc_pins.get(board)
    if not expected:
        fail(f"docs/board-pins.md has no pin table for BOARD_{board}")
        return
    checked = 0
    for macro, want in sorted(expected.items()):
        if macro.startswith("__"):
            continue
        got = macros.get(macro)
        if got is None:
            fail(f"{board}: {macro} is not defined (doc says {want})")
        elif got != want:
            fail(f"{board}: {macro}={got} but docs/board-pins.md says {want}")
        else:
            checked += 1
            if verbose:
                print(f"  [ ok ] {macro} = {got}")
    ok(f"{checked} pin macros match docs/board-pins.md")
    # LED capability flags.
    for flag, want_key in (("BIRDIEBOT_BOARD_HAS_STATUS_LEDS", "__has_status_leds"),
                           ("BIRDIEBOT_BOARD_HAS_WIFI_LED", "__has_wifi_led")):
        want = expected.get(want_key, "0")
        got = macros.get(flag)
        if got != want:
            fail(f"{board}: {flag}={got} but docs/board-pins.md says {want}")
        elif verbose:
            print(f"  [ ok ] {flag} = {got}")
    ok("LED capability flags match the doc")
    # UART instance.
    want_uart = expected.get("__uart_num")
    got_uart = macros.get("BIRDIEBOT_UART_BRIDGE_UART_NUM")
    if want_uart is not None and got_uart != want_uart:
        fail(f"{board}: BIRDIEBOT_UART_BRIDGE_UART_NUM={got_uart} but docs say UART{want_uart}")
    else:
        ok(f"UART bridge instance = UART{got_uart}")
    # Identity / target consistency.
    ident = IDENTITY[board]
    for macro, want in (("BIRDIEBOT_BOARD_ID", f'"{ident["id"]}"'),
                        ("BIRDIEBOT_BOARD_TARGET", f'"{ident["target"]}"'),
                        ("BIRDIEBOT_BOARD_TARGET_ID", str(ident["target_id"]))):
        got = macros.get(macro)
        if got != want:
            fail(f"{board}: {macro}={got} expected {want}")
    ok("board identity/target macros match the contract")
    # §3 overview: target + UART + LED flags.
    ov = doc_overview.get(board)
    if ov:
        if ov["target"] != ident["target"]:
            fail(f"{board}: doc §3 target '{ov['target']}' != header target '{ident['target']}'")
        if "uart" in ov and ov["uart"] != got_uart:
            fail(f"{board}: doc §3 UART{ov['uart']} != header UART{got_uart}")
        if ov["has_status"] != expected.get("__has_status_leds", "0"):
            fail(f"{board}: doc §3 status-LED column disagrees with §4")
        if ov["has_wifi"] != expected.get("__has_wifi_led", "0"):
            fail(f"{board}: doc §3 WiFi-LED column disagrees with §4")
        ok("docs §3 overview agrees with docs §4 and the headers")


def check_project_defaults(board: str = "ZXR_DAP_S3") -> None:
    print("\n[project-wide defaults]")
    macros = gcc_macros(board)
    for macro, want in (("BIRDIEBOT_UART_BRIDGE_PORT", "1346"),
                        ("UART_BRIDGE_BAUDRATE", "115200"),
                        ("BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ", "1000000")):
        got = macros.get(macro)
        if got != want:
            fail(f"{macro}={got} expected {want} (docs/board-pins.md §5)")
        else:
            print(f"  [ ok ] {macro} = {got}")


def check_syntax() -> None:
    print("\n[host syntax checks, docs/board-pins.md §8.1]")
    checks = [
        ("default board (no sdkconfig)",
         ["gcc", "-fsyntax-only", "-x", "c", f"-I{INCLUDE_DIR}", "-"],
         '#include "birdiebot_board.h"\nint main(void) { return BIRDIEBOT_BOARD_TARGET_ID; }\n'),
    ]
    for board in BOARDS:
        checks.append((
            f"BOARD_{board}",
            ["gcc", "-fsyntax-only", "-x", "c", f"-DCONFIG_BIRDIEBOT_BOARD_{board}",
             f"-I{INCLUDE_DIR}", "-"],
            '#include "birdiebot_board.h"\nint main(void) { return BIRDIEBOT_BOARD_HAS_WIFI_LED; }\n',
        ))
    for board in BOARDS:
        checks.append((
            f"components/board/src (BOARD_{board})",
            ["gcc", "-fsyntax-only", f"-DCONFIG_BIRDIEBOT_BOARD_{board}",
             f"-I{INCLUDE_DIR}", str(BOARD_DIR / "src/birdiebot_board.c")],
            None,
        ))
    for label, cmd, stdin in checks:
        proc = run(cmd, stdin)
        if proc.returncode == 0:
            print(f"  [ ok ] {label}")
        else:
            fail(f"syntax check {label} failed: {proc.stderr.strip()[:400]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="print every matched macro")
    parser.add_argument("--no-syntax", action="store_true",
                        help="skip the gcc -fsyntax-only checks")
    opts = parser.parse_args()

    print(f"check_board_pins.py: project root {ROOT}")
    if not DOC.is_file():
        fail(f"missing contract {DOC}")
    if not INCLUDE_DIR.is_dir():
        fail(f"missing component {BOARD_DIR}")
    if ERRORS:
        return 1

    doc_pins = parse_doc_pins()
    doc_overview = parse_doc_overview()
    for board in BOARDS:
        if board not in doc_pins:
            fail(f"docs/board-pins.md §4 has no table for {board}")
    if ERRORS:
        return 1
    for board in BOARDS:
        check_board(board, doc_pins, doc_overview, opts.verbose)
    check_project_defaults()
    if not opts.no_syntax:
        check_syntax()

    print()
    if ERRORS:
        print(f"FAILED: {len(ERRORS)} board contract error(s)")
        for err in ERRORS:
            print(f"  - {err}")
        return 1
    print("OK: components/board matches docs/board-pins.md (pins, LEDs, UART, defaults)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
