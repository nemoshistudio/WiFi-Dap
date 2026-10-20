#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""ota_upload.py -- upload a WiFi Dap app image to a real device.

Pure Python standard library (no pip packages). Two modes:

  upload :  POST http://<host>/api/ota/upload  (Content-Type application/octet-stream,
            Content-Length, optional X-SHA256, optional Authorization: Bearer)
  --info :  connect to ws://<host>/ws and query OTA module 5 GET_INFO
            ({"module":5,"cmd":1}) to print the running slot/version.

Contract: docs/ota-local-upload.md §3/§4, docs/protocol.md §2.5, and the
hardware checklist docs/hardware-test-ota.md (Wave3).

Examples:
  # upload (SHA-256 is computed from the file and sent as X-SHA256 by default)
  python3 scripts/ota_upload.py --host 192.168.1.1 --file build/esp32s3/birdiebot_wifi_dap.bin

  # upload, then poll WS GET_INFO while the device reboots (verify new slot/version)
  python3 scripts/ota_upload.py --host 192.168.1.1 --file build/esp32s3/birdiebot_wifi_dap.bin --info

  # info only (running slot/version/status)
  python3 scripts/ota_upload.py --host 192.168.1.1 --info

  # auth enabled (BIRDIEBOT_WEB_AUTH=y): same pair code as the Web UI token
  python3 scripts/ota_upload.py --host 192.168.1.1 --file fw.bin --token 12345678

  # error injection: send a deliberately wrong X-SHA256 (device must answer 422)
  python3 scripts/ota_upload.py --host 192.168.1.1 --file fw.bin \
      --sha256 0000000000000000000000000000000000000000000000000000000000000000

Exit codes:
  0  upload succeeded (HTTP 200 + JSON ok) / info query succeeded
  2  usage or local input error (bad args, missing/short file, bad SHA format)
  3  network/HTTP/WS failure (timeout, connection error, non-200, device error)

The script only talks to the device over plain HTTP/WS; it never builds,
flashes or reboots the device itself (the device reboots ~1 s after a
successful upload by contract).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import os
import socket
import struct
import sys
import time
from urllib.parse import quote, urlparse

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_FAIL = 3

MIN_IMAGE_SIZE = 4096            # docs/ota-local-upload.md §3.1
MAX_SLOT_SIZE = 3 * 1024 * 1024  # 8MB dual-OTA slot (0x300000)
UPLOAD_PATH = "/api/ota/upload"
WS_PATH = "/ws"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CHUNK_SIZE = 64 * 1024
DEFAULT_TIMEOUT = 300.0          # upload session cap is 300 s by contract


def eprint(*args, **kwargs) -> None:
    kwargs.setdefault("file", sys.stderr)
    print(*args, **kwargs)


def usage_error(message: str) -> None:
    eprint(f"ota_upload.py: error: {message}")
    raise SystemExit(EXIT_USAGE)


def parse_host_port(value: str) -> tuple[str, int]:
    """Accept '192.168.1.1', 'host:8080', 'http://host[:port]'."""
    text = value.strip()
    if not text:
        usage_error("--host is empty")
    if "://" in text:
        parsed = urlparse(text)
        if not parsed.hostname:
            usage_error(f"invalid --host URL: {value!r}")
        return parsed.hostname, parsed.port or 80
    if text.count(":") == 1:
        name, port_text = text.split(":", 1)
        try:
            return name, int(port_text)
        except ValueError:
            usage_error(f"invalid port in --host: {value!r}")
    return text, 80


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_sha256_hex(value: str) -> bool:
    if len(value) != 64:
        return False
    return all(c in "0123456789abcdefABCDEF" for c in value)


def parse_app_image(path: str) -> str | None:
    """Return esp_app_desc.version for a valid ESP-IDF app image, else None."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(0x100)
    except OSError as exc:
        usage_error(f"cannot read --file: {exc}")
    if len(head) < 0x50 or head[0] != 0xE9:
        return None
    if struct.unpack_from("<I", head, 0x20)[0] != 0xABCD5432:
        return None
    return head[0x30:0x50].split(b"\x00", 1)[0].decode("ascii", "replace")


def pretty_json(data) -> str:
    try:
        return json.dumps(data, indent=2, ensure_ascii=False, sort_keys=False)
    except (TypeError, ValueError):
        return str(data)


def print_info(info: dict) -> None:
    print(pretty_json(info))
    if info.get("err"):
        eprint(f"device error: {info.get('err')} (code={info.get('code')})")


# ---------------------------------------------------------------------------
# Minimal WebSocket client (text frames only; used for OTA GET_INFO / ABORT)
# ---------------------------------------------------------------------------

class WsClient:
    def __init__(self, host: str, port: int, timeout: float, token: str | None = None):
        path = WS_PATH + (f"?token={quote(token)}" if token else "")
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self._buf = b""
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            f"Origin: http://{host}:{port}\r\n"
            "\r\n"
        )
        self.sock.sendall(request.encode("ascii"))
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("connection closed during WS handshake")
            header += chunk
            if len(header) > 16384:
                raise ConnectionError("WS handshake header too large")
        head, rest = header.split(b"\r\n\r\n", 1)
        self._buf = rest
        lines = head.decode("latin-1").split("\r\n")
        if not lines or " 101 " not in lines[0]:
            raise ConnectionError(f"WS handshake failed: {lines[0] if lines else '?'}")
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
        ).decode("ascii")
        accept = ""
        for line in lines[1:]:
            if line.lower().startswith("sec-websocket-accept:"):
                accept = line.split(":", 1)[1].strip()
        if accept != expected:
            raise ConnectionError("WS handshake failed: bad Sec-WebSocket-Accept")

    def close(self) -> None:
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass

    def _read_exact(self, count: int) -> bytes:
        while len(self._buf) < count:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("WS connection closed")
            self._buf += chunk
        data, self._buf = self._buf[:count], self._buf[count:]
        return data

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        mask = os.urandom(4)
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
        header += mask
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def send_json(self, obj: dict) -> None:
        self._send_frame(0x1, json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def recv_json(self, timeout: float) -> dict:
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for a WS text message")
            self.sock.settimeout(remaining)
            b1, b2 = self._read_exact(2)
            opcode = b1 & 0x0F
            masked = bool(b2 & 0x80)
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read_exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else b""
            payload = self._read_exact(length)
            if masked:
                payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))

            if opcode == 0x1:  # text
                text = payload.decode("utf-8", "replace")
                if not text.strip():
                    continue  # firmware heartbeat: empty TEXT frame
                try:
                    return json.loads(text)
                except json.JSONDecodeError:
                    eprint(f"warning: ignoring non-JSON WS text: {text[:200]}")
                    continue
            if opcode == 0x9:  # ping -> pong
                self._send_frame(0xA, payload)
                continue
            if opcode == 0x8:  # close
                raise ConnectionError("WS closed by device")
            # binary / pong frames are not used by OTA; ignore.

    def request(self, obj: dict, timeout: float) -> dict:
        self.send_json(obj)
        return self.recv_json(timeout)


def ws_get_info(host: str, port: int, token: str | None, timeout: float) -> dict:
    client = WsClient(host, port, timeout, token)
    try:
        if token:
            # Query ?token= is the primary auth mechanism; the AUTH first frame
            # is also accepted by the firmware, so send it and ignore its reply.
            try:
                client.request({"module": 0, "cmd": 0, "token": token}, timeout=3.0)
            except (TimeoutError, ConnectionError, OSError):
                pass
        reply = client.request({"module": 5, "cmd": 1}, timeout=5.0)
    finally:
        client.close()
    if reply.get("module") != 5 or reply.get("cmd") != 1:
        raise ConnectionError(f"unexpected WS reply to OTA/GET_INFO: {reply}")
    return reply


def fetch_info_with_retry(host: str, port: int, token: str | None,
                          timeout: float, wait_seconds: float) -> dict:
    deadline = time.time() + wait_seconds
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            return ws_get_info(host, port, token, timeout)
        except (OSError, ConnectionError, TimeoutError, ValueError) as exc:
            last_error = exc
            time.sleep(1.0)
    raise TimeoutError(f"GET_INFO not reachable during reboot wait: {last_error}")


# ---------------------------------------------------------------------------
# upload mode
# ---------------------------------------------------------------------------

def progress_line(sent: int, total: int) -> None:
    percent = (sent * 100.0 / total) if total else 100.0
    eprint(f"\r  upload: {percent:5.1f}%  {sent}/{total} bytes", end="", flush=True)


def run_upload(args: argparse.Namespace, host: str, port: int) -> int:
    path = args.file
    if not os.path.isfile(path):
        usage_error(f"--file not found: {path}")

    size = os.path.getsize(path)
    if size < MIN_IMAGE_SIZE:
        usage_error(
            f"--file is {size} bytes, below the {MIN_IMAGE_SIZE}-byte minimum "
            "(docs/ota-local-upload.md §3.1; the device would answer 400)"
        )
    if size > MAX_SLOT_SIZE:
        eprint(
            f"warning: {size} bytes exceed the 3 MiB OTA slot; the device should "
            "reject it with HTTP 413 (useful for error-injection tests)"
        )

    image_version = parse_app_image(path)
    file_sha = sha256_file(path)
    if image_version is None:
        eprint("warning: file is not a valid ESP-IDF app image (expected magic 0xE9 "
               "and esp_app_desc magic 0xABCD5432); esp_ota_end() should answer 422")
    explicit_sha = args.sha256.strip() if args.sha256 else None
    if explicit_sha:
        if not valid_sha256_hex(explicit_sha):
            usage_error("--sha256 must be 64 hexadecimal characters")
        send_sha = explicit_sha
    elif args.no_sha256:
        send_sha = None
    else:
        send_sha = file_sha

    print("== WiFi Dap OTA upload ==")
    print(f"host         : {host}:{port}  (POST http://{host}:{port}{UPLOAD_PATH})")
    print(f"file         : {path}")
    print(f"size         : {size} bytes")
    print(f"image version: {image_version if image_version else 'n/a (not an app image)'}")
    print(f"file sha256  : {file_sha}")
    print(f"X-SHA256     : {send_sha if send_sha else '(not sent)'}")
    if send_sha and explicit_sha and send_sha.lower() != file_sha.lower():
        eprint("note: --sha256 differs from the file hash -> device should answer 422 "
               "(error-injection mode)")

    connection = http.client.HTTPConnection(host, port, timeout=args.timeout)
    try:
        connection.putrequest("POST", UPLOAD_PATH, skip_accept_encoding=True)
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(size))
        connection.putheader("Connection", "close")
        if send_sha:
            connection.putheader("X-SHA256", send_sha)
        if args.token:
            connection.putheader("Authorization", f"Bearer {args.token}")
        connection.endheaders()

        sent = 0
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(CHUNK_SIZE)
                if not chunk:
                    break
                connection.send(chunk)
                sent += len(chunk)
                progress_line(sent, size)
        eprint("")
        response = connection.getresponse()
        status = response.status
        body = response.read()
    except (OSError, http.client.HTTPException) as exc:
        eprint(f"\nota_upload.py: upload failed: {exc}")
        return EXIT_FAIL
    finally:
        connection.close()

    print(f"\nHTTP status  : {status} {response.reason}")
    content_type = response.getheader("Content-Type") or ""
    if content_type:
        print(f"Content-Type : {content_type}")
    text = body.decode("utf-8", "replace").strip()
    payload = None
    if text:
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            eprint(f"warning: response is not JSON: {text[:500]}")
    if payload is not None:
        print("response JSON:")
        print(pretty_json(payload))
    elif text:
        print(f"response body: {text[:1000]}")

    if status != 200:
        eprint(f"ota_upload.py: device rejected the upload with HTTP {status} "
               f"(see error mapping in docs/ota-local-upload.md §3.4)")
        return EXIT_FAIL
    if isinstance(payload, dict) and payload.get("ok") is False:
        eprint(f"ota_upload.py: device returned ok=false: {payload.get('err')}")
        return EXIT_FAIL

    device_sha = payload.get("sha256") if isinstance(payload, dict) else None
    if device_sha and device_sha.lower() != file_sha.lower():
        eprint("warning: device-reported sha256 differs from the file sha256 "
               f"(device={device_sha}, file={file_sha}); verify the upload log")

    print("\nupload accepted: the device sets the boot slot and reboots in ~1 s.")
    print("verify with: python3 scripts/ota_upload.py --host "
          f"{args.host} --info   (expect running_ver={image_version or '<new version>'})")

    if args.info:
        wait_seconds = max(args.info_wait, 0.0)
        print(f"\nwaiting up to {wait_seconds:.0f}s for the device to reboot and "
              "answer WS OTA/GET_INFO ...")
        try:
            info = fetch_info_with_retry(host, port, args.token, args.timeout, wait_seconds)
        except (TimeoutError, OSError, ConnectionError) as exc:
            eprint(f"warning: upload succeeded but GET_INFO after reboot failed: {exc}")
            return EXIT_OK
        print("GET_INFO after reboot:")
        print_info(info)
    return EXIT_OK


# ---------------------------------------------------------------------------
# info mode
# ---------------------------------------------------------------------------

def run_info(args: argparse.Namespace, host: str, port: int) -> int:
    print(f"== OTA GET_INFO via ws://{host}:{port}{WS_PATH} ==")
    try:
        info = ws_get_info(host, port, args.token, args.timeout)
    except (OSError, ConnectionError, TimeoutError, ValueError) as exc:
        eprint(f"ota_upload.py: WS OTA/GET_INFO failed: {exc}")
        return EXIT_FAIL
    print_info(info)
    status = info.get("status", "?")
    running = info.get("running", "?")
    version = info.get("running_ver", "?")
    print(f"\nrunning={running} running_ver={version} status={status}")
    if isinstance(info.get("free"), int) and isinstance(info.get("slot_size"), int):
        print(f"free={info['free']} slot_size={info['slot_size']} "
              f"rollback={info.get('rollback')}")
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ota_upload.py",
        description="Upload a WiFi Dap app image over HTTP and/or query "
                    "OTA module 5 GET_INFO over WebSocket (pure stdlib).",
        epilog="Contract: docs/ota-local-upload.md, docs/protocol.md §2.5; "
               "hardware checklist: docs/hardware-test-ota.md",
    )
    parser.add_argument("--host", required=True,
                        help="device address: IP, host:port, or http://host[:port]")
    parser.add_argument("--file", help="app .bin to upload (e.g. build/esp32s3/birdiebot_wifi_dap.bin)")
    parser.add_argument("--sha256",
                        help="X-SHA256 header value (64 hex); default = computed file hash, "
                             "use a wrong value to test the 422 path")
    parser.add_argument("--no-sha256", action="store_true",
                        help="do not send X-SHA256 (device still verifies esp_ota_end)")
    parser.add_argument("--token", help="pair code for Authorization: Bearer / WS ?token= "
                                        "(needed when BIRDIEBOT_WEB_AUTH=y)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"socket/upload timeout in seconds (default {DEFAULT_TIMEOUT:.0f})")
    parser.add_argument("--info", action="store_true",
                        help="WS OTA/GET_INFO: info-only mode, or poll after an upload")
    parser.add_argument("--info-wait", type=float, default=20.0,
                        help="seconds to retry GET_INFO after an upload reboot (default 20)")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if not args.file and not args.info:
        parser.error("nothing to do: pass --file to upload and/or --info to query GET_INFO")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.file and args.file.lower().endswith((".zip", ".tar.gz")):
        parser.error("--file must be the app .bin (build/esp32s3/birdiebot_wifi_dap.bin), "
                     "not an archive")

    host, port = parse_host_port(args.host)
    if args.file:
        return run_upload(args, host, port)
    return run_info(args, host, port)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        eprint("\nota_upload.py: interrupted")
        sys.exit(EXIT_FAIL)
    except SystemExit:
        raise
    except Exception as exc:  # noqa: BLE001 - map unexpected failures to exit 3
        eprint(f"ota_upload.py: unexpected error: {exc}")
        sys.exit(EXIT_FAIL)
