# WiFi Dap - curve page device-local sampler (Phase 0/1/2)

Firmware half of `docs/curve-local-sampler.md`: the Web curve page sends its
`AddressExpr` series configuration over HTTP, this component samples the STM32
over the **local SWD link** on a dedicated task and the page polls binary CVF1
frame batches.  The per-sample WiFi/WebSocket round-trip leaves the critical
path (`Web -> ESP32 -> STM32` becomes one local SWD read burst per tick).

## HTTP endpoints (same origin, port 80)

All JSON responses contain `{"ok":true|false}`; failures add `"err":"..."`.
All endpoints pass the normal `web_host_guard` browser safety policy.

| Method | URI | Notes |
| --- | --- | --- |
| POST | `/api/curve/selftest` | Phase 0 diagnostic, optional `{}` body |
| POST | `/api/curve/start` | JSON body <= 4096 B, 1..16 series |
| POST | `/api/curve/stop` | idempotent, optional body |
| GET | `/api/curve/status` | sampler status JSON |
| GET | `/api/curve/frames?since=&max=` | binary CVF1, `application/octet-stream` |

`POST /api/curve/start` body (schema identical to the Web `AddressExpr`):

```json
{"trigger":"periodic",
 "interval_us":20000,"halt":true,
 "series":[
   {"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}},
   {"id":1,"size":4,"expr":{"kind":"deref","inner":{"kind":"addr","address":603991984},"offset":220}}]}
```

Phase 2 breakpoint trigger (FPB hardware breakpoint, see below):

```json
{"trigger":"breakpoint","breakpoint_address":134356037,"interval_us":20000,
 "series":[{"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}}]}
```

Limits enforced by the parser (`src/curve_json.c`): `trigger` is `periodic`
(default) or `breakpoint`; `interval_us` is 5000..200000 for periodic and the
throttle floor 5000..1000000 for breakpoint (default 20000); `series` 1..16,
`size` 1..8, `sum(size)` <= 256, expression depth <= 8; expression kinds
`addr`/`reg`/`cfa`/`add`/`deref`.  `halt=false` rejects `reg`/`cfa`
expressions in periodic mode (no register can be read while the core runs);
breakpoint mode ignores `halt` because the FPB hit already halted the core.
`breakpoint_address` must be a Thumb instruction address in target flash
(0x08000000..0x08FFFFFF, bit0 ignored); anything else returns HTTP 400
`{"ok":false,"err":"bad breakpoint address"}`.

`GET /api/curve/frames` returns HTTP 204 when there is no frame newer than
`since`.  `since` is the last `seq` received (0 = oldest retained frame), `max`
1..64 (default 32).

Ring: dynamically 256..512 frames, clamped to a ~32 KiB budget
(`capacity = clamp(32768 / (12 + payload_bytes), 256, 512)`: 16 B payload -> 512
frames, 128 B payload -> 256 frames).  `overwritten` counts **frames that were
evicted before any `frames?since=...` response delivered them** (a served
cursor advances on every non-204 batch, including `since=0`); a client that
keeps polling at the sampling rate therefore stays at `overwritten=0` even
though the ring physically recycles slots every tick.  `gap` (CVF1
`flags.bit0`) keeps its meaning: `since` is older than the oldest retained
frame, so the batch starts at the oldest retained frame.

`GET /api/curve/status` also exposes the diagnostics `ring_frames` (capacity)
and `served_seq` (highest seq handed to a client in this session), plus the
Phase 2 fields `trigger` (`"periodic"`/`"breakpoint"`), `breakpoint_address`,
`hits`, `sampled`, `throttled`.

### curl examples

```sh
# Phase 0: real SWD timing (halt -> r13/r7/r15 -> 2 memory words -> resume, N=20)
curl -s -X POST http://<device-ip>/api/curve/selftest -d '{}'

# fixed address (e.g. 0x24000000) sampling at 100 Hz
curl -s -X POST http://<device-ip>/api/curve/start \
     -H 'Content-Type: application/json' \
     -d '{"interval_us":10000,"halt":true,"series":[{"id":0,"size":4,"expr":{"kind":"addr","address":603979776}}]}'
curl -s http://<device-ip>/api/curve/status
# binary batch -> decode CVF1 (save and inspect with `xxd` / the Web decoder)
curl -s -o frames.bin 'http://<device-ip>/api/curve/frames?since=0&max=32'
curl -s -X POST http://<device-ip>/api/curve/stop -d '{}'
```

## Phase 2: breakpoint trigger (FPB)

`trigger=breakpoint` turns the session into an event-driven sampler:

1. Bring-up + DAP hand-off are identical to Phase 1; the target is first made
   to run (resumed if a previous session left it halted).
2. The sampler saves `FP_CTRL`/`FP_COMP0` (Cortex-M7 FPB at 0xE0002000) and
   arms comparator 0 with `FP_COMP0 = (address & ~1) | 1`
   (`FP_CTRL = KEY | ENABLE`).  Only one comparator is used; no DWT dependency.
3. The worker task polls `DHCSR.S_HALT` over local SWD every ~1 ms (esp_timer
   one-shot + task notification; the httpd task is never involved).  On a hit
   the target is already halted by the FPB, so the frame is read directly:
   `hits++`, read required core registers / evaluate expressions / read target
   memory / append a CVF1 frame (`sampled++`), with the timestamp taken at hit
   detection.
4. Before resuming, comparator 0 is disabled and the FPB unit is gated off
   (`FP_CTRL = KEY`); the target is resumed and verified running
   (`DHCSR.S_HALT` cleared).  After the `interval_us` throttle wait the
   comparator is re-armed (`FP_CTRL = KEY|ENABLE`, `FP_COMP0 = compare`).
5. `stop`/error: restore the original `FP_CTRL`/`FP_COMP0`, make sure the core
   runs, release the DAP (`RESET_HANDLE`).  `throttled` is reserved and stays 0
   in this implementation (every hit is sampled; the throttle only limits how
   often the comparator is armed).

Breakpoint limitations:

* If the breakpoint is placed at a function entry, its locals may not be
  initialized yet; put the address on an instruction after initialization
  (the Web UI allows entering the Thumb address manually / from disassembly).
* FPB resources are shared with other debug probes/RTOS: while a local
  breakpoint session owns the DAP no debugger may attach, and `FP_COMP0` is
  saved/restored to avoid clobbering another tool's comparator.
* Polling detects the halt within about 1 ms; the effective sample rate is
  limited by the throttled function call rate, the 1 ms poll interval and the
  `interval_us` floor.
* `hits` counts observed `S_HALT` events; a non-breakpoint halt (e.g. target
  lockup) would also be sampled.  `sampled` counts frames written to the ring.

## Implementation notes

* `src/curve_http.c` - registers **two independent URI modules** through
  `WEB_URI_MODULE_REGISTER` (one POST, one GET) on the `"/api/curve/"` prefix.
  The project web server has one handler slot per module and its custom
  `uri_match()` only supports exact matches or a trailing-`/` prefix, so the
  method is used to split the dispatch.  Both come before the `/` SPA fallback
  (priority 0x72/0x73 vs 0x90).  The generic `/api` handler and its 2 KiB
  memory pool are untouched; request bodies use a dedicated 4 KiB static
  buffer on the (single) httpd task.
* `src/curve_status.c` - pure-C `GET /api/curve/status` JSON formatter (host
  tested together with the ring/config code).
* `src/curve_ring.c` - pure-C ring bookkeeping (capacity clamp, write
  slot/seq, overwritten accounting against the served cursor, read planning,
  gap flag).  No RTOS/ESP dependencies, exercised by the host regression test.
* `src/curve_engine.c` - owns the DAP for one session:
  `tcp_server_kick_session()` -> 50 ms settle ->
  `dap_handle_request_reset(DELETE_HANDLE)` + `dap_handle_wait_reset(3000)`
  (the `wt_target_flash.c` hand-off), then `tf_swd_link_init()` retried at
  2 MHz / 1 MHz / 500 kHz.  A dedicated worker task (core 1 = DAP/SWD core,
  8 KiB stack, priority 4) runs the sample loop; an `esp_timer` periodic timer
  (5..200 ms) is the tick source so no httpd time is used.
* Sample tick: `[halt]` -> read only the core registers the expressions need
  (`tf_swd_read_reg`, DCRSR REGWnR=0 + S_REGRDY poll) -> evaluate each
  expression -> AHB-AP reads of the series bytes -> `[resume]` -> one CVF1
  frame (4 B seq, 8 B `esp_timer_get_time()` timestamp, raw payload bytes in
  series order).  The per-tick resume is a raw `DHCSR = DBGKEY|C_DEBUGEN`
  write: `tf_swd_resume()`'s 10 ms settle delay cannot fit a 5-10 ms interval.
* Safety semantics: addresses are only read inside DTCM `0x20000000..0x2001FFFF`,
  AXI SRAM `0x24000000..0x2407FFFF`, flash `0x08000000..0x08FFFFFF` (checked on
  the aligned word span the block read touches, including `deref` pointer
  loads); everything else zero-fills that series and increments `invalid`
  without any bus access.  An SWD/AP error runs `tf_swd_recover_bus()`; 5
  consecutive failed ticks stop the session.  `stop` and every error path
  resume the core (retry after bus recovery) and release the DAP with
  `RESET_HANDLE`, restoring the USBIP/WS data path.
* `components/target_flash/src/tf_swd.c` gained `tf_swd_read_reg()` (DCRSR read
  direction REGWnR=0, wait S_REGRDY, read DCRDR); everything else reuses the
  existing local SWD layer.

## Tests / build

* ESP-IDF v5.2.1 Docker build (`scripts/build_all.sh -t esp32s3`) - see the
  task report for the exact command, binary size and remaining unverified
  items (real-target timing needs the device).
* `src/curve_json.c` + `src/curve_expr.c` + `src/curve_ring.c` were additionally
  regression-tested on the host (`test/host/run.sh`, no hardware needed):
  contract example, all limits, depth 8/9, address window boundaries,
  `addr`/`reg`/`cfa`/`add`/`deref` evaluation, pointer-read filtering, ring
  capacity clamp and the `overwritten`/`gap` semantics (prompt consumer ->
  `overwritten=0` after many ring wraps; stalled consumer -> real losses
  counted once; served-then-evicted frames are not counted), Phase 2 trigger
  config/limits, breakpoint address window validation and the status JSON
  field set.

## Known limitations / behaviour to be aware of

* Breakpoint mode requires the breakpoint address to be in flash; RAM
  breakpoints (needing FPB literal comparators / DWT) are not implemented.
* The session bring-up reuses `tf_swd_link_init()`, which pulses nRESET before
  the SWD switch sequence (same as the target-flash probe).  **Starting a local
  sampling session therefore resets the target once**; the sampler then keeps
  it running between ticks.
* Cross-component DAP exclusion is best-effort: `start`/`selftest` refuse to
  run while a target-flash session is active and the sample loop polls
  `wt_tf_get_status()` and stops if one appears.  The reverse direction (a
  flash begin while curve sampling is active) is not blocked by
  `target_flash` because that component was out of this task's write scope -
  the Web UI must not run target flash and local curve sampling at the same
  time.
* While local sampling owns the DAP, the Web UI must not open a WebSocket-DAP
  session; the sampler only kicks sessions that already exist when it starts.
* `overwritten` is a single-consumer metric: the served cursor belongs to the
  most recently polled client.  Concurrent clients with independent `since`
  values would share it (the Web curve page is single-client).
* `frames`/`overwritten`/... are per session; `seq` restarts at 1 on every
  `start`, so a client must reset `since` to 0 when it (re)starts sampling.
* HTTP 204/`since` handling assumes a monotonic client `since`; after a
  session restart the retained ring is reset and stale `since` values simply
  return no data (204) until a new session's `seq` passes them.
* The doc's start-body example uses `"size":12`, which contradicts the
  explicit `size` 1..8 limit (and the Web `SampleLeaf` 1..8); the parser
  follows the explicit limit.
