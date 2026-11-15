#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CMSIS-DAP over TCP (port 3241) smoke test -- no target hardware needed.

Checks the M3 bridge end-to-end on a live probe:
  * frame framing ("DAP\\0" + u16 LE length + type + reserved)
  * DAP_Info(CAPABILITIES): one data byte for the v2.1-compliant build
  * DAP_Info(SER_NUM): "WiFi-Dap-<12HEX>" (unique identity)
  * DAP_Info(PACKET_SIZE / PACKET_COUNT) sanity
  * DAP_SWJ_Clock: request 20 MHz (M4 engine) and back to 2 MHz, probe still
    answers DAP_Info afterwards
  * DAP_Connect/DAP_Disconnect accepted (target not required for DAP_Info)
  * invalid frame closes the session without a response

Usage:
  python3 scripts/dap_tcp_smoke.py --host <device-ip> [--port 3241] [--clock 20000000]

Example (hardware-verified 2026-11-15 on the WiFi DAP S3):
  python3 scripts/dap_tcp_smoke.py --host 192.168.7.1        # USB-NCM link
  python3 scripts/dap_tcp_smoke.py --host <device-ip>     # Wi-Fi STA link
Exit: 0 all checks passed, 1 failed check, 2 transport error.
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys

HEADER = struct.Struct("<4sHBB")
TYPE_REQUEST = 1
TYPE_RESPONSE = 2
MAGIC = b"DAP\0"
DAP_INFO = 0x00
DAP_CONNECT = 0x02
DAP_DISCONNECT = 0x03
DAP_SWJ_CLOCK = 0x11
ID_CAPABILITIES = 0xF0  # CMSIS-DAP DAP_ID_CAPABILITIES
ID_SER_NUM = 0x03
ID_PACKET_SIZE = 0xFF  # CMSIS-DAP DAP_ID_PACKET_SIZE
ID_PACKET_COUNT = 0xFE
DAP_OK = 0x00

checks = 0
failures = 0


def check(cond: bool, name: str, detail: str = "") -> None:
    global checks, failures
    checks += 1
    if cond:
        print(f"ok {checks} - {name}")
    else:
        failures += 1
        print(f"not ok {checks} - {name}" + (f"  # {detail}" if detail else ""))


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf += chunk
    return buf


def send_frame(sock: socket.socket, payload: bytes, ftype: int = TYPE_REQUEST) -> None:
    sock.sendall(HEADER.pack(MAGIC, len(payload), ftype, 0) + payload)


def recv_frame(sock: socket.socket) -> tuple[int, bytes]:
    magic, length, ftype, _reserved = HEADER.unpack(recv_exact(sock, HEADER.size))
    payload = recv_exact(sock, length)
    if magic != MAGIC:
        raise ConnectionError(f"bad magic {magic!r}")
    return ftype, payload


def dap_cmd(sock: socket.socket, payload: bytes) -> bytes:
    send_frame(sock, payload)
    ftype, resp = recv_frame(sock)
    if ftype != TYPE_RESPONSE:
        raise ConnectionError(f"unexpected frame type {ftype}")
    if not resp or resp[0] != payload[0]:
        raise ConnectionError(f"bad response echo {resp[:2]!r}")
    return resp


def dap_info(sock: socket.socket, info_id: int) -> bytes:
    resp = dap_cmd(sock, bytes([DAP_INFO, info_id]))
    if len(resp) < 2:
        raise ConnectionError("short DAP_Info response")
    length = resp[1]
    data = resp[2 : 2 + length]
    if len(data) != length:
        raise ConnectionError("truncated DAP_Info data")
    return data


def dap_swj_clock(sock: socket.socket, hz: int) -> int:
    resp = dap_cmd(sock, bytes([DAP_SWJ_CLOCK]) + struct.pack("<I", hz))
    return resp[1] if len(resp) > 1 else 0xFF


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=3241)
    parser.add_argument("--clock", type=int, default=20_000_000)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as exc:
        print(f"FATAL: cannot connect to {args.host}:{args.port}: {exc}")
        return 2

    with sock:
        sock.settimeout(args.timeout)

        caps = dap_info(sock, ID_CAPABILITIES)
        check(len(caps) == 1, "DAP_Info(CAPABILITIES) is one byte (CMSIS-DAP v2.1)",
              f"got {len(caps)} bytes: {caps.hex()}")
        check(bool(caps and (caps[0] & 0x01)), "SWD capability bit set",
              f"caps=0x{caps[0]:02x}" if caps else "empty")

        serial = dap_info(sock, ID_SER_NUM).decode("ascii", "replace").rstrip("\x00")
        check(serial.startswith("WiFi-Dap-"),
              "DAP_Info(SER_NUM) is the unique WiFi Dap serial", serial)
        check(len(serial) == len("WiFi-Dap-") + 12,
              "serial carries the full 12-hex MAC", serial)

        packet_size = dap_info(sock, ID_PACKET_SIZE)
        size = struct.unpack("<H", packet_size[:2])[0] if len(packet_size) >= 2 else 0
        check(size == 1024, "DAP_Info(PACKET_SIZE) is 1024 in the TCP build", str(size))

        packet_count = dap_info(sock, ID_PACKET_COUNT)
        count = packet_count[0] if packet_count else 0
        check(count > 0, "DAP_Info(PACKET_COUNT) is non-zero", str(count))

        connect_resp = dap_cmd(sock, bytes([DAP_CONNECT, 0x01]))  # SWD
        # DAP_Connect response is [echo, connected port]: 1 = SWD.
        check(len(connect_resp) >= 2 and connect_resp[1] == 0x01,
              "DAP_Connect(SWD) accepted", connect_resp.hex())

        rc = dap_swj_clock(sock, args.clock)
        check(rc == DAP_OK, f"DAP_SWJ_Clock({args.clock}) accepted (M4 engine select)", hex(rc))
        serial_after = dap_info(sock, ID_SER_NUM).decode("ascii", "replace").rstrip("\x00")
        check(serial_after == serial,
              "probe still services DAP_Info after the speed change", serial_after)

        rc = dap_swj_clock(sock, 2_000_000)
        check(rc == DAP_OK, "DAP_SWJ_Clock(2 MHz) fallback accepted", hex(rc))

        disconnect_resp = dap_cmd(sock, bytes([DAP_DISCONNECT]))
        check(len(disconnect_resp) >= 2 and disconnect_resp[1] == DAP_OK,
              "DAP_Disconnect accepted", disconnect_resp.hex())

    # Invalid frame must terminate the session (no resynchronisation).
    try:
        bad = socket.create_connection((args.host, args.port), timeout=args.timeout)
        with bad:
            bad.settimeout(args.timeout)
            bad.sendall(b"BAD\0" + b"\x01\x00\x01\x00" + b"\x00")
            closed = False
            try:
                closed = bad.recv(1) == b""
            except (ConnectionError, socket.timeout, OSError):
                closed = True
            check(closed, "invalid frame closes the TCP session")
    except OSError as exc:
        check(False, "invalid frame closes the TCP session", str(exc))

    print(f"\n{checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
