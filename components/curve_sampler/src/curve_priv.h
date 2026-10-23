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
 *   CURVE_MAX_SERIES            512 (total across groups)
 *   CURVE_MAX_VARIANTS_PER_SERIES 16
 *   CURVE_MAX_VARIANTS          256 (total loclist variant pool)
 *   CURVE_EXPR_POOL_MAX        2048 expression nodes (12 B each, 24 KiB).
 *     Object member paths share their prefix: the parser deduplicates identical
 *     (kind, inner, offset/reg/address) nodes, so 324 object fields typically
 *     need only a few hundred nodes instead of ~1600; 2048 still leaves headroom
 *     for 512 independent series with 3-4 node chains.
 *   CURVE_JSON_BODY_MAX        24576 request bytes (512-series/324-field stop
 *     requests are ~16-20 KiB even in the compact v3 format).
 *                                   every expr chain node (single expr or
 *                                   variant expr, depth <= 8) comes from here.
 * A request that exceeds the variant/expr pool is rejected with a clear parse
 * error (groups/series limits themselves follow the contract exactly). */
#define CURVE_MAX_GROUPS            8U
#define CURVE_MAX_SERIES_PER_GROUP  64U
#define CURVE_MAX_SERIES            512U
#define CURVE_MAX_VARIANTS_PER_SERIES 16U
#define CURVE_MAX_VARIANTS          256U
#define CURVE_EXPR_POOL_MAX         768U
#define CURVE_MAX_EXPR_DEPTH        8U
#define CURVE_MAX_SAMPLE_BYTES      256U
#define CURVE_MIN_INTERVAL_US       1000U
#define CURVE_MAX_INTERVAL_US       200000U
#define CURVE_SERIES_SIZE_MIN       1U
#define CURVE_SERIES_SIZE_MAX       8U
#define CURVE_JSON_BODY_MAX         20480U
#define CURVE_MAX_FRAMES_PER_REQ    64U

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

/* ---- P1.8/P1.9: DWT data watchpoint --------------------------------------
 * Cortex-M7 DWT comparator 0: DWT_COMP0 = address, DWT_MASK0 = log2(size),
 * DWT_FUNCTION0 = 5 (read) / 6 (write) / 7 (read+write) with MATCHED bit24
 * after a hit, DEMCR.TRCENA (bit24) must be set.  The sampler saves and
 * restores DEMCR/COMP0/MASK0/FUNCTION0 and disarms comparator 0 before every
 * resume (same throttle flow as the FPB breakpoint trigger). */
#define CURVE_TRIGGER_WATCH_MIN_US   5000U
#define CURVE_TRIGGER_WATCH_MAX_US   1000000U
#define CURVE_TRIGGER_WATCH_DEFAULT_US 20000U
/* Watch-size checks are 1/2/4 bytes (DWT_MASK = log2(size)). */
#define CURVE_WATCH_SIZE_1           1U
#define CURVE_WATCH_SIZE_2           2U
#define CURVE_WATCH_SIZE_4           4U
#define CURVE_WATCH_DEFAULT_SIZE     4U

typedef enum {
    CURVE_WATCH_ACCESS_WRITE = 0,
    CURVE_WATCH_ACCESS_RW = 1,
} curve_watch_access_t;

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
#define CURVE_DWT_COMP0              0xE0001020U
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

/** "write"/"readwrite" name for a curve_watch_access_t. */
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
    uint8_t reserved;         /* keep explicit: sizeof == 16, 512 series = 8 KiB */
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
    uint8_t watch_access;           /* curve_watch_access_t (write/readwrite) */
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
    bool watchpoint_armed;       /* DWT comparator currently armed */
    uint32_t watchpoint_address; /* DWT watchpoint address (0 when unrelated) */
    uint32_t watchpoint_hits;    /* DWT watchpoint triggers observed */
    uint32_t trigger_seq;        /* seq of the last trigger-event frame (0=none) */
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

/** Write the 16-byte CVF2 batch header; returns 16.
 *  flags bit1 = PC_PRESENT when `pc_present` is set. */
size_t curve_cvf2_write_header(uint8_t *buf, uint16_t count, uint32_t first_seq,
                               uint16_t frame_bytes, bool gap, bool pc_present,
                               bool triggered, bool condition);

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
