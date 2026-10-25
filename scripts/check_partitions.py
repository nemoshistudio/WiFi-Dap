#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""check_partitions.py -- validate the frozen WiFi Dap partition tables
and the size of the built application images.

Contracts checked (docs/ota-local-upload.md):
  * partitions/partitions_8mb_ota.csv : ESP32-S3 8MB dual-OTA layout (frozen,
    offsets/sizes must not change without a Lead-approved contract change).
  * partitions/partitions_4mb_single.csv : 4MB single-app fallback for the
    ESP32-C3 / ESP32 compile-only targets (no OTA).
  * sdkconfig.defaults.<target> must point at the matching CSV, select the
    matching flash size and use the IDF default partition table offset 0x8000.
  * `gen_esp32part.py` (run inside the ESP-IDF docker image, or on the host
    when IDF_PATH is exported) must parse + verify each CSV.
  * app size cap: every built app image must leave >= 128 KiB free in its app
    partition (ota_0/ota_1 for S3, factory for C3/ESP32), and its embedded
    esp_app_desc.version must be non-empty.

Usage:
  python3 scripts/check_partitions.py                     # full check
  python3 scripts/check_partitions.py --require-bin       # fail if no image found
  python3 scripts/check_partitions.py --bin build/esp32s3/birdiebot_wifi_dap.bin
  python3 scripts/check_partitions.py --no-docker         # only if gen tool unavailable
  IDF_DOCKER_IMAGE=espressif/idf:v5.2.1 python3 scripts/check_partitions.py

Exit status: 0 = all checks pass, 1 = validation failure, 2 = usage error.
This script only reads files; it never builds, flashes or touches git.
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent
DEFAULT_IMAGE = os.environ.get("IDF_DOCKER_IMAGE", "espressif/idf:v5.2.1")
APP_MARGIN = 128 * 1024  # docs/ota-local-upload.md §1.2.4

PTABLE_OFFSET = 0x8000
SECTOR = 0x1000

# ---------------------------------------------------------------------------
# Frozen layouts. Keep in sync with docs/ota-local-upload.md §1.1 / §1.2; a
# change here requires a Lead-approved contract change (MAJOR version bump).
# ---------------------------------------------------------------------------
LAYOUT_8MB = [
    # name,       type,  subtype,    offset,     size
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("phy_init", "data", "phy", 0xF000, 0x1000),
    ("otadata", "data", "ota", 0x10000, 0x2000),
    ("ota_0", "app", "ota_0", 0x20000, 0x300000),
    ("ota_1", "app", "ota_1", 0x320000, 0x300000),
    ("wt_nvs", "data", "nvs", 0x620000, 0x10000),
    ("coredump", "data", "coredump", 0x630000, 0x10000),
]
FLASH_8MB = 0x800000

LAYOUT_4MB = [
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("phy_init", "data", "phy", 0xF000, 0x1000),
    ("factory", "app", "factory", 0x20000, 0x3C0000),
    ("wt_nvs", "data", "nvs", 0x3E0000, 0x10000),
    ("coredump", "data", "coredump", 0x3F0000, 0x10000),
]
FLASH_4MB = 0x400000

TABLES = {
    "partitions/partitions_8mb_ota.csv": (LAYOUT_8MB, FLASH_8MB),
    "partitions/partitions_4mb_single.csv": (LAYOUT_4MB, FLASH_4MB),
}

# sdkconfig.defaults.<target> expectations (WP-141/142 wiring).
SDKCONFIG_EXPECT = {
    "sdkconfig.defaults.esp32s3": ("partitions/partitions_8mb_ota.csv", "8MB"),
    "sdkconfig.defaults.esp32c3": ("partitions/partitions_4mb_single.csv", "4MB"),
    "sdkconfig.defaults.esp32": ("partitions/partitions_4mb_single.csv", "4MB"),
    # ESP32-C5: 8MB DevKit (N8R8) reuses the frozen dual-OTA S3 layout; no USB
    # OTG/NCM, Wi-Fi-only variant (compile-only until hardware validation).
    "sdkconfig.defaults.esp32c5": ("partitions/partitions_8mb_ota.csv", "8MB"),
}

# Build directory -> partition table used for the app-size check.
BUILD_TARGETS = {
    "build/esp32s3": "partitions/partitions_8mb_ota.csv",
    "build/s3": "partitions/partitions_8mb_ota.csv",
    "build-esp32s3": "partitions/partitions_8mb_ota.csv",
    "build-s3": "partitions/partitions_8mb_ota.csv",
    "build/esp32c3": "partitions/partitions_4mb_single.csv",
    "build/c3": "partitions/partitions_4mb_single.csv",
    "build-esp32c3": "partitions/partitions_4mb_single.csv",
    "build/esp32": "partitions/partitions_4mb_single.csv",
    "build-esp32": "partitions/partitions_4mb_single.csv",
    "build/esp32c5": "partitions/partitions_8mb_ota.csv",
    "build/c5": "partitions/partitions_8mb_ota.csv",
}

ERRORS: list[str] = []
WARNINGS: list[str] = []


def fail(msg: str) -> None:
    ERRORS.append(msg)
    print(f"  [FAIL] {msg}")


def warn(msg: str) -> None:
    WARNINGS.append(msg)
    print(f"  [warn] {msg}")


def ok(msg: str) -> None:
    print(f"  [ ok ] {msg}")


@dataclass
class Part:
    name: str
    type: str
    subtype: str
    offset: int
    size: int


def parse_csv(path: Path) -> list[Part]:
    """Parse an ESP-IDF partition CSV (the subset of the format we use)."""
    parts: list[Part] = []
    next_free = PTABLE_OFFSET + SECTOR  # first offset after the partition table
    text = path.read_text(encoding="utf-8")
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) < 5:
            raise ValueError(f"{path.name}:{lineno}: expected at least 5 fields: {raw!r}")
        name, ptype, subtype = fields[0], fields[1], fields[2]
        offset_s, size_s = fields[3], fields[4]
        if not offset_s:
            offset = align_up(next_free, 0x1000)
        else:
            offset = int(offset_s, 0)
        if not size_s:
            raise ValueError(f"{path.name}:{lineno}: empty size: {raw!r}")
        size = int(size_s, 0)
        parts.append(Part(name, ptype.lower(), subtype.lower(), offset, size))
        next_free = offset + size
    return parts


def align_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def check_layout(csv_rel: str, expected: list[tuple], flash_size: int) -> None:
    path = ROOT / csv_rel
    print(f"\n[{csv_rel}] layout (frozen offsets/sizes)")
    if not path.is_file():
        fail(f"missing partition CSV: {csv_rel}")
        return
    try:
        actual = parse_csv(path)
    except ValueError as exc:
        fail(f"cannot parse {csv_rel}: {exc}")
        return

    exp = [Part(*row) for row in expected]
    if len(actual) != len(exp):
        fail(f"{csv_rel}: expected {len(exp)} partitions, found {len(actual)}")
        return
    for got, want in zip(actual, exp):
        if (got.name, got.type, got.subtype, got.offset, got.size) != (
            want.name,
            want.type,
            want.subtype,
            want.offset,
            want.size,
        ):
            fail(
                f"{csv_rel}: partition {want.name} mismatch: got "
                f"{got.name},{got.type},{got.subtype},0x{got.offset:x},0x{got.size:x} "
                f"expected {want.name},{want.type},{want.subtype},0x{want.offset:x},0x{want.size:x}"
            )
    if ERRORS:
        return
    ok("frozen names/types/offsets/sizes match docs/ota-local-upload.md")

    # --- structural checks -------------------------------------------------
    for p in actual:
        if p.offset % 0x1000:
            fail(f"{p.name}: offset 0x{p.offset:x} is not 4KB (sector) aligned")
        if p.type == "app" and p.offset % 0x10000:
            fail(f"{p.name}: app partition offset 0x{p.offset:x} is not 64KB aligned")
        if p.offset + p.size > flash_size:
            fail(
                f"{p.name}: ends at 0x{p.offset + p.size:x}, beyond flash size "
                f"0x{flash_size:x}"
            )
    for prev, cur in zip(actual, actual[1:]):
        if prev.offset + prev.size > cur.offset:
            fail(f"{prev.name} overlaps {cur.name}")
        if cur.offset - (prev.offset + prev.size) > 0x10000:
            warn(
                f"gap between {prev.name} and {cur.name}: "
                f"0x{cur.offset - (prev.offset + prev.size):x}"
            )
    by_name = {p.name: p for p in actual}
    ota0, ota1 = by_name.get("ota_0"), by_name.get("ota_1")
    if ota0 and ota1:
        if ota0.size != ota1.size:
            fail(f"ota_0 size 0x{ota0.size:x} != ota_1 size 0x{ota1.size:x} (must be equal)")
        otadata = by_name.get("otadata")
        if otadata is None:
            fail("missing otadata data/ota partition (required by esp_ota)")
        elif otadata.size < 2 * SECTOR:
            fail(f"otadata size 0x{otadata.size:x} < 2 sectors (0x{2 * SECTOR:x})")
        else:
            ok(f"ota_0/ota_1 equal (0x{ota0.size:x} each), otadata 0x{otadata.size:x} >= 2 sectors")
        if ota0.offset + ota0.size != ota1.offset:
            warn("ota_0 does not end exactly at ota_1 (gap/overlap checked above)")
    # ptable itself must not overlap the first partition
    first = actual[0]
    if first.offset < PTABLE_OFFSET + SECTOR:
        fail(f"first partition {first.name} starts at 0x{first.offset:x}, overlapping the partition table")
    else:
        ok(f"first partition starts at 0x{first.offset:x} (after ptable @0x{PTABLE_OFFSET:x})")
    # image size margin: app partition must be >= 128K bigger than 0-byte app
    for p in actual:
        if p.type == "app" and p.size <= APP_MARGIN:
            fail(f"{p.name}: app partition 0x{p.size:x} too small for {APP_MARGIN:#x} margin")
    print(f"  [ ok ] flash usage: 0x{actual[-1].offset + actual[-1].size:x} / 0x{flash_size:x}")


def check_sdkconfig_wiring() -> None:
    print("\n[sdkconfig default wiring]")
    for rel, (csv, flash) in SDKCONFIG_EXPECT.items():
        path = ROOT / rel
        if not path.is_file():
            fail(f"missing {rel}")
            continue
        text = path.read_text(encoding="utf-8")
        checks = {
            "CONFIG_PARTITION_TABLE_CUSTOM=y": "CONFIG_PARTITION_TABLE_CUSTOM=y",
            f'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="{csv}"': "CUSTOM_FILENAME",
            f'CONFIG_PARTITION_TABLE_FILENAME="{csv}"': "FILENAME",
            "CONFIG_PARTITION_TABLE_OFFSET=0x8000": "PTABLE_OFFSET=0x8000",
            f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash}"': f"FLASHSIZE={flash}",
        }
        missing = [label for line, label in checks.items() if line not in text]
        if missing:
            fail(f"{rel}: missing/incorrect {', '.join(missing)}")
        else:
            ok(f"{rel}: {flash} flash + {csv} @0x8000")


def gen_esp32part_available_host() -> str | None:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        return None
    tool = Path(idf_path) / "components/partition_table/gen_esp32part.py"
    return str(tool) if tool.is_file() else None


def check_with_gen_esp32part(csvs: list[str], use_docker: bool) -> None:
    """Parse+verify every CSV with the IDF gen_esp32part.py tool."""
    print("\n[gen_esp32part.py validation]")
    host_tool = gen_esp32part_available_host()
    if host_tool:
        for csv in csvs:
            argv = [sys.executable, host_tool, str(ROOT / csv)]
            rc = subprocess.run(argv, cwd=ROOT, stdout=subprocess.DEVNULL,
                                stderr=subprocess.PIPE, text=True)
            if rc.returncode != 0:
                fail(f"gen_esp32part.py rejected {csv}: {rc.stderr.strip()[-500:]}")
            else:
                ok(f"gen_esp32part.py (host) accepted {csv}")
        return
    if not use_docker:
        fail("IDF_PATH is not set and --no-docker was given: cannot run gen_esp32part.py")
        return
    if shutil.which("docker") is None:
        fail("docker not found in PATH and IDF_PATH is not set: cannot run gen_esp32part.py")
        return
    # One container for both CSVs: the docker entrypoint has a few seconds of
    # startup cost, doubling it per file is wasteful.
    cmd = " && ".join(
        f'python3 "$IDF_PATH/components/partition_table/gen_esp32part.py" "{csv}" >/dev/null'
        for csv in csvs
    )
    argv = [
        "docker", "run", "--rm",
        "-v", f"{ROOT}:/project", "-w", "/project",
        DEFAULT_IMAGE, "bash", "-c", cmd,
    ]
    print(f"  (docker image {DEFAULT_IMAGE})")
    rc = subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if rc.returncode != 0:
        fail(f"gen_esp32part.py rejected a CSV: {rc.stderr.strip()[-800:]}")
    else:
        for csv in csvs:
            ok(f"gen_esp32part.py (docker) accepted {csv}")


def app_cap_for_table(csv_rel: str) -> tuple[str, int]:
    """Return (app partition label, smallest app partition size) for a table.

    8MB S3 tables have two app slots (ota_0/ota_1, equal size); a built image must
    fit in every app slot, so the cap uses the smallest one.
    """
    layout, _ = TABLES[csv_rel]
    apps = [row for row in layout if row[1] == "app"]
    assert apps, f"{csv_rel}: no app partition in the expected layout"
    label = "/".join(row[0] for row in apps)
    return label, min(row[4] for row in apps)


def read_app_desc_version(bin_path: Path) -> str:
    data = bin_path.read_bytes()
    if len(data) < 0x50:
        raise ValueError(f"{bin_path}: too small to contain an app descriptor")
    if data[0] != 0xE9:
        raise ValueError(f"{bin_path}: bad image magic 0x{data[0]:02x} (expected 0xE9)")
    magic = struct.unpack_from("<I", data, 0x20)[0]
    if magic != 0xABCD5432:
        raise ValueError(f"{bin_path}: bad esp_app_desc.magic_word 0x{magic:08x}")
    raw = struct.unpack_from("<32s", data, 0x30)[0]
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def discover_bins(explicit: list[str]) -> list[tuple[Path, str]]:
    found: list[tuple[Path, str]] = []
    for arg in explicit:
        path = Path(arg)
        if not path.is_absolute():
            path = ROOT / path
        csv = None
        for build_dir, table in BUILD_TARGETS.items():
            if path.parent == ROOT / build_dir:
                csv = table
                break
        if csv is None:
            # Allow --bin build/esp32s3/... or derive from the CSV next to it.
            csv = "partitions/partitions_8mb_ota.csv" if "s3" in str(path.parent) else None
        if csv is None:
            raise SystemExit(
                f"check_partitions.py: cannot infer the partition table for {arg}; "
                "pass it via a known build directory (build/esp32s3|build/esp32c3|build/esp32)"
            )
        found.append((path, csv))
    if found:
        return found
    for build_dir, table in BUILD_TARGETS.items():
        path = ROOT / build_dir / "birdiebot_wifi_dap.bin"
        if path.is_file():
            found.append((path, table))
    return found


def check_app_sizes(bins: list[tuple[Path, str]], require_bin: bool) -> None:
    print("\n[app image size + esp_app_desc.version]")
    if not bins:
        msg = "no built birdiebot_wifi_dap.bin found (build/esp32s3, build/esp32c3, build/esp32)"
        if require_bin:
            fail(msg)
        else:
            warn(msg + "; size check skipped (run scripts/build_all.sh first)")
        return
    seen_tables = set()
    for path, csv_rel in bins:
        if not path.is_file():
            fail(f"missing app image: {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path}")
            continue
        seen_tables.add(csv_rel)
        app_name, app_size = app_cap_for_table(csv_rel)
        cap = app_size - APP_MARGIN
        size = path.stat().st_size
        shown = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
        if size > cap:
            fail(
                f"{shown}: {size} bytes (0x{size:x}) exceeds cap 0x{cap:x} "
                f"({app_name} 0x{app_size:x} - {APP_MARGIN:#x} margin)"
            )
        else:
            ok(f"{shown}: {size} bytes, {cap - size} bytes free in {app_name} (cap 0x{cap:x})")
        try:
            version = read_app_desc_version(path)
        except ValueError as exc:
            fail(str(exc))
            continue
        if not version:
            fail(f"{shown}: esp_app_desc.version is EMPTY (PROJECT_VER not injected)")
        elif version.endswith("-dirty"):
            warn(f"{shown}: esp_app_desc.version={version} (-dirty, not release quality)")
        else:
            ok(f"{shown}: esp_app_desc.version={version}")
    for csv_rel in TABLES:
        if csv_rel not in seen_tables:
            warn(f"no built image checked against {csv_rel}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate WiFi Dap partition tables and app image sizes."
    )
    parser.add_argument("--bin", action="append", default=[],
                        help="app .bin to size-check (default: auto-discover build dirs)")
    parser.add_argument("--require-bin", action="store_true",
                        help="fail when no built app image is found (release gate)")
    parser.add_argument("--no-docker", action="store_true",
                        help="never invoke docker (requires IDF_PATH for gen_esp32part.py)")
    parser.add_argument("--skip-layout", action="store_true",
                        help="skip the frozen-layout/gen_esp32part checks (size check only)")
    opts = parser.parse_args(argv)

    print(f"check_partitions.py: project root {ROOT}")
    csvs = list(TABLES)
    if not opts.skip_layout:
        for csv_rel, (layout, flash) in TABLES.items():
            check_layout(csv_rel, layout, flash)
        check_sdkconfig_wiring()
        check_with_gen_esp32part(csvs, use_docker=not opts.no_docker)

    try:
        bins = discover_bins(opts.bin)
    except SystemExit as exc:
        print(f"  [FAIL] {exc}", file=sys.stderr)
        return 2
    check_app_sizes(bins, require_bin=opts.require_bin)

    print()
    if ERRORS:
        print(f"FAILED: {len(ERRORS)} error(s), {len(WARNINGS)} warning(s)")
        for err in ERRORS:
            print(f"  - {err}")
        return 1
    print(f"OK: partition tables valid, app images within limits "
          f"({len(WARNINGS)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
