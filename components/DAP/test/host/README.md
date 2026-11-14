# ESP32-S3 ultra-fast SWD engine — host test

Host-only unit test for the pure (no ESP-IDF) logic of
`components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h`:

- SWD request header byte construction (`start/APnDP/RnW/A2/A3/parity/stop/park`),
  checked against the documented request bytes (`0xA5` IDCODE, `0xA9` W CTRL/STAT,
  `0xBD` RDBUFF, …);
- even parity (4-bit and 32-bit, no libcall);
- ACK bit decoding;
- the timing level tables and `SWD_Ultra_PickLevel()` safety rules documented in
  [docs/swd-23mhz-engine.md](../../../../docs/swd-23mhz-engine.md) §2:
  half periods respect the 20 ns floor, the 160 MHz build is 20/16/13.33/10 MHz,
  the 240 MHz build is 24/20/15/12 MHz, and the engine never runs faster than
  the host's adapter-speed request;
- the per-transfer wire-clock budget used in the curve-sampling estimate.

The Xtensa assembly itself cannot run on the host. It is verified by:

1. compiling the real `SWD_Fast_GPIO.c` with the `xtensa-esp32s3-elf`
   toolchain (the ESP-IDF Docker build does this for real), and
2. the HIL checklist (logic analyzer + OpenOCD) in the doc above.

## Run

```bash
components/DAP/test/host/run.sh              # ASan/UBSan when available
CC=clang components/DAP/test/host/run.sh
SANITIZE=0 components/DAP/test/host/run.sh   # plain build
```

Expected tail: `== 574 checks passed, 0 failed ==`, exit code 0.
No ESP-IDF, Docker, network or hardware is needed.
