/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - internal types and contract constants.
 *
 * The public contract is docs/curve-local-sampler.md (HTTP + CVF1 binary
 * framing).  This header is private to components/curve_sampler; it only holds
 * the config / expression / status structures shared between the HTTP layer,
 * the JSON parser and the sampling engine.
 */
#ifndef CURVE_PRIV_H
#define CURVE_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CURVE_TAG "curve_sampler"

/* ---- contract limits (docs/curve-local-sampler.md §1.1 + §9 protocol v2) - */
/* Multi-group sampling (protocol v2): up to 8 groups, 32 series per group and
 * 256 series total, each group payload <= 256 B.  Everything is static memory
 * (not on a task stack): the HTTP parser owns one static curve_config_t and the
 * engine only keeps a pointer to it while a session runs.
 *
 * Actual static-memory caps (reported to the Lead, not all in the Web API):
 *   CURVE_MAX_GROUPS              8
 *   CURVE_MAX_SERIES_PER_GROUP   64 (object watch: up to 64 members/group)
 *   CURVE_MAX_SERIES            256 (total across groups; typical use is tens)
 *   CURVE_MAX_VARIANTS_PER_SERIES 16
 *   CURVE_MAX_VARIANTS          256 (total loclist variant pool)
 *   CURVE_EXPR_POOL_MAX        2048 expression nodes (12 B each, 24 KiB).
 *     Object member paths share their prefix: the parser deduplicates identical
 *     (kind, inner, offset/reg/address) nodes, so 324 object fields typically
 *     need only a few hundred nodes instead of ~1600; 2048 still leaves headroom
 *     for 512 independent series with 3-4 node chains.
 *   CURVE_JSON_BODY_MAX        16384 JSON request bytes (restored target).
 *     The JSON path is a compatibility path: the body is a single heap block,
 *     which the unified NCM+WiFi image cannot always provide once the heap is
 *     fragmented.  New Web clients use POST /api/curve/start-bin instead: the
 *     compact binary (CBS1) request is parsed in a streaming fashion with a
 *     256 B stack buffer, so it never needs a contiguous 16 KiB heap block.
 * A request that exceeds the variant/expr pool is rejected with a clear parse
 * error (groups/series limits themselves follow the contract exactly). */
#define CURVE_MAX_GROUPS            8U
#define CURVE_MAX_SERIES_PER_GROUP  64U
#define CURVE_MAX_SERIES            256U
#define CURVE_MAX_VARIANTS_PER_SERIES 16U
#define CURVE_MAX_VARIANTS          256U
#define CURVE_EXPR_POOL_MAX         768U
#define CURVE_MAX_EXPR_DEPTH        8U
#define CURVE_MAX_SAMPLE_BYTES      256U
#define CURVE_MIN_INTERVAL_US       1000U
#define CURVE_MAX_INTERVAL_US       200000U
#define CURVE_SERIES_SIZE_MIN       1U
#define CURVE_SERIES_SIZE_MAX       8U
#define CURVE_JSON_BODY_MAX         16384U
/* Per-series divider (protocol v3 "exact refresh rate"): sample this series
 * every N ticks of its group (1 = every tick, default).  Stored in
 * curve_series_t.sample_every; the engine keeps a per-series counter and marks
 * non-sampled ticks stale in a CVF2 footer (flags bit4). */
#define CURVE_SAMPLE_EVERY_MIN      1U
#define CURVE_SAMPLE_EVERY_MAX      64U
#define CURVE_MAX_FRAMES_PER_REQ    64U
/* Worst-case GET /api/curve/status JSON (all counters at u32 max, message and
 * last_err full).  Static response buffer size: the http layer uses it for
 * status / start / trace / backtrace responses.  Kept as one constant so the
 * host test can assert the exact formatting never truncates. */
#define CURVE_STATUS_JSON_MAX       2048U

/* Frame ring.  The hard constraint is the heap budget (8 KiB), so the capacity
 * is budget/stride first and only then clamped to a small safety floor.  Large
 * payloads (v2: up to 256 B per frame) therefore get fewer frames instead of a
 * large allocation that could fail on the fragmented WiFi heap:
 *   stride   = CURVE_FRAME_META_BYTES (15) + frame_bytes
 *   capacity = clamp(CURVE_RING_BYTES_BUDGET / stride,
 *                    CURVE_RING_FRAMES_MIN (16), CURVE_RING_FRAMES_MAX (512))
 *   ring bytes = capacity * stride <= CURVE_RING_BYTES_BUDGET (8 KiB)
 *   16 B/frame  -> 264 frames (~8.2 KiB incl. metadata; <= budget)
 *   64 B/frame  -> 103 frames
 *   128 B/frame ->  57 frames
 *   256 B/frame ->  30 frames (the 16-frame floor is never reached within the
 *                           contract payload limit; it only guards the math)
 * Allocated lazily on the first start and reused across sessions; if malloc
 * still fails the engine halves the frame count down to 8 frames. */
#define CURVE_RING_FRAMES_MIN       16U
#define CURVE_RING_FRAMES_MAX       512U
#define CURVE_RING_BYTES_BUDGET     (8U * 1024U)
/* Protocol v2 ring/CVF2 per-frame metadata: seq (u32) + timestamp_us (u64) +
 * group (u8) + payload_len (u16) = 15 B.  The ring slot also reserves the
 * maximum group payload (frame_bytes) after that. */
#define CURVE_FRAME_META_BYTES      15U /* seq+ts+group+payload_len */
#define CURVE_FRAME_META_BYTES_PC   19U /* + pc u32 (CVF2 flags bit1) */
#define CURVE_FRAME_HEADER_BYTES    16U
#define CURVE_CVF2_VERSION          2U

/* ---- Phase 2: breakpoint trigger (FPB) ------------------------------------
 * trigger=breakpoint installs one Cortex-M7 FPB code comparator on a target
 * flash instruction address.  The core stops itself on the breakpoint, the
 * sampler reads the frame while it is halted, then disables the comparator,
 * resumes the core, waits `interval_us` (throttle) and re-enables it.  Only one
 * comparator is used; FPB_CTRL/FP_COMP0 original values are saved and restored
 * on stop/error. */
#define CURVE_BP_INTERVAL_MIN_US     5000U
#define CURVE_BP_INTERVAL_MAX_US     1000000U
#define CURVE_BP_INTERVAL_DEFAULT_US 20000U
/* DHCSR.S_HALT polling period while waiting for the breakpoint hit. */
#define CURVE_BP_POLL_INTERVAL_US    1000U
/* Target flash window allowed for breakpoint addresses (Thumb bit0 ignored). */
#define CURVE_BP_FLASH_START         0x08000000U
#define CURVE_BP_FLASH_END           0x08FFFFFFU
/* Cortex-M7 FPB (flash patch and breakpoint unit) PPB registers. */
#define CURVE_FPB_CTRL               0xE0002000U
#define CURVE_FPB_COMP0              0xE0002008U
#define CURVE_FPB_CTRL_ENABLE        0x00000001U
#define CURVE_FPB_CTRL_KEY           0x00000002U

typedef enum {
    CURVE_TRIGGER_PERIODIC = 0,
    CURVE_TRIGGER_BREAKPOINT = 1,
    CURVE_TRIGGER_WATCHPOINT = 2, /* P1.8/P1.9: DWT data watchpoint trigger */
} curve_trigger_t;

/* ---- P1.8/P1.9 + P1.8.1: DWT data watchpoints ---------------------------
 * Cortex-M7 DWT comparators: DWT_COMPn = address, DWT_MASKn = log2(size),
 * DWT_FUNCTIONn = 5 (read) / 6 (write) / 7 (read+write) with MATCHED bit24
 * after a hit, DEMCR.TRCENA (bit24) must be set.  The sampler saves and
 * restores DEMCR/COMPn/MASKn/FUNCTIONn for every comparator it allocated and
 * disarms them before every resume (same throttle flow as the FPB breakpoint
 * trigger).  Only comparators found free (FUNCTIONn & 0xF == 0) are used;
 * the original single-watch requests still prefer comparator 0 first. */
#define CURVE_TRIGGER_WATCH_MIN_US   5000U
#define CURVE_TRIGGER_WATCH_MAX_US   1000000U
#define CURVE_TRIGGER_WATCH_DEFAULT_US 20000U
/* Watch-size checks are 1/2/4 bytes (DWT_MASK = log2(size)). */
#define CURVE_WATCH_SIZE_1           1U
#define CURVE_WATCH_SIZE_2           2U
#define CURVE_WATCH_SIZE_4           4U
#define CURVE_WATCH_DEFAULT_SIZE     4U

/* DWT data-watch access type -> DWT_FUNCTION value (Cortex-M3/M4/M7):
 *   5 = read (load), 6 = write (store), 7 = read/write (load/store).
 * The original firmware only exposed write/readwrite; P1.8.1 adds read and a
 * second comparator while keeping the old request/status fields intact. */
typedef enum {
    CURVE_WATCH_ACCESS_WRITE = 0,
    CURVE_WATCH_ACCESS_RW = 1,
    CURVE_WATCH_ACCESS_READ = 2,
} curve_watch_access_t;

/* DWT comparator count (P1.8.1 added a second, P1-12 extends to four).
 * Cortex-M7 implements 4 data-watch comparators; the sampler allocates only
 * free ones (FUNCTIONn == 0) and reports a clear start error when the target
 * has fewer free comparators than requested.  watch/watch2 stay the legacy
 * JSON keys (slots 0/1); the canonical request form is the `watchpoints`
 * array (0..4 entries) and watch3/watch4 are convenience aliases. */
#define CURVE_MAX_WATCHPOINTS       4U
#define CURVE_DWT_MAX_COMP         4U
#define CURVE_DWT_FUNCTION_MASK    0x0FU

/* ---- P1.8.2 "function snapshot": one-shot breakpoint capture --------------
 * Opt-in (start request field `snapshot_after`, 0..CURVE_SNAPSHOT_MAX_AFTER).
 * On the first FPB hit the sampler records the trigger frame, then captures
 * `snapshot_after` additional periodic (halt -> sample -> resume) frames and
 * stops the session.  The pre-trigger history stays in the normal ring, so
 * Web can fetch [trigger_seq - snapshot_pre_frames, trigger_seq + after].
 * Default (field absent) keeps the continuous breakpoint behavior unchanged. */
typedef enum {
    CURVE_SNAPSHOT_OFF = 0,       /* not configured */
    CURVE_SNAPSHOT_ARMED = 1,     /* configured, waiting for the first hit */
    CURVE_SNAPSHOT_CAPTURING = 2, /* trigger recorded, capturing post frames */
    CURVE_SNAPSHOT_COMPLETE = 3,  /* all post frames captured, session stopped */
    CURVE_SNAPSHOT_ABORTED = 4,   /* stopped/error before the snapshot finished */
} curve_snapshot_state_t;

#define CURVE_SNAPSHOT_MAX_AFTER    32U

/** "off"/"armed"/"capturing"/"complete"/"aborted" (unknown -> "off"). */
const char *curve_snapshot_state_name(uint8_t state);

/* ---- P1.8.2 prototype: frame-pointer backtrace ----------------------------
 * Prototype only (not a DWARF-CFI unwinder): at a breakpoint/watchpoint hit
 * the firmware follows the AAPCS frame-pointer chain ([fp] = previous fp,
 * [fp+4] = return address) through the AHB-AP and keeps up to 8 PCs.  It needs
 * the target code to maintain frame pointers (-O0 or -fno-omit-frame-pointer);
 * optimized code that reuses r7 yields a short/invalid chain. */
#define CURVE_BACKTRACE_MAX_FRAMES  8U

typedef bool (*curve_backtrace_read_fn)(void *ctx, uint32_t addr, uint32_t *value);

typedef struct {
    uint8_t count;                            /* valid PCs, 0 = no capture */
    bool truncated;                           /* stopped at the frame limit */
    uint32_t pcs[CURVE_BACKTRACE_MAX_FRAMES]; /* [0] = halted PC, then returns */
} curve_backtrace_t;

void curve_backtrace_reset(curve_backtrace_t *bt);

/**
 * @brief Unwind a frame-pointer chain into `out` (pure, host-tested).
 *
 * @param pc   halted core PC (Thumb bit0 ignored)
 * @param fp   frame pointer (Cortex-M r7 at the hit)
 * @param read target-word reader (engine passes the AHB-AP read with the
 *             allowed-memory-window filter; the host test passes a mock)
 * @return true when at least the halted PC was captured.
 */
bool curve_backtrace_capture(uint32_t pc, uint32_t fp,
                             curve_backtrace_read_fn read, void *ctx,
                             curve_backtrace_t *out);

/* ---- P1.8.3 prototype: target-UART trace capture -------------------------
 * The board already has a target-UART link (components/uart_service is the
 * single owner, also used by the Web UART page / TCP 1346 bridge).  This
 * prototype taps that existing link: a 2 KiB byte ring keeps the most recent
 * target output and the HTTP layer serves it to the Web as raw bytes.
 *
 * Boundaries: no SWO/ITM (SWO_FUNCTION_ENABLE=0, no SWO pin to the target),
 * no per-byte target timestamp (only the byte sequence counter), no baud
 * control from this component (use the Web UART module), and the UART is
 * shared with the existing consumers. */
#define CURVE_TRACE_RING_BYTES      2048U

typedef struct {
    uint8_t *storage;
    uint32_t capacity;
    uint32_t head;   /* next write offset */
    uint32_t used;   /* retained bytes (<= capacity) */
    uint32_t total;  /* bytes appended since reset (byte seq) */
} curve_trace_ring_t;

/** One contiguous piece of the retained trace window for a chunked response. */
typedef struct {
    uint32_t offset; /* index into ring storage */
    uint32_t len;
} curve_trace_seg_t;

void curve_trace_ring_reset(curve_trace_ring_t *ring, uint8_t *storage,
                            uint32_t capacity);

/** Append target bytes (oldest bytes are overwritten when the ring is full). */
void curve_trace_ring_append(curve_trace_ring_t *ring, const uint8_t *data,
                             uint32_t len);

/**
 * @brief Plan the retained bytes newer than `since` (pure, host-tested).
 *
 * @param first_seq first returned byte sequence number (oldest retained when
 *                  `since` was evicted or 0)
 * @param gap       true when `since` is older than the oldest retained byte
 * @param seg       up to two contiguous storage segments (wrap split)
 * @return total bytes to send (0 = nothing new)
 */
uint32_t curve_trace_ring_plan(const curve_trace_ring_t *ring, uint32_t since,
                               uint32_t *first_seq, bool *gap,
                               curve_trace_seg_t seg[2]);

/** Trace capture control (curve_trace.c): subscribes to the UART service. */
esp_err_t curve_trace_start(void);
esp_err_t curve_trace_stop(void);

/**
 * @brief Start trace capture with an optional line trigger (P1-13).
 *
 * In addition to the raw byte ring, the RX fan-out assembles target lines
 * (split on '\n', '\r' stripped, non-printable bytes replaced with '.') and
 * keeps the most recent CURVE_TRACE_LINE_MAX lines with the ESP32
 * esp_timer_get_time() timestamp of line completion.  When `trigger` is a
 * non-empty substring, the first matching line sets triggered/trigger_seq;
 * `stop_after=true` stops the capture at that line (the raw byte ring keeps
 * everything captured so far).  No target-side timestamps are invented: t_us
 * is the ESP32 receive time.
 *
 * @param trigger    substring to match in a completed line (NULL/"" = no trigger)
 * @param stop_after stop capturing after the first trigger match
 */
esp_err_t curve_trace_start_with_config(const char *trigger, bool stop_after);

/** Retained trace status: active flag, byte seq, evicted bytes. */
void curve_trace_get_status(bool *active, uint32_t *total_bytes,
                            uint32_t *dropped_bytes);

/* ---- P1-13: line-buffered trace (timestamped, triggerable) --------------- */
#define CURVE_TRACE_LINE_TEXT_MAX   96U
#define CURVE_TRACE_LINE_MAX        32U
#define CURVE_TRACE_TRIGGER_MAX     32U
#define CURVE_TRACE_LINES_PER_REQ   8U
#define CURVE_TRACE_LINE_FLAG_TRUNCATED 0x01U

typedef struct {
    uint32_t seq;      /* line sequence (starts at 1) */
    uint64_t t_us;     /* ESP32 esp_timer time when the line was completed */
    uint8_t len;       /* bytes in text (no NUL) */
    uint8_t flags;     /* CURVE_TRACE_LINE_FLAG_* */
    char text[CURVE_TRACE_LINE_TEXT_MAX + 1U]; /* NUL-terminated copy */
} curve_trace_line_t;

typedef struct {
    curve_trace_line_t *storage;
    uint32_t capacity;
    uint32_t head;   /* next write slot */
    uint32_t count;  /* retained lines (<= capacity) */
    uint32_t total;  /* lines completed since reset */
} curve_trace_line_ring_t;

void curve_trace_line_ring_reset(curve_trace_line_ring_t *ring,
                                 curve_trace_line_t *storage, uint32_t capacity);

/** Append one completed (sanitized) line; the oldest line is overwritten when
 *  the ring is full.  len is clamped to CURVE_TRACE_LINE_TEXT_MAX. */
void curve_trace_line_ring_append(curve_trace_line_ring_t *ring, uint64_t t_us,
                                  const char *text, uint32_t len,
                                  bool truncated);

/**
 * @brief Copy up to `max` retained lines newer than line sequence `since`.
 * @param out        caller array with room for `max` lines
 * @param first_seq  seq of out[0]
 * @param gap        true when `since` is older than the oldest retained line
 * @return copied line count (0 = nothing new)
 */
uint32_t curve_trace_line_ring_plan(const curve_trace_line_ring_t *ring,
                                    uint32_t since, uint32_t max,
                                    curve_trace_line_t *out,
                                    uint32_t *first_seq, bool *gap);

/** Case-sensitive substring match used by the trace trigger (NULL-safe). */
bool curve_trace_line_match(const char *text, const char *pattern);

/**
 * @brief Read timestamped lines for GET /api/curve/trace/lines (thread-safe).
 * @return copied line count, first returned seq, gap flag.
 */
uint32_t curve_trace_lines_read(uint32_t since, uint32_t max,
                                curve_trace_line_t *out, uint32_t *first_seq,
                                bool *gap);

/** Line-ring status: active, total lines, evicted lines, trigger latch. */
void curve_trace_lines_get_status(bool *active, uint32_t *total_lines,
                                  uint32_t *dropped_lines, bool *triggered,
                                  uint32_t *trigger_seq);

/**
 * @brief Copy the retained trace bytes newer than `since` into `out`.
 *
 * Thread-safe view of the live trace state for the HTTP layer: the plan is
 * computed and copied under the trace mutex so a concurrent UART RX append
 * cannot tear the response; the caller sends the bytes after the lock is
 * released.  At most `max` bytes are copied.
 *
 * @return copied byte count (0 = nothing new), first returned byte seq, gap.
 */
uint32_t curve_trace_read(uint32_t since, uint8_t *out, uint32_t max,
                          uint32_t *first_seq, bool *gap);

/* ---- P1.8 conditional trigger (trigger_condition) ------------------------
 * After each sampled tick the sampler reads one target address/size and
 * compares it with an int32 value.  A match marks the frame/batch in CVF2
 * (flags bit3) and status; `mode=once` latches after the first match,
 * `continuous` marks/counts every match and `stop_after=true` ends the session
 * after the first match.  Works on top of periodic/breakpoint/watchpoint. */
typedef enum {
    CURVE_COND_OP_EQ = 0,
    CURVE_COND_OP_NE = 1,
    CURVE_COND_OP_LT = 2,
    CURVE_COND_OP_LE = 3,
    CURVE_COND_OP_GT = 4,
    CURVE_COND_OP_GE = 5,
} curve_cond_op_t;

typedef enum {
    CURVE_COND_MODE_ONCE = 0,
    CURVE_COND_MODE_CONTINUOUS = 1,
} curve_cond_mode_t;

#define CURVE_COND_SIZE_MIN 1U
#define CURVE_COND_SIZE_MAX 4U

/** Compare a size-byte little-endian raw value with the configured int32. */
bool curve_condition_match(uint32_t raw_value, uint8_t size, bool is_signed,
                           uint8_t op, int32_t value);

/** "eq"/"ne"/"lt"/"le"/"gt"/"ge" (unknown -> "?"). */
const char *curve_cond_op_name(uint8_t op);

/* ---- Cortex-M7 DWT / DEMCR registers ------------------------------------- */
#define CURVE_DWT_CTRL               0xE0001000U
/* Comparator n register block: COMP/MASK/FUNCTION at +0x10*n from COMP0. */
#define CURVE_DWT_COMP0              0xE0001020U
#define CURVE_DWT_COMP(n)            (CURVE_DWT_COMP0 + 0x10U * (uint32_t)(n))
#define CURVE_DWT_MASK(n)            (CURVE_DWT_COMP0 + 0x04U + 0x10U * (uint32_t)(n))
#define CURVE_DWT_FUNCTION(n)        (CURVE_DWT_COMP0 + 0x08U + 0x10U * (uint32_t)(n))
#define CURVE_DWT_MASK0              0xE0001024U
#define CURVE_DWT_FUNCTION0          0xE0001028U
#define CURVE_DEMCR                  0xE000EDFCU
#define CURVE_DEMCR_TRCENA           (1U << 24)
#define CURVE_DWT_FUNCTION_MATCHED   (1U << 24)
#define CURVE_DWT_FUNC_READ          5U
#define CURVE_DWT_FUNC_WRITE         6U
#define CURVE_DWT_FUNC_ACCESS        7U
#define CURVE_DWT_NUM_COMP(ctrl)     (((ctrl) >> 28) & 0xFU)

/** True when the watchpoint address/size is valid: size 1/2/4, size-aligned,
 *  and the whole range is in an allowed target memory window (same filter as
 *  sampling reads). */
bool curve_watchpoint_params_ok(uint32_t address, uint8_t size);

/** DWT_MASK value for a watch size (0=1B, 1=2B, 2=4B); invalid size -> 0. */
uint32_t curve_dwt_mask_for_size(uint8_t size);

/** DWT_FUNCTION value for a curve_watch_access_t; invalid access -> 0. */
uint32_t curve_dwt_function_for_access(uint8_t access);

/**
 * @brief Allocate free DWT comparators for `want` watchpoints (pure, host-tested).
 *
 * @param num_comp  DWT_CTRL.NUMCOMP (available comparators, clamped to 4)
 * @param functions current DWT_FUNCTIONn values, one per available comparator
 *                  (only the low 4 bits are checked; ~0 when occupied = busy)
 * @param want      requested comparators (1..CURVE_MAX_WATCHPOINTS)
 * @param out       comparator indexes (lowest free first)
 * @return true when `want` free comparators were found and written to `out`.
 */
bool curve_dwt_alloc_comparators(uint8_t num_comp, const uint32_t *functions,
                                 uint8_t want, uint8_t *out);

/**
 * @brief Bitmask of the allocated comparators whose MATCHED bit is set.
 * @param functions current DWT_FUNCTIONn values (indexed by comparator)
 * @param indices   allocated comparator indexes (0..3)
 * @param count     allocated comparator count
 * @return bit n set = comparator indices[n] matched (n = watchpoint slot 0/1).
 */
uint8_t curve_dwt_hit_mask(const uint32_t *functions, const uint8_t *indices,
                           uint8_t count);

/** "write"/"read"/"readwrite" name for a curve_watch_access_t. */
const char *curve_watch_access_name(uint8_t access);

/** True when the breakpoint address (Thumb bit0 ignored) is in target flash. */
bool curve_breakpoint_addr_ok(uint32_t address);

/** FPB comparator value: address with Thumb bit0 ignored, low bit set. */
uint32_t curve_breakpoint_compare_value(uint32_t address);

/** "periodic"/"breakpoint" (unknown values fall back to "periodic"). */
const char *curve_trigger_name(uint8_t trigger);

/**
 * @brief PC scope test for a PC-gated series.
 *
 * `pc_low`/`pc_high` are the parser-normalized (bit0 cleared) bounds, high is
 * exclusive.  `pc` is the raw DCRSR PC value (Thumb bit0 may be set or clear);
 * it is normalized here, so the range check is independent of the Thumb-bit
 * convention used by the Web/DWARF side.
 */
bool curve_pc_in_scope(uint32_t pc_low, uint32_t pc_high, uint32_t pc);

/* The per-frame ring group byte uses bit7 as the trigger-event marker (group
 * indexes are 0..7), so CVF2 batches can set flags.bit2 without extra RAM. */
#define CURVE_RING_GROUP_TRIGGERED 0x80U
#define CURVE_RING_GROUP_CONDITION 0x40U

static inline uint8_t curve_ring_group_encode(uint8_t group, bool triggered,
                                              bool condition)
{
    return (uint8_t)((group & 0x07U) |
                     (triggered ? CURVE_RING_GROUP_TRIGGERED : 0U) |
                     (condition ? CURVE_RING_GROUP_CONDITION : 0U));
}

static inline uint8_t curve_ring_group_index(uint8_t encoded)
{
    return (uint8_t)(encoded & 0x07U);
}

static inline bool curve_ring_group_triggered(uint8_t encoded)
{
    return (encoded & CURVE_RING_GROUP_TRIGGERED) != 0U;
}

static inline bool curve_ring_group_condition(uint8_t encoded)
{
    return (encoded & CURVE_RING_GROUP_CONDITION) != 0U;
}

/** Advance a weighted-round-robin schedule slot. */
static inline uint8_t curve_next_slot(uint8_t position, uint8_t schedule_len)
{
    if (schedule_len == 0U) {
        return 0U;
    }
    position++;
    return (position >= schedule_len) ? 0U : position;
}

/* Phase 0 selftest: N halt -> regs -> mem -> resume iterations. */
#define CURVE_SELFTEST_ITERATIONS   20U

/* SWD error policy: 5 consecutive failed samples stop the session. */
#define CURVE_MAX_CONSECUTIVE_ERRORS 5U

/* DAP hand-off / restore (same values as wt_target_flash.c). */
#define CURVE_DAP_HANDOFF_TIMEOUT_MS 3000U
#define CURVE_DAP_RESTORE_TIMEOUT_MS 1000U
#define CURVE_BRINGUP_TIMEOUT_MS    10000U
#define CURVE_STOP_TIMEOUT_MS       5000U
/* Let a kicked WebSocket-DAP session finish its close/RESET_HANDLE path before
 * we post DELETE_HANDLE ourselves. */
#define CURVE_KICK_SETTLE_MS        50U

/* ---- Cortex-M debug registers (accessed through the AHB-AP) --------------- */
#define CURVE_REG_DHCSR             0xE000EDF0U
#define CURVE_REG_DCRSR             0xE000EDF4U
#define CURVE_REG_DCRDR             0xE000EDF8U

#define CURVE_DHCSR_DBGKEY          0xA05F0000U
#define CURVE_DHCSR_C_DEBUGEN       0x00000001U
#define CURVE_DHCSR_C_HALT          0x00000002U
#define CURVE_DHCSR_S_REGRDY        0x00010000U
#define CURVE_DHCSR_S_HALT          0x00020000U

/* CFA (DW_OP_call_frame_cfa) is approximated with SP=r13, same as the Web UI
 * (web/src/utils/dwarfLocation.ts: CFA_FRAME_REG). */
#define CURVE_CFA_REG               13U
#define CURVE_MAX_CORE_REGS         16U

/* ---- target memory ranges allowed for sampling (STM32H7) ------------------
 * DTCM 0x20000000..0x2001FFFF, AXI SRAM 0x24000000..0x2407FFFF,
 * Flash 0x08000000..0x08FFFFFF.  The STM32F4 SRAM range coincides with the
 * first region, so the same filter works on the F4 boards used for bring-up.
 * Only these windows are ever read; anything else is reported as invalid. */
#define CURVE_REGION_COUNT          3U

/**
 * @brief True when [addr, addr+size) is inside one allowed region and every
 *        32-bit word the AHB-AP block read touches is inside it too.
 *
 * Implemented in curve_expr.c (single definition shared by engine/parser).
 */
bool curve_addr_range_ok(uint32_t addr, uint32_t size);

/* ---- address expression (schema identical to Web AddressExpr) ------------- */
typedef enum {
    CURVE_EXPR_ADDR = 0,
    CURVE_EXPR_REG,
    CURVE_EXPR_CFA,
    CURVE_EXPR_ADD,
    CURVE_EXPR_DEREF,
} curve_expr_kind_t;

#define CURVE_EXPR_NONE 0xFFFFU

typedef struct curve_expr {
    uint8_t kind;              /* curve_expr_kind_t */
    uint8_t reg;               /* CURVE_EXPR_REG: r0..r15 */
    uint16_t inner;            /* add/deref: index of the nested expression in
                                  curve_config_t.pool (CURVE_EXPR_NONE = leaf) */
    uint32_t address;          /* CURVE_EXPR_ADDR */
    int32_t offset;            /* reg/cfa/add/deref: signed byte offset */
} curve_expr_t;

#define CURVE_SERIES_FLAG_PC_GATE  0x01U
#define CURVE_SERIES_FLAG_VARIANTS 0x02U

/** One loclist variant: PC range [pc_low, pc_high) selects `expr`. */
typedef struct {
    uint32_t pc_low;   /* normalized (bit0 cleared) flash address */
    uint32_t pc_high;  /* exclusive */
    uint16_t expr;     /* index into curve_config_t.pool */
    uint16_t reserved;
} curve_variant_t;

typedef struct {
    uint32_t pc_low;          /* single-style PC gate bounds (normalized) */
    uint32_t pc_high;         /* valid when CURVE_SERIES_FLAG_PC_GATE is set */
    uint16_t expr;            /* single expression index (CURVE_EXPR_NONE when
                                 the series only has loclist variants) */
    uint16_t variant_first;   /* first index into curve_config_t.variants */
    uint8_t size;             /* 1..8 bytes read from the evaluated address */
    uint8_t flags;            /* CURVE_SERIES_FLAG_* */
    uint8_t variant_count;    /* 0 = single expr/pc-gate series, 1..16 otherwise */
    uint8_t sample_every;     /* 1..CURVE_SAMPLE_EVERY_MAX ticks per sample
                               * (0 is normalized to 1 by the parsers).  Uses
                               * the former `reserved` byte, so sizeof stays 16
                               * and the 256-series pool is still 4 KiB. */
} curve_series_t;

/* P2: optional per-group scheduling weight (weighted round-robin).  Web does
 * not send it yet; every group defaults to 1 => the existing equal round-robin
 * schedule (0,1,2,...) is reproduced exactly. */
#define CURVE_MAX_GROUP_WEIGHT      8U

/** One sampling group: a contiguous slice of curve_config_t.series[]. */
typedef struct {
    uint16_t series_first;
    uint16_t series_count;
    uint16_t sample_bytes;    /* sum of this group's series sizes (<= 256) */
    bool pc_needed;           /* any series needs r15 (pc gate or variants) */
    uint8_t weight;           /* 1..8 scheduling slots per round (default 1) */
    uint8_t reg_count;        /* core registers needed by this group only */
    uint8_t regs[CURVE_MAX_CORE_REGS];
} curve_group_t;

/** Immutable sampling configuration (parsed before the worker is started).
 *
 * Protocol v2: `groups` rotate round-robin, one group per tick.  Old clients
 * that send a root `series` array are parsed as a single group.  The HTTP layer
 * owns the only static instance and the engine keeps a const pointer to it for
 * the duration of a session (the parser refuses to reuse it while running).
 */
typedef struct {
    uint8_t trigger;                /* curve_trigger_t */
    bool halt;
    uint8_t group_count;            /* 1..CURVE_MAX_GROUPS */
    uint16_t series_count;          /* total series across all groups (<=256) */
    uint16_t sample_bytes;          /* max group payload (CVF2 frame_bytes) */
    uint32_t interval_us;           /* periodic period / breakpoint throttle */
    uint32_t breakpoint_address;    /* CURVE_TRIGGER_BREAKPOINT: raw Thumb addr */
    bool needs_regs;                /* any expr depends on a core register */
    bool want_pc;                   /* CVF2 PC_PRESENT opt-out (default true) */
    uint32_t watch_address;         /* CURVE_TRIGGER_WATCHPOINT: data address */
    uint8_t watch_size;             /* 1/2/4 bytes */
    uint8_t watch_access;           /* curve_watch_access_t (write/read/readwrite) */
    /* P1.8.1 optional second DWT comparator (same trigger, different address).
     * Kept as the legacy view of watchpoint slot 1 (JSON `watch2`, status
     * watchpoint2_*); the engine reads the canonical arrays below. */
    bool watch2_enabled;
    uint32_t watch2_address;
    uint8_t watch2_size;            /* 1/2/4 bytes */
    uint8_t watch2_access;          /* curve_watch_access_t */
    /* P1-12 canonical DWT watchpoint list (0..4 comparators).  Slot 0 mirrors
     * watch_address/watch_size/watch_access, slot 1 mirrors watch2_*. */
    uint8_t watch_count;            /* 0 = not a watchpoint session; 1..4 */
    uint32_t watch_addr[CURVE_MAX_WATCHPOINTS];
    uint8_t watch_sz[CURVE_MAX_WATCHPOINTS];   /* 1/2/4 bytes */
    uint8_t watch_acc[CURVE_MAX_WATCHPOINTS];  /* curve_watch_access_t */
    /* P1.8.2 optional one-shot function snapshot (breakpoint trigger only) */
    bool snapshot_enabled;          /* true when the request carried snapshot_after */
    uint8_t snapshot_after;         /* post-trigger frames, 0..32 */
    bool backtrace;                 /* prototype: capture r7 chain at trigger hits */
    /* P1.8 optional trigger_condition (independent of the trigger mode) */
    bool cond_enabled;
    bool cond_signed;
    bool cond_stop_after;
    uint8_t cond_size;              /* 1/2/4 */
    uint8_t cond_op;                /* curve_cond_op_t */
    uint8_t cond_mode;              /* curve_cond_mode_t */
    uint32_t cond_address;
    int32_t cond_value;
    uint8_t reg_count;              /* unique core registers to read per tick */
    uint8_t regs[CURVE_MAX_CORE_REGS];
    uint16_t pc_gate_count;         /* series that need r15 (gate or variants) */
    curve_group_t groups[CURVE_MAX_GROUPS];
    /* Weighted round-robin expansion: group index repeated `weight` times.
     * schedule_len = sum(weights) <= CURVE_MAX_GROUPS * CURVE_MAX_GROUP_WEIGHT. */
    uint8_t schedule[CURVE_MAX_GROUPS * CURVE_MAX_GROUP_WEIGHT];
    uint8_t schedule_len;
    curve_series_t series[CURVE_MAX_SERIES];
    curve_variant_t variants[CURVE_MAX_VARIANTS];
    uint16_t variant_used;
    curve_expr_t pool[CURVE_EXPR_POOL_MAX];
    uint16_t pool_used;
} curve_config_t;

/** True when a series with divider `sample_every` must be read on the tick
 *  whose per-series counter is `tick` (1 = every tick, i.e. always). */
static inline bool curve_sample_due(uint8_t sample_every, uint8_t tick)
{
    if (sample_every <= CURVE_SAMPLE_EVERY_MIN) {
        return true;
    }
    return (uint8_t)(tick % sample_every) == 0U;
}

/** True when sampling this series requires the tick's PC (single-style gate or
 *  loclist variants). */
static inline bool curve_series_needs_pc(const curve_series_t *series)
{
    return (series->flags & (CURVE_SERIES_FLAG_PC_GATE | CURVE_SERIES_FLAG_VARIANTS)) != 0U;
}

/**
 * @brief Select the expression index to evaluate for a series at PC `pc`.
 *
 * - loclist variants: first variant whose [pc_low, pc_high) contains the PC;
 * - single-style pc gate: the series expression when in scope;
 * - plain series: its expression (PC ignored).
 *
 * @return pool index, or CURVE_EXPR_NONE when the current PC is out of scope
 *         (caller zeroes the series bytes and increments out_of_scope).
 */
uint16_t curve_series_expr_for_pc(const curve_config_t *cfg,
                                  const curve_series_t *series, uint32_t pc);

/* ---- status / selftest snapshots ----------------------------------------- */
typedef struct {
    bool running;
    bool halt;
    uint32_t interval_us;
    uint16_t sample_bytes;
    uint16_t series_count;
    uint8_t group_count;
    uint8_t current_group;
    uint32_t frames;             /* frames captured since start (contract) */
    uint32_t overwritten;        /* frames dropped before a client read them */
    uint32_t ring_capacity;      /* ring slots (256..512, see above) */
    uint32_t served_seq;         /* highest seq handed to a client (0=none) */
    uint8_t trigger;             /* curve_trigger_t */
    uint32_t breakpoint_address; /* breakpoint trigger: FPB address */
    uint32_t hits;               /* FPB breakpoint hits */
    uint32_t sampled;            /* breakpoint hits actually written to ring */
    uint32_t throttled;          /* hits dropped by interval throttling */
    uint32_t invalid;            /* invalid series values, bytes zeroed */
    uint32_t out_of_scope;       /* PC-gated series skipped outside [low,high) */
    bool pc_available;           /* CVF2 frames carry PC (halt/breakpoint/watch) */
    uint32_t last_pc;            /* last captured PC (Thumb bit0 cleared) */
    bool watchpoint_armed;       /* first DWT comparator currently armed */
    uint32_t watchpoint_address; /* DWT watchpoint address (0 when unrelated) */
    uint32_t watchpoint_hits;    /* DWT watchpoint triggers observed (total) */
    uint32_t trigger_seq;        /* seq of the last trigger-event frame (0=none) */
    /* P1.8.1/P1-12 DWT comparator status (up to 4 watchpoints) */
    uint8_t watch_count;         /* configured watchpoints 0..4 */
    bool watchpoint2_armed;      /* second comparator armed */
    uint32_t watchpoint2_address;
    uint32_t watchpoint2_hits;   /* triggers observed on the second watchpoint */
    bool watchpoint3_armed;      /* third comparator armed (P1-12) */
    uint32_t watchpoint3_address;
    uint32_t watchpoint3_hits;
    bool watchpoint4_armed;      /* fourth comparator armed (P1-12) */
    uint32_t watchpoint4_address;
    uint32_t watchpoint4_hits;
    uint8_t last_watch_index;    /* 1..4 = last watchpoint that triggered, 0=none */
    /* P1.8.2 function snapshot status */
    uint8_t snapshot_state;      /* curve_snapshot_state_t */
    uint8_t snapshot_after;      /* configured post-trigger frame count */
    uint32_t snapshot_trigger_seq; /* seq of the trigger frame (0=none) */
    uint32_t snapshot_pre_frames;  /* retained pre-trigger frames at hit time */
    uint32_t snapshot_post_frames; /* post-trigger frames captured so far */
    /* P1.8.3 prototype status */
    bool trace_active;           /* UART trace capture is subscribed */
    uint32_t trace_bytes;        /* total target bytes captured this trace run */
    uint32_t trace_dropped;      /* bytes evicted from the 2 KiB trace ring */
    bool backtrace_valid;        /* last trigger hit produced a backtrace */
    uint8_t backtrace_count;     /* captured PCs of the last trigger hit */
    /* P1.8 trigger_condition status */
    bool condition_armed;        /* configured and still evaluating (once latch clears) */
    bool condition_met;          /* latched true after the first match */
    uint32_t condition_hits;     /* number of matching ticks */
    uint32_t condition_trigger_seq; /* seq of the last condition-match frame */
    /* object-watch capacity (compile-time limits, reported for the Web UI) */
    uint16_t max_groups;
    uint16_t max_series;
    uint16_t max_series_per_group;
    uint16_t expr_pool_max;
    uint16_t expr_pool_used;
    uint32_t errors;             /* failed sample ticks */
    uint32_t consecutive_errors;
    /* protocol v3 exact per-series refresh rate (sample_every) */
    uint8_t sample_every_max;    /* largest divider configured this session */
    uint32_t series_stale;       /* stale (skipped) series slots in session */
    /* heap snapshot at status time (read-only diagnostics; 0 when unknown) */
    uint32_t heap_free;
    uint32_t heap_largest;
    uint32_t link_hz;
    uint32_t dpidr;
    uint32_t last_tick_us;
    uint32_t last_error;         /* esp_err_t of the last failed operation */
    char last_err[24];           /* esp_err_to_name(last_error), "" = none */
    char message[64];            /* diagnostic / last stop reason */
} curve_status_t;

typedef struct {
    uint32_t iterations;         /* requested N */
    uint32_t completed;          /* successful iterations measured */
    uint32_t failed;
    uint32_t min_us;
    uint32_t median_us;
    uint32_t p90_us;
    uint32_t max_us;
    uint32_t halt_us;            /* per-iteration stage averages (us) */
    uint32_t regs_us;
    uint32_t mem_us;
    uint32_t resume_us;
    uint32_t link_hz;
    uint32_t dpidr;
} curve_selftest_t;

/* ---- expression evaluation (curve_expr.c) -------------------------------- */
typedef enum {
    CURVE_EVAL_OK = 0,
    CURVE_EVAL_INVALID,  /* expression not evaluable / hole address: no read */
    CURVE_EVAL_BUS,      /* AHB-AP read failed: recover the bus */
} curve_eval_result_t;

curve_eval_result_t curve_expr_eval(const curve_config_t *cfg, uint16_t expr_index,
                                    const uint32_t *reg_values,
                                    const bool *reg_valid,
                                    uint32_t *address);

/* P1.8 object-watch optimization: per-tick memoization of pointer reads.
 * Object member paths (e.g. 324 fields of one object) dereference the same
 * intermediate pointers several times per tick.  The engine enables the cache
 * only while the core is halted (a running core may change memory between two
 * reads, so no-halt ticks keep it disabled).  Cache is cleared per tick. */
void curve_expr_cache_begin(bool core_halted);
void curve_expr_cache_end(void);

/* ---- CVF2 wire framing (curve_cvf2.c) ------------------------------------
 * magic "CVF2", version 2, flags bit0=gap, count u16, first_seq u32,
 * frame_bytes u16 (max group payload), reserved u16.
 * Frame: seq u32, timestamp_us u64, group u8, payload_len u16, payload bytes. */
#define CURVE_CVF2_FLAG_GAP        0x01U
#define CURVE_CVF2_FLAG_PC_PRESENT 0x02U
#define CURVE_CVF2_FLAG_TRIGGERED  0x04U /* batch contains trigger-event frames */
#define CURVE_CVF2_FLAG_CONDITION  0x08U /* batch contains condition-match frames */
/* P0-3 per-series valid/stale map (protocol v3, backward compatible).
 * Set when at least one frame in the batch has a stale (sample_every-skipped)
 * series.  When set, `count * CURVE_CVF2_SERIES_MASK_BYTES` footer bytes are
 * appended AFTER the last frame record (not in the frame layout, so decoders
 * that do not know this flag still parse every frame and simply ignore the
 * trailing bytes): one little-endian u64 bitmask per frame in frame order.
 * bit i = series slot i within that frame's group (i.e. the group's
 * cfg.series[series_first + i]) was sampled on this tick (1) or is stale (0,
 * payload bytes are zero and the previous value should be held). */
#define CURVE_CVF2_FLAG_SERIES_MASK    0x10U
#define CURVE_CVF2_SERIES_MASK_BYTES   8U /* u64 bitmask covers 64 series/group */

/** Write the 16-byte CVF2 batch header; returns 16.
 *  flags bit1 = PC_PRESENT when `pc_present` is set. */
size_t curve_cvf2_write_header(uint8_t *buf, uint16_t count, uint32_t first_seq,
                               uint16_t frame_bytes, bool gap, bool pc_present,
                               bool triggered, bool condition);

/** Same as curve_cvf2_write_header() but ORs `extra_flags` into the flags byte
 *  (protocol v3 uses CURVE_CVF2_FLAG_SERIES_MASK here).  Kept separate so the
 *  original function/ABI and host test stay stable. */
size_t curve_cvf2_write_header_ex(uint8_t *buf, uint16_t count,
                                  uint32_t first_seq, uint16_t frame_bytes,
                                  uint8_t extra_flags, bool gap,
                                  bool pc_present, bool triggered,
                                  bool condition);

/** Append one 8-byte little-endian per-series valid bitmask to `buf`. */
size_t curve_cvf2_write_series_mask(uint8_t *buf, uint64_t mask);

/** Write the CVF2 frame metadata; returns 19 with PC, 15 without.
 *  PC (Thumb bit0 already cleared by the caller) goes after payload_len. */
size_t curve_cvf2_write_frame_meta(uint8_t *buf, uint32_t seq,
                                   uint64_t timestamp_us, uint8_t group,
                                   uint16_t payload_len, uint32_t pc,
                                   bool pc_present);

/* ---- frame ring bookkeeping (curve_ring.c) -------------------------------
 * Pure C, no RTOS/esp dependencies: the engine owns the payload arrays and
 * calls these helpers under its mutex; the host test exercises the exact same
 * code for the overwritten/gap semantics. */
typedef struct {
    uint32_t capacity;     /* slots, 256..512 */
    uint32_t next_seq;     /* seq of the next frame to write (starts at 1) */
    uint32_t count;        /* retained frames, <= capacity */
    uint32_t served_seq;   /* highest seq delivered to a client (0 = none) */
    uint32_t frames_total; /* frames captured this session */
    uint32_t overwritten;  /* frames evicted before any client received them */
} curve_ring_t;

/**
 * @brief Ring capacity for one frame payload, bounded by CURVE_RING_BYTES_BUDGET.
 *
 * @param frame_bytes max group payload bytes
 * @param with_pc     true when the session stores a per-frame PC (CVF2 19 B
 *                    metadata instead of 15 B), so the budget accounts for it.
 */
uint32_t curve_ring_capacity_for(uint32_t frame_bytes, bool with_pc);

/** Reset a ring for a new sampling session. */
void curve_ring_reset(curve_ring_t *ring, uint32_t capacity);

/**
 * @brief Prepare one frame write: slot index / seq to store, and account for a
 *        frame that is evicted by this write.  The frame is only counted as
 *        overwritten when its seq was never delivered to a client
 *        (evicted_seq > served_seq); a client that keeps consuming at the
 *        sampling rate therefore never increments `overwritten`.
 */
void curve_ring_begin_write(curve_ring_t *ring, uint32_t *slot, uint32_t *seq);

/** Commit the frame prepared by curve_ring_begin_write(). */
void curve_ring_commit_write(curve_ring_t *ring);

/**
 * @brief Plan a CVF1 read: first seq and frame count available for `since`.
 * @return frame count (0 = nothing new; gap=true when `since` is older than
 *         the oldest retained frame).
 */
uint32_t curve_ring_plan_read(const curve_ring_t *ring, uint32_t since,
                              uint32_t max, uint32_t *first_seq, bool *gap);

/** Advance the served cursor after [first_seq, first_seq+count-1] was sent. */
void curve_ring_mark_served(curve_ring_t *ring, uint32_t first_seq, uint32_t count);

/**
 * @brief Format the GET /api/curve/status JSON body (without HTTP framing).
 *
 * @return snprintf() result (number of bytes that would be written).
 */
int curve_status_format_json(const curve_status_t *status, char *buf, size_t buf_len);

/* ---- JSON config parser (curve_json.c) ----------------------------------- */
esp_err_t curve_config_parse(const char *json, size_t len, curve_config_t *cfg,
                             char *err, size_t err_len);

/** Parse a compact v3 expression prefix (A/R/C/+ /D encoding).  Shared by the
 *  JSON `expr_s`/variants parser and the binary (CBS1) parser. */
esp_err_t curve_parse_expr_compact(const char **cursor, curve_config_t *cfg,
                                   uint16_t *out_index, uint32_t depth,
                                   char *err, size_t err_len);

/** Cross-field validation + derived fields (schedule, needs_regs, group regs,
 *  halt/interval checks) for an already filled curve_config_t.  Shared by the
 *  JSON and binary parsers; returns ESP_ERR_INVALID_ARG with err set. */
esp_err_t curve_config_finalize(curve_config_t *cfg, char *err, size_t err_len);

/** Normalize a target-flash [low, high) PC range (Thumb bit0 cleared); false
 *  when the pair is not a valid flash range.  Shared with the binary parser. */
bool curve_normalize_pc_range(uint32_t pc_low, uint32_t pc_high,
                              uint32_t *low_norm, uint32_t *high_norm);

/* ---- compact binary start request (CBS1, curve_bin.c) ---------------------
 * POST /api/curve/start-bin body: a compact binary encoding of the same
 * curve_config_t the JSON parser produces.  It is parsed STREAMING (the HTTP
 * layer feeds it 256 B chunks from the socket), so the 16 KiB request-body
 * heap block is not needed => no largest-contiguous-block requirement on the
 * fragmented unified NCM+WiFi heap.  Wire layout (all little-endian):
 *
 *   "CBS1" magic u32 | version u8 (1) | flags u8 |
 *   trigger u8 | watch_count u8 |
 *   interval_us u32 | breakpoint_address u32 |
 *   cond_address u32 | cond_value i32 |
 *   cond_size u8 | cond_op u8 | cond_mode u8 | cond_flags u8 |
 *   snapshot_after u8 | group_count u8 | reserved u16 (0)
 *   flags: bit0 halt, bit1 want_pc, bit2 backtrace,
 *          bit3 trigger_condition present, bit4 snapshot_after present
 *   cond_flags: bit0 signed, bit1 stop_after
 *   watch_count entries: address u32, size u8, access u8   (0..4)
 *   group_count groups:
 *     weight u8 | series_count u8 | reserved u16 (0)
 *     series_count series:
 *       size u8 | flags u8 | sample_every u8 | variant_count u8 |
 *       pc_low u32 | pc_high u32 |
 *       u16 expr_len | expr_len bytes ASCII compact expr (empty = none) |
 *       variant_count variants:
 *         pc_low u32 | pc_high u32 | u16 expr_len | expr bytes
 *   series flags: bit0 pc gate (pc_low/pc_high used), rest reserved (0).
 *   Request body max CURVE_BIN_BODY_MAX (streamed, not buffered as one block).
 * A malformed/truncated body is rejected with ESP_ERR_INVALID_ARG and err. */
#define CURVE_BIN_MAGIC             0x31534243U /* bytes 'C','B','S','1' LE */
#define CURVE_BIN_VERSION           1U
#define CURVE_BIN_BODY_MAX          CURVE_JSON_BODY_MAX
#define CURVE_BIN_EXPR_MAX          64U
#define CURVE_BIN_FLAG_HALT         0x01U
#define CURVE_BIN_FLAG_WANT_PC      0x02U
#define CURVE_BIN_FLAG_BACKTRACE    0x04U
#define CURVE_BIN_FLAG_CONDITION    0x08U
#define CURVE_BIN_FLAG_SNAPSHOT     0x10U
#define CURVE_BIN_COND_FLAG_SIGNED  0x01U
#define CURVE_BIN_COND_FLAG_STOP    0x02U
#define CURVE_BIN_SERIES_FLAG_PC_GATE 0x01U

/** Streaming byte source used by the binary parser: fill exactly `len` bytes
 *  at `dst` or return an error (short read / socket timeout). */
typedef esp_err_t (*curve_bin_read_fn)(void *ctx, void *dst, size_t len);

/** Parse a CBS1 request from a streaming source into cfg (memset + full
 *  validation, same post-conditions as curve_config_parse). */
esp_err_t curve_bin_parse(curve_bin_read_fn read_fn, void *ctx,
                          curve_config_t *cfg, char *err, size_t err_len);

/** Parse a CBS1 request already in memory (host test / unit use). */
esp_err_t curve_bin_parse_mem(const uint8_t *data, size_t len,
                              curve_config_t *cfg, char *err, size_t err_len);

/* ---- sampling engine (curve_engine.c) ------------------------------------ */
esp_err_t curve_engine_init(void);

/** Start sampling.  Blocks until the DAP bring-up finished (or failed). */
/**
 * @brief True when the engine is idle and the HTTP layer may overwrite the
 *        config buffer it passed to curve_engine_start() (that buffer is owned
 *        by the HTTP layer and only referenced by the engine while running).
 */
bool curve_engine_is_idle(void);

/** Release the retained frame ring while idle.  The ring can be tens of KiB
 *  after a large group session; freeing it before parsing the next start
 *  request preserves heap headroom for the cJSON parse (which otherwise can
 *  fail with "invalid JSON object" while the browser keeps a WebSocket open). */
void curve_engine_release_ring(void);

esp_err_t curve_engine_start(const curve_config_t *cfg, char *err, size_t err_len);

/** Stop sampling: resumes the target, releases the DAP and waits for the
 *  worker to go idle.  Idempotent. */
esp_err_t curve_engine_stop(uint32_t timeout_ms, char *err, size_t err_len);

/** Phase 0 selftest (blocks the caller until it finished). */
esp_err_t curve_engine_selftest(curve_selftest_t *out, char *err, size_t err_len);

void curve_engine_status(curve_status_t *out);

/** Build a complete CVF2 response for GET /api/curve/frames (protocol v2:
 *  group u8 + payload_len u16 per frame).  Allocates the response buffer
 *  (caller frees) and returns *no_data=true when there is no frame newer than
 *  `since`. */
esp_err_t curve_engine_alloc_frames(uint32_t since, uint32_t max,
                                    uint8_t **out_buf, size_t *out_len,
                                    bool *no_data);

/** Current ring seq window (oldest retained / newest); false when no frames. */
bool curve_engine_ring_seqs(uint32_t *oldest_seq, uint32_t *latest_seq);

/** Copy the backtrace captured at the last trigger hit (prototype; P1.8.2).
 *  Returns false when no backtrace was captured in this session. */
bool curve_engine_backtrace(curve_backtrace_t *out);

/* ---- WebSocket frame push (curve_ws.c / curve_ws_core.c) ------------------
 * GET /ws/curve WebSocket endpoint: after POST /api/curve/start a browser can
 * subscribe and receive CVF2 batches pushed by the sampler (same wire format
 * as GET /api/curve/frames).  HTTP polling keeps working unchanged. */
void curve_ws_notify_frame(void);

/* ---- tf_swd access layer (components/target_flash/src) -------------------
 * tf_swd.h is a private header of target_flash, so the small subset needed by
 * the sampler is declared here.  The target flash component is a hard link
 * dependency (see CMakeLists.txt); the DAP engine itself is taken over with
 * the documented hand-off protocol. */
esp_err_t tf_swd_link_init(uint32_t clock_hz, uint32_t *dpidr);
void tf_swd_set_forced_clock(uint32_t clock_hz);
uint32_t tf_swd_get_forced_clock(void);
esp_err_t tf_swd_recover_bus(void);
esp_err_t tf_swd_read_u32(uint32_t addr, uint32_t *value);
esp_err_t tf_swd_write_u32(uint32_t addr, uint32_t value);
esp_err_t tf_swd_read_mem(uint32_t addr, void *buf, size_t len);
esp_err_t tf_swd_halt(void);
esp_err_t tf_swd_resume(void);
esp_err_t tf_swd_is_halted(bool *halted);
esp_err_t tf_swd_read_reg(uint8_t reg, uint32_t *value);

#ifdef __cplusplus
}
#endif

#endif /* CURVE_PRIV_H */
