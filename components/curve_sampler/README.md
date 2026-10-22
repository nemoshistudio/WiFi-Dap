# WiFi Dap - curve page device-local sampler (Phase 0/1/2/2.5, protocol v2)

Firmware half of `docs/curve-local-sampler.md`: the Web curve page sends its
`AddressExpr` series configuration over HTTP, this component samples the STM32
over the **local SWD link** on a dedicated task and the page polls binary CVF2
frame batches (multi-group round-robin + loclist variants).  The per-sample WiFi/WebSocket round-trip leaves the critical
path (`Web -> ESP32 -> STM32` becomes one local SWD read burst per tick).

## HTTP endpoints (same origin, port 80)

All JSON responses contain `{"ok":true|false}`; failures add `"err":"..."`.
All endpoints pass the normal `web_host_guard` browser safety policy.

| Method | URI | Notes |
| --- | --- | --- |
| POST | `/api/curve/selftest` | Phase 0 diagnostic, optional `{}` body |
| POST | `/api/curve/start` | JSON body <= 4096 B, 1..8 groups x 1..32 series |
| POST | `/api/curve/stop` | idempotent, optional body |
| GET | `/api/curve/status` | sampler status JSON |
| GET | `/api/curve/frames?since=&max=` | binary CVF2, `application/octet-stream` |

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

Protocol v2 multi-group / loclist variants (see "Protocol v2" below):

```json
{"interval_us":20000,"halt":true,
 "groups":[
   {"id":0,"series":[
     {"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}},
     {"id":1,"size":4,"variants":[
       {"pc_low":134386244,"pc_high":134386692,"expr":{"kind":"reg","reg":13,"offset":48}},
       {"pc_low":134386692,"pc_high":134387000,"expr":{"kind":"reg","reg":7,"offset":-32}}]}]},
   {"id":1,"series":[...]}]}
```

Limits enforced by the parser (`src/curve_json.c`): `trigger` is `periodic`
(default) or `breakpoint`; `interval_us` is 5000..200000 for periodic and the
throttle floor 5000..1000000 for breakpoint (default 20000); `groups` 1..8
(legacy root `series` = one group), 1..32 series per group, 256 series total,
`size` 1..8, per-group `sum(size)` <= 256, expression depth <= 8, `variants`
1..16 per series; expression kinds
`addr`/`reg`/`cfa`/`add`/`deref`.  `halt=false` rejects `reg`/`cfa`
expressions in periodic mode (no register can be read while the core runs);
breakpoint mode ignores `halt` because the FPB hit already halted the core.
`breakpoint_address` must be a Thumb instruction address in target flash
(0x08000000..0x08FFFFFF, bit0 ignored); anything else returns HTTP 400
`{"ok":false,"err":"bad breakpoint address"}`.

`GET /api/curve/frames` returns HTTP 204 when there is no frame newer than
`since`.  `since` is the last `seq` received (0 = oldest retained frame), `max`
1..64 (default 32).  The response is **CVF2**: 16 B header (`"CVF2"`,
version=2, flags bit0=gap, count u16, first_seq u32, frame_bytes u16 = max group
payload, reserved) and variable-length frames (`seq u32`, `timestamp_us u64`,
`group u8`, `payload_len u16`, payload bytes).  CVF1 is no longer produced.

Ring: dynamically 128..512 frames, clamped to a ~32 KiB budget
(`capacity = clamp(32768 / (15 + frame_bytes), 128, 512)`: 16 B payload -> 512
frames, 32 B -> 512, 128 B -> 229, 256 B (one full group) -> 128 frames /
~34 KiB heap).  This is the *ESP32* localStorage ring; the target-side CVF2
`frame_bytes` is the max group payload, and per-frame `payload_len` can be
smaller.  The 128-frame floor keeps the worst-case allocation bounded while
still covering >2.5 s of history at 50 Hz (`ring_frames` in status reports the
actual capacity).  The 128-frame floor keeps the worst-case allocation bounded while still
covering >2.5 s of history at 50 Hz (`ring_frames` in status reports the
actual capacity).  `overwritten` counts **frames that were
evicted before any `frames?since=...` response delivered them** (a served
cursor advances on every non-204 batch, including `since=0`); a client that
keeps polling at the sampling rate therefore stays at `overwritten=0` even
though the ring physically recycles slots every tick.  `gap` (CVF2
`flags.bit0`) keeps its meaning: `since` is older than the oldest retained
frame, so the batch starts at the oldest retained frame.

`GET /api/curve/status` also exposes the diagnostics `ring_frames` (capacity)
and `served_seq` (highest seq handed to a client in this session), plus the
Phase 2 fields `trigger` (`"periodic"`/`"breakpoint"`), `breakpoint_address`,
`hits`, `sampled`, `throttled`, the Phase 2.5 field `out_of_scope` (number of
PC-gated/variant series values skipped because the halted core's PC did not
match), and the protocol v2 fields `group_count`/`current_group`.  The start
response adds `group_count`, `series_count` (total) and `sample_bytes` (max
group payload).

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

## Phase 2.5: PC scope gating (stack-frame variables)

A stack-frame local read while the sampled function is *not* on the current
call stack returns unrelated stack data.  Series entries may carry an optional
function PC range:

```json
{"id":0,"size":4,"pc_low":134348800,"pc_high":134352896,
 "expr":{"kind":"reg","reg":13,"offset":244}}
```

* `pc_low`/`pc_high` are optional; the gate is enabled only when both are
  present, both are target-flash addresses (0x08000000..0x08FFFFFF) and
  `pc_low < pc_high`.  Missing/zero/invalid pairs leave the series ungated
  (the field is backward compatible: 16-series clients without it are
  unaffected).
* The sampler reads r15 **once per tick** with the existing DCRSR path:
  after the per-tick halt in periodic mode, and at the FPB hit in breakpoint
  mode.  A series is sampled only when `(pc & ~1)` is inside the normalized
  `[pc_low & ~1, pc_high & ~1)` range; otherwise its payload bytes stay 0 and
  the cumulative `out_of_scope` status counter (not `invalid`) is incremented.
  The session keeps running.
* Both range bounds are normalized (Thumb bit0 cleared) at parse time and the
  PC is normalized at runtime, so either Thumb-bit convention from the
  ELF/DWARF side works (DWARF addresses are usually even; ELF symbols may set
  bit0).
* `out_of_scope` and `invalid` are distinct: `invalid` still counts
  address/range/read failures and PC-read failures; `out_of_scope` only counts
  "function not on stack" skips.
* PC gating needs the core halted to read r15: periodic mode requires
  `halt=true` for gated data (in no-halt mode gated series are always
  out_of_scope, never garbage); breakpoint mode is always halted at the hit.
* The 1 ms no-halt floor and the periodic/breakpoint/frames/overwritten
  semantics are unchanged.

## Protocol v2: multi-group round-robin + loclist variants

* **Groups**: up to 8 groups x 32 series (<=256 total).  Each tick samples only
  the current group, then `current_group = (current_group + 1) % group_count`
  round-robins.  `interval_us` is the *per-frame* period, so each group's
  refresh rate is `1 / (group_count * interval_us)` (the Web UI shows group
  refresh rate).  Legacy requests with a root `series` array are parsed as a
  single group; `groups` takes precedence if both are present.
* **loclist variants**: a series may carry `variants` (1..16 entries, each
  `{pc_low, pc_high, expr}`).  The sampler reads r15 once per tick (as with the
  single-style PC gate) and picks the first variant whose half-open PC range
  contains the PC; no match -> series zeroed + `out_of_scope++`.  Variants
  require the core's PC to be readable: periodic mode must use `halt=true`
  (else HTTP 400 "variants require halt=true"); breakpoint mode is fine because
  the FPB hit already halted the core.  A plain `expr` + single `pc_low/pc_high`
  gate keeps working as before; a series needs `expr` or `variants`.
* **CVF2**: per-frame `group`/`payload_len` allow different group payload sizes;
  the ring slots are `max(group sample_bytes)`.  `served_seq`/`overwritten`/`gap`
  semantics are unchanged (one ring for all groups).
* **Static budget**: the HTTP layer owns one static `curve_config_t`
  (~17.2 KiB: 8 groups, 256 series, 256 loclist variants, 768 expression nodes)
  and the engine only keeps a const pointer while running; the parser refuses
  to overwrite it until the engine is idle (start returns 409 busy).  The 4 KiB
  request-body limit bounds real Web requests well below those pool caps; a
  request that exhausts the variant/expr pool is rejected with a clear error.
  Logical limits reported by the parser: groups 1..8, series/group 1..32, total
  series <=256, variants/series 1..16, total variants <=256, expression nodes
  <=768 (each node 12 B).

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
* `src/curve_cvf2.c` - pure-C CVF2 batch-header / frame-metadata writer (host
  tested byte-for-byte).
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
  config/limits, breakpoint address window validation, Phase 2.5 PC-scope
  parsing/normalization/scope test, 32-series config/pool/ring boundaries, the
  status JSON field set, and the protocol v2 model (groups parsing/boundaries
  8x32=256 series, variant selection + normalisation, variant/expression pool
  caps, CVF2 header/frame byte layout, group round-robin wrap, legacy
  single-group compatibility and the static config-size budget).

## Known limitations / behaviour to be aware of

* Breakpoint mode requires the breakpoint address to be in flash; RAM
  breakpoints (needing FPB literal comparators / DWT) are not implemented.
* PC-gated series in periodic no-halt mode cannot be checked (r15 is only
  readable in debug state); they are reported as out_of_scope instead of
  reading potentially stale stack data.  Use periodic halt or breakpoint mode
  for stack-frame variables.
* Protocol v2 emits CVF2 only; a Web client must decode v1 and v2 if it also
  talks to older firmware.
* The static config is a single instance: only one sampler session at a time
  (as before), and a new start request while running returns 409 busy before
  the request body is parsed.
* Very large logical combinations (e.g. hundreds of deep variant expressions)
  are bounded by the 256-variant / 768-expression-node pools; the parser
  rejects them explicitly.  HTTP request bodies are capped at 4 KiB, so real
  Web requests stay far below those caps.
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
