# DAP command-layer host regression harness (N1 / N6 / N9)

Host-only regression for `components/DAP/cmsis-dap/source/DAP.c`: the file is
compiled on a PC against the minimal `DAP_config.h` stub and FreeRTOS shims in
this directory, with **ASan + UBSan**. No ESP-IDF, Docker, network or hardware
is needed.

## Provenance

- Written by dap-dev for the Wave-3 security review (`docs/security-review-dap.md`):
  N1 (P0, unbounded 0x7E ↔ 0x7F mutual recursion), N6 (P2, `SWJ_Pins` wait
  unit), N9 (P2/P3, `TransferBlock` write-direction 1-byte OOB read).
- Original location: `/tmp/dap_host` (ephemeral; harness verified 36/36 PASS at
  the time, **51/51** after the 0x7F/0x7E batch cases were added). Pre-fix
  `DAP.c` reproduced N1 as `AddressSanitizer: stack-overflow` and N9 as
  `heap-buffer-overflow READ of size 1`.
- Fix commits: `666df38` (N1/N9/N6), merged by `89bfd0d` (`feature/birdiebot-v1`).
- Integrated into `tests/host/dap/` by WP-302 (2026-10-20). The test code is
  intentionally unchanged; only this provenance/README and build paths were
  adapted, so the harness always compiles against the **current** repo DAP.c.

## Run

```bash
tests/host/dap/run.sh                # ASan+UBSan (default)
SANITIZE=0 tests/host/dap/run.sh     # plain build
scripts/test_host.sh                 # full host suite (includes this harness)
```

Expected tail:

```
== 51 checks passed, 0 failed ==
```

## Coverage

| Test | Contract / regression |
|---|---|
| `test_n1_top_level_0x7e` | top-level 0x7E is a queue-less alias of 0x7F (echo 0x7E, execute batch) |
| `test_n1_nested_execute_rejected` | nested 0x7E/0x7F sub-commands inside a batch return `ID_DAP_Invalid` (no recursion) |
| `test_n1_execute_entry_0x7e` | `DAP_ExecuteCommand(0x7E)` accepted at entry (N1 fix) |
| `test_n9_*` (6 cases) | `TransferBlock` direction-aware caps: read 127 / write 126; 127 write and 128 read rejected; batch (0x7F) and 0x7E-alias paths |
| `test_n6_swj_pins_wait_microseconds` | `SWJ_Pins` wait is µs (100 µs ≈ 100 ticks) and 3 s clamp still holds (N6 conclusion: previous conversion was a false positive) |

The Python `tests/host/test_security_regressions.py` adds source-level guards
(N2/N8/N9 fix markers) and models the N8 short-OUT tail clearing; the C harness
here is the executable ASan proof for N1/N9.
