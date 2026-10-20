"""Host tests for the JSON / WebSocket wire contract (Wave 3 / WP-302).

Contract sources: docs/protocol.md §1/§3, docs/ws-binary-router.md,
docs/ota-local-upload.md. Covers only pure logic that runs without a device:

  * 4-byte binary frame header (data_type / module / sub_mod / reserved)
    encode-decode and the constants frozen in the contracts;
  * JSON request/response envelope rules (``{"module","cmd",...}``; the
    error envelope ``{"module","cmd","err","code"}``; error-code domain);
  * the pure helper functions of the production test tools
    ``scripts/e2e_ws_client.py`` and ``scripts/ota_upload.py`` (imported, not
    copied, so the tests fail if those helpers drift).
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import unittest
from types import SimpleNamespace

import script_loader as sl

e2e = sl.load_script("e2e_ws_client")
ota = sl.load_script("ota_upload")
PROTOCOL_DOC = sl.read_doc("protocol.md")
ROUTER_DOC = sl.read_doc("ws-binary-router.md")
ROOT = sl.ROOT

EXPECTED_MODULES = {
    "SYSTEM": 0,
    "WIFI": 1,
    "DATA_FLOW": 2,
    "RESERVED": 3,
    "UART": 4,
    "OTA": 5,
}
EXPECTED_DATA_TYPES = {
    "EVENT": 0x02,
    "RAW_BROADCAST": 0x04,
    "RAW": 0x20,
    "CMD": 0x21,
    "RESPONSE": 0x22,
}


# ---------------------------------------------------------------------------
# Contract parsers
# ---------------------------------------------------------------------------

def parse_module_ids(doc: str) -> dict[str, int]:
    m = re.search(
        r"SYSTEM=(\d+)、WIFI=(\d+)、DATA_FLOW=(\d+)、RESERVED=(\d+)、UART=(\d+)、OTA=(\d+)",
        doc,
    )
    if not m:
        raise AssertionError("module id table not found in docs/protocol.md")
    names = ("SYSTEM", "WIFI", "DATA_FLOW", "RESERVED", "UART", "OTA")
    return dict(zip(names, (int(g) for g in m.groups())))


def parse_data_types(doc: str) -> dict[str, int]:
    section = doc.split("## 3. 二进制帧协议", 1)[-1].split("## 4.", 1)[0]
    types: dict[str, int] = {}
    for name in EXPECTED_DATA_TYPES:
        m = re.search(rf"{name}=(0x[0-9A-Fa-f]{{2}})", section)
        if m:
            types[name] = int(m.group(1), 16)
    return types


def parse_error_codes(doc: str) -> dict[int, str]:
    section = doc.split("错误码（沿用", 1)[-1].split("## 2.", 1)[0]
    codes: dict[int, str] = {}
    for line in section.splitlines():
        m = re.match(r"^\|\s*(\d+)\s*\|\s*`([^`]+)`", line)
        if m:
            codes[int(m.group(1))] = m.group(2).strip()
    return codes


def parse_router_contract(doc: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for name in ("WT_WS_ROUTER_MAX_PAYLOAD", "WT_WS_MODULE_MAX"):
        m = re.search(rf"{name}\s+(\d+)", doc)
        if m:
            values[name] = int(m.group(1))
    return values


# ---------------------------------------------------------------------------
# Codec helpers mirroring docs/protocol.md §3 (4-byte header)
# ---------------------------------------------------------------------------

def pack_bin_header(data_type: int, module: int, sub_mod: int, reserved: int = 0) -> bytes:
    for name, value, limit in (("data_type", data_type, 0xFF),
                               ("module", module, 0xFF),
                               ("sub_mod", sub_mod, 0xFF),
                               ("reserved", reserved, 0xFF)):
        if not isinstance(value, int) or isinstance(value, bool):
            raise TypeError(f"{name} must be int")
        if not 0 <= value <= limit:
            raise ValueError(f"{name} out of range: {value}")
    return bytes([data_type, module, sub_mod, reserved])


def unpack_bin_header(frame: bytes) -> tuple[int, int, int, int, bytes]:
    if len(frame) < e2e.BIN_HDR_LEN:
        raise ValueError(f"binary frame shorter than {e2e.BIN_HDR_LEN} bytes: {len(frame)}")
    data_type, module, sub_mod, reserved = frame[0], frame[1], frame[2], frame[3]
    return data_type, module, sub_mod, reserved, frame[e2e.BIN_HDR_LEN:]


def make_request(module: int, cmd: int, **fields):
    req = {"module": module, "cmd": cmd}
    req.update(fields)
    return req


def validate_request(obj) -> dict:
    if not isinstance(obj, dict):
        raise ValueError("request must be a JSON object")
    for key in ("module", "cmd"):
        if key not in obj:
            raise ValueError(f"missing '{key}'")
        value = obj[key]
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"'{key}' must be an integer")
    if not 0 <= obj["module"] <= 0xFF:
        raise ValueError("module out of u8 range")
    if not 0 <= obj["cmd"] <= 0xFFFF:
        raise ValueError("cmd out of u16 range")
    return obj


def validate_success_echo(resp, module: int, cmd: int) -> dict:
    if not isinstance(resp, dict):
        raise ValueError("response must be a JSON object")
    if resp.get("module") != module or resp.get("cmd") != cmd:
        raise ValueError("success response must echo module/cmd")
    return resp


def validate_error_envelope(resp, codes) -> dict:
    if not isinstance(resp, dict):
        raise ValueError("error response must be a JSON object")
    for key in ("module", "cmd", "err", "code"):
        if key not in resp:
            raise ValueError(f"error response missing '{key}'")
    if not isinstance(resp["err"], str) or not resp["err"]:
        raise ValueError("'err' must be a non-empty string")
    if isinstance(resp["code"], bool) or not isinstance(resp["code"], int):
        raise ValueError("'code' must be an integer")
    if resp["code"] not in codes:
        raise ValueError(f"unknown error code: {resp['code']}")
    return resp


# ---------------------------------------------------------------------------
# Contract constants
# ---------------------------------------------------------------------------

class TestProtocolConstants(unittest.TestCase):
    def test_module_ids_match_contract(self) -> None:
        self.assertEqual(parse_module_ids(PROTOCOL_DOC), EXPECTED_MODULES)
        self.assertEqual(e2e.MODULE_SYSTEM, EXPECTED_MODULES["SYSTEM"])
        self.assertEqual(e2e.MODULE_WIFI, EXPECTED_MODULES["WIFI"])
        self.assertEqual(e2e.MODULE_DATA_FLOW, EXPECTED_MODULES["DATA_FLOW"])
        self.assertEqual(e2e.MODULE_UART, EXPECTED_MODULES["UART"])
        self.assertEqual(e2e.MODULE_OTA, EXPECTED_MODULES["OTA"])

    def test_binary_data_types_match_contract(self) -> None:
        self.assertEqual(parse_data_types(PROTOCOL_DOC), EXPECTED_DATA_TYPES)
        self.assertEqual(e2e.DATA_TYPE_RAW, EXPECTED_DATA_TYPES["RAW"])
        self.assertEqual(e2e.DATA_TYPE_RAW_BROADCAST, EXPECTED_DATA_TYPES["RAW_BROADCAST"])
        self.assertEqual(e2e.BIN_HDR_LEN, 4)
        self.assertEqual(e2e.UART_SUB_MOD, 0)

    def test_websocket_constants(self) -> None:
        self.assertEqual(e2e.WS_GUID, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
        self.assertEqual(e2e.WS_VERSION, 13)
        self.assertEqual(e2e.OP_CONT, 0x0)
        self.assertEqual(e2e.OP_TEXT, 0x1)
        self.assertEqual(e2e.OP_BINARY, 0x2)
        self.assertEqual(e2e.OP_CLOSE, 0x8)
        self.assertEqual(e2e.OP_PING, 0x9)
        self.assertEqual(e2e.OP_PONG, 0xA)

    def test_router_contract_matches_implementation_header(self) -> None:
        expected = {"WT_WS_ROUTER_MAX_PAYLOAD": 1792, "WT_WS_MODULE_MAX": 6}
        self.assertEqual(parse_router_contract(ROUTER_DOC), expected)
        header = ROOT / "components/ws_router/include/wt_ws_router.h"
        if not header.is_file():
            self.skipTest("components/ws_router/include/wt_ws_router.h not present")
        text = header.read_text(encoding="utf-8")
        for name, value in expected.items():
            m = re.search(rf"#define\s+{name}\s+(\d+)", text)
            self.assertIsNotNone(m, f"{name} missing from wt_ws_router.h")
            self.assertEqual(int(m.group(1)), value, f"{name} drifted from the contract")


# ---------------------------------------------------------------------------
# 4-byte binary frame header
# ---------------------------------------------------------------------------

class TestBinaryHeaderCodec(unittest.TestCase):
    def test_layout_is_four_bytes_in_order(self) -> None:
        frame = pack_bin_header(e2e.DATA_TYPE_RAW, e2e.MODULE_UART, e2e.UART_SUB_MOD, 0)
        self.assertEqual(len(frame), 4)
        self.assertEqual(frame, bytes([0x20, 4, 0, 0]))
        data_type, module, sub_mod, reserved, payload = unpack_bin_header(frame)
        self.assertEqual((data_type, module, sub_mod, reserved), (0x20, 4, 0, 0))
        self.assertEqual(payload, b"")

    def test_roundtrip_boundary_values(self) -> None:
        for value in (0, 1, 0x20, 0x7F, 0x80, 0xFF):
            with self.subTest(value=hex(value)):
                frame = pack_bin_header(value, value, value, 0)
                self.assertEqual(unpack_bin_header(frame)[:3], (value, value, value))

    def test_payload_is_split_after_header(self) -> None:
        payload = b"Hello UART"
        frame = pack_bin_header(e2e.DATA_TYPE_RAW, e2e.MODULE_UART, 2) + payload
        data_type, module, sub_mod, reserved, body = unpack_bin_header(frame)
        self.assertEqual(reserved, 0)
        self.assertEqual(body, payload)
        self.assertEqual((data_type, module, sub_mod), (0x20, 4, 2))

    def test_short_frame_rejected(self) -> None:
        for short in (b"", b"\x20", b"\x20\x04\x00"):
            with self.subTest(length=len(short)):
                with self.assertRaises(ValueError):
                    unpack_bin_header(short)

    def test_out_of_range_values_rejected(self) -> None:
        for bad in (-1, 256):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    pack_bin_header(bad, 0, 0)
        with self.assertRaises(TypeError):
            pack_bin_header("0x20", 0, 0)

    def test_e2e_uart_scenario_header_matches_contract(self) -> None:
        # scripts/e2e_ws_client.py builds the UART frame as
        # bytes([DATA_TYPE_RAW, MODULE_UART, UART_SUB_MOD, 0]) + payload.
        header = bytes([e2e.DATA_TYPE_RAW, e2e.MODULE_UART, e2e.UART_SUB_MOD, 0])
        self.assertEqual(header, pack_bin_header(0x20, 4, 0))
        data_type, module, sub_mod, reserved, body = unpack_bin_header(header + b"xyz")
        self.assertEqual((data_type, module, sub_mod, reserved, body), (0x20, 4, 0, 0, b"xyz"))


# ---------------------------------------------------------------------------
# JSON envelope
# ---------------------------------------------------------------------------

class TestJsonEnvelope(unittest.TestCase):
    def setUp(self) -> None:
        self.codes = parse_error_codes(PROTOCOL_DOC)
        self.assertEqual(sorted(self.codes), [2, 3, 4, 5, 6, 7])

    def test_doc_describes_request_and_error_envelopes(self) -> None:
        section = PROTOCOL_DOC.split("### 1.1", 1)[-1].split("## 2.", 1)[0]
        for token in ('"module":<u8>', '"cmd":<u16>', '"err"', '"code"'):
            self.assertIn(token, section)

    def test_request_bounds(self) -> None:
        validate_request(make_request(0, 0))
        validate_request(make_request(0xFF, 0xFFFF))
        validate_request(make_request(5, 1, extra="field"))
        for bad in ({"module": 0}, {"cmd": 1}, {"module": "0", "cmd": 1},
                    {"module": 0, "cmd": 1.5}, {"module": -1, "cmd": 1},
                    {"module": 256, "cmd": 1}, {"module": 0, "cmd": 65536},
                    {"module": True, "cmd": 1}, ["not", "an", "object"]):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    validate_request(bad)

    def test_success_echo_requires_matching_module_and_cmd(self) -> None:
        validate_success_echo({"module": 1, "cmd": 11, "auto_save": True}, 1, 11)
        with self.assertRaises(ValueError):
            validate_success_echo({"module": 1, "cmd": 12}, 1, 11)
        with self.assertRaises(ValueError):
            validate_success_echo({"cmd": 11}, 1, 11)

    def test_error_envelope_accepts_every_documented_code(self) -> None:
        for code, meaning in self.codes.items():
            with self.subTest(code=code):
                envelope = {"module": 1, "cmd": 12, "err": meaning, "code": code}
                self.assertIs(validate_error_envelope(envelope, self.codes), envelope)
        for bad in ({"module": 1, "cmd": 1, "err": "x", "code": 0},
                    {"module": 1, "cmd": 1, "err": "x", "code": 8},
                    {"module": 1, "cmd": 1, "err": "", "code": 5},
                    {"module": 1, "cmd": 1, "err": "x"},
                    {"module": 1, "cmd": 1, "code": 5}):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    validate_error_envelope(bad, self.codes)

    def test_protocol_error_code_table(self) -> None:
        self.assertEqual(self.codes[2], "Bad json request")
        self.assertEqual(self.codes[3], "Internal error")
        self.assertEqual(self.codes[4], "Unsupported cmd")
        self.assertEqual(self.codes[5], "Property error")
        self.assertEqual(self.codes[6], "Busy")
        self.assertEqual(self.codes[7], "Unauthorized")


# ---------------------------------------------------------------------------
# Pure helpers of the production scripts
# ---------------------------------------------------------------------------

class TestE2eHelpers(unittest.TestCase):
    def test_fmt_json_is_stable_and_utf8(self) -> None:
        self.assertEqual(e2e.fmt_json({"b": 1, "a": [2, 3]}), '{"a": [2, 3], "b": 1}')
        self.assertEqual(e2e.fmt_json({"ssid": "无线DAP"}), '{"ssid": "无线DAP"}')
        self.assertIsInstance(e2e.fmt_json(object()), str)

    def test_hex_dump(self) -> None:
        self.assertEqual(e2e.hex_dump(b""), "<empty>")
        self.assertEqual(e2e.hex_dump(b"He"), "48 65")
        dumped = e2e.hex_dump(bytes(range(70)), limit=4)
        self.assertTrue(dumped.startswith("00 01 02 03"))
        self.assertIn("(70 bytes)", dumped)

    def test_parse_http_header(self) -> None:
        header = ("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\n"
                  "Sec-WebSocket-Accept: abc123\r\n")
        status_line, headers = e2e.WsClient._parse_http_header(header)
        self.assertEqual(status_line, "HTTP/1.1 101 Switching Protocols")
        self.assertEqual(headers["upgrade"], "websocket")
        self.assertEqual(headers["sec-websocket-accept"], "abc123")

    def test_parse_host_arg(self) -> None:
        cases = [
            (("192.168.1.9", None, None), ("192.168.1.9", 80, "/ws")),
            (("192.168.1.9:8080", None, None), ("192.168.1.9", 8080, "/ws")),
            (("ws://dap.local:81/ws", None, None), ("dap.local", 81, "/ws")),
            (("http://dap.local/ws", None, None), ("dap.local", 80, "/ws")),
            (("dap.local", 9000, "/ws"), ("dap.local", 9000, "/ws")),
            (("dap.local", None, "/custom"), ("dap.local", 80, "/custom")),
            (("[::1]", None, None), ("::1", 80, "/ws")),
        ]
        for args, expected in cases:
            with self.subTest(host=args[0]):
                self.assertEqual(e2e.parse_host_arg(*args), expected)

    def test_parse_uart_payload(self) -> None:
        self.assertEqual(
            e2e._parse_uart_payload(SimpleNamespace(hex=False, payload="Hello")), b"Hello")
        for text in ("48656c6c6f", "48 65 6c 6c 6f", "0x48 0x65 0x6c 0x6c 0x6f"):
            with self.subTest(hex_payload=text):
                self.assertEqual(
                    e2e._parse_uart_payload(SimpleNamespace(hex=True, payload=text)),
                    b"Hello")
        for text in ("abc", "zz"):
            with self.subTest(bad=text):
                with self.assertRaises(e2e.WsAssertError):
                    e2e._parse_uart_payload(SimpleNamespace(hex=True, payload=text))


class TestOtaHelpers(unittest.TestCase):
    def test_parse_host_port(self) -> None:
        cases = [
            ("192.168.1.1", ("192.168.1.1", 80)),
            ("host:8080", ("host", 8080)),
            ("http://host:81", ("host", 81)),
            ("ws://host:9000/ws", ("host", 9000)),
        ]
        for value, expected in cases:
            with self.subTest(value=value):
                self.assertEqual(ota.parse_host_port(value), expected)
        for bad in ("", "http://", "host:notaport"):
            with self.subTest(bad=bad):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    with self.assertRaises(SystemExit) as ctx:
                        ota.parse_host_port(bad)
                self.assertEqual(ctx.exception.code, ota.EXIT_USAGE)
                self.assertIn("error", stderr.getvalue())

    def test_pretty_json_roundtrip(self) -> None:
        obj = {"module": 5, "cmd": 1, "status": "IDLE"}
        self.assertEqual(json.loads(ota.pretty_json(obj)), obj)


if __name__ == "__main__":
    unittest.main()
