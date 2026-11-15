"""Host tests for the OTA upload contract (Wave 3 / WP-302).

Contract source: tests/host/fixtures/ota-upload-contract.md (v1 frozen) + docs/protocol.md.
Covers the parts that can be tested without a device:

  * SHA-256 file digest (``scripts/ota_upload.py:sha256_file``) against known
    vectors, and the optional ``X-SHA256`` header validation (lower/upper case);
  * the HTTP status -> JSON ``code`` / ``err`` mapping table from the contract,
    parsed out of the markdown so the test fails when the table drifts;
  * the upload success/error JSON envelope schema;
  * the OTA state machine states/transitions documented in the contract;
  * the app-image sanity helper (``parse_app_image``) and frozen size constants.

Contract-consistency regression guard: login/auth was removed by product
decision; docs/protocol.md no longer advertises an Unauthorized code and the
OTA contract no longer maps HTTP 401. The table parser below keeps the tests
in sync with the (now auth-free) contract.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path

import script_loader as sl

ota = sl.load_script("ota_upload")
OTA_DOC = sl.read_fixture("ota-upload-contract.md")
PROTOCOL_DOC = sl.read_doc("protocol.md")

STATES = ("IDLE", "RECEIVING", "VERIFYING", "READY", "APPLYING", "ERROR")

# Frozen copy of tests/host/fixtures/ota-upload-contract.md §3.4 (kept independent of the parser).
EXPECTED_HTTP_MAP = {
    400: 5,
    403: 5,
    409: 6,
    411: 5,
    413: 5,
    415: 5,
    422: 5,
    408: 6,
    500: 3,
    503: 6,
}
EXPECTED_HTTP_ERR_HINTS = {
    400: ("invalid content length", "bad sha256 header", "image too small"),
    403: ("forbidden origin",),
    409: ("ota session busy",),
    411: ("content-length required",),
    413: ("image too large",),
    415: ("unsupported content type",),
    422: ("image verification failed", "sha256 mismatch"),
    408: ("upload timeout",),
    500: ("ota begin failed", "ota write failed", "set boot partition failed"),
    503: ("ota service unavailable",),
}
EXPECTED_SUCCESS_KEYS = {
    "module", "cmd", "ok", "slot", "running", "running_ver", "offset", "total",
    "sha256", "status", "reboot_in_ms",
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def extract_json_blocks(doc: str) -> list[str]:
    return [m.group(1) for m in re.finditer(r"```json\s*\n(.*?)\n```", doc, re.S)]


def parsed_json_blocks(doc: str) -> list[dict]:
    out: list[dict] = []
    for block in extract_json_blocks(doc):
        try:
            obj = json.loads(block)
        except ValueError:
            continue  # blocks with // comments (GET_INFO examples) are not strict JSON
        if isinstance(obj, dict):
            out.append(obj)
    return out


def parse_http_error_table(doc: str) -> dict[int, int]:
    """Extract (HTTP status -> JSON code) rows from the §3.4 markdown table."""
    table = doc.split("### 3.4", 1)[-1].split("## 4.", 1)[0]
    mapping: dict[int, int] = {}
    for line in table.splitlines():
        m = re.match(r"^\|\s*(\d{3})\s*\|.*?\|\s*(\d+)\s*\|", line)
        if not m:
            continue
        status = int(m.group(1))
        if status in (400, 403, 408, 409, 411, 413, 415, 422, 500, 503):
            mapping[status] = int(m.group(2))
    return mapping


def parse_protocol_error_codes(doc: str) -> dict[int, str]:
    section = doc.split("错误码（沿用", 1)[-1].split("## 2.", 1)[0]
    codes: dict[int, str] = {}
    for line in section.splitlines():
        m = re.match(r"^\|\s*(\d+)\s*\|\s*`([^`]+)`", line)
        if m:
            codes[int(m.group(1))] = m.group(2).strip()
    return codes


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_matches(header_value: str, device_digest: str) -> bool:
    """Mirror the device rule: X-SHA256 accepts upper/lower case, 64 hex chars."""
    if not ota.valid_sha256_hex(header_value):
        return False
    if len(device_digest) != 64:
        return False
    return header_value.lower() == device_digest.lower()


def make_app_image(version: bytes = b"v1.0.0", image_magic: int = 0xE9,
                   desc_magic: int = 0xABCD5432) -> bytes:
    data = bytearray(0x200)
    data[0] = image_magic
    data[0x20:0x24] = desc_magic.to_bytes(4, "little")
    data[0x30:0x30 + len(version)] = version
    return bytes(data)


# ---------------------------------------------------------------------------
# SHA-256 / X-SHA256
# ---------------------------------------------------------------------------

class TestSha256(unittest.TestCase):
    def write_tmp(self, data: bytes) -> str:
        tmp = tempfile.NamedTemporaryFile(prefix="birdiebot-ota-", delete=False)
        tmp.write(data)
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return tmp.name

    def test_known_vectors_match_hashlib(self) -> None:
        vectors = [
            b"",
            b"abc",
            b"The quick brown fox jumps over the lazy dog",
            bytes(range(256)) * 64,  # multi-chunk (>1 MiB) path in sha256_file
        ]
        for data in vectors:
            path = self.write_tmp(data)
            with self.subTest(length=len(data)):
                self.assertEqual(ota.sha256_file(path), sha256_hex(data))
                self.assertEqual(len(ota.sha256_file(path)), 64)

    def test_valid_sha256_hex(self) -> None:
        self.assertTrue(ota.valid_sha256_hex("a" * 64))
        self.assertTrue(ota.valid_sha256_hex("A" * 64))
        self.assertTrue(ota.valid_sha256_hex("0123456789abcdefABCDEF" + "0" * 42))
        for bad in ("", "a" * 63, "a" * 65, "g" * 64, "0x" + "a" * 62,
                    " " + "a" * 63, "a" * 64 + "\n"):
            with self.subTest(value=bad[:12]):
                self.assertFalse(ota.valid_sha256_hex(bad))

    def test_xsha256_header_is_case_insensitive(self) -> None:
        digest = sha256_hex(b"firmware-image")
        upper = digest.upper()
        mixed = "".join(c.upper() if i % 2 else c for i, c in enumerate(digest))
        self.assertTrue(sha256_matches(upper, digest))
        self.assertTrue(sha256_matches(mixed, digest))
        self.assertTrue(sha256_matches(digest, digest))
        self.assertFalse(sha256_matches("0" * 64, digest))
        self.assertFalse(sha256_matches(digest[:-1], digest))
        self.assertFalse(sha256_matches("zz" + digest[2:], digest))

    def test_sha256_file_matches_hashlib_for_temp_file(self) -> None:
        data = b"line one\nline two\n" * 1000
        self.assertEqual(ota.sha256_file(self.write_tmp(data)), sha256_hex(data))


# ---------------------------------------------------------------------------
# HTTP status -> JSON code mapping (contract §3.4)
# ---------------------------------------------------------------------------

class TestHttpErrorMapping(unittest.TestCase):
    def test_mapping_table_matches_frozen_contract(self) -> None:
        self.assertEqual(parse_http_error_table(OTA_DOC), EXPECTED_HTTP_MAP)

    def test_error_hints_present_in_contract(self) -> None:
        table = OTA_DOC.split("### 3.4", 1)[-1].split("## 4.", 1)[0]
        for status, hints in EXPECTED_HTTP_ERR_HINTS.items():
            with self.subTest(http=status):
                self.assertIn(f"| {status} |", table)
                for hint in hints:
                    self.assertIn(hint, table)

    def test_json_codes_are_in_the_protocol_domain(self) -> None:
        codes = parse_protocol_error_codes(PROTOCOL_DOC)
        self.assertEqual(sorted(codes), [2, 3, 4, 5, 6])
        self.assertEqual(codes[2], "Bad json request")
        self.assertEqual(codes[3], "Internal error")
        self.assertEqual(codes[4], "Unsupported cmd")
        self.assertEqual(codes[5], "Property error")
        self.assertEqual(codes[6], "Busy")
        for status, code in EXPECTED_HTTP_MAP.items():
            with self.subTest(http=status):
                self.assertIn(code, codes, f"HTTP {status} maps to unknown code {code}")

# ---------------------------------------------------------------------------
# Upload success/error JSON envelope (contract §3.4)
# ---------------------------------------------------------------------------

class TestUploadEnvelope(unittest.TestCase):
    def test_success_envelope_schema(self) -> None:
        blocks = [b for b in parsed_json_blocks(OTA_DOC) if b.get("ok") is True]
        self.assertEqual(len(blocks), 1, "one strict-JSON success example expected")
        body = blocks[0]
        self.assertTrue(EXPECTED_SUCCESS_KEYS.issubset(body.keys()),
                        f"missing keys: {EXPECTED_SUCCESS_KEYS - body.keys()}")
        self.assertEqual(body["module"], 5)
        self.assertEqual(body["cmd"], 2)
        self.assertEqual(body["status"], "APPLYING")
        self.assertEqual(body["reboot_in_ms"], 1000)
        self.assertEqual(body["total"], ota.MAX_SLOT_SIZE)
        self.assertEqual(body["offset"], ota.MAX_SLOT_SIZE)
        self.assertIn(body["status"], STATES)
        self.assertRegex(body["sha256"], r"^[0-9a-f]{64}$")

    def test_error_envelope_schema(self) -> None:
        blocks = [b for b in parsed_json_blocks(OTA_DOC) if b.get("ok") is False]
        self.assertEqual(len(blocks), 1, "one strict-JSON error example expected")
        body = blocks[0]
        for key in ("module", "cmd", "ok", "err", "code"):
            self.assertIn(key, body)
        self.assertEqual(body["module"], 5)
        self.assertEqual(body["cmd"], 2)
        self.assertFalse(body["ok"])
        self.assertIsInstance(body["err"], str)
        self.assertTrue(body["err"])
        self.assertIn(body["code"], parse_protocol_error_codes(PROTOCOL_DOC))


# ---------------------------------------------------------------------------
# State machine (contract §5)
# ---------------------------------------------------------------------------

class TestOtaStateMachine(unittest.TestCase):
    ALLOWED_OPS = {
        "IDLE": {"POST", "GET_INFO", "GET_PROGRESS"},
        "RECEIVING": {"GET_PROGRESS", "ABORT", "GET_INFO"},
        "VERIFYING": {"GET_PROGRESS", "GET_INFO"},
        "READY": {"GET_INFO", "GET_PROGRESS"},
        "APPLYING": {"GET_INFO", "GET_PROGRESS"},
        "ERROR": {"GET_INFO", "GET_PROGRESS", "POST"},
    }

    def state_rows(self) -> dict[str, str]:
        section = OTA_DOC.split("## 5. 状态机", 1)[-1].split("## 6.", 1)[0]
        rows: dict[str, str] = {}
        for line in section.splitlines():
            m = re.match(r"^\|\s*`(IDLE|RECEIVING|VERIFYING|READY|APPLYING|ERROR)`\s*\|(.*)\|\s*$",
                         line)
            if m:
                rows[m.group(1)] = m.group(2)
        return rows

    def test_all_states_documented(self) -> None:
        self.assertEqual(tuple(self.state_rows()), STATES)
        for state in STATES:
            self.assertIn(state, PROTOCOL_DOC,
                          f"{state} missing from protocol.md GET_PROGRESS status list")

    def test_allowed_operations_per_state(self) -> None:
        rows = self.state_rows()
        for state, expected in self.ALLOWED_OPS.items():
            with self.subTest(state=state):
                row = rows[state]
                for op in expected:
                    self.assertIn(op, row, f"{state} must allow {op}")
        self.assertIn("invalid state", rows["VERIFYING"],
                      "VERIFYING ABORT must be rejected with err='invalid state'")

    def test_flow_references_esp_ota_apis(self) -> None:
        section = OTA_DOC.split("### 6.1", 1)[-1].split("### 6.2", 1)[0]
        for api in ("esp_ota_begin", "esp_ota_write", "esp_ota_end",
                    "esp_ota_set_boot_partition", "esp_restart"):
            with self.subTest(api=api):
                self.assertIn(api, section)
        self.assertIn("esp_ota_abort", OTA_DOC.split("### 3.3", 1)[-1].split("### 3.4", 1)[0])


# ---------------------------------------------------------------------------
# Constants / app image helper
# ---------------------------------------------------------------------------

class TestOtaConstants(unittest.TestCase):
    def test_frozen_constants_match_doc(self) -> None:
        self.assertEqual(ota.UPLOAD_PATH, "/api/ota/upload")
        self.assertEqual(ota.MIN_IMAGE_SIZE, 4096)
        self.assertEqual(ota.MAX_SLOT_SIZE, 3 * 1024 * 1024)
        self.assertEqual(ota.MAX_SLOT_SIZE, 0x300000)
        self.assertEqual(ota.DEFAULT_TIMEOUT, 300.0)
        self.assertIn("/api/ota/upload", OTA_DOC)
        self.assertIn("4096", OTA_DOC)
        self.assertIn("0x300000", OTA_DOC)
        self.assertIn("300s", OTA_DOC)
        self.assertIn("10s", OTA_DOC)
        self.assertIn("1000ms", OTA_DOC)
        self.assertIn("application/octet-stream", OTA_DOC)
        self.assertIn("X-SHA256", OTA_DOC)
        self.assertIn("Content-Length", OTA_DOC)
        self.assertIn("411", OTA_DOC)
        self.assertIn("415", OTA_DOC)
        self.assertIn("409", OTA_DOC)

    def test_parse_app_image(self) -> None:
        tmp = tempfile.TemporaryDirectory(prefix="birdiebot-app-")
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)

        good = root / "good.bin"
        good.write_bytes(make_app_image(b"v1.2.3"))
        self.assertEqual(ota.parse_app_image(str(good)), "v1.2.3")

        empty_ver = root / "empty.bin"
        empty_ver.write_bytes(make_app_image(b""))
        self.assertEqual(ota.parse_app_image(str(empty_ver)), "")

        bad_magic = root / "bad-magic.bin"
        bad_magic.write_bytes(make_app_image(b"v1.2.3", image_magic=0x00))
        self.assertIsNone(ota.parse_app_image(str(bad_magic)))

        bad_desc = root / "bad-desc.bin"
        bad_desc.write_bytes(make_app_image(b"v1.2.3", desc_magic=0xDEADBEEF))
        self.assertIsNone(ota.parse_app_image(str(bad_desc)))

        short = root / "short.bin"
        short.write_bytes(b"\xE9" * 8)
        self.assertIsNone(ota.parse_app_image(str(short)))


if __name__ == "__main__":
    unittest.main()
