"""Host tests for scripts/hil_extended.py (task-65: opt-in HIL extension).

Covers everything that must be verifiable without a device or network:

  * the offline ``--dry-run`` self-test suite (10 fake-device scenarios with
    expected exit codes and side-effect assertions: no write without the second
    safety switch, no write with a failed pre-check, no blind OTA retry, Flash
    failure triggers a target reset, unavailable USBIP is SKIPped not failed);
  * ``check_image_requirements()`` pure validation (H7 family, size vs. Flash
    capacity, UID/SHA-256 confirmation, OTA minimum size);
  * helper functions (sha256_file, normalize_uid, is_sha256_hex);
  * CLI contract: default invocation is the offline dry-run, ``--yes-flash``
    without ``--flash-image`` is a usage error, dry-run writes never upload;
  * JSON report shape / ``--report`` file, exit-code mapping (0 pass / 1 fail /
    3 device unreachable);
  * OpenOCD command safety (identify-only session; no Flash/erase command).

The default read-only gate (``scripts/hil_gate.py``) is not modified by this
workstream and its own tests live in ``test_hil_gate.py``.

No ESP-IDF, Docker, network or hardware is required.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

import script_loader as sl

ext = sl.load_script("hil_extended")

TMPDIR = tempfile.mkdtemp(prefix="hil-extended-tests.")


def _image(name: str, size: int, seed: int = 0) -> str:
    return ext._selftest_image(TMPDIR, name, size, seed)


FLASH_IMAGE = _image("h7_test.bin", 4096, seed=1)
FLASH_SHA = ext.sha256_file(FLASH_IMAGE)
OTA_IMAGE = ext._selftest_image(TMPDIR, "ota_test.bin", 128 * 1024,
                                seed=2, first_byte=0xE9)


def _base_args(**overrides):
    """Parser defaults + test-safe offline overrides."""
    args = ext.build_parser().parse_args([])
    values = dict(host="dry-run", port=0, timeout=2.0, dry_run=True,
                  verbose=False, report=None, json=False)
    values.update(overrides)
    for key, value in values.items():
        setattr(args, key, value)
    return args


def _run(args, harness=None):
    return ext.run_pipeline(args, harness or ext.FakeHarness())


def _check_ids(report):
    return {check["id"]: check for check in report["checks"]}


class OfflineSelfTestTests(unittest.TestCase):
    """The built-in --dry-run suite must validate all safety scenarios."""

    def test_selftest_suite_passes(self) -> None:
        args = _base_args()
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ext.run_selftest(args)
        self.assertEqual(code, ext.EXIT_PASS, stderr.getvalue())
        self.assertIn("dry-run self-test scenarios: 10/10 as expected",
                      stdout.getvalue() + stderr.getvalue())

    def test_selftest_json_contract(self) -> None:
        args = _base_args(json=True)
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(io.StringIO()):
            code = ext.run_selftest(args)
        self.assertEqual(code, ext.EXIT_PASS)
        payload = json.loads(stdout.getvalue())
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["failures"], 0)
        self.assertEqual(len(payload["scenarios"]), 10)
        names = {item["scenario"] for item in payload["scenarios"]}
        self.assertIn("healthy-all-modes", names)
        self.assertIn("flash-failure-recovery", names)
        self.assertIn("usbip-environment-unavailable", names)
        for item in payload["scenarios"]:
            self.assertTrue(item["ok"], item)


class DefaultInvocationTests(unittest.TestCase):
    def test_no_mode_defaults_to_offline_dry_run(self) -> None:
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ext.main([])
        self.assertEqual(code, ext.EXIT_PASS)
        self.assertIn("dry-run", stderr.getvalue().lower())

    def test_dry_run_mode_uses_fake_device_and_never_writes(self) -> None:
        harness = ext.FakeHarness()
        args = _base_args(flash_image=FLASH_IMAGE, yes_flash=True,
                          flash_expect_sha256=FLASH_SHA)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        self.assertTrue(report["dry_run"])
        self.assertTrue(report["fake_device"])
        self.assertEqual(harness.device.flash_attempts, 1)
        checks = _check_ids(report)
        self.assertTrue(checks["flash.response"]["ok"])
        self.assertTrue(checks["flash.target.detected"]["ok"])
        self.assertEqual(checks["flash.preflight.identity"]["values"]
                         ["image_sha256"], FLASH_SHA)


class SafetyTests(unittest.TestCase):
    def test_flash_without_yes_flash_does_not_write(self) -> None:
        harness = ext.FakeHarness()
        args = _base_args(flash_image=FLASH_IMAGE)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        self.assertEqual(harness.device.flash_attempts, 0)
        checks = _check_ids(report)
        self.assertTrue(checks["flash.write"].get("dry_run"))
        self.assertIn("--yes-flash", checks["flash.write"]["detail"])

    def test_ota_without_yes_ota_does_not_write(self) -> None:
        harness = ext.FakeHarness()
        args = _base_args(ota_image=OTA_IMAGE)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        self.assertEqual(harness.device.ota_attempts, 0)
        self.assertTrue(_check_ids(report)["ota.write"].get("dry_run"))

    def test_yes_flash_without_image_is_usage_error(self) -> None:
        parser = ext.build_parser()
        args = parser.parse_args(["--yes-flash"])
        with self.assertRaises(SystemExit) as raised:
            ext.validate_args(parser, args)
        self.assertEqual(raised.exception.code, ext.EXIT_USAGE)

    def test_yes_ota_without_image_is_usage_error(self) -> None:
        parser = ext.build_parser()
        args = parser.parse_args(["--yes-ota"])
        with self.assertRaises(SystemExit) as raised:
            ext.validate_args(parser, args)
        self.assertEqual(raised.exception.code, ext.EXIT_USAGE)

    def test_duration_implies_soak(self) -> None:
        parser = ext.build_parser()
        args = parser.parse_args(["--duration", "5"])
        self.assertFalse(args.soak)
        ext.validate_args(parser, args)
        self.assertTrue(args.soak)
        self.assertEqual(args.duration, 5.0)

    def test_flash_failure_triggers_reset_and_report(self) -> None:
        device = ext.FakeDevice(flash_status=500)
        harness = ext.FakeHarness(device)
        args = _base_args(flash_image=FLASH_IMAGE, yes_flash=True,
                          flash_timeout=20.0)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertEqual(device.flash_attempts, 1)
        self.assertGreaterEqual(device.reset_calls, 1)
        self.assertTrue(device.detected)
        checks = _check_ids(report)
        self.assertFalse(checks["flash.upload"]["ok"])
        self.assertTrue(checks["flash.recovery.target_detected"]["ok"])

    def test_ota_failure_is_not_retried_and_keeps_old_slot(self) -> None:
        device = ext.FakeDevice(ota_status=503)
        harness = ext.FakeHarness(device)
        args = _base_args(ota_image=OTA_IMAGE, yes_ota=True, ota_settle=0.0,
                          ota_reboot_timeout=5.0)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertEqual(device.ota_attempts, 1)  # never a blind retry
        self.assertEqual(device.running_slot, "ota_0")
        checks = _check_ids(report)
        self.assertFalse(checks["ota.upload"]["ok"])
        self.assertIn("ota.failure", json.dumps(report["sections"]))

    def test_wrong_expected_sha_refuses_without_upload(self) -> None:
        harness = ext.FakeHarness()
        args = _base_args(flash_image=FLASH_IMAGE, yes_flash=True,
                          flash_expect_sha256="0" * 64)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertEqual(harness.device.flash_attempts, 0)
        self.assertFalse(_check_ids(report)["flash.precheck"]["ok"])

    def test_target_not_detected_aborts_every_write(self) -> None:
        device = ext.FakeDevice(detected=False)
        harness = ext.FakeHarness(device)
        args = _base_args(flash_image=FLASH_IMAGE, yes_flash=True,
                          ota_image=OTA_IMAGE, yes_ota=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertEqual(device.flash_attempts, 0)
        self.assertEqual(device.ota_attempts, 0)
        self.assertFalse(_check_ids(report)["target.detected"]["ok"])

    def test_usbip_unavailable_is_skipped_not_failed(self) -> None:
        harness = ext.FakeHarness(root_helper=False)
        args = _base_args(usbip=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        checks = _check_ids(report)
        self.assertTrue(checks["usbip.environment.root_helper"].get("skipped"))
        self.assertGreaterEqual(report["summary"]["skipped"], 1)


class UsbipModeTests(unittest.TestCase):
    def test_usbip_only_skips_dap_probe_before_session(self) -> None:
        """USBIP-only runs must not touch /api/target/info before OpenOCD.

        An HTTP target probe right before the USBIP session triggers the known
        CMSIS-DAP CMD_INFO firmware limitation; the mode verifies the target
        after detach instead.
        """
        device = ext.FakeDevice()
        harness = ext.FakeHarness(device)
        args = _base_args(usbip=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        self.assertTrue(report["modes"]["usbip_skipped_target_probe"])
        checks = _check_ids(report)
        self.assertTrue(checks["target.probe.skipped"]["ok"])
        self.assertNotIn("target.detected", checks)  # only checked after detach
        self.assertTrue(checks["usbip.attach"]["ok"])
        self.assertTrue(checks["usbip.openocd.session"]["ok"])
        self.assertTrue(checks["usbip.target.still_detected"]["ok"])
        self.assertTrue(report["sections"]["usbip"]["target_after"]["detected"])
        # The identify session must never contain a Flash/erase command.
        openocd_calls = [call for call in harness.calls
                         if any("openocd" in part for part in call)]
        self.assertTrue(openocd_calls)
        for call in openocd_calls:
            joined = " ".join(call)
            self.assertNotIn("flash write_image", joined)
            self.assertNotIn("reset halt", joined)

    def test_usbip_known_cmd_info_issue_is_warning_by_default(self) -> None:
        class CmdInfoFailHarness(ext.FakeHarness):
            def run(self, argv, timeout, env=None):
                result = super().run(argv, timeout, env)
                joined = " ".join(argv)
                if "openocd" in joined and "--version" not in joined:
                    result = {
                        "rc": 1, "seconds": 0.1,
                        "output": ("Info : Using CMSIS-DAPv2 interface\n"
                                   "Error: CMSIS-DAP command CMD_INFO failed.\n"),
                    }
                return result

        harness = CmdInfoFailHarness()
        args = _base_args(usbip=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)  # does not block
        checks = _check_ids(report)
        session = checks["usbip.openocd.session"]
        self.assertFalse(session["ok"])
        self.assertEqual(session["severity"], "warn")
        self.assertIn("CMD_INFO", session["detail"])
        self.assertTrue(report["sections"]["usbip"].get("known_issue"))
        self.assertTrue(any("CMD_INFO" in warning
                            for warning in report["warnings"]))

    def test_usbip_known_cmd_info_issue_is_failure_with_strict(self) -> None:
        class CmdInfoFailHarness(ext.FakeHarness):
            def run(self, argv, timeout, env=None):
                result = super().run(argv, timeout, env)
                joined = " ".join(argv)
                if "openocd" in joined and "--version" not in joined:
                    result = {"rc": 1, "seconds": 0.1,
                              "output": ("Error: CMSIS-DAP command CMD_INFO "
                                         "failed.\n")}
                return result

        harness = CmdInfoFailHarness()
        args = _base_args(usbip=True, usbip_strict=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertFalse(_check_ids(report)["usbip.openocd.session"]["ok"])


class ExitCodeTests(unittest.TestCase):
    def test_unreachable_device_exit_code(self) -> None:
        class Unreachable(ext.FakeDevice):
            def request(self, *args, **kwargs):
                raise ext.DeviceUnreachable("simulated network down")

            def request_bytes(self, *args, **kwargs):
                raise ext.DeviceUnreachable("simulated network down")

        harness = ext.FakeHarness(Unreachable())
        args = _base_args(read_only=True)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_UNREACHABLE)
        self.assertTrue(_check_ids(report)["device.reachable"]["ok"] is False)

    def test_pass_fail_exit_codes_documented(self) -> None:
        self.assertEqual(ext.EXIT_PASS, 0)
        self.assertEqual(ext.EXIT_FAIL, 1)
        self.assertEqual(ext.EXIT_USAGE, 2)
        self.assertEqual(ext.EXIT_UNREACHABLE, 3)


class ImageRequirementTests(unittest.TestCase):
    TARGET = {
        "detected": True,
        "supported": True,
        "family": "stm32h7",
        "flash_size": 1048576,
        "uid": "<target-uid>",
    }

    def test_valid_h7_image_passes(self) -> None:
        errors, facts = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256=FLASH_SHA,
            expect_uid="1e00 0d00 1151 3332 3738 3732",
            expect_flash_kib=1024, target=self.TARGET, require_h7=True)
        self.assertEqual(errors, [])
        self.assertEqual(facts["image_size"], 4096)
        self.assertEqual(facts["image_sha256"], FLASH_SHA)

    def test_missing_image_is_rejected(self) -> None:
        errors, _ = ext.check_image_requirements(
            os.path.join(TMPDIR, "does-not-exist.bin"), expect_sha256="",
            expect_uid="", expect_flash_kib=0, target=self.TARGET,
            require_h7=True)
        self.assertTrue(any("not found" in error for error in errors))

    def test_non_h7_target_is_rejected(self) -> None:
        target = dict(self.TARGET, family="stm32f4", series="STM32F4")
        errors, _ = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256="", expect_uid="", expect_flash_kib=0,
            target=target, require_h7=True)
        self.assertTrue(any("STM32H7" in error for error in errors))

    def test_image_larger_than_flash_is_rejected(self) -> None:
        target = dict(self.TARGET, flash_size=1024)
        errors, _ = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256="", expect_uid="", expect_flash_kib=0,
            target=target, require_h7=True)
        self.assertTrue(any("does not fit" in error for error in errors))

    def test_uid_mismatch_is_rejected(self) -> None:
        errors, _ = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256="", expect_uid="DEADBEEF",
            expect_flash_kib=0, target=self.TARGET, require_h7=True)
        self.assertTrue(any("UID mismatch" in error for error in errors))

    def test_flash_kib_mismatch_is_rejected(self) -> None:
        errors, _ = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256="", expect_uid="",
            expect_flash_kib=512, target=self.TARGET, require_h7=True)
        self.assertTrue(any("KiB" in error for error in errors))

    def test_ota_min_size_is_enforced(self) -> None:
        errors, _ = ext.check_image_requirements(
            FLASH_IMAGE, expect_sha256="", expect_uid="", expect_flash_kib=0,
            target=self.TARGET, require_h7=False, min_size=64 * 1024)
        self.assertTrue(any("too small" in error for error in errors))


class HelperTests(unittest.TestCase):
    def test_sha256_file(self) -> None:
        self.assertEqual(ext.sha256_file(FLASH_IMAGE), FLASH_SHA)
        self.assertEqual(len(FLASH_SHA), 64)

    def test_normalize_uid(self) -> None:
        self.assertEqual(ext.normalize_uid("0x1e00 0d00"),
                         "1E000D00")
        self.assertEqual(ext.normalize_uid(" 1e000d00 "), "1E000D00")
        self.assertEqual(ext.normalize_uid(None), "")

    def test_is_sha256_hex(self) -> None:
        self.assertTrue(ext.is_sha256_hex(FLASH_SHA))
        self.assertFalse(ext.is_sha256_hex("abc"))
        self.assertFalse(ext.is_sha256_hex("z" * 64))
        self.assertFalse(ext.is_sha256_hex(None))

    def test_openocd_session_is_identify_only(self) -> None:
        args = _base_args()
        argv = ext.openocd_probe_commands(args)
        joined = " ".join(argv)
        self.assertIn("interface/cmsis-dap.cfg", argv)
        self.assertIn("target/stm32h7x.cfg", argv)
        self.assertIn("-s", argv)
        self.assertIn(args.openocd_scripts, argv)
        self.assertIn("-c init", joined)
        self.assertIn("-c shutdown", joined)
        for dangerous in ("flash", "erase", "write_image", "program",
                          "reset halt", "mdw"):
            self.assertNotIn(dangerous, joined)


class ReportAndSoakTests(unittest.TestCase):
    def test_read_only_report_shape(self) -> None:
        args = _base_args(read_only=True)
        report = _run(args)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        for key in ("tool", "version", "ok", "exit_code", "dry_run",
                    "host", "port", "started_at", "finished_at", "duration_s",
                    "modes", "thresholds", "summary", "checks", "sections",
                    "warnings", "error"):
            self.assertIn(key, report)
        self.assertEqual(report["tool"], "birdiebot-wifi-dap-hil-extended")
        self.assertIn("preflight", report["sections"])
        self.assertIn("final", report["sections"])
        self.assertTrue(_check_ids(report)["read_only.ok"]["ok"])

    def test_soak_cycles_and_heap_checks(self) -> None:
        device = ext.FakeDevice()
        args = _base_args(soak=True, duration=1.0, soak_run=0.3,
                          poll_interval=0.1)
        report = _run(args, ext.FakeHarness(device))
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        soak = report["sections"]["soak"]
        self.assertGreaterEqual(soak["cycles"], 1)
        self.assertEqual(soak["failures"], 0)
        self.assertGreater(soak["frames"], 0)
        self.assertIsNotNone(soak["heap_free_min"])
        checks = _check_ids(report)
        for check_id in ("soak.cycles", "soak.failures", "soak.frames",
                         "soak.heap.min", "soak.heap.no_leak",
                         "final.curve.stopped", "final.target.detected"):
            self.assertTrue(checks[check_id]["ok"], check_id)

    def test_soak_sampler_error_fails(self) -> None:
        device = ext.FakeDevice(curve_error=True)
        args = _base_args(soak=True, duration=1.0, soak_run=0.3,
                          poll_interval=0.1)
        report = _run(args, ext.FakeHarness(device))
        self.assertEqual(report["exit_code"], ext.EXIT_FAIL)
        self.assertFalse(_check_ids(report)["soak.curve.error"]["ok"])

    def test_ota_simulation_changes_slot_and_records_reboot(self) -> None:
        device = ext.FakeDevice(ota_version="v-test-before")
        harness = ext.FakeHarness(device)
        args = _base_args(ota_image=OTA_IMAGE, yes_ota=True, ota_settle=0.0,
                          ota_reboot_timeout=6.0)
        report = _run(args, harness)
        self.assertEqual(report["exit_code"], ext.EXIT_PASS)
        self.assertEqual(device.ota_attempts, 1)
        self.assertEqual(device.running_slot, "ota_1")
        ota = report["sections"]["ota"]
        self.assertEqual(ota["upload_response"]["slot"], "ota_1")
        self.assertTrue(ota["reboot"]["disconnect_seen"])
        self.assertTrue(ota["reboot"]["reachable_after"])
        checks = _check_ids(report)
        for check_id in ("ota.upload", "ota.slot.after", "ota.reboot.recovered",
                         "ota.health.target", "ota.web.served"):
            self.assertTrue(checks[check_id]["ok"], check_id)

    def test_report_file_is_written(self) -> None:
        report_path = os.path.join(TMPDIR, "report.json")
        args = _base_args(flash_image=FLASH_IMAGE, yes_flash=True,
                          flash_expect_sha256=FLASH_SHA, report=report_path)
        report = _run(args)
        ext._write_report(report, report_path)
        self.assertTrue(os.path.isfile(report_path))
        with open(report_path, "r", encoding="utf-8") as handle:
            written = json.load(handle)
        self.assertTrue(written["ok"])
        self.assertTrue(written["dry_run"])
        self.assertEqual(written["summary"]["required_failed"], 0)


if __name__ == "__main__":
    unittest.main()
