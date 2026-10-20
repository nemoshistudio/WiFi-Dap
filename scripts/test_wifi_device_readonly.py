#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_wifi_device_readonly.py -- read-only WiFi cmd 11/5/1/6 validation.

Connects to a running WiFi Dap device over WebSocket and validates the
multi-credential JSON API (docs/wifi-credential-memory.md §7, docs/protocol.md
§1.1/§2.2) **without changing any device state**:

  R1 SYSTEM(0)/GET_FM_INFO(1)   protocol_version == 1, fm_ver non-empty
  R2 SYSTEM(0)/GET_SYS_INFO(3)  sn is 12 upper-case hex chars (STA MAC)
  R3 WIFI(1)/GET_SAVED_LIST(11) auto_save/count/list shape, idx order,
                                SSID/last_success/priority ranges, no password,
                                optional max (device limit, must not be < count,
                                contract b015a0c) and connected_idx; cmd 11
                                unsupported (code 4 = old firmware) is reported
  R4 WIFI(1)/GET_AP_INFO(5)     effective AP SSID: either the dynamic default
                                "WiFi-Dap-XXXX" (XXXX == sn[-4:]) or a
                                custom 1..32 byte value (user value wins, §6)
  R5 WIFI(1)/GET_MODE(6)        mode/status present and in range
  R6 WIFI(1)/GET_STA_INFO(1)    ssid/rssi/mac keys present (informational)
  R7 cmd 12/13/14 error paths   deliberately invalid requests must be rejected
                                with code 5 and the protocol error shape
                                {module,cmd,err,code}; nothing is persisted

The script only sends GET commands plus invalid probes that the firmware rejects
before touching NVS. It never calls CONNECT/ADD_SAVED/DEL_SAVED/SET_AUTO_SAVE/
SET_AP_CRED with valid write payloads.

Dependency-free: pure Python 3 standard library WebSocket client (RFC 6455).
Output is TAP-ish (one "ok N - ..." line per check) and pasteable into the test
record; --json prints a machine-readable summary instead.

Usage
    python3 scripts/test_wifi_device_readonly.py --host 192.168.1.1
    python3 scripts/test_wifi_device_readonly.py --host 192.168.1.1 --json

Exit codes
    0  all checks passed
    2  connection / WebSocket handshake / command line error
    3  at least one check failed

See docs/hardware-test-wifi.md for the complete hardware checklist.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Protocol constants
# ---------------------------------------------------------------------------
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_VERSION = 13

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

MODULE_SYSTEM = 0
MODULE_WIFI = 1

SYS_GET_FM_INFO = 1
SYS_GET_SYS_INFO = 3

WIFI_GET_STA_INFO = 1
WIFI_GET_AP_INFO = 5
WIFI_GET_MODE = 6
WIFI_GET_SAVED_LIST = 11
WIFI_ADD_SAVED = 12
WIFI_DEL_SAVED = 13
WIFI_SET_AUTO_SAVE = 14

# docs/protocol.md §1.1
ERR_BAD_REQUEST = 2
ERR_INTERNAL = 3
ERR_UNSUPPORTED = 4
ERR_PROPERTY = 5
ERR_BUSY = 6

PERMANENT_MODES = {0, 4, 5, 6, 7}  # wifi_apsta_mode_e permanent values
WIFI_MODES = {0, 1, 2, 3}          # wifi_mode_t: NULL/STA/AP/APSTA

MAX_FRAME = 1 << 20
MAX_HTTP_HEADER = 64 * 1024

AP_DEFAULT_RE = re.compile(r"^WiFi-Dap-([0-9A-F]{4})$")
SN_RE = re.compile(r"^[0-9A-F]{12}$")


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------
class WsConnectError(Exception):
    """TCP connect / HTTP upgrade failed (exit code 2)."""


class WsClosed(Exception):
    """The device closed the WebSocket connection."""


class CheckError(Exception):
    """A validation assertion failed (exit code 3)."""


# ---------------------------------------------------------------------------
# Minimal RFC 6455 client (text frames only, no third-party packages)
# ---------------------------------------------------------------------------
class WsClient:
    def __init__(self, host: str, port: int, path: str, timeout: float):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self._rx = b""

    # -- low level ---------------------------------------------------------
    def _recv_exact(self, count: int, deadline: float) -> bytes:
        while len(self._rx) < count:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise WsConnectError("timeout while reading from device")
            self.sock.settimeout(remain)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise WsConnectError("timeout while reading from device") from exc
            except OSError as exc:
                raise WsConnectError(f"socket read failed: {exc}") from exc
            if not chunk:
                raise WsClosed("device closed the connection")
            self._rx += chunk
        data, self._rx = self._rx[:count], self._rx[count:]
        return data

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", length)
        mask = os.urandom(4)
        header += mask
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        try:
            self.sock.sendall(bytes(header) + masked)
        except OSError as exc:
            raise WsConnectError(f"socket write failed: {exc}") from exc

    def _recv_frame(self, deadline: float) -> tuple[int, bytes, bool]:
        first, second = self._recv_exact(2, deadline)
        fin = bool(first & 0x80)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._recv_exact(2, deadline))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._recv_exact(8, deadline))
        if length > MAX_FRAME:
            raise WsClosed(f"inbound frame too large: {length}")
        mask = self._recv_exact(4, deadline) if masked else b""
        payload = self._recv_exact(length, deadline)
        if masked:
            payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        if opcode == OP_PING:
            self._send_frame(OP_PONG, payload)
            return self._recv_frame(deadline)
        return opcode, payload, fin

    # -- handshake ---------------------------------------------------------
    def connect(self) -> None:
        path = self.path
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: {WS_VERSION}\r\n"
            "\r\n"
        )
        try:
            self.sock = socket.create_connection((self.host, self.port),
                                                 timeout=self.timeout)
        except OSError as exc:
            raise WsConnectError(
                f"cannot connect to ws://{self.host}:{self.port}{self.path}: {exc}"
            ) from exc

        try:
            self.sock.sendall(request.encode("ascii"))
        except OSError as exc:
            raise WsConnectError(f"handshake write failed: {exc}") from exc

        deadline = time.monotonic() + self.timeout
        raw = b""
        while b"\r\n\r\n" not in raw:
            if len(raw) > MAX_HTTP_HEADER:
                raise WsConnectError("HTTP upgrade response too large")
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise WsConnectError("timeout waiting for HTTP upgrade response")
            self.sock.settimeout(remain)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout as exc:
                raise WsConnectError("timeout waiting for HTTP upgrade response") from exc
            except OSError as exc:
                raise WsConnectError(f"handshake read failed: {exc}") from exc
            if not chunk:
                raise WsConnectError("device closed the connection during the handshake")
            raw += chunk

        head, self._rx = raw.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        status = lines[0].split(" ", 2)
        if len(status) < 2 or not status[0].startswith("HTTP/") or status[1] != "101":
            raise WsConnectError(f"WebSocket upgrade rejected: {lines[0]}")
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                headers[name.strip().lower()] = value.strip()
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
        ).decode("ascii")
        actual = headers.get("sec-websocket-accept", "")
        if actual != expected:
            raise WsConnectError("invalid Sec-WebSocket-Accept in upgrade response")

    # -- message level -----------------------------------------------------
    def send_json(self, message: dict) -> None:
        self._send_frame(OP_TEXT, json.dumps(message, separators=(",", ":")).encode("utf-8"))

    def recv_json(self, timeout: float | None = None) -> dict:
        """Return the next JSON text message; skip empty heartbeat frames."""
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        buffer = b""
        while True:
            if time.monotonic() >= deadline:
                raise CheckError("timeout waiting for a JSON response")
            opcode, payload, fin = self._recv_frame(deadline)
            if opcode == OP_CLOSE:
                raise WsClosed("device closed the WebSocket")
            if opcode == OP_PONG:
                continue
            if opcode not in (OP_TEXT, OP_BINARY, OP_CONT):
                continue
            buffer += payload
            if not fin:
                continue
            if not buffer:
                continue  # 1500 ms heartbeat: empty TEXT frame
            text = buffer.decode("utf-8", errors="replace")
            buffer = b""
            try:
                message = json.loads(text)
            except json.JSONDecodeError:
                raise CheckError(f"device sent non-JSON text frame: {text[:120]!r}")
            if not isinstance(message, dict):
                raise CheckError(f"device sent a non-object JSON message: {text[:120]!r}")
            return message

    def request(self, module: int, cmd: int, body: dict | None = None,
                timeout: float | None = None) -> dict:
        message = {"module": module, "cmd": cmd}
        if body:
            message.update(body)
        self.send_json(message)
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                raise CheckError(f"timeout waiting for module={module} cmd={cmd}")
            response = self.recv_json(remain)
            if response.get("module") == module and response.get("cmd") == cmd:
                return response
            # Some firmware/web-server error paths send the legacy
            # {"error": "...", "code": N} frame without module/cmd (the web
            # layer's ws_set_err_msg). Keep it usable but mark it so the
            # protocol-shape checks can report the inconsistency.
            if (is_int(response.get("code"))
                    and ("err" in response or "error" in response)
                    and "module" not in response and "cmd" not in response):
                normalized = dict(response)
                normalized.setdefault("err", response.get("error"))
                normalized["module"] = module
                normalized["cmd"] = cmd
                normalized["_bare_error_frame"] = True
                return normalized

    def close(self) -> None:
        if self.sock is None:
            return
        try:
            self._send_frame(OP_CLOSE, b"\x03\xe8")  # 1000 normal closure
        except (WsConnectError, OSError):
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock = None


# ---------------------------------------------------------------------------
# Reporting (TAP-ish, or --json)
# ---------------------------------------------------------------------------
class Reporter:
    def __init__(self, json_mode: bool, verbose: bool):
        self.json_mode = json_mode
        self.verbose = verbose
        self.checks: list[dict] = []
        self.number = 0

    def _record(self, ok: bool, name: str, detail: str = "") -> None:
        self.number += 1
        entry = {"n": self.number, "ok": ok, "name": name, "detail": detail}
        self.checks.append(entry)
        if self.json_mode:
            return
        if ok:
            suffix = f": {detail}" if detail else ""
            print(f"ok {self.number} - {name}{suffix}")
        else:
            print(f"not ok {self.number} - {name}")
            if detail:
                print(f"#   {detail}")

    def ok(self, name: str, detail: str = "") -> None:
        self._record(True, name, detail)

    def fail(self, name: str, detail: str = "") -> None:
        self._record(False, name, detail)

    def info(self, text: str) -> None:
        if not self.json_mode:
            print(f"# {text}")

    def finish(self) -> int:
        failed = [entry for entry in self.checks if not entry["ok"]]
        passed = self.number - len(failed)
        if self.json_mode:
            print(json.dumps({
                "tool": "test_wifi_device_readonly.py",
                "ok": not failed,
                "passed": passed,
                "failed": len(failed),
                "checks": self.checks,
            }, ensure_ascii=False, indent=2))
        else:
            print(f"1..{self.number}")
            print(f"# {passed}/{self.number} checks passed"
                  + (f" ({len(failed)} FAILED)" if failed else ""))
        return 0 if not failed else 3


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------
def is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_bool(value) -> bool:
    return isinstance(value, bool)


def byte_len(text: str) -> int:
    return len(text.encode("utf-8"))


def rng(value: int, low: int, high: int) -> bool:
    return is_int(value) and low <= value <= high


def error_response_bad(response: dict) -> str | None:
    """Return None when @p response looks like the protocol error shape."""
    if response.get("_bare_error_frame"):
        return ("error frame lacks module/cmd (web-layer ws_set_err_msg format, "
                "protocol §1.1)")
    if not is_int(response.get("module")):
        return "error response has no integer 'module'"
    if not is_int(response.get("cmd")):
        return "error response has no integer 'cmd'"
    if not isinstance(response.get("err"), str) or not response.get("err"):
        return "error response has no non-empty string 'err'"
    if not is_int(response.get("code")):
        return "error response has no integer 'code'"
    return None


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def check_fm_info(ws: WsClient, rep: Reporter) -> dict:
    response = ws.request(MODULE_SYSTEM, SYS_GET_FM_INFO)
    expected = response.get("protocol_version")
    fm_ver = response.get("fm_ver")
    if is_int(expected) and expected == 1 and isinstance(fm_ver, str) and fm_ver:
        rep.ok("R1 SYSTEM/GET_FM_INFO", f"fm_ver={fm_ver} protocol_version={expected}")
    elif is_int(expected) and expected != 1:
        rep.fail("R1 SYSTEM/GET_FM_INFO", f"protocol_version={expected}, expected 1")
    else:
        rep.fail("R1 SYSTEM/GET_FM_INFO", f"unexpected response: {response}")
    return response


def check_sys_info(ws: WsClient, rep: Reporter) -> str:
    response = ws.request(MODULE_SYSTEM, SYS_GET_SYS_INFO)
    sn = response.get("sn", "")
    if isinstance(sn, str) and SN_RE.match(sn):
        rep.ok("R2 SYSTEM/GET_SYS_INFO", f"sn={sn} (AP 默认 SSID 后缀应为 {sn[-4:]})")
        return sn
    # SN is documented as the STA MAC by default; some debug builds may return
    # an empty string. Treat it as informational so R4 falls back to a format
    # check only.
    rep.info(f"R2 SYSTEM/GET_SYS_INFO: sn={sn!r} is not 12 upper-case hex chars;"
             " skipping the AP SSID suffix comparison")
    rep.ok("R2 SYSTEM/GET_SYS_INFO", "sn present (suffix check skipped)")
    return ""


def check_saved_list(ws: WsClient, rep: Reporter) -> dict:
    response = ws.request(MODULE_WIFI, WIFI_GET_SAVED_LIST)
    if "err" in response:
        code = response.get("code")
        if code == ERR_UNSUPPORTED:
            rep.fail("R3 WIFI/GET_SAVED_LIST(11)",
                     "device returned code=4 Unsupported cmd: firmware lacks WP-202 "
                     "(upgrade to a build with cmd 11-14)")
            return response
        rep.fail("R3 WIFI/GET_SAVED_LIST(11)", f"device error: {response}")
        return response

    problems = []
    if not is_bool(response.get("auto_save")):
        problems.append("auto_save is not a JSON bool")
    saved_list = response.get("list")
    if not isinstance(saved_list, list):
        problems.append("list is not an array")
        saved_list = []
    count = response.get("count")
    if count is not None and not is_int(count):
        problems.append("count is not an integer")
    elif is_int(count) and count != len(saved_list):
        problems.append(f"count={count} != len(list)={len(saved_list)}")
    for index, item in enumerate(saved_list):
        prefix = f"list[{index}]"
        if not isinstance(item, dict):
            problems.append(f"{prefix} is not an object")
            continue
        if item.get("idx") != index:
            problems.append(f"{prefix}.idx={item.get('idx')!r} != {index} (list must be "
                            "idx ascending/compact)")
        ssid = item.get("ssid")
        if not isinstance(ssid, str) or not (1 <= byte_len(ssid) <= 32):
            problems.append(f"{prefix}.ssid is not a 1..32 byte UTF-8 string")
        if "password" in item or "password" in response:
            problems.append("response leaks a password field (security exception §7.2)")
        if not is_int(item.get("last_success")) or item.get("last_success") < 0:
            problems.append(f"{prefix}.last_success is not a non-negative integer")
        if not rng(item.get("priority"), 0, 255):
            problems.append(f"{prefix}.priority is not in 0..255")
    max_entries = response.get("max")
    if max_entries is not None:
        if not is_int(max_entries) or not (1 <= max_entries <= 8):
            problems.append(f"max={max_entries!r} is not an integer in 1..8")
        elif is_int(count) and count > max_entries:
            problems.append(f"count={count} > max={max_entries}")
    connected_idx = response.get("connected_idx")
    if connected_idx is not None:
        if not is_int(connected_idx) or not (0 <= connected_idx < len(saved_list)):
            problems.append(f"connected_idx={connected_idx!r} is out of range")

    if problems:
        rep.fail("R3 WIFI/GET_SAVED_LIST(11)", "; ".join(problems))
    else:
        rep.ok("R3 WIFI/GET_SAVED_LIST(11)",
               f"auto_save={response.get('auto_save')} count={len(saved_list)} "
               f"max={max_entries!r} connected_idx={connected_idx!r} "
               "(no password echo)")
    return response


def check_ap_info(ws: WsClient, rep: Reporter, sn: str) -> dict:
    response = ws.request(MODULE_WIFI, WIFI_GET_AP_INFO)
    if "err" in response:
        rep.fail("R4 WIFI/GET_AP_INFO(5)", f"device error: {response}")
        return response

    problems = []
    ssid = response.get("ssid")
    password = response.get("password")
    for field in ("ip", "gateway", "netmask"):
        if not isinstance(response.get(field), str) or not response.get(field):
            problems.append(f"{field} missing")
    if not isinstance(password, str) or not (8 <= byte_len(password) <= 63):
        problems.append("password is not an 8..63 byte string")
    if not isinstance(ssid, str) or not (1 <= byte_len(ssid) <= 32):
        problems.append("ssid is not a 1..32 byte string")
        default_match = None
    else:
        default_match = AP_DEFAULT_RE.match(ssid)
        if default_match:
            if sn:
                expected_suffix = sn[-4:]
                if default_match.group(1) != expected_suffix:
                    problems.append(
                        f"default AP SSID suffix {default_match.group(1)} != "
                        f"sn[-4:] {expected_suffix} (contract §6)")
        else:
            rep.info(f"R4 WIFI/GET_AP_INFO(5): custom AP SSID {ssid!r} "
                     "(user value wins, suffix check skipped per contract §6)")

    if problems:
        rep.fail("R4 WIFI/GET_AP_INFO(5)", "; ".join(problems))
    else:
        kind = "default" if default_match else "custom"
        rep.ok("R4 WIFI/GET_AP_INFO(5)",
               f"ssid={ssid} ({kind}), password length={byte_len(password)}")
    return response


def check_mode(ws: WsClient, rep: Reporter) -> dict:
    response = ws.request(MODULE_WIFI, WIFI_GET_MODE)
    problems = []
    if "err" in response:
        problems = [f"device error: {response}"]
    else:
        mode = response.get("mode")
        status = response.get("status")
        if not is_int(mode) or mode not in PERMANENT_MODES:
            problems.append(f"mode={mode!r} not in {sorted(PERMANENT_MODES)}")
        if not is_int(status) or status not in WIFI_MODES:
            problems.append(f"status={status!r} not in {sorted(WIFI_MODES)}")
    if problems:
        rep.fail("R5 WIFI/GET_MODE(6)", "; ".join(problems))
    else:
        rep.ok("R5 WIFI/GET_MODE(6)",
               f"mode={response.get('mode')} status={response.get('status')}")
    return response


def check_sta_info(ws: WsClient, rep: Reporter) -> dict:
    response = ws.request(MODULE_WIFI, WIFI_GET_STA_INFO)
    if "err" in response:
        rep.fail("R6 WIFI/GET_STA_INFO(1)", f"device error: {response}")
        return response
    problems = []
    for field, kind in (("ssid", str), ("rssi", int), ("mac", str)):
        value = response.get(field)
        if kind is int:
            if not is_int(value):
                problems.append(f"{field} is not an integer")
        elif not isinstance(value, kind):
            problems.append(f"{field} is not a {kind.__name__}")
    if problems:
        rep.fail("R6 WIFI/GET_STA_INFO(1)", "; ".join(problems))
    else:
        ssid = response.get("ssid") or "(disconnected)"
        rep.ok("R6 WIFI/GET_STA_INFO(1)", f"ssid={ssid} rssi={response.get('rssi')}")
    return response


def check_error_paths(ws: WsClient, rep: Reporter) -> None:
    """Invalid write commands must be rejected with code 5 and no side effects."""
    probes = [
        ("R7a ADD_SAVED invalid ssid", WIFI_ADD_SAVED, {"ssid": "x" * 33, "password": "12345678"}),
        ("R7b DEL_SAVED invalid idx", WIFI_DEL_SAVED, {"idx": 255}),
        ("R7c SET_AUTO_SAVE non-bool", WIFI_SET_AUTO_SAVE, {"enable": 1}),
    ]
    for name, cmd, body in probes:
        try:
            response = ws.request(MODULE_WIFI, cmd, body)
        except CheckError as exc:
            rep.fail(name, str(exc))
            continue
        if "err" not in response:
            rep.fail(name, f"invalid request was accepted: {response}")
            continue
        shape_problem = error_response_bad(response)
        if shape_problem:
            rep.fail(name, shape_problem)
        elif response.get("code") == ERR_PROPERTY:
            rep.ok(name, f"code=5 err={response.get('err')!r}")
        elif response.get("code") == ERR_UNSUPPORTED:
            rep.fail(name, "code=4 Unsupported cmd: firmware lacks WP-202")
        else:
            rep.fail(name, f"expected code=5, got code={response.get('code')!r}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="test_wifi_device_readonly.py",
        description="Read-only validation of the WiFi multi-credential JSON API "
                    "(cmd 11/5/1/6 + invalid cmd 12-14 probes). No device state is "
                    "modified (docs/hardware-test-wifi.md).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python3 scripts/test_wifi_device_readonly.py --host 192.168.1.1\n"
            "  python3 scripts/test_wifi_device_readonly.py --host dap.local --json\n"
        ),
    )
    parser.add_argument("--host", required=True, help="device IP or hostname")
    parser.add_argument("--port", type=int, default=80, help="HTTP/WS port (default 80)")
    parser.add_argument("--path", default="/ws", help="WebSocket path (default /ws)")
    parser.add_argument("--timeout", type=float, default=5.0,
                        help="per-request timeout in seconds (default 5.0)")
    parser.add_argument("--skip-error-probes", action="store_true",
                        help="skip the invalid cmd 12/13/14 error-shape probes")
    parser.add_argument("--json", action="store_true",
                        help="print a machine-readable JSON summary instead of TAP lines")
    parser.add_argument("-v", "--verbose", action="store_true", help="verbose diagnostics")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    rep = Reporter(args.json, args.verbose)
    ws = WsClient(args.host, args.port, args.path, args.timeout)

    rep.info(f"target ws://{args.host}:{args.port}{args.path} "
             f"(read-only: only GET commands and invalid probes are sent)")

    try:
        ws.connect()
        rep.ok("R0 WebSocket handshake", f"HTTP 101 @ {args.host}:{args.port}{args.path}")
    except WsConnectError as exc:
        rep.info(f"connection failed: {exc}")
        if args.json:
            print(json.dumps({"tool": "test_wifi_device_readonly.py", "ok": False,
                              "error": str(exc)}, ensure_ascii=False, indent=2))
        else:
            print(f"Bail out! {exc}")
        return 2
    except CheckError as exc:
        rep.info(f"connection setup failed: {exc}")
        if args.json:
            print(json.dumps({"tool": "test_wifi_device_readonly.py", "ok": False,
                              "error": str(exc)}, ensure_ascii=False, indent=2))
        else:
            print(f"Bail out! {exc}")
        return 3

    sn = ""
    try:
        check_fm_info(ws, rep)
        sn = check_sys_info(ws, rep)
        check_saved_list(ws, rep)
        check_ap_info(ws, rep, sn)
        check_mode(ws, rep)
        check_sta_info(ws, rep)
        if not args.skip_error_probes:
            check_error_paths(ws, rep)
    except WsClosed as exc:
        rep.fail("R9 connection", f"device closed the WebSocket: {exc}")
    except CheckError as exc:
        rep.fail("R9 connection", str(exc))
    except WsConnectError as exc:
        rep.fail("R9 connection", str(exc))
    finally:
        ws.close()

    return rep.finish()


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        print("# interrupted", file=sys.stderr)
        sys.exit(2)
