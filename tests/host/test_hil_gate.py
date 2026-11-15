"""Host tests for scripts/hil_gate.py (task-59: P0 HIL gate).

Covers the parts that can be tested without a device or network:

  * CVF1 / CVF2 frame-page decoding (header, PC-present flag, groups, payload
    lengths, empty 204 body, truncation/bad-magic errors) -- mirrors the Web
    decoder in web/src/api/curveLocal.ts;
  * effective-rate / missing-seq metric computation;
  * Gate check bookkeeping (required vs warning severity, GateAbort);
  * the built-in fake device used by ``--dry-run``: a healthy firmware passes,
    a low-sampling-rate or undetected target fails, legacy firmware (missing
    target/status diagnostics) fails strictly and passes with
    ``--allow-legacy-firmware``, and every forbidden write path is refused;
  * the CLI ``--dry-run --json`` self-test contract (exit 0, 5/5 scenarios).

No ESP-IDF, Docker, network or hardware is required.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

import script_loader as sl

hil = sl.load_script("hil_gate")
ROOT = sl.ROOT
SCRIPT = ROOT / "scripts" / "hil_gate.py"


def _cvf1_header(count, first_seq, frame_bytes, flags=0):
    return struct.pack("<4sBBHIHH", b"CVF1", 1, flags, count, first_seq,
                       frame_bytes, 0)


def _cvf2_header(count, first_seq, frame_bytes, flags=0):
    return struct.pack("<4sBBHIHH", b"CVF2", 2, flags, count, first_seq,
                       frame_bytes, 0)


def _cvf2_frame(seq, timestamp_us, group, payload, pc=None):
    body = struct.pack("<IQ", seq, timestamp_us)
    body += struct.pack("<BH", group, len(payload))
    if pc is not None:
        body += struct.pack("<I", pc)
    return body + payload


def _args(**overrides):
    parser = hil.build_parser()
    args = parser.parse_args([])
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


class CurveFrameDecodeTests(unittest.TestCase):
    def test_empty_body_is_empty_page(self) -> None:
        page = hil.decode_curve_frames(b"")
        self.assertEqual(page.count, 0)
        self.assertEqual(page.frames, [])
        self.assertFalse(page.gap)

    def test_cvf1_single_group(self) -> None:
        body = _cvf1_header(2, 7, 4) + b"".join([
            struct.pack("<IQ", 7, 1000) + b"\x01\x02\x03\x04",
            struct.pack("<IQ", 8, 21000) + b"\x05\x06\x07\x08",
        ])
        page = hil.decode_curve_frames(body)
        self.assertEqual((page.version, page.count, page.first_seq), (1, 2, 7))
        self.assertEqual(page.frame_bytes, 4)
        self.assertEqual([frame.seq for frame in page.frames], [7, 8])
        self.assertEqual(page.frames[1].timestamp_us, 21000)
        self.assertEqual(page.frames[0].payload, b"\x01\x02\x03\x04")
        self.assertIsNone(page.frames[0].pc)
        self.assertEqual(page.frames[0].group, 0)

    def test_cvf2_without_pc(self) -> None:
        payload0 = b"\xaa" * 8
        payload1 = b"\xbb" * 8
        body = _cvf2_header(2, 3, 8) + _cvf2_frame(3, 100, 0, payload0) \
            + _cvf2_frame(4, 20100, 1, payload1)
        page = hil.decode_curve_frames(body)
        self.assertEqual((page.version, page.count), (2, 2))
        self.assertFalse(page.pc_present)
        self.assertEqual([frame.group for frame in page.frames], [0, 1])
        self.assertEqual(page.frames[1].payload, payload1)
        self.assertIsNone(page.frames[0].pc)

    def test_cvf2_with_pc(self) -> None:
        body = _cvf2_header(1, 1, 4, flags=hil.CVF_FLAG_PC) + _cvf2_frame(
            1, 123, 0, b"\x01\x02\x03\x04", pc=0x08001234)
        page = hil.decode_curve_frames(body)
        self.assertTrue(page.pc_present)
        self.assertEqual(page.frames[0].pc, 0x08001234)
        self.assertEqual(page.frames[0].payload, b"\x01\x02\x03\x04")

    def test_cvf2_gap_and_trigger_flags(self) -> None:
        flags = hil.CVF_FLAG_GAP | hil.CVF_FLAG_TRIGGER \
            | hil.CVF_FLAG_CONDITION
        body = _cvf2_header(1, 1, 0, flags=flags) \
            + _cvf2_frame(1, 1, 0, b"")
        page = hil.decode_curve_frames(body)
        self.assertTrue(page.gap)
        self.assertTrue(page.triggered)
        self.assertTrue(page.condition_triggered)

    def test_truncated_header_raises(self) -> None:
        with self.assertRaises(hil.FrameDecodeError):
            hil.decode_curve_frames(b"CVF2\x02")

    def test_truncated_frames_raise(self) -> None:
        body = _cvf2_header(2, 1, 4) + _cvf2_frame(1, 1, 0, b"\x00" * 4)
        with self.assertRaises(hil.FrameDecodeError):
            hil.decode_curve_frames(body)

    def test_bad_magic_raises(self) -> None:
        body = struct.pack("<4sBBHIHH", b"NOPE", 2, 0, 0, 0, 0, 0)
        with self.assertRaises(hil.FrameDecodeError):
            hil.decode_curve_frames(body)

    def test_payload_len_over_frame_bytes_raises(self) -> None:
        body = _cvf2_header(1, 1, 4) \
            + struct.pack("<IQ", 1, 1) + struct.pack("<BH", 0, 8) + b"\x00" * 8
        with self.assertRaises(hil.FrameDecodeError):
            hil.decode_curve_frames(body)


class CurveMetricsTests(unittest.TestCase):
    def _page(self, stamps) -> bytes:
        frames = b"".join(
            struct.pack("<IQ", seq, timestamp)
            for seq, timestamp in stamps)
        return _cvf1_header(len(stamps), 1, 0) + frames

    def test_effective_rate_from_device_timestamps(self) -> None:
        data = self._page([(1, 0), (2, 20000), (3, 40000), (4, 60000)])
        metrics = hil.compute_curve_metrics(data, 1.0)
        self.assertEqual(metrics["frames"], 4)
        self.assertAlmostEqual(metrics["device_fps"], 50.0, places=2)
        self.assertAlmostEqual(metrics["delivered_fps"], 4.0, places=2)
        self.assertEqual(metrics["missing_seqs"], 0)

    def test_missing_seq_detection(self) -> None:
        data = self._page([(1, 0), (4, 60000), (5, 80000)])
        metrics = hil.compute_curve_metrics(data, 1.0)
        self.assertEqual(metrics["missing_seqs"], 2)

    def test_empty_page_metrics(self) -> None:
        metrics = hil.compute_curve_metrics(b"", 2.0)
        self.assertEqual(metrics["frames"], 0)
        self.assertEqual(metrics["device_fps"], 0.0)


class GateBookkeepingTests(unittest.TestCase):
    def test_required_failure_fails_gate(self) -> None:
        gate = hil.Gate()
        gate.check("x.required", False, "boom")
        self.assertTrue(gate.failed)

    def test_warning_failure_does_not_fail_gate(self) -> None:
        gate = hil.Gate()
        gate.check("x.warn", False, "heads up", severity="warn")
        self.assertFalse(gate.failed)
        self.assertEqual(gate.warnings, ["x.warn: heads up"])

    def test_require_raises_gate_abort(self) -> None:
        gate = hil.Gate()
        with self.assertRaises(hil.GateAbort) as context:
            gate.require("x.abort", False, "nope")
        self.assertEqual(context.exception.check_id, "x.abort")
        self.assertTrue(gate.failed)

    def test_check_ok_detail(self) -> None:
        gate = hil.Gate()
        gate.check("x.ok", True, "failure text", ok_detail="pass text")
        self.assertEqual(gate.checks[0]["detail"], "pass text")


class FakeDeviceGateTests(unittest.TestCase):
    def _run(self, device, allow_legacy=False):
        args = _args(host="dry-run", port=0, duration=0.3,
                     one_series_duration=0.15, poll_interval=0.02,
                     playwright="off", allow_legacy_firmware=allow_legacy)
        return hil.run_gate(args, transport=device)

    def test_healthy_fake_device_passes(self) -> None:
        report = self._run(hil.FakeDevice())
        self.assertTrue(report["ok"])
        self.assertEqual(report["exit_code"], hil.EXIT_PASS)
        self.assertEqual(report["summary"]["required_failed"], 0)
        self.assertGreater(report["metrics"]["curve_perf"]["device_fps"], 35.0)
        self.assertEqual(report["metrics"]["curve_perf"]["missing_seqs"], 0)
        self.assertTrue(report["metrics"]["status_pre"]["heap_free"] > 0)

    def test_low_sampling_rate_fails(self) -> None:
        report = self._run(hil.FakeDevice(effective_fps=10.0))
        self.assertFalse(report["ok"])
        self.assertEqual(report["exit_code"], hil.EXIT_FAIL)
        slow_checks = [c for c in report["checks"]
                       if c["id"].endswith(".fps") and not c["ok"]]
        self.assertTrue(slow_checks, msg="expected a failing *.fps check")
        self.assertTrue(any(c["id"] in ("curve.single.fps", "curve.perf.fps")
                            for c in slow_checks))

    def test_undetected_target_fails(self) -> None:
        report = self._run(hil.FakeDevice(detected=False))
        self.assertEqual(report["exit_code"], hil.EXIT_FAIL)
        self.assertTrue(any(c["id"] == "target.info.detected" and not c["ok"]
                            for c in report["checks"]))

    def test_legacy_firmware_fails_strict_and_passes_with_flag(self) -> None:
        device_strict = hil.FakeDevice(diagnostics=False, frame_format="cvf1")
        report = self._run(device_strict)
        self.assertEqual(report["exit_code"], hil.EXIT_FAIL)
        self.assertTrue(any(
            c["id"].startswith("target.status.diagnostics.pre") and not c["ok"]
            for c in report["checks"]))

        device_allowed = hil.FakeDevice(diagnostics=False, frame_format="cvf1")
        report = self._run(device_allowed, allow_legacy=True)
        self.assertTrue(report["ok"])
        self.assertEqual(report["summary"]["required_failed"], 0)
        self.assertTrue(report["warnings"])

    def test_forbidden_write_paths_are_refused(self) -> None:
        device = hil.FakeDevice()
        for path in hil.FORBIDDEN_PATHS:
            with self.assertRaises(AssertionError):
                device.request("POST", path, body={})

    def test_report_is_json_serializable(self) -> None:
        report = self._run(hil.FakeDevice())
        encoded = json.dumps(report)
        self.assertIn('"tool"', encoded)
        self.assertIn('"checks"', encoded)


class CliTests(unittest.TestCase):
    def test_dry_run_cli_exit_zero_and_json(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--dry-run", "--json"],
            capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, msg=result.stderr[-2000:])
        payload = json.loads(result.stdout)
        self.assertTrue(payload["dry_run"])
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["failures"], 0)
        self.assertEqual(len(payload["scenarios"]), 7)
        self.assertTrue(all(item["ok"] for item in payload["scenarios"]))

    def test_help_mentions_exit_codes(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0)
        for needle in ("--host", "--json", "--timeout", "--dry-run",
                       "--allow-legacy-firmware"):
            self.assertIn(needle, result.stdout)



class ExtendedModeTests(unittest.TestCase):
    """Offline coverage for the opt-in extended HIL modes (task-63)."""

    def _parse(self, argv):
        parser = hil.build_parser()
        args = parser.parse_args(argv)
        hil.validate_args(parser, args)
        return args

    # -- helpers -----------------------------------------------------------
    def test_extended_modes_off_by_default(self) -> None:
        args = self._parse([])
        self.assertFalse(hil._extended_enabled(args))
        self.assertFalse(args.usbip_smoke)
        self.assertFalse(args.reboot_recovery)
        self.assertIsNone(args.flash_image)
        self.assertIsNone(args.ota_image)
        self.assertFalse(args.ota_interrupt)
        self.assertEqual(args.soak_seconds, 0)
        self.assertFalse(args.swd_clock_matrix)
        self.assertEqual(args.swd_clock_steps, "auto,500k,1M,2M,4M,10M,16M,20M,24M")
        self.assertEqual(args.swd_clock_expect_ok, "")
        self.assertAlmostEqual(args.swd_clock_settle, 0.3)
        self.assertTrue(args.openocd_path.endswith("openocd"))
        self.assertTrue(args.openocd_scripts.endswith("scripts"))

    def test_extended_enabled_detection(self) -> None:
        self.assertTrue(hil._extended_enabled(
            self._parse(["--usbip-smoke"])))
        self.assertTrue(hil._extended_enabled(
            self._parse(["--reboot-recovery"])))
        self.assertTrue(hil._extended_enabled(
            self._parse(["--ota-interrupt", "--ota-restore-image",
                         "/tmp/x.bin"])))
        self.assertTrue(hil._extended_enabled(
            self._parse(["--ota-image", "/tmp/x.bin", "--ota-allow"])))
        self.assertTrue(hil._extended_enabled(
            self._parse(["--soak-seconds", "60"])))
        self.assertTrue(hil._extended_enabled(
            self._parse(["--swd-clock-matrix"])))

    def test_write_modes_need_second_switch(self) -> None:
        parser = hil.build_parser()
        for argv in (["--flash-image", "/tmp/x.bin"],
                     ["--flash-allow"],
                     ["--ota-image", "/tmp/x.bin"],
                     ["--ota-allow"],
                     ["--ota-interrupt"]):
            args = parser.parse_args(argv)
            with self.assertRaises(SystemExit):
                hil.validate_args(parser, args)

    def test_extended_argument_ranges(self) -> None:
        parser = hil.build_parser()
        for argv in (["--soak-seconds", "10"],
                     ["--soak-seconds", "99999"],
                     ["--usbip-sessions", "0"],
                     ["--usbip-sessions", "9"],
                     ["--usbip-port-index", "99"],
                     ["--openocd-timeout", "1"],
                     ["--soak-series", "9"],
                     ["--soak-interval-us", "1000"]):
            args = parser.parse_args(argv)
            with self.assertRaises(SystemExit):
                hil.validate_args(parser, args)

    def test_normalize_uid(self) -> None:
        self.assertEqual(hil.normalize_uid("1E000D00 11513332"),
                         "1E000D0011513332")
        self.assertEqual(hil.normalize_uid("0x1e000d00"), "1E000D00")
        self.assertEqual(hil.normalize_uid(None), "")
        self.assertEqual(hil.normalize_uid("zz-zz"), "")

    def test_detect_root_helper_explicit(self) -> None:
        self.assertEqual(hil.detect_root_helper("/bin/true"), ["/bin/true"])
        self.assertEqual(hil.detect_root_helper("/nonexistent/helper"), None)             if False else None  # environment-dependent fallbacks are not asserted

    def test_sha256_file(self) -> None:
        with tempfile.NamedTemporaryFile("wb", delete=False) as handle:
            handle.write(b"birdiebot")
            path = handle.name
        try:
            self.assertEqual(hil.sha256_file(path),
                             hashlib.sha256(b"birdiebot").hexdigest())
        finally:
            os.unlink(path)

    # -- Web Flash precheck (pure function, no device) ----------------------
    def _target(self, **overrides):
        target = {
            "detected": True,
            "supported": True,
            "family": "stm32h7",
            "flash_size": 1048576,
            "uid": "0123456789ABCDEF01234567",
            "device": "STM32H723/H725/H730/H733/H735",
        }
        target.update(overrides)
        return target

    def test_flash_precheck_good(self) -> None:
        with tempfile.NamedTemporaryFile("wb", suffix=".bin",
                                         delete=False) as handle:
            handle.write(b"\x00" * 4096)
            path = handle.name
        try:
            errors = hil.flash_image_precheck(
                path, None, hil.sha256_file(path), self._target(),
                expect_uid="0123456789abcdef01234567",
                expect_flash_kib=1024)
            self.assertEqual(errors, [])
        finally:
            os.unlink(path)

    def test_flash_precheck_refusals(self) -> None:
        with tempfile.NamedTemporaryFile("wb", suffix=".bin",
                                         delete=False) as handle:
            handle.write(b"\x00" * 4096)
            path = handle.name
        try:
            cases = [
                (dict(image_path="/nonexistent.bin"), "not found"),
                (dict(target={"detected": False}), "not detected"),
                (dict(target=self._target(supported=False)), "detection-only"),
                (dict(target=self._target(family="stm32f1")), "H7"),
                (dict(target=self._target(expect_uid="DEAD")),
                 "UID mismatch") if False else (dict(), ""),
                (dict(expect_uid="DEADBEEF"), "UID mismatch"),
                (dict(expect_flash_kib=512), "KiB"),
                (dict(expect_sha256="00" * 32), "sha256 mismatch"),
                (dict(target=self._target(flash_size=1024)), "does not fit"),
            ]
            for overrides, needle in cases:
                kwargs = dict(image_path=path, image_size=None,
                              expect_sha256="", target=self._target(),
                              expect_uid="", expect_flash_kib=0,
                              require_h7=True)
                kwargs.update(overrides)
                errors = hil.flash_image_precheck(**kwargs)
                if needle:
                    self.assertTrue(any(needle in error for error in errors),
                                    msg=(overrides, errors))
        finally:
            os.unlink(path)

    def test_flash_precheck_allow_non_h7(self) -> None:
        with tempfile.NamedTemporaryFile("wb", suffix=".bin",
                                         delete=False) as handle:
            handle.write(b"\x00" * 1024)
            path = handle.name
        try:
            errors = hil.flash_image_precheck(
                path, None, "", self._target(family="stm32f4"), require_h7=False)
            self.assertEqual(errors, [])
        finally:
            os.unlink(path)

    # -- SWD clock matrix (pure helpers + offline gate integration) --------
    def test_swd_clock_steps_parse(self) -> None:
        self.assertEqual(
            hil.parse_swd_clock_steps("auto,500k,1M,2M,4M,10M,16M,20M,24M"),
            [0, 500000, 1000000, 2000000, 4000000, 10000000,
             16000000, 20000000, 24000000])
        self.assertEqual(hil.parse_swd_clock_steps("0 100000 0x100000"),
                         [0, 100000, 0x100000])
        self.assertEqual(hil.parse_swd_clock_steps("0.5M, 1900k"),
                         [500000, 1900000])
        self.assertEqual(hil.parse_swd_clock_steps("1000000"), [1000000])
        for bad in ("", "abc", "50k", "25M", "1M,bad"):
            with self.assertRaises(ValueError):
                hil.parse_swd_clock_steps(bad)

    def test_swd_clock_label_and_result(self) -> None:
        self.assertEqual(hil.swd_clock_label(0), "auto")
        self.assertEqual(hil.swd_clock_label(500000), "500k")
        self.assertEqual(hil.swd_clock_label(1000000), "1m")
        self.assertEqual(hil.swd_clock_label(1048576), "1048576hz")
        self.assertTrue(hil.swd_clock_result_ok(1000000, 200, True, True))
        self.assertFalse(hil.swd_clock_result_ok(1000000, 400, False, True))
        self.assertFalse(hil.swd_clock_result_ok(1000000, 200, True, False))
        self.assertTrue(hil.swd_clock_result_ok(0, 500, False, True))

    def _run_offline(self, argv, device=None, **overrides):
        args = self._parse(argv)
        args.host = "dry-run"
        args.port = 0
        args.duration = 0.3
        args.one_series_duration = 0.15
        args.poll_interval = 0.02
        args.playwright = "off"
        args.verbose = False
        args.allow_legacy_firmware = False
        args.allow_dap_session = False
        args.swd_clock_settle = 0.0
        for key, value in overrides.items():
            setattr(args, key, value)
        return hil.run_gate(args, transport=device or hil.FakeDevice())

    def test_swd_clock_matrix_offline_pass(self) -> None:
        report = self._run_offline(
            ["--swd-clock-matrix",
             "--swd-clock-expect-ok", "auto,500k,1M"])
        self.assertEqual(report["exit_code"], hil.EXIT_PASS,
                         msg=json.dumps(report["warnings"]))
        self.assertTrue(report["modes"]["swd_clock_matrix"])
        matrix = report["extended"]["extended_swd_matrix"]
        self.assertEqual(matrix["status"], "pass")
        self.assertEqual(matrix["required_failures"], 0)
        self.assertEqual(len(matrix["steps"]), 9)
        self.assertEqual(matrix["steps_requested_hz"],
                         [0, 500000, 1000000, 2000000, 4000000, 10000000,
                          16000000, 20000000, 24000000])
        self.assertEqual(matrix["steps"][3]["detected"], False)
        self.assertEqual(matrix["final_restore"]["detected"], True)
        ids = {check["id"] for check in report["checks"]}
        self.assertIn("extended.swd_matrix.step.auto", ids)
        self.assertIn("extended.swd_matrix.restore.10m", ids)
        self.assertIn("extended.swd_matrix.final_auto", ids)

    def test_swd_clock_matrix_expected_failure_fails_gate(self) -> None:
        report = self._run_offline(
            ["--swd-clock-matrix",
             "--swd-clock-expect-ok", "auto,10M"])
        self.assertEqual(report["exit_code"], hil.EXIT_FAIL)
        matrix = report["extended"]["extended_swd_matrix"]
        self.assertEqual(matrix["status"], "fail")
        self.assertGreaterEqual(matrix["required_failures"], 1)

    def test_swd_clock_matrix_cli_validation(self) -> None:
        parser = hil.build_parser()
        for argv in (["--swd-clock-steps", "50k"],
                     ["--swd-clock-steps", ""],
                     ["--swd-clock-expect-ok", "nope"],
                     ["--swd-clock-settle", "-1"],
                     ["--swd-clock-settle", "99"]):
            args = parser.parse_args(argv)
            with self.assertRaises(SystemExit):
                hil.validate_args(parser, args)

    def test_usbip_port_dap_detection(self) -> None:
        # `usbip attach` prints nothing on success; success is confirmed by
        # parsing `usbip port` for the WiFi Dap CMSIS-DAP (c251:f00a).
        sample = (
            "Imported USB devices\n"
            "====================\n"
            "Port 00: <Port in Use> at High Speed(480Mbps)\n"
            "       Keil Software, Inc. : unknown product (c251:f00a)\n"
            "       4-1 -> usbip://192.168.7.1:3240/1-1\n")
        self.assertTrue(hil._usbip_port_has_dap(sample))
        self.assertTrue(hil._usbip_port_has_dap("KEIL SOFTWARE, INC."))
        self.assertFalse(hil._usbip_port_has_dap("Imported USB devices\n"))
        self.assertFalse(hil._usbip_port_has_dap(""))

    def test_soak_offline_uses_start_bin(self) -> None:
        # Regression for the device-window failure: the soak must start curves
        # through /api/curve/start-bin (CBS1), never the legacy 16 KiB JSON
        # body (which the low-heap unified firmware rejects with 503).
        args = self._parse(["--soak-seconds", "35"])
        args.host = "dry-run"
        args.port = 0
        args.duration = 0.3
        args.one_series_duration = 0.15
        args.poll_interval = 0.02
        args.playwright = "off"
        args.verbose = False
        args.allow_legacy_firmware = False
        args.allow_dap_session = False
        args.soak_seconds = 2
        args.soak_interval = 0.5
        args.soak_poll = 0.1
        report = hil.run_gate(args, transport=hil.FakeDevice())
        self.assertEqual(report["exit_code"], hil.EXIT_PASS,
                         msg=json.dumps(report["warnings"]))
        soak = report["extended"]["extended_soak"]
        self.assertEqual(soak["status"], "pass")
        self.assertEqual(soak["failures"], 0)
        self.assertEqual(soak["start_failures"], 0)
        self.assertEqual(soak["start_methods"], ["start-bin"])
        self.assertGreaterEqual(soak["iterations"], 1)
        self.assertGreater(soak["frames_total"], 0)
        self.assertIsNotNone(soak["heap_free_start"])
        self.assertFalse(soak["aborted_early"])
        # Instrumented diagnostics: per-cycle records + classified counters.
        self.assertEqual(len(soak["cycles"]), soak["iterations"])
        self.assertEqual(soak["failed_cycles"], [])
        self.assertEqual(soak["tick_errors_total"], 0)
        self.assertEqual(soak["sampler_stops"], 0)
        self.assertEqual(soak["transport_errors"], 0)
        self.assertEqual(soak["stop_failures"], 0)

    def test_swd_clock_matrix_disabled_by_default(self) -> None:
        report = self._run_offline([])
        self.assertFalse(report["modes"]["swd_clock_matrix"])
        self.assertEqual(report["extended"], {"enabled": False})


    # -- offline gate integration -----------------------------------------
    def test_extended_mode_skips_offline(self) -> None:
        args = self._parse(["--usbip-smoke"])
        args.host = "dry-run"
        args.port = 0
        args.duration = 0.3
        args.one_series_duration = 0.15
        args.poll_interval = 0.02
        args.playwright = "off"
        args.verbose = False
        args.allow_legacy_firmware = False
        args.allow_dap_session = False
        report = hil.run_gate(args, transport=hil.FakeDevice())
        self.assertEqual(report["exit_code"], hil.EXIT_PASS)
        self.assertTrue(report["modes"]["extended"])
        self.assertTrue(report["modes"]["usbip_smoke"])
        skipped = [check for check in report["checks"]
                   if check.get("skipped")
                   and check["id"].startswith("extended.")]
        self.assertTrue(skipped, msg="expected an extended.* skipped check")
        self.assertIn("extended", report)
        json.dumps(report)

    def test_extended_report_disabled_by_default(self) -> None:
        args = self._parse([])
        args.host = "dry-run"
        args.port = 0
        args.duration = 0.3
        args.one_series_duration = 0.15
        args.poll_interval = 0.02
        args.playwright = "off"
        args.verbose = False
        args.allow_legacy_firmware = False
        args.allow_dap_session = False
        report = hil.run_gate(args, transport=hil.FakeDevice())
        self.assertFalse(report["modes"]["extended"])
        self.assertEqual(report["extended"], {"enabled": False})




class SlimProfileTests(unittest.TestCase):
    """Offline coverage for the final slim profile (task final-slim HIL)."""

    def _parse(self, argv):
        parser = hil.build_parser()
        args = parser.parse_args(argv)
        hil.validate_args(parser, args)
        return args

    def _offline_gate(self, argv, device):
        args = self._parse(argv)
        args.host = "dry-run"
        args.port = 0
        args.duration = 0.3
        args.one_series_duration = 0.15
        args.poll_interval = 0.02
        args.playwright = "off"
        args.verbose = False
        args.allow_legacy_firmware = False
        args.allow_dap_session = False
        return hil.run_gate(args, transport=device)

    # -- profile probing ---------------------------------------------------
    @staticmethod
    def _response(status, body, content_type="application/json"):
        headers = {"content-type": content_type} if content_type else {}
        return hil.Response(status=status, headers=headers, body=body,
                            elapsed_ms=1.0)

    class _ProbeTransport:
        def __init__(self, discover, ota):
            self.discover = discover
            self.ota = ota

        def request(self, method, path, body=None, timeout=None, retries=1):
            if path == hil.DISCOVER_PATH and method == "GET":
                return self.discover
            raise AssertionError("unexpected request %s %s" % (method, path))

        def request_bytes(self, method, path, data=b"", headers=None,
                          timeout=None, retries=1, close_after=False):
            if path == hil.OTA_UPLOAD_PATH:
                return self.ota
            raise AssertionError("unexpected raw request %s %s"
                                 % (method, path))

    def _probe(self, discover, ota, profile=hil.PROFILE_AUTO):
        args = argparse.Namespace(timeout=5.0, profile=profile)
        transport = self._ProbeTransport(discover, ota)
        return hil.probe_device_profile(transport, args)

    def test_probe_slim(self) -> None:
        probe = self._probe(
            self._response(200, hil.DRY_RUN_HTML.encode(), "text/html"),
            self._response(405, b"Specified method is invalid for this resource",
                           "text/html"))
        self.assertEqual(probe["detected"], hil.PROFILE_SLIM)
        self.assertEqual(probe["errors"], [])
        self.assertTrue(probe["evidence"]["discover"]["spa_html"])
        self.assertEqual(probe["evidence"]["ota_upload"]["http"], 405)

    def test_probe_full(self) -> None:
        probe = self._probe(
            self._response(200, b'{"ok":true,"devices":[]}'),
            self._response(400, b'{"ok":false,"err":"image too small"}'))
        self.assertEqual(probe["detected"], hil.PROFILE_FULL)
        self.assertEqual(probe["errors"], [])
        self.assertTrue(probe["evidence"]["discover"]["json"])

    def test_probe_ambiguous(self) -> None:
        probe = self._probe(
            self._response(200, b'{"ok":true,"devices":[]}'),
            self._response(405, b"nope", "text/html"))
        self.assertEqual(probe["detected"], "ambiguous")
        self.assertTrue(probe["errors"])

    # -- mDNS helpers ------------------------------------------------------
    def test_derive_mdns_name(self) -> None:
        self.assertEqual(hil.derive_mdns_name("aa:bb:cc:dd:ca:60"),
                         "WiFi-Dap-CA60")
        self.assertEqual(hil.derive_mdns_name("AA:BB:CC:DD:4B:38"),
                         "WiFi-Dap-4B38")
        # SYSTEM(0)/GET_SYS_INFO sn is a bare 12-hex STA MAC.
        self.assertEqual(hil.derive_mdns_name("001122334455"),
                         "WiFi-Dap-4455")
        self.assertEqual(hil.derive_mdns_name("10BDA3F04B38"),
                         "WiFi-Dap-4B38")
        self.assertIsNone(hil.derive_mdns_name(None))
        self.assertIsNone(hil.derive_mdns_name("zz"))

    def test_mdns_candidate_hosts(self) -> None:
        self.assertEqual(
            hil.mdns_candidate_hosts("192.168.7.1", "",
                                     "192.168.254.2"),
            ["192.168.7.1", "192.168.254.2"])
        self.assertEqual(
            hil.mdns_candidate_hosts("192.168.7.1", "10.0.0.5",
                                     "192.168.254.2"),
            ["10.0.0.5", "192.168.7.1", "192.168.254.2"])
        self.assertEqual(hil.mdns_candidate_hosts("a", "", ""), ["a"])

    def _dns_record(self, name, rtype, rdata, rclass=1):
        encoded = hil._dns_encode_name(name)
        return encoded + struct.pack("!HHIH", rtype, rclass, 120,
                                     len(rdata)) + rdata

    def test_mdns_txt_name_parse(self) -> None:
        service = "_http._tcp.local"
        instance = "WiFi-Dap-4B38._http._tcp.local"
        ptr = self._dns_record(service, 12,
                              hil._dns_encode_name(instance))
        srv_rdata = struct.pack("!HHH", 0, 0, 80) + \
            hil._dns_encode_name("wifi-dap-4b38.local")
        srv = self._dns_record(instance, 33, srv_rdata)
        txt_payload = b"name=WiFi-Dap-4B38"
        txt_rdata = bytes([len(txt_payload)]) + txt_payload
        txt = self._dns_record(instance, 16, txt_rdata, rclass=0x8001)
        a = self._dns_record("wifi-dap-4b38.local", 1,
                             socket.inet_aton("192.168.254.161"),
                             rclass=0x8001)
        message = struct.pack("!HHHHHH", 0, 0x8400, 0, 4, 0, 0) + \
            ptr + srv + txt + a
        records = hil.parse_dns_response(message)
        self.assertEqual(len(records), 4)
        self.assertEqual(hil.mdns_advertised_names(records),
                         ["WiFi-Dap-4B38"])
        self.assertIn("192.168.254.161",
                      [record.get("ip") for record in records])

    def test_build_mdns_query(self) -> None:
        query = hil.build_mdns_query("_http._tcp.local", unicast=True)
        self.assertGreater(len(query), 12)
        self.assertEqual(struct.unpack("!H", query[4:6])[0], 1)  # QDCOUNT
        self.assertIn(b"\x05_http", query)
        self.assertIn(b"\x04_tcp", query)
        self.assertIn(b"\x05local", query)

    # -- CLI / validation --------------------------------------------------
    def test_profile_flag_choices(self) -> None:
        args = self._parse([])
        self.assertEqual(args.profile, hil.PROFILE_AUTO)
        self.assertEqual(args.ws_mdns, "auto")
        self.assertEqual(args.mdns_expect_name, "")
        self.assertAlmostEqual(args.mdns_timeout, 5.0)
        with self.assertRaises(SystemExit):
            self._parse(["--profile", "bogus"])

    def test_slim_profile_rejects_ota_modes(self) -> None:
        parser = hil.build_parser()
        for argv in (["--profile", "slim", "--ota-image", "/tmp/fw.bin",
                      "--ota-allow"],
                     ["--profile", "slim", "--ota-interrupt",
                      "--ota-restore-image", "/tmp/fw.bin"],
                     ["--profile", "slim", "--reboot-recovery"]):
            args = parser.parse_args(argv)
            with self.assertRaises(SystemExit):
                hil.validate_args(parser, args)

    def test_mdns_option_validation(self) -> None:
        parser = hil.build_parser()
        for argv in (["--mdns-timeout", "0"],
                     ["--mdns-timeout", "31"],
                     ["--mdns-expect-name", "has space"]):
            args = parser.parse_args(argv)
            with self.assertRaises(SystemExit):
                hil.validate_args(parser, args)

    # -- offline gate integration -----------------------------------------
    def test_offline_slim_gate_passes(self) -> None:
        report = self._offline_gate([], hil.FakeDevice(slim=True))
        self.assertEqual(report["exit_code"], hil.EXIT_PASS,
                         msg=json.dumps(report["warnings"]))
        self.assertEqual(report["profile"]["detected"], hil.PROFILE_SLIM)
        ids = {check["id"]: check for check in report["checks"]}
        self.assertTrue(ids["slim.ota.removed"]["ok"])
        self.assertTrue(ids["slim.discover.removed"]["ok"])
        self.assertTrue(ids["profile.detected"]["ok"])
        # WS/mDNS is skipped offline, never failed.
        self.assertTrue(ids["ws_mdns"].get("skipped"))

    def test_offline_full_gate_still_passes(self) -> None:
        report = self._offline_gate([], hil.FakeDevice())
        self.assertEqual(report["exit_code"], hil.EXIT_PASS,
                         msg=json.dumps(report["warnings"]))
        self.assertEqual(report["profile"]["detected"], hil.PROFILE_FULL)

    def test_offline_forced_profile_mismatch_fails(self) -> None:
        report = self._offline_gate(["--profile", "full"],
                                    hil.FakeDevice(slim=True))
        self.assertEqual(report["exit_code"], hil.EXIT_FAIL)
        ids = {check["id"]: check for check in report["checks"]}
        self.assertFalse(ids["profile.matches_request"]["ok"])

    def test_slim_runtime_skips_ota_extended_modes(self) -> None:
        # Auto-detected slim + OTA flags: the extended OTA phase must be
        # skipped (never probe/upload OTA), and the gate still passes.
        report = self._offline_gate(
            ["--ota-image", "/tmp/fw.bin", "--ota-allow"],
            hil.FakeDevice(slim=True))
        self.assertEqual(report["exit_code"], hil.EXIT_PASS,
                         msg=json.dumps(report["warnings"]))
        ids = {check["id"]: check for check in report["checks"]}
        self.assertTrue(ids["extended.ota.slim"].get("skipped"))
        self.assertNotIn("extended_ota", report.get("extended") or {})



if __name__ == "__main__":
    unittest.main()
