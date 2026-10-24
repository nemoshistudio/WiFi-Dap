#!/usr/bin/env python3
"""scripts/hil_gate.py -- WiFi Dap HIL regression / performance gate.

Read-only hardware-in-the-loop (HIL) gate for a running WiFi Dap
device.  It exercises exactly the same HTTP surface the Web UI uses and prints a
machine-readable verdict (JSON with ``--json``) plus a CI exit code:

    target/info (detect) -> target/status (diagnostics) ->
    curve/start 1 series -> status/frames (short window) -> curve/stop ->
    curve/start 5 series -> status/frames 10 s (performance gate) -> curve/stop ->
    target/status (heap/no-leak) -> target/info (target alive) ->
    page smoke (index HTML + optional Playwright SPA routes)

Safety: by default this tool NEVER writes the target Flash, never OTA-updates
the ESP32 and never resets the target.  The only active operations are
``POST /api/curve/start`` (device-local SWD sampling: halt -> read RAM ->
resume, no writes) and ``POST /api/curve/stop``; sampling reads a fixed RAM
address, not Flash.  A ``curve/stop`` is attempted from a ``finally`` block even
when a check fails.

Extended (opt-in, OFF by default) modes under ``--usbip-smoke``,
``--reboot-recovery``, ``--flash-image ... --flash-allow``,
``--ota-image ... --ota-allow`` / ``--ota-interrupt`` and
``--soak-seconds N`` add host-side USBIP/OpenOCD sessions, a Web Flash write of
an explicitly named test image, OTA upload/interrupt tests and a stability soak.
Those modes are documented in docs/hil-gate.md ("HIL 扩展模式"); every write
mode needs a second safety switch plus a concrete image file, and environment
limitations (no root helper, no OpenOCD build, no image) are reported as
SKIPPED checks, never as PASS.

Checks and threshold rationale are documented in docs/hil-gate.md; the defaults
are based on the measured H7 baseline (50 Hz nominal, ~47.6 fps delivered over
Wi-Fi for 5 series at 20 ms).

Exit codes:
    0  PASS   all required checks passed (warnings allowed)
    1  FAIL   a required check failed
    2  usage / argument error
    3  device unreachable (network / HTTP)

Usage:
    python3 scripts/hil_gate.py --host <device-ip>
    python3 scripts/hil_gate.py --host <device-ip> --json --report /tmp/hil.json
    python3 scripts/hil_gate.py --dry-run              # offline self-test
    python3 scripts/hil_gate.py --help

Requires: Python >= 3.9, stdlib only (Playwright is optional, for page smoke).
"""

from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Endpoints / constants (docs/curve-local-sampler.md, uri_target.c)
# ---------------------------------------------------------------------------

TARGET_INFO_PATH = "/api/target/info"
TARGET_STATUS_PATH = "/api/target/status"
CURVE_START_PATH = "/api/curve/start"
CURVE_START_BIN_PATH = "/api/curve/start-bin"
CURVE_STOP_PATH = "/api/curve/stop"
CURVE_STATUS_PATH = "/api/curve/status"
CURVE_FRAMES_PATH = "/api/curve/frames"

#: Endpoints that would mutate device/target state.  The gate must never call
#: them; FakeDevice asserts on them in --dry-run and docs/hil-gate.md states the
#: read-only boundary.  Kept explicit so a future edit is a visible diff.
FORBIDDEN_PATHS = (
    "/api/target/flash",
    "/api/target/reset",
    "/api/ota",
    "/api/dap/kick",
)

#: Flash engine states exposed by GET /api/target/status (uri_target.c).
TARGET_STATES = ("IDLE", "ERASING", "PROGRAMMING", "VERIFYING", "RESETTING",
                 "DONE", "ERROR")

#: CURVE frame page header is 16 bytes for both CVF1 and CVF2.
CURVE_FRAME_HEADER_BYTES = 16
CVF1_MAGIC = b"CVF1"
CVF2_MAGIC = b"CVF2"
CVF1_VERSION = 1
CVF2_VERSION = 2
CVF_FLAG_GAP = 0x01
CVF_FLAG_PC = 0x02
CVF_FLAG_TRIGGER = 0x04
CVF_FLAG_CONDITION = 0x08
CVF2_FRAME_HEADER_BYTES = 15
CVF1_FRAME_HEADER_BYTES = 12
CVF2_MAX_FRAMES_PER_REQUEST = 64

#: Fake single-file index page for --dry-run (padded so the "body too small"
#: regression check behaves like the real ~1.6 MB inlined bundle).
DRY_RUN_HTML = (
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\">"
    "<title>Dry-run | WiFi Dap</title></head><body>"
    "<div id=\"app\">dry-run</div><script src=\"inline\"></script>"
    + "<!-- dry-run filler -->" * 80
    + "</body></html>"
)

EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_UNREACHABLE = 3

TOOL_NAME = "birdiebot-wifi-dap-hil-gate"
TOOL_VERSION = 1


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------

class DeviceUnreachable(Exception):
    """TCP/HTTP transport could not reach the device."""


class FrameDecodeError(ValueError):
    """Malformed CVF1/CVF2 frame page."""


class GateAbort(Exception):
    """A phase cannot continue (structural contract violation)."""

    def __init__(self, check_id: str, detail: str) -> None:
        super().__init__("%s: %s" % (check_id, detail))
        self.check_id = check_id
        self.detail = detail


class ProtocolError(ValueError):
    """Unexpected JSON payload / HTTP envelope."""


# ---------------------------------------------------------------------------
# CVF1 / CVF2 frame decoding (mirrors web/src/api/curveLocal.ts)
# ---------------------------------------------------------------------------

@dataclass
class CurveFrame:
    seq: int
    timestamp_us: int
    group: int
    payload_len: int
    payload: bytes = b""
    pc: Optional[int] = None


@dataclass
class CurveFramesPage:
    version: int = 0
    flags: int = 0
    count: int = 0
    first_seq: int = 0
    frame_bytes: int = 0
    frames: List[CurveFrame] = field(default_factory=list)

    @property
    def gap(self) -> bool:
        return bool(self.flags & CVF_FLAG_GAP)

    @property
    def pc_present(self) -> bool:
        return bool(self.flags & CVF_FLAG_PC)

    @property
    def triggered(self) -> bool:
        return bool(self.flags & CVF_FLAG_TRIGGER)

    @property
    def condition_triggered(self) -> bool:
        return bool(self.flags & CVF_FLAG_CONDITION)


def decode_curve_frames(payload: bytes) -> CurveFramesPage:
    """Decode a ``GET /api/curve/frames`` body (CVF1 or CVF2, little-endian).

    Empty body (HTTP 204 / 0 bytes) decodes to an empty page.  Raises
    FrameDecodeError for a bad magic/version, a truncated buffer or an invalid
    CVF2 frame payload length.  Trailing bytes are ignored for forward
    compatibility.
    """
    if payload is None:
        payload = b""
    if len(payload) == 0:
        return CurveFramesPage()
    if len(payload) < CURVE_FRAME_HEADER_BYTES:
        raise FrameDecodeError(
            "CVF header too short (%d < %d bytes)"
            % (len(payload), CURVE_FRAME_HEADER_BYTES))

    magic = payload[0:4]
    version, flags, count, first_seq, frame_bytes, _reserved = struct.unpack_from(
        "<BBHIHH", payload, 4)

    if magic == CVF1_MAGIC:
        if version != CVF1_VERSION:
            raise FrameDecodeError("CVF1 unsupported version %d" % version)
        stride = CVF1_FRAME_HEADER_BYTES + frame_bytes
        need = CURVE_FRAME_HEADER_BYTES + count * stride
        if len(payload) < need:
            raise FrameDecodeError(
                "CVF1 truncated: need %d bytes, got %d" % (need, len(payload)))
        frames: List[CurveFrame] = []
        offset = CURVE_FRAME_HEADER_BYTES
        for _index in range(count):
            seq, timestamp_us = struct.unpack_from("<IQ", payload, offset)
            start = offset + CVF1_FRAME_HEADER_BYTES
            frames.append(CurveFrame(
                seq=seq, timestamp_us=timestamp_us, group=0,
                payload_len=frame_bytes,
                payload=payload[start:start + frame_bytes]))
            offset += stride
        return CurveFramesPage(version=version, flags=flags, count=count,
                               first_seq=first_seq, frame_bytes=frame_bytes,
                               frames=frames)

    if magic == CVF2_MAGIC:
        if version != CVF2_VERSION:
            raise FrameDecodeError("CVF2 unsupported version %d" % version)
        pc_present = bool(flags & CVF_FLAG_PC)
        frame_header = CVF2_FRAME_HEADER_BYTES + (4 if pc_present else 0)
        frames = []
        offset = CURVE_FRAME_HEADER_BYTES
        for index in range(count):
            if offset + frame_header > len(payload):
                raise FrameDecodeError(
                    "CVF2 truncated frame header at index %d" % index)
            seq, timestamp_us = struct.unpack_from("<IQ", payload, offset)
            group = payload[offset + 12]
            payload_len = struct.unpack_from("<H", payload, offset + 13)[0]
            if payload_len > frame_bytes:
                raise FrameDecodeError(
                    "CVF2 frame[%d] payload_len %d > frame_bytes %d"
                    % (index, payload_len, frame_bytes))
            pc = None
            if pc_present:
                pc = struct.unpack_from("<I", payload, offset + 15)[0]
            start = offset + frame_header
            if start + payload_len > len(payload):
                raise FrameDecodeError(
                    "CVF2 truncated payload at index %d" % index)
            frames.append(CurveFrame(
                seq=seq, timestamp_us=timestamp_us, group=group,
                payload_len=payload_len,
                payload=payload[start:start + payload_len], pc=pc))
            offset = start + payload_len
        return CurveFramesPage(version=version, flags=flags, count=count,
                               first_seq=first_seq, frame_bytes=frame_bytes,
                               frames=frames)

    raise FrameDecodeError("CVF magic mismatch: %r (want CVF1/CVF2)" % magic)


def compute_curve_metrics(payload: bytes, window_s: float) -> Dict[str, Any]:
    """Decode one frame page and compute rate/gap metrics for a sampling window.

    ``device_fps`` is the effective sampling rate measured from the device
    ``esp_timer`` timestamps (independent of HTTP polling jitter);
    ``delivered_fps`` includes Wi-Fi/poll scheduling and is reported for
    diagnostics.  ``missing_seqs`` counts holes in the seq stream.
    """
    page = decode_curve_frames(payload)
    frames = page.frames
    metrics: Dict[str, Any] = {
        "version": page.version,
        "flags": page.flags,
        "frames": len(frames),
        "first_seq": frames[0].seq if frames else 0,
        "last_seq": frames[-1].seq if frames else 0,
        "frame_bytes": page.frame_bytes,
        "gap": page.gap,
        "pc_present": page.pc_present,
        "window_s": round(window_s, 3),
        "missing_seqs": 0,
        "device_duration_s": 0.0,
        "device_fps": 0.0,
        "delivered_fps": 0.0,
    }
    if not frames:
        return metrics
    previous = None
    missing = 0
    for frame in frames:
        if previous is not None and frame.seq > previous + 1:
            missing += frame.seq - previous - 1
        previous = frame.seq
    metrics["missing_seqs"] = missing
    span_s = (frames[-1].timestamp_us - frames[0].timestamp_us) / 1e6
    metrics["device_duration_s"] = round(span_s, 3)
    if span_s > 0 and len(frames) > 1:
        metrics["device_fps"] = round((len(frames) - 1) / span_s, 2)
    if window_s > 0:
        metrics["delivered_fps"] = round(len(frames) / window_s, 2)
    return metrics


# ---------------------------------------------------------------------------
# Gate bookkeeping
# ---------------------------------------------------------------------------

class Gate:
    """Collects checks/metrics and computes the PASS/FAIL verdict."""

    def __init__(self, legacy_firmware: bool = False,
                 allow_dap_session: bool = False) -> None:
        self.checks: List[Dict[str, Any]] = []
        self.metrics: Dict[str, Any] = {}
        self.warnings: List[str] = []
        self.legacy_firmware = legacy_firmware
        self.allow_dap_session = allow_dap_session
        self.failed = False

    # -- checks ------------------------------------------------------------
    def check(self, check_id: str, ok: bool, detail: str,
              severity: str = "required",
              values: Optional[Dict[str, Any]] = None,
              ok_detail: Optional[str] = None) -> bool:
        entry: Dict[str, Any] = {
            "id": check_id,
            "severity": severity,
            "ok": bool(ok),
            "detail": ok_detail if (ok and ok_detail) else detail,
        }
        if values:
            entry["values"] = values
        self.checks.append(entry)
        if not ok:
            if severity == "required":
                self.failed = True
            elif severity == "warn":
                self.warnings.append("%s: %s" % (check_id, detail))
        return bool(ok)

    def require(self, check_id: str, ok: bool, detail: str,
                values: Optional[Dict[str, Any]] = None,
                ok_detail: Optional[str] = None) -> None:
        """Record a required check and abort the current phase when it fails."""
        if not self.check(check_id, ok, detail, values=values,
                          ok_detail=ok_detail):
            raise GateAbort(check_id, detail)

    def skip(self, check_id: str, detail: str) -> None:
        self.checks.append({
            "id": check_id,
            "severity": "info",
            "ok": True,
            "skipped": True,
            "detail": detail,
        })

    def info(self, check_id: str, detail: str,
             values: Optional[Dict[str, Any]] = None) -> None:
        self.check(check_id, True, detail, severity="info", values=values)

    # -- metrics -----------------------------------------------------------
    def metric(self, key: str, value: Any) -> None:
        self.metrics[key] = value

    def metrics_update(self, values: Dict[str, Any]) -> None:
        self.metrics.update(values)


# ---------------------------------------------------------------------------
# HTTP transport
# ---------------------------------------------------------------------------

@dataclass
class Response:
    status: int
    headers: Dict[str, str]
    body: bytes
    elapsed_ms: float = 0.0

    def json(self) -> Dict[str, Any]:
        try:
            parsed = json.loads(self.body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ProtocolError("invalid JSON body: %s (first bytes: %r)"
                                % (exc, self.body[:80]))
        if not isinstance(parsed, dict):
            raise ProtocolError("JSON body is not an object: %r" % (parsed,))
        return parsed


class HttpTransport:
    """Minimal keep-alive-less HTTP client for the gate (stdlib only)."""

    def __init__(self, host: str, port: int = 80, timeout: float = 10.0,
                 verbose: bool = False) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.verbose = verbose

    def _one_request(self, method: str, path: str, data: Optional[bytes],
                     timeout: float) -> Response:
        connection = http.client.HTTPConnection(self.host, self.port,
                                                timeout=timeout)
        headers = {"Accept": "*/*", "Connection": "close",
                   "User-Agent": TOOL_NAME}
        if data is not None:
            headers["Content-Type"] = "application/json"
        started = time.monotonic()
        try:
            connection.request(method, path, body=data, headers=headers)
            raw = connection.getresponse()
            body = raw.read()
            response_headers = {}
            for name, value in raw.getheaders():
                response_headers[name.lower()] = value
            elapsed_ms = (time.monotonic() - started) * 1000.0
        finally:
            connection.close()
        if response_headers.get("content-encoding", "").lower() == "gzip":
            try:
                body = gzip.decompress(body)
            except OSError as exc:
                raise ProtocolError("gzip decode failed: %s" % exc)
        if self.verbose:
            print("[hil_gate] HTTP %s %s -> %d (%d B, %.1f ms)"
                  % (method, path, raw.status, len(body), elapsed_ms),
                  file=sys.stderr)
        return Response(status=raw.status, headers=response_headers,
                        body=body, elapsed_ms=elapsed_ms)

    def _one_request_bytes(self, method: str, path: str, data: bytes,
                           headers: Dict[str, str],
                           timeout: float,
                           close_after: bool) -> Response:
        connection = http.client.HTTPConnection(self.host, self.port,
                                                timeout=timeout)
        started = time.monotonic()
        try:
            send_headers = {"Accept": "*/*", "Connection": "close",
                            "User-Agent": TOOL_NAME}
            send_headers.update(headers)
            if "Content-Length" not in send_headers:
                send_headers["Content-Length"] = str(len(data))
            connection.putrequest(method, path, skip_accept_encoding=True)
            for name, value in send_headers.items():
                connection.putheader(name, value)
            connection.endheaders()
            if data:
                connection.send(data)
            if close_after:
                # Interrupted-upload test: drop the TCP connection after a
                # partial body; the device must abort the OTA session and keep
                # running the current slot.
                try:
                    connection.sock.close()  # type: ignore[union-attr]
                except OSError:
                    pass
                return Response(status=0, headers={}, body=b"",
                                elapsed_ms=(time.monotonic() - started) * 1000.0)
            raw = connection.getresponse()
            body = raw.read()
            response_headers = {}
            for name, value in raw.getheaders():
                response_headers[name.lower()] = value
            return Response(status=raw.status, headers=response_headers,
                            body=body,
                            elapsed_ms=(time.monotonic() - started) * 1000.0)
        finally:
            connection.close()

    def request_bytes(self, method: str, path: str, data: bytes = b"",
                      headers: Optional[Dict[str, str]] = None,
                      timeout: Optional[float] = None, retries: int = 1,
                      close_after: bool = False) -> Response:
        """Send a raw (non-JSON) body, e.g. /api/target/flash, /api/ota/upload."""
        request_headers = dict(headers or {})
        request_timeout = self.timeout if timeout is None else timeout
        last_error: Optional[BaseException] = None
        for attempt in range(max(0, retries) + 1):
            try:
                return self._one_request_bytes(method, path, data,
                                               request_headers,
                                               request_timeout, close_after)
            except (OSError, http.client.HTTPException, socket.timeout) as exc:
                last_error = exc
                if close_after:
                    break
                if attempt < retries:
                    time.sleep(0.3)
        raise DeviceUnreachable("%s %s: %s" % (method, path, last_error))

    def request(self, method: str, path: str,
                body: Optional[Dict[str, Any]] = None,
                timeout: Optional[float] = None,
                retries: int = 1) -> Response:
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
        request_timeout = self.timeout if timeout is None else timeout
        last_error: Optional[BaseException] = None
        for attempt in range(max(0, retries) + 1):
            try:
                return self._one_request(method, path, data, request_timeout)
            except (OSError, http.client.HTTPException, socket.timeout) as exc:
                last_error = exc
                if attempt < retries:
                    time.sleep(0.3)
        raise DeviceUnreachable("%s %s: %s" % (method, path, last_error))


# ---------------------------------------------------------------------------
# JSON helper
# ---------------------------------------------------------------------------

def response_json(gate: Gate, check_id: str, response: Response,
                  abort: bool = True) -> Dict[str, Any]:
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
        detail = "HTTP %d (expected 200)" % response.status
    if not ok:
        gate.check(check_id, False, detail)
        if abort:
            raise GateAbort(check_id, detail)
    return data


def number(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return float(value)


def integer(value: Any) -> Optional[int]:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

def phase_preflight(gate: Gate, http: HttpTransport, args: argparse.Namespace) -> None:
    """Ensure the device answers an HTTP request before doing anything else."""
    try:
        response = http.request("GET", TARGET_INFO_PATH, retries=1,
                                timeout=args.timeout)
    except DeviceUnreachable as exc:
        gate.check("device.reachable", False, str(exc))
        raise
    gate.require("device.reachable", response.status in (200, 403),
                 "HTTP %d from GET %s" % (response.status, TARGET_INFO_PATH),
                 values={"status": response.status})


def phase_target_info(gate: Gate, http: HttpTransport,
                      args: argparse.Namespace) -> Dict[str, Any]:
    """GET /api/target/info: the target must be detected and probed."""
    response = http.request("GET", TARGET_INFO_PATH, retries=2,
                            timeout=args.timeout)
    data = response_json(gate, "target.info.json", response)

    detected = data.get("detected") is True and data.get("ok") is True
    gate.require(
        "target.info.detected", detected,
        "detected=%r ok=%r err=%r (check SWD wiring / target power / "
        "BOOT/RST state; a DAP session may hold the bus)"
        % (data.get("detected"), data.get("ok"), data.get("err")),
        values={"detected": data.get("detected"), "ok": data.get("ok")},
        ok_detail="target detected on SWD")

    family = data.get("family")
    series = data.get("series")
    swd_clock_hz = number(data.get("swd_clock_hz"))
    flash_size = number(data.get("flash_size"))
    supported = data.get("supported")

    gate.require("target.info.identity",
                 isinstance(family, str) and bool(family)
                 and isinstance(series, str) and bool(series),
                 "family=%r series=%r" % (family, series))
    gate.require("target.info.swd_clock", swd_clock_hz is not None and swd_clock_hz > 0,
                 "swd_clock_hz=%r" % (data.get("swd_clock_hz"),))
    gate.require("target.info.flash_size", flash_size is not None and flash_size > 0,
                 "flash_size=%r" % (data.get("flash_size"),))
    if supported is not True:
        gate.check("target.info.supported", False,
                   "family=%r reported supported=%r (detection-only target: "
                   "flash gate thresholds may not apply)" % (family, supported),
                   severity="warn")

    target = {
        "detected": True,
        "family": family,
        "series": series,
        "device": data.get("device"),
        "core": data.get("core"),
        "supported": supported,
        "swd_clock_hz": int(swd_clock_hz),
        "flash_size": int(flash_size),
        "flash_kib": data.get("flash_kib"),
        "idcode": data.get("idcode"),
        "dpidr": data.get("dpidr"),
        "uid": data.get("uid"),
    }
    gate.metric("target", target)
    gate.info("target.info.summary",
              "%s %s @ %d kHz, flash=%d KiB, supported=%r"
              % (series, data.get("device"), int(swd_clock_hz) // 1000,
                 int(flash_size) // 1024, supported),
              values=target)
    return target


def _check_target_status_contract(gate: Gate, data: Dict[str, Any],
                                  tag: str) -> None:
    """Existing GET /api/target/status fields must keep their meaning."""
    errors: List[str] = []
    state = data.get("state")
    if state not in TARGET_STATES:
        errors.append("state=%r not in %s" % (state, list(TARGET_STATES)))
    for key in ("total", "written", "verified"):
        if integer(data.get(key)) is None or integer(data.get(key)) < 0:
            errors.append("%s=%r" % (key, data.get(key)))
    percent = integer(data.get("percent"))
    if percent is None or not 0 <= percent <= 100:
        errors.append("percent=%r" % (data.get("percent"),))
    if integer(data.get("err")) is None:
        errors.append("err=%r" % (data.get("err"),))
    if not isinstance(data.get("message"), str):
        errors.append("message=%r" % (data.get("message"),))
    if not isinstance(data.get("busy"), bool):
        errors.append("busy=%r" % (data.get("busy"),))
    gate.require("target.status.contract.%s" % tag, not errors,
                 "; ".join(errors) if errors else
                 "state/total/written/verified/percent/err/message/busy ok",
                 values={"state": state, "err": data.get("err"),
                         "busy": data.get("busy")})


def _diagnostics(gate: Gate, data: Dict[str, Any], tag: str,
                 pre: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Validate the additive diagnostics fields (heap / DAP session / clock).

    Missing fields fail required checks in strict mode; with
    ``--allow-legacy-firmware`` they are downgraded to warnings so the gate can
    still measure an older firmware image (pre-OTA) without pretending the
    diagnostics contract is covered.
    """
    strict_missing = not gate.legacy_firmware
    result: Dict[str, Any] = {}

    heap_free = number(data.get("heap_free"))
    heap_largest = number(data.get("heap_largest"))
    dap_active = data.get("dap_session_active")
    has_heap = heap_free is not None and heap_largest is not None
    has_dap = isinstance(dap_active, bool)
    missing = []
    if not has_heap:
        missing.append("heap_free/heap_largest")
    if not has_dap:
        missing.append("dap_session_active")
    if missing:
        detail = ("GET /api/target/status is missing diagnostics fields: %s "
                  "(firmware predates the HIL gate API extension; rebuild/OTA "
                  "the firmware or rerun with --allow-legacy-firmware)"
                  % ", ".join(missing))
        gate.check("target.status.diagnostics.%s" % tag, False, detail,
                   severity="required" if strict_missing else "warn")
    else:
        heap_ok = (heap_free >= 0 and heap_largest >= 0
                   and heap_largest <= heap_free)
        gate.require("target.status.heap.%s" % tag, heap_ok,
                     "heap_free=%d heap_largest=%d"
                     % (int(heap_free), int(heap_largest)),
                     values={"heap_free": int(heap_free),
                             "heap_largest": int(heap_largest)})
        gate.require(
            "target.status.heap_min.%s" % tag,
            heap_free >= gate_heap_min_free(gate) and
            heap_largest >= gate_heap_min_largest(gate),
            "heap_free=%d (min %d) heap_largest=%d (min %d)"
            % (int(heap_free), gate_heap_min_free(gate),
               int(heap_largest), gate_heap_min_largest(gate)))
        gate.info("target.status.heap.%s" % tag,
                  "heap_free=%d heap_largest=%d"
                  % (int(heap_free), int(heap_largest)))
        result["heap_free"] = int(heap_free)
        result["heap_largest"] = int(heap_largest)

        if dap_active:
            gate.check(
                "target.status.dap_session.%s" % tag, False,
                "dap_session_active=true (a USBIP/elaphureLink/WS-DAP session "
                "is connected; stop it for a reproducible gate)",
                severity="warn" if gate.allow_dap_session else "required")
            if not gate.allow_dap_session:
                raise GateAbort(
                    "target.status.dap_session.%s" % tag,
                    "active DAP proxy session (rerun on an idle device or pass "
                    "--allow-dap-session to let curve/start kick it)")
        else:
            gate.info("target.status.dap_session.%s" % tag,
                      "dap_session_active=false (no USBIP/WS-DAP client)")
        result["dap_session_active"] = dap_active

    # swd_clock_hz is served from the last successful /api/target/info probe.
    clock = number(data.get("swd_clock_hz"))
    if "swd_clock_hz" in data:
        clock_ok = clock is not None and clock > 0
        if pre is not None and pre.get("swd_clock_hz"):
            clock_ok = clock_ok and int(clock) == int(pre["swd_clock_hz"])
        gate.require("target.status.swd_clock.%s" % tag, clock_ok,
                     "swd_clock_hz=%r (expected %r)"
                     % (data.get("swd_clock_hz"),
                        pre.get("swd_clock_hz") if pre else None))
        result["swd_clock_hz"] = int(clock) if clock is not None else None
    else:
        detail = ("swd_clock_hz omitted from /api/target/status (contract: "
                  "present after at least one successful target/info probe)")
        if gate.legacy_firmware:
            gate.check("target.status.swd_clock.%s" % tag, False, detail,
                       severity="warn")
        else:
            gate.check("target.status.swd_clock.%s" % tag, False, detail)

    # last_error must only appear together with a non-zero err.
    if "last_error" in data:
        last_error = data.get("last_error")
        err = integer(data.get("err")) or 0
        gate.require("target.status.last_error.%s" % tag,
                     isinstance(last_error, str) and bool(last_error)
                     and err != 0,
                     "last_error=%r with err=%r (contract: present only when "
                     "err != 0)" % (last_error, data.get("err")))
        if isinstance(last_error, str) and last_error:
            result["last_error"] = last_error[:160]
    elif (integer(data.get("err")) or 0) != 0:
        gate.check("target.status.last_error.%s" % tag, False,
                   "err=%r but no last_error text" % (data.get("err"),))
    return result


# Thresholds are attached to the Gate instance from run_gate() so the phase
# helpers stay functions (keeps the script easy to host-test).
def gate_heap_min_free(gate: Gate) -> int:
    return int(getattr(gate, "_heap_min_free", 32768))


def gate_heap_min_largest(gate: Gate) -> int:
    return int(getattr(gate, "_heap_min_largest", 8192))


def phase_target_status_pre(gate: Gate, http: HttpTransport,
                            args: argparse.Namespace) -> Dict[str, Any]:
    response = http.request("GET", TARGET_STATUS_PATH, retries=2,
                            timeout=args.timeout)
    data = response_json(gate, "target.status.json.pre", response)
    gate.require("target.status.ok.pre", data.get("ok") is True,
                 "ok=%r err=%r" % (data.get("ok"), data.get("err")))
    _check_target_status_contract(gate, data, "pre")
    gate.require("target.status.idle.pre",
                 data.get("state") in ("IDLE", "DONE") and data.get("busy") is False,
                 "flash engine state=%r busy=%r (a flash/OTA operation is "
                 "running; retry when idle)"
                 % (data.get("state"), data.get("busy")))
    diagnostics = _diagnostics(gate, data, "pre")
    gate.metric("status_pre", diagnostics)
    return data


def phase_target_status_post(gate: Gate, http: HttpTransport,
                             args: argparse.Namespace) -> None:
    pre = gate.metrics.get("status_pre") or {}
    response = http.request("GET", TARGET_STATUS_PATH, retries=2,
                            timeout=args.timeout)
    data = response_json(gate, "target.status.json.post", response)
    gate.require("target.status.ok.post", data.get("ok") is True,
                 "ok=%r err=%r" % (data.get("ok"), data.get("err")))
    _check_target_status_contract(gate, data, "post")
    gate.require("target.status.no_error.post",
                 (integer(data.get("err")) or 0) == 0
                 and data.get("state") != "ERROR",
                 "after curve sessions: state=%r err=%r last_error=%r"
                 % (data.get("state"), data.get("err"),
                    data.get("last_error")))
    diagnostics = _diagnostics(gate, data, "post", pre=pre)
    gate.metric("status_post", diagnostics)

    # No-leak check: sampling allocations must be released by curve/stop.
    leak_tolerance = int(getattr(gate, "_heap_leak_tolerance", 16384))
    if "heap_free" in diagnostics and "heap_free" in pre:
        delta = int(diagnostics["heap_free"]) - int(pre["heap_free"])
        gate.require(
            "target.status.heap_no_leak",
            delta >= -leak_tolerance,
            "heap_free %d -> %d (%+d B, tolerance -%d B over %d curve "
            "sessions)" % (pre["heap_free"], diagnostics["heap_free"],
                           delta, leak_tolerance, 2))
        gate.metric("heap_delta_bytes", delta)


def _cbs1_body(args: argparse.Namespace, count: int) -> bytes:
    """Build a CBS1 streaming start body (no 16 KiB JSON body on unified builds)."""
    groups = []
    left = count
    while left > 0:
        take = min(64, left)
        groups.append(take)
        left -= take
    body = bytearray(b"CBS1")
    body += struct.pack("<BBBB", 1, 0x03, 0, 0)  # version, halt|want_pc, periodic, watch count
    body += struct.pack("<IIII", args.interval_us, 0, 0, 0)
    body += struct.pack("<BBBB", 0, 0, 0, 0)
    body += struct.pack("<BBH", 0, len(groups), 0)
    index = 0
    for group_count in groups:
        body += struct.pack("<BBH", 1, group_count, 0)
        for _ in range(group_count):
            expression = ("A%08X" % ((args.address + index * 4) & 0xFFFFFFFF)).encode("ascii")
            body += struct.pack("<BBBBIIH", 4, 0, 1, 0, 0, 0, len(expression))
            body += expression
            index += 1
    return bytes(body)


def _series_body(args: argparse.Namespace, count: int) -> Dict[str, Any]:
    series = []
    for index in range(count):
        series.append({
            "id": index,
            "size": 4,
            # Fixed read-only RAM address (STM32H7 DTCM by default); sampling
            # only reads memory, it never writes the target.
            "expr": {"kind": "addr",
                     "address": (args.address + index * 4) & 0xFFFFFFFF},
        })
    return {"interval_us": args.interval_us, "halt": True, "series": series}


def start_curve(gate: Gate, http: HttpTransport, args: argparse.Namespace,
                count: int, tag: str) -> Dict[str, Any]:
    binary = _cbs1_body(args, count)
    response = http.request_bytes(
        "POST", CURVE_START_BIN_PATH, data=binary,
        headers={"Content-Type": "application/octet-stream"},
        timeout=args.timeout, retries=0)
    if response.status in (404, 405, 501):
        legacy_body = _series_body(args, count)
        response = http.request("POST", CURVE_START_PATH, body=legacy_body,
                                timeout=args.timeout, retries=0)
    data = response_json(gate, "curve.%s.start.json" % tag, response)
    gate.require("curve.%s.start.ok" % tag, data.get("ok") is True,
                 "curve/start ok=%r err=%r (DAP busy or target not "
                 "responding)" % (data.get("ok"), data.get("err")))
    gate.require("curve.%s.start.series" % tag,
                 integer(data.get("series_count")) == count,
                 "series_count=%r expected %d" % (data.get("series_count"),
                                                  count))
    gate.require("curve.%s.start.bytes" % tag,
                 integer(data.get("sample_bytes")) == count * 4,
                 "sample_bytes=%r expected %d"
                 % (data.get("sample_bytes"), count * 4))
    gate.check("curve.%s.start.interval" % tag,
               integer(data.get("interval_us")) == args.interval_us,
               "interval_us=%r expected %d"
               % (data.get("interval_us"), args.interval_us),
               severity="required")
    return data


def stop_curve(gate: Gate, http: HttpTransport, args: argparse.Namespace,
               tag: str) -> None:
    try:
        response = http.request("POST", CURVE_STOP_PATH, body={},
                                timeout=args.timeout, retries=1)
    except DeviceUnreachable as exc:
        gate.check("curve.%s.stop" % tag, False, str(exc))
        return
    if response.status != 200:
        gate.check("curve.%s.stop" % tag, False, "HTTP %d" % response.status)
        return
    try:
        data = response.json()
    except ProtocolError:
        data = {}
    gate.require("curve.%s.stop" % tag, data.get("ok") is not False,
                 "curve/stop response=%r" % (data or response.body[:120]))
    # Verify the sampling task stopped and the target was resumed.
    status = http.request("GET", CURVE_STATUS_PATH, retries=1,
                          timeout=args.timeout)
    data = response_json(gate, "curve.%s.stop.status" % tag, status)
    gate.require("curve.%s.stop.running" % tag,
                 data.get("running") is False,
                 "running=%r after curve/stop" % (data.get("running"),))
    gate.require("curve.%s.stop.error" % tag,
                 (integer(data.get("error")) or 0) == 0,
                 "error=%r after curve/stop" % (data.get("error"),))


def sample_window(gate: Gate, http: HttpTransport, args: argparse.Namespace,
                  window_s: float, expected_frame_bytes: int,
                  tag: str) -> Dict[str, Any]:
    """Poll status/frames for ``window_s`` seconds and validate the stream."""
    frames: List[CurveFrame] = []
    last_seq = 0
    missing_seqs = 0
    gap_pages = 0
    protocol_issues: List[str] = []
    status_error_samples = 0
    status_invalid = 0
    status_overwritten = 0
    status_running_seen = False
    request_times: List[float] = []
    request_count = 0

    started = time.monotonic()
    rounds = 0
    while True:
        rounds += 1
        elapsed = time.monotonic() - started

        status_response = http.request("GET", CURVE_STATUS_PATH,
                                       timeout=args.timeout, retries=1)
        status_data = response_json(gate, "curve.%s.status.json" % tag,
                                    status_response)
        if (integer(status_data.get("error")) or 0) != 0:
            status_error_samples += 1
        status_invalid = max(status_invalid,
                             integer(status_data.get("invalid")) or 0)
        status_overwritten = max(status_overwritten,
                                 integer(status_data.get("overwritten")) or 0)
        if status_data.get("running") is True:
            status_running_seen = True

        frames_response = http.request(
            "GET",
            "%s?since=%d&max=%d"
            % (CURVE_FRAMES_PATH, last_seq, CVF2_MAX_FRAMES_PER_REQUEST),
            timeout=args.timeout, retries=1)
        request_count += 1
        request_times.append(frames_response.elapsed_ms)

        if frames_response.status == 200:
            try:
                page = decode_curve_frames(frames_response.body)
            except FrameDecodeError as exc:
                gate.check("curve.%s.frames.decode" % tag, False, str(exc))
                raise GateAbort("curve.%s.frames.decode" % tag, str(exc))
            if page.count > 0:
                if page.frame_bytes != expected_frame_bytes:
                    message = ("frame_bytes=%d expected %d"
                               % (page.frame_bytes, expected_frame_bytes))
                    gate.check("curve.%s.frames.bytes" % tag, False, message)
                    raise GateAbort("curve.%s.frames.bytes" % tag, message)
                if page.gap:
                    gap_pages += 1
                for frame in page.frames:
                    if last_seq and frame.seq > last_seq + 1:
                        missing_seqs += frame.seq - last_seq - 1
                    if last_seq and frame.seq <= last_seq:
                        protocol_issues.append(
                            "seq %d after %d (duplicate/out of order)"
                            % (frame.seq, last_seq))
                    last_seq = frame.seq
                    frames.append(frame)
                if page.frames[0].seq != page.first_seq:
                    protocol_issues.append(
                        "first frame seq %d != header first_seq %d"
                        % (page.frames[0].seq, page.first_seq))
        elif frames_response.status == 204:
            pass
        else:
            message = "HTTP %d from GET curve/frames" % frames_response.status
            gate.check("curve.%s.frames.http" % tag, False, message)
            raise GateAbort("curve.%s.frames.http" % tag, message)

        elapsed = time.monotonic() - started
        if elapsed >= window_s:
            break
        if rounds > 100000:  # defensive bound; never reached in practice
            break
        time.sleep(max(0.001, min(args.poll_interval, window_s - elapsed)))

    actual_window = time.monotonic() - started
    metrics: Dict[str, Any] = {
        "version": 0,
        "flags": 0,
        "device_duration_s": 0.0,
        "device_fps": 0.0,
        "delivered_fps": 0.0,
        "pc_present": False,
    }
    metrics.update({
        "frames": len(frames),
        "first_seq": frames[0].seq if frames else 0,
        "last_seq": frames[-1].seq if frames else 0,
        "frame_bytes": expected_frame_bytes,
        "missing_seqs": missing_seqs,
        "gap_pages": gap_pages,
        "window_s": round(actual_window, 3),
        "status_error_samples": status_error_samples,
        "status_invalid": status_invalid,
        "status_overwritten": status_overwritten,
        "frame_requests": request_count,
        "frame_request_avg_ms": round(sum(request_times) / max(1, len(request_times)), 1),
        "frame_request_max_ms": round(max(request_times or [0.0]), 1),
    })
    if frames:
        span_s = (frames[-1].timestamp_us - frames[0].timestamp_us) / 1e6
        metrics["device_duration_s"] = round(span_s, 3)
        metrics["pc_present"] = any(frame.pc is not None for frame in frames)
        if span_s > 0 and len(frames) > 1:
            metrics["device_fps"] = round((len(frames) - 1) / span_s, 2)
        if actual_window > 0:
            metrics["delivered_fps"] = round(len(frames) / actual_window, 2)

    # ---- checks over the collected window --------------------------------
    gate.require("curve.%s.frames.received" % tag, len(frames) > 0,
                 "no frames received in %.2f s (sampler/DAP failure?)"
                 % actual_window,
                 ok_detail="%d frames received in %.2f s"
                 % (len(frames), actual_window))
    gate.require("curve.%s.frames.protocol" % tag, not protocol_issues,
                 "; ".join(protocol_issues[:4]) or "CVF header/seq protocol ok",
                 ok_detail="CVF magic/version/seq protocol ok")
    gate.require(
        "curve.%s.frames.contiguous" % tag,
        missing_seqs == 0 and gap_pages == 0,
        "missing_seqs=%d gap_pages=%d (ring overrun or poll starvation; "
        "increase polling or reduce interval)" % (missing_seqs, gap_pages),
        ok_detail="no missing seq, no ring-overrun gap flags")
    gate.require(
        "curve.%s.status.running" % tag, status_running_seen,
        "status never reported running=true during the sampling window",
        ok_detail="sampler reported running=true")
    gate.require(
        "curve.%s.status.error" % tag, status_error_samples == 0,
        "sampler error!=0 observed in %d/%d status polls"
        % (status_error_samples, rounds),
        ok_detail="sampler error=0 in all %d status polls" % rounds)
    invalid_ratio = (status_invalid / float(len(frames))) if frames else 0.0
    gate.require(
        "curve.%s.status.invalid" % tag,
        invalid_ratio <= args.max_invalid_ratio,
        "invalid=%d/%d frames (ratio %.4f > %.4f): fixed RAM addresses failed "
        "to read; check target memory range/address"
        % (status_invalid, len(frames), invalid_ratio,
           args.max_invalid_ratio),
        ok_detail="invalid=%d/%d frames (ratio %.4f)"
        % (status_invalid, len(frames), invalid_ratio))
    gate.require(
        "curve.%s.fps" % tag,
        len(frames) >= 2 and metrics["device_fps"] >= args.fps_min,
        "effective device sampling rate %.2f fps < threshold %.2f fps "
        "(frames=%d; fewer than 2 frames cannot be measured -- sampler too "
        "slow / DAP stall)"
        % (metrics["device_fps"], args.fps_min, len(frames)),
        ok_detail="effective device sampling rate %.2f fps >= %.2f fps"
        % (metrics["device_fps"], args.fps_min))
    delivery_ok = metrics["delivered_fps"] >= args.fps_min * 0.8
    if delivery_ok:
        gate.check(
            "curve.%s.delivery" % tag, True,
            "delivered %.2f fps over %.2f s (device %.2f fps)"
            % (metrics["delivered_fps"], actual_window,
               metrics["device_fps"]),
            severity="info")
    else:
        gate.check(
            "curve.%s.delivery" % tag, False,
            "delivered %.2f fps < 80%% of threshold (HTTP/Wi-Fi poll "
            "starvation)" % metrics["delivered_fps"], severity="warn")
    gate.metric("curve_%s" % tag, metrics)
    gate.info("curve.%s.window" % tag,
              "%d frames, device %.2f fps, delivered %.2f fps, invalid=%d, "
              "overwritten=%d"
              % (len(frames), metrics["device_fps"],
                 metrics["delivered_fps"], status_invalid,
                 status_overwritten),
              values=metrics)
    return metrics


def phase_curve_single(gate: Gate, http: HttpTransport,
                       args: argparse.Namespace) -> None:
    start_curve(gate, http, args, 1, "single")
    try:
        sample_window(gate, http, args, args.one_series_duration, 4, "single")
    finally:
        stop_curve(gate, http, args, "single")


def phase_curve_perf(gate: Gate, http: HttpTransport,
                     args: argparse.Namespace) -> None:
    start_curve(gate, http, args, args.series, "perf")
    try:
        sample_window(gate, http, args, args.duration, args.series * 4, "perf")
    finally:
        stop_curve(gate, http, args, "perf")


def phase_target_info_after(gate: Gate, http: HttpTransport,
                            args: argparse.Namespace) -> None:
    """Probe again: proves curve/stop resumed the target and released the DAP."""
    before = gate.metrics.get("target") or {}
    response = http.request("GET", TARGET_INFO_PATH, retries=2,
                            timeout=args.timeout)
    data = response_json(gate, "target.info.after.json", response)
    gate.require("target.info.after.detected",
                 data.get("detected") is True and data.get("ok") is True,
                 "target not detected after sampling (target left halted or "
                 "DAP not released): err=%r" % (data.get("err"),),
                 ok_detail="target still detected after sampling (DAP "
                           "released, target resumed)")
    if before.get("series"):
        gate.check("target.info.after.identity",
                   data.get("series") == before.get("series")
                   and data.get("family") == before.get("family"),
                   "target changed after sampling: %r -> %r"
                   % ({"family": before.get("family"),
                       "series": before.get("series")},
                      {"family": data.get("family"),
                       "series": data.get("series")}),
                   severity="warn",
                   ok_detail="target identity unchanged after sampling")
    if before.get("swd_clock_hz"):
        after_clock = number(data.get("swd_clock_hz"))
        gate.check("target.info.after.swd_clock",
                   after_clock is not None
                   and after_clock == before.get("swd_clock_hz"),
                   "swd_clock_hz %r -> %r"
                   % (before.get("swd_clock_hz"), data.get("swd_clock_hz")),
                   severity="warn",
                   ok_detail="swd_clock_hz unchanged after sampling (%r)"
                   % (before.get("swd_clock_hz"),))


# ---------------------------------------------------------------------------
# Page smoke (HTTP + optional Playwright SPA routes)
# ---------------------------------------------------------------------------

PAGE_ROUTES = (
    ("/#/target", "target flash page (default route)"),
    ("/#/debug-curve", "variable curve page"),
    ("/#/update", "OTA update page"),
)


def _page_http_check(gate: Gate, http: HttpTransport,
                     args: argparse.Namespace) -> None:
    response = http.request("GET", "/", timeout=args.timeout, retries=1)
    gate.require("pages.index.http", response.status == 200,
                 "HTTP %d from GET /" % response.status)
    content_type = response.headers.get("content-type", "")
    gate.require("pages.index.content_type", "text/html" in content_type.lower(),
                 "content-type=%r" % content_type)
    text = response.body.decode("utf-8", "replace")
    lowered = text.lower()
    problems = []
    if "<!doctype html" not in lowered:
        problems.append("missing <!DOCTYPE html>")
    if 'id="app"' not in text:
        problems.append('missing app mount point id="app"')
    if len(response.body) < 1000:
        problems.append("body too small (%d B)" % len(response.body))
    gate.require("pages.index.content", not problems,
                 "; ".join(problems) if problems else
                 "single-file Web UI served (%d B)" % len(response.body))
    gate.metric("page_index_bytes", len(response.body))
    gate.info("pages.index.ok", "GET / -> 200 text/html, %d B (decoded)"
              % len(response.body))


def _page_playwright_check(gate: Gate, args: argparse.Namespace) -> None:
    mode = args.playwright
    if mode == "off":
        gate.skip("pages.playwright", "disabled by --playwright off")
        return
    try:
        from playwright.sync_api import sync_playwright  # type: ignore
    except Exception as exc:  # pragma: no cover - env dependent
        detail = "python playwright not importable: %s" % exc
        if mode == "on":
            gate.require("pages.playwright", False, detail)
        else:
            gate.check("pages.playwright", False, detail, severity="warn")
            gate.skip("pages.playwright.routes", "skipped (playwright missing)")
        return

    base = "http://%s:%d" % (args.host, args.port)
    failures: List[str] = []
    observations: List[Dict[str, Any]] = []
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            context = browser.new_context(viewport={"width": 1280, "height": 800})
            page = context.new_page()
            page_errors: List[str] = []
            page.on("pageerror", lambda error: page_errors.append(str(error)))
            for route, label in PAGE_ROUTES:
                page_errors.clear()
                page.goto(base + route, wait_until="domcontentloaded",
                          timeout=int(args.timeout * 1000))
                page.wait_for_timeout(1200)
                title = page.title()
                current_hash = page.evaluate("() => location.hash")
                app_text_len = page.evaluate(
                    "() => (document.querySelector('#app')?.innerText || '').length")
                title_parts = [part.strip() for part in title.split("|")]
                duplicate_title = (len(title_parts) >= 2
                                   and title_parts[0] == title_parts[-1])
                problem = None
                if page_errors:
                    problem = "uncaught JS error: %s" % page_errors[0][:160]
                elif current_hash != "#" + route.split("#", 1)[-1]:
                    problem = "hash=%r (expected %r)" % (
                        current_hash, "#" + route.split("#", 1)[-1])
                elif app_text_len < 250:
                    problem = ("app not rendered (innerText=%d chars, likely "
                               "404/blank route)" % app_text_len)
                elif duplicate_title:
                    problem = "unmatched route title=%r" % title
                observations.append({
                    "route": route,
                    "label": label,
                    "title": title,
                    "hash": current_hash,
                    "app_text_len": app_text_len,
                    "ok": problem is None,
                })
                if problem:
                    failures.append("%s: %s" % (route, problem))
            context.close()
            browser.close()
    except Exception as exc:  # pragma: no cover - env/hardware dependent
        detail = "playwright page smoke failed: %s" % exc
        if mode == "on":
            gate.require("pages.playwright", False, detail)
        else:
            gate.check("pages.playwright", False, detail, severity="warn")
            gate.skip("pages.playwright.routes", "skipped (%s)" % exc)
        return

    gate.metric("pages", observations)
    gate.require("pages.playwright.routes", not failures,
                 "; ".join(failures) if failures else
                 "3 SPA routes mounted (target/debug-curve/update)")
    gate.info("pages.playwright.ok",
              "; ".join("%s=%r" % (item["route"], item["title"])
                        for item in observations))


def phase_pages(gate: Gate, http: HttpTransport,
                args: argparse.Namespace) -> None:
    if args.skip_pages:
        gate.skip("pages", "skipped by --skip-pages")
        return
    _page_http_check(gate, http, args)
    _page_playwright_check(gate, args)


# ---------------------------------------------------------------------------
# Gate driver
# ---------------------------------------------------------------------------

def _report(gate: Gate, args: argparse.Namespace, started_at: str,
            duration_s: float, exit_code: int,
            error: Optional[str]) -> Dict[str, Any]:
    required = [check for check in gate.checks if check["severity"] == "required"]
    passed = [check for check in required if check["ok"]]
    return {
        "tool": TOOL_NAME,
        "version": TOOL_VERSION,
        "ok": exit_code == EXIT_PASS,
        "exit_code": exit_code,
        "host": args.host,
        "port": args.port,
        "started_at": started_at,
        "finished_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "duration_s": round(duration_s, 3),
        "thresholds": {
            "fps_min": args.fps_min,
            "duration_s": args.duration,
            "interval_us": args.interval_us,
            "series": args.series,
            "max_invalid_ratio": args.max_invalid_ratio,
            "heap_min_free": args.heap_min_free,
            "heap_min_largest": args.heap_min_largest,
            "heap_leak_tolerance": args.heap_leak_tolerance,
            "usbip_sessions": getattr(args, "usbip_sessions", 0),
            "openocd_timeout_s": getattr(args, "openocd_timeout", 0.0),
            "soak_seconds": getattr(args, "soak_seconds", 0),
        },
        "modes": {
            "extended": _extended_enabled(args),
            "usbip_smoke": bool(getattr(args, "usbip_smoke", False)),
            "reboot_recovery": bool(getattr(args, "reboot_recovery", False)),
            "target_flash_write": bool(getattr(args, "flash_image", None)),
            "ota": bool(getattr(args, "ota_image", None)
                        or getattr(args, "ota_interrupt", False)),
            "soak": bool(getattr(args, "soak_seconds", 0)),
        },
        "extended": {
            key: value for key, value in gate.metrics.items()
            if key.startswith("extended")
        } if _extended_enabled(args) else {"enabled": False},
        "summary": {
            "checks": len(gate.checks),
            "required": len(required),
            "required_passed": len(passed),
            "required_failed": len(required) - len(passed),
            "warnings": len(gate.warnings),
            "skipped": len([c for c in gate.checks if c.get("skipped")]),
        },
        "checks": gate.checks,
        "metrics": gate.metrics,
        "warnings": gate.warnings,
        "error": error,
    }


def _human_report(report: Dict[str, Any], stream: Any = sys.stdout) -> None:
    status = "PASS" if report["ok"] else "FAIL"
    print("=== WiFi Dap HIL gate ===", file=stream)
    print("host: %s:%d   thresholds: fps>=%.1f, %gs x %d series @ %dus"
          % (report["host"], report["port"],
             report["thresholds"]["fps_min"],
             report["thresholds"]["duration_s"],
             report["thresholds"]["series"],
             report["thresholds"]["interval_us"]), file=stream)
    print("", file=stream)
    for check in report["checks"]:
        if check.get("skipped"):
            tag = "SKIP"
        elif check["ok"]:
            tag = "PASS" if check["severity"] == "required" else "INFO"
        else:
            tag = "WARN" if check["severity"] == "warn" else "FAIL"
        print("[%s] %-42s %s" % (tag, check["id"], check["detail"]),
              file=stream)
    print("", file=stream)
    metrics = report["metrics"]
    curve_perf = metrics.get("curve_perf") or {}
    if curve_perf:
        print("performance: device_fps=%.2f delivered_fps=%.2f frames=%d "
              "missing_seqs=%d invalid=%d"
              % (curve_perf.get("device_fps", 0.0),
                 curve_perf.get("delivered_fps", 0.0),
                 curve_perf.get("frames", 0),
                 curve_perf.get("missing_seqs", 0),
                 curve_perf.get("status_invalid", 0)), file=stream)
    target = metrics.get("target") or {}
    if target:
        print("target: %s %s flash=%d KiB swd=%d kHz"
              % (target.get("series", "?"), target.get("device", "?"),
                 (target.get("flash_size") or 0) // 1024,
                 (target.get("swd_clock_hz") or 0) // 1000), file=stream)
    status_pre = metrics.get("status_pre") or {}
    status_post = metrics.get("status_post") or {}
    if status_pre or status_post:
        print("heap: pre=%r post=%r delta=%r"
              % (status_pre.get("heap_free"), status_post.get("heap_free"),
                 metrics.get("heap_delta_bytes")), file=stream)
    extended = report.get("extended") or {}
    if (report.get("modes") or {}).get("extended"):
        for key in ("extended_usbip", "extended_recovery", "extended_flash",
                    "extended_ota", "extended_soak"):
            section = extended.get(key)
            if not section:
                continue
            detail = "status=%s" % (section.get("status"),)
            if key == "extended_flash" and section.get("flash_seconds"):
                detail += " size=%r written=%r verified=%r %.2fs" % (
                    section.get("image_size"),
                    (section.get("response") or {}).get("written"),
                    (section.get("response") or {}).get("verified"),
                    section.get("flash_seconds"))
            if key == "extended_soak" and section.get("iterations"):
                detail += (" cycles=%d heap=%r..%r B failures=%r"
                           % (section.get("iterations"),
                              section.get("heap_free_min"),
                              section.get("heap_free_end"),
                              section.get("failures")))
            if key == "extended_usbip" and section.get("sessions"):
                detail += " sessions=%d" % len(section["sessions"])
            if key == "extended_ota" and section.get("upload"):
                detail += " upload_http=%r" % (
                    section["upload"].get("status"),)
            print("extended %-20s %s" % (key.replace("extended_", ""), detail),
                  file=stream)
    for warning in report["warnings"]:
        print("warning: %s" % warning, file=stream)
    summary = report["summary"]
    print("", file=stream)
    print("RESULT: %s  (required %d/%d passed, %d warning(s), exit %d)"
          % (status, summary["required_passed"], summary["required"],
             summary["warnings"], report["exit_code"]), file=stream)
    if report.get("error"):
        print("error: %s" % report["error"], file=stream)


def _write_report(report: Dict[str, Any], path: str) -> None:
    try:
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=False)
            handle.write("\n")
    except OSError as exc:
        print("[hil_gate] warning: cannot write report %s: %s" % (path, exc),
              file=sys.stderr)


def run_gate(args: argparse.Namespace,
             transport: Optional[Any] = None) -> Dict[str, Any]:
    """Run the complete gate; returns the JSON report (also used by --dry-run)."""
    gate = Gate(legacy_firmware=args.allow_legacy_firmware,
                allow_dap_session=args.allow_dap_session)
    # Attach numeric thresholds used by the diagnostics helper.
    gate._heap_min_free = args.heap_min_free           # type: ignore[attr-defined]
    gate._heap_min_largest = args.heap_min_largest     # type: ignore[attr-defined]
    gate._heap_leak_tolerance = args.heap_leak_tolerance  # type: ignore[attr-defined]

    http = transport if transport is not None else HttpTransport(
        args.host, args.port, timeout=args.timeout, verbose=args.verbose)
    #: Offline self-test marker (FakeDevice): extended host-side modes must not
    #: touch the real host (usbip/subprocess) during --dry-run scenarios.
    args._offline = transport is not None
    started_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    started = time.monotonic()
    error: Optional[str] = None
    exit_code = EXIT_PASS

    try:
        phase_preflight(gate, http, args)
        phase_target_info(gate, http, args)
        phase_target_status_pre(gate, http, args)
        phase_curve_single(gate, http, args)
        phase_curve_perf(gate, http, args)
    except GateAbort as exc:
        error = "aborted at %s: %s" % (exc.check_id, exc.detail)
    except DeviceUnreachable as exc:
        error = "device unreachable: %s" % exc
        # phase_preflight() already records device.reachable on the common
        # path; only add it here if the failure happened before that phase.
        if not any(check["id"] == "device.reachable"
                   for check in gate.checks):
            gate.check("device.reachable", False, str(exc))
        exit_code = EXIT_UNREACHABLE
    except ProtocolError as exc:
        error = "protocol error: %s" % exc
        gate.check("protocol", False, str(exc))
    except Exception as exc:  # defensive: a crash must still emit a report
        error = "unexpected error: %s: %s" % (type(exc).__name__, exc)
        gate.check("internal", False, error)
        if args.verbose:
            traceback.print_exc()

    # Safety net: even after an aborted phase the sampling task must be stopped
    # (curve/start kicks a DAP session; curve/stop resumes the target).
    if exit_code != EXIT_UNREACHABLE:
        try:
            status_response = http.request("GET", CURVE_STATUS_PATH,
                                           timeout=args.timeout, retries=0)
            status_data = status_response.json()
            if status_data.get("running") is True:
                gate.check("cleanup.curve_surface", False,
                           "curve session still running at gate end; forcing "
                           "curve/stop so the target is resumed")
                try:
                    http.request("POST", CURVE_STOP_PATH, body={},
                                 timeout=args.timeout, retries=1)
                except DeviceUnreachable:
                    pass
                status_data = http.request("GET", CURVE_STATUS_PATH,
                                           timeout=args.timeout).json()
            gate.check("cleanup.curve_stopped",
                       status_data.get("running") is False,
                       "curve session still running after cleanup (running=%r)"
                       % (status_data.get("running"),),
                       ok_detail="curve sampler stopped, target resumed "
                                 "(running=False)")
        except (DeviceUnreachable, ProtocolError) as exc:
            gate.check("cleanup.curve_stopped", False,
                       "cannot verify curve/stop cleanup: %s" % exc)

    if exit_code != EXIT_UNREACHABLE and not error:
        try:
            # Post-sampling diagnostics only make sense after the curve phases.
            phase_target_status_post(gate, http, args)
            phase_target_info_after(gate, http, args)
            phase_pages(gate, http, args)
        except GateAbort as exc:
            error = "aborted at %s: %s" % (exc.check_id, exc.detail)
        except DeviceUnreachable as exc:
            error = "device unreachable: %s" % exc
            exit_code = EXIT_UNREACHABLE
        except ProtocolError as exc:
            error = "protocol error: %s" % exc
        except Exception as exc:
            error = "unexpected error: %s: %s" % (type(exc).__name__, exc)
            gate.check("internal", False, error)
            if args.verbose:
                traceback.print_exc()

    # Opt-in extended HIL modes (USBIP/OpenOCD, Web Flash, OTA, soak).  They run
    # only after the read-only gate passed: a broken baseline must not be
    # followed by a target-Flash write.
    if _extended_enabled(args) and exit_code != EXIT_UNREACHABLE and not error:
        try:
            phase_extended(gate, args, http)
        except GateAbort as exc:
            error = "extended aborted at %s: %s" % (exc.check_id, exc.detail)
        except DeviceUnreachable as exc:
            error = "device unreachable during extended modes: %s" % exc
            exit_code = EXIT_UNREACHABLE
        except Exception as exc:  # noqa: BLE001 - report even on a crash
            error = "extended error: %s: %s" % (type(exc).__name__, exc)
            gate.check("extended.internal", False, error)
            if args.verbose:
                traceback.print_exc()
        finally:
            try:
                extended_cleanup(gate, args)
            except Exception:  # noqa: BLE001 - cleanup must never mask the report
                pass

    if exit_code == EXIT_PASS and (gate.failed or error):
        exit_code = EXIT_FAIL
    duration = time.monotonic() - started
    report = _report(gate, args, started_at, duration, exit_code, error)
    return report


# ---------------------------------------------------------------------------
# --dry-run: offline self-test with an in-process fake device
# ---------------------------------------------------------------------------

class FakeDevice:
    """In-process device double used by --dry-run (no sockets, no hardware).

    Simulates GET target/info, GET target/status (optionally with the new
    diagnostics fields), POST curve/start|stop, GET curve/status|frames and the
    single-file index page.  It refuses every mutating endpoint so a regression
    that adds a Flash/OTA/reset call fails the self-test loudly.
    """

    def __init__(self, detected: bool = True, diagnostics: bool = True,
                 frame_format: str = "cvf2-pc", effective_fps: float = 50.0,
                 port: int = 0) -> None:
        self.detected = detected
        self.diagnostics = diagnostics
        self.frame_format = frame_format
        self.effective_interval_us = 1e6 / effective_fps if effective_fps > 0 else 1e6
        self.running = False
        self.started_at = 0.0
        self.interval_us = 20000
        self.series_count = 0
        self.heap_free = 120000
        self.heap_largest = 40000
        self.swd_clock_hz = 1000000

    # -- helpers -----------------------------------------------------------
    def _json(self, data: Dict[str, Any], status: int = 200) -> Response:
        return Response(status=status, headers={"content-type": "application/json"},
                        body=json.dumps(data).encode("utf-8"))

    def _curve_frames(self) -> List[CurveFrame]:
        """Deterministic frames for the elapsed sampling time (no side effects)."""
        if not self.running:
            return []
        elapsed = max(0.0, time.monotonic() - self.started_at)
        count = int(elapsed * 1e6 / self.effective_interval_us)
        frames = []
        base_ts = int(self.started_at * 1e6)
        for index in range(1, count + 1):
            frames.append(CurveFrame(
                seq=index,
                timestamp_us=base_ts + int(index * self.effective_interval_us),
                group=0, payload_len=self.series_count * 4,
                payload=b"\x01\x02\x03\x04" * self.series_count))
        return frames

    def _encode_page(self, frames: List[CurveFrame]) -> bytes:
        if not frames:
            return b""
        frame_bytes = self.series_count * 4
        if self.frame_format == "cvf2-pc":
            flags = CVF_FLAG_PC
            header = struct.pack("<4sBBHIHH", CVF2_MAGIC, CVF2_VERSION, flags,
                                 len(frames), frames[0].seq, frame_bytes, 0)
            body = b""
            for frame in frames:
                # CVF2 frame header: seq u32 + ts u64 + group u8 +
                # payload_len u16 (+ pc u32 when flags bit1 is set).
                body += struct.pack("<IQ", frame.seq, frame.timestamp_us)
                body += struct.pack("<BHI", 0, frame.payload_len, 0x08000123)
                body += frame.payload
            return header + body
        # CVF1 (single group, no PC) -- exercises the backwards-compatible path.
        flags = 0
        header = struct.pack("<4sBBHIHH", CVF1_MAGIC, CVF1_VERSION, flags,
                             len(frames), frames[0].seq, frame_bytes, 0)
        body = b""
        for frame in frames:
            body += struct.pack("<IQ", frame.seq, frame.timestamp_us)
            body += frame.payload
        return header + body

    # -- transport interface ----------------------------------------------
    def request_bytes(self, method: str, path: str, data: bytes = b"",
                      headers: Optional[Dict[str, str]] = None,
                      timeout: Optional[float] = None, retries: int = 1,
                      close_after: bool = False) -> Response:
        del headers, timeout, retries, close_after
        for forbidden in FORBIDDEN_PATHS:
            if path == forbidden or path.startswith(forbidden + "?") \
                    or path.startswith(forbidden + "/"):
                raise AssertionError("gate attempted forbidden write: %s %s"
                                     % (method, path))
        if path == CURVE_START_BIN_PATH and method == "POST":
            if not data.startswith(b"CBS1"):
                return self._json({"ok": False, "err": "bad CBS1"}, 400)
            try:
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
                self.series_count = total
                self.interval_us = interval_us
                self.running = True
                self.started_at = time.monotonic()
                return self._json({
                    "ok": True, "group_count": group_count,
                    "series_count": total, "sample_bytes": total * 4,
                    "interval_us": interval_us, "trigger": "periodic",
                    "halt": True, "pc_capture": True, "link_hz": 1000000,
                    "sample_every_max": 1,
                })
            except (IndexError, struct.error):
                return self._json({"ok": False, "err": "bad CBS1"}, 400)
        if path == CURVE_START_PATH and method == "POST":
            # Not used by the gate anymore, but keep dry-run compatibility.
            return self.request(method, path, body={})
        raise AssertionError("FakeDevice: unexpected raw request %s %s"
                             % (method, path))

    def request(self, method: str, path: str,
                body: Optional[Dict[str, Any]] = None,
                timeout: Optional[float] = None,
                retries: int = 1) -> Response:
        del timeout, retries
        for forbidden in FORBIDDEN_PATHS:
            if path == forbidden or path.startswith(forbidden + "?") \
                    or path.startswith(forbidden + "/"):
                raise AssertionError("gate attempted forbidden write: %s %s"
                                     % (method, path))
        if path == TARGET_INFO_PATH and method == "GET":
            if not self.detected:
                return self._json({"module": 6, "cmd": 1, "ok": False,
                                   "err": "target not detected", "code": 5,
                                   "detected": False})
            return self._json({
                "module": 6, "cmd": 1, "ok": True, "detected": True,
                "family": "stm32h7", "series": "STM32H7",
                "device": "STM32H723/H725/H730/H733/H735",
                "supported": True, "swd_clock_hz": self.swd_clock_hz,
                "dpidr": "0x6ba02477", "cpuid": "0x411fc272",
                "idcode": "0x10016483", "uid": "<target-uid>",
                "flash_base": "0x08000000", "flash_size": 1048576,
                "max_image_size": 1048576, "dev_id": 1155,
                "flash_kib": 1024,
                "model_candidates": "STM32H723/H725/H730/H733/H735 [xG, 1024 KiB]",
                "core": "Cortex-M7", "detection_confidence": "flash-narrowed",
            })
        if path == TARGET_STATUS_PATH and method == "GET":
            data: Dict[str, Any] = {
                "module": 6, "cmd": 3, "ok": True, "state": "IDLE",
                "total": 0, "written": 0, "verified": 0, "percent": 0,
                "err": 0, "message": "probe ok: STM32H723 @1000 kHz",
                "busy": False,
            }
            if self.diagnostics:
                data.update({"heap_free": self.heap_free,
                             "heap_largest": self.heap_largest,
                             "dap_session_active": False,
                             "swd_clock_hz": self.swd_clock_hz})
            return self._json(data)
        if path == CURVE_START_PATH and method == "POST":
            self.series_count = len(body.get("series", [])) if body else 0
            self.interval_us = int(body.get("interval_us", 20000)) if body else 20000
            self.running = True
            self.started_at = time.monotonic()
            return self._json({
                "ok": True, "group_count": 1,
                "series_count": self.series_count,
                "sample_bytes": self.series_count * 4,
                "interval_us": self.interval_us, "trigger": "periodic",
                "halt": True, "pc_capture": True, "link_hz": 1000000,
            })
        if path == CURVE_STOP_PATH and method == "POST":
            self.running = False
            return self._json({"ok": True})
        if path == CURVE_STATUS_PATH and method == "GET":
            if not self.running:
                return self._json({"ok": True, "running": False, "frames": 0,
                                   "error": 0, "invalid": 0, "overwritten": 0,
                                   "interval_us": self.interval_us,
                                   "sample_bytes": self.series_count * 4,
                                   "series_count": self.series_count})
            frames = self._curve_frames()
            return self._json({
                "ok": True, "running": True, "frames": len(frames),
                "interval_us": self.interval_us, "error": 0, "invalid": 0,
                "overwritten": 0, "sample_bytes": self.series_count * 4,
                "series_count": self.series_count, "link_hz": 1000000,
            })
        if path.startswith(CURVE_FRAMES_PATH):
            query = path.split("?", 1)[1] if "?" in path else ""
            params = {}
            for pair in query.split("&"):
                if "=" in pair:
                    key, value = pair.split("=", 1)
                    params[key] = value
            since = int(params.get("since", "0") or 0)
            max_frames = int(params.get("max", "32") or 32)
            frames = self._curve_frames()
            frames = [frame for frame in frames if frame.seq > since]
            frames = frames[:max_frames]
            if not frames:
                return Response(status=204, headers={}, body=b"")
            return Response(status=200,
                            headers={"content-type": "application/octet-stream"},
                            body=self._encode_page(frames))
        if path == "/" and method == "GET":
            return Response(status=200,
                            headers={"content-type": "text/html"},
                            body=DRY_RUN_HTML.encode("utf-8"))
        raise AssertionError("FakeDevice: unexpected request %s %s"
                             % (method, path))


def run_dry_run(args: argparse.Namespace) -> int:
    """Offline self-test: 5 fake devices, each with an expected exit code."""
    scenarios = [
        ("healthy-firmware", dict(detected=True, diagnostics=True,
                                  frame_format="cvf2-pc", effective_fps=50.0),
         False, EXIT_PASS),
        ("legacy-firmware-strict", dict(diagnostics=False,
                                        frame_format="cvf1", effective_fps=50.0),
         False, EXIT_FAIL),
        ("legacy-firmware-allowed", dict(diagnostics=False,
                                         frame_format="cvf1", effective_fps=50.0),
         True, EXIT_PASS),
        ("low-sampling-rate", dict(diagnostics=True,
                                   frame_format="cvf2-pc", effective_fps=10.0),
         False, EXIT_FAIL),
        ("target-not-detected", dict(detected=False),
         False, EXIT_FAIL),
    ]
    results = []
    failures = 0
    for name, device_options, allow_legacy, expected in scenarios:
        scenario_args = argparse.Namespace(**vars(args))
        scenario_args.dry_run = False
        scenario_args.host = "dry-run"
        scenario_args.port = 0
        scenario_args.duration = 0.3
        scenario_args.one_series_duration = 0.15
        scenario_args.poll_interval = 0.02
        scenario_args.playwright = "off"
        scenario_args.verbose = False
        scenario_args.allow_legacy_firmware = allow_legacy
        scenario_args.allow_dap_session = False
        report = run_gate(scenario_args, transport=FakeDevice(**device_options))
        actual = int(report["exit_code"])
        ok = actual == expected
        if not ok:
            failures += 1
        results.append({
            "scenario": name,
            "expected_exit": expected,
            "actual_exit": actual,
            "ok": ok,
            "required_failed": report["summary"]["required_failed"],
            "warnings": report["warnings"],
        })
        print("[dry-run] %-26s expected=%d actual=%d %s"
              % (name, expected, actual, "OK" if ok else "MISMATCH"),
              file=sys.stderr)
    summary = {
        "tool": TOOL_NAME,
        "version": TOOL_VERSION,
        "dry_run": True,
        "ok": failures == 0,
        "scenarios": results,
        "failures": failures,
    }
    if args.json:
        print(json.dumps(summary, indent=2))
    else:
        print("dry-run scenarios: %d/%d as expected"
              % (len(results) - failures, len(results)))
    return EXIT_PASS if failures == 0 else EXIT_FAIL



# ---------------------------------------------------------------------------
# Extended (opt-in) HIL modes: USBIP/OpenOCD, Web Flash, OTA, soak
# ---------------------------------------------------------------------------
#
# Everything in this section is OFF by default.  The classic gate above stays
# strictly read-only; a caller has to enable a mode explicitly and (for the
# write paths) pass a second safety switch *and* a concrete image file:
#
#   --usbip-smoke          host-side USBIP/OpenOCD session smoke (needs root)
#   --reboot-recovery      force the known HTTP-probe -> CMD_INFO failure, then
#                          recover by rebooting the ESP32 and prove USBIP works
#   --flash-image PATH --flash-allow
#                          POST the image to /api/target/flash (target Flash
#                          write), then reset/run the target and re-probe it
#   --ota-image PATH --ota-allow [--ota-restore-image PATH]
#                          upload the image over /api/ota/upload, poll the OTA
#                          module until the reboot, then (optionally) upload a
#                          restore image and verify the running version again
#   --ota-interrupt        start an OTA upload and abort it mid-stream: the
#                          device must stay on the running slot
#   --soak-seconds N       N-second stability loop (curve start/stop cycles +
#                          heap/HTTP/latency sampling); N=0 disables it
#
# Every mode records its own checks under ids starting with "extended." and
# writes a machine-readable summary into report["extended"].  Environment
# limitations (no root helper, no OpenOCD build, no image file) are recorded
# as *skipped* checks with an explicit reason, never as PASS claims.

#: Default OpenOCD install used by the repo (see scripts/build-openocd-master.sh).
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OPENOCD = os.path.join(REPO_ROOT, "build/openocd-master-install/bin/openocd")
DEFAULT_OPENOCD_SCRIPTS = os.path.join(
    REPO_ROOT, "build/openocd-master-install/share/openocd/scripts")

SWD_CLOCK_PATH = "/api/target/swd-clock"
TARGET_FLASH_PATH = "/api/target/flash"
TARGET_RESET_PATH = "/api/target/reset"
OTA_UPLOAD_PATH = "/api/ota/upload"

#: OTA JSON module (docs/protocol.md §2.5).
OTA_MODULE_ID = 5
OTA_GET_INFO = 1
OTA_GET_PROGRESS = 3
OTA_ABORT = 4


class ExtendedUnavailable(Exception):
    """An opt-in extended mode cannot run in this environment (skip, no fail)."""


def _extended_enabled(args: argparse.Namespace) -> bool:
    return bool(getattr(args, "usbip_smoke", False)
                or getattr(args, "reboot_recovery", False)
                or getattr(args, "flash_image", None)
                or getattr(args, "ota_image", None)
                or getattr(args, "ota_interrupt", False)
                or (getattr(args, "soak_seconds", 0) or 0) > 0)


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


def flash_image_precheck(image_path: Optional[str], image_size: Optional[int],
                         expect_sha256: str, target: Dict[str, Any],
                         expect_uid: str = "",
                         expect_flash_kib: int = 0,
                         require_h7: bool = True) -> List[str]:
    """Return a list of human-readable reasons a Web-Flash write must not run.

    Pure function: no I/O besides ``os.path`` checks on the image path, so the
    host unit tests can exercise every refusal branch offline.
    """
    errors: List[str] = []
    if not image_path:
        return ["no --flash-image given"]
    if not os.path.isfile(image_path):
        errors.append("image file not found: %s" % image_path)
        size = None
    else:
        size = os.path.getsize(image_path)
        if size <= 0:
            errors.append("image file is empty: %s" % image_path)
        if image_size is not None and size != image_size:
            errors.append("image size changed while reading: %d != %d"
                          % (size, image_size))
    if not target or target.get("detected") is not True:
        errors.append("target not detected (%.0f%% of the write would be unsafe)"
                      % 100)
        return errors
    if target.get("supported") is not True:
        errors.append("target family %r is detection-only (no flash driver)"
                      % (target.get("family"),))
    flash_size = target.get("flash_size") or 0
    if not flash_size:
        errors.append("target flash size unknown")
    elif size is not None and size > flash_size:
        errors.append("image (%d B) does not fit target flash (%d B)"
                      % (size, flash_size))
    family = str(target.get("family") or "")
    if require_h7 and not family.lower().startswith("stm32h7"):
        errors.append("only STM32H7 writes are covered by this gate "
                      "(detected %r); pass --flash-any-target to override"
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
        if not image_path or not os.path.isfile(image_path):
            errors.append("cannot verify sha256 of missing image")
        else:
            actual = sha256_file(image_path)
            if actual.lower() != expect_sha256.lower():
                errors.append("image sha256 mismatch: %s != %s"
                              % (actual, expect_sha256))
    return errors


def detect_root_helper(explicit: str = "") -> Optional[List[str]]:
    """Resolve a command prefix that runs a subprocess as root.

    Order: explicit CLI value, ``$HIL_ROOT_HELPER``, the lab's
    ``/tmp/hostroot.sh`` helper, then passwordless ``sudo``.  Returns None when
    no helper is available (the USBIP modes are then skipped as environment
    limitations, not failures).
    """
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


def _run_subprocess(argv: List[str], timeout: float) -> Dict[str, Any]:
    started = time.monotonic()
    try:
        proc = subprocess.run(argv, capture_output=True, text=True,
                              timeout=max(1.0, timeout))
        return {
            "rc": proc.returncode,
            "seconds": round(time.monotonic() - started, 3),
            "output": ((proc.stdout or "") + (proc.stderr or ""))[-4000:],
        }
    except subprocess.TimeoutExpired as exc:
        output = ((exc.stdout or "") + (exc.stderr or ""))
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        return {
            "rc": 124,
            "seconds": round(time.monotonic() - started, 3),
            "output": output[-4000:] + "\n[hil_gate] TIMEOUT\n",
        }
    except OSError as exc:
        return {"rc": 127, "seconds": 0.0, "output": "cannot execute: %s" % exc}


def _openocd_argv(args: argparse.Namespace) -> List[str]:
    return [
        args.openocd_path,
        "-f", "interface/cmsis-dap.cfg",
        "-c", "adapter usb vid_pid 0xc251 0xf00a",
        "-c", "transport select swd",
        "-f", "target/stm32h7x.cfg",
        "-c", "adapter speed 1900",
        "-c", "reset_config none",
        "-c", "cortex_m reset_config sysresetreq",
    ]


def openocd_probe(args: argparse.Namespace, helper: Optional[List[str]]) -> Dict[str, Any]:
    """Run one short OpenOCD CMSIS-DAP session (init + reset halt + shutdown)."""
    if getattr(args, "_offline", False):
        raise ExtendedUnavailable("offline --dry-run self-test: no OpenOCD")
    argv = _openocd_argv(args) + ["-c", "init", "-c", "reset halt",
                                  "-c", "shutdown"]
    if helper:
        argv = helper + argv
    env_note = ""
    if not os.path.isfile(args.openocd_path):
        raise ExtendedUnavailable(
            "OpenOCD binary not found: %s (build it with "
            "scripts/build-openocd-master.sh)" % args.openocd_path)
    if not os.path.isdir(args.openocd_scripts):
        raise ExtendedUnavailable(
            "OpenOCD scripts dir not found: %s" % args.openocd_scripts)
    if not helper:
        raise ExtendedUnavailable(
            "no root helper for 'usbip attach' (set HIL_ROOT_HELPER or use "
            "--usbip-root-helper; sudo -n is not available)")
    # OPENOCD_SCRIPTS is inherited from the caller's environment.
    old_scripts = os.environ.get("OPENOCD_SCRIPTS")
    os.environ["OPENOCD_SCRIPTS"] = args.openocd_scripts
    try:
        result = _run_subprocess(argv, args.openocd_timeout)
    finally:
        if old_scripts is None:
            os.environ.pop("OPENOCD_SCRIPTS", None)
        else:
            os.environ["OPENOCD_SCRIPTS"] = old_scripts
    ok = (result["rc"] == 0
          and "CMSIS-DAP" in result["output"]
          and "Interface Initialised" in result["output"])
    result["ok"] = ok
    result["version"] = _openocd_version(args)
    if not ok:
        result["error_tail"] = "\n".join(result["output"].strip().splitlines()[-8:])
    return result


def _openocd_version(args: argparse.Namespace) -> str:
    try:
        probe = subprocess.run([args.openocd_path, "--version"],
                               capture_output=True, text=True, timeout=20)
        first = ((probe.stdout or "") + (probe.stderr or "")).strip().splitlines()
        return first[0] if first else "unknown"
    except (OSError, subprocess.TimeoutExpired, IndexError):
        return "unknown"


def _usbip_attach(args: argparse.Namespace, helper: List[str]) -> Dict[str, Any]:
    result = _run_subprocess(helper + ["usbip", "attach",
                                       "-r", args.usbip_host,
                                       "-b", args.usbip_busid], 30)
    result["ok"] = result["rc"] == 0 and "usbip" in result["output"].lower()
    return result


def _usbip_detach(args: argparse.Namespace, helper: List[str]) -> Dict[str, Any]:
    return _run_subprocess(helper + ["usbip", "detach", "-p",
                                     str(args.usbip_port_index)], 30)


def phase_extended_usbip(gate: Gate, args: argparse.Namespace) -> None:
    """USBIP/OpenOCD session smoke: single session and two consecutive ones."""
    section: Dict[str, Any] = {"status": "skipped", "sessions": []}
    gate.metric("extended_usbip", section)
    helper = detect_root_helper(args.usbip_root_helper)
    try:
        result = openocd_probe(args, helper)
    except ExtendedUnavailable as exc:
        gate.skip("extended.usbip.environment", str(exc))
        section["reason"] = str(exc)
        return
    gate.check("extended.usbip.version",
               bool(result.get("version")),
               "OpenOCD: %s" % result.get("version"),
               severity="info",
               values={"openocd": result.get("version")})
    try:
        detach = _usbip_detach(args, helper)
        section["detach_before"] = detach
        attach = _usbip_attach(args, helper)
        section["attach"] = attach
        if not attach["ok"]:
            gate.require("extended.usbip.attach", False,
                         "usbip attach failed (rc=%d): %s"
                         % (attach["rc"], attach["output"].strip()[-400:]))
        time.sleep(1.0)
        sessions_needed = max(1, args.usbip_sessions)
        for index in range(sessions_needed):
            probe = openocd_probe(args, helper)
            section["sessions"].append(probe)
            gate.check("extended.usbip.session.%d" % (index + 1), probe["ok"],
                       "OpenOCD session %d: %.2f s%s"
                       % (index + 1, probe["seconds"],
                          "" if probe["ok"] else "; " + probe.get("error_tail", "")),
                       values={"seconds": probe["seconds"], "rc": probe["rc"]})
            if not probe["ok"]:
                raise GateAbort("extended.usbip.session.%d" % (index + 1),
                                "OpenOCD session failed")
        section["status"] = "pass"
    finally:
        _usbip_detach(args, helper)


def phase_extended_reboot_recovery(gate: Gate, args: argparse.Namespace) -> None:
    """Prove the known 'HTTP probe -> CMD_INFO failure' state is recoverable.

    The current firmware frees the DAP ring buffers for an HTTP target probe and
    may fail to re-create them on a fragmented heap (docs/hil-gate.md §10).
    After such a probe the *next* USBIP session can fail with
    ``CMSIS-DAP command CMD_INFO failed``.  A plain ESP32 reboot restores the
    session.  This mode forces that state, records it, then must recover.
    """
    section: Dict[str, Any] = {"status": "skipped", "pre_reboot_ok": None,
                               "post_reboot_ok": None}
    gate.metric("extended_recovery", section)
    helper = detect_root_helper(args.usbip_root_helper)
    try:
        openocd_probe(args, helper)
    except ExtendedUnavailable as exc:
        gate.skip("extended.recovery.environment", str(exc))
        section["reason"] = str(exc)
        return
    try:
        # 1) HTTP probe: takes the DAP away from the USBIP data path.
        probe_event = {}
        try:
            response = HttpTransport(args.host, args.port, timeout=args.timeout,
                                     verbose=args.verbose).request(
                "GET", TARGET_INFO_PATH, retries=0, timeout=args.openocd_timeout)
            probe_event["status"] = response.status
        except DeviceUnreachable as exc:
            gate.require("extended.recovery.http_probe", False, str(exc))
            return
        section["http_probe"] = probe_event
        # 2) Attach and try a session: failure is allowed here (that is the bug).
        _usbip_detach(args, helper)
        attach = _usbip_attach(args, helper)
        if not attach["ok"]:
            gate.require("extended.recovery.attach", False,
                         "usbip attach failed: " + attach["output"].strip()[-300:])
        time.sleep(1.0)
        pre = openocd_probe(args, helper)
        section["pre_reboot_ok"] = pre["ok"]
        section["pre_reboot_output"] = pre.get("error_tail", pre["output"][-400:])
        gate.check("extended.recovery.known_failure_reproduced", True,
                   "pre-reboot USBIP session %s after HTTP probe"
                   % ("succeeded" if pre["ok"] else "failed (known ring "
                      "re-create limitation)"),
                   severity="info",
                   values={"pre_reboot_ok": pre["ok"]})
        # 3) Reboot the ESP32 (SYSTEM/REBOOT) and wait for the Web server.
        _usbip_detach(args, helper)
        ws = None
        try:
            ws = WsJsonClient(args.host, args.port, timeout=args.timeout)
            ws.request(0, 2, timeout=args.timeout)  # SYSTEM/REBOOT
        finally:
            if ws is not None:
                ws.close()
        deadline = time.monotonic() + args.reboot_timeout
        reachable = False
        while time.monotonic() < deadline:
            time.sleep(1.0)
            try:
                probe_http = HttpTransport(args.host, args.port,
                                           timeout=2.0).request(
                    "GET", TARGET_STATUS_PATH, retries=0, timeout=2.0)
                if probe_http.status == 200:
                    reachable = True
                    break
            except DeviceUnreachable:
                continue
        gate.require("extended.recovery.reboot_reachable", reachable,
                     "device did not come back within %.0f s after reboot"
                     % args.reboot_timeout)
        # 4) Fresh session after reboot must pass.
        attach = _usbip_attach(args, helper)
        time.sleep(2.0)
        post = openocd_probe(args, helper)
        section["post_reboot_ok"] = post["ok"]
        section["post_reboot_seconds"] = post["seconds"]
        gate.require("extended.recovery.post_reboot_session", post["ok"],
                     "OpenOCD session after reboot failed: %s"
                     % post.get("error_tail", post["output"][-400:]),
                     ok_detail="USBIP/OpenOCD recovered by ESP32 reboot "
                               "(%.2f s)" % post["seconds"])
        section["status"] = "pass" if post["ok"] else "fail"
    finally:
        _usbip_detach(args, helper)


def phase_extended_flash(gate: Gate, args: argparse.Namespace) -> None:
    """Opt-in Web Flash write of an explicit test image (target Flash write)."""
    section: Dict[str, Any] = {"status": "skipped", "image": args.flash_image}
    gate.metric("extended_flash", section)
    if not args.flash_image:
        return
    image = os.path.abspath(os.path.expanduser(args.flash_image))
    section["image"] = image
    try:
        size = os.path.getsize(image)
    except OSError as exc:
        gate.require("extended.flash.image", False,
                     "cannot read --flash-image %s: %s" % (image, exc))
        return
    section["image_size"] = size
    if args.flash_expect_sha256:
        actual_sha = sha256_file(image)
        section["image_sha256"] = actual_sha
    else:
        actual_sha = sha256_file(image)
        section["image_sha256"] = actual_sha

    # -- pre-flight: identity / capacity / UID confirmation -----------------
    info_response = HttpTransport(args.host, args.port, timeout=args.timeout,
                                  verbose=args.verbose).request(
        "GET", TARGET_INFO_PATH, retries=2)
    info_data = info_response.json()
    target = {
        "detected": info_data.get("detected") is True,
        "supported": info_data.get("supported") is True,
        "family": info_data.get("family"),
        "flash_size": integer(info_data.get("flash_size")),
        "uid": info_data.get("uid"),
        "device": info_data.get("device"),
    }
    section["target_before"] = target
    errors = flash_image_precheck(
        image, size, args.flash_expect_sha256, target,
        expect_uid=args.flash_expect_uid,
        expect_flash_kib=args.flash_expect_flash_kib,
        require_h7=not args.flash_any_target)
    gate.require("extended.flash.precheck", not errors,
                 "; ".join(errors) if errors else "",
                 ok_detail="image %s (%d B) confirmed against target %s "
                           "UID=%s flash=%d KiB"
                           % (os.path.basename(image), size,
                              target.get("device"), target.get("uid"),
                              (target.get("flash_size") or 0) // 1024))
    if errors:
        return
    if not args.flash_allow:
        gate.require("extended.flash.allow", False,
                     "refusing to write target Flash without --flash-allow")
        return

    http = HttpTransport(args.host, args.port, timeout=args.flash_timeout,
                         verbose=args.verbose)
    started = time.monotonic()
    try:
        response = http.request_bytes(
            "POST", TARGET_FLASH_PATH, data=open(image, "rb").read(),
            headers={
                "Content-Type": "application/octet-stream",
                "X-Target-SHA256": actual_sha,
            }, timeout=args.flash_timeout)
    except DeviceUnreachable as exc:
        gate.require("extended.flash.upload", False,
                     "target Flash upload failed: %s" % exc)
        _safe_target_reset(gate, args, run=True)
        return
    elapsed = time.monotonic() - started
    section["flash_seconds"] = round(elapsed, 3)
    payload: Dict[str, Any] = {}
    try:
        payload = response.json()
    except ProtocolError as exc:
        gate.require("extended.flash.response", False, str(exc))
        _safe_target_reset(gate, args, run=True)
        return
    section["response"] = {key: payload.get(key) for key in
                           ("ok", "written", "verified", "sha256", "state",
                            "message", "err")}
    written = integer(payload.get("written")) or 0
    verified = integer(payload.get("verified")) or 0
    sha_ok = (not payload.get("sha256")
              or str(payload.get("sha256")).lower() == actual_sha.lower())
    gate.require("extended.flash.response", response.status == 200
                 and payload.get("ok") is True,
                 "HTTP %d ok=%r message=%r"
                 % (response.status, payload.get("ok"),
                    payload.get("message")),
                 values=section["response"])
    gate.require("extended.flash.written", written == size,
                 "written=%d expected=%d" % (written, size))
    gate.require("extended.flash.verified", verified == size,
                 "verified=%d expected=%d" % (verified, size))
    gate.require("extended.flash.sha256", sha_ok,
                 "response sha256 does not match the uploaded image")

    # -- finish: reset/run the target, then re-probe it ---------------------
    _safe_target_reset(gate, args, run=True)
    time.sleep(1.0)
    after_response = HttpTransport(args.host, args.port, timeout=args.timeout,
                                   verbose=args.verbose).request(
        "GET", TARGET_INFO_PATH, retries=2)
    after = after_response.json()
    detected_after = after.get("detected") is True
    uid_after = normalize_uid(after.get("uid"))
    uid_before = normalize_uid(target.get("uid"))
    gate.require("extended.flash.target_alive", detected_after,
                 "target not detected after write+reset: %r"
                 % (after.get("err"),))
    gate.require("extended.flash.uid_stable", (not uid_before) or uid_after == uid_before,
                 "target UID changed after write: %s -> %s"
                 % (uid_before, uid_after))
    section["target_after"] = {"detected": detected_after, "uid": uid_after}
    section["status"] = "pass"
    gate.info("extended.flash.summary",
              "wrote+verified %d B in %.2f s (%.1f KiB/s), target reset/run OK"
              % (size, elapsed, (size / 1024.0 / elapsed) if elapsed else 0.0),
              values=section)


def _safe_target_reset(gate: Gate, args: argparse.Namespace,
                       run: bool = True) -> None:
    try:
        response = HttpTransport(args.host, args.port, timeout=args.timeout) \
            .request("POST", TARGET_RESET_PATH, body={"run": bool(run)},
                     retries=1)
        gate.check("extended.cleanup.target_reset", response.status == 200,
                   "target reset(run=%r) HTTP %d" % (run, response.status),
                   severity="warn")
    except (DeviceUnreachable, ProtocolError) as exc:
        gate.check("extended.cleanup.target_reset", False,
                   "target reset failed: %s" % exc, severity="warn")


class WsJsonClient:
    """Minimal stdlib WebSocket JSON client for SYSTEM/OTA module queries.

    Only what the extended HIL modes need: RFC 6455 client handshake, masked
    text frames, JSON request/response matching on (module, cmd).  Messages may
    arrive out of order or as binary frames; those are ignored until the
    matching JSON response appears (same rule as scripts/e2e_ws_client.py).
    """

    GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, host: str, port: int = 80, path: str = "/ws",
                 timeout: float = 10.0, verbose: bool = False) -> None:
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self.verbose = verbose
        self.sock: Optional[socket.socket] = None
        self._buffer = b""

    # -- transport ---------------------------------------------------------
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
                raise ProtocolError("WS handshake: connection closed")
            response += chunk
            if len(response) > 16384:
                raise ProtocolError("WS handshake: response too large")
        header, _, rest = response.partition(b"\r\n\r\n")
        self._buffer = rest
        lines = header.decode("latin-1").split("\r\n")
        if not lines or "101" not in lines[0]:
            raise ProtocolError("WS handshake failed: %r" % lines[0])
        accept_expected = base64.b64encode(
            hashlib.sha1((key + self.GUID).encode("ascii")).digest()
        ).decode("ascii")
        accepted = ""
        for line in lines[1:]:
            if ":" in line and line.split(":", 1)[0].strip().lower() == \
                    "sec-websocket-accept":
                accepted = line.split(":", 1)[1].strip()
        if accepted != accept_expected:
            raise ProtocolError("WS handshake: bad Sec-WebSocket-Accept")

    def _recv_exact(self, count: int) -> bytes:
        while len(self._buffer) < count:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ProtocolError("WS connection closed")
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

    # -- JSON protocol -----------------------------------------------------
    def send_json(self, payload: Dict[str, Any]) -> None:
        self._send_frame(0x1, json.dumps(payload).encode("utf-8"))

    def request(self, module: int, cmd: int,
                extra: Optional[Dict[str, Any]] = None,
                timeout: Optional[float] = None) -> Dict[str, Any]:
        wait = self.timeout if timeout is None else timeout
        payload: Dict[str, Any] = {"module": module, "cmd": cmd}
        if extra:
            payload.update(extra)
        self.send_json(payload)
        deadline = time.monotonic() + wait
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("WS timeout waiting for module=%d cmd=%d"
                                    % (module, cmd))
            self.sock.settimeout(remaining)
            opcode, data = self._recv_frame()
            if opcode == 0x8:
                raise ProtocolError("WS server closed the connection")
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
            if self.verbose:
                print("[hil_gate] WS ignoring module=%r cmd=%r"
                      % (parsed.get("module"), parsed.get("cmd")),
                      file=sys.stderr)

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


def _ota_info(ws: WsJsonClient) -> Dict[str, Any]:
    info = ws.request(OTA_MODULE_ID, OTA_GET_INFO)
    return {
        "running": info.get("running"),
        "running_ver": info.get("running_ver"),
        "status": info.get("status"),
        "free": info.get("free"),
        "slot_size": info.get("slot_size"),
        "rollback": info.get("rollback"),
    }


def _wait_for_ota_boot(ws_factory: Any, ping: Any, previous_ver: Any,
                       timeout: float) -> Dict[str, Any]:
    """Poll WS OTA/GET_INFO until the running version changes (or timeout)."""
    deadline = time.monotonic() + timeout
    last: Dict[str, Any] = {}
    latest_error: Optional[str] = None
    while time.monotonic() < deadline:
        time.sleep(1.0)
        if not ping():
            continue
        try:
            ws = ws_factory()
            try:
                last = _ota_info(ws)
            finally:
                ws.close()
        except (OSError, ProtocolError) as exc:
            latest_error = str(exc)
            continue
        if last.get("running_ver") and last.get("running_ver") != previous_ver:
            return {"booted": True, "info": last}
    return {"booted": False, "info": last, "last_error": latest_error}


def _ota_upload(http_conn: HttpTransport, image: str, sha256: str,
                timeout: float, partial_bytes: int = 0) -> Dict[str, Any]:
    """POST an app image to /api/ota/upload.

    ``partial_bytes > 0`` sends only that prefix of the image and closes the
    connection immediately (interrupted-upload test).
    """
    data = open(image, "rb").read()
    headers = {"Content-Type": "application/octet-stream",
               "X-SHA256": sha256}
    body = data[:partial_bytes] if partial_bytes else data
    try:
        response = http_conn.request_bytes("POST", OTA_UPLOAD_PATH, data=body,
                                           headers=headers, timeout=timeout,
                                           close_after=(partial_bytes > 0))
        payload: Any = response.body[:500]
        try:
            payload = response.json()
        except ProtocolError:
            pass
        return {"status": response.status, "seconds": response.elapsed_ms,
                "payload": payload, "aborted": False}
    except DeviceUnreachable as exc:
        if partial_bytes:
            return {"status": 0, "aborted": True,
                    "payload": {"note": "connection closed after %d B" % len(body)},
                    "error": str(exc)}
        return {"status": 0, "aborted": True, "payload": {},
                "error": str(exc)}


def _safe_ota_abort(ws: WsJsonClient) -> None:
    try:
        ws.request(OTA_MODULE_ID, OTA_ABORT, timeout=5.0)
    except (OSError, ProtocolError):
        pass


def phase_extended_ota(gate: Gate, args: argparse.Namespace) -> None:
    """Opt-in OTA upload / interruption tests."""
    section: Dict[str, Any] = {"status": "skipped"}
    gate.metric("extended_ota", section)
    if not (args.ota_image or args.ota_interrupt):
        return

    def ws_factory() -> WsJsonClient:
        client = WsJsonClient(args.host, args.port, timeout=args.timeout,
                              verbose=args.verbose)
        client.connect()
        return client

    def ping() -> bool:
        try:
            http = HttpTransport(args.host, args.port, timeout=2.0)
            return http.request("GET", TARGET_STATUS_PATH, retries=0,
                                timeout=2.0).status == 200
        except DeviceUnreachable:
            return False

    try:
        ws = ws_factory()
        try:
            initial = _ota_info(ws)
            progress = ws.request(OTA_MODULE_ID, OTA_GET_PROGRESS)
            abort_supported = True
        finally:
            ws.close()
    except (OSError, ProtocolError) as exc:
        gate.require("extended.ota.ws", False,
                     "WS OTA module unavailable: %s" % exc)
        return
    section["initial_info"] = initial
    section["abort_supported"] = abort_supported
    gate.check("extended.ota.rollback_capability",
               bool(initial.get("rollback")),
               "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=%r in OTA GET_INFO"
               % (initial.get("rollback"),),
               severity="info")
    gate.require("extended.ota.idle", initial.get("status") in ("IDLE", None),
                 "OTA session busy before the test: status=%r"
                 % (initial.get("status"),))
    http = HttpTransport(args.host, args.port, timeout=args.timeout,
                         verbose=args.verbose)

    # -- interrupted upload: device must keep running the current slot ------
    if args.ota_interrupt:
        try:
            target_ws = ws_factory()
        except (OSError, ProtocolError) as exc:
            gate.require("extended.ota.interrupt.ws", False, str(exc))
            return
        try:
            before = _ota_info(target_ws)
            image_path = args.ota_image or args.ota_restore_image
            if not image_path:
                gate.require("extended.ota.interrupt.image", False,
                             "--ota-interrupt needs --ota-image (or "
                             "--ota-restore-image) as the payload source")
                return
            sha = sha256_file(image_path)
            partial = min(os.path.getsize(image_path) // 2, 256 * 1024)
            result = _ota_upload(http, image_path, sha, args.ota_timeout,
                                 partial_bytes=partial)
            section["interrupt"] = {
                "partial_bytes": partial,
                "upload_status": result.get("status"),
                "aborted": result.get("aborted"),
            }
            time.sleep(args.ota_settle)
            after = _ota_info(target_ws)
            section["interrupt"]["info_after"] = after
            gate.require(
                "extended.ota.interrupt.running_unchanged",
                after.get("running_ver") == before.get("running_ver")
                and after.get("running") == before.get("running"),
                "running slot/version changed after an interrupted upload: "
                "%r -> %r" % (before.get("running_ver"),
                              after.get("running_ver")))
            gate.require("extended.ota.interrupt.recovered",
                         after.get("status") in ("IDLE", "ERROR"),
                         "OTA state did not settle after client disconnect: %r"
                         % (after.get("status"),))
            _safe_ota_abort(target_ws)
            section["status"] = "pass"
        finally:
            target_ws.close()

    # -- full upload: boot the new slot, then optionally restore ------------
    if args.ota_image and not args.ota_restore_image:
        gate.check("extended.ota.restore", False,
                   "--ota-image without --ota-restore-image leaves the device "
                   "on the uploaded test image (explicit choice)", severity="warn")
    if args.ota_image:
        image = os.path.abspath(os.path.expanduser(args.ota_image))
        if not os.path.isfile(image):
            gate.require("extended.ota.image", False,
                         "OTA image not found: %s" % image)
            return
        sha = sha256_file(image)
        section["upload"] = {"image": image,
                             "size": os.path.getsize(image),
                             "sha256": sha}
        result = _ota_upload(http, image, sha, args.ota_timeout)
        section["upload"]["status"] = result.get("status")
        section["upload"]["payload"] = result.get("payload")
        gate.require("extended.ota.upload", result.get("status") == 200,
                     "OTA upload failed: HTTP %r payload=%r"
                     % (result.get("status"), result.get("payload")))
        boot = _wait_for_ota_boot(ws_factory, ping, initial.get("running_ver"),
                                  args.ota_timeout)
        section["boot"] = boot
        gate.require("extended.ota.booted", boot.get("booted") is True,
                     "device did not report a new running version within %.0f s"
                     % args.ota_timeout,
                     values={"info": boot.get("info")})
        if args.ota_restore_image:
            restore = os.path.abspath(os.path.expanduser(args.ota_restore_image))
            if not os.path.isfile(restore):
                gate.require("extended.ota.restore_image", False,
                             "restore image not found: %s" % restore)
                return
            restore_sha = sha256_file(restore)
            restored_ver = ((boot.get("info") or {}).get("running_ver"))
            restore_result = _ota_upload(http, restore, restore_sha,
                                         args.ota_timeout)
            section["restore"] = {"image": restore,
                                  "size": os.path.getsize(restore),
                                  "sha256": restore_sha,
                                  "status": restore_result.get("status"),
                                  "payload": restore_result.get("payload")}
            gate.require("extended.ota.restore_upload",
                         restore_result.get("status") == 200,
                         "restore upload failed: HTTP %r payload=%r"
                         % (restore_result.get("status"),
                            restore_result.get("payload")))
            restored = _wait_for_ota_boot(ws_factory, ping, restored_ver,
                                          args.ota_timeout)
            section["restore"]["boot"] = restored
            gate.require("extended.ota.restored",
                         restored.get("booted") is True,
                         "device did not boot the restore image within %.0f s"
                         % args.ota_timeout,
                         ok_detail="OTA round-trip verified and original image "
                                   "restored (version=%r)"
                                   % ((restored.get("info") or {})
                                      .get("running_ver"),))
        section["status"] = "pass"


def phase_extended_soak(gate: Gate, args: argparse.Namespace,
                        http: HttpTransport) -> None:
    """Opt-in stability soak: repeated curve sessions + heap/latency sampling."""
    section: Dict[str, Any] = {"status": "skipped", "seconds": args.soak_seconds}
    gate.metric("extended_soak", section)
    if not args.soak_seconds:
        return
    iterations = 0
    failures = 0
    heaps: List[int] = []
    latencies: List[float] = []
    curve_errors = 0
    target_misses = 0
    started = time.monotonic()
    deadline = started + args.soak_seconds
    series = max(1, min(4, args.soak_series))
    gate.check("extended.soak.started", True,
               "soak %.0f s, %.1f s per cycle, %d series"
               % (args.soak_seconds, args.soak_interval, series),
               severity="info")
    while time.monotonic() < deadline:
        iterations += 1
        try:
            start = http.request("POST", CURVE_START_PATH, body={
                "interval_us": args.soak_interval_us,
                "series": [{"id": index, "size": 4, "expr": {
                    "kind": "addr",
                    "address": args.address + index * 4}} for index in
                    range(series)],
                "trigger": "periodic",
            }, timeout=args.timeout, retries=0)
            latencies.append(start.elapsed_ms)
            if start.status != 200:
                failures += 1
                continue
            cycle_end = min(deadline, time.monotonic() + args.soak_interval)
            frames = 0
            while time.monotonic() < cycle_end:
                status = http.request("GET", CURVE_STATUS_PATH,
                                      timeout=args.timeout, retries=0)
                latencies.append(status.elapsed_ms)
                status_data = status.json()
                frames = int(status_data.get("frames") or 0)
                curve_errors += int(status_data.get("error") or 0)
                if status_data.get("running") is not True:
                    failures += 1
                    break
                time.sleep(max(0.05, args.soak_poll))
            stop = http.request("POST", CURVE_STOP_PATH, body={},
                                timeout=args.timeout, retries=1)
            latencies.append(stop.elapsed_ms)
            if stop.status != 200:
                failures += 1
            status = http.request("GET", TARGET_STATUS_PATH,
                                  timeout=args.timeout, retries=0)
            latencies.append(status.elapsed_ms)
            status_data = status.json()
            heap = integer(status_data.get("heap_free"))
            if heap is not None:
                heaps.append(heap)
            section.setdefault("frames_per_cycle", []).append(frames)
            if iterations % 3 == 0:
                info = http.request("GET", TARGET_INFO_PATH,
                                    timeout=args.timeout, retries=0)
                latencies.append(info.elapsed_ms)
                if info.json().get("detected") is not True:
                    target_misses += 1
        except (DeviceUnreachable, ProtocolError) as exc:
            failures += 1
            section["last_error"] = str(exc)
            time.sleep(1.0)
        except GateAbort:
            raise
    elapsed = time.monotonic() - started
    heap_start = heaps[0] if heaps else None
    heap_end = heaps[-1] if heaps else None
    heap_min = min(heaps) if heaps else None
    section.update({
        "iterations": iterations,
        "elapsed_s": round(elapsed, 1),
        "failures": failures,
        "curve_errors": curve_errors,
        "target_misses": target_misses,
        "heap_free_start": heap_start,
        "heap_free_end": heap_end,
        "heap_free_min": heap_min,
        "heap_delta": (heap_end - heap_start)
        if heap_start is not None and heap_end is not None else None,
        "http_latency_avg_ms": round(sum(latencies) / len(latencies), 1)
        if latencies else None,
        "http_latency_max_ms": round(max(latencies), 1) if latencies else None,
    })
    gate.require("extended.soak.iterations", iterations > 0,
                 "soak performed no iterations")
    gate.require("extended.soak.no_failures", failures == 0,
                 "%d failed sample cycle(s) during soak" % failures)
    gate.require("extended.soak.curve_errors", curve_errors == 0,
                 "curve sampler reported %d error(s) during soak"
                 % curve_errors)
    gate.require("extended.soak.target_alive", target_misses == 0,
                 "target was not detected %d time(s) during soak"
                 % target_misses)
    if heap_min is not None:
        gate.require("extended.soak.heap_min", heap_min >= args.heap_min_free,
                     "heap_free dropped to %d B (threshold %d B)"
                     % (heap_min, args.heap_min_free),
                     values={"heap_free_min": heap_min})
        gate.require("extended.soak.heap_no_leak",
                     section["heap_delta"] is not None
                     and section["heap_delta"] >= -args.heap_leak_tolerance,
                     "heap_free changed by %r B over the soak (tolerance -%d B)"
                     % (section["heap_delta"], args.heap_leak_tolerance))
    section["status"] = "pass" if not failures and curve_errors == 0 \
        and target_misses == 0 else "fail"
    gate.info("extended.soak.summary",
              "%d cycles in %.1f s, failures=%d, heap %r..%r B, "
              "HTTP avg=%.1f ms max=%.1f ms"
              % (iterations, elapsed, failures, heap_min, heap_end,
                 section.get("http_latency_avg_ms") or 0.0,
                 section.get("http_latency_max_ms") or 0.0),
              values=section)


def phase_extended(gate: Gate, args: argparse.Namespace,
                   http: HttpTransport) -> None:
    """Run every enabled extended mode, isolated so one failure cannot skip the
    rest, and always stop the curve sampler at the end."""
    gate.metric("extended_enabled", True)
    gate.check("extended.mode", True,
               "extended modes enabled: %s" % ", ".join(
                   name for name, on in (
                       ("usbip-smoke", args.usbip_smoke),
                       ("reboot-recovery", args.reboot_recovery),
                       ("flash", bool(args.flash_image)),
                       ("ota", bool(args.ota_image or args.ota_interrupt)),
                       ("soak", bool(args.soak_seconds)),
                   ) if on),
               severity="info")
    phases = []
    if args.usbip_smoke:
        phases.append(("usbip", phase_extended_usbip, (gate, args)))
    if args.reboot_recovery:
        phases.append(("recovery", phase_extended_reboot_recovery, (gate, args)))
    if args.flash_image:
        phases.append(("flash", phase_extended_flash, (gate, args)))
    if args.ota_image or args.ota_interrupt:
        phases.append(("ota", phase_extended_ota, (gate, args)))
    if args.soak_seconds:
        phases.append(("soak", phase_extended_soak, (gate, args, http)))
    for name, function, call_args in phases:
        try:
            function(*call_args)
        except ExtendedUnavailable as exc:
            gate.skip("extended.%s.environment" % name, str(exc))
        except GateAbort as exc:
            gate.check("extended.%s.aborted" % name, False,
                       "aborted at %s: %s" % (exc.check_id, exc.detail))
        except (DeviceUnreachable, ProtocolError) as exc:
            gate.check("extended.%s.transport" % name, False,
                       "transport error: %s" % exc)
        except Exception as exc:  # noqa: BLE001 - keep the report complete
            gate.check("extended.%s.internal" % name, False,
                       "%s: %s" % (type(exc).__name__, exc))
            if args.verbose:
                traceback.print_exc()
        finally:
            # A failed write test must never leave the target halted.
            if name == "flash":
                _safe_target_reset(gate, args, run=True)


def extended_cleanup(gate: Gate, args: argparse.Namespace) -> None:
    """Best-effort cleanup after the extended modes (never raises)."""
    helper = detect_root_helper(args.usbip_root_helper)
    if getattr(args, "_offline", False):
        helper = None
    if helper is not None and (args.usbip_smoke or args.reboot_recovery):
        try:
            _usbip_detach(args, helper)
            gate.info("extended.cleanup.usbip", "usbip detached (port %d)"
                      % args.usbip_port_index)
        except Exception as exc:  # noqa: BLE001
            gate.check("extended.cleanup.usbip", False,
                       "usbip detach failed: %s" % exc, severity="warn")
    try:
        status = HttpTransport(args.host, args.port, timeout=args.timeout) \
            .request("GET", CURVE_STATUS_PATH, retries=0)
        if status.json().get("running") is True:
            HttpTransport(args.host, args.port, timeout=args.timeout) \
                .request("POST", CURVE_STOP_PATH, body={}, retries=1)
            gate.check("extended.cleanup.curve_stopped", True,
                       "forced curve/stop after extended modes")
    except (DeviceUnreachable, ProtocolError):
        pass


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_u32(text: str) -> int:
    try:
        value = int(text, 16) if text.strip().lower().startswith("0x") \
            else int(text, 10)
    except (AttributeError, ValueError):
        raise argparse.ArgumentTypeError("expected a 32-bit integer, got %r"
                                         % (text,))
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("address out of 32-bit range: %r"
                                         % (text,))
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hil_gate.py",
        description="WiFi Dap HIL regression/performance gate "
                    "(read-only: no target Flash write, no OTA).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        epilog="Exit codes: 0 PASS, 1 FAIL, 2 usage, 3 device unreachable. "
               "See docs/hil-gate.md for thresholds and CI usage.")
    parser.add_argument("--host", default="<device-ip>",
                        help="device IP/hostname (no scheme)")
    parser.add_argument("--port", type=int, default=80,
                        help="device HTTP port")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-HTTP-request timeout in seconds")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="5-series sampling window in seconds")
    parser.add_argument("--one-series-duration", type=float, default=2.0,
                        help="single-series sampling window in seconds")
    parser.add_argument("--interval-us", type=int, default=20000,
                        help="curve sampling interval in microseconds")
    parser.add_argument("--series", type=int, default=5,
                        help="series count for the performance window")
    parser.add_argument("--fps-min", type=float, default=35.0,
                        help="minimum effective device sampling rate (fps)")
    parser.add_argument("--max-invalid-ratio", type=float, default=0.01,
                        help="maximum invalid-frame ratio")
    parser.add_argument("--address", type=parse_u32, default=parse_u32("0x20000000"),
                        help="fixed read-only target RAM address for sampling")
    parser.add_argument("--poll-interval", type=float, default=0.2,
                        help="status/frames polling interval in seconds")
    parser.add_argument("--heap-min-free", type=int, default=8192,
                        help="minimum heap_free in bytes (diagnostics check)")
    parser.add_argument("--heap-min-largest", type=int, default=4096,
                        help="minimum heap_largest in bytes (diagnostics check)")
    parser.add_argument("--heap-leak-tolerance", type=int, default=32768,
                        help="allowed heap_free drop after sampling (bytes)")
    parser.add_argument("--allow-legacy-firmware", action="store_true",
                        help="downgrade missing target/status diagnostics "
                             "fields to warnings (pre-OTA firmware)")
    parser.add_argument("--allow-dap-session", action="store_true",
                        help="allow an active DAP proxy session (curve/start "
                             "will kick it); default fails fast")
    parser.add_argument("--playwright", choices=("auto", "on", "off"),
                        default="auto",
                        help="Playwright SPA page smoke mode")
    parser.add_argument("--skip-pages", action="store_true",
                        help="skip the page smoke (index + Playwright)")
    parser.add_argument("--json", action="store_true",
                        help="print the report as JSON on stdout")
    parser.add_argument("--report", metavar="PATH", default=None,
                        help="also write the JSON report to PATH")
    parser.add_argument("--dry-run", action="store_true",
                        help="offline self-test with a fake device (no network)")
    parser.add_argument("--verbose", action="store_true",
                        help="log HTTP requests to stderr")

    extended = parser.add_argument_group(
        "extended (opt-in) HIL modes -- OFF by default, write paths need a "
        "second safety switch")
    extended.add_argument("--usbip-smoke", action="store_true",
                          help="host USBIP attach + N consecutive OpenOCD "
                               "CMSIS-DAP sessions (needs root helper)")
    extended.add_argument("--usbip-root-helper", default="",
                          help="command prefix that runs as root "
                               "(default: $HIL_ROOT_HELPER, /tmp/hostroot.sh, "
                               "passwordless sudo)")
    extended.add_argument("--usbip-host", default="",
                          help="device IP for 'usbip attach' (default: --host)")
    extended.add_argument("--usbip-busid", default="1-1",
                          help="usbip remote bus id (default: 1-1)")
    extended.add_argument("--usbip-port-index", type=int, default=0,
                          help="local vhci port index for detach (default: 0)")
    extended.add_argument("--usbip-sessions", type=int, default=2,
                          help="consecutive OpenOCD sessions in the smoke "
                               "(default: 2)")
    extended.add_argument("--openocd", dest="openocd_path",
                          default=DEFAULT_OPENOCD,
                          help="OpenOCD binary (default: repo "
                               "build/openocd-master-install/bin/openocd)")
    extended.add_argument("--openocd-scripts", default=DEFAULT_OPENOCD_SCRIPTS,
                          help="OpenOCD scripts directory")
    extended.add_argument("--openocd-timeout", type=float, default=120.0,
                          help="per-OpenOCD-session timeout in seconds")
    extended.add_argument("--reboot-recovery", action="store_true",
                          help="force the known HTTP-probe -> CMD_INFO failure, "
                               "reboot the ESP32 via WS and require USBIP to "
                               "recover")
    extended.add_argument("--reboot-timeout", type=float, default=90.0,
                          help="seconds to wait for the device to come back "
                               "after the recovery reboot")
    extended.add_argument("--flash-image", default=None,
                          help="target image for the opt-in Web Flash write "
                               "(POST /api/target/flash)")
    extended.add_argument("--flash-allow", action="store_true",
                          help="second safety switch: actually write the "
                               "target Flash")
    extended.add_argument("--flash-expect-sha256", default="",
                          help="expected sha256 of --flash-image")
    extended.add_argument("--flash-expect-uid", default="",
                          help="expected target UID (hex, spaces ignored)")
    extended.add_argument("--flash-expect-flash-kib", type=int, default=0,
                          help="expected target flash size in KiB (0=skip)")
    extended.add_argument("--flash-any-target", action="store_true",
                          help="allow non-STM32H7 targets (default: H7 only)")
    extended.add_argument("--flash-timeout", type=float, default=300.0,
                          help="HTTP timeout for the target Flash upload")
    extended.add_argument("--ota-image", default=None,
                          help="app image for the opt-in OTA upload test")
    extended.add_argument("--ota-allow", action="store_true",
                          help="second safety switch: actually OTA the device")
    extended.add_argument("--ota-restore-image", default=None,
                          help="image uploaded after the OTA test to restore "
                               "the original firmware")
    extended.add_argument("--ota-interrupt", action="store_true",
                          help="start an OTA upload and drop the connection "
                               "mid-stream; the running slot must not change")
    extended.add_argument("--ota-timeout", type=float, default=180.0,
                          help="OTA upload / reboot-poll timeout in seconds")
    extended.add_argument("--ota-settle", type=float, default=4.0,
                          help="wait after the interrupted upload before "
                               "checking OTA state")
    extended.add_argument("--soak-seconds", type=int, default=0,
                          help="opt-in stability soak length in seconds "
                               "(0=off; 30..3600; task baseline 300)")
    extended.add_argument("--soak-interval", type=float, default=5.0,
                          help="seconds per soak cycle")
    extended.add_argument("--soak-poll", type=float, default=0.2,
                          help="curve/status polling interval inside a cycle")
    extended.add_argument("--soak-interval-us", type=int, default=20000,
                          help="curve sampling interval for the soak")
    extended.add_argument("--soak-series", type=int, default=1,
                          help="series count for the soak (1..4)")
    return parser


def validate_args(parser: argparse.ArgumentParser,
                  args: argparse.Namespace) -> None:
    if args.dry_run:
        return
    if not args.host or "://" in args.host or "/" in args.host:
        parser.error("--host must be a bare IP/hostname: %r" % args.host)
    if not 1 <= args.port <= 65535:
        parser.error("--port out of range: %d" % args.port)
    if not 0.1 <= args.timeout <= 120:
        parser.error("--timeout out of range: %g" % args.timeout)
    if not 0.2 <= args.duration <= 300:
        parser.error("--duration out of range: %g" % args.duration)
    if not 0.1 <= args.one_series_duration <= 60:
        parser.error("--one-series-duration out of range: %g"
                     % args.one_series_duration)
    if not 5000 <= args.interval_us <= 200000:
        parser.error("--interval-us out of range (contract 5000..200000): %d"
                     % args.interval_us)
    if not 1 <= args.series <= 16:
        parser.error("--series out of range (contract 1..16): %d" % args.series)
    if not 0 < args.fps_min <= 200:
        parser.error("--fps-min out of range: %g" % args.fps_min)
    if not 0 <= args.max_invalid_ratio <= 1:
        parser.error("--max-invalid-ratio out of range: %g"
                     % args.max_invalid_ratio)
    if not 0.01 <= args.poll_interval <= 5:
        parser.error("--poll-interval out of range: %g" % args.poll_interval)
    if args.heap_min_free < 0 or args.heap_min_largest < 0 \
            or args.heap_leak_tolerance < 0:
        parser.error("heap thresholds must be >= 0")

    # ---- extended (opt-in) mode validation ----
    if args.flash_allow and not args.flash_image:
        parser.error("--flash-allow requires --flash-image")
    if args.flash_image and not args.flash_allow:
        parser.error("--flash-image requires --flash-allow (second safety switch)")
    if args.ota_allow and not args.ota_image:
        parser.error("--ota-allow requires --ota-image")
    if args.ota_image and not args.ota_allow:
        parser.error("--ota-image requires --ota-allow (second safety switch)")
    if args.ota_interrupt and not (args.ota_image or args.ota_restore_image):
        parser.error("--ota-interrupt needs --ota-image or --ota-restore-image "
                     "as the payload source")
    if not (0 <= args.soak_seconds <= 3600):
        parser.error("--soak-seconds out of range (0 or 30..3600): %d"
                     % args.soak_seconds)
    if 0 < args.soak_seconds < 30:
        parser.error("--soak-seconds must be 0 (off) or >= 30 s")
    if not 1 <= args.usbip_sessions <= 5:
        parser.error("--usbip-sessions out of range (1..5): %d"
                     % args.usbip_sessions)
    if not 0 <= args.usbip_port_index <= 31:
        parser.error("--usbip-port-index out of range (0..31): %d"
                     % args.usbip_port_index)
    for value, name, low, high in (
            (args.openocd_timeout, "--openocd-timeout", 10.0, 600.0),
            (args.flash_timeout, "--flash-timeout", 10.0, 600.0),
            (args.ota_timeout, "--ota-timeout", 10.0, 600.0),
            (args.reboot_timeout, "--reboot-timeout", 10.0, 300.0),
            (args.ota_settle, "--ota-settle", 0.0, 60.0),
            (args.soak_interval, "--soak-interval", 0.5, 60.0),
            (args.soak_poll, "--soak-poll", 0.05, 5.0)):
        if not low <= value <= high:
            parser.error("%s out of range (%g..%g): %g"
                         % (name, low, high, value))
    if not 1 <= args.soak_series <= 4:
        parser.error("--soak-series out of range (1..4): %d"
                     % args.soak_series)
    if not 5000 <= args.soak_interval_us <= 200000:
        parser.error("--soak-interval-us out of range (5000..200000): %d"
                     % args.soak_interval_us)
    if not 1 <= args.flash_expect_flash_kib <= 65536:
        if args.flash_expect_flash_kib != 0:
            parser.error("--flash-expect-flash-kib out of range: %d"
                         % args.flash_expect_flash_kib)
    if args.usbip_host and ("://" in args.usbip_host
                            or "/" in args.usbip_host):
        parser.error("--usbip-host must be a bare IP/hostname: %r"
                     % args.usbip_host)


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)
    if args.dry_run:
        return run_dry_run(args)
    report = run_gate(args)
    if args.report:
        _write_report(report, args.report)
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        _human_report(report)
    return int(report["exit_code"])


if __name__ == "__main__":
    sys.exit(main())
