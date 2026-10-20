#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
WiFi Dap — WebSocket E2E test client (BF-201 / hardware bring-up).

Pure Python standard library WebSocket (RFC 6455) client for the device JSON /
binary protocol (docs/protocol.md, docs/ws-binary-router.md).  No third-party
packages (websocket-client etc.) are required, so it can be copied to a test PC
or CI runner as a single file.

Connection
    ws://<host>[:<port>]/ws            (default port 80, path /ws)

Scenarios (subcommands)
    ping            WS PING/PONG transport check + SYSTEM(0)/GET_FM_INFO(1):
                    prints fm_ver / upd_date / protocol_version.
    wifi-scan       WIFI(1)/SCAN(3) (async): prints the scan_list entries.
    uart-echo       UART(4) binary RAW frame -> module=4/sub_mod=0; waits for the
                    RAW_BROADCAST echo.  Requires a TX<->RX loopback jumper on
                    the target UART (see docs/hardware-test-uart-dataflow.md).
    dataflow-list   DATA_FLOW(2)/GET_INS_LIST(1): prints instances; asserts the
                    UART instance (mod_type=130).
    ota-info        OTA(5)/GET_INFO(1): prints running slot / version / free.
    json            Generic: send any {"module":m,"cmd":c,...} and print the
                    matching response (used by the hardware test checklist for
                    UART configuration commands).

Exit codes
    0  all checks of the scenario passed
    2  connection/handshake failed (or invalid command line usage)
    3  an assertion/timeout failed (device answered an error / no echo / ...)

Examples
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 ping
    python3 scripts/e2e_ws_client.py --host dap.local wifi-scan --wait 20
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 uart-echo --payload 48656c6c6f --hex
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 uart-echo --listen-only
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 dataflow-list
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 ota-info
    python3 scripts/e2e_ws_client.py --host 192.168.1.50 json --module 4 --cmd 6 --body '{"sub_mod":0}'

The output is plain text and can be pasted directly into a test record.
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

# ---------------------------------------------------------------------------
# WebSocket protocol constants (RFC 6455)
# ---------------------------------------------------------------------------
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_VERSION = 13

OP_CONT = 0x0
OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA

# WiFi Dap wire header (docs/protocol.md §3)
BIN_HDR_LEN = 4
DATA_TYPE_RAW = 0x20
DATA_TYPE_RAW_BROADCAST = 0x04
MODULE_SYSTEM = 0
MODULE_WIFI = 1
MODULE_DATA_FLOW = 2
MODULE_UART = 4
MODULE_OTA = 5
UART_SUB_MOD = 0

# Exit codes (task contract)
EXIT_OK = 0
EXIT_CONNECT = 2
EXIT_ASSERT = 3

MAX_FRAME = 1 << 20          # sanity cap for inbound frames
MAX_HTTP_HEADER = 64 * 1024  # sanity cap for the handshake response


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------
class WsConnectError(Exception):
    """TCP connect / HTTP upgrade failed (exit code 2)."""


class WsAssertError(Exception):
    """An expected condition was not met (exit code 3)."""


class WsClosed(Exception):
    """The server closed the WebSocket connection."""


# ---------------------------------------------------------------------------
# Logging helpers (plain text, pasteable into a test record)
# ---------------------------------------------------------------------------
class Log:
    verbose = False

    @staticmethod
    def _stamp():
        return time.strftime("%H:%M:%S")

    @classmethod
    def info(cls, msg):
        print(f"[{cls._stamp()}] [INFO] {msg}")

    @classmethod
    def debug(cls, msg):
        if cls.verbose:
            print(f"[{cls._stamp()}] [DBG ] {msg}")

    @classmethod
    def tx(cls, msg):
        print(f"[{cls._stamp()}] [TX  ] {msg}")

    @classmethod
    def rx(cls, msg):
        print(f"[{cls._stamp()}] [RX  ] {msg}")

    @classmethod
    def warn(cls, msg):
        print(f"[{cls._stamp()}] [WARN] {msg}")

    @classmethod
    def fail(cls, msg):
        print(f"[{cls._stamp()}] [FAIL] {msg}")

    @classmethod
    def ok(cls, msg):
        print(f"[{cls._stamp()}] [PASS] {msg}")


def fmt_json(obj):
    try:
        return json.dumps(obj, ensure_ascii=False, sort_keys=True)
    except (TypeError, ValueError):
        return repr(obj)


def hex_dump(data, limit=64):
    """Short hex dump for logs: '48 65 6c ...' (+ '... N bytes' when longer)."""
    if not data:
        return "<empty>"
    shown = data[:limit]
    text = " ".join(f"{b:02x}" for b in shown)
    if len(data) > limit:
        text += f" ... ({len(data)} bytes)"
    return text


# ---------------------------------------------------------------------------
# WebSocket client
# ---------------------------------------------------------------------------
class WsClient:
    def __init__(self, host, port, path, timeout, verbose):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.sock = None
        self._rx = b""
        Log.verbose = verbose

    # -- connect -----------------------------------------------------------
    def connect(self):
        endpoint = f"{self.host}:{self.port}{self.path}"
        Log.info(f"connecting ws://{endpoint} (timeout {self.timeout:.1f}s)")
        try:
            self.sock = socket.create_connection((self.host, self.port),
                                                 timeout=self.timeout)
        except OSError as exc:
            raise WsConnectError(f"TCP connect to {self.host}:{self.port} failed: {exc}")

        self.sock.settimeout(self.timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        path = self.path

        host_header = self.host
        if ":" in host_header and not host_header.startswith("["):
            host_header = f"[{host_header}]"  # IPv6 literal
        if self.port != 80:
            host_header += f":{self.port}"

        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_header}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: {WS_VERSION}\r\n"
            "User-Agent: birdiebot-e2e/1.0\r\n"
            "\r\n"
        )
        try:
            self.sock.sendall(request.encode("ascii"))
            header = self._read_http_header()
        except OSError as exc:
            raise WsConnectError(f"HTTP upgrade request failed: {exc}")

        status_line, headers = self._parse_http_header(header)
        Log.debug(f"handshake status: {status_line}")
        if " 101 " not in status_line + " ":
            raise WsConnectError(f"HTTP upgrade rejected: {status_line}")
        expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode("ascii")).digest()).decode("ascii")
        accept = headers.get("sec-websocket-accept", "")
        if accept != expected:
            raise WsConnectError(
                f"bad Sec-WebSocket-Accept header: got '{accept}', expected '{expected}'")
        Log.info("WebSocket handshake completed (HTTP 101)")

    def _read_http_header(self):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise WsConnectError("connection closed during HTTP handshake")
            data += chunk
            if len(data) > MAX_HTTP_HEADER:
                raise WsConnectError("HTTP handshake response too large")
        head, _, rest = data.partition(b"\r\n\r\n")
        self._rx = rest
        return head.decode("latin-1", errors="replace")

    @staticmethod
    def _parse_http_header(header):
        lines = header.split("\r\n")
        status_line = lines[0].strip()
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, _, value = line.partition(":")
                headers[name.strip().lower()] = value.strip()
        return status_line, headers

    # -- low level IO ------------------------------------------------------
    def _recv_exact(self, size, deadline):
        while len(self._rx) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("receive timeout")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(max(1024, size - len(self._rx)))
            except socket.timeout:
                raise TimeoutError("receive timeout")
            if not chunk:
                raise WsClosed("connection closed by device")
            self._rx += chunk
        data, self._rx = self._rx[:size], self._rx[size:]
        return data

    def send_frame(self, opcode, payload=b"", fin=True):
        if self.sock is None:
            raise WsConnectError("not connected")
        if payload is None:
            payload = b""
        header = bytearray()
        header.append((0x80 if fin else 0x00) | opcode)
        mask_bit = 0x80
        length = len(payload)
        if length < 126:
            header.append(mask_bit | length)
        elif length <= 0xFFFF:
            header.append(mask_bit | 126)
            header += struct.pack("!H", length)
        else:
            header.append(mask_bit | 127)
            header += struct.pack("!Q", length)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        try:
            self.sock.sendall(bytes(header) + masked)
        except OSError as exc:
            raise WsClosed(f"send failed: {exc}")

    def recv_frame(self, deadline):
        b1, b2 = self._recv_exact(2, deadline)
        fin = bool(b1 & 0x80)
        opcode = b1 & 0x0F
        masked = bool(b2 & 0x80)
        length = b2 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._recv_exact(2, deadline))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._recv_exact(8, deadline))
        if length > MAX_FRAME:
            raise WsAssertError(f"inbound frame too large: {length} bytes")
        mask = self._recv_exact(4, deadline) if masked else b""
        payload = self._recv_exact(length, deadline)
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return fin, opcode, payload

    def next_event(self, deadline):
        """Return ('text', str) | ('binary', bytes) | ('pong', bytes).

        Handles server PING (answers PONG), ignores empty heartbeat TEXT frames.
        Fragmented data frames are reassembled (the firmware never fragments,
        the client tolerates it for robustness).
        """
        fragments = []
        fragment_opcode = None
        while True:
            fin, opcode, payload = self.recv_frame(deadline)
            if opcode == OP_PING:
                Log.debug(f"WS PING from device ({len(payload)} bytes), replying PONG")
                self.send_frame(OP_PONG, payload)
                continue
            if opcode == OP_PONG:
                return ("pong", payload)
            if opcode == OP_CLOSE:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 0
                raise WsClosed(f"device sent CLOSE (code {code})")
            if opcode in (OP_TEXT, OP_BINARY):
                if fin:
                    return self._decode_data(opcode, payload)
                fragments = [payload]
                fragment_opcode = opcode
                Log.debug("fragmented frame started; waiting for continuation")
                continue
            if opcode == OP_CONT:
                fragments.append(payload)
                if fin:
                    data = b"".join(fragments)
                    fragments = []
                    return self._decode_data(fragment_opcode, data)
                continue
            Log.warn(f"ignoring unknown WS opcode 0x{opcode:x}")

    @staticmethod
    def _decode_data(opcode, payload):
        if opcode == OP_TEXT:
            text = payload.decode("utf-8", errors="replace")
            return ("text", text)
        return ("binary", payload)

    # -- protocol helpers --------------------------------------------------
    def send_json(self, obj):
        Log.tx(f"JSON {fmt_json(obj)}")
        self.send_frame(OP_TEXT, json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def send_binary(self, data):
        Log.tx(f"BIN  {len(data)} bytes: {hex_dump(data)}")
        self.send_frame(OP_BINARY, data)

    def request(self, module, cmd, extra=None, timeout=None):
        """Send {"module":m,"cmd":c,...} and return the matching JSON response."""
        payload = {"module": module, "cmd": cmd}
        if extra:
            payload.update(extra)
        self.send_json(payload)
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while True:
            try:
                kind, data = self.next_event(deadline)
            except TimeoutError:
                raise WsAssertError(
                    f"timeout waiting for response module={module} cmd={cmd} "
                    f"({timeout if timeout is not None else self.timeout:.1f}s)")
            if kind == "pong":
                Log.debug(f"unexpected PONG ({len(data)} bytes)")
                continue
            if kind == "binary":
                Log.debug(f"ignoring binary frame while waiting for JSON: {hex_dump(data)}")
                continue
            # kind == "text"
            if data == "":
                continue  # 1500 ms heartbeat: empty TEXT frame (docs/protocol.md §1)
            try:
                obj = json.loads(data)
            except ValueError:
                Log.warn(f"non-JSON text frame: {data!r}")
                continue
            if not isinstance(obj, dict):
                continue
            if "module" in obj and "cmd" in obj:
                if obj.get("module") == module and obj.get("cmd") == cmd:
                    Log.rx(f"JSON {fmt_json(obj)}")
                    return obj
                Log.debug(f"ignoring response for module={obj.get('module')} cmd={obj.get('cmd')}")
                continue
            if "err" in obj or "error" in obj:
                raise WsAssertError(f"device error response: {fmt_json(obj)}")
            Log.debug(f"ignoring JSON without module/cmd: {fmt_json(obj)}")

    def close(self):
        if self.sock is None:
            return
        try:
            self.send_frame(OP_CLOSE, struct.pack("!H", 1000))
        except (WsClosed, WsConnectError, OSError):
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock = None


# ---------------------------------------------------------------------------
# Endpoint parsing / CLI helpers
# ---------------------------------------------------------------------------
def parse_host_arg(host_arg, port_arg, path_arg):
    """Accept 'ip', 'ip:port', 'ws://ip:port/path', 'http://ip/ws'."""
    host = host_arg.strip()
    path = path_arg
    port = port_arg

    for scheme in ("ws://", "wss://", "http://", "https://"):
        if host.startswith(scheme):
            host = host[len(scheme):]
            break
    if "/" in host:
        host, _, rest = host.partition("/")
        if path is None:
            path = "/" + rest
        else:
            path = "/" + rest if path in ("", "/ws") else path
    if ":" in host and not host.startswith("["):
        maybe_host, _, maybe_port = host.rpartition(":")
        if maybe_port.isdigit():
            host, port = maybe_host, int(maybe_port)
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]
    if path is None or path == "":
        path = "/ws"
    if not path.startswith("/"):
        path = "/" + path
    if port is None:
        port = 80
    return host, port, path


def make_client(args):
    host, port, path = parse_host_arg(args.host, args.port, args.path)
    return WsClient(host=host, port=port, path=path,
                    timeout=args.timeout, verbose=args.verbose)


def print_header(args, scenario, elapsed=None, result=None):
    print("=" * 64)
    print("WiFi Dap — WS E2E test")
    print(f"scenario : {scenario}")
    host, port, path = parse_host_arg(args.host, args.port, args.path)
    print(f"target   : ws://{host}:{port}{path}")
    print(f"started  : {time.strftime('%Y-%m-%d %H:%M:%S')}")
    if elapsed is not None:
        print(f"elapsed  : {elapsed:.2f} s")
    if result is not None:
        print(f"result   : {result}")
    print("=" * 64)


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------
def scenario_ping(client, args):
    """WS PING/PONG transport check, then SYSTEM/GET_FM_INFO."""
    payload = b"birdiebot-e2e"
    Log.tx(f"WS PING payload={payload.decode()}")
    client.send_frame(OP_PING, payload)

    deadline = time.monotonic() + min(client.timeout, 3.0)
    try:
        while True:
            kind, data = client.next_event(deadline)
            if kind == "pong":
                if data == payload:
                    Log.ok(f"WS PONG received (payload echoed, {len(data)} bytes)")
                else:
                    Log.warn(f"WS PONG payload mismatch: {data!r}")
                break
            Log.debug(f"ignoring {kind} frame before PONG")
    except TimeoutError:
        Log.warn("no WS PONG within timeout (transport check skipped, continuing)")

    resp = client.request(MODULE_SYSTEM, 1)  # SYSTEM/GET_FM_INFO
    fm_ver = resp.get("fm_ver")
    upd_date = resp.get("upd_date")
    protocol_version = resp.get("protocol_version")
    print(f"         fm_ver           : {fm_ver!r}")
    print(f"         upd_date         : {upd_date!r}")
    print(f"         protocol_version : {protocol_version!r}")
    if not fm_ver:
        raise WsAssertError("GET_FM_INFO response has no fm_ver field")
    if protocol_version is None:
        Log.warn("protocol_version missing (firmware older than docs/versioning.md v1)")
    elif protocol_version != 1:
        Log.warn(f"protocol_version={protocol_version!r} differs from client expectation (1)")
    Log.ok(f"SYSTEM/GET_FM_INFO ok: fm_ver={fm_ver} protocol_version={protocol_version}")
    return {"fm_ver": fm_ver, "protocol_version": protocol_version}


def scenario_wifi_scan(client, args):
    """WIFI/SCAN (async)."""
    wait = args.wait if args.wait is not None else max(client.timeout, 20.0)
    Log.info(f"WIFI/SCAN request (async, waiting up to {wait:.1f}s)")
    resp = client.request(MODULE_WIFI, 3, timeout=wait)
    scan_list = resp.get("scan_list")
    if not isinstance(scan_list, list):
        raise WsAssertError(f"SCAN response has no scan_list array: {fmt_json(resp)}")
    print(f"         networks         : {len(scan_list)}")
    for idx, ap in enumerate(scan_list[: args.max_results]):
        if isinstance(ap, dict):
            print(f"         [{idx:02d}] ssid={ap.get('ssid')!r} rssi={ap.get('rssi')} "
                  f"mac={ap.get('mac')}")
    Log.ok(f"WIFI/SCAN ok: {len(scan_list)} network(s)")
    return {"networks": len(scan_list)}


def _parse_uart_payload(args):
    if args.hex:
        text = args.payload.replace(" ", "").replace("0x", "").replace(",", "")
        if len(text) % 2 != 0:
            raise WsAssertError(f"--hex payload must have an even number of digits: {args.payload!r}")
        try:
            return bytes.fromhex(text)
        except ValueError as exc:
            raise WsAssertError(f"invalid hex payload {args.payload!r}: {exc}")
    return args.payload.encode("utf-8")


def scenario_uart_echo(client, args):
    """Binary RAW -> UART(4), wait for the RAW_BROADCAST loopback echo."""
    payload = _parse_uart_payload(args)
    wait = args.wait if args.wait is not None else max(client.timeout, 3.0)
    header = bytes([DATA_TYPE_RAW, MODULE_UART, UART_SUB_MOD, 0])

    if not args.listen_only:
        frame = header + payload
        Log.info(f"sending {len(payload)} payload bytes to UART(module=4, sub_mod={UART_SUB_MOD}): "
                 f"{hex_dump(payload)}")
        client.send_binary(frame)
    else:
        Log.info("listen-only mode: not sending, printing RAW_BROADCAST frames")

    if args.listen_only:
        expect = None
    else:
        expect = payload

    received = []
    deadline = time.monotonic() + wait
    errors = []
    while True:
        try:
            kind, data = client.next_event(deadline)
        except TimeoutError:
            break
        if kind == "pong":
            continue
        if kind == "text":
            if data == "":
                continue
            try:
                obj = json.loads(data)
            except ValueError:
                Log.warn(f"non-JSON text frame: {data!r}")
                continue
            if isinstance(obj, dict) and ("err" in obj or "error" in obj):
                errors.append(obj)
                Log.warn(f"device error while waiting for UART broadcast: {fmt_json(obj)}")
            else:
                Log.debug(f"ignoring JSON frame: {fmt_json(obj)}")
            continue
        # binary frame: 4B header + payload
        if len(data) < BIN_HDR_LEN:
            Log.warn(f"binary frame shorter than 4-byte header: {hex_dump(data)}")
            continue
        data_type, module, sub_mod, _reserved = data[0], data[1], data[2], data[3]
        body = data[BIN_HDR_LEN:]
        if module == MODULE_UART and data_type == DATA_TYPE_RAW_BROADCAST:
            received.append((sub_mod, body))
            print(f"         RX broadcast     : module=4 sub_mod={sub_mod} "
                  f"len={len(body)} data={hex_dump(body)}")
            if expect is not None and body == expect:
                Log.ok(f"UART loopback echo matched ({len(body)} bytes)")
                break
        else:
            Log.debug(f"ignoring binary frame data_type=0x{data_type:02x} "
                      f"module={module} sub_mod={sub_mod} len={len(body)}")

    if args.listen_only:
        if received:
            Log.ok(f"received {len(received)} UART RAW_BROADCAST frame(s)")
            return {"frames": len(received)}
        raise WsAssertError(f"no UART RAW_BROADCAST frame within {wait:.1f}s")

    if any(body == expect for _sub, body in received):
        return {"echo_len": len(expect)}
    if received:
        details = ", ".join(f"sub_mod={s} len={len(b)} data={hex_dump(b)}" for s, b in received)
        raise WsAssertError(
            f"UART echo mismatch (sent {hex_dump(expect)}; received: {details}) — "
            "check the TX<->RX loopback jumper and baud rate")
    if errors:
        raise WsAssertError(f"UART echo not received; device reported {fmt_json(errors[-1])}")
    raise WsAssertError(
        f"UART echo not received within {wait:.1f}s — check the TX<->RX loopback jumper, "
        "baud rate and that the UART service is initialized")


def scenario_dataflow_list(client, args):
    """DATA_FLOW/GET_INS_LIST: assert the UART instance (mod_type=130)."""
    resp = client.request(MODULE_DATA_FLOW, 1)
    instances = resp.get("instances")
    if not isinstance(instances, list) or not instances:
        raise WsAssertError(f"GET_INS_LIST response has no instances: {fmt_json(resp)}")
    uart_found = False
    for ins in instances:
        if not isinstance(ins, dict):
            continue
        port_info = ins.get("port_info") or {}
        periph = port_info.get("periph_num") if isinstance(port_info, dict) else None
        print(f"         instance         : ins_idx={ins.get('ins_idx')} mod_idx={ins.get('mod_idx')} "
              f"mod_type={ins.get('mod_type')} periph_num={periph}")
        if ins.get("mod_type") == 0x82:  # 130 = UART (docs/protocol.md §2.3)
            uart_found = True
    if not uart_found:
        raise WsAssertError("no UART instance (mod_type=130) in GET_INS_LIST")
    Log.ok(f"DATA_FLOW/GET_INS_LIST ok: {len(instances)} instance(s), UART present")
    return {"instances": len(instances)}


def scenario_ota_info(client, args):
    """OTA/GET_INFO: running slot / version / free space."""
    resp = client.request(MODULE_OTA, 1)
    if "status" not in resp and "running" not in resp:
        raise WsAssertError(f"OTA/GET_INFO response lacks status/running: {fmt_json(resp)}")
    for key in ("running", "running_ver", "free", "slot_size", "status", "rollback"):
        if key in resp:
            print(f"         {key:<16} : {resp.get(key)}")
    Log.ok(f"OTA/GET_INFO ok: running={resp.get('running')} status={resp.get('status')}")
    return resp


def scenario_json(client, args):
    """Generic JSON request (used by the hardware checklist for e.g. UART config)."""
    body = {}
    if args.body:
        try:
            body = json.loads(args.body)
        except ValueError as exc:
            raise WsAssertError(f"--body is not valid JSON: {exc}")
        if not isinstance(body, dict):
            raise WsAssertError("--body must be a JSON object")
    resp = client.request(args.module, args.cmd, extra=body)
    print(f"         response         : {fmt_json(resp)}")
    Log.ok(f"JSON module={args.module} cmd={args.cmd} ok")
    return resp


SCENARIOS = {
    "ping": scenario_ping,
    "wifi-scan": scenario_wifi_scan,
    "uart-echo": scenario_uart_echo,
    "dataflow-list": scenario_dataflow_list,
    "ota-info": scenario_ota_info,
    "json": scenario_json,
}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def add_common_options(parser, suppress_defaults=False):
    # Not "required" here: --host may be given before or after the subcommand
    # (validated explicitly in main()).
    parser.add_argument("--host", metavar="HOST", required=False,
                        default=argparse.SUPPRESS if suppress_defaults else None,
                        help="device IP/host name, optionally with scheme/port/path")
    parser.add_argument("--port", type=int, metavar="PORT",
                        default=argparse.SUPPRESS if suppress_defaults else 80,
                        help="HTTP/WS port (default: 80)")
    parser.add_argument("--path", metavar="PATH",
                        default=argparse.SUPPRESS if suppress_defaults else "/ws",
                        help="WebSocket path (default: /ws)")
    parser.add_argument("--timeout", type=float, metavar="SEC",
                        default=argparse.SUPPRESS if suppress_defaults else 5.0,
                        help="default response timeout in seconds (default: 5)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        default=argparse.SUPPRESS if suppress_defaults else False,
                        help="print frame-level debug information")


def build_parser():
    parser = argparse.ArgumentParser(
        prog="e2e_ws_client.py",
        description="WiFi Dap WebSocket E2E test client (pure Python stdlib).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "exit codes: 0 success | 2 connect/handshake failure (or usage) | "
            "3 assertion/timeout failure\n"
            "scenarios: ping, wifi-scan, uart-echo, dataflow-list, ota-info, json\n"
            "loopback test: connect target UART TX to RX (board: TX=GPIO10, RX=GPIO9 on "
            "ESP32-S3 default board); see docs/hardware-test-uart-dataflow.md"
        ),
    )
    add_common_options(parser)

    sub = parser.add_subparsers(dest="scenario", metavar="SCENARIO")
    sub.required = True

    def add(name, help_text):
        sp = sub.add_parser(name, help=help_text,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
        add_common_options(sp, suppress_defaults=True)
        return sp

    add("ping", "WS PING/PONG + SYSTEM/GET_FM_INFO (version/protocol_version)")

    p = add("wifi-scan", "WIFI/SCAN (async) and print the scan list")
    p.add_argument("--wait", type=float, metavar="SEC", default=None,
                   help="scan timeout in seconds (default: max(timeout, 20))")
    p.add_argument("--max-results", type=int, metavar="N", default=10,
                   help="print at most N networks (default: 10)")

    p = add("uart-echo", "binary RAW to UART(4) and wait for the loopback RAW_BROADCAST")
    p.add_argument("--payload", metavar="PAYLOAD", default="0123",
                   help="data to send: text (default) or hex bytes with --hex")
    p.add_argument("--hex", action="store_true",
                   help="interpret --payload as hex bytes (e.g. 48656c6c6f)")
    p.add_argument("--wait", type=float, metavar="SEC", default=None,
                   help="echo wait timeout in seconds (default: max(timeout, 3))")
    p.add_argument("--listen-only", action="store_true",
                   help="do not send; only print incoming UART RAW_BROADCAST frames "
                        "(use for the second client in the two-client test)")

    add("dataflow-list", "DATA_FLOW/GET_INS_LIST and assert the UART instance")

    add("ota-info", "OTA/GET_INFO (running slot / version / free space)")

    p = add("json", "generic JSON request: --module/--cmd/--body")
    p.add_argument("--module", type=int, required=True, metavar="M",
                   help="JSON module id (0=SYSTEM, 1=WIFI, 2=DATA_FLOW, 4=UART, 5=OTA)")
    p.add_argument("--cmd", type=int, required=True, metavar="C", help="command id")
    p.add_argument("--body", metavar="JSON", default=None,
                   help="extra request fields as a JSON object, e.g. '{\"sub_mod\":0}'")

    return parser


def execute(args):
    client = make_client(args)
    started = time.monotonic()
    print_header(args, args.scenario)
    try:
        client.connect()
    except WsConnectError as exc:
        Log.fail(f"connection failed: {exc}")
        print_header(args, args.scenario, elapsed=time.monotonic() - started,
                     result="CONNECT FAILED")
        return EXIT_CONNECT

    try:
        result = SCENARIOS[args.scenario](client, args)
        elapsed = time.monotonic() - started
        Log.ok(f"{args.scenario}: PASS ({fmt_json(result) if result else 'ok'})")
        print_header(args, args.scenario, elapsed=elapsed, result="PASS")
        return EXIT_OK
    except WsAssertError as exc:
        elapsed = time.monotonic() - started
        Log.fail(f"{args.scenario}: {exc}")
        print_header(args, args.scenario, elapsed=elapsed, result="FAIL (assertion)")
        return EXIT_ASSERT
    except WsClosed as exc:
        elapsed = time.monotonic() - started
        Log.fail(f"connection closed by device: {exc}")
        print_header(args, args.scenario, elapsed=elapsed, result="FAIL (connection closed)")
        return EXIT_ASSERT
    finally:
        client.close()


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    # Subparsers use SUPPRESS defaults so that options given before the
    # subcommand survive; fill the defaults that were never provided.
    for name, value in (("port", 80), ("path", "/ws"),
                        ("timeout", 5.0), ("verbose", False), ("host", None)):
        if not hasattr(args, name):
            setattr(args, name, value)
    if not args.host:
        parser.error("--host is required")
    if args.timeout <= 0:
        parser.error("--timeout must be > 0")
    try:
        return execute(args)
    except KeyboardInterrupt:
        print("\n[INFO] interrupted by user", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
