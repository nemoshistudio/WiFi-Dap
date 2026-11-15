#!/usr/bin/env python3
"""scripts/hil_extended.py -- opt-in extended HIL smoke for the WiFi Dap.

This is the **opt-in** companion to ``scripts/hil_gate.py`` (the default,
strictly read-only regression/performance gate).  Every operation here is
demanded explicitly on the command line and falls into one of five modes:

    default / --dry-run   offline self-test with an in-process fake device
                          (never touches the network or the host)
    --read-only           device read-only pre-flight only (target/status/curve)
    --soak / --duration N repeated curve start-bin -> sample -> stop cycles;
                          heap / leak / sampler-error / WWi-Fi-delivery checks
    --flash-image PATH --yes-flash
                          Web Flash smoke: GET target/info (H7 + UID recorded),
                          POST /api/target/flash (application/octet-stream +
                          X-Target-SHA256), concurrent GET /api/target/status
                          polling, then target/info must still show the target
                          detected with the same UID
    --ota-image PATH --yes-ota
                          OTA smoke: POST /api/ota/upload (octet-stream +
                          X-SHA256), tolerate the reboot/disconnect, then poll
                          the device + OTA(5)/GET_INFO until the new slot boots;
                          a failed upload/interrupt is reported, never retried
                          blindly
    --usbip               optional host USBIP attach/detach + one OpenOCD
                          CMSIS-DAP identify session; SKIPPED (not failed) when
                          usbip/OpenOCD/root helper are unavailable

Safety contract
---------------
* **No write happens by default.**  ``--flash-image`` without ``--yes-flash``
  and ``--ota-image`` without ``--yes-ota`` only run read-only image/target
  validation and record a ``dry-run`` check; nothing is uploaded.
* ``scripts/hil_gate.py`` is **not modified** and keeps its read-only default;
  this tool reuses its HTTP transport / JSON / CBS1 / CVF2 decoding helpers
  (``import hil_gate``) and adds the opt-in write paths on top.
* Every write mode prints and records the target UID + flash size + image
  size + SHA-256 *before* uploading anything.
* A failed Flash write triggers an explicit ``POST /api/target/reset {"run":
  true}`` recovery attempt and re-probes the target; a failed OTA is reported
  with the still-running slot/version and is not retried.
* ``curve/stop`` and USBIP detach are attempted from cleanup blocks even when a
  check fails, so the target/session are not left halted/attached.

Exit codes (same convention as ``scripts/hil_gate.py``):
    0  PASS   all required checks passed (warnings/skips allowed)
    1  FAIL   a required check failed
    2  usage / argument error
    3  device unreachable (network / HTTP)

Usage examples
--------------
    # default: offline dry-run self-test (no network, no writes)
    python3 scripts/hil_extended.py
    python3 scripts/hil_extended.py --dry-run --json

    # real device, read-only first
    python3 scripts/hil_extended.py --host <device-ip> --read-only --json
    python3 scripts/hil_extended.py --host <device-ip> --soak --duration 60 \
        --json --report /tmp/hil-ext/soak.json

    # opt-in writes (double switch required)
    python3 scripts/hil_extended.py --host <device-ip> \
        --flash-image /path/to/h7_test.bin --flash-expect-uid <target-uid> \
        --yes-flash --json --report /tmp/hil-ext/flash.json
    python3 scripts/hil_extended.py --host <device-ip> \
        --ota-image build/esp32s3/birdiebot_wifi_dap.bin \
        --yes-ota --json --report /tmp/hil-ext/ota.json

    # dry-run the write flow against the fake device (validates flags/report)
    python3 scripts/hil_extended.py --dry-run --flash-image build/esp32s3/birdiebot_wifi_dap.bin \
        --yes-flash --json

Docs / risks: ``docs/hil-extended.md``; gate contract: ``docs/hil-gate.md``.

Requires: Python >= 3.9, stdlib only.  USBIP/OpenOCD mode additionally needs
the host ``usbip`` tool, a built OpenOCD and a root helper (``HIL_ROOT_HELPER``,
``/tmp/hostroot.sh`` or passwordless sudo); otherwise that mode is SKIPPED.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
from dataclasses import dataclass, field
from datetime import datetime, timezone
from types import SimpleNamespace
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Reuse the read-only gate's proven HTTP / JSON / CBS1 / CVF2 helpers.
#
# scripts/hil_gate.py is owned by the default HIL-gate workstream; this file
# only imports it and never edits it (the default gate stays read-only).
# ---------------------------------------------------------------------------

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(_SCRIPT_DIR)
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

try:
    import hil_gate as hg  # noqa: E402  (path set up above)
except ImportError as exc:  # pragma: no cover - broken checkout
    print("[hil_extended] FATAL: cannot import scripts/hil_gate.py (%s); run "
          "from the repository scripts/ directory" % exc, file=sys.stderr)
    raise SystemExit(2)

_REQUIRED_HIL_GATE = (
    "HttpTransport",
    "Response",
    "DeviceUnreachable",
    "ProtocolError",
    "decode_curve_frames",
    "FrameDecodeError",
    "_cbs1_body",
    "TARGET_INFO_PATH",
    "TARGET_STATUS_PATH",
    "CURVE_START_PATH",
    "CURVE_START_BIN_PATH",
    "CURVE_STOP_PATH",
    "CURVE_STATUS_PATH",
    "CURVE_FRAMES_PATH",
    "CVF2_MAGIC",
    "CVF2_VERSION",
    "CVF_FLAG_PC",
)
_missing = [name for name in _REQUIRED_HIL_GATE if not hasattr(hg, name)]
if _missing:  # pragma: no cover - incompatible hil_gate.py
    print("[hil_extended] FATAL: scripts/hil_gate.py is missing expected "
          "helpers: %s (read-only gate API changed; update hil_extended.py)"
          % ", ".join(_missing), file=sys.stderr)
    raise SystemExit(2)

HttpTransport = hg.HttpTransport
Response = hg.Response
DeviceUnreachable = hg.DeviceUnreachable
ProtocolError = hg.ProtocolError
FrameDecodeError = hg.FrameDecodeError

TARGET_INFO_PATH = hg.TARGET_INFO_PATH
TARGET_STATUS_PATH = hg.TARGET_STATUS_PATH
CURVE_START_PATH = hg.CURVE_START_PATH
CURVE_START_BIN_PATH = hg.CURVE_START_BIN_PATH
CURVE_STOP_PATH = hg.CURVE_STOP_PATH
CURVE_STATUS_PATH = hg.CURVE_STATUS_PATH
CURVE_FRAMES_PATH = hg.CURVE_FRAMES_PATH

#: Write endpoints (opt-in only; the default HIL gate never calls these).
TARGET_FLASH_PATH = "/api/target/flash"
TARGET_RESET_PATH = "/api/target/reset"
OTA_UPLOAD_PATH = "/api/ota/upload"

#: OTA JSON module (docs/protocol.md module 5: GET_INFO=1, GET_PROGRESS=3).
OTA_MODULE_ID = 5
OTA_GET_INFO = 1

#: Engine states served by GET /api/target/status.
TARGET_IDLE_STATES = ("IDLE", "DONE")

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_UNREACHABLE = 3

TOOL_NAME = "birdiebot-wifi-dap-hil-extended"
TOOL_VERSION = 1

DEFAULT_HOST = "192.168.7.1"
DEFAULT_DURATION = 20.0            # short soak smoke (task baseline: 1800 s)
DEFAULT_OPENOCD = os.path.join(REPO_ROOT, "build/openocd-master-install/bin/openocd")
DEFAULT_OPENOCD_SCRIPTS = os.path.join(
    REPO_ROOT, "build/openocd-master-install/share/openocd/scripts")
DEFAULT_OTA_IMAGE = os.path.join(REPO_ROOT, "build/esp32s3/birdiebot_wifi_dap.bin")


# ---------------------------------------------------------------------------
# Exceptions / recording
# ---------------------------------------------------------------------------

class SmokeAbort(Exception):
    """A phase cannot continue (recorded check failed)."""

    def __init__(self, check_id: str, detail: str) -> None:
        super().__init__("%s: %s" % (check_id, detail))
        self.check_id = check_id
        self.detail = detail


class Recorder:
    """Collects checks / sections / warnings and computes the PASS/FAIL flag."""

    def __init__(self) -> None:
        self.checks: List[Dict[str, Any]] = []
        self.warnings: List[str] = []
        self.sections: Dict[str, Any] = {}
        self.failed = False

    # -- checks ------------------------------------------------------------
    def _add(self, check_id: str, ok: bool, detail: str, severity: str,
             skipped: bool = False, dry_run: bool = False,
             values: Optional[Dict[str, Any]] = None) -> bool:
        entry: Dict[str, Any] = {
            "id": check_id,
            "ok": bool(ok),
            "severity": severity,
            "detail": detail,
        }
        if skipped:
            entry["skipped"] = True
        if dry_run:
            entry["dry_run"] = True
        if values:
            entry["values"] = values
        self.checks.append(entry)
        if not ok:
            if severity == "required":
                self.failed = True
            elif severity == "warn":
                self.warnings.append("%s: %s" % (check_id, detail))
        return bool(ok)

    def pass_(self, check_id: str, detail: str,
              values: Optional[Dict[str, Any]] = None) -> bool:
        return self._add(check_id, True, detail, "required", values=values)

    def fail(self, check_id: str, detail: str,
             values: Optional[Dict[str, Any]] = None,
             severity: str = "required") -> bool:
        return self._add(check_id, False, detail, severity, values=values)

    def warn(self, check_id: str, detail: str,
             values: Optional[Dict[str, Any]] = None) -> bool:
        return self._add(check_id, False, detail, "warn", values=values)

    def info(self, check_id: str, detail: str,
             values: Optional[Dict[str, Any]] = None) -> bool:
        return self._add(check_id, True, detail, "info", values=values)

    def skip(self, check_id: str, detail: str,
             values: Optional[Dict[str, Any]] = None) -> bool:
        return self._add(check_id, True, detail, "info", skipped=True,
                         values=values)

    def dry(self, check_id: str, detail: str,
            values: Optional[Dict[str, Any]] = None) -> bool:
        return self._add(check_id, True, detail, "info", dry_run=True,
                         values=values)

    def check(self, check_id: str, ok: bool, detail: str,
              severity: str = "required",
              values: Optional[Dict[str, Any]] = None) -> bool:
        """Record a check without aborting (required failures still FAIL)."""
        return self._add(check_id, ok, detail, severity, values=values)

    def require(self, check_id: str, ok: bool, detail: str,
                values: Optional[Dict[str, Any]] = None,
                ok_detail: Optional[str] = None) -> None:
        if self._add(check_id, ok, detail if not ok else (ok_detail or detail),
                     "required", values=values):
            return
        raise SmokeAbort(check_id, detail)

    # -- sections ----------------------------------------------------------
    def section(self, name: str, initial: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        section = self.sections.setdefault(name, initial if initial is not None else {})
        return section


# ---------------------------------------------------------------------------
# Small pure helpers
# ---------------------------------------------------------------------------

def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_uid(value: Any) -> str:
    """Normalise an STM32 UID string for comparisons (hex, upper, no spaces)."""
    if value is None:
        return ""
    text = "".join(str(value).split()).upper()
    if text.startswith("0X"):
        text = text[2:]
    return "".join(ch for ch in text if ch in "0123456789ABCDEF")


def parse_u32(text: str) -> int:
    try:
        value = int(text, 16) if text.strip().lower().startswith("0x") \
            else int(text, 10)
    except (AttributeError, ValueError):
        raise argparse.ArgumentTypeError("expected a 32-bit integer, got %r" % (text,))
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("address out of 32-bit range: %r" % (text,))
    return value


def is_sha256_hex(text: Any) -> bool:
    if not isinstance(text, str) or len(text) != 64:
        return False
    return all(ch in "0123456789abcdefABCDEF" for ch in text)


def response_json(rec: Recorder, check_id: str, response: Response,
                  abort: bool = True) -> Dict[str, Any]:
    """Parse a JSON HTTP response and record ``check_id`` on failure."""
    ok = True
    detail = "HTTP %d" % response.status
    try:
        data = response.json()
    except ProtocolError as exc:
        ok = False
        detail = str(exc)
        data = {}
    if ok and response.status != 200:
        ok = False
        detail = "HTTP %d (expected 200); body=%r" % (
            response.status, response.body[:160])
    if not ok:
        rec.fail(check_id, detail)
        if abort:
            raise SmokeAbort(check_id, detail)
    return data


def check_image_requirements(image_path: str, *, expect_sha256: str,
                             expect_uid: str, expect_flash_kib: int,
                             target: Dict[str, Any], require_h7: bool,
                             min_size: int = 1) -> Tuple[List[str], Dict[str, Any]]:
    """Pure read-only validation of an image against the detected target.

    Returns ``(errors, facts)`` where ``facts`` always records what was seen
    (image path/size/sha256, target family/UID/flash size) so the caller can
    log the confirmation before any write.
    """
    errors: List[str] = []
    facts: Dict[str, Any] = {
        "image": os.path.abspath(image_path),
        "expect_sha256": expect_sha256,
        "expect_uid": expect_uid,
        "expect_flash_kib": expect_flash_kib,
        "target": target,
    }
    size: Optional[int] = None
    actual_sha = ""
    if not os.path.isfile(image_path):
        errors.append("image file not found: %s" % image_path)
    else:
        size = os.path.getsize(image_path)
        facts["image_size"] = size
        if size <= 0:
            errors.append("image file is empty: %s" % image_path)
        elif size < min_size:
            errors.append("image too small: %d B < %d B" % (size, min_size))
        actual_sha = sha256_file(image_path)
        facts["image_sha256"] = actual_sha

    flash_size = int(target.get("flash_size") or 0)
    family = str(target.get("family") or "")
    if target.get("detected") is not True:
        errors.append("target not detected: refusing any Flash/OTA write")
    elif not flash_size:
        errors.append("target flash size unknown")
    elif size is not None and size > flash_size:
        errors.append("image (%d B) does not fit target flash (%d B)"
                      % (size, flash_size))
    if target.get("supported") is not True:
        errors.append("target family %r is detection-only (no flash driver)"
                      % (family,))
    if require_h7 and not family.lower().startswith("stm32h7"):
        errors.append("only STM32H7 writes are covered by this smoke "
                      "(detected %r); pass --allow-any-target to override"
                      % (family,))
    if expect_flash_kib and flash_size and flash_size // 1024 != expect_flash_kib:
        errors.append("detected flash %d KiB != expected %d KiB"
                      % (flash_size // 1024, expect_flash_kib))
    if expect_uid:
        detected_uid = normalize_uid(target.get("uid"))
        wanted_uid = normalize_uid(expect_uid)
        if not detected_uid:
            errors.append("target UID unavailable but --flash-expect-uid was given")
        elif detected_uid != wanted_uid:
            errors.append("target UID mismatch: detected %s, expected %s"
                          % (detected_uid, wanted_uid))
    if expect_sha256:
        if not is_sha256_hex(expect_sha256):
            errors.append("--flash-expect-sha256 is not 64 hex chars")
        elif actual_sha and actual_sha.lower() != expect_sha256.lower():
            errors.append("image sha256 mismatch: %s != %s"
                          % (actual_sha, expect_sha256.lower()))
    return errors, facts


def detect_root_helper(explicit: str = "") -> Optional[List[str]]:
    """Best-effort command prefix that runs a subprocess as root."""
    candidates: List[str] = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("HIL_ROOT_HELPER", "")
    if env:
        candidates.append(env)
    if os.access("/tmp/hostroot.sh", os.X_OK):
        candidates.append("/tmp/hostroot.sh")
    for candidate in candidates:
        words = candidate.split()
        if words and (os.path.exists(words[0]) or shutil.which(words[0])):
            return words
    if shutil.which("sudo"):
        try:
            probe = subprocess.run(["sudo", "-n", "true"], capture_output=True,
                                   timeout=10)
            if probe.returncode == 0:
                return ["sudo"]
        except (OSError, subprocess.TimeoutExpired):
            pass
    return None


def run_subprocess(argv: List[str], timeout: float,
                   env: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
    started = time.monotonic()
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=max(1.0, timeout), env=full_env)
        return {
            "rc": proc.returncode,
            "seconds": round(time.monotonic() - started, 3),
            "output": ((proc.stdout or "") + (proc.stderr or ""))[-4000:],
        }
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(output, bytes):  # pragma: no cover - py<3.9 style
            output = output.decode("utf-8", "replace")
        return {"rc": 124, "seconds": round(time.monotonic() - started, 3),
                "output": output[-4000:] + "\n[hil_extended] TIMEOUT\n"}
    except OSError as exc:
        return {"rc": 127, "seconds": 0.0, "output": "cannot execute: %s" % exc}


def _short(output: str, limit: int = 320) -> str:
    text = (output or "").strip()
    return text[-limit:] if len(text) > limit else text


# ---------------------------------------------------------------------------
# Minimal stdlib WebSocket JSON client (OTA(5)/GET_INFO verification)
# ---------------------------------------------------------------------------

class WsJsonClient:
    """Small RFC 6455 client for the device JSON module protocol.

    Only what the OTA verification needs: handshake, masked text frames and
    matching a response on ``(module, cmd)``.  Mirrors the client used by
    ``scripts/hil_gate.py`` / ``scripts/e2e_ws_client.py``; kept local so the
    extended smoke does not depend on another task's private helpers.
    """

    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, host: str, port: int = 80, path: str = "/ws",
                 timeout: float = 10.0) -> None:
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.sock: Optional[socket.socket] = None
        self._buffer = b""

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port),
                                             timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Origin: http://%s:%d\r\n"
            "\r\n" % (self.path, self.host, self.port, key, self.host, self.port)
        )
        self.sock.sendall(request.encode("ascii"))
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("WS handshake: connection closed")
            response += chunk
            if len(response) > 16384:
                raise OSError("WS handshake: response too large")
        header, _, rest = response.partition(b"\r\n\r\n")
        self._buffer = rest
        lines = header.decode("latin-1").split("\r\n")
        if not lines or "101" not in lines[0]:
            raise OSError("WS handshake failed: %r" % (lines[0] if lines else ""))
        accept_expected = base64.b64encode(
            hashlib.sha1((key + self.GUID).encode("ascii")).digest()).decode("ascii")
        accepted = ""
        for line in lines[1:]:
            if ":" in line and line.split(":", 1)[0].strip().lower() == \
                    "sec-websocket-accept":
                accepted = line.split(":", 1)[1].strip()
        if accepted != accept_expected:
            raise OSError("WS handshake: bad Sec-WebSocket-Accept")

    def _recv_exact(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise OSError("WS connection closed")
            self._buffer += chunk
        data, self._buffer = self._buffer[:count], self._buffer[count:]
        return data

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        header = bytearray([0x80 | (opcode & 0x0F)])
        length = len(payload)
        mask = os.urandom(4)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack("!H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack("!Q", length)
        header += mask
        masked = bytes(byte ^ mask[index % 4]
                       for index, byte in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _recv_frame(self) -> Tuple[int, bytes]:
        first, second = self._recv_exact(2)
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self._recv_exact(8))[0]
        mask = self._recv_exact(4) if masked else b""
        payload = self._recv_exact(length) if length else b""
        if masked:
            payload = bytes(byte ^ mask[index % 4]
                            for index, byte in enumerate(payload))
        return opcode, payload

    def request(self, module: int, cmd: int,
                extra: Optional[Dict[str, Any]] = None,
                timeout: Optional[float] = None) -> Dict[str, Any]:
        wait = self.timeout if timeout is None else timeout
        payload: Dict[str, Any] = {"module": module, "cmd": cmd}
        if extra:
            payload.update(extra)
        self._send_frame(0x1, json.dumps(payload).encode("utf-8"))
        deadline = time.monotonic() + wait
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise OSError("WS timeout waiting for module=%d cmd=%d"
                              % (module, cmd))
            self.sock.settimeout(remaining)
            opcode, data = self._recv_frame()
            if opcode == 0x8:
                raise OSError("WS server closed the connection")
            if opcode == 0x9:
                self._send_frame(0xA, data)
                continue
            if opcode != 0x1:
                continue
            try:
                parsed = json.loads(data.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if isinstance(parsed, dict) and parsed.get("module") == module \
                    and parsed.get("cmd") == cmd:
                return parsed

    def close(self) -> None:
        if self.sock is not None:
            try:
                self._send_frame(0x8, b"")
            except OSError:
                pass
            try:
                self.sock.close()
            finally:
                self.sock = None


def ws_ota_info(host: str, port: int, timeout: float = 10.0) -> Dict[str, Any]:
    """OTA(5)/GET_INFO over WS -> normalised dict (raises OSError/ProtocolError)."""
    client = WsJsonClient(host, port, timeout=timeout)
    client.connect()
    try:
        info = client.request(OTA_MODULE_ID, OTA_GET_INFO, timeout=timeout)
    finally:
        client.close()
    if info.get("ok") is False:
        raise ProtocolError("OTA GET_INFO returned ok=false: %r" % (info,))
    return {
        "running": info.get("running"),
        "running_ver": info.get("running_ver"),
        "status": info.get("status"),
        "free": info.get("free"),
        "slot_size": info.get("slot_size"),
        "rollback": info.get("rollback"),
    }


# ---------------------------------------------------------------------------
# Harness: live device or in-process fake (used by --dry-run / host tests)
# ---------------------------------------------------------------------------

class LiveHarness:
    """Real network/host harness used for any non-dry-run invocation."""

    offline = False

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args

    def http(self, timeout: Optional[float] = None) -> Any:
        return HttpTransport(self.args.host, self.args.port,
                             timeout=self.args.timeout if timeout is None else timeout,
                             verbose=self.args.verbose)

    def ota_info(self) -> Dict[str, Any]:
        return ws_ota_info(self.args.host, self.args.port,
                           timeout=min(self.args.timeout, 10.0))

    def run(self, argv: List[str], timeout: float,
            env: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
        return run_subprocess(argv, timeout, env=env)

    def root_helper(self, explicit: str = "") -> Optional[List[str]]:
        return detect_root_helper(explicit)

    def usbip_available(self) -> bool:
        return shutil.which("usbip") is not None

    def openocd_environment(self) -> Tuple[Optional[str], Optional[str], Optional[str]]:
        """Return (bin, scripts_dir, reason-unavailable)."""
        binary = self.args.openocd_path
        scripts = self.args.openocd_scripts
        if not os.path.isfile(binary):
            return None, None, ("OpenOCD binary not found: %s (build with "
                                "scripts/build-openocd-master.sh)" % binary)
        if not os.path.isdir(scripts):
            return None, None, "OpenOCD scripts directory not found: %s" % scripts
        return binary, scripts, None


class FakeDevice:
    """In-process device double for ``--dry-run`` and host tests.

    Implements the read-only endpoints plus the two write paths with the same
    headers/response shape as the firmware, and simulates a short reboot after
    a successful OTA so the poll/reconnect logic is exercised offline.
    """

    def __init__(self, *, detected: bool = True, diagnostics: bool = True,
                 flash_status: int = 200, ota_status: int = 200,
                 curve_error: bool = False, ota_version: str = "v1.2.0-67-ge4facd0-dirty",
                 heap_free: int = 120000, heap_largest: int = 40000) -> None:
        self.detected = detected
        self.diagnostics = diagnostics
        self.flash_status = flash_status
        self.ota_status = ota_status
        self.curve_error = curve_error
        self.heap_free = heap_free
        self.heap_largest = heap_largest
        self.uid = "0123456789ABCDEF01234567"
        self.family = "stm32h7"
        self.series = "STM32H7"
        self.device = "STM32H723/H725/H730/H733/H735"
        self.flash_size = 1048576
        self.running_slot = "ota_0"
        self.running_ver = ota_version
        self.rollback = True
        self.slot_size = 3145728
        self.flash_busy = False
        self.flash_state = "IDLE"
        self.flash_written = 0
        self.flash_verified = 0
        self.flash_err = 0
        self.flash_message = "probe ok: STM32H723 @1000 kHz"
        self.flash_started = 0.0
        self.flash_seconds = 0.25
        self.flash_attempts = 0
        self.flash_bytes = 0
        self.reset_calls = 0
        self.ota_attempts = 0
        self.ota_bytes = 0
        self.reboot_start = 0.0
        self.reboot_end = 0.0
        self.curve_running = False
        self.curve_started = 0.0
        self.curve_interval_us = 20000
        self.curve_series = 0
        self._lock = threading.Lock()

    # -- helpers -----------------------------------------------------------
    @staticmethod
    def _json(data: Dict[str, Any], status: int = 200) -> Response:
        return Response(status=status, headers={"content-type": "application/json"},
                        body=json.dumps(data).encode("utf-8"))

    def _maybe_rebooting(self) -> bool:
        return self.reboot_start <= time.monotonic() < self.reboot_end

    def _status_json(self) -> Dict[str, Any]:
        data: Dict[str, Any] = {
            "module": 6, "cmd": 3, "ok": True, "state": self.flash_state,
            "total": self.flash_bytes, "written": self.flash_written,
            "verified": self.flash_verified, "percent": 0 if not self.flash_busy else 50,
            "err": self.flash_err, "message": self.flash_message,
            "busy": self.flash_busy,
        }
        if self.diagnostics:
            data.update({
                "heap_free": self.heap_free,
                "heap_largest": self.heap_largest,
                "dap_session_active": False,
                "swd_clock_hz": 1000000,
            })
        return data

    def _curve_frames(self) -> List[Any]:
        if not self.curve_running:
            return []
        elapsed = max(0.0, time.monotonic() - self.curve_started)
        count = int(elapsed * 1e6 / self.curve_interval_us)
        base_ts = int(self.curve_started * 1e6)
        frame_bytes = self.curve_series * 4
        frames = []
        for index in range(1, count + 1):
            frames.append(SimpleNamespace(
                seq=index, timestamp_us=base_ts + int(index * self.curve_interval_us),
                group=0, payload_len=frame_bytes,
                payload=b"\x01\x02\x03\x04" * self.curve_series, pc=None))
        return frames

    def _encode_page(self, frames: List[Any]) -> bytes:
        if not frames:
            return b""
        frame_bytes = self.curve_series * 4
        header = struct.pack("<4sBBHIHH", hg.CVF2_MAGIC, hg.CVF2_VERSION,
                             hg.CVF_FLAG_PC, len(frames), frames[0].seq,
                             frame_bytes, 0)
        body = b""
        for frame in frames:
            body += struct.pack("<IQ", frame.seq, frame.timestamp_us)
            body += struct.pack("<BHI", 0, frame.payload_len, 0x08000123)
            body += bytes(frame.payload)
        return header + body

    def _parse_cbs1(self, data: bytes) -> Tuple[int, int]:
        if not data.startswith(b"CBS1"):
            raise ValueError("not CBS1")
        interval_us = struct.unpack_from("<I", data, 8)[0]
        group_count = data[29]
        offset = 32
        total = 0
        for _ in range(group_count):
            count = data[offset + 1]
            total += count
            offset += 4
            for _ in range(count):
                expr_len = struct.unpack_from("<H", data, offset + 12)[0]
                offset += 14 + expr_len
        return interval_us, total

    # -- write endpoints ---------------------------------------------------
    def _flash(self, path: str, method: str, data: bytes,
               headers: Dict[str, str]) -> Response:
        with self._lock:
            self.flash_attempts += 1
            self.flash_bytes = len(data)
        content_type = headers.get("Content-Type", "")
        if not content_type.startswith("application/octet-stream"):
            return self._json({"ok": False, "err": "unsupported content type",
                               "code": 5}, 415)
        expected = headers.get("X-Target-SHA256", "")
        actual = hashlib.sha256(data).hexdigest()
        if not is_sha256_hex(expected):
            return self._json({"ok": False, "err": "bad sha256 header",
                               "code": 5}, 400)
        if expected.lower() != actual:
            return self._json({"ok": False, "err": "sha256 mismatch",
                               "code": 5}, 422)
        if self.flash_status != 200:
            with self._lock:
                self.flash_state = "ERROR"
                self.flash_err = 3
                self.flash_message = "simulated flash failure"
            return self._json({"ok": False, "err": "simulated flash failure",
                               "code": 3}, self.flash_status)
        with self._lock:
            self.flash_busy = True
            self.flash_state = "PROGRAMMING"
            self.flash_started = time.monotonic()
        time.sleep(self.flash_seconds)
        with self._lock:
            self.flash_busy = False
            self.flash_state = "DONE"
            self.flash_written = len(data)
            self.flash_verified = len(data)
            self.flash_err = 0
            self.flash_message = "write+verify ok"
        return self._json({
            "module": 6, "cmd": 2, "ok": True, "family": self.family,
            "uid": self.uid, "written": len(data), "verified": len(data),
            "sha256": actual, "state": "DONE", "message": "write+verify ok",
            "flash_size": self.flash_size,
        })

    def _ota(self, data: bytes, headers: Dict[str, str]) -> Response:
        with self._lock:
            self.ota_attempts += 1
            self.ota_bytes = len(data)
        if self.ota_status != 200:
            return self._json({"ok": False, "err": "simulated ota failure",
                               "code": 6}, self.ota_status)
        content_type = headers.get("Content-Type", "")
        if not content_type.startswith("application/octet-stream"):
            return self._json({"ok": False, "err": "unsupported content type",
                               "code": 5}, 415)
        expected = headers.get("X-SHA256", "")
        actual = hashlib.sha256(data).hexdigest()
        if expected and (not is_sha256_hex(expected) or expected.lower() != actual):
            return self._json({"ok": False, "err": "sha256 mismatch", "code": 5},
                              422)
        target_slot = "ota_1" if self.running_slot == "ota_0" else "ota_0"
        old_ver = self.running_ver
        with self._lock:
            self.running_slot = target_slot
            self.reboot_start = time.monotonic() + 0.3
            self.reboot_end = self.reboot_start + 0.6
        return self._json({
            "module": 5, "cmd": 2, "ok": True, "slot": target_slot,
            "running": "ota_1" if target_slot == "ota_0" else "ota_0",
            "running_ver": old_ver, "offset": len(data), "total": len(data),
            "sha256": actual, "status": "READY", "reboot_in_ms": 1000,
        })

    def _reset(self, body: Dict[str, Any]) -> Response:
        with self._lock:
            self.reset_calls += 1
            self.flash_state = "IDLE"
            self.flash_busy = False
            self.flash_err = 0
            self.flash_message = "target reset(run=%r)" % body.get("run", True)
        return self._json({"module": 6, "cmd": 4, "ok": True,
                           "run": bool(body.get("run", True))})

    # -- transport interface ----------------------------------------------
    def request_bytes(self, method: str, path: str, data: bytes = b"",
                      headers: Optional[Dict[str, str]] = None,
                      timeout: Optional[float] = None, retries: int = 1,
                      close_after: bool = False) -> Response:
        del timeout, retries, close_after
        request_headers = dict(headers or {})
        if method == "POST" and path == TARGET_FLASH_PATH:
            return self._flash(path, method, data, request_headers)
        if method == "POST" and path == OTA_UPLOAD_PATH:
            return self._ota(data, request_headers)
        if method == "POST" and path == CURVE_START_BIN_PATH:
            try:
                interval_us, total = self._parse_cbs1(data)
            except (IndexError, struct.error, ValueError):
                return self._json({"ok": False, "err": "bad CBS1"}, 400)
            with self._lock:
                self.curve_series = total
                self.curve_interval_us = interval_us
                self.curve_running = True
                self.curve_started = time.monotonic()
            return self._json({
                "ok": True, "group_count": 1, "series_count": total,
                "sample_bytes": total * 4, "interval_us": interval_us,
                "trigger": "periodic", "halt": True, "pc_capture": True,
                "link_hz": 1000000,
            })
        raise AssertionError("FakeDevice: unexpected raw request %s %s"
                             % (method, path))

    def request(self, method: str, path: str,
                body: Optional[Dict[str, Any]] = None,
                timeout: Optional[float] = None,
                retries: int = 1) -> Response:
        del timeout, retries
        if self._maybe_rebooting():
            raise DeviceUnreachable("simulated OTA reboot in progress")
        if path == TARGET_INFO_PATH and method == "GET":
            if not self.detected:
                return self._json({"module": 6, "cmd": 1, "ok": False,
                                   "detected": False, "err": "target not detected",
                                   "code": 5})
            return self._json({
                "module": 6, "cmd": 1, "ok": True, "detected": True,
                "family": self.family, "series": self.series,
                "device": self.device, "supported": True,
                "swd_clock_hz": 1000000, "idcode": "0x10016483",
                "uid": self.uid, "flash_base": "0x08000000",
                "flash_size": self.flash_size, "flash_kib": self.flash_size // 1024,
                "max_image_size": self.flash_size, "dev_id": 1155,
                "model_candidates": "STM32H723/H725/H730/H733/H735 [xG, 1024 KiB]",
                "core": "Cortex-M7", "detection_confidence": "flash-narrowed",
            })
        if path == TARGET_STATUS_PATH and method == "GET":
            with self._lock:
                data = self._status_json()
            return self._json(data)
        if path == TARGET_RESET_PATH and method == "POST":
            return self._reset(body or {})
        if path == CURVE_START_PATH and method == "POST":
            series = (body or {}).get("series", [])
            with self._lock:
                self.curve_series = len(series)
                self.curve_interval_us = int((body or {}).get("interval_us", 20000))
                self.curve_running = True
                self.curve_started = time.monotonic()
            return self._json({"ok": True, "series_count": self.curve_series,
                               "sample_bytes": self.curve_series * 4,
                               "interval_us": self.curve_interval_us})
        if path == CURVE_STOP_PATH and method == "POST":
            with self._lock:
                self.curve_running = False
            return self._json({"ok": True})
        if path == CURVE_STATUS_PATH and method == "GET":
            frames = self._curve_frames()
            return self._json({
                "ok": True, "running": self.curve_running, "frames": len(frames),
                "interval_us": self.curve_interval_us,
                "error": 7 if (self.curve_error and self.curve_running) else 0,
                "invalid": 0, "overwritten": 0,
                "sample_bytes": self.curve_series * 4,
                "series_count": self.curve_series,
            })
        if path.startswith(CURVE_FRAMES_PATH):
            frames = self._curve_frames()
            query = path.split("?", 1)[1] if "?" in path else ""
            params: Dict[str, str] = {}
            for pair in query.split("&"):
                if "=" in pair:
                    key, value = pair.split("=", 1)
                    params[key] = value
            since = int(params.get("since", "0") or 0)
            limit = int(params.get("max", "64") or 64)
            frames = [frame for frame in frames if frame.seq > since][:limit]
            if not frames:
                return Response(status=204, headers={}, body=b"")
            return Response(status=200,
                            headers={"content-type": "application/octet-stream"},
                            body=self._encode_page(frames))
        if path == "/" and method == "GET":
            html = ("<!DOCTYPE html><html><head><title>Fake DAP</title></head>"
                    "<body><div id=\"app\">fake</div>" + "<!-- filler -->" * 200
                    + "</body></html>")
            return Response(status=200, headers={"content-type": "text/html"},
                            body=html.encode("utf-8"))
        raise AssertionError("FakeDevice: unexpected request %s %s" % (method, path))


class FakeHarness:
    """Offline harness: fake HTTP device + fake WS/host-tool results."""

    offline = True

    def __init__(self, device: Optional[FakeDevice] = None,
                 *, usbip_available: bool = True,
                 root_helper: bool = True) -> None:
        self.device = device or FakeDevice()
        self._usbip_available = usbip_available
        self._root_helper = ["fake-root"] if root_helper else None
        self.calls: List[List[str]] = []

    def http(self, timeout: Optional[float] = None) -> FakeDevice:
        del timeout
        return self.device

    def ota_info(self) -> Dict[str, Any]:
        if self.device._maybe_rebooting():
            raise OSError("simulated OTA reboot in progress")
        return {
            "running": self.device.running_slot,
            "running_ver": self.device.running_ver,
            "status": "IDLE",
            "free": self.device.slot_size,
            "slot_size": self.device.slot_size,
            "rollback": self.device.rollback,
        }

    def run(self, argv: List[str], timeout: float,
            env: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
        del timeout, env
        self.calls.append(list(argv))
        joined = " ".join(argv)
        if "usbip" in joined and "attach" in joined:
            return {"rc": 0, "seconds": 0.01,
                    "output": "usbip: info: using remote host fake\n"}
        if "usbip" in joined and "detach" in joined:
            return {"rc": 0, "seconds": 0.01, "output": "usbip: detach fake\n"}
        if "openocd" in joined and "--version" in joined:
            return {"rc": 0, "seconds": 0.01,
                    "output": "Open On-Chip Debugger 0.12.0-fake\n"}
        if "--version" in joined:
            return {"rc": 0, "seconds": 0.01, "output": "usbip (fake) 2.0\n"}
        return {"rc": 0, "seconds": 0.05,
                "output": ("Open On-Chip Debugger 0.12.0-fake\n"
                           "Info : CMSIS-DAP: SWD supported\n"
                           "Info : CMSIS-DAP: Interface Initialised (SWD)\n"
                           "Info : STM32H723 detected\nInfo : shutdown\n")}

    def root_helper(self, explicit: str = "") -> Optional[List[str]]:
        del explicit
        return self._root_helper

    def usbip_available(self) -> bool:
        return self._usbip_available

    def openocd_environment(self) -> Tuple[Optional[str], Optional[str], Optional[str]]:
        return DEFAULT_OPENOCD, DEFAULT_OPENOCD_SCRIPTS, None


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

def phase_preflight(rec: Recorder, args: argparse.Namespace,
                    harness: Any, probe_target: bool = True) -> Dict[str, Any]:
    """Read-only device validation; every write mode runs this first.

    ``probe_target=False`` is used for a USBIP-only run: an HTTP
    ``/api/target/info`` probe right before the USBIP/OpenOCD session can
    trigger the known "CMSIS-DAP command CMD_INFO failed" limitation
    (docs/hil-extended.md §8.3), so that mode does a non-DAP connectivity check
    here and verifies the target with ``/api/target/info`` *after* the session.
    """
    section = rec.section("preflight", {})
    write_requested = bool(args.flash_image or args.ota_image)
    http = harness.http()
    info: Dict[str, Any] = {}
    if probe_target:
        try:
            response = http.request("GET", TARGET_INFO_PATH, retries=2)
        except DeviceUnreachable as exc:
            rec.fail("device.reachable", str(exc))
            raise
        rec.require("device.reachable", response.status in (200, 403),
                    "HTTP %d from GET %s" % (response.status, TARGET_INFO_PATH),
                    values={"status": response.status})
        try:
            info = response.json()
        except ProtocolError as exc:
            rec.fail("target.info.json", str(exc))
            raise SmokeAbort("target.info.json", str(exc))
    else:
        try:
            response = http.request("GET", "/", retries=2)
        except DeviceUnreachable as exc:
            rec.fail("device.reachable", str(exc))
            raise
        rec.require("device.reachable", response.status == 200,
                    "HTTP %d from GET /" % response.status,
                    values={"status": response.status})
        rec.info("target.probe.skipped",
                 "USBIP-only run: the /api/target/info HTTP probe before the "
                 "OpenOCD session is intentionally skipped (known CMD_INFO "
                 "limitation); target detection is verified after detach")
    section["target"] = {
        "detected": info.get("detected"),
        "family": info.get("family"),
        "series": info.get("series"),
        "device": info.get("device"),
        "uid": info.get("uid"),
        "idcode": info.get("idcode"),
        "dpidr": info.get("dpidr"),
        "flash_size": info.get("flash_size"),
        "flash_kib": info.get("flash_kib"),
        "supported": info.get("supported"),
        "swd_clock_hz": info.get("swd_clock_hz"),
    }
    if probe_target:
        detected = info.get("detected") is True and info.get("ok") is True
        rec.require("target.detected", detected,
                    "detected=%r ok=%r err=%r (SWD wiring/power/debug session); "
                    "refusing to run a write or soak without a detected target"
                    % (info.get("detected"), info.get("ok"), info.get("err")),
                    values={"detected": info.get("detected")})
        family = str(info.get("family") or "")
        rec.require("target.h7", family.lower().startswith("stm32h7")
                    or not (write_requested or args.usbip),
                    "write/USBIP smoke requires an STM32H7 target (detected %r); "
                    "pass --allow-any-target to override" % (family,),
                    ok_detail="target family %s" % (info.get("series") or family))
        uid = normalize_uid(info.get("uid"))
        flash_size = int(info.get("flash_size") or 0)
        rec.require("target.flash_size", flash_size > 0,
                    "flash_size=%r" % (info.get("flash_size"),))
        if write_requested:
            rec.require("target.uid.recorded", bool(uid),
                        "target UID missing from /api/target/info; refusing a "
                        "Flash/OTA write without an identity to record")
        rec.info("target.uid",
                 "target UID=%s flash=%d KiB family=%r idcode=%r"
                 % (uid or "?", flash_size // 1024, family,
                    info.get("idcode")),
                 values={"uid": uid, "flash_size": flash_size,
                         "family": family, "idcode": info.get("idcode")})
    else:
        family = ""
        uid = ""
        flash_size = 0

    status_response = harness.http().request("GET", TARGET_STATUS_PATH, retries=2)
    status = response_json(rec, "target.status.json", status_response) if \
        status_response.status == 200 else {}
    if status_response.status != 200:
        raise SmokeAbort("target.status.json",
                         "HTTP %d from GET %s"
                         % (status_response.status, TARGET_STATUS_PATH))
    state = status.get("state")
    busy = status.get("busy")
    rec.require("target.status.idle",
                state in TARGET_IDLE_STATES and busy is False,
                "flash engine state=%r busy=%r (another Flash/OTA operation "
                "is running? retry when idle)" % (state, busy))
    rec.require("target.status.no_error",
                int(status.get("err") or 0) == 0,
                "err=%r last_error=%r" % (status.get("err"),
                                          status.get("last_error")))
    heap_free = status.get("heap_free")
    heap_largest = status.get("heap_largest")
    rec.require("target.status.diagnostics",
                isinstance(heap_free, int) and isinstance(heap_largest, int),
                "heap_free=%r heap_largest=%r missing from "
                "/api/target/status (old firmware?)" % (heap_free, heap_largest))
    if isinstance(heap_free, int) and isinstance(heap_largest, int):
        rec.require("target.heap.min",
                    heap_free >= args.heap_min_free
                    and heap_largest >= args.heap_min_largest,
                    "heap_free=%d (min %d) heap_largest=%d (min %d)"
                    % (heap_free, args.heap_min_free,
                       heap_largest, args.heap_min_largest))
        section["heap"] = {"free": heap_free, "largest": heap_largest}
        rec.info("target.heap", "heap_free=%d B heap_largest=%d B"
                 % (heap_free, heap_largest))
    dap_active = status.get("dap_session_active")
    if dap_active is True:
        if args.allow_dap_session:
            rec.warn("target.dap_session",
                     "dap_session_active=true (a USBIP/WS-DAP client is "
                     "connected); continuing because --allow-dap-session")
        else:
            rec.require("target.dap_session", False,
                        "dap_session_active=true: another USBIP/elaphureLink/"
                        "WS-DAP client owns the DAP; close it or pass "
                        "--allow-dap-session")
    elif isinstance(dap_active, bool):
        rec.info("target.dap_session", "dap_session_active=false (no client)")
    section["status"] = {
        "state": state, "busy": busy, "err": status.get("err"),
        "heap_free": heap_free, "heap_largest": heap_largest,
        "dap_session_active": dap_active,
        "swd_clock_hz": status.get("swd_clock_hz"),
        "wifi_rssi": status.get("wifi_rssi"),
    }
    curve_response = harness.http().request("GET", CURVE_STATUS_PATH, retries=1)
    curve = response_json(rec, "curve.status.pre", curve_response) if \
        curve_response.status == 200 else {}
    rec.require("curve.idle.pre", curve.get("running") is False,
                "curve sampler already running=true; stop it before the "
                "soak/write smoke")
    section["curve_pre"] = {"running": curve.get("running"),
                            "error": curve.get("error")}
    section["recorded_at"] = datetime.now(timezone.utc).isoformat(
        timespec="seconds")
    return {"info": info, "status": status, "uid": uid,
            "flash_size": flash_size, "family": family,
            "probe_target": probe_target,
            "heap": section.get("heap", {})}


def _start_curve(rec: Recorder, args: argparse.Namespace, http: Any,
                 series: int, tag: str) -> Dict[str, Any]:
    body = hg._cbs1_body(SimpleNamespace(interval_us=args.interval_us,
                                        address=args.address), series)
    response = http.request_bytes("POST", CURVE_START_BIN_PATH, data=body,
                                  headers={"Content-Type": "application/octet-stream"},
                                  timeout=args.timeout, retries=0)
    if response.status in (404, 405, 501):
        legacy = {"interval_us": args.interval_us, "halt": True,
                  "series": [{"id": index, "size": 4,
                              "expr": {"kind": "addr",
                                       "address": (args.address + index * 4)
                                       & 0xFFFFFFFF}}
                             for index in range(series)]}
        response = http.request("POST", CURVE_START_PATH, body=legacy,
                                timeout=args.timeout, retries=0)
    data = response_json(rec, "curve.%s.start.json" % tag, response)
    rec.require("curve.%s.start.ok" % tag, data.get("ok") is True,
                "curve/start ok=%r err=%r" % (data.get("ok"), data.get("err")))
    rec.require("curve.%s.start.series" % tag,
                data.get("series_count") == series,
                "series_count=%r expected %d" % (data.get("series_count"), series))
    rec.require("curve.%s.start.interval" % tag,
                data.get("interval_us") == args.interval_us,
                "interval_us=%r expected %d"
                % (data.get("interval_us"), args.interval_us))
    return data


def _stop_curve(rec: Recorder, args: argparse.Namespace, http: Any,
                tag: str, required: bool = True) -> None:
    try:
        response = http.request("POST", CURVE_STOP_PATH, body={},
                                timeout=args.timeout, retries=1)
    except DeviceUnreachable as exc:
        (rec.fail if required else rec.warn)("curve.%s.stop" % tag, str(exc))
        return
    ok = response.status == 200
    if not ok:
        (rec.fail if required else rec.warn)(
            "curve.%s.stop" % tag, "HTTP %d" % response.status)
        return
    try:
        status = http.request("GET", CURVE_STATUS_PATH, timeout=args.timeout,
                              retries=1).json()
    except (DeviceUnreachable, ProtocolError) as exc:
        (rec.fail if required else rec.warn)(
            "curve.%s.stop.status" % tag, "cannot verify curve/stop: %s" % exc)
        return
    ok = status.get("running") is False and int(status.get("error") or 0) == 0
    if ok:
        rec.pass_("curve.%s.stop" % tag, "sampler stopped, target resumed")
    else:
        (rec.fail if required else rec.warn)(
            "curve.%s.stop" % tag,
            "running=%r error=%r after curve/stop"
            % (status.get("running"), status.get("error")))


def phase_soak(rec: Recorder, args: argparse.Namespace, harness: Any,
               preflight: Dict[str, Any]) -> bool:
    """Repeated curve start-bin / sample / stop cycles with heap checks."""
    duration = float(args.duration)
    section = rec.section("soak", {
        "duration_s": duration,
        "series": args.soak_series,
        "interval_us": args.interval_us,
        "run_s": args.soak_run,
    })
    rec.info("soak.started",
             "soak %.0f s: repeated curve start-bin/stop, %.1f s per cycle, "
             "%d series @ %d us, heap thresholds free>=%d largest>=%d"
             % (duration, args.soak_run, args.soak_series, args.interval_us,
                args.heap_min_free, args.heap_min_largest))
    started = time.monotonic()
    cycles = 0
    failures = 0
    curve_errors = 0
    invalid_total = 0
    frames_total = 0
    missing_seqs = 0
    gap_pages = 0
    target_misses = 0
    heaps: List[int] = []
    latencies: List[float] = []
    cycle_records: List[Dict[str, Any]] = []
    last_seq = 0

    while time.monotonic() - started < duration:
        remaining = duration - (time.monotonic() - started)
        run_s = max(0.3, min(args.soak_run, remaining))
        cycles += 1
        tag = "soak.%d" % cycles
        record: Dict[str, Any] = {"cycle": cycles, "run_s": round(run_s, 2)}
        # CVF2 seq restarts for every curve/start session: track frames per
        # cycle so `since=` does not filter out the new session's frames.
        last_seq = 0
        http = harness.http()
        cycle_ok = True
        try:
            _start_curve(rec, args, http, args.soak_series, tag)
        except SmokeAbort:
            failures += 1
            cycle_ok = False
            record["start_error"] = "curve/start failed"
            time.sleep(0.2)
            continue
        cycle_started = time.monotonic()
        cycle_invalid = 0
        cycle_frames = 0
        cycle_errors = 0
        try:
            while time.monotonic() - cycle_started < run_s:
                status_response = http.request("GET", CURVE_STATUS_PATH,
                                               timeout=args.timeout, retries=0)
                latencies.append(status_response.elapsed_ms)
                status = response_json(rec, "curve.%s.status.json" % tag,
                                       status_response, abort=False)
                if int(status.get("error") or 0) != 0:
                    cycle_errors += 1
                cycle_invalid = max(cycle_invalid,
                                    int(status.get("invalid") or 0) or 0)
                if status.get("running") is not True:
                    cycle_ok = False
                    record["stopped_early"] = True
                    break
                frames_response = http.request(
                    "GET", "%s?since=%d&max=64" % (CURVE_FRAMES_PATH, last_seq),
                    timeout=args.timeout, retries=0)
                if frames_response.status == 200:
                    try:
                        page = hg.decode_curve_frames(frames_response.body)
                    except FrameDecodeError as exc:
                        rec.fail("curve.%s.frames.decode" % tag, str(exc))
                        cycle_ok = False
                        break
                    if page.gap:
                        gap_pages += 1
                    for frame in page.frames:
                        if last_seq and frame.seq > last_seq + 1:
                            missing_seqs += frame.seq - last_seq - 1
                        last_seq = frame.seq
                    cycle_frames += len(page.frames)
                elif frames_response.status not in (204, 200):
                    rec.warn("curve.%s.frames.http" % tag,
                             "HTTP %d from curve/frames"
                             % frames_response.status)
                time.sleep(args.poll_interval)
        finally:
            _stop_curve(rec, args, http, tag, required=True)
        if not cycle_ok:
            failures += 1
        frames_total += cycle_frames
        invalid_total = max(invalid_total, cycle_invalid)
        curve_errors += cycle_errors
        stat_response = http.request("GET", TARGET_STATUS_PATH,
                                     timeout=args.timeout, retries=0)
        latencies.append(stat_response.elapsed_ms)
        stat = response_json(rec, "%s.target_status" % tag,
                             stat_response, abort=False)
        heap = stat.get("heap_free")
        if isinstance(heap, int):
            heaps.append(heap)
        if int(stat.get("err") or 0) != 0 or stat.get("state") == "ERROR":
            rec.fail("soak.%d.target_status" % tag,
                     "state=%r err=%r last_error=%r after cycle"
                     % (stat.get("state"), stat.get("err"),
                        stat.get("last_error")))
            cycle_ok = False
        record.update({
            "frames": cycle_frames,
            "heap_free": heap,
            "heap_largest": stat.get("heap_largest"),
            "ok": cycle_ok,
        })
        cycle_records.append(record)
        if cycles % 3 == 0:
            try:
                info_response = http.request("GET", TARGET_INFO_PATH,
                                             timeout=args.timeout, retries=0)
                latencies.append(info_response.elapsed_ms)
                if info_response.json().get("detected") is not True:
                    target_misses += 1
            except (DeviceUnreachable, ProtocolError):
                target_misses += 1

    elapsed = time.monotonic() - started
    invalid_ratio = (invalid_total / float(frames_total)) if frames_total else 0.0
    heap_start = heaps[0] if heaps else None
    heap_end = heaps[-1] if heaps else None
    heap_min = min(heaps) if heaps else None
    heap_delta = (heap_end - heap_start) if heap_start is not None \
        and heap_end is not None else None
    section.update({
        "cycles": cycles,
        "elapsed_s": round(elapsed, 2),
        "iterations": cycles,
        "failures": failures,
        "curve_errors": curve_errors,
        "frames": frames_total,
        "missing_seqs": missing_seqs,
        "gap_pages": gap_pages,
        "invalid": invalid_total,
        "invalid_ratio": round(invalid_ratio, 5),
        "target_misses": target_misses,
        "heap_free_start": heap_start,
        "heap_free_end": heap_end,
        "heap_free_min": heap_min,
        "heap_delta": heap_delta,
        "http_latency_avg_ms": round(sum(latencies) / len(latencies), 1)
        if latencies else None,
        "http_latency_max_ms": round(max(latencies), 1) if latencies else None,
        "heap_series": heaps,
        "cycles_detail": cycle_records,
    })
    rec.require("soak.cycles", cycles > 0, "no soak cycle completed")
    rec.require("soak.failures", failures == 0,
                "%d soak cycle(s) failed (start/stop/status)" % failures)
    rec.require("soak.curve.error", curve_errors == 0,
                "curve sampler error=%d observed" % curve_errors)
    rec.require("soak.frames", frames_total > 0,
                "no curve frames received in %.1f s" % elapsed)
    rec.require("soak.frames.contiguous", missing_seqs == 0 and gap_pages == 0,
                "missing_seqs=%d gap_pages=%d (poll starvation/ring overrun)"
                % (missing_seqs, gap_pages))
    rec.require("soak.invalid",
                invalid_ratio <= args.max_invalid_ratio,
                "invalid=%d/%d frames (ratio %.4f > %.4f): check target RAM "
                "address 0x%08x" % (invalid_total, frames_total, invalid_ratio,
                                    args.max_invalid_ratio, args.address))
    if heap_min is not None:
        rec.require("soak.heap.min", heap_min >= args.heap_min_free,
                    "heap_free dropped to %d B (threshold %d B)"
                    % (heap_min, args.heap_min_free),
                    values={"heap_free_min": heap_min})
        rec.require("soak.heap.no_leak",
                    heap_delta is not None
                    and heap_delta >= -args.heap_leak_tolerance,
                    "heap_free %r -> %r (%r B, tolerance -%d B over %d cycles)"
                    % (heap_start, heap_end, heap_delta,
                       args.heap_leak_tolerance, cycles))
    rec.require("soak.target.alive", target_misses == 0,
                "target not detected %d time(s) during soak" % target_misses)
    section["ok"] = (failures == 0 and frames_total > 0
                     and curve_errors == 0 and target_misses == 0)
    rec.info("soak.summary",
             "%d cycles in %.1f s, failures=%d, frames=%d, invalid=%d, "
             "heap %r..%r B, HTTP avg=%.1f ms max=%.1f ms"
             % (cycles, elapsed, failures, frames_total, invalid_total,
                heap_min, heap_end, section.get("http_latency_avg_ms") or 0.0,
                section.get("http_latency_max_ms") or 0.0),
             values={key: section[key] for key in
                     ("cycles", "failures", "frames", "missing_seqs",
                      "heap_free_min", "heap_delta")})
    return failures == 0 and frames_total > 0


def _recover_target(rec: Recorder, args: argparse.Namespace, harness: Any,
                    reason: str) -> None:
    """Best-effort recovery after a failed target write: reset+run + re-probe."""
    section = rec.section("flash_recovery", {"reason": reason})
    try:
        response = harness.http(timeout=max(10.0, args.timeout)).request(
            "POST", TARGET_RESET_PATH, body={"run": True}, retries=1,
            timeout=args.timeout)
        section["reset_status"] = response.status
        if response.status == 200:
            rec.info("flash.recovery.reset", "POST target/reset run=true -> 200")
        else:
            rec.warn("flash.recovery.reset",
                     "POST target/reset -> HTTP %d" % response.status)
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.warn("flash.recovery.reset", "target reset failed: %s" % exc)
    time.sleep(1.0)
    try:
        info = harness.http().request("GET", TARGET_INFO_PATH,
                                      retries=2, timeout=args.timeout).json()
        detected = info.get("detected") is True
        section["target_after"] = {"detected": detected, "uid": info.get("uid")}
        if detected:
            rec.pass_("flash.recovery.target_detected",
                      "target detected after recovery reset (UID=%s)"
                      % normalize_uid(info.get("uid")))
        else:
            rec.warn("flash.recovery.target_detected",
                     "target not detected after recovery reset: err=%r"
                     % (info.get("err"),))
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.warn("flash.recovery.target_detected",
                 "cannot re-probe target after recovery: %s" % exc)


def phase_flash(rec: Recorder, args: argparse.Namespace, harness: Any,
                preflight: Dict[str, Any]) -> bool:
    """Opt-in Web Flash smoke of an explicitly named test image."""
    section = rec.section("flash", {"image": os.path.abspath(args.flash_image)})
    image = os.path.abspath(os.path.expanduser(args.flash_image))
    target = preflight["info"]
    target_meta = {
        "detected": target.get("detected") is True,
        "supported": target.get("supported") is True,
        "family": target.get("family"),
        "flash_size": int(target.get("flash_size") or 0),
        "uid": normalize_uid(target.get("uid")),
        "device": target.get("device"),
    }
    errors, facts = check_image_requirements(
        image, expect_sha256=args.flash_expect_sha256,
        expect_uid=args.flash_expect_uid,
        expect_flash_kib=args.flash_expect_flash_kib,
        target=target_meta, require_h7=not args.allow_any_target)
    section.update(facts)
    section["target"] = target_meta
    rec.info("flash.preflight.identity",
             "pre-write confirmation: image=%s size=%r sha256=%s | target=%s "
             "UID=%s flash=%d KiB"
             % (os.path.basename(image), facts.get("image_size"),
                facts.get("image_sha256"), target_meta.get("device"),
                target_meta.get("uid"),
                target_meta.get("flash_size", 0) // 1024),
             values={"image": image, "image_size": facts.get("image_size"),
                     "image_sha256": facts.get("image_sha256"),
                     "target_uid": target_meta.get("uid"),
                     "target_flash_size": target_meta.get("flash_size")})
    if errors:
        rec.fail("flash.precheck", "; ".join(errors), values=facts)
        section["status"] = "refused"
        return False
    rec.pass_("flash.precheck",
              "image %s (%d B, sha256 %s) fits target UID=%s flash=%d KiB"
              % (os.path.basename(image), facts["image_size"],
                 facts["image_sha256"][:16] + "...", target_meta["uid"],
                 target_meta["flash_size"] // 1024))
    if not args.yes_flash:
        rec.dry("flash.write",
                "dry-run: POST %s with the image above would write target Flash; "
                "refused without --yes-flash (read-only validation only)"
                % TARGET_FLASH_PATH, values={"image_size": facts["image_size"]})
        section["status"] = "dry-run"
        return True
    # Re-confirm the device is still idle immediately before the write.
    fresh = harness.http().request("GET", TARGET_STATUS_PATH, retries=1,
                                   timeout=args.timeout).json()
    section["status_before_write"] = {
        "state": fresh.get("state"), "busy": fresh.get("busy"),
        "dap_session_active": fresh.get("dap_session_active"),
        "heap_free": fresh.get("heap_free"),
    }
    if fresh.get("state") not in TARGET_IDLE_STATES or fresh.get("busy") is not False:
        rec.fail("flash.precheck.idle",
                 "Flash engine state=%r busy=%r before upload; refusing to "
                 "start a second session" % (fresh.get("state"),
                                             fresh.get("busy")))
        section["status"] = "refused"
        return False
    with open(image, "rb") as handle:
        payload = handle.read()
    upload_headers = {
        "Content-Type": "application/octet-stream",
        "X-Target-SHA256": facts["image_sha256"],
    }
    result: Dict[str, Any] = {}
    upload_started = time.monotonic()

    def worker() -> None:
        try:
            result["response"] = harness.http(
                timeout=max(20.0, args.flash_timeout)).request_bytes(
                "POST", TARGET_FLASH_PATH, data=payload, headers=upload_headers,
                timeout=args.flash_timeout, retries=0)
        except BaseException as exc:  # noqa: BLE001 - reported by the main thread
            result["error"] = exc

    thread = threading.Thread(target=worker, daemon=True, name="hil-ext-flash")
    thread.start()
    poll_http = harness.http(timeout=3.0)
    polls = 0
    trail: List[Dict[str, Any]] = []
    max_percent = 0
    last_poll_error: Optional[str] = None
    deadline = upload_started + args.flash_timeout + 10.0
    while thread.is_alive():
        polls += 1
        try:
            poll = poll_http.request("GET", TARGET_STATUS_PATH, retries=0,
                                     timeout=3.0)
            data = poll.json()
            state = data.get("state")
            percent = int(data.get("percent") or 0)
            max_percent = max(max_percent, percent)
            point = {"t": round(time.monotonic() - upload_started, 2),
                     "state": state, "percent": percent,
                     "written": data.get("written"), "busy": data.get("busy")}
            trail.append(point)
            if data.get("err") not in (0, None):
                last_poll_error = "poll err=%r state=%r" % (data.get("err"), state)
        except (DeviceUnreachable, ProtocolError, ValueError) as exc:
            last_poll_error = str(exc)
        if time.monotonic() > deadline:
            rec.fail("flash.upload.timeout",
                     "POST %s did not finish within %.0f s; aborting poll loop"
                     % (TARGET_FLASH_PATH, args.flash_timeout))
            break
        time.sleep(0.5)
    thread.join(timeout=5.0)
    upload_elapsed = time.monotonic() - upload_started
    section["upload_seconds"] = round(upload_elapsed, 2)
    section["poll_count"] = polls
    section["poll_max_percent"] = max_percent
    section["poll_trail"] = trail[-40:]
    section["image"] = image
    section["image_size"] = facts.get("image_size")
    section["image_sha256"] = facts.get("image_sha256")
    section["target_uid_before"] = target_meta["uid"]
    if last_poll_error:
        section["last_poll_error"] = last_poll_error

    response = result.get("response")
    error = result.get("error")
    section["status"] = "fail"
    if response is None:
        rec.fail("flash.upload", "upload thread failed: %r" % (error,))
        _recover_target(rec, args, harness, "flash upload exception")
        return False
    section["upload"] = {"status": response.status,
                         "elapsed_ms": round(response.elapsed_ms, 1)}
    if response.status != 200:
        detail = "HTTP %d from POST %s" % (response.status, TARGET_FLASH_PATH)
        try:
            body = response.json()
            detail += ": err=%r message=%r" % (body.get("err"),
                                              body.get("message"))
        except ProtocolError:
            detail += ": body=%r" % response.body[:160]
        rec.fail("flash.upload", detail)
        _recover_target(rec, args, harness, "flash upload failed")
        return False
    try:
        data = response.json()
    except ProtocolError as exc:
        rec.fail("flash.response", str(exc))
        _recover_target(rec, args, harness, "malformed flash response")
        return False
    written = int(data.get("written") or 0)
    verified = int(data.get("verified") or 0)
    image_size = int(facts.get("image_size") or 0)
    response_sha = str(data.get("sha256") or "")
    section["response"] = {
        "ok": data.get("ok"), "written": written, "verified": verified,
        "sha256": response_sha, "family": data.get("family"),
        "uid": data.get("uid"), "state": data.get("state"),
        "message": data.get("message"),
    }
    try:
        rec.require("flash.response", data.get("ok") is True,
                    "POST /api/target/flash ok=%r state=%r message=%r"
                    % (data.get("ok"), data.get("state"), data.get("message")))
        rec.require("flash.written", written == image_size,
                    "written=%d expected image size %d" % (written, image_size))
        rec.require("flash.verified", verified == image_size,
                    "verified=%d expected image size %d" % (verified, image_size))
        rec.require("flash.sha256",
                    (not response_sha)
                    or response_sha.lower() == str(facts.get("image_sha256")).lower(),
                    "response sha256=%r does not match uploaded image sha256=%r"
                    % (response_sha, facts.get("image_sha256")))
        if data.get("uid"):
            rec.require("flash.response.uid",
                        normalize_uid(data.get("uid")) == target_meta["uid"],
                        "UID reported by flash response %r != preflight UID %r"
                        % (normalize_uid(data.get("uid")), target_meta["uid"]))
        if upload_elapsed > 0:
            section["throughput_kib_s"] = round(
                image_size / 1024.0 / upload_elapsed, 1)

        # Success: the firmware already resets/runs the target after verify; prove
        # the target is still detected and unchanged.
        time.sleep(1.0)
        try:
            after = harness.http().request("GET", TARGET_INFO_PATH, retries=2,
                                           timeout=args.timeout).json()
        except (DeviceUnreachable, ProtocolError) as exc:
            rec.fail("flash.target.detected",
                     "cannot re-probe target after successful write: %s" % exc)
            _recover_target(rec, args, harness, "post-flash probe failed")
            return False
        detected_after = after.get("detected") is True
        uid_after = normalize_uid(after.get("uid"))
        section["target_after"] = {"detected": detected_after, "uid": uid_after,
                                   "idcode": after.get("idcode")}
        rec.require("flash.target.detected", detected_after,
                    "target not detected after write: err=%r" % (after.get("err"),))
        rec.require("flash.target.uid_stable",
                    not target_meta["uid"] or uid_after == target_meta["uid"],
                    "target UID changed after write: %s -> %s"
                    % (target_meta["uid"], uid_after))
        try:
            stat = harness.http().request("GET", TARGET_STATUS_PATH, retries=1,
                                          timeout=args.timeout).json()
            rec.require("flash.target.status.idle",
                        stat.get("state") in TARGET_IDLE_STATES
                        and stat.get("busy") is False
                        and int(stat.get("err") or 0) == 0,
                        "after write: state=%r busy=%r err=%r"
                        % (stat.get("state"), stat.get("busy"), stat.get("err")))
            section["status_after"] = {"state": stat.get("state"),
                                       "err": stat.get("err"),
                                       "heap_free": stat.get("heap_free")}
        except (DeviceUnreachable, ProtocolError) as exc:
            rec.warn("flash.target.status.idle",
                     "cannot read target/status after write: %s" % exc)
        section["status"] = "pass"
        rec.info("flash.summary",
                 "wrote+verified %d B in %.2f s (%.1f KiB/s); target detected, "
                 "UID stable" % (image_size, upload_elapsed,
                                 section.get("throughput_kib_s") or 0.0),
                 values={"image_size": image_size,
                         "upload_seconds": section["upload_seconds"],
                         "poll_max_percent": max_percent})
        return True
    except SmokeAbort:
        section["status"] = "fail"
        _recover_target(rec, args, harness,
                        "flash post-write verification failed")
        raise


def _poke_ota_info(rec: Recorder, harness: Any, check_id: str,
                   required: bool, section: Dict[str, Any],
                   attempts: int = 1) -> Optional[Dict[str, Any]]:
    last_error: Optional[BaseException] = None
    for attempt in range(max(1, attempts)):
        try:
            info = harness.ota_info()
        except (OSError, ProtocolError) as exc:
            last_error = exc
            if attempt + 1 < attempts:
                time.sleep(2.0)
            continue
        section[check_id] = info
        return info
    (rec.fail if required else rec.warn)(
        check_id, "WS OTA(5)/GET_INFO: %s" % last_error)
    return None


def phase_ota(rec: Recorder, args: argparse.Namespace, harness: Any,
              preflight: Dict[str, Any]) -> bool:
    """Opt-in OTA upload smoke with reboot/disconnect handling (no blind retry)."""
    image = os.path.abspath(os.path.expanduser(args.ota_image))
    section = rec.section("ota", {"image": image})
    section["target_uid"] = preflight["uid"]
    before = _poke_ota_info(rec, harness, "ota.info.before", True, section)
    if before is None:
        section["status"] = "fail"
        return False
    rec.info("ota.before",
             "before OTA: running slot=%r version=%r status=%r free=%r "
             "slot_size=%r rollback=%r"
             % (before.get("running"), before.get("running_ver"),
                before.get("status"), before.get("free"),
                before.get("slot_size"), before.get("rollback")),
             values=before)
    rec.require("ota.idle.before", before.get("status") in ("IDLE", None),
                "OTA session busy before the test: status=%r"
                % (before.get("status"),))
    # OTA images target the ESP32, not the H7: validate them separately (the
    # H7 flash-capacity precheck in check_image_requirements does not apply).
    errors: List[str] = []
    facts: Dict[str, Any] = {"image": image}
    if not os.path.isfile(image):
        errors.append("OTA image not found: %s" % image)
    else:
        image_size = os.path.getsize(image)
        facts["image_size"] = image_size
        facts["image_sha256"] = sha256_file(image)
        if image_size <= 0:
            errors.append("OTA image is empty: %s" % image)
        elif image_size < 64 * 1024:
            errors.append("OTA image too small: %d B < 65536 B (not an "
                          "ESP application image?)" % image_size)
        else:
            with open(image, "rb") as handle:
                magic = handle.read(1)
            if magic != b"\xe9":
                errors.append("OTA image does not start with the ESP application "
                              "magic 0xE9 (first byte=0x%02x)"
                              % (magic[0] if magic else 0))
    section.update(facts)
    image_size = int(facts.get("image_size") or 0)
    image_sha = str(facts.get("image_sha256") or "")
    if before.get("slot_size"):
        rec.require("ota.image.fits", image_size <= int(before["slot_size"]),
                    "image %d B > OTA slot %d B"
                    % (image_size, int(before["slot_size"])))
    if errors:
        rec.fail("ota.precheck", "; ".join(errors))
        section["status"] = "refused"
        return False
    rec.info("ota.preflight.identity",
             "pre-write confirmation: image=%s size=%d sha256=%s | device "
             "running=%r ver=%r rollback=%r"
             % (os.path.basename(image), image_size, image_sha,
                before.get("running"), before.get("running_ver"),
                before.get("rollback")),
             values={"image": image, "image_size": image_size,
                     "image_sha256": image_sha, "running_ver":
                     before.get("running_ver")})
    if not args.yes_ota:
        rec.dry("ota.write",
                "dry-run: POST %s (%d B) would OTA the device from %r to the "
                "image above; refused without --yes-ota (read-only validation)"
                % (OTA_UPLOAD_PATH, image_size, before.get("running_ver")))
        section["status"] = "dry-run"
        return True
    # Re-confirm the device is still idle right before the upload.
    fresh = harness.http().request("GET", TARGET_STATUS_PATH, retries=1,
                                   timeout=args.timeout).json()
    if fresh.get("state") not in TARGET_IDLE_STATES or fresh.get("busy") is not False \
            or fresh.get("dap_session_active") is True:
        rec.fail("ota.precheck.idle",
                 "before OTA: state=%r busy=%r dap_session_active=%r"
                 % (fresh.get("state"), fresh.get("busy"),
                    fresh.get("dap_session_active")))
        section["status"] = "refused"
        return False
    with open(image, "rb") as handle:
        payload = handle.read()
    upload_started = time.monotonic()
    try:
        response = harness.http(timeout=max(20.0, args.ota_timeout)).request_bytes(
            "POST", OTA_UPLOAD_PATH, data=payload,
            headers={"Content-Type": "application/octet-stream",
                     "X-SHA256": image_sha},
            timeout=args.ota_timeout, retries=0)
    except DeviceUnreachable as exc:
        rec.fail("ota.upload", "upload transport failed: %s" % exc)
        section["status"] = "fail"
        _ota_after_failure(rec, args, harness, section, before)
        return False
    upload_elapsed = time.monotonic() - upload_started
    section["upload_seconds"] = round(upload_elapsed, 2)
    section["bytes_per_second"] = round(image_size / max(upload_elapsed, 0.001), 1)
    if response.status != 200:
        detail = "HTTP %d from POST %s" % (response.status, OTA_UPLOAD_PATH)
        try:
            body = response.json()
            detail += ": err=%r code=%r" % (body.get("err"), body.get("code"))
        except ProtocolError:
            detail += ": body=%r" % response.body[:160]
        rec.fail("ota.upload", detail)
        section["status"] = "fail"
        _ota_after_failure(rec, args, harness, section, before)
        return False
    try:
        data = response.json()
    except ProtocolError as exc:
        rec.fail("ota.upload", "malformed upload response: %s" % exc)
        section["status"] = "fail"
        _ota_after_failure(rec, args, harness, section, before)
        return False
    section["upload_response"] = {
        "ok": data.get("ok"), "slot": data.get("slot"),
        "running": data.get("running"), "running_ver": data.get("running_ver"),
        "offset": data.get("offset"), "total": data.get("total"),
        "sha256": data.get("sha256"), "status": data.get("status"),
        "reboot_in_ms": data.get("reboot_in_ms"),
    }
    if data.get("ok") is not True:
        rec.fail("ota.upload",
                 "upload response ok=%r status=%r err=%r"
                 % (data.get("ok"), data.get("status"), data.get("err")))
        section["status"] = "fail"
        _ota_after_failure(rec, args, harness, section, before)
        return False
    rec.pass_("ota.upload",
              "OTA upload HTTP 200 ok=true: slot=%r status=%r "
              "reboot_in_ms=%r"
              % (data.get("slot"), data.get("status"),
                 data.get("reboot_in_ms")),
              values=section["upload_response"])
    rec.check("ota.upload.bytes",
              int(data.get("total") or 0) in (image_size, 0)
              and int(data.get("offset") or 0) in (image_size, 0),
              "offset=%r total=%r expected %d"
              % (data.get("offset"), data.get("total"), image_size))
    if data.get("sha256"):
        rec.check("ota.upload.sha256",
                  str(data.get("sha256")).lower() == image_sha.lower(),
                  "upload sha256=%r != image sha256=%r"
                  % (data.get("sha256"), image_sha))
    target_slot = data.get("slot")
    section["target_slot"] = target_slot

    # -- tolerate the reboot / disconnect, then poll OTA info ----------------
    rec.info("ota.reboot.wait", "waiting up to %.0f s for the device reboot "
             "(HTTP disconnect/reconnect is expected)"
             % args.ota_reboot_timeout)
    deadline = time.monotonic() + args.ota_reboot_timeout
    down_seen = False
    reboot_seconds: Optional[float] = None
    reachable = False
    reboot_started = time.monotonic()
    while time.monotonic() < deadline:
        try:
            probe = harness.http(timeout=2.0).request(
                "GET", TARGET_STATUS_PATH, retries=0, timeout=2.0)
            reachable = probe.status == 200 and probe.status != 0
        except DeviceUnreachable:
            reachable = False
        if not reachable:
            down_seen = True
        elif down_seen:
            reboot_seconds = round(time.monotonic() - reboot_started, 2)
            break
        time.sleep(0.5)
    section["reboot"] = {"disconnect_seen": down_seen,
                         "reachable_after": reachable,
                         "reboot_seconds": reboot_seconds}
    rec.require("ota.reboot.device_up",
                reachable or not down_seen,
                "device did not answer /api/target/status within %.0f s after "
                "the OTA reboot" % args.ota_reboot_timeout)
    if down_seen and reachable:
        rec.pass_("ota.reboot.recovered",
                  "device disconnected and came back in %r s after the OTA "
                  "reboot" % reboot_seconds)
    elif not down_seen:
        rec.warn("ota.reboot.observed",
                 "reboot was too fast to observe a disconnect (upload "
                 "response arrived after %r s); continuing with version/slot "
                 "verification" % section["upload_seconds"])
    time.sleep(1.0)
    # Web UI still served?
    try:
        page = harness.http().request("GET", "/", retries=1, timeout=args.timeout)
        rec.require("ota.web.served", page.status == 200
                    and b"<!doctype html" in page.body.lower(),
                    "GET / after OTA -> HTTP %d (%d B)"
                    % (page.status, len(page.body)))
        section["web_bytes"] = len(page.body)
        rec.info("ota.web.served", "GET / after OTA -> 200, %d B" % len(page.body))
    except DeviceUnreachable as exc:
        rec.fail("ota.web.served", "GET / after OTA failed: %s" % exc)
    after = _poke_ota_info(rec, harness, "ota.info.after", True, section,
                           attempts=5)
    slot_ok = False
    version_changed = False
    if after is not None:
        slot_ok = bool(target_slot) and after.get("running") == target_slot
        version_changed = after.get("running_ver") != before.get("running_ver")
        rec.require("ota.slot.after", slot_ok,
                    "after reboot the running slot is %r, expected the upload "
                    "target slot %r (before: %r)"
                    % (after.get("running"), target_slot,
                       before.get("running")))
        rec.info("ota.version.after",
                 "running version %r -> %r (slot %r -> %r)"
                 % (before.get("running_ver"), after.get("running_ver"),
                    before.get("running"), after.get("running")),
                 values={"before_ver": before.get("running_ver"),
                         "after_ver": after.get("running_ver"),
                         "version_changed": version_changed})
        rec.require("ota.status.idle", after.get("status") in ("IDLE", None),
                    "OTA status after reboot=%r" % (after.get("status"),))
    section["version_changed"] = version_changed
    # Rollback stability: after the settle window the device must still run the
    # newly booted slot (no automatic rollback to the previous slot).
    if args.ota_settle > 0:
        time.sleep(args.ota_settle)
        settled = _poke_ota_info(rec, harness, "ota.info.settled", False, section)
        if settled is not None and after is not None:
            rec.require("ota.rollback.stable",
                        settled.get("running") == after.get("running")
                        and settled.get("status") in ("IDLE", None),
                        "after %.0f s settle the running slot/status changed "
                        "(%r/%r -> %r/%r), possible rollback"
                        % (args.ota_settle, after.get("running"),
                           after.get("status"), settled.get("running"),
                           settled.get("status")))
            section["settled"] = settled
    # Device/target health after the OTA.
    try:
        stat = harness.http().request("GET", TARGET_STATUS_PATH, retries=1,
                                      timeout=args.timeout).json()
        rec.require("ota.health.status", stat.get("ok") is True
                    and int(stat.get("err") or 0) == 0,
                    "target/status after OTA: ok=%r err=%r"
                    % (stat.get("ok"), stat.get("err")))
        section["status_after"] = {"heap_free": stat.get("heap_free"),
                                   "heap_largest": stat.get("heap_largest"),
                                   "state": stat.get("state"),
                                   "wifi_rssi": stat.get("wifi_rssi")}
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.fail("ota.health.status", "target/status after OTA: %s" % exc)
    try:
        info = harness.http().request("GET", TARGET_INFO_PATH, retries=2,
                                      timeout=args.timeout).json()
        detected = info.get("detected") is True
        uid_after = normalize_uid(info.get("uid"))
        rec.require("ota.health.target", detected,
                    "target not detected after the ESP32 OTA reboot: err=%r"
                    % (info.get("err"),))
        if preflight["uid"]:
            rec.require("ota.health.uid", uid_after == preflight["uid"],
                        "target UID changed across the OTA reboot: %s -> %s"
                        % (preflight["uid"], uid_after))
        section["target_after"] = {"detected": detected, "uid": uid_after}
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.fail("ota.health.target", "target/info after OTA: %s" % exc)
    section["status"] = "pass" if not rec.failed else "fail"
    rec.info("ota.summary",
             "OTA upload %d B in %.2f s; slot %r -> %r, version %r -> %r, "
             "version_changed=%r"
             % (image_size, upload_elapsed, before.get("running"),
                (after or {}).get("running"), before.get("running_ver"),
                (after or {}).get("running_ver"), version_changed),
             values={"image_size": image_size,
                     "upload_seconds": section["upload_seconds"],
                     "target_slot": target_slot})
    return not rec.failed


def _ota_after_failure(rec: Recorder, args: argparse.Namespace, harness: Any,
                       section: Dict[str, Any],
                       before: Dict[str, Any]) -> None:
    """Report the device state after a failed OTA (no further upload attempts)."""
    deadline = time.monotonic() + min(30.0, args.ota_reboot_timeout)
    reachable = False
    while time.monotonic() < deadline:
        try:
            reachable = harness.http(timeout=2.0).request(
                "GET", TARGET_STATUS_PATH, retries=0, timeout=2.0).status == 200
        except DeviceUnreachable:
            reachable = False
        if reachable:
            break
        time.sleep(1.0)
    section["failure_recovery"] = {"reachable": reachable}
    if not reachable:
        rec.fail("ota.failure.reachable",
                 "device did not become reachable after the failed OTA "
                 "(do NOT blindly retry; inspect serial/Wi-Fi)")
        return
    info = _poke_ota_info(rec, harness, "ota.failure.info", False, section)
    if info is not None:
        same = info.get("running") == before.get("running") \
            and info.get("running_ver") == before.get("running_ver")
        section["failure_recovery"]["still_old_slot"] = same
        if same:
            rec.info("ota.failure.old_slot",
                     "OTA failed but the device still runs the previous slot "
                     "(%r / %r) - no boot-slot change, retry is explicit only"
                     % (info.get("running"), info.get("running_ver")))
        else:
            rec.warn("ota.failure.old_slot",
                     "after the failed OTA the device reports running slot=%r "
                     "version=%r (changed from %r/%r); verify manually"
                     % (info.get("running"), info.get("running_ver"),
                        before.get("running"), before.get("running_ver")))


def openocd_probe_commands(args: argparse.Namespace) -> List[str]:
    return [
        args.openocd_path,
        "-s", args.openocd_scripts,
        "-f", "interface/cmsis-dap.cfg",
        "-c", "adapter usb vid_pid 0xc251 0xf00a",
        "-c", "transport select swd",
        "-f", "target/stm32h7x.cfg",
        "-c", "adapter speed 1000",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
        "-c", "init",
        "-c", "shutdown",
    ]


def phase_usbip(rec: Recorder, args: argparse.Namespace, harness: Any,
                preflight: Dict[str, Any]) -> bool:
    """Optional host USBIP attach/detach + OpenOCD identify smoke (skip if N/A)."""
    section = rec.section("usbip", {"host": args.usbip_host or args.host,
                                    "busid": args.usbip_busid})
    helper = harness.root_helper(args.root_helper)
    if helper is None:
        reason = ("no root helper for 'usbip attach' (set HIL_ROOT_HELPER or "
                  "--root-helper; passwordless sudo not available)")
        rec.skip("usbip.environment.root_helper", reason)
        section["status"] = "skipped"
        section["reason"] = reason
        return True
    if not harness.usbip_available():
        reason = "host 'usbip' tool not found; USBIP smoke skipped"
        rec.skip("usbip.environment.usbip", reason)
        section["status"] = "skipped"
        section["reason"] = reason
        return True
    binary, scripts, reason = harness.openocd_environment()
    if reason is not None:
        rec.skip("usbip.environment.openocd", reason)
        section["status"] = "skipped"
        section["reason"] = reason
        return True
    section["root_helper"] = " ".join(helper)
    section["openocd"] = binary
    remote = args.usbip_host or args.host
    attached = False

    def detach() -> None:
        result = harness.run(helper + ["usbip", "detach", "-p",
                                       "%02d" % args.usbip_port_index], 30)
        section["detach"] = result
        rec.info("usbip.detach", "usbip detach -p %02d rc=%d"
                 % (args.usbip_port_index, result["rc"]))

    try:
        detach()  # clear a stale local attachment first (rc ignored)
        attach = harness.run(helper + ["usbip", "attach", "-r", remote,
                                       "-b", args.usbip_busid], 45)
        section["attach"] = attach
        if attach["rc"] != 0:
            rec.fail("usbip.attach",
                     "usbip attach -r %s -b %s failed (rc=%d): %s"
                     % (remote, args.usbip_busid, attach["rc"],
                        _short(attach["output"])))
            return False
        attached = True
        rec.pass_("usbip.attach", "usbip attach -r %s -b %s rc=0"
                  % (remote, args.usbip_busid))
        time.sleep(1.0)
        session = harness.run(helper + openocd_probe_commands(args),
                              args.openocd_timeout)
        version = harness.run(helper + [binary, "--version"], 20)
        section["openocd_version"] = _short(version.get("output", ""), 120)
        output = session.get("output", "")
        session_ok = (session["rc"] == 0 and "CMSIS-DAP" in output
                      and "Interface Initialised" in output)
        section["session"] = {"rc": session["rc"],
                              "seconds": session["seconds"],
                              "ok": session_ok,
                              "tail": _short(output)}
        # Known current-firmware limitation (docs/hil-extended.md §8.3): the
        # first USBIP session after an HTTP target/info probe can fail with
        # "CMSIS-DAP command CMD_INFO failed" because the DAP ring buffers are
        # re-created on a fragmented heap; a device reboot restores the session.
        # It is recorded as a WARNING by default (so it cannot block the main
        # read-only/soak/flash/OTA smoke) but is NEVER reported as PASS/verified;
        # --usbip-strict makes it a required failure for dedicated CI runs.
        known_issue: Optional[str] = None
        if not session_ok and "CMD_INFO failed" in output:
            known_issue = (
                "known firmware limitation: CMSIS-DAP CMD_INFO fails for the "
                "first USBIP session right after an HTTP target/info probe "
                "(DAP ring re-init on fragmented heap); reboot the ESP32 and "
                "run USBIP-only/openocd before any target probe to recover. "
                "This USBIP identify is NOT verified.")
            section["known_issue"] = known_issue
        if known_issue is not None and not args.usbip_strict:
            rec.warn("usbip.openocd.session",
                     "OpenOCD CMSIS-DAP session failed (rc=%d): %s | %s"
                     % (session["rc"], _short(output), known_issue))
            section["status"] = "warn-known-issue"
        else:
            rec.require("usbip.openocd.session", session_ok,
                        "OpenOCD CMSIS-DAP session failed (rc=%d): %s"
                        % (session["rc"], _short(output)))
            target_seen = session_ok and any(marker in output for marker in
                                             ("STM32H7", "cortex_m", "Cortex-M7"))
            rec.require("usbip.openocd.identify", target_seen,
                        "OpenOCD session did not identify the H7 target: %s"
                        % _short(output))
            if target_seen:
                rec.info("usbip.openocd.identify",
                         "OpenOCD identified the STM32H7 target over USBIP")
            section["status"] = "pass"
    finally:
        detach()
        try:
            status = harness.http().request("GET", TARGET_STATUS_PATH,
                                            retries=1, timeout=args.timeout).json()
            released = status.get("dap_session_active") is not True
            section["dap_session_active"] = status.get("dap_session_active")
            if released:
                rec.info("usbip.dap.session.released",
                         "dap_session_active=false after usbip detach")
            else:
                rec.fail("usbip.dap.session.released",
                         "dap_session_active=%r after usbip detach; a session "
                         "may be left attached"
                         % (status.get("dap_session_active"),))
        except (DeviceUnreachable, ProtocolError) as exc:
            rec.warn("usbip.dap.session.released",
                     "cannot verify DAP session cleanup: %s" % exc)
        try:
            info = harness.http().request("GET", TARGET_INFO_PATH, retries=2,
                                          timeout=args.timeout).json()
            section["target_after"] = {"detected": info.get("detected"),
                                       "uid": normalize_uid(info.get("uid"))}
            if info.get("detected") is True:
                rec.info("usbip.target.still_detected",
                         "target detected after USBIP/OpenOCD smoke")
            else:
                rec.fail("usbip.target.still_detected",
                         "target not detected after USBIP/OpenOCD smoke: err=%r"
                         % (info.get("err"),))
        except (DeviceUnreachable, ProtocolError) as exc:
            rec.warn("usbip.target.still_detected",
                     "cannot re-probe target after USBIP smoke: %s" % exc)
    return not rec.failed


def phase_final(rec: Recorder, args: argparse.Namespace, harness: Any) -> None:
    """Final cleanup/health checks: curve stopped, target alive, DAP released."""
    section = rec.section("final", {})
    try:
        status = harness.http().request("GET", CURVE_STATUS_PATH, retries=0,
                                        timeout=args.timeout)
        curve = status.json()
        if curve.get("running") is True:
            harness.http().request("POST", CURVE_STOP_PATH, body={},
                                   retries=1, timeout=args.timeout)
            curve = harness.http().request("GET", CURVE_STATUS_PATH, retries=1,
                                           timeout=args.timeout).json()
            section["forced_curve_stop"] = True
        rec.require("final.curve.stopped", curve.get("running") is False,
                    "curve sampler still running at the end of the smoke; the "
                    "target may be left halted")
        section["curve"] = {"running": curve.get("running"),
                            "error": curve.get("error")}
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.warn("final.curve.stopped",
                 "cannot verify curve sampler cleanup: %s" % exc)
    try:
        stat = harness.http().request("GET", TARGET_STATUS_PATH, retries=1,
                                      timeout=args.timeout).json()
        section["device"] = {"state": stat.get("state"),
                             "err": stat.get("err"),
                             "heap_free": stat.get("heap_free"),
                             "dap_session_active": stat.get("dap_session_active"),
                             "wifi_rssi": stat.get("wifi_rssi")}
        rec.require("final.device.ok", stat.get("ok") is True
                    and stat.get("state") in TARGET_IDLE_STATES
                    and int(stat.get("err") or 0) == 0,
                    "device state after smoke: ok=%r state=%r err=%r"
                    % (stat.get("ok"), stat.get("state"), stat.get("err")))
        rec.require("final.dap_session.released",
                    stat.get("dap_session_active") is not True,
                    "dap_session_active=%r: a USBIP/WS-DAP client session is "
                    "still connected" % (stat.get("dap_session_active"),))
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.fail("final.device.ok", "cannot read target/status at the end: %s" % exc)
    try:
        info = harness.http().request("GET", TARGET_INFO_PATH, retries=2,
                                      timeout=args.timeout).json()
        section["target"] = {"detected": info.get("detected"),
                             "uid": normalize_uid(info.get("uid")),
                             "series": info.get("series")}
        rec.require("final.target.detected",
                    info.get("detected") is True,
                    "target not detected at the end of the smoke: err=%r"
                    % (info.get("err"),),
                    ok_detail="target detected at the end (UID=%s)"
                              % normalize_uid(info.get("uid")))
    except (DeviceUnreachable, ProtocolError) as exc:
        rec.fail("final.target.detected",
                 "cannot re-probe the target at the end: %s" % exc)
    rec.info("final.summary",
             "curve stopped, target detected, DAP session released")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def _device_modes_requested(args: argparse.Namespace) -> List[str]:
    modes = []
    if args.read_only:
        modes.append("read-only")
    if args.soak:
        modes.append("soak")
    if args.flash_image:
        modes.append("flash")
    if args.ota_image:
        modes.append("ota")
    if args.usbip:
        modes.append("usbip")
    return modes


def _report(rec: Recorder, args: argparse.Namespace, started_at: str,
            duration_s: float, exit_code: int,
            error: Optional[str]) -> Dict[str, Any]:
    required = [check for check in rec.checks if check.get("severity") == "required"]
    passed = [check for check in required if check["ok"]]
    skipped = [check for check in rec.checks if check.get("skipped")]
    dry_runs = [check for check in rec.checks if check.get("dry_run")]
    return {
        "tool": TOOL_NAME,
        "version": TOOL_VERSION,
        "ok": exit_code == EXIT_PASS,
        "exit_code": exit_code,
        "dry_run": bool(args.dry_run or (
            (args.flash_image and not args.yes_flash)
            or (args.ota_image and not args.yes_ota))),
        "offline": bool(args.dry_run),
        "fake_device": bool(getattr(args, "_fake_device", False)),
        "host": args.host,
        "port": args.port,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "duration_s": round(duration_s, 3),
        "modes": {
            "requested": _device_modes_requested(args),
            "read_only": bool(args.read_only),
            "soak": bool(args.soak),
            "duration_s": args.duration if args.soak else None,
            "flash": bool(args.flash_image),
            "flash_write_allowed": bool(args.yes_flash and args.flash_image),
            "ota": bool(args.ota_image),
            "ota_write_allowed": bool(args.yes_ota and args.ota_image),
            "usbip": bool(args.usbip),
            "usbip_strict": bool(args.usbip_strict),
            "usbip_skipped_target_probe": bool(
                getattr(args, "_target_probe_skipped", False)),
        },
        "images": {
            "flash": args.flash_image,
            "ota": args.ota_image,
        },
        "thresholds": {
            "interval_us": args.interval_us,
            "soak_series": args.soak_series,
            "soak_run_s": args.soak_run,
            "fps_note": "soak checks frames/errors/heap, not a hard FPS threshold",
            "heap_min_free": args.heap_min_free,
            "heap_min_largest": args.heap_min_largest,
            "heap_leak_tolerance": args.heap_leak_tolerance,
            "max_invalid_ratio": args.max_invalid_ratio,
            "poll_interval": args.poll_interval,
        },
        "summary": {
            "checks": len(rec.checks),
            "required": len(required),
            "required_passed": len(passed),
            "required_failed": len(required) - len(passed),
            "warnings": len(rec.warnings),
            "skipped": len(skipped),
            "dry_run_checks": len(dry_runs),
        },
        "checks": rec.checks,
        "sections": rec.sections,
        "warnings": rec.warnings,
        "error": error,
    }


def _human_report(report: Dict[str, Any], stream: Any = sys.stdout) -> None:
    status = "PASS" if report["ok"] else "FAIL"
    print("=== WiFi Dap extended HIL smoke ===", file=stream)
    print("host: %s:%d   modes: %s   dry_run=%r"
          % (report["host"], report["port"],
             ", ".join(report["modes"]["requested"]) or "offline self-test",
             report["dry_run"]), file=stream)
    print("", file=stream)
    for check in report["checks"]:
        if check.get("skipped"):
            tag = "SKIP"
        elif check.get("dry_run"):
            tag = "DRY"
        elif check["ok"]:
            tag = "PASS" if check["severity"] == "required" else "INFO"
        else:
            tag = "WARN" if check["severity"] == "warn" else "FAIL"
        print("[%s] %-40s %s" % (tag, check["id"], check["detail"]),
              file=stream)
    for name, section in (report.get("sections") or {}).items():
        if not isinstance(section, dict):
            continue
        if name == "preflight":
            heap = section.get("heap") or {}
            target = section.get("target") or {}
            print("preflight: target=%s UID=%s heap_free=%r heap_largest=%r"
                  % (target.get("device"), target.get("uid"),
                     heap.get("free"), heap.get("largest")), file=stream)
        elif name == "soak":
            print("soak: cycles=%r failures=%r frames=%r heap=%r..%r "
                  "invalid=%r"
                  % (section.get("cycles"), section.get("failures"),
                     section.get("frames"), section.get("heap_free_min"),
                     section.get("heap_free_end"), section.get("invalid")),
                  file=stream)
        elif name == "flash" and section.get("status") in ("pass", "dry-run"):
            print("flash: image=%r size=%r written=%r verified=%r %.2fs "
                  "poll_max_percent=%r"
                  % (section.get("image"), section.get("image_size"),
                     (section.get("response") or {}).get("written"),
                     (section.get("response") or {}).get("verified"),
                     section.get("upload_seconds") or 0.0,
                     section.get("poll_max_percent")), file=stream)
        elif name == "ota" and section.get("status") in ("pass", "dry-run"):
            print("ota: image=%r size=%r running=%r->%r version=%r->%r "
                  "reboot=%r"
                  % (section.get("image"), section.get("image_size"),
                     (section.get("ota.info.before") or {}).get("running"),
                     (section.get("ota.info.after") or {}).get("running"),
                     (section.get("ota.info.before") or {}).get("running_ver"),
                     (section.get("ota.info.after") or {}).get("running_ver"),
                     (section.get("reboot") or {}).get("reboot_seconds")),
                  file=stream)
        elif name == "usbip" and section.get("status") == "pass":
            print("usbip: attach+OpenOCD identify OK, detached", file=stream)
    for warning in report["warnings"]:
        print("warning: %s" % warning, file=stream)
    summary = report["summary"]
    print("", file=stream)
    print("RESULT: %s  (required %d/%d passed, %d warning(s), %d skipped, "
          "%d dry-run check(s), exit %d)"
          % (status, summary["required_passed"], summary["required"],
             summary["warnings"], summary["skipped"],
             summary["dry_run_checks"], report["exit_code"]), file=stream)
    if report.get("error"):
        print("error: %s" % report["error"], file=stream)


def _write_report(report: Dict[str, Any], path: str) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    try:
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=False)
            handle.write("\n")
        print("[hil_extended] JSON report written to %s" % path,
              file=sys.stderr)
    except OSError as exc:
        print("[hil_extended] warning: cannot write report %s: %s"
              % (path, exc), file=sys.stderr)


def run_pipeline(args: argparse.Namespace, harness: Any) -> Dict[str, Any]:
    """Run preflight + every requested mode; always emit a complete report."""
    rec = Recorder()
    started_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    started = time.monotonic()
    error: Optional[str] = None
    exit_code = EXIT_PASS
    args._fake_device = bool(getattr(harness, "offline", False))
    preflight: Optional[Dict[str, Any]] = None

    only_usbip = bool(args.usbip and not (args.soak or args.flash_image
                                          or args.ota_image or args.read_only))
    args._target_probe_skipped = only_usbip
    try:
        preflight = phase_preflight(rec, args, harness,
                                    probe_target=not only_usbip)
        abort_writes = False
        if args.soak:
            ok = phase_soak(rec, args, harness, preflight)
            if not ok:
                abort_writes = True
        if args.flash_image:
            if abort_writes:
                rec.skip("flash.aborted",
                         "skipped: an earlier read-only soak failed; refusing "
                         "a target Flash write on an unhealthy baseline")
                rec.section("flash", {"status": "skipped"})
            else:
                ok = phase_flash(rec, args, harness, preflight)
                abort_writes = abort_writes or not ok
        if args.ota_image:
            if abort_writes:
                rec.skip("ota.aborted",
                         "skipped: an earlier soak/Flash check failed; "
                         "refusing an OTA on an unhealthy baseline")
                rec.section("ota", {"status": "skipped"})
            else:
                ok = phase_ota(rec, args, harness, preflight)
                abort_writes = abort_writes or not ok
        if args.usbip:
            if abort_writes:
                rec.skip("usbip.aborted",
                         "skipped: an earlier check failed; USBIP smoke not run")
                rec.section("usbip", {"status": "skipped"})
            else:
                phase_usbip(rec, args, harness, preflight)
        if args.read_only:
            rec.pass_("read_only.ok",
                      "device read-only preflight passed (target/status/curve/"
                      "heap/diagnostics)")
    except DeviceUnreachable as exc:
        error = "device unreachable: %s" % exc
        rec.fail("device.unreachable", error)
        exit_code = EXIT_UNREACHABLE
    except SmokeAbort as exc:
        error = "aborted at %s: %s" % (exc.check_id, exc.detail)
    except Exception as exc:  # noqa: BLE001 - a crash must still emit a report
        error = "unexpected error: %s: %s" % (type(exc).__name__, exc)
        rec.fail("internal", error)
        if args.verbose:
            traceback.print_exc()

    # Cleanup/health verification: always attempted unless the device is gone.
    if exit_code != EXIT_UNREACHABLE:
        try:
            phase_final(rec, args, harness)
        except DeviceUnreachable as exc:
            exit_code = EXIT_UNREACHABLE
            error = error or ("device unreachable during final checks: %s" % exc)
        except Exception as exc:  # noqa: BLE001
            rec.fail("final.internal",
                     "final checks failed: %s: %s" % (type(exc).__name__, exc))

    if exit_code == EXIT_PASS and (rec.failed or error):
        exit_code = EXIT_FAIL
    report = _report(rec, args, started_at, time.monotonic() - started,
                     exit_code, error)
    return report


# ---------------------------------------------------------------------------
# --dry-run: offline self-test with the in-process fake device
# ---------------------------------------------------------------------------

def _selftest_image(directory: str, name: str, size: int,
                    seed: int = 0, first_byte: Optional[int] = None) -> str:
    path = os.path.join(directory, name)
    with open(path, "wb") as handle:
        handle.write(bytes((index * 31 + seed) & 0xFF for index in range(size)))
        if first_byte is not None:
            handle.seek(0)
            handle.write(bytes([first_byte & 0xFF]))
    return path


def _scenario_args(base: argparse.Namespace, **overrides: Any) -> argparse.Namespace:
    args = argparse.Namespace(**vars(base))
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


def run_selftest(args: argparse.Namespace) -> int:
    """Offline self-test: fake device scenarios with expected exit codes."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="hil-extended-selftest.") as tmpdir:
        flash_image = _selftest_image(tmpdir, "h7_test.bin", 4096, seed=1)
        ota_image = _selftest_image(tmpdir, "ota_test.bin", 128 * 1024,
                                    seed=2, first_byte=0xE9)
        flash_sha = sha256_file(flash_image)
        scenarios: List[Tuple[str, Dict[str, Any], FakeHarness, int]] = [
            ("healthy-all-modes", dict(
                dry_run=True, soak=True, duration=1.0, soak_run=0.25,
                flash_image=flash_image, yes_flash=True,
                flash_expect_sha256=flash_sha, ota_image=ota_image,
                yes_ota=True, usbip=True, ota_settle=0.0,
                ota_reboot_timeout=5.0, flash_timeout=20.0),
             FakeHarness(), EXIT_PASS),
            ("flash-without-yes-flash", dict(
                dry_run=True, flash_image=flash_image, yes_flash=False),
             FakeHarness(), EXIT_PASS),
            ("flash-wrong-expect-sha256", dict(
                dry_run=True, flash_image=flash_image, yes_flash=True,
                flash_expect_sha256="0" * 64),
             FakeHarness(), EXIT_FAIL),
            ("target-not-detected", dict(
                dry_run=True, flash_image=flash_image, yes_flash=True),
             FakeHarness(FakeDevice(detected=False)), EXIT_FAIL),
            ("flash-failure-recovery", dict(
                dry_run=True, flash_image=flash_image, yes_flash=True,
                flash_timeout=20.0),
             FakeHarness(FakeDevice(flash_status=500)), EXIT_FAIL),
            ("ota-without-yes-ota", dict(
                dry_run=True, ota_image=ota_image, yes_ota=False),
             FakeHarness(), EXIT_PASS),
            ("ota-failure-old-slot-kept", dict(
                dry_run=True, ota_image=ota_image, yes_ota=True,
                ota_settle=0.0, ota_reboot_timeout=5.0),
             FakeHarness(FakeDevice(ota_status=503)), EXIT_FAIL),
            ("soak-sampler-error", dict(
                dry_run=True, soak=True, duration=1.0, soak_run=0.25),
             FakeHarness(FakeDevice(curve_error=True)), EXIT_FAIL),
            ("usbip-environment-unavailable", dict(
                dry_run=True, usbip=True),
             FakeHarness(root_helper=False), EXIT_PASS),
            ("dry-run-write-not-uploaded", dict(
                dry_run=True, flash_image=flash_image, yes_flash=False,
                ota_image=ota_image, yes_ota=False),
             FakeHarness(), EXIT_PASS),
        ]
        results: List[Dict[str, Any]] = []
        failures = 0
        for name, overrides, harness, expected in scenarios:
            scenario_args = _scenario_args(args, **overrides)
            scenario_args.host = "dry-run"
            scenario_args.port = 0
            scenario_args.report = None
            scenario_args.verbose = False
            report = run_pipeline(scenario_args, harness)
            actual = int(report["exit_code"])
            extra_errors: List[str] = []
            if actual != expected:
                extra_errors.append("exit %d != expected %d" % (actual, expected))
            if name == "flash-without-yes-flash" and harness.device.flash_attempts:
                extra_errors.append("dry-run uploaded the image without --yes-flash")
            if name == "flash-wrong-expect-sha256" and harness.device.flash_attempts:
                extra_errors.append("wrong-sha precheck still uploaded")
            if name == "target-not-detected" and harness.device.flash_attempts:
                extra_errors.append("write attempted with no detected target")
            if name == "dry-run-write-not-uploaded" and (
                    harness.device.flash_attempts or harness.device.ota_attempts):
                extra_errors.append("write mode ran without safety switch")
            if name == "flash-failure-recovery":
                if harness.device.flash_attempts != 1:
                    extra_errors.append("expected exactly one flash attempt, got %d"
                                        % harness.device.flash_attempts)
                if harness.device.reset_calls < 1:
                    extra_errors.append("no recovery reset after failed flash")
                if not harness.device.detected:
                    extra_errors.append("fake target not detected after recovery")
            if name == "ota-failure-old-slot-kept":
                if harness.device.ota_attempts != 1:
                    extra_errors.append("expected exactly one OTA attempt (no "
                                        "blind retry), got %d"
                                        % harness.device.ota_attempts)
                if harness.device.running_slot != "ota_0":
                    extra_errors.append("failed OTA changed the running slot")
            if name == "usbip-environment-unavailable":
                if not any(check.get("id") == "usbip.environment.root_helper"
                           and check.get("skipped") for check in report["checks"]):
                    extra_errors.append("usbip unavailable was not recorded as SKIP")
            if name == "healthy-all-modes":
                if harness.device.flash_attempts != 1:
                    extra_errors.append("healthy scenario did not flash once")
                if harness.device.ota_attempts != 1:
                    extra_errors.append("healthy scenario did not OTA once")
                if not any(check.get("id") == "flash.response" and check["ok"]
                           for check in report["checks"]):
                    extra_errors.append("missing flash.response check")
                if not any(check.get("id") == "ota.slot.after" and check["ok"]
                           for check in report["checks"]):
                    extra_errors.append("missing ota.slot.after check")
                if not any(check.get("id") == "usbip.openocd.session"
                           and check["ok"] for check in report["checks"]):
                    extra_errors.append("missing usbip.openocd.session check")
                if not report["dry_run"]:
                    extra_errors.append("report must be marked dry_run")
            ok = not extra_errors
            if not ok:
                failures += 1
            results.append({
                "scenario": name,
                "expected_exit": expected,
                "actual_exit": actual,
                "ok": ok,
                "problems": extra_errors,
                "required_failed": report["summary"]["required_failed"],
                "skipped": report["summary"]["skipped"],
            })
            print("[dry-run] %-30s expected=%d actual=%d %s%s"
                  % (name, expected, actual, "OK" if ok else "MISMATCH",
                     "" if ok else " (" + "; ".join(extra_errors) + ")"),
                  file=sys.stderr)
    summary = {
        "tool": TOOL_NAME,
        "version": TOOL_VERSION,
        "dry_run": True,
        "selftest": True,
        "ok": failures == 0,
        "scenarios": results,
        "failures": failures,
    }
    if getattr(args, "report", None):
        _write_report(summary, args.report)
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print("dry-run self-test scenarios: %d/%d as expected"
              % (len(results) - failures, len(results)))
    return EXIT_PASS if failures == 0 else EXIT_FAIL


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hil_extended.py",
        description="Opt-in extended HIL smoke for the WiFi Dap: "
                    "read-only preflight, curve soak, Web Flash smoke, OTA "
                    "smoke and optional USBIP/OpenOCD identify. Default is an "
                    "offline dry-run self-test; write modes need a second "
                    "safety switch (--yes-flash / --yes-ota).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Exit codes: 0 PASS, 1 FAIL, 2 usage, 3 device unreachable. "
               "The default read-only gate scripts/hil_gate.py is unchanged; "
               "see docs/hil-extended.md for the risk model.")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help="device IP/hostname (no scheme)")
    parser.add_argument("--port", type=int, default=80,
                        help="device HTTP port")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-HTTP-request timeout in seconds")
    parser.add_argument("--json", action="store_true",
                        help="print the JSON report on stdout")
    parser.add_argument("--report", metavar="PATH", default=None,
                        help="also write the JSON report to PATH")
    parser.add_argument("--verbose", action="store_true",
                        help="print HTTP/subprocess diagnostics to stderr")
    parser.add_argument("--dry-run", action="store_true",
                        help="offline: no network, fake device in-process "
                             "(default when no mode is selected)")
    parser.add_argument("--allow-dap-session", action="store_true",
                        help="allow an active USBIP/WS-DAP session instead of "
                             "failing the preflight")
    parser.add_argument("--allow-any-target", action="store_true",
                        help="allow non-STM32H7 targets for write modes "
                             "(default: H7 only)")

    soak = parser.add_argument_group("soak (opt-in, read-only sampling)")
    soak.add_argument("--soak", action="store_true",
                      help="run repeated curve start-bin/stop cycles")
    soak.add_argument("--duration", type=float, default=None,
                      help="soak length in seconds (implies --soak; default "
                           "%g when --soak is given)" % DEFAULT_DURATION)
    soak.add_argument("--soak-run", type=float, default=2.0,
                      help="seconds of sampling per soak cycle")
    soak.add_argument("--soak-series", type=int, default=1,
                      help="series count per soak cycle (1..4)")
    soak.add_argument("--interval-us", type=int, default=20000,
                      help="curve sampling interval in microseconds")
    soak.add_argument("--address", type=parse_u32,
                      default=parse_u32("0x20000000"),
                      help="fixed read-only target RAM address")
    soak.add_argument("--poll-interval", type=float, default=0.2,
                      help="status/frames polling interval inside a cycle")
    soak.add_argument("--max-invalid-ratio", type=float, default=0.01,
                      help="maximum invalid-frame ratio during soak")
    soak.add_argument("--heap-min-free", type=int, default=6144,
                      help="minimum heap_free in bytes")
    soak.add_argument("--heap-min-largest", type=int, default=4096,
                      help="minimum heap_largest in bytes")
    soak.add_argument("--heap-leak-tolerance", type=int, default=32768,
                      help="allowed heap_free drop across the soak (bytes)")
    soak.add_argument("--read-only", action="store_true",
                      help="device read-only preflight only (no soak/writes)")

    flash = parser.add_argument_group(
        "Web Flash smoke (opt-in WRITE; needs --yes-flash)")
    flash.add_argument("--flash-image", default=None,
                       help="target image for POST /api/target/flash")
    flash.add_argument("--yes-flash", action="store_true",
                       help="second safety switch: actually write target Flash")
    flash.add_argument("--flash-expect-sha256", default="",
                       help="expected sha256 of --flash-image")
    flash.add_argument("--flash-expect-uid", default="",
                       help="expected target UID (hex, spaces ignored)")
    flash.add_argument("--flash-expect-flash-kib", type=int, default=0,
                       help="expected target flash size in KiB (0=skip)")
    flash.add_argument("--flash-timeout", type=float, default=300.0,
                       help="HTTP timeout for the target Flash upload")

    ota = parser.add_argument_group(
        "OTA smoke (opt-in WRITE; needs --yes-ota)")
    ota.add_argument("--ota-image", default=None,
                     help="ESP32 app image for POST /api/ota/upload "
                          "(default example: %s)" % DEFAULT_OTA_IMAGE)
    ota.add_argument("--yes-ota", action="store_true",
                     help="second safety switch: actually OTA the device")
    ota.add_argument("--ota-timeout", type=float, default=180.0,
                     help="HTTP timeout for the OTA upload")
    ota.add_argument("--ota-reboot-timeout", type=float, default=90.0,
                     help="seconds to wait for the device to come back")
    ota.add_argument("--ota-settle", type=float, default=12.0,
                     help="seconds to wait after reboot before the rollback "
                          "stability check (0=skip; app confirms rollback in "
                          "~10 s)")

    usbip = parser.add_argument_group(
        "USBIP/OpenOCD identify smoke (optional; skips when unavailable)")
    usbip.add_argument("--usbip", action="store_true",
                       help="host usbip attach/detach + one OpenOCD "
                            "CMSIS-DAP identify session")
    usbip.add_argument("--usbip-strict", action="store_true",
                       help="treat the known CMD_INFO-after-HTTP-probe "
                            "limitation as a required failure (default: warning "
                            "so it does not block the main smoke)")
    usbip.add_argument("--root-helper", default="",
                       help="command prefix that runs as root (default: "
                            "$HIL_ROOT_HELPER, /tmp/hostroot.sh or sudo -n)")
    usbip.add_argument("--usbip-host", default="",
                       help="device IP for 'usbip attach' (default: --host)")
    usbip.add_argument("--usbip-busid", default="1-1",
                       help="usbip remote bus id")
    usbip.add_argument("--usbip-port-index", type=int, default=0,
                       help="local vhci port index for detach")
    usbip.add_argument("--openocd", dest="openocd_path", default=DEFAULT_OPENOCD,
                       help="OpenOCD binary")
    usbip.add_argument("--openocd-scripts", default=DEFAULT_OPENOCD_SCRIPTS,
                       help="OpenOCD scripts directory")
    usbip.add_argument("--openocd-timeout", type=float, default=120.0,
                       help="per-OpenOCD-session timeout in seconds")
    return parser


def validate_args(parser: argparse.ArgumentParser,
                  args: argparse.Namespace) -> None:
    if not args.host or "://" in args.host or "/" in args.host:
        parser.error("--host must be a bare IP/hostname: %r" % args.host)
    if not 1 <= args.port <= 65535:
        parser.error("--port out of range: %d" % args.port)
    if not 0.5 <= args.timeout <= 120:
        parser.error("--timeout out of range: %g" % args.timeout)
    if args.duration is not None:
        if not 1.0 <= args.duration <= 7200:
            parser.error("--duration out of range (1..7200 s): %g" % args.duration)
        args.soak = True
    if args.soak:
        if not 0.2 <= args.soak_run <= 120:
            parser.error("--soak-run out of range: %g" % args.soak_run)
        if not 1 <= args.soak_series <= 4:
            parser.error("--soak-series out of range (1..4): %d" % args.soak_series)
        effective = args.duration if args.duration is not None else DEFAULT_DURATION
        if effective < args.soak_run:
            parser.error("--duration (%g s) must be >= --soak-run (%g s)"
                         % (effective, args.soak_run))
        args.duration = effective
    if not 5000 <= args.interval_us <= 200000:
        parser.error("--interval-us out of range (contract 5000..200000): %d"
                     % args.interval_us)
    if not 0.05 <= args.poll_interval <= 5:
        parser.error("--poll-interval out of range: %g" % args.poll_interval)
    if not 0 <= args.max_invalid_ratio <= 1:
        parser.error("--max-invalid-ratio out of range: %g"
                     % args.max_invalid_ratio)
    for value, name in ((args.heap_min_free, "--heap-min-free"),
                        (args.heap_min_largest, "--heap-min-largest"),
                        (args.heap_leak_tolerance, "--heap-leak-tolerance")):
        if value < 0:
            parser.error("%s must be >= 0" % name)
    if args.yes_flash and not args.flash_image:
        parser.error("--yes-flash requires --flash-image")
    if args.flash_image:
        if not os.path.isfile(os.path.expanduser(args.flash_image)):
            parser.error("--flash-image not found: %s" % args.flash_image)
        if args.flash_expect_sha256 and not is_sha256_hex(args.flash_expect_sha256):
            parser.error("--flash-expect-sha256 must be 64 hex chars")
        if args.flash_expect_uid:
            expected_uid = normalize_uid(args.flash_expect_uid)
            if not expected_uid:
                parser.error("--flash-expect-uid must contain hex digits")
        if not 0 <= args.flash_expect_flash_kib <= 65536:
            parser.error("--flash-expect-flash-kib out of range")
        if not 10 <= args.flash_timeout <= 600:
            parser.error("--flash-timeout out of range (10..600 s)")
    if args.yes_ota and not args.ota_image:
        parser.error("--yes-ota requires --ota-image")
    if args.ota_image:
        if not os.path.isfile(os.path.expanduser(args.ota_image)):
            parser.error("--ota-image not found: %s" % args.ota_image)
        if os.path.getsize(os.path.expanduser(args.ota_image)) <= 0:
            parser.error("--ota-image is empty: %s" % args.ota_image)
        if not 10 <= args.ota_timeout <= 600:
            parser.error("--ota-timeout out of range (10..600 s)")
        if not 10 <= args.ota_reboot_timeout <= 300:
            parser.error("--ota-reboot-timeout out of range (10..300 s)")
        if not 0 <= args.ota_settle <= 120:
            parser.error("--ota-settle out of range (0..120 s)")
    if args.usbip:
        if not 0 <= args.usbip_port_index <= 31:
            parser.error("--usbip-port-index out of range (0..31)")
        if not 10 <= args.openocd_timeout <= 600:
            parser.error("--openocd-timeout out of range (10..600 s)")
        if args.usbip_host and ("://" in args.usbip_host
                                or "/" in args.usbip_host):
            parser.error("--usbip-host must be a bare IP/hostname: %r"
                         % args.usbip_host)
        if not os.path.exists(os.path.expanduser(args.openocd_path)) \
                and not args.dry_run:
            print("[hil_extended] warning: OpenOCD not found at %s; the USBIP "
                  "mode will be SKIPPED" % args.openocd_path, file=sys.stderr)


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)
    # Write mode warnings are printed before the preflight so an operator sees
    # them even if the run is later refused by a safety check.
    if args.flash_image and not args.yes_flash:
        print("[hil_extended] flash: %s is a DRY-RUN (no --yes-flash): target "
              "Flash will not be written" % args.flash_image, file=sys.stderr)
    if args.ota_image and not args.yes_ota:
        print("[hil_extended] ota: %s is a DRY-RUN (no --yes-ota): the device "
              "will not be OTA-updated" % args.ota_image, file=sys.stderr)
    modes = _device_modes_requested(args)
    if args.dry_run and not modes:
        return run_selftest(args)
    if not modes and not args.dry_run:
        print("[hil_extended] no device mode selected (--read-only / --soak / "
              "--flash-image / --ota-image / --usbip); running the offline "
              "dry-run self-test (default safe behavior)", file=sys.stderr)
        return run_selftest(args)
    if args.dry_run:
        # Dry-run with explicit modes: exercise the real pipeline against the
        # in-process fake device (validates flags/headers/report offline).
        harness: Any = FakeHarness()
        report = run_pipeline(args, harness)
    else:
        harness = LiveHarness(args)
        report = run_pipeline(args, harness)
    if args.report:
        _write_report(report, args.report)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        _human_report(report)
    return int(report["exit_code"])


if __name__ == "__main__":
    sys.exit(main())
