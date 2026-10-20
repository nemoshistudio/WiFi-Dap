"""Host/static regression for the DAP security-review fixes (Wave 3 / WP-302).

Covers fixes from docs/security-review-dap.md that can be verified without
hardware:

  * **N8** (P2) -- USBIP OUT short packet: the old handler always copied
    ``DAP_HANDLE_SIZE`` (516 B) from the shared TCP buffer, so stale bytes from
    a previous URB/session reached the DAP parser. The fix copies only the
    received ``length`` into a zero-filled packet. Tests: a Python model of the
    fixed handler (old-data marker + short packet -> tail all zero, long packet
    clamped at DAP_PACKET_SIZE) plus a source guard against re-introducing the
    old fixed-size copy.
  * **N2** (P0) -- USB string descriptor: remote-controlled ``wValue.u8lo``
    used as a ``strings_list[]`` index and an unbounded ``strlen`` into a
    ``char temp_buff[64]``. Tests: descriptor boundary/truncation/clip table
    derived from the fixed rules + source guard (index whitelist, max 31
    UTF-16LE chars, ``wLength`` clip, STALL on invalid index).
  * **N9** (P2/P3) -- ``DAP_TransferBlock``: the single 127 cap let the write
    direction read 1 byte past the 512-byte request packet. Tests: direction
    caps read 127 / write 126, request/response size boundary table, source
    guard, and a cross-check that the executable ASan harness
    (``tests/host/dap/harness.c``) still contains the N9 cases.

Fix commits: N2/N8 ``3de3a94`` (merge ``4dfddfe``); N1/N9/N6 ``666df38``
(merge ``89bfd0d``). The source guards are deliberate regression detectors: if
a fix or its required shape is removed/undone, the suite must fail.

No hardware, ESP-IDF, Docker or network is required.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import script_loader as sl

ROOT = sl.ROOT
DAP_HANDLE_C = ROOT / "components/dap_proxy/DAP_handle.c"
USB_HANDLE_C = ROOT / "components/USBIP/usb_handle.c"
DAP_C = ROOT / "components/DAP/cmsis-dap/source/DAP.c"
DAP_HARNESS_C = ROOT / "tests/host/dap/harness.c"

DAP_PACKET_SIZE = 512
DAP_HANDLE_SIZE = 4 + DAP_PACKET_SIZE  # DapPacket_t: uint32 length + buf[512]
USB_DT_STRING = 3
USB_STR_STAGE_BUFFER = 64
USB_STR_MAX_CHARS = (USB_STR_STAGE_BUFFER - 2) // 2  # bLength + bDescriptorType => 31


def read(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"expected source file missing: {path}")
    return path.read_text(encoding="utf-8")


def strip_c_comments(src: str) -> str:
    """Remove /* ... */ and // ... comments (negative guards must inspect code)."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


# ---------------------------------------------------------------------------
# N8: USBIP OUT short packet -> zero-filled request packet (no stale bytes)
# ---------------------------------------------------------------------------

def fixed_out_packet_model(payload: bytes, stale_marker: int = 0xAA) -> bytes:
    """Model of the fixed DAP_handle.c handle_dap_data_request() (N8).

    The static ``dap_request_packet`` is pre-filled with stale marker bytes
    (simulating data from a previous URB/session), then the fix does:
        memset(&dap_request_packet, 0, sizeof(dap_request_packet));
        memcpy(dap_request_packet.buf, data_in, length);   // length clamped
    Returns the 516-byte packet (4-byte length prefix + 512-byte payload buf).
    """
    packet = bytearray([stale_marker]) * DAP_HANDLE_SIZE
    length = min(len(payload), DAP_PACKET_SIZE)
    packet[:] = b"\x00" * DAP_HANDLE_SIZE  # memset(&dap_request_packet, 0, ...)
    packet[4:4 + length] = payload[:length]  # memcpy(..., data_in, length)
    return bytes(packet)


class TestN8StaleOutData(unittest.TestCase):
    def test_short_packet_tail_is_zeroed(self) -> None:
        payload = b"\x01\x02Hello"
        packet = fixed_out_packet_model(payload)
        self.assertEqual(len(packet), DAP_HANDLE_SIZE)
        self.assertEqual(packet[4:4 + len(payload)], payload)
        # N8 regression: the tail must be zero, never the stale 0xAA marker.
        self.assertEqual(packet[4 + len(payload):], b"\x00" * (DAP_PACKET_SIZE - len(payload)))
        self.assertNotIn(0xAA, packet)

    def test_old_fixed_size_copy_would_have_leaked_marker(self) -> None:
        # Demonstrates the pre-fix behaviour (copying DAP_HANDLE_SIZE from the
        # RX buffer) would forward stale marker bytes; documents why N8 matters.
        payload = b"\x7F\x01\x00"
        stale_rx = bytearray([0xAA]) * DAP_HANDLE_SIZE
        stale_rx[:len(payload)] = payload
        old_packet = bytes(stale_rx)  # old: ring item was the raw RX buffer slice
        self.assertIn(0xAA, old_packet[len(payload):])

        fixed_packet = fixed_out_packet_model(payload)
        self.assertNotIn(0xAA, fixed_packet)

    def test_long_packet_is_clamped_to_dap_packet_size(self) -> None:
        payload = bytes([(i & 0xFF) for i in range(DAP_PACKET_SIZE + 88)])
        packet = fixed_out_packet_model(payload)
        self.assertEqual(packet[4:], payload[:DAP_PACKET_SIZE])
        self.assertEqual(len(packet), DAP_HANDLE_SIZE)  # no OOB write

    def test_source_guard_fix_markers(self) -> None:
        src = read(DAP_HANDLE_C)
        self.assertIn("N8", src, "N8 fix comment missing from DAP_handle.c")
        self.assertIn("if (length > DAP_PACKET_SIZE)", src,
                      "N8: OUT length must be clamped to DAP_PACKET_SIZE")
        self.assertIn("memset(&dap_request_packet, 0, sizeof(dap_request_packet));", src,
                      "N8: request packet must be zero-filled before the bounded copy")
        self.assertIn("memcpy(dap_request_packet.buf, data_in, length);", src,
                      "N8: only the received data_length may be copied")
        self.assertIn("xRingbufferSend(dap_dataIN_handle, &dap_request_packet,", src,
                      "N8: the fixed-size packet (not the raw RX buffer) is queued")
        # Old pattern must not come back: copying DAP_HANDLE_SIZE from data_in.
        code = strip_c_comments(src)
        self.assertNotIn("data_in - sizeof(uint32_t)", code,
                         "N8 regression: raw RX buffer copy with fixed 516B size is back")
        self.assertNotIn("data_in), DAP_HANDLE_SIZE", code)


# ---------------------------------------------------------------------------
# N2: USB string descriptor index whitelist / length bounds / wLength clip
# ---------------------------------------------------------------------------

# Mirrors usb_handle.c strings_list[]: index 1..3 are real strings (index 0 is
# the LANGID descriptor handled separately, index 0xEE is the MS-OS special).
STRINGS = [None, "windowsair", "Wireless ESP CMSIS-DAP", "1234"]


def clip_len(length: int, wlength: int) -> int:
    """Mirror of usb_handle_clip_response_len(): wLength==0 means 'no clip'."""
    if length < 0:
        return 0
    if wlength != 0 and length > wlength:
        return wlength
    return length


def string_descriptor_model(index: int, wlength: int = 0,
                            strings=STRINGS, max_chars: int = USB_STR_MAX_CHARS):
    """Model of the fixed USB_DT_STRING branch (N2).

    Returns descriptor bytes, b"" for the MS-OS 0xEE special, or None for an
    invalid index (device STALLs the control request with -EPIPE).
    """
    if index == 0:  # String Descriptor Zero: LANGID list
        data = bytes([4, USB_DT_STRING, 0x09, 0x04])
        return data[:clip_len(len(data), wlength)]
    if index == 0xEE:  # MS OS 1.0 string: not provided
        return b""
    if index >= len(strings) or strings[index] is None:
        return None  # STALL (-EPIPE)
    text = strings[index]
    slen = min(len(text), max_chars)
    body = text[:slen].encode("utf-16-le")
    data = bytes([2 + len(body), USB_DT_STRING]) + body
    return data[:clip_len(len(data), wlength)]


class TestN2UsbStringDescriptor(unittest.TestCase):
    def test_langid_descriptor_index_zero(self) -> None:
        self.assertEqual(string_descriptor_model(0), bytes([4, 3, 0x09, 0x04]))
        self.assertEqual(string_descriptor_model(0, wlength=2), bytes([4, 3]))

    def test_valid_string_indexes(self) -> None:
        for index in (1, 2, 3):
            with self.subTest(index=index):
                data = string_descriptor_model(index)
                self.assertIsNotNone(data)
                self.assertEqual(data[1], USB_DT_STRING)
                self.assertEqual(data[0], len(data))
                self.assertEqual(len(data), 2 + 2 * len(STRINGS[index]))

    def test_invalid_indexes_stall(self) -> None:
        for index in (4, 5, 17, 255):
            with self.subTest(index=index):
                self.assertIsNone(string_descriptor_model(index),
                                  "out-of-range string index must STALL (old OOB read)")
        self.assertEqual(string_descriptor_model(0xEE), b"")

    def test_long_string_is_truncated_to_stage_buffer(self) -> None:
        long_text = "A" * 40
        strings = [None, long_text]
        data = string_descriptor_model(1, strings=strings)
        self.assertEqual(len(data), USB_STR_STAGE_BUFFER)  # 64 B, never > temp_buff[64]
        self.assertEqual(data[0], USB_STR_STAGE_BUFFER)
        self.assertLessEqual(len(data), USB_STR_STAGE_BUFFER)

    def test_wlength_clipping(self) -> None:
        full = string_descriptor_model(2)
        self.assertEqual(string_descriptor_model(2, wlength=6), full[:6])
        self.assertEqual(string_descriptor_model(2, wlength=1), full[:1])
        self.assertEqual(string_descriptor_model(2, wlength=10_000), full)

    def test_utf16le_encoding(self) -> None:
        data = string_descriptor_model(1)  # "windowsair"
        self.assertEqual(data[2:], "windowsair".encode("utf-16-le"))

    def test_source_guard_fix_markers(self) -> None:
        src = read(USB_HANDLE_C)
        self.assertIn("N2", src, "N2 fix comment missing from usb_handle.c")
        self.assertIn("if (str_index >= ARRAY_SIZE(strings_list) || strings_list[str_index] == NULL)",
                      src, "N2: index whitelist check is gone")
        self.assertIn("send_stage2_submit(header, -EPIPE, 0);", src,
                      "N2: invalid index must STALL")
        self.assertIn("const size_t max_chars = (sizeof(temp_buff) - sizeof(usb_string_descriptor)) / sizeof(uint16_t);",
                      src, "N2: bounded UTF-16LE encoding is gone")
        self.assertIn("usb_handle_clip_response_len(header, (int)buff_len)", src,
                      "N2: wLength clipping is gone")
        self.assertIn("_Static_assert(sizeof(temp_buff) > sizeof(usb_string_descriptor)", src)
        self.assertNotIn("strlen(strings_list[", strip_c_comments(src),
                         "N2 regression: strlen(strings_list[idx]) on a remote index is back")


# ---------------------------------------------------------------------------
# N9: TransferBlock direction-aware caps (read 127 / write 126)
# ---------------------------------------------------------------------------

class TestN9TransferBlockBounds(unittest.TestCase):
    @staticmethod
    def max_count(rnw: bool) -> int:
        # Mirrors DAP_TransferBlock() in DAP.c:
        #   read  : (DAP_PACKET_SIZE - 4) / 4 = 127
        #   write : (DAP_PACKET_SIZE - 5) / 4 = 126
        return (DAP_PACKET_SIZE - (4 if rnw else 5)) // 4

    @staticmethod
    def request_size(count: int, rnw: bool) -> int:
        # read : cmd(1)+index(1)+count(2)+value(1) = 5 B (no payload)
        # write: cmd(1)+index(1)+count(2)+value(1)+4*count = 5 + 4*count
        return (5 if rnw else 5 + 4 * count)

    @staticmethod
    def response_size(count: int, rnw: bool) -> int:
        # read response = cmd echo(1) + 3 + 4*count; write response = echo + 3
        return 1 + (3 + 4 * count if rnw else 3)

    def test_direction_dependent_caps(self) -> None:
        self.assertEqual(self.max_count(True), 127, "read cap must stay 127")
        self.assertEqual(self.max_count(False), 126, "write cap must be 126 (N9)")

    def test_request_sizes_prove_the_old_off_by_one(self) -> None:
        # N9: write count=127 needs 513 bytes -> 1 byte past the 512 B packet.
        self.assertEqual(self.request_size(127, rnw=False), 513)
        self.assertGreater(self.request_size(127, rnw=False), DAP_PACKET_SIZE)
        self.assertLessEqual(self.request_size(126, rnw=False), DAP_PACKET_SIZE)
        self.assertLessEqual(self.request_size(127, rnw=True), DAP_PACKET_SIZE)

    def test_read_response_fits_exactly_at_127(self) -> None:
        self.assertEqual(self.response_size(127, rnw=True), DAP_PACKET_SIZE)
        self.assertGreater(self.response_size(128, rnw=True), DAP_PACKET_SIZE)

    def test_boundary_table(self) -> None:
        cases = [
            (True, 0, True), (True, 1, True), (True, 127, True),
            (True, 128, False), (True, 0xFFFF, False),
            (False, 0, True), (False, 126, True), (False, 127, False),
            (False, 0xFFFF, False),
        ]
        for rnw, count, accepted in cases:
            with self.subTest(rnw=rnw, count=count):
                got = count <= self.max_count(rnw)
                self.assertEqual(got, accepted)

    def test_source_guard_fix_markers(self) -> None:
        src = read(DAP_C)
        self.assertIn("N9", src, "N9 fix comment missing from DAP.c")
        self.assertIn("(DAP_PACKET_SIZE - 4U) / 4U", src,
                      "N9: read-direction cap missing")
        self.assertIn("(DAP_PACKET_SIZE - 5U) / 4U", src,
                      "N9: write-direction cap missing")
        self.assertIn("request_count > max_count", src,
                      "N9: over-limit count must be rejected")
        self.assertIn("DAP_ERROR", src)

    def test_executable_asan_harness_has_n9_cases(self) -> None:
        src = read(DAP_HARNESS_C)
        for marker in ("test_n9_write_block_127_rejected",
                       "test_n9_read_block_127_accepted",
                       "test_n9_read_block_128_rejected",
                       "test_n9_write_block_126_accepted"):
            with self.subTest(marker=marker):
                self.assertIn(marker, src, f"N9 ASan harness case missing: {marker}")


if __name__ == "__main__":
    unittest.main()
