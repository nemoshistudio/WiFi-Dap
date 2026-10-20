#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""WiFi Dap smoke test (pure Python standard library).

Read-only network smoke test for a running WiFi Dap:

  1. USBIP   - TCP 3240, OP_REQ_DEVLIST: parse the device list (VID/PID/busid)
               and optionally OP_REQ_IMPORT (attach reply);
  2. HTTP    - TCP 80, GET /: expect 200 + gzip-embedded Web UI (marker check);
  3. JSON-WS - TCP 80, WebSocket /ws: send SYSTEM(0)/GET_FM_INFO(1) and verify
               the JSON handshake (protocol_version, fm_ver, upd_upd_date);
  4. WS-DAP  - optional: TCP 3240 WebSocket upgrade (legacy WebSocket-DAP
               transport used by elaphure-dap.js).

The script does NOT flash, reset, or run DAP commands; it only speaks the
transport handshakes (plus GET_FM_INFO, a read-only JSON command). Safe to run
against a device that is being debugged over another transport? No - the DAP
proxy port (3240) is single-connection and serial, so run it while no debugger
session is active (see docs/hardware-test-dap.md T0).

Exit codes: 0 = all selected checks passed, 1 = at least one check failed,
2 = usage error (argparse).

Examples:
    python3 scripts/dap_smoke.py --host 192.168.1.1
    python3 scripts/dap_smoke.py --host 192.168.1.1 --with-usbip-attach --with-ws-dap
    python3 scripts/dap_smoke.py --host 192.168.1.1 --json --expect protocol_version=1
    python3 scripts/dap_smoke.py --host wifi-dap.local --skip-usbip   # mDNS name, HTTP/WS only
"""

from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import os
import socket
import struct
import sys
import time

# ---------------------------------------------------------------------------
# Protocol constants (must match components/dap_proxy + components/USBIP)
# ---------------------------------------------------------------------------
USBIP_VERSION = 0x0111  # 273, see usbip_server.c send_stage1_header()
USBIP_OP_REQ_DEVLIST = 0x8005
USBIP_OP_REP_DEVLIST = 0x0005
USBIP_OP_REQ_IMPORT = 0x8003
USBIP_OP_REP_IMPORT = 0x0003
USBIP_BUSID_SIZE = 32
# usbip_stage1_usb_device: path[256] + busid[32] + 3*u32 + 3*u16 + 6*u8
USBIP_DEVICE_SIZE = 312
USBIP_INTERFACE_SIZE = 4

DAP_USB_VID = 0xC251  # components/USBIP/usb_descriptor.h
DAP_USB_PID = 0xF00A
DAP_USB_BUSID = "1-1"

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_OP_CONT, WS_OP_TEXT, WS_OP_BINARY, WS_OP_CLOSE, WS_OP_PING, WS_OP_PONG = 0, 1, 2, 8, 9, 10

# SYSTEM(0)/GET_FM_INFO(1) request (docs/protocol.md §2.1)
FM_INFO_REQUEST = {"module": 0, "cmd": 1}

# Default expectations; override with --expect key=value (see build_expectations)
DEFAULT_EXPECT = {
    "vid": DAP_USB_VID,
    "pid": DAP_USB_PID,
    "busid": DAP_USB_BUSID,
    "protocol_version": 1,
    "http_marker": '<div id="app">',
}
ALLOWED_EXPECT_KEYS = {"vid", "pid", "busid", "protocol_version", "http_marker"}


class SmokeError(RuntimeError):
    """A probe failed for a protocol/reachability reason."""


class CheckTimeout(SmokeError):
    """A socket operation exceeded its deadline."""


# ---------------------------------------------------------------------------
# Small socket helpers
# ---------------------------------------------------------------------------
def connect(host: str, port: int, timeout: float, retries: int = 2) -> socket.socket:
    last: Exception | None = None
    for attempt in range(retries + 1):
        try:
            sock = socket.create_connection((host, port), timeout=timeout)
            sock.settimeout(timeout)
            return sock
        except OSError as exc:  # connection refused / unreachable / DNS
            last = exc
            if attempt < retries:
                time.sleep(0.4)
    raise SmokeError(f"cannot connect to {host}:{port}: {last}")


def recv_exact(sock: socket.socket, size: int, what: str = "data") -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise SmokeError(f"connection closed while reading {what} "
                             f"({size - remaining}/{size} bytes received)")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def recv_until(sock: socket.socket, marker: bytes, limit: int, what: str = "data") -> bytes:
    data = bytearray()
    while marker not in data:
        if len(data) > limit:
            raise SmokeError(f"{what}: no {marker!r} within {limit} bytes")
        chunk = sock.recv(4096)
        if not chunk:
            raise SmokeError(f"{what}: connection closed before {marker!r}")
        data.extend(chunk)
    return bytes(data)


def recv_available(sock: socket.socket, limit: int, idle_timeout: float = 0.5) -> bytes:
    """Read until EOF (Connection: close) or an idle timeout; bounded by limit."""
    sock.settimeout(idle_timeout)
    data = bytearray()
    try:
        while len(data) < limit:
            chunk = sock.recv(min(65536, limit - len(data)))
            if not chunk:
                break
            data.extend(chunk)
    except socket.timeout:
        pass
    return bytes(data)


# ---------------------------------------------------------------------------
# USBIP probe (TCP 3240)
# ---------------------------------------------------------------------------
def _decode_cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def _parse_usbip_device(raw: bytes, offset: int = 0) -> tuple[dict, int]:
    if len(raw) - offset < USBIP_DEVICE_SIZE:
        raise SmokeError("USBIP device record truncated")
    path = _decode_cstr(raw[offset:offset + 256])
    busid = _decode_cstr(raw[offset + 256:offset + 288])
    busnum, devnum, speed = struct.unpack_from("!III", raw, offset + 288)
    vid, pid, bcd = struct.unpack_from("!HHH", raw, offset + 300)
    num_interfaces = raw[offset + 311]
    dev = {
        "path": path,
        "busid": busid,
        "busnum": busnum,
        "devnum": devnum,
        "speed": speed,
        "vid": vid,
        "pid": pid,
        "bcd_device": bcd,
        "num_interfaces": num_interfaces,
    }
    return dev, offset + USBIP_DEVICE_SIZE + num_interfaces * USBIP_INTERFACE_SIZE


def probe_usbip_devlist(host: str, port: int, timeout: float, retries: int) -> dict:
    sock = connect(host, port, timeout, retries)
    try:
        sock.sendall(struct.pack("!HHI", USBIP_VERSION, USBIP_OP_REQ_DEVLIST, 0))
        header = recv_exact(sock, 8, "USBIP OP_REP_DEVLIST header")
        version, command, status = struct.unpack("!HHI", header)
        if command != USBIP_OP_REP_DEVLIST:
            raise SmokeError(f"unexpected USBIP reply command 0x{command:04X} "
                             f"(expected 0x{USBIP_OP_REP_DEVLIST:04X})")
        if status != 0:
            raise SmokeError(f"USBIP OP_REP_DEVLIST status={status}")
        (list_size,) = struct.unpack("!I", recv_exact(sock, 4, "USBIP device list size"))
        if list_size > 16:
            raise SmokeError(f"USBIP device list size {list_size} looks bogus")
        devices = []
        interfaces = 0
        # OP_REP_DEVLIST layout: per device, the fixed-size device record is
        # immediately followed by its bNumInterfaces * 4-byte interface records.
        for _ in range(list_size):
            raw = recv_exact(sock, USBIP_DEVICE_SIZE, "USBIP device record")
            dev, _offset = _parse_usbip_device(raw)
            devices.append(dev)
            if dev["num_interfaces"]:
                recv_exact(sock, dev["num_interfaces"] * USBIP_INTERFACE_SIZE,
                           "USBIP interface records")
                interfaces += dev["num_interfaces"]
        return {
            "version": version,
            "devices": devices,
            "interfaces": interfaces,
        }
    finally:
        sock.close()


def probe_usbip_import(host: str, port: int, timeout: float, retries: int,
                       busid: str = DAP_USB_BUSID) -> dict:
    """OP_REQ_IMPORT probe on a fresh connection (usbip client 'attach')."""
    sock = connect(host, port, timeout, retries)
    try:
        busid_raw = busid.encode("ascii")
        if len(busid_raw) >= USBIP_BUSID_SIZE:
            raise SmokeError("busid too long")
        req = struct.pack("!HHI", USBIP_VERSION, USBIP_OP_REQ_IMPORT, 0)
        req += busid_raw + b"\x00" * (USBIP_BUSID_SIZE - len(busid_raw))
        sock.sendall(req)
        header = recv_exact(sock, 8, "USBIP OP_REP_IMPORT header")
        _version, command, status = struct.unpack("!HHI", header)
        if command != USBIP_OP_REP_IMPORT:
            raise SmokeError(f"unexpected USBIP import reply command 0x{command:04X}")
        if status != 0:
            raise SmokeError(f"USBIP OP_REP_IMPORT status={status}")
        raw = recv_exact(sock, USBIP_DEVICE_SIZE, "USBIP import device record")
        dev, _ = _parse_usbip_device(raw)
        return dev
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# HTTP probe (TCP 80)
# ---------------------------------------------------------------------------
def _http_parse(raw: bytes) -> tuple[int, dict, bytes]:
    if b"\r\n\r\n" not in raw:
        raise SmokeError("HTTP response header incomplete")
    head, body = raw.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split(b" ")[1])
    except (IndexError, ValueError):
        raise SmokeError(f"bad HTTP status line: {lines[0]!r}")
    headers = {}
    for line in lines[1:]:
        if b":" in line:
            key, value = line.split(b":", 1)
            headers[key.strip().lower().decode("latin-1")] = value.strip().decode("latin-1")
    if headers.get("transfer-encoding", "").lower() == "chunked":
        body = _http_dechunk(body)
    return status, headers, body


def _http_dechunk(body: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while True:
        line_end = body.find(b"\r\n", pos)
        if line_end < 0:
            raise SmokeError("chunked HTTP body truncated")
        try:
            size = int(body[pos:line_end].split(b";")[0], 16)
        except ValueError:
            raise SmokeError("bad chunk size in HTTP body")
        pos = line_end + 2
        if size == 0:
            return bytes(out)
        out.extend(body[pos:pos + size])
        pos += size + 2


def probe_http_index(host: str, port: int, timeout: float, retries: int) -> dict:
    sock = connect(host, port, timeout, retries)
    try:
        req = (f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n"
               f"User-Agent: wifi-dap-smoke/1.0\r\n\r\n")
        sock.sendall(req.encode("ascii"))
        raw = recv_available(sock, limit=4 << 20)
    finally:
        sock.close()
    status, headers, body = _http_parse(raw)
    encoding = headers.get("content-encoding", "").lower()
    decoded = body
    if "gzip" in encoding:
        try:
            decoded = gzip.decompress(body)
        except OSError as exc:
            raise SmokeError(f"Content-Encoding: gzip but body is not gzip: {exc}")
    return {
        "status": status,
        "content_encoding": encoding,
        "content_type": headers.get("content-type", ""),
        "bytes_raw": len(body),
        "bytes_decoded": len(decoded),
        "html_prefix": decoded[:160].decode("utf-8", "replace").replace("\n", " "),
        "html": decoded,
    }


# ---------------------------------------------------------------------------
# WebSocket helpers (handshake + minimal frame codec)
# ---------------------------------------------------------------------------
class WsConnection:
    def __init__(self, sock: socket.socket, leftover: bytes):
        self.sock = sock
        self.buf = bytearray(leftover)

    def send_text(self, payload: str) -> None:
        self._send_frame(WS_OP_TEXT, payload.encode("utf-8"))

    def send_pong(self, payload: bytes) -> None:
        self._send_frame(WS_OP_PONG, payload)

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 1 << 16:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        mask = os.urandom(4)
        header.extend(mask)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _recv_exact(self, size: int) -> bytes:
        while len(self.buf) < size:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SmokeError("WebSocket connection closed by peer")
            self.buf.extend(chunk)
        out = bytes(self.buf[:size])
        del self.buf[:size]
        return out

    def recv_frame(self) -> tuple[int, bytes]:
        b0, b1 = self._recv_exact(2)
        fin = (b0 & 0x80) != 0
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._recv_exact(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._recv_exact(8))
        if length > 1 << 20:
            raise SmokeError(f"WebSocket frame too large: {length}")
        mask = self._recv_exact(4) if masked else None
        payload = self._recv_exact(length) if length else b""
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if not fin:
            raise SmokeError("fragmented WebSocket frames are not supported by this smoke test")
        return opcode, payload

    def recv_json_text(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            self.sock.settimeout(remaining)
            try:
                opcode, payload = self.recv_frame()
            except socket.timeout:
                continue
            if opcode == WS_OP_PING:
                self.send_pong(payload)
                continue
            if opcode == WS_OP_CLOSE:
                raise SmokeError("server closed the WebSocket during handshake")
            if opcode not in (WS_OP_TEXT, WS_OP_BINARY):
                continue
            if not payload:
                continue  # the 1500 ms heartbeat is an empty TEXT frame
            try:
                msg = json.loads(payload.decode("utf-8", "replace"))
            except ValueError:
                continue  # not a JSON text frame (binary DAP data etc.)
            if isinstance(msg, dict):
                return msg
        raise CheckTimeout("no JSON response on /ws before timeout")


def ws_upgrade(host: str, port: int, path: str, timeout: float, retries: int,
               extra_headers: dict | None = None) -> tuple[socket.socket, WsConnection, int, dict]:
    sock = connect(host, port, timeout, retries)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    lines = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
        "User-Agent: wifi-dap-smoke/1.0",
    ]
    for name, value in (extra_headers or {}).items():
        lines.append(f"{name}: {value}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii"))
    raw = recv_until(sock, b"\r\n\r\n", 16 * 1024, "WebSocket handshake")
    header, leftover = raw.split(b"\r\n\r\n", 1)
    status, headers, _body = _http_parse(header + b"\r\n\r\n")
    expected_accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
    accept_ok = headers.get("sec-websocket-accept") == expected_accept
    conn = WsConnection(sock, leftover)
    return sock, conn, status, {
        "status": status,
        "accept_ok": accept_ok,
        "headers": headers,
    }


def probe_ws_json(host: str, port: int, path: str, timeout: float, retries: int) -> dict:
    sock, conn, status, info = ws_upgrade(host, port, path, timeout, retries, None)
    try:
        if status != 101:
            raise SmokeError(f"WS upgrade returned HTTP {status} (expected 101)")
        if not info["accept_ok"]:
            raise SmokeError("WS handshake Sec-WebSocket-Accept mismatch")
        conn.send_text(json.dumps(FM_INFO_REQUEST))
        msg = conn.recv_json_text(timeout)
        return {"upgrade": info, "fm_info": msg}
    finally:
        try:
            sock.close()
        except OSError:
            pass


def probe_ws_dap(host: str, port: int, timeout: float, retries: int) -> dict:
    """Legacy WebSocket-DAP transport on the DAP proxy port (3240)."""
    sock, _conn, status, info = ws_upgrade(host, port, "/", timeout, retries, None)
    try:
        if status != 101:
            raise SmokeError(f"WS-DAP upgrade returned HTTP {status} (expected 101); "
                             f"is CONFIG_BIRDIEBOT_DAP_WS_ENABLE=y?")
        if not info["accept_ok"]:
            raise SmokeError("WS-DAP handshake Sec-WebSocket-Accept mismatch")
        return {"upgrade": info}
    finally:
        try:
            sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Expectations / result checks
# ---------------------------------------------------------------------------
def parse_expectations(pairs: list[str], defaults: dict) -> dict:
    exp = dict(defaults)
    for pair in pairs:
        if "=" not in pair:
            raise ValueError(f"--expect expects key=value, got {pair!r}")
        key, value = pair.split("=", 1)
        key = key.strip().lower()
        if key not in ALLOWED_EXPECT_KEYS:
            raise ValueError(f"--expect: unknown key {key!r} "
                             f"(allowed: {', '.join(sorted(ALLOWED_EXPECT_KEYS))})")
        if key in ("vid", "pid"):
            exp[key] = int(value, 0)
        elif key == "protocol_version":
            exp[key] = int(value, 0)
        else:
            exp[key] = value
    return exp


def check(ok: bool, message: str, errors: list[str]) -> bool:
    if not ok:
        errors.append(message)
    return ok


def assess(results: dict, exp: dict) -> tuple[dict, list[str]]:
    """Turn raw probe results into pass/fail per section."""
    verdict: dict[str, dict] = {}
    errors: list[str] = []

    if "usbip" in results:
        raw = results["usbip"]
        errs: list[str] = []
        devs = raw.get("devices", [])
        check(len(devs) >= 1, "USBIP: device list is empty", errs)
        match = None
        for dev in devs:
            if dev["vid"] == exp["vid"] and dev["pid"] == exp["pid"]:
                match = dev
                break
        check(match is not None,
              f"USBIP: no device with VID=0x{exp['vid']:04X} PID=0x{exp['pid']:04X} "
              f"(got {[(hex(d['vid']), hex(d['pid'])) for d in devs]})", errs)
        if match is not None:
            check(match["busid"] == exp["busid"],
                  f"USBIP: busid {match['busid']!r} != {exp['busid']!r}", errs)
        verdict["usbip"] = {"ok": not errs, "errors": errs,
                            "devices": [{k: v for k, v in d.items()} for d in devs]}
        errors.extend(errs)

    if "usbip_import" in results:
        raw = results["usbip_import"]
        errs = []
        check(raw["vid"] == exp["vid"] and raw["pid"] == exp["pid"],
              f"USBIP import: VID/PID 0x{raw['vid']:04X}/0x{raw['pid']:04X} mismatch", errs)
        check(raw["busid"] == exp["busid"],
              f"USBIP import: busid {raw['busid']!r} != {exp['busid']!r}", errs)
        verdict["usbip_import"] = {"ok": not errs, "errors": errs, "device": raw}
        errors.extend(errs)

    if "http" in results:
        raw = results["http"]
        errs = []
        check(raw["status"] == 200, f"HTTP: GET / returned {raw['status']} (expected 200)", errs)
        check("gzip" in raw["content_encoding"],
              f"HTTP: Content-Encoding={raw['content_encoding']!r} (expected gzip)", errs)
        marker = exp["http_marker"].encode("utf-8")
        check(marker in raw["html"],
              f"HTTP: Web UI marker {exp['http_marker']!r} not found in decoded index.html", errs)
        verdict["http"] = {"ok": not errs, "errors": errs,
                           "status": raw["status"],
                           "content_encoding": raw["content_encoding"],
                           "bytes_decoded": raw["bytes_decoded"],
                           "html_prefix": raw["html_prefix"]}
        errors.extend(errs)

    if "ws" in results:
        raw = results["ws"]
        errs = []
        fm = raw.get("fm_info", {})
        pv = fm.get("protocol_version")
        check(raw.get("upgrade", {}).get("accept_ok", False),
              "WS /ws: handshake Sec-WebSocket-Accept mismatch", errs)
        check(fm.get("module") == 0 and fm.get("cmd") == 1,
              f"WS /ws: unexpected GET_FM_INFO response: {fm}", errs)
        check(pv == exp["protocol_version"],
              f"WS /ws: protocol_version {pv!r} != expected {exp['protocol_version']}", errs)
        verdict["ws"] = {"ok": not errs, "errors": errs,
                         "protocol_version": pv,
                         "fm_info": fm}
        errors.extend(errs)

    if "ws_dap" in results:
        raw = results["ws_dap"]
        errs = []
        check(raw.get("upgrade", {}).get("status") == 101,
              "WS-DAP: expected HTTP 101 on TCP 3240", errs)
        check(raw.get("upgrade", {}).get("accept_ok", False),
              "WS-DAP: Sec-WebSocket-Accept mismatch", errs)
        verdict["ws_dap"] = {"ok": not errs, "errors": errs}
        errors.extend(errs)

    return verdict, errors


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="WiFi Dap smoke test (USBIP devlist + HTTP UI + JSON-WS + optional WS-DAP).",
        epilog="Exit code: 0 = all selected checks passed, 1 = failure, 2 = usage error.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", required=True,
                        help="device IP (AP default 192.168.1.1) or mDNS name")
    parser.add_argument("--usbip-port", type=int, default=3240, help="DAP proxy port")
    parser.add_argument("--http-port", type=int, default=80, help="Web server port")
    parser.add_argument("--timeout", type=float, default=3.0,
                        help="per-operation socket timeout in seconds")
    parser.add_argument("--retries", type=int, default=2, help="TCP connect retries")
    parser.add_argument("--skip-usbip", action="store_true", help="skip the USBIP devlist probe")
    parser.add_argument("--skip-http", action="store_true", help="skip the HTTP UI probe")
    parser.add_argument("--skip-ws", action="store_true", help="skip the JSON /ws probe")
    parser.add_argument("--with-usbip-attach", action="store_true",
                        help="also probe USBIP OP_REQ_IMPORT (attach reply)")
    parser.add_argument("--with-ws-dap", action="store_true",
                        help="also probe the legacy WebSocket-DAP transport on the proxy port")
    parser.add_argument("--expect", action="append", default=[], metavar="KEY=VALUE",
                        help="override expectation (keys: vid,pid,busid,protocol_version,http_marker)")
    parser.add_argument("--json", action="store_true", help="print machine-readable JSON summary")
    parser.add_argument("-v", "--verbose", action="store_true", help="print per-probe details")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        exp = parse_expectations(args.expect, DEFAULT_EXPECT)
    except ValueError as exc:
        parser.error(str(exc))  # exits with status 2 (usage error)

    results: dict = {}
    verdict: dict = {}
    errors: list[str] = []
    skipped: list[str] = []

    def run(name: str, func, *fargs, **fkwargs):
        if args.verbose:
            print(f"[ smoketest ] {name}: probing ...", file=sys.stderr)
        raw = func(*fargs, **fkwargs)
        results[name] = raw
        return raw

    try:
        if not args.skip_usbip:
            run("usbip", probe_usbip_devlist, args.host, args.usbip_port,
                args.timeout, args.retries)
            if args.with_usbip_attach:
                run("usbip_import", probe_usbip_import, args.host, args.usbip_port,
                    args.timeout, args.retries, exp["busid"])
        else:
            skipped.append("usbip")

        if not args.skip_http:
            run("http", probe_http_index, args.host, args.http_port,
                args.timeout, args.retries)
        else:
            skipped.append("http")

        if not args.skip_ws:
            run("ws", probe_ws_json, args.host, args.http_port, "/ws",
                args.timeout, args.retries)
        else:
            skipped.append("ws")

        if args.with_ws_dap:
            run("ws_dap", probe_ws_dap, args.host, args.usbip_port,
                args.timeout, args.retries)
    except SmokeError as exc:
        errors.append(f"probe failed: {exc}")
    except (OSError, CheckTimeout) as exc:
        errors.append(f"probe failed: {exc}")

    if results:
        verdict, assess_errors = assess(results, exp)
        errors.extend(assess_errors)

    ok = not errors

    if args.json:
        payload = {
            "host": args.host,
            "ok": ok,
            "expectations": exp,
            "skipped": skipped,
            "verdict": verdict,
            "errors": errors,
        }
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print(f"WiFi Dap smoke test  host={args.host}")
        names = {
            "usbip": "USBIP devlist (3240)",
            "usbip_import": "USBIP import (3240)",
            "http": "HTTP UI (80)",
            "ws": "JSON-WS /ws (80)",
            "ws_dap": "WS-DAP transport (3240)",
        }
        for key, info in verdict.items():
            status = "PASS" if info["ok"] else "FAIL"
            detail = ""
            if key == "usbip" and info.get("devices"):
                dev = info["devices"][0]
                detail = f"  VID=0x{dev['vid']:04X} PID=0x{dev['pid']:04X} busid={dev['busid']}"
            elif key == "usbip_import" and info.get("device"):
                dev = info["device"]
                detail = f"  busid={dev['busid']}"
            elif key == "http":
                detail = f"  {info['status']} {info['content_encoding']} {info['bytes_decoded']}B"
            elif key == "ws":
                detail = f"  protocol_version={info.get('protocol_version')}"
            print(f"  [{status}] {names.get(key, key)}{detail}")
        for item in skipped:
            print(f"  [SKIP] {item}")
        for err in errors:
            print(f"  [FAIL] {err}")
        print("RESULT:", "PASS" if ok else "FAIL")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
