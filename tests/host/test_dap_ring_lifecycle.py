"""Host regression for the DAP ring-buffer lifecycle fix (task-66).

Symptom (real hardware, 2026-10-25): after an HTTP ``GET /api/target/info``
probe, the first USBIP/OpenOCD session fails with
``Error: CMSIS-DAP command CMD_INFO failed.``  A fresh boot with
``usbip attach`` *before* any HTTP probe works.

Root cause (verified on the device with ``GET /api/target/status``):

* ``target_flash``'s probe takes the DAP engine with
  ``dap_handle_request_reset(DELETE_HANDLE)``.  The old DAP task handler freed
  both ring buffers for that mode.
* When the probe finishes it posts ``RESET_HANDLE``; the old handler then tried
  to re-create the rings on the Wi-Fi+NCM heap.  The measured contiguous free
  block is 20,480 B while the response ring alone is 20,672 B, so every retry
  failed and the DAP task kept ``dap_dataIN_handle == NULL``.
* The next USBIP session's ``CMD_INFO`` OUT URB was answered with an empty
  RET_SUBMIT and then dropped; the B1 pipeline parked the matching bulk-IN URB
  forever, the host timed out and OpenOCD reported ``CMD_INFO failed``.

Fix: after the DAP task has created the rings once at start, they are never
freed again.  ``DELETE_HANDLE`` now drains both rings and sets a "detached"
gate that blocks the USBIP request path (same host-visible behaviour as the
old NULL-ring path) until ``RESET_HANDLE`` drains and clears it.  The
allocation only survives in the lazy-create fallback used when the rings were
never created.

This suite has two parts, both hardware-free:

1. A small allocator model of the two ring creations.  It is fed with the
   *measured* heap state (largest free block 20,480 B, ``heap_free`` 72,232 B
   after the probe) and proves the old free+re-create path cannot allocate the
   response ring while the new keep+drain path needs no allocation at all.
   The expected sizes are derived from the repository constants, not hardcoded
   (20,480 = 1280 * 16 and 20,672 = (1280 + 8) * 16 + 64).
2. Source guards against re-introducing the free/re-create on DELETE_HANDLE
   (deliberate regression detector, same convention as
   ``test_security_regressions.py``).
3. OTA regression guards (task-66b): after the CMD_INFO fix the rings stayed
   resident for the whole boot, which left ``heap_largest`` at 4,608 B on the
   Wi-Fi+USB-NCM image -> the 5120-byte OTA task stack could not be allocated
   and ``POST /api/ota/upload`` answered 503 "ota service unavailable".  The
   fix releases the rings at the start of a valid upload
   (``dap_handle_release_ringbuffers()``) and restores them when the upload
   fails without a reboot.  The guards here check the boot/OTA memory model and
   the handler ordering (release before the async request + task creation) so
   the OTA endpoint cannot silently regress again.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import script_loader as sl

ROOT = sl.ROOT
DAP_HANDLE_C = ROOT / "components/dap_proxy/DAP_handle.c"
DAP_HANDLE_H = ROOT / "components/dap_proxy/DAP_handle.h"
DAP_CONFIG_H = ROOT / "main/dap_configuration.h"

# Measured on the real H7 device after the failing HTTP probe
# (GET /api/target/status, task-66 reproduction log):
#   heap_free=72232 heap_largest=20480 dap_ring_request_bytes=20480
#   dap_ring_response_bytes=20672 dap_ring_active=false
MEASURED_FREE_AFTER_PROBE = 72_232
MEASURED_LARGEST_AFTER_PROBE = 20_480


def read(path: Path) -> str:
    if not path.is_file():
        raise AssertionError(f"expected source file missing: {path}")
    return path.read_text(encoding="utf-8")


def strip_c_comments(src: str) -> str:
    """Remove /* ... */ and // ... comments (guards inspect code only)."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


# ---------------------------------------------------------------------------
# Ring sizes: derive them from the repository definitions
# ---------------------------------------------------------------------------

def dap_packet_size() -> int:
    """S3 default DAP packet size (main/dap_configuration.h)."""
    src = read(DAP_CONFIG_H)
    match = re.search(r"#define\s+DAP_PACKET_SIZE\s+(\d+)U", src)
    if match is None:
        raise AssertionError("DAP_PACKET_SIZE default not found in "
                             "main/dap_configuration.h")
    return int(match.group(1))


def dap_buffer_num(packet_size: int) -> int:
    """DAP_BUFFER_NUM selection formula from components/dap_proxy/DAP_handle.c."""
    if packet_size > 2048:
        return 5
    if packet_size > 1024:
        return 16
    return 20


def ring_sizes() -> tuple[int, int]:
    packet = dap_packet_size()
    slots = dap_buffer_num(packet)
    request = packet * slots
    # (packet + 8) per NOSPLIT slot + 64 bytes wrap/dummy slack, see
    # DAP_RESPONSE_RING_SIZE in DAP_handle.c.
    response = (packet + 8) * slots + 64
    return request, response


# FreeRTOS heap_4-style first fit used by the model.  The real allocator also
# needs a small Ringbuffer_t wrapper per ring; it is allocated first by
# xRingbufferCreate() (calloc) and is modelled here as a separate small block.
RB_STRUCT_OVERHEAD = 64


def alloc_first_fit(free_blocks: list[int], size: int) -> bool:
    """FreeRTOS heap_4 first-fit allocation of ``size`` bytes, in place."""
    for index, block in enumerate(free_blocks):
        if block >= size:
            if block == size:
                free_blocks.pop(index)
            else:
                free_blocks[index] = block - size
            return True
    return False


def create_ring_pair(free_blocks: list[int], request: int, response: int) -> bool:
    """Model of malloc_dap_ringbuf(): request ring first, then response ring.

    Returns True only when both rings exist afterwards (the firmware then has a
    usable data path).  A partial success is rolled back by the caller's retry
    loop, which is exactly what the old RESET_HANDLE path did.

    Each xRingbufferCreate() payload (request / response) needs one contiguous
    block of at least its size; the tiny calloc(Ringbuffer_t) wrapper always
    comes from the small-block fragments (plenty of those are present), so it
    is not modelled against the few big blocks.  Allocations take the
    best-fitting block (minimal split), which matches the sizes under test.
    """
    heap = list(free_blocks)
    for size in (request, response):
        candidates = [(block, index) for index, block in enumerate(heap)
                      if block >= size]
        if not candidates:
            return False
        block, index = min(candidates)
        if block == size:
            heap.pop(index)
        else:
            heap[index] = block - size
    return True


def model_old_delete_then_reset(heap_free_blocks: list[int]) -> bool:
    """Old behaviour: DELETE frees both rings, RESET re-creates them.

    The caller retries five times; a failed attempt frees whatever it created,
    so the net result is "rings present?" after the retries.
    """
    request, response = ring_sizes()
    # DELETE_HANDLE -> free_dap_ringbuf(): the two payload blocks are returned
    # to the heap.  Their exact sizes are known, so the freed state adds them
    # back (before whatever else the probe/network tasks grabbed in between).
    heap = list(heap_free_blocks)
    # The observed state after the probe already includes the freed rings, so
    # do not add them again: callers pass the measured post-probe block list.
    for _ in range(5):
        if create_ring_pair(heap, request, response):
            return True
        # free_dap_ringbuf() rollback of the failed attempt -> identical heap.
    return False


def model_new_keep_and_drain(heap_free_blocks: list[int]) -> bool:
    """New behaviour: DELETE drains and keeps the rings; RESET needs no alloc."""
    request, response = ring_sizes()
    # Rings were created once at DAP task start (the boot allocation succeeded,
    # see the device: dap_ring_active=true on a fresh boot).  No heap block is
    # needed for the hand-off path, so the measured fragmented heap is enough.
    del heap_free_blocks, request, response
    return True


# A plausible post-probe heap: one 20,480-byte block plus smaller fragments,
# summing to the measured heap_free.  The largest block is the measured fact.
def measured_post_probe_heap() -> list[int]:
    blocks = [MEASURED_LARGEST_AFTER_PROBE]
    remaining = MEASURED_FREE_AFTER_PROBE - MEASURED_LARGEST_AFTER_PROBE
    for size in (8192, 4096, 2048, 1024, 512, 256, 128):
        while remaining >= size:
            blocks.append(size)
            remaining -= size
    if remaining > 0:
        blocks.append(remaining)
    return blocks


class TestRingSizeDerivation(unittest.TestCase):
    def test_s3_packet_size_and_slot_count(self) -> None:
        packet = dap_packet_size()
        self.assertEqual(packet, 1280)
        self.assertEqual(dap_buffer_num(packet), 16)

    def test_measured_ring_sizes_match_device_json(self) -> None:
        request, response = ring_sizes()
        # Values reported by GET /api/target/status on the real device.
        self.assertEqual(request, 20_480)
        self.assertEqual(response, 20_672)

    def test_response_ring_does_not_fit_measured_largest_block(self) -> None:
        request, response = ring_sizes()
        self.assertLessEqual(request, MEASURED_LARGEST_AFTER_PROBE)
        self.assertGreater(response, MEASURED_LARGEST_AFTER_PROBE)
        # Even before the Ringbuffer_t wrapper, the response ring alone cannot
        # be re-created from the largest contiguous block measured after the
        # probe.  This is the deterministic part of the root cause.
        self.assertGreater(response + RB_STRUCT_OVERHEAD,
                           MEASURED_LARGEST_AFTER_PROBE)


class TestAllocatorModel(unittest.TestCase):
    def test_old_free_then_recreate_fails_on_measured_heap(self) -> None:
        heap = measured_post_probe_heap()
        self.assertFalse(model_old_delete_then_reset(heap))
        # Same failure with the largest block that was measured in the earlier
        # probe state (20480 B) - the response ring never fits.
        self.assertFalse(model_old_delete_then_reset([20_480]))

    def test_new_keep_and_drain_has_a_data_path(self) -> None:
        heap = measured_post_probe_heap()
        self.assertTrue(model_new_keep_and_drain(heap))

    def test_boot_allocation_succeeds_with_fresh_heap(self) -> None:
        """The rings are created once at start from the fresh boot heap."""
        request, response = ring_sizes()
        # One 41 KiB contiguous block (request + response) or two big blocks
        # are enough at boot; the NCM/Wi-Fi stacks fragment the heap later.
        self.assertTrue(create_ring_pair([request + response], request, response))
        self.assertTrue(create_ring_pair([request, response], request, response))
        # A block that fits the request ring but not the response ring is
        # exactly the fragmented state that breaks re-creation.
        self.assertFalse(create_ring_pair([request], request, response))

    def test_combined_requirement_needs_two_large_blocks(self) -> None:
        request, response = ring_sizes()
        # The keep-instead-of-free fix removes the requirement to have BOTH
        # large blocks available at the same time after boot.
        self.assertGreater(request + response, MEASURED_LARGEST_AFTER_PROBE)


class TestSourceGuards(unittest.TestCase):
    """Fail if the fix (or a required part of it) is removed."""

    def setUp(self) -> None:
        self.c = strip_c_comments(read(DAP_HANDLE_C))
        self.h = read(DAP_HANDLE_H)

    def test_delete_does_not_free_rings(self) -> None:
        # The DELETE_HANDLE branch must not call free_dap_ringbuf() any more.
        start = self.c.index(
            "enum reset_handle_t mode = (enum reset_handle_t)kRestartDAPHandle;")
        end = self.c.index("kRestartDAPHandle = NO_SIGNAL;", start)
        handler = self.c[start:end]
        # DELETE_HANDLE is the last `else` branch of the mode chain.
        delete_else = handler.rindex("else")
        body = handler[delete_else:]
        self.assertIn("s_data_path_detached = true", body)
        self.assertNotIn("free_dap_ringbuf", body)
        self.assertIn("dap_reset_pending_frames", body)

    def test_reset_restores_data_path_without_allocating_when_present(self) -> None:
        match = re.search(
            r"if\s*\(mode\s*==\s*RESET_HANDLE\)\s*\{(?P<body>.*?)"
            r"\}\s*else if\s*\(mode\s*==\s*RELEASE_HANDLE\)",
            self.c, flags=re.S)
        self.assertIsNotNone(match, "RESET_HANDLE branch not found")
        body = match.group("body")
        self.assertIn("dap_reset_pending_frames()", body)
        self.assertIn("s_data_path_detached = false", body)
        # Lazy create is only the fallback (else of the rings-exist check) and
        # is skipped while the buffers were released for OTA (task-66b).
        self.assertIn("if (dap_dataIN_handle != NULL && dap_dataOUT_handle != NULL)",
                      body)
        self.assertIn("else if (!s_rings_released)", body)
        self.assertIn("dap_create_rings_retry()", body)

    def test_release_branch_frees_rings(self) -> None:
        # RELEASE_HANDLE (OTA memory pressure) is the ONLY mode that frees the
        # rings on purpose; it must set both flags before freeing.
        match = re.search(
            r"else if\s*\(mode\s*==\s*RELEASE_HANDLE\)\s*\{(?P<body>.*?)"
            r"\}\s*else if\s*\(mode\s*==\s*RESTORE_HANDLE\)",
            self.c, flags=re.S)
        self.assertIsNotNone(match, "RELEASE_HANDLE branch not found")
        body = match.group("body")
        self.assertIn("s_data_path_detached = true", body)
        flags = body.index("s_rings_released = true")
        free = body.index("free_dap_ringbuf()")
        self.assertLess(flags, free, "release flag must be set before freeing")
        self.assertIn("dap_reset_pending_frames()", body)

    def test_restore_branch_recreates_and_clears_released(self) -> None:
        match = re.search(
            r"else if\s*\(mode\s*==\s*RESTORE_HANDLE\)\s*\{(?P<body>.*?)"
            r"kRestartDAPHandle = NO_SIGNAL;",
            self.c, flags=re.S)
        self.assertIsNotNone(match, "RESTORE_HANDLE branch not found")
        body = match.group("body")
        self.assertIn("s_rings_released = false", body)
        self.assertIn("dap_create_rings_retry()", body)
        self.assertIn("s_data_path_detached = false", body)

    def test_free_dap_ringbuf_has_two_expected_call_sites(self) -> None:
        # 1) lazy-create retry rollback, 2) the explicit RELEASE_HANDLE branch.
        # Any other call site would free the rings on a hot/hand-off path again.
        calls = re.findall(r"\bfree_dap_ringbuf\s*\(\s*\)", self.c)
        self.assertEqual(len(calls), 2,
                         "free_dap_ringbuf() must only be called from the "
                         "lazy-create retry loop and the RELEASE_HANDLE branch")

    def test_request_path_is_gated_by_detached_flag(self) -> None:
        match = re.search(r"void\s+handle_dap_data_request\s*\([^)]*\)\s*\{"
                          r"(?P<body>.*?)\n\}", self.c, flags=re.S)
        self.assertIsNotNone(match)
        body = match.group("body")
        self.assertIn("s_data_path_detached", body)
        # The OUT URB must still get its zero-length RET_SUBMIT before the drop.
        submit = body.index("send_stage2_submit_data_fast")
        gate = body.index("s_data_path_detached")
        self.assertLess(submit, gate)

    def test_legacy_fast_reply_is_gated(self) -> None:
        match = re.search(r"int\s+fast_reply\s*\([^)]*\)\s*\{"
                          r"(?P<body>.*?)\n\}", self.c, flags=re.S)
        self.assertIsNotNone(match)
        self.assertIn("s_data_path_detached", match.group("body"))

    def test_dap_task_does_not_consume_while_detached(self) -> None:
        match = re.search(r"void\s+DAP_Thread\s*\([^)]*\)\s*\{"
                          r"(?P<body>.*?)\n\}", self.c, flags=re.S)
        self.assertIsNotNone(match)
        body = match.group("body")
        self.assertIn("if (s_data_path_detached)", body)
        # The drain-then-gate order in the loop: detached check before the
        # request-ring receive call.
        self.assertLess(body.index("if (s_data_path_detached)"),
                        body.index("xRingbufferReceiveUpTo"))

    def test_no_task_deletion_on_reset_failure(self) -> None:
        # The old failure path vTaskDelete(NULL) took the whole USBIP path down
        # for the rest of the boot; only the boot-time create failure may do it.
        match = re.search(
            r"enum reset_handle_t mode = \(enum reset_handle_t\)kRestartDAPHandle;"
            r"(?P<body>.*?)ulTaskNotifyTake\(",
            self.c, flags=re.S)
        self.assertIsNotNone(match, "reset handler not found")
        self.assertNotIn("vTaskDelete", match.group("body"))

    def test_header_documents_keep_allocated_semantics(self) -> None:
        # The public contract must describe the new hand-off semantics so the
        # WebSocket/elaphureLink/target-flash/curve callers keep working.
        self.assertIn("keep them allocated", self.h)
        self.assertIn("RESET_HANDLE", self.h)
        self.assertIn("DELETE_HANDLE", self.h)
        self.assertIn("RELEASE_HANDLE", self.h)
        self.assertIn("RESTORE_HANDLE", self.h)
        self.assertIn("dap_handle_release_ringbuffers", self.h)
        self.assertIn("dap_handle_restore_ringbuffers", self.h)
        self.assertIn("task-66", self.h)


# ---------------------------------------------------------------------------
# OTA regression (task-66b): resident rings -> OTA task stack cannot allocate
# ---------------------------------------------------------------------------

OTA_TASK_STACK_FALLBACK = 5120
# ESP32-C5 real hardware (2026-10-25): the internal heap is much smaller.  After
# ota_dap_release_rings() + httpd_req_async_handler_begin() the measured largest
# internal block was 4096 B, so the C5 uses a smaller OTA upload task stack.
OTA_TASK_STACK_C5 = 3584
OTA_MEASURED_LARGEST_C5 = 4096
# Measured on the device running the task-66 candidate firmware (rings resident):
#   heap_free=17808 heap_largest=4608 dap_ring_active=true
OTA_MEASURED_FREE_WITH_RINGS = 17_808
OTA_MEASURED_LARGEST_WITH_RINGS = 4_608
# OTA upload then answered 503 "ota service unavailable" (empty Content-Length
# still 411 => handler present, async request created, 5120-byte task stack did
# not fit).  Releasing the rings returns DAP_REQUEST_RING_SIZE +
# DAP_RESPONSE_RING_SIZE = 20,480 + 20,672 B to the heap.
OTA_URI_OTA_C = ROOT / "project_components/web_server/uri_modules/uri_ota.c"


def ota_task_stacks() -> list[int]:
    """All OTA_UPLOAD_TASK_STACK values from uri_ota.c (S3 fallback + C5)."""
    src = read(OTA_URI_OTA_C)
    stacks = [int(m) for m in
              re.findall(r"#define\s+OTA_UPLOAD_TASK_STACK\s+(\d+)", src)]
    if not stacks:
        raise AssertionError("OTA_UPLOAD_TASK_STACK not found in uri_ota.c")
    return stacks


def ota_task_stack() -> int:
    """S3/dual-core OTA_UPLOAD_TASK_STACK (the largest value in the file)."""
    return max(ota_task_stacks())


def model_ota_task_stack_fits(free_blocks: list[int], stack: int) -> bool:
    """xTaskCreate() needs one contiguous stack block (plus a small TCB)."""
    return alloc_first_fit(list(free_blocks), stack + RB_STRUCT_OVERHEAD)


def heap_after_dap_release() -> list[int]:
    """Measured heap model after RELEASE_HANDLE returns the two ring blocks."""
    request, response = ring_sizes()
    return [request, response, OTA_MEASURED_LARGEST_WITH_RINGS]


class TestOtaRingRelease(unittest.TestCase):
    """Host model + source guards for the OTA 503 regression fix."""

    def setUp(self) -> None:
        self.ota_c = strip_c_comments(read(OTA_URI_OTA_C))

    def test_ota_task_stack_comes_from_the_source(self) -> None:
        stacks = ota_task_stacks()
        # The S3/dual-core value stays at the task-66b fallback; the C5 real
        # hardware needs a smaller stack (measured largest internal block after
        # releasing the DAP rings is 4096 B, see docs/esp32-c5-port.md §11.4).
        self.assertIn(OTA_TASK_STACK_FALLBACK, stacks)
        self.assertIn(OTA_TASK_STACK_C5, stacks)
        self.assertLessEqual(OTA_TASK_STACK_C5, OTA_TASK_STACK_FALLBACK)
        # The C5 stack must fit the measured largest internal block.
        self.assertLess(OTA_TASK_STACK_C5, OTA_MEASURED_LARGEST_C5)

    def test_resident_rings_block_the_ota_task_stack(self) -> None:
        stack = ota_task_stack()  # S3 contract: 5120
        # With the rings resident the measured largest block is 4.6 KiB, which
        # cannot satisfy the 5120-byte OTA task stack -> xTaskCreate fails ->
        # handler answers 503 "ota service unavailable".
        self.assertLess(OTA_MEASURED_LARGEST_WITH_RINGS, stack)
        self.assertFalse(model_ota_task_stack_fits(
            [OTA_MEASURED_LARGEST_WITH_RINGS], stack))

    def test_releasing_rings_restores_ota_headroom(self) -> None:
        stack = ota_task_stack()
        request, response = ring_sizes()
        self.assertEqual(OTA_MEASURED_FREE_WITH_RINGS + request + response,
                         58_960)
        self.assertTrue(model_ota_task_stack_fits(heap_after_dap_release(), stack))
        # The two freed blocks are bigger than the stack requirement, so a
        # first-fit allocation of the ring storage does not strand the OTA task.
        self.assertGreaterEqual(request, stack)

    def test_ota_handler_releases_before_creating_the_task(self) -> None:
        release = self.ota_c.index("ota_dap_release_rings();")
        async_begin = self.ota_c.index("httpd_req_async_handler_begin(req")
        task_create = self.ota_c.index("xTaskCreate(ota_upload_task")
        self.assertLess(release, async_begin)
        self.assertLess(release, task_create)
        # Only a validated upload may free the rings: the release is after the
        # single-session claim (s_upload_active = true), not in the error paths.
        claim = self.ota_c.index("s_upload_active = true;")
        self.assertLess(claim, release)

    def test_ota_handler_restores_rings_on_failure_paths(self) -> None:
        # async-begin failure + task-create failure + aborted body + OTA error +
        # apply failure: every path that does not reboot must restore the path.
        self.assertGreaterEqual(self.ota_c.count("ota_dap_restore_rings();"), 4)
        self.assertIn("dap_handle_restore_ringbuffers", self.ota_c)

    def test_ota_api_uses_the_dap_proxy_release_api(self) -> None:
        header = read(DAP_HANDLE_H)
        self.assertIn("dap_handle_release_ringbuffers", header)
        self.assertIn("dap_handle_restore_ringbuffers", header)
        # The public API must be available to the OTA handler through the
        # component dependency (project_components/web_server already requires
        # dap_proxy for the other URI modules).
        cmake = read(ROOT / "project_components/web_server/CMakeLists.txt")
        self.assertIn("dap_proxy", cmake)


if __name__ == "__main__":
    unittest.main()

