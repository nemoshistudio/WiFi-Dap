#!/usr/bin/env python3
"""Incremental OpenOCD flashing helper.

Only erases/programs the sectors whose bytes differ from the previous image
(kept in a per-target cache).  First run (no cache) does a normal full flash.
Intended for iterative development where the firmware changes a little between
builds; e.g. STM32H7 128 KiB sectors.

Usage (USBIP attach must already be done):
  python3 scripts/openocd_incremental_flash.py --bin app.bin \
      --target-cfg target/stm32h7x.cfg --adapter-speed 1900 \
      --openocd /path/to/openocd --scripts /path/to/openocd/scripts

Exit codes: 0 ok, 1 error, 2 usage.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

CHUNK_FF = b"\xff"


def log(msg: str) -> None:
    print(f"[inc-flash] {msg}", flush=True)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_image(path: Path) -> bytes:
    data = path.read_bytes()
    if not data:
        raise SystemExit("[inc-flash] empty image")
    return data


def sectors_for(image_size: int, sector: int, base: int):
    """Yield (index, offset, address, size) covering the image."""
    idx = 0
    off = 0
    while off < image_size:
        size = min(sector, image_size - off)
        yield idx, off, base + off, size
        idx += 1
        off += size


def changed_sectors(new: bytes, old: bytes | None, sector: int, base: int):
    out = []
    for idx, off, addr, size in sectors_for(len(new), sector, base):
        new_part = new[off:off + size]
        old_part = old[off:off + size] if old is not None else b""
        if len(old_part) < size:
            old_part = old_part + CHUNK_FF * (size - len(old_part))
        if new_part != old_part:
            out.append((idx, off, addr, size, new_part))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, type=Path, help="new firmware .bin")
    ap.add_argument("--base-addr", default="0x08000000", help="flash base (default 0x08000000)")
    ap.add_argument("--sector-size", type=lambda s: int(s, 0), default=0x20000,
                    help="flash sector size in bytes (H7=0x20000, default)")
    ap.add_argument("--cache-dir", default=Path("build/openocd-flash-cache"), type=Path)
    ap.add_argument("--openocd", default="/tmp/openocd-master-install/bin/openocd")
    ap.add_argument("--scripts", default="/tmp/openocd-master-install/share/openocd/scripts",
                    help="OPENOCD_SCRIPTS directory")
    ap.add_argument("--interface-cfg", default="interface/cmsis-dap.cfg")
    ap.add_argument("--target-cfg", default="target/stm32h7x.cfg")
    ap.add_argument("--adapter-speed", default="1900")
    ap.add_argument("--vid-pid", default="0xc251 0xf00a")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force-full", action="store_true")
    ap.add_argument("--verify", action="store_true", default=True)
    args = ap.parse_args()

    img = load_image(args.bin)
    base = int(args.base_addr, 0)
    new_sha = sha256_file(args.bin)

    cache = args.cache_dir / f"last-{args.target_cfg.replace('/', '_')}-{base:x}.bin"
    old = None
    if cache.exists() and not args.force_full:
        try:
            old = cache.read_bytes()[:len(img)]
        except OSError:
            old = None

    changed = changed_sectors(img, old, args.sector_size, base)
    total_sectors = len(list(sectors_for(len(img), args.sector_size, base)))
    if not changed:
        log(f"up to date: {args.bin} sha256={new_sha[:12]} size={len(img)} "
            f"(no changed sectors, {total_sectors} sectors); nothing to flash")
        return 0

    # If every sector changed (or no cache), a single full-image erase/write is
    # more efficient than per-sector erase+write of padded sectors.
    if old is None or len(changed) == total_sectors:
        log(f"full flash selected ({len(changed)}/{total_sectors} sectors changed)")

    log(f"image {args.bin} size={len(img)} sha256={new_sha[:12]}")
    log(f"changed sectors: {len(changed)}/{total_sectors} "
        f"(old cache: {'yes' if old is not None else 'no'})")
    for idx, off, addr, size, _ in changed:
        log(f"  sector {idx}: offset=0x{off:x} addr=0x{addr:08x} size={size}")
    if args.dry_run:
        log("dry-run: no OpenOCD command executed")
        return 0

    with tempfile.TemporaryDirectory(prefix="inc-flash-") as td:
        tdir = Path(td)
        cmds = []
        if old is None or len(changed) == total_sectors:
            # One erase/write of the whole image is cheaper than padded sectors.
            full = tdir / "full.bin"
            full.write_bytes(img)
            cmds.append(f"flash write_image erase {full} 0x{base:08x}")
        else:
            for i, (_idx, _off, addr, size, part) in enumerate(changed):
                f = tdir / f"sector-{i}.bin"
                f.write_bytes(part)
                cmds.append(f"flash write_image erase {f} 0x{addr:08x}")
        if args.verify:
            cmds.append(f"verify_image {args.bin} 0x{base:08x}")
        cmds += ["reset run", "shutdown"]
        tcl = tdir / "flash.tcl"
        tcl.write_text("\n".join(cmds) + "\n")

        env = os.environ.copy()
        env["OPENOCD_SCRIPTS"] = args.scripts
        cmd = [
            args.openocd,
            "-f", args.interface_cfg,
            "-c", f"cmsis_dap_vid_pid {args.vid_pid}",
            "-c", "transport select swd",
            "-f", args.target_cfg,
            "-c", f"adapter speed {args.adapter_speed}",
            "-c", "reset_config none",
            "-c", "cortex_m reset_config sysresetreq",
            "-c", "init", "-c", "reset halt",
            "-f", str(tcl),
        ]
        log("running OpenOCD ...")
        rc = subprocess.run(cmd, env=env).returncode
        if rc != 0:
            log(f"OpenOCD failed rc={rc}; cache NOT updated")
            return 1

    args.cache_dir.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(img)
    log(f"done; cache updated: {cache} sha256={new_sha[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
