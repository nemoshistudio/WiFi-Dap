"""Host tests for the frozen partition tables (Wave 3 / WP-302).

Exercises the parse/assert helpers of ``scripts/check_partitions.py`` against
the frozen contract ``docs/ota-local-upload.md`` §1 and both CSVs:

  * partitions/partitions_8mb_ota.csv   -- S3 8MB dual-OTA (frozen offsets)
  * partitions/partitions_4mb_single.csv -- C3/ESP32 4MB single app (no OTA)

No ESP-IDF, Docker, network or built firmware image is required: the
``gen_esp32part.py`` step is intentionally out of scope (reported as a gap in
docs/host-test-report.md); structural invariants are asserted in Python.
"""

from __future__ import annotations

import contextlib
import io
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import script_loader as sl

cp = sl.load_script("check_partitions")
OTA_DOC = sl.read_doc("ota-local-upload.md")
ROOT = sl.ROOT

# Hard-coded copy of the frozen contract (docs/ota-local-upload.md §1.1).
# Kept independent from check_partitions.LAYOUT_* so that a silent change in
# either the script or the CSV fails this test loudly.
EXPECTED_8MB = [
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("phy_init", "data", "phy", 0xF000, 0x1000),
    ("otadata", "data", "ota", 0x10000, 0x2000),
    ("ota_0", "app", "ota_0", 0x20000, 0x300000),
    ("ota_1", "app", "ota_1", 0x320000, 0x300000),
    ("wt_nvs", "data", "nvs", 0x620000, 0x10000),
    ("coredump", "data", "coredump", 0x630000, 0x10000),
]
EXPECTED_4MB = [
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("phy_init", "data", "phy", 0xF000, 0x1000),
    ("factory", "app", "factory", 0x20000, 0x3C0000),
    ("wt_nvs", "data", "nvs", 0x3E0000, 0x10000),
    ("coredump", "data", "coredump", 0x3F0000, 0x10000),
]
PTABLE_OFFSET = 0x8000
SECTOR = 0x1000
FLASH_8MB = 0x800000
FLASH_4MB = 0x400000
APP_MARGIN = 128 * 1024


@contextlib.contextmanager
def quiet():
    """check_layout/check_sdkconfig_wiring print status lines; silence them."""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        yield buf


def parse_doc_8mb_layout() -> dict[str, tuple[str, str, int, int]]:
    """Parse the markdown layout table of docs/ota-local-upload.md §1.1."""
    row_re = re.compile(
        r"^\|\s*`([a-z0-9_]+)`\s*\|\s*([a-z]+)\s*\|\s*([a-z_0-9]+)\s*\|\s*"
        r"`0x([0-9A-Fa-f]+)`\s*\|\s*`0x([0-9A-Fa-f]+)`",
        re.MULTILINE,
    )
    rows: dict[str, tuple[str, str, int, int]] = {}
    for match in row_re.finditer(OTA_DOC):
        name, ptype, subtype, offset, size = match.groups()
        rows[name] = (ptype, subtype, int(offset, 16), int(size, 16))
    return rows


def write_image(path: Path, version: bytes, magic: int = 0xE9,
                desc_magic: int = 0xABCD5432) -> None:
    """Create a synthetic ESP-IDF app image good enough for check_partitions."""
    data = bytearray(0x200)
    data[0] = magic
    data[0x20:0x24] = desc_magic.to_bytes(4, "little")
    data[0x30:0x30 + len(version)] = version
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(data))


def write_csv(path: Path, rows: list[tuple]) -> None:
    lines = ["# synthetic partition table for host tests"]
    for name, ptype, subtype, offset, size in rows:
        lines.append(f"{name}, {ptype}, {subtype}, 0x{offset:x}, 0x{size:x},")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class TestFrozenLayouts(unittest.TestCase):
    def setUp(self) -> None:
        cp.ERRORS.clear()
        cp.WARNINGS.clear()
        self.tmp = tempfile.TemporaryDirectory(prefix="birdiebot-ptable-")
        self.addCleanup(self.tmp.cleanup)
        self.tmpdir = Path(self.tmp.name)

    # -- contract <-> script constants ----------------------------------
    def test_expected_layouts_match_script_constants(self) -> None:
        self.assertEqual(cp.LAYOUT_8MB, EXPECTED_8MB)
        self.assertEqual(cp.LAYOUT_4MB, EXPECTED_4MB)
        self.assertEqual(cp.FLASH_8MB, FLASH_8MB)
        self.assertEqual(cp.FLASH_4MB, FLASH_4MB)
        self.assertEqual(cp.PTABLE_OFFSET, PTABLE_OFFSET)
        self.assertEqual(cp.APP_MARGIN, APP_MARGIN)
        self.assertEqual(cp.TABLES["partitions/partitions_8mb_ota.csv"],
                         (EXPECTED_8MB, FLASH_8MB))
        self.assertEqual(cp.TABLES["partitions/partitions_4mb_single.csv"],
                         (EXPECTED_4MB, FLASH_4MB))
        self.assertEqual(cp.SDKCONFIG_EXPECT["sdkconfig.defaults.esp32s3"],
                         ("partitions/partitions_8mb_ota.csv", "8MB"))
        self.assertEqual(cp.SDKCONFIG_EXPECT["sdkconfig.defaults.esp32c3"],
                         ("partitions/partitions_4mb_single.csv", "4MB"))
        self.assertEqual(cp.SDKCONFIG_EXPECT["sdkconfig.defaults.esp32"],
                         ("partitions/partitions_4mb_single.csv", "4MB"))

    def test_doc_table_matches_expected_8mb_layout(self) -> None:
        rows = parse_doc_8mb_layout()
        self.assertEqual(len(rows), len(EXPECTED_8MB),
                         "docs/ota-local-upload.md §1.1 must list all 8MB partitions")
        for name, ptype, subtype, offset, size in EXPECTED_8MB:
            self.assertIn(name, rows, f"{name} missing from the contract doc table")
            self.assertEqual(rows[name], (ptype, subtype, offset, size),
                             f"{name} in the contract doc differs from the frozen layout")

    # -- CSV parsing -----------------------------------------------------
    def test_parse_8mb_csv(self) -> None:
        parts = cp.parse_csv(ROOT / "partitions/partitions_8mb_ota.csv")
        self.assertEqual([(p.name, p.type, p.subtype, p.offset, p.size) for p in parts],
                         EXPECTED_8MB)

    def test_parse_4mb_csv(self) -> None:
        parts = cp.parse_csv(ROOT / "partitions/partitions_4mb_single.csv")
        self.assertEqual([(p.name, p.type, p.subtype, p.offset, p.size) for p in parts],
                         EXPECTED_4MB)

    def test_parse_csv_ignores_comments_and_blank_lines(self) -> None:
        csv = self.tmpdir / "doc.csv"
        csv.write_text(
            "# comment\n\nnvs, data, nvs, 0x9000, 0x6000,\n"
            "  # indented comment\nfactory, app, factory, 0x20000, 0x100000,\n",
            encoding="utf-8",
        )
        parts = cp.parse_csv(csv)
        self.assertEqual([p.name for p in parts], ["nvs", "factory"])

    def test_parse_csv_rejects_missing_size(self) -> None:
        csv = self.tmpdir / "bad.csv"
        csv.write_text("nvs, data, nvs, 0x9000,\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            cp.parse_csv(csv)

    # -- frozen layout / structural invariants ---------------------------
    def test_check_layout_8mb_passes(self) -> None:
        with quiet():
            cp.check_layout("partitions/partitions_8mb_ota.csv", EXPECTED_8MB, FLASH_8MB)
        self.assertEqual(cp.ERRORS, [])

    def test_check_layout_4mb_passes(self) -> None:
        with quiet():
            cp.check_layout("partitions/partitions_4mb_single.csv", EXPECTED_4MB, FLASH_4MB)
        self.assertEqual(cp.ERRORS, [])

    def test_sdkconfig_wiring_passes(self) -> None:
        with quiet():
            cp.check_sdkconfig_wiring()
        self.assertEqual(cp.ERRORS, [])
        for target in ("sdkconfig.defaults.esp32", "sdkconfig.defaults.esp32c3",
                       "sdkconfig.defaults.esp32s3"):
            self.assertTrue((ROOT / target).is_file(), f"missing {target}")

    def test_structural_invariants(self) -> None:
        for csv_name, expected, flash in (
                ("partitions/partitions_8mb_ota.csv", EXPECTED_8MB, FLASH_8MB),
                ("partitions/partitions_4mb_single.csv", EXPECTED_4MB, FLASH_4MB)):
            parts = cp.parse_csv(ROOT / csv_name)
            prev_end = PTABLE_OFFSET + SECTOR
            for name, ptype, _subtype, offset, size in expected:
                self.assertEqual(offset % SECTOR, 0, f"{name} not 4KB aligned")
                if ptype == "app":
                    self.assertEqual(offset % 0x10000, 0, f"{name} app not 64KB aligned")
                self.assertGreaterEqual(offset, prev_end, f"{name} overlaps previous partition")
                self.assertLessEqual(offset + size, flash, f"{name} exceeds flash size")
                prev_end = offset + size
            self.assertEqual(parts[0].offset, PTABLE_OFFSET + SECTOR,
                             "first partition must follow the 0x8000 partition table")

    def test_8mb_ota_requirements(self) -> None:
        parts = {p.name: p for p in cp.parse_csv(ROOT / "partitions/partitions_8mb_ota.csv")}
        self.assertIn("otadata", parts, "8MB table must have otadata for esp_ota")
        self.assertGreaterEqual(parts["otadata"].size, 2 * SECTOR,
                                "otadata must hold 2 OTA select sectors")
        self.assertEqual(parts["ota_0"].size, parts["ota_1"].size,
                         "ota_0/ota_1 must be equal size for rollback")
        self.assertEqual(parts["ota_0"].size, 0x300000, "slot size must be 3MB")
        self.assertEqual(parts["ota_0"].offset, 0x20000)
        self.assertEqual(parts["ota_1"].offset, 0x320000)
        self.assertEqual(parts["ota_0"].offset + parts["ota_0"].size,
                         parts["ota_1"].offset, "ota slots must be contiguous")

    def test_4mb_has_no_ota(self) -> None:
        parts = {p.name: p for p in cp.parse_csv(ROOT / "partitions/partitions_4mb_single.csv")}
        self.assertIn("factory", parts)
        self.assertNotIn("ota_0", parts)
        self.assertNotIn("ota_1", parts)
        self.assertNotIn("otadata", parts)
        self.assertEqual(len([p for p in parts.values() if p.type == "app"]), 1)

    def test_negative_overlap_is_detected(self) -> None:
        rows = [row for row in EXPECTED_8MB]
        rows[4] = ("ota_1", "app", "ota_1", 0x310000, 0x300000)  # overlaps ota_0
        csv = self.tmpdir / "overlap.csv"
        write_csv(csv, rows)
        with quiet():
            cp.check_layout(str(csv), rows, FLASH_8MB)
        self.assertTrue(any("overlap" in err.lower() for err in cp.ERRORS),
                        f"overlap not detected: {cp.ERRORS}")

    def test_negative_unaligned_app_is_detected(self) -> None:
        rows = [row for row in EXPECTED_8MB]
        rows[3] = ("ota_0", "app", "ota_0", 0x218000, 0x2F0000)
        rows[4] = ("ota_1", "app", "ota_1", 0x320000, 0x300000)
        csv = self.tmpdir / "unaligned.csv"
        write_csv(csv, rows)
        with quiet():
            cp.check_layout(str(csv), rows, FLASH_8MB)
        self.assertTrue(any("64kb aligned" in err.lower() for err in cp.ERRORS),
                        f"unaligned app partition not detected: {cp.ERRORS}")

    def test_negative_unequal_slots_are_detected(self) -> None:
        rows = [row for row in EXPECTED_8MB]
        rows[4] = ("ota_1", "app", "ota_1", 0x320000, 0x2F0000)
        csv = self.tmpdir / "unequal.csv"
        write_csv(csv, rows)
        with quiet():
            cp.check_layout(str(csv), rows, FLASH_8MB)
        self.assertTrue(any("must be equal" in err for err in cp.ERRORS),
                        f"unequal OTA slots not detected: {cp.ERRORS}")

    def test_negative_too_small_app_partition(self) -> None:
        rows = [
            ("nvs", "data", "nvs", 0x9000, 0x6000),
            ("phy_init", "data", "phy", 0xF000, 0x1000),
            ("otadata", "data", "ota", 0x10000, 0x2000),
            ("ota_0", "app", "ota_0", 0x20000, 0x10000),
            ("ota_1", "app", "ota_1", 0x30000, 0x10000),
        ]
        csv = self.tmpdir / "small.csv"
        write_csv(csv, rows)
        with quiet():
            cp.check_layout(str(csv), rows, FLASH_8MB)
        self.assertTrue(any("too small" in err for err in cp.ERRORS),
                        f"too-small app partition not detected: {cp.ERRORS}")

    def test_negative_missing_otadata(self) -> None:
        rows = [
            ("nvs", "data", "nvs", 0x9000, 0x6000),
            ("phy_init", "data", "phy", 0xF000, 0x1000),
            ("ota_0", "app", "ota_0", 0x20000, 0x300000),
            ("ota_1", "app", "ota_1", 0x320000, 0x300000),
        ]
        csv = self.tmpdir / "no-otadata.csv"
        write_csv(csv, rows)
        with quiet():
            cp.check_layout(str(csv), rows, FLASH_8MB)
        self.assertTrue(any("otadata" in err for err in cp.ERRORS),
                        f"missing otadata not detected: {cp.ERRORS}")

    # -- helpers ---------------------------------------------------------
    def test_align_up(self) -> None:
        self.assertEqual(cp.align_up(0, 0x1000), 0)
        self.assertEqual(cp.align_up(1, 0x1000), 0x1000)
        self.assertEqual(cp.align_up(0x1000, 0x1000), 0x1000)
        self.assertEqual(cp.align_up(0x1001, 0x1000), 0x2000)

    def test_app_cap_for_table(self) -> None:
        self.assertEqual(cp.app_cap_for_table("partitions/partitions_8mb_ota.csv"),
                         ("ota_0/ota_1", 0x300000))
        self.assertEqual(cp.app_cap_for_table("partitions/partitions_4mb_single.csv"),
                         ("factory", 0x3C0000))

    def test_read_app_desc_version_synthetic(self) -> None:
        good = self.tmpdir / "good.bin"
        write_image(good, b"v1.2.3")
        self.assertEqual(cp.read_app_desc_version(good), "v1.2.3")

        bad_magic = self.tmpdir / "bad-magic.bin"
        write_image(bad_magic, b"v1.2.3", magic=0x00)
        with self.assertRaises(ValueError):
            cp.read_app_desc_version(bad_magic)

        bad_desc = self.tmpdir / "bad-desc.bin"
        write_image(bad_desc, b"v1.2.3", desc_magic=0xDEADBEEF)
        with self.assertRaises(ValueError):
            cp.read_app_desc_version(bad_desc)

        short = self.tmpdir / "short.bin"
        short.write_bytes(b"\xE9" * 0x10)
        with self.assertRaises(ValueError):
            cp.read_app_desc_version(short)


class TestCliAppSizeCheck(unittest.TestCase):
    """End-to-end: the CLI can size-check a synthetic app image without IDF."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="birdiebot-cli-")
        self.addCleanup(self.tmp.cleanup)
        self.tmpdir = Path(self.tmp.name)

    def run_cli(self, bin_path: Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(ROOT / "scripts/check_partitions.py"),
             "--skip-layout", "--bin", str(bin_path)],
            cwd=ROOT, capture_output=True, text=True, timeout=60,
        )

    def test_valid_app_image_passes(self) -> None:
        image = self.tmpdir / "build" / "esp32s3" / "birdiebot_wifi_dap.bin"
        write_image(image, b"v1.0.0")
        proc = self.run_cli(image)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("esp_app_desc.version=v1.0.0", proc.stdout)

    def test_empty_version_fails(self) -> None:
        image = self.tmpdir / "build" / "esp32s3" / "birdiebot_wifi_dap.bin"
        write_image(image, b"")
        proc = self.run_cli(image)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("EMPTY", proc.stdout)

    def test_missing_image_fails_for_explicit_bin(self) -> None:
        image = self.tmpdir / "build" / "esp32s3" / "nope.bin"
        proc = self.run_cli(image)
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
