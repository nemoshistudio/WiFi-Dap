"""Static contract tests for the M3 DAP-over-TCP executor bridge.

The transport (components/dap_tcp) is deliberately DAP-engine free: it calls the
weak hooks ``birdiebot_dap_tcp_link_changed()`` / ``birdiebot_dap_tcp_execute()``.
The strong definitions live in main/dap_tcp_bridge.c and are default-off.  These
host tests pin the contract that wiring mistakes historically broke:

  * the bridge is actually referenced by app_main (otherwise the linker keeps
    the weak "not wired" stubs and the backend silently closes connections);
  * ownership hand-off uses DELETE_HANDLE/RESET_HANDLE with a bounded wait;
  * DAP_ExecuteCommand()'s packed return value is masked with 0xFFFF
    (request length << 16 | response length) -- treating it as a raw byte
    count would copy a huge length;
  * the engine packet size is capped for OpenOCD's 1024-byte TCP packets;
  * CONFIG_BIRDIEBOT_DAP_TCP_ENABLE stays default-off in the shipped tree.

No hardware, ESP-IDF, Docker or network is required.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import script_loader as sl

ROOT = sl.ROOT
BRIDGE_C = ROOT / "main/dap_tcp_bridge.c"
BRIDGE_H = ROOT / "main/dap_tcp_bridge.h"
MAIN_C = ROOT / "main/main.c"
DAP_TCP_SERVER_C = ROOT / "components/dap_tcp/src/dap_tcp_server.c"
DAP_TCP_KCONFIG = ROOT / "components/dap_tcp/Kconfig"
DAP_CONFIGURATION_H = ROOT / "main/dap_configuration.h"


def read(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"expected source file missing: {path}")
    return path.read_text(encoding="utf-8")


def strip_c_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


class TestDapTcpBridgeContract(unittest.TestCase):
    def test_bridge_files_exist(self) -> None:
        self.assertTrue(BRIDGE_C.is_file(), "main/dap_tcp_bridge.c must exist")
        self.assertTrue(BRIDGE_H.is_file(), "main/dap_tcp_bridge.h must exist")

    def test_strong_hooks_present(self) -> None:
        src = strip_c_comments(read(BRIDGE_C))
        for marker in (
            "void birdiebot_dap_tcp_link_changed(bool connected)",
            "int birdiebot_dap_tcp_execute(",
            "void dap_tcp_bridge_init(void)",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, src, f"bridge strong hook missing: {marker}")

    def test_ownership_handoff(self) -> None:
        src = strip_c_comments(read(BRIDGE_C))
        self.assertIn("dap_handle_request_reset(DELETE_HANDLE)", src,
                      "the TCP session must detach the USBIP data path first")
        self.assertIn("dap_handle_wait_reset(", src,
                      "the hand-off must wait for the DAP task acknowledgement")
        self.assertIn("dap_handle_request_reset(RESET_HANDLE)", src,
                      "disconnect must restore the USBIP data path")
        self.assertIn("DAP_TCP_BRIDGE_RESET_WAIT_MS", src,
                      "the ownership wait must be bounded")

    def test_execute_masks_packed_return_value(self) -> None:
        src = strip_c_comments(read(BRIDGE_C))
        self.assertIn("DAP_ExecuteCommand(", src,
                      "the bridge must run the CMSIS-DAP engine")
        # DAP_ExecuteCommand returns (request_len << 16) | response_len.
        self.assertRegex(
            src,
            r"DAP_ExecuteCommand\([^;]*\)\s*&\s*0xFFFFU",
            "packed DAP_ExecuteCommand() return must be masked with 0xFFFF",
        )
        self.assertIn("n > response_cap", src,
                      "oversized responses must be rejected, not truncated")
        self.assertIn("DAP_PACKET_SIZE > 1024U", src,
                      "compile-time packet-size guard for OpenOCD's 1024 limit")

    def test_bridge_referenced_by_app_main(self) -> None:
        src = strip_c_comments(read(MAIN_C))
        self.assertIn("dap_tcp_bridge_init();", src,
                      "app_main must reference the bridge object (linker drop guard)")
        self.assertIn("dap_tcp_server_start();", src,
                      "app_main must start the TCP backend (no-op when disabled)")
        main_init = src.find("dap_tcp_bridge_init();")
        server_start = src.find("dap_tcp_server_start();")
        self.assertLess(main_init, server_start,
                        "the bridge object must be linked before the server starts")

    def test_weak_defaults_still_not_wired(self) -> None:
        src = strip_c_comments(read(DAP_TCP_SERVER_C))
        self.assertIn("__attribute__((weak)) int birdiebot_dap_tcp_execute", src,
                      "the transport must keep the weak default hook")
        self.assertIn("__attribute__((weak)) void birdiebot_dap_tcp_link_changed",
                      src,
                      "the transport must keep the weak link hook")
        self.assertIn("return -1;", src,
                      "the weak default must report 'not wired'")

    def test_tcp_backend_is_default_off(self) -> None:
        kconfig = read(DAP_TCP_KCONFIG)
        match = re.search(
            r"config BIRDIEBOT_DAP_TCP_ENABLE\b.*?(?=^\s*config |\Z)",
            kconfig,
            flags=re.S | re.M,
        )
        self.assertIsNotNone(match, "BIRDIEBOT_DAP_TCP_ENABLE config block missing")
        self.assertIsNotNone(
            re.search(r"^\s+default n\s*$", match.group(0), flags=re.M),
            "the shipped default must keep the TCP backend off",
        )

    def test_packet_size_cap_in_configuration(self) -> None:
        src = read(DAP_CONFIGURATION_H)
        self.assertIn("CONFIG_BIRDIEBOT_DAP_TCP_ENABLE", src,
                      "dap_configuration.h must cap DAP_PACKET_SIZE for TCP")
        self.assertIn("#undef DAP_PACKET_SIZE", src,
                      "the TCP build must override the S3 1280-byte packet size")


if __name__ == "__main__":
    unittest.main()
