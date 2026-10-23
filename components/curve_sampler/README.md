# WiFi Dap - curve page device-local sampler (Phase 0/1/2/2.5 + object watch/watchpoint/condition trigger, protocol v2)

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
| POST | `/api/curve/start` | JSON body <= 16384 B, 1..8 groups x 1..64 series (<=256 total; typical use: tens) |
| POST | `/api/curve/stop` | idempotent, optional body |
| GET | `/api/curve/status` | sampler status JSON |
| GET | `/api/curve/frames?since=&max=` | binary CVF2, `application/octet-stream` |
| GET | `/ws/curve` | WebSocket push subscription (CVF2 batches, P1.11) |

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

P1.8 conditional trigger can be combined with any trigger mode:

```json
{"interval_us":20000,"halt":true,
 "trigger_condition":{"address":536870912,"size":4,"op":"gt","value":100,
                      "signed":false,"mode":"once","stop_after":true},
 "series":[{"id":0,"size":4,"expr":{"kind":"addr","address":536870916}}]}
```

P1.8/P1.9 DWT data watchpoint trigger (core halts itself on a variable write or
read+write):

```json
{"trigger":"watchpoint","watch_address":536870912,"watch_size":4,
 "watch_access":"write","interval_us":20000,
 "series":[{"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}}]}
```

Limits enforced by the parser (`src/curve_json.c`): `trigger` is `periodic`
(default), `breakpoint` (FPB) or `watchpoint` (DWT); `interval_us` is
5000..200000 for periodic and the throttle floor 5000..1000000 for
breakpoint/watchpoint (default 20000); `groups` 1..8
(legacy root `series` = one group), 1..64 series per group, 512 series total,
`size` 1..8, per-group `sum(size)` <= 256, expression depth <= 8, `variants`
1..16 per series; expression kinds
`addr`/`reg`/`cfa`/`add`/`deref`.  `halt=false` rejects `reg`/`cfa`
expressions in periodic mode (no register can be read while the core runs);
breakpoint mode ignores `halt` because the FPB hit already halted the core.
`want_pc` (optional bool, default true) controls the CVF2 PC field; a request
may disable it.  Optional `trigger_condition` (P1.8, works on top of every
trigger mode): `address` (u32, required, inside an allowed target memory
window), `size` (1/2/4, default 4), `op` (`eq`/`ne`/`lt`/`le`/`gt`/`ge`, also
`==`/`!=`/`<`/`<=`/`>`/`>=`), `value` (int32, required), `signed` (bool, default
false), `mode` (`once` default or `continuous`) and `stop_after` (bool, default
false).  Invalid objects/fields return HTTP 400.  For `trigger=watchpoint`: `watch_address` (required, must be
naturally aligned to the size and inside an allowed target memory window),
`watch_size` (1/2/4, default 4) and `watch_access` (`"write"` default or
`"readwrite"`) select the DWT comparator; `halt` is ignored because the DWT hit
already halted the core.  `breakpoint_address` must be a Thumb instruction address in target flash
(0x08000000..0x08FFFFFF, bit0 ignored); anything else returns HTTP 400
`{"ok":false,"err":"bad breakpoint address"}`.

`GET /api/curve/frames` returns HTTP 204 when there is no frame newer than
`since`.  `since` is the last `seq` received (0 = oldest retained frame), `max`
1..64 (default 32).  The response is **CVF2**: 16 B header (`"CVF2"`,
version=2, flags bit0=gap, count u16, first_seq u32, frame_bytes u16 = max group
payload, reserved) and variable-length frames (`seq u32`, `timestamp_us u64`,
`group u8`, `payload_len u16`, payload bytes).  CVF1 is no longer produced.

**PC extension (CVF2 flags bit1 = PC_PRESENT)**: when the session can read r15
(periodic `halt=true`, or breakpoint mode) every frame carries the current PC
after `payload_len`: frame metadata becomes 19 B (`... payload_len u16, pc u32`,
PC with Thumb bit0 cleared).  No-halt periodic sessions cannot read DCRSR, so
they leave bit1 clear and keep the 15 B metadata.  The batch header size and
layout are unchanged; decoders **must** drive the frame layout from
`flags.bit1`.  `want_pc:false` in the start request disables the PC field even
in halt/breakpoint/watchpoint mode (default `true`).

`flags.bit2 = TRIGGERED` marks a batch that contains at least one trigger-event
frame (DWT watchpoint trigger); the exact sequence number is
`status.trigger_seq`.  `flags.bit3 = CONDITION` marks a batch that contains at
least one frame whose `trigger_condition` matched; the exact seq is
`status.condition_trigger_seq`.  The wire format has no per-frame marker bit, so
batch flags are the compatible minimum (watchpoint-mode frames are all trigger
frames; condition frames are all frames that matched, not every frame).  `/api/curve/status` reports
`pc_available` and the last captured `last_pc`.

Ring: hard-bounded by an **8 KiB heap budget**; the capacity is
`clamp(8192 / (stride), 16, 512)` frames with
`stride = (PC?19:15) + frame_bytes`, so a large payload buys fewer slots instead
of a larger allocation:
16 B -> 264 frames (~8.2 KiB), 64 B -> 103, 128 B -> 57, 256 B (one full group)
-> 30 frames (~8.1 KiB); with the PC field: 234 / 98 / 55 / 29 frames.  The 16-frame floor is only a math guard (never
reached within the 256 B payload cap); if `malloc` still fails the engine halves
the count down to 8 frames.  This guarantees `start` for 64x4B / 32x8B / 256+
series payloads without the old ~34 KiB worst case (series count does not change
the payload cap: a group of 64 size-4 series is exactly 256 B).  `ring_frames` in
`/api/curve/status` reports the actual capacity (never a nominal value);
history at 50 Hz is ~5.3 s / 2.1 s / 1.1 s / 0.6 s for the sizes above.
`overwritten` counts **frames that were
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
match), the protocol v2 fields `group_count`/`current_group`, the CVF2 PC
fields `pc_available` (bool) / `last_pc` (last captured PC, Thumb bit0 cleared),
the DWT watchpoint fields `watchpoint_armed`, `watchpoint_address`,
`watchpoint_hits`, `trigger_seq` (seq of the last trigger-event frame), and the
condition fields `condition_armed` (still evaluating), `condition_met`
(latched), `condition_hits` and `condition_trigger_seq` (seq of the last
condition-match frame).  The start
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

* **Groups**: up to 8 groups x 64 series (<=512 total; object watch: one object
  can be split over groups of <=64 members, each group payload still <=256 B).
  Each tick samples only
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
* **CVF2 PC**: batch `flags.bit1` = PC_PRESENT; when set, every frame has a
  `pc u32` (Thumb bit0 cleared) after `payload_len` (19 B metadata vs 15 B).
  periodic `halt=true` reads r15 once per tick (same DCRSR read used by the PC
  gate/variants), breakpoint reuses the halted core's PC at the FPB hit; no-halt
  cannot read DCRSR and leaves the flag clear.  `want_pc:false` opts out.
* **CVF2**: per-frame `group`/`payload_len` allow different group payload sizes;
  the ring slots are `max(group sample_bytes)`.  `served_seq`/`overwritten`/`gap`
  semantics are unchanged (one ring for all groups).
* **Group weights (P2, backend-only)**: each `groups[]` entry accepts an
  optional `weight` 1..8 (default 1).  The parser expands the weights into a
  deterministic schedule (group index repeated `weight` times, total slots
  <= 8 groups x 8 = 64) and every tick samples the next schedule entry, so a
  heavier ("visible/critical") group gets proportionally more frames.  All
  weights default to 1, which reproduces the legacy equal round-robin exactly;
  the Web UI does not send the field yet and existing clients are unaffected.
  Invalid weights (0, >8, non-numeric) return HTTP 400.  `CVF2.group` still
  carries the original group index, so the Web mapping is unchanged.
* **Static budget (object watch, P1.8)**: the HTTP layer owns one static
  `curve_config_t` (**38232 B**: 8 groups, 512 series x 20 B, 256 loclist
  variants x 12 B, 768 expression nodes x 12 B) plus a static 20480 B request
  body buffer (HTTP `s_json` 1280 B); the engine only keeps a const pointer
  while running and the parser refuses to overwrite it until the engine is idle
  (start returns 409 busy).  Builds stay far below the ESP32/S3 DRAM budget
  (measured image sizes are reported in `PHASE0-1-REPORT.md` §16).
  Logical limits reported by the parser and `GET /api/curve/status`
  (`max_groups`/`max_series`/`max_series_per_group`/`expr_pool_max`/
  `expr_pool_used`): groups 1..8, series/group 1..64, total series <=512,
  variants/series 1..16, total variants <=256, expression nodes <=768
  (each node 12 B).  A request that exhausts the variant/expr pool is rejected
  with a clear 400 error.  The **compact v3 `expr_s` format** is required for
  large object-watch requests: 512 minimal series are ~17 KiB, while the same
  configuration in the verbose v2 object form easily exceeds the 24 KiB body.

## P1.8 object watch: 512 series, expression DAG, per-tick read cache

Ozone-style "watch one object" needs hundreds of member paths (e.g. 324 fields
of `target_debug_handle`).  To support that without exploding memory:

1. **Series limits**: 512 series total, up to 64 per group, 8 groups, per-group
   payload cap stays 256 B (`sum(size)`), so all CVF2/ring/weighted-schedule
   behaviour is unchanged.  The parser and status report the actual maxima.
2. **Expression DAG dedup**: every expression node is canonicalised while it is
   appended to `curve_config_t.pool`: a new node whose `(kind, inner,
   offset/reg/address)` equals an existing node reuses that index instead of
   growing the pool.  Member paths of one object share their root/prefix nodes,
   so 324 fields with two levels of dereference need ~326 nodes instead of
   ~973; 512 distinct one-node `addr` series need 512 nodes.  Nodes are still
   addressed by `uint16_t` indexes, so evaluation is unchanged.
3. **Per-tick pointer-read cache** (optional, automatic): while the core is
   halted the evaluator memoizes 4-byte pointer loads for the current tick
   (`curve_expr_cache_begin(true)` in the sampler loop), so a shared root pointer
   is read once per tick instead of once per series.  Correctness:
   - the cache is cleared every tick and only enabled for halted ticks
     (periodic `halt=true`, breakpoint and watchpoint hits); a no-halt periodic
     tick disables it because the running core may change memory between two
     reads;
   - each series still loads its own value at its own address, so per-series
     values are independent and unchanged;
   - host tests verify read counts (cache hit reduces `tf_swd_read_mem` calls)
     and that no-halt ticks do not cache.

Memory budget (measured on the built firmware, see report §16): the static
configuration object grew from 17.6 KiB to 38.2 KiB and the static request body
from 16 KiB to 24 KiB; the ESP32/S3 images still link with plenty of DRAM/heap
headroom.  Practical upper bounds: 512 series when their expressions average
<=1.5 nodes each on average (768 / 512); object-watch requests with deep member
chains should stay at a few hundred fields (the 324-field case uses ~326
nodes).  Exceeding the pool returns HTTP 400 "too many expr nodes".

## P1.8: conditional trigger `trigger_condition`

`trigger_condition` is an optional, trigger-mode-independent condition
evaluated **after** every sampled tick:

1. The sampler reads `address`/`size` (1/2/4 bytes, little-endian) through the
   same local SWD/AP path used for series (works while the core is halted *or*
   running, so it can be combined with periodic no-halt too).
2. The raw value is zero-extended (`signed=false`, default) or sign-extended
   (`signed=true`) and compared with the int32 `value` using `op`.
3. A match marks the just-recorded frame: CVF2 batch `flags.bit3 =
   CONDITION_TRIGGERED` (exact seq in `status.condition_trigger_seq`), increments
   `status.condition_hits` and latches `status.condition_met`.
   - `mode:"once"` (default): only the first match is evaluated/marked
     (`condition_armed` becomes false, `condition_met` stays true);
   - `mode:"continuous"`: every matching tick is marked/counted.
   - `stop_after:true`: after a match the session stops gracefully (target
     resumed, DAP released, `running=false`, status message
     `"stopped: trigger_condition reached"`).
4. A condition read failure counts as a failed tick (same 5-consecutive-error
   recovery policy as series reads) - the frame is still recorded with its
   previous invalid flag and no condition mark.
5. Pre/post trigger: the ring keeps the most recent frames; the Web client uses
   `condition_trigger_seq` to pick the frames around the match (the frames
   before it are the pre-trigger window, later ones post-trigger).

Limits/notes:

* `value` is an int32 (`-2147483648..2147483647`); `size` is 1/2/4 (no 8-byte
  compare yet).  Address must be inside DTCM/AXI/flash like other target reads.
* The condition is sampled after the series snapshot, so it reflects a value
  read slightly later in the same tick (typically immediately after); it does
  not change the trigger mode itself (periodic/breakpoint/watchpoint still
  decide when a tick is sampled).
* `stop_after` stops the sampling session (`running=false`) but leaves the
  frames in the ring; clients can still fetch them via HTTP/WS until the next
  start.

## P1.8/P1.9: DWT data watchpoint trigger

`trigger=watchpoint` turns the session into a data-access-driven sampler using
Cortex-M7 DWT comparator 0:

1. Bring-up + DAP hand-off are the same as the other trigger modes; the target
   is first resumed if it was halted.
2. The sampler verifies DWT availability (`DWT_CTRL.NUMCOMP > 0`), requires
   `DWT_FUNCTION0 == 0` (free comparator) and saves `DEMCR`, `DWT_COMP0`,
   `DWT_MASK0`, `DWT_FUNCTION0`.  It then sets `DEMCR.TRCENA` and programs
   `DWT_COMP0 = watch_address`, `DWT_MASK0 = log2(watch_size)`,
   `DWT_FUNCTION0 = 6` (write) or `7` (read+write).  Comparator 1..n and the FPB
   are not touched.
3. The worker polls `DHCSR.S_HALT` every ~1 ms on the local SWD link (esp_timer
   one-shot + task notification; httpd is never involved).  On halt it verifies
   `DWT_FUNCTION0.MATCHED` (bit24) so unrelated halts are not recorded as
   watchpoint events.
4. On a real hit the frame is sampled exactly like the breakpoint path (PC
   capture, PC gate/variants, group rotation, CVF2 record with the trigger
   marker), then the comparator is disabled (`DWT_FUNCTION0 = 0`), the core is
   resumed (verified via `DHCSR.S_HALT` clear), the `interval_us` throttle is
   waited and the comparator is re-armed at the top of the loop.
5. `stop`/error restores `DWT_FUNCTION0/COMP0/MASK0` and `DEMCR`, makes sure the
   core runs and releases the DAP.  `watchpoint_hits` counts triggers and
   `trigger_seq` is the CVF2 seq of the last triggered frame, so the Web client
   can fetch the pre/post-trigger window from the ring (the ring always keeps
   the most recent frames).

Watchpoint limitations / risks:

* Only RAM-ish target memory windows are accepted (DTCM/AXI SRAM/flash filter,
  same as sampling reads), and the address must be naturally aligned to
  `watch_size`.  Flash watchpoints would never match writes.
* `DEMCR.TRCENA` is shared with ITM/trace: the original `DEMCR` is restored on
  stop, but another debugger owning DWT/ITM at the same time is a resource
  conflict (comparator 0 busy is rejected with a clear start error).
* DWT data watchpoints halt the core on *every* matching access until the
  comparator is disarmed; the throttle (`interval_us`, 5..1000 ms) controls how
  often the target is allowed to stop again.
* A sticky/unexpected halt that is not our comparator is resumed and counted;
  5 consecutive unexpected halts / SWD errors stop the session and restore DWT.
* Conditional (`trigger_condition`) evaluation is implemented on top of every
  trigger mode (see "P1.8: conditional trigger" above); the DWT address/access
  match is still what makes a watchpoint tick happen, the condition is an
  additional per-tick address comparison.
* `watch_access:"read"` is not exposed (contract only lists write/readwrite);
  the DWT read-only encoding 5 is unused.

## P1.11: WebSocket CVF2 frame push (`/ws/curve`)

Optional low-latency replacement for HTTP polling: after
`POST /api/curve/start` the browser opens `GET /ws/curve` (WebSocket, same
origin + `web_host_guard` policy) and subscribes; the sampler pushes the same
CVF2 batches that `GET /api/curve/frames` would return.  HTTP polling remains
fully functional (it is not disabled by a WS subscription).

- **Client -> server (text JSON, one message per frame, no fragmentation):**
  - `{"cmd":"subscribe"}` - start pushing from the oldest retained ring frame
    (like HTTP `since=0`); reply
    `{"ok":true,"cmd":"subscribe","interval_ms":20,"max_batch":32,"format":"CVF2"}`.
  - `{"cmd":"unsubscribe"}` - stop pushing, keep the socket open;
    reply `{"ok":true,"cmd":"unsubscribe"}`.
  - `{"cmd":"ack","seq":N}` - optional flow-control ack (monotonic, clamped to
    the last pushed seq); no reply.  The device currently pushes on a fixed
    20 ms cadence and does not retransmit; ack is informational/for Web display.
  - `{"cmd":"ping"}` - reply `{"ok":true,"cmd":"pong"}` (WS PING control frames
    are also answered with PONG automatically).
- **Server -> client (binary):** one CVF2 batch per frame, exactly the format of
  `GET /api/curve/frames` (16 B `CVF2` header with `flags` bit0 gap / bit1
  PC_PRESENT / bit2 TRIGGERED, then variable-length frames).  Decode it with the
  same decoder as the HTTP path; there is no HTTP status line.
- **Errors (text JSON):** `{"ok":false,"err":"bad json"}`,
  `{"ok":false,"err":"cmd missing"}`, `{"ok":false,"err":"unknown cmd"}`,
  `{"ok":false,"err":"not connected"}`.  A client that sends oversized/binary/
  fragmented messages, or whose socket send fails, is disconnected.
- **Backpressure:** at most `CURVE_WS_PUSH_INTERVAL_MS` (20 ms) between batches
  per client and at most `CURVE_WS_MAX_BATCH_FRAMES` (32) frames per batch; if
  heap allocation or the socket send fails the batch is skipped (counted in the
  per-client `dropped_batches`) and a client with send errors is detached.  The
  push runs on its own FreeRTOS task, so a slow WebSocket client never blocks
  the sampler worker or the httpd task.
- **Limits:** `CURVE_WS_MAX_CLIENTS = 4` concurrent sockets; one subscription
  table entry per socket; no WebSocket fragmentation support for client
  messages (<= 256 B JSON commands).
- **Relationship to HTTP polling:** both paths read the same ring and advance
  the same `served`/`overwritten` accounting (single-consumer assumption), so a
  page should use either WS push or HTTP polling for a session, not both
  simultaneously (otherwise `overwritten` may count frames delivered to only
  one of the consumers).
- **Suggested Web flow:** `POST /api/curve/start` -> open `ws(s)://<host>/ws/curve`
  -> send `{"cmd":"subscribe"}` -> on each binary message decode CVF2 and feed
  the chart (`flags.bit2` marks trigger batches, `status.trigger_seq` the exact
  trigger frame) -> `POST /api/curve/stop` (or close) on page unload; fall back
  to HTTP polling if the WS cannot connect.  `status` remains on HTTP.

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
* `src/curve_ws_core.c` - pure-C WebSocket subscription/cursor/backpressure
  state (host tested: attach/subscribe/plan/ack/drop/table limits).
* `src/curve_ws.c` - `/ws/curve` IDF glue: WS URI handler, JSON command
  parsing, dedicated push task (`httpd_ws_send_frame_async` with a send lock).
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
  parsing/normalization/scope test, 64-series/group + 512-series config/pool/
  ring boundaries (including a 512-series compact v3 body that fits the HTTP
  budget), expression DAG dedup (324 object fields -> <=326 nodes, identical
  sub-trees collapse), per-tick read-cache hit/miss/no-halt behaviour, the
  status JSON field set, and the protocol v2 model (groups parsing/boundaries
  8x64=512 series, variant selection + normalisation, variant/expression pool
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
  talks to older firmware.  The CVF2 frame layout depends on `flags.bit1`
  (PC_PRESENT): old decoders that ignore bit1 will misread frames from
  halt/breakpoint sessions (PC bytes shift the payload), so any client must
  read the flag before decoding.
* The static config is a single instance: only one sampler session at a time
  (as before), and a new start request while running returns 409 busy before
  the request body is parsed.
* The ring is intentionally small (8 KiB budget): at 256 B/frame only 30 frames
  (~0.6 s at 50 Hz) are retained.  The Web UI polls every 30-50 ms, so this is
  enough; a stalled browser will see `overwritten` grow sooner than before.
* Object watch: 512 series only fit the expression pool when the average chain
  is <=4 nodes; deep member chains should stay at a few hundred fields (DAG
  dedup shares prefixes).  Large configurations must use the compact v3
  `expr_s` format - the verbose object format can exceed the 24 KiB body.
* The per-tick pointer-read cache is active only while the core is halted
  (breakpoint/watchpoint, periodic halt=true); no-halt ticks intentionally read
  every time to avoid stale values.
* `trigger_condition` compares an int32 value only (no 64-bit compare yet) and
  is evaluated after the series snapshot; the CVF2 bit3 marker is per batch
  with the exact seq in `status.condition_trigger_seq`.
* WebSocket push and HTTP polling share the single ring/served cursor; use one
  delivery path per session.  WS client commands must be single unfragmented
  text frames <= 256 B; the endpoint is limited to 4 concurrent sockets.
* DWT watchpoint triggers sample the halted frame *after* the watched access;
  the pre-trigger frames are whatever the ring already holds (poll promptly to
  keep the window useful).  Conditional expressions are not implemented yet.
* Group weights are a backend-only scheduling feature for now; the Web UI does
  not send `weight` and always gets equal round-robin.  A weight only changes
  how often a group is sampled, not its refresh timestamp cadence per group.
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
