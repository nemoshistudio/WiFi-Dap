# ESP32-S3 dedicated-GPIO SWD engine — host tests (M5)

Host-only tests for the M5 "all SWD rates on the dedicated-GPIO engine" change
(no ESP-IDF, Docker, network or hardware needed):

1. **`test_swd_ultra_plan.c`** — pure logic of
   `components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h`:
   - SWD request header byte construction (`start/APnDP/RnW/A2/A3/parity/stop/park`),
     checked against the documented request bytes (`0xA5` IDCODE, `0xA9` W CTRL/STAT,
     `0xBD` RDBUFF, …);
   - even parity (4-bit and 32-bit, no libcall), ACK decoding;
   - the fast level tables (160 MHz: 20/16/13.33/11.43/10/8/6.67/5/4 MHz,
     240 MHz: 24/20/17.1/15/12/10/7.5/6 MHz) and the `SWD_Ultra_Plan()` rules:
     **never faster than the host request**, error within 0 .. -25 %, monotone,
     paced formula `half = ceil(cpu / (2 * request))`, coverage swept over
     100 kHz .. 24 MHz;
   - the per-transfer wire-clock budget used in the curve-sampling estimate.

2. **`test_swd_ultra_routing.py`** — source-level routing contract:
   - `SW_DP.c`: S3 `SWD_Transfer` goes through
     `SWD_UltraFast_EnsureConfigured()` / `SWD_Transfer_Ultra()` and reports
     `DAP_TRANSFER_ERROR` when the engine is unavailable (no fallback to the
     legacy GPIO transfer backend, which is compiled out for the S3);
     `SWJ_Sequence` / `SWD_Sequence` use the engine sequence entry points and
     keep the legacy path for JTAG;
   - `SWD_Fast_GPIO.h`: routing macro, plan API and sequence API;
   - Kconfig defaults: engine `y`, deprecated `MIN_CLOCK=0`, `OEN_STAY_ON` kept;
   - `DAP.h` enum; `DAP.c` wiring status (a WARN, not a failure, while the
     M4 wiring is still in place — the M5 patch is in docs/swd-23mhz-engine.md §4).

The Xtensa assembly itself cannot run on the host. It is verified by:

1. compiling the real `SWD_Fast_GPIO.c` with the `xtensa-esp32s3-elf`
   toolchain (the ESP-IDF Docker build does this for real), and
2. the HIL checklist in docs/swd-23mhz-engine.md §6.

## Run

```bash
components/DAP/test/host/run.sh              # ASan/UBSan when available
CC=clang components/DAP/test/host/run.sh
SANITIZE=0 components/DAP/test/host/run.sh   # plain build
```

Expected tail: `== 11786 checks passed, 0 failed ==` (C) and
`== 36 checks, 0 failed, N warnings ==` (routing), exit code 0.
