#!/usr/bin/env python3
"""Source-level contract test for the M5 "all SWD rates on the dedicated-GPIO
engine" migration.

The C test (test_swd_ultra_plan.c) covers the pure plan/level arithmetic; this
script covers the *routing* contract that cannot be tested by compiling a
single header on the host:

  * SW_DP.c: on an ESP32-S3 engine build SWD_Transfer must go through
    SWD_UltraFast_EnsureConfigured()/SWD_Transfer_Ultra() with no fallback to
    the legacy GPIO transfer backend, and SWJ_Sequence / SWD_Sequence must use
    the engine sequence entry points (legacy code only in the #else branch).
  * SWD_Fast_GPIO.h: exposes the routing macro, plan API and sequence API.
  * Kconfig: BIRDIEBOT_SWD_ULTRA_FAST defaults to y, the deprecated
    BIRDIEBOT_SWD_ULTRA_MIN_CLOCK_HZ defaults to 0 (all rates) and
    BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON is kept.
  * DAP.h: the kTransfer_GPIO_ultra_fast enum value exists.
  * DAP.c (Lead-owned wiring): the engine is entered from DAP_SWJ_Clock for
    SWD.  The stricter M5 wiring (return DAP_ERROR instead of falling through
    to the legacy ladder) is reported as a warning while the M4 wiring is
    still in place, so this test is green for both revisions and tells the
    integrator exactly what is missing.

Exit code: 0 = all mandatory checks passed, 1 = a mandatory check failed,
2 = a required file is missing.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))

failures = 0
checks = 0
warnings = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL: {msg}")
    return cond


def warn(msg):
    global warnings
    warnings += 1
    print(f"  WARN: {msg}")


def read(rel, required=True):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        if required:
            print(f"  FAIL: missing file {rel}")
            global failures, checks
            failures += 1
            checks += 1
        return None
    with open(path, encoding="utf-8") as f:
        return f.read()


def section_between(text, start_marker, end_marker):
    start = text.find(start_marker)
    if start < 0:
        return None
    end = text.find(end_marker, start + len(start_marker))
    if end < 0:
        end = len(text)
    return text[start:end]


def test_sw_dp():
    print("[sw-dp]")
    src = read("components/DAP/cmsis-dap/source/SW_DP.c")
    if src is None:
        return
    # The engine header must be included.
    check('#include "SWD_Fast_GPIO.h"' in src, "SW_DP.c includes SWD_Fast_GPIO.h")

    # SWD_Transfer: S3 engine branch, no legacy fallback in it.
    dispatch = section_between(src, "uint8_t  SWD_Transfer(uint32_t request",
                               "#endif  /* (DAP_SWD != 0) */")
    check(dispatch is not None, "SWD_Transfer definition found")
    if dispatch:
        check("#if SWD_ULTRA_FAST_ENGINE_ENABLED" in dispatch,
              "SWD_Transfer has an SWD_ULTRA_FAST_ENGINE_ENABLED branch")
        check("SWD_UltraFast_EnsureConfigured()" in dispatch,
              "S3 SWD_Transfer lazy-configures the engine")
        check("SWD_Transfer_Ultra(request, data)" in dispatch,
              "S3 SWD_Transfer calls SWD_Transfer_Ultra")
        check("DAP_TRANSFER_ERROR" in dispatch,
              "S3 SWD_Transfer reports an error when the engine is unavailable")
        # On the S3 branch there must be no reference to the legacy dispatch
        # or to the legacy transfer functions.
        s3 = section_between(dispatch, "#if SWD_ULTRA_FAST_ENGINE_ENABLED",
                             "#else")
        check(s3 is not None, "S3 branch delimited by #else")
        if s3:
            for forbidden in ("SWD_Transfer_GPIO", "SWD_Transfer_SPI",
                              "kTransfer_GPIO_ultra_fast", "kTransfer_GPIO_fast"):
                check(forbidden not in s3,
                      f"S3 SWD_Transfer branch has no legacy reference '{forbidden}'")

    # Legacy transfer backends compiled out on S3.
    check("#if !SWD_ULTRA_FAST_ENGINE_ENABLED" in src,
          "legacy SWD transfer backends are behind #if !SWD_ULTRA_FAST_ENGINE_ENABLED")
    check("SWD_Transfer_GPIO (uint32_t request" in src,
          "legacy GPIO transfer implementation still present (non-S3 targets)")

    # SWJ_Sequence routed to the engine in SWD mode.
    swj = section_between(src, "void SWJ_Sequence (uint32_t count",
                          "void IRAM_ATTR SWJ_Sequence_GPIO")
    check(swj is not None, "SWJ_Sequence definition found")
    if swj:
        check("SWD_UltraFast_SequenceWrite(count, data)" in swj,
              "S3 SWJ_Sequence uses SWD_UltraFast_SequenceWrite")
        check("DAP_Data.debug_port != DAP_PORT_JTAG" in swj,
              "SWJ_Sequence keeps the legacy path for JTAG")

    # SWD_Sequence routed to the engine.
    swdseq = section_between(src, "void SWD_Sequence (uint32_t info",
                             "void IRAM_ATTR SWD_Sequence_GPIO")
    check(swdseq is not None, "SWD_Sequence definition found")
    if swdseq:
        check("SWD_UltraFast_SequenceRead" in swdseq,
              "S3 SWD_Sequence DIN uses SWD_UltraFast_SequenceRead")
        check("SWD_UltraFast_SequenceWrite" in swdseq,
              "S3 SWD_Sequence write uses SWD_UltraFast_SequenceWrite")


def test_header():
    print("[header]")
    hdr = read("components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h")
    if hdr is None:
        return
    for token in ("SWD_ULTRA_FAST_ENGINE_ENABLED", "SWD_Ultra_Plan",
                  "SWD_UltraFast_EnsureConfigured",
                  "SWD_UltraFast_SequenceWrite", "SWD_UltraFast_SequenceRead",
                  "SWD_Transfer_Ultra", "SWD_Ultra_SequenceHalf"):
        check(token in hdr, f"header exposes {token}")


def test_kconfig():
    print("[kconfig]")
    cfg = read("components/DAP/Kconfig")
    if cfg is None:
        return
    block = section_between(cfg, "config BIRDIEBOT_SWD_ULTRA_FAST",
                            "config BIRDIEBOT_SWD_ULTRA_MIN_CLOCK_HZ")
    check(block is not None, "BIRDIEBOT_SWD_ULTRA_FAST block found")
    if block:
        check(re.search(r"default y", block) is not None,
              "BIRDIEBOT_SWD_ULTRA_FAST defaults to y")
        check("IDF_TARGET_ESP32S3" in block,
              "BIRDIEBOT_SWD_ULTRA_FAST depends on the ESP32-S3 target")
    min_block = section_between(cfg, "config BIRDIEBOT_SWD_ULTRA_MIN_CLOCK_HZ",
                                "config BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON")
    check(min_block is not None, "MIN_CLOCK option kept (deprecated)")
    if min_block:
        check(re.search(r"default 0", min_block) is not None,
              "MIN_CLOCK defaults to 0 = all rates go to the engine")
    check("config BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON" in cfg,
          "OEN_STAY_ON escape hatch kept")


def test_dap_h_enum():
    print("[dap-h]")
    dap_h = read("components/DAP/cmsis-dap/include/DAP.h")
    if dap_h is None:
        return
    check("kTransfer_GPIO_ultra_fast" in dap_h,
          "DAP.h has the kTransfer_GPIO_ultra_fast enum value")


def test_dap_c_wiring():
    print("[dap-c-wiring]")
    src = read("components/DAP/cmsis-dap/source/DAP.c")
    if src is None:
        return
    block = section_between(src, "static uint32_t DAP_SWJ_Clock",
                            "// clock >= 10MHz")
    check(block is not None, "DAP_SWJ_Clock found")
    if block:
        check("SWD_UltraFast_Enter" in block,
              "DAP_SWJ_Clock enters the engine")
        check("DAP_SPI_Deinit()" in block,
              "DAP_SWJ_Clock maps the pads to the dedicated GPIO before Enter")
        # M5 wiring: no threshold, error instead of legacy fallback.
        if "CONFIG_BIRDIEBOT_SWD_ULTRA_MIN_CLOCK_HZ" in block:
            warn("DAP.c still gates the engine on MIN_CLOCK_HZ "
                 "(M4 wiring); M5 patch: remove the threshold and return "
                 "DAP_ERROR instead of falling through - docs/swd-23mhz-engine.md §4")
        if "DAP_ERROR" not in block:
            warn("DAP.c falls through to the legacy ladder when the engine "
                 "cannot be entered; M5 patch: set *response = DAP_ERROR and "
                 "return - docs/swd-23mhz-engine.md §4")


def main():
    print(f"SWD engine M5 routing contract test (root: {ROOT})")
    test_sw_dp()
    test_header()
    test_kconfig()
    test_dap_h_enum()
    test_dap_c_wiring()
    print(f"\n== {checks} checks, {failures} failed, {warnings} warnings ==")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
