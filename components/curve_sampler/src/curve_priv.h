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
 *   CURVE_MAX_SERIES_PER_GROUP   32 (Web API limit)
 *   CURVE_MAX_SERIES            256 (total across groups)
 *   CURVE_MAX_VARIANTS_PER_SERIES 16
 *   CURVE_MAX_VARIANTS          256 (total loclist variant pool)
 *   CURVE_EXPR_POOL_MAX         768 expression nodes (12 B each, ~9 KiB);
 *                                   every expr chain node (single expr or
 *                                   variant expr, depth <= 8) comes from here.
 * A request that exceeds the variant/expr pool is rejected with a clear parse
 * error (groups/series limits themselves follow the contract exactly). */
#define CURVE_MAX_GROUPS            8U
#define CURVE_MAX_SERIES_PER_GROUP  32U
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
#define CURVE_MAX_FRAMES_PER_REQ    64U

/* Frame ring.  The contract recommends >= 256 frames; the sampler uses up to
 * 512 when the per-frame payload is small and clamps down for large payloads
 * so the heap cost stays bounded.  Phase 2.5 allows 32 series (up to 256 B
 * payload) and the minimum was lowered to 128 frames to keep the worst-case
 * allocation at ~34 KiB:
 *   capacity = clamp(CURVE_RING_BYTES_BUDGET / (12 + frame_bytes), 128, 512)
 *   <= 116 B/frame -> >= 256 frames (e.g. 32 x 4 B: 292 frames / ~33 KiB)
 *   128 B/frame    -> 234 frames / ~33 KiB
 *   256 B/frame    -> 128 frames / ~34 KiB
 * Allocated lazily on the first start and reused across sessions. */
#define CURVE_RING_FRAMES_MIN       128U
#define CURVE_RING_FRAMES_MAX       512U
#define CURVE_RING_BYTES_BUDGET     (8U * 1024U)
/* Protocol v2 ring/CVF2 per-frame metadata: seq (u32) + timestamp_us (u64) +
 * group (u8) + payload_len (u16) = 15 B.  The ring slot also reserves the
 * maximum group payload (frame_bytes) after that. */
#define CURVE_FRAME_META_BYTES      15U
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
} curve_trigger_t;

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

/** Round-robin increment for the current sampling group. */
static inline uint8_t curve_next_group(uint8_t current, uint8_t group_count)
{
    if (group_count == 0U) {
        return 0U;
    }
    current++;
    return (current >= group_count) ? 0U : current;
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
    uint32_t id;              /* echoed back by callers; frames ordered by index */
    uint32_t pc_low;          /* single-style PC gate bounds (normalized) */
    uint32_t pc_high;         /* valid when CURVE_SERIES_FLAG_PC_GATE is set */
    uint16_t expr;            /* single expression index (CURVE_EXPR_NONE when
                                 the series only has loclist variants) */
    uint16_t variant_first;   /* first index into curve_config_t.variants */
    uint8_t size;             /* 1..8 bytes read from the evaluated address */
    uint8_t flags;            /* CURVE_SERIES_FLAG_* */
    uint8_t variant_count;    /* 0 = single expr/pc-gate series, 1..16 otherwise */
    uint8_t reserved;
} curve_series_t;

/** One sampling group: a contiguous slice of curve_config_t.series[]. */
typedef struct {
    uint16_t series_first;
    uint16_t series_count;
    uint16_t sample_bytes;    /* sum of this group's series sizes (<= 256) */
    bool pc_needed;           /* any series needs r15 (pc gate or variants) */
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
    uint8_t reg_count;              /* unique core registers to read per tick */
    uint8_t regs[CURVE_MAX_CORE_REGS];
    uint16_t pc_gate_count;         /* series that need r15 (gate or variants) */
    curve_group_t groups[CURVE_MAX_GROUPS];
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

/* ---- CVF2 wire framing (curve_cvf2.c) ------------------------------------
 * magic "CVF2", version 2, flags bit0=gap, count u16, first_seq u32,
 * frame_bytes u16 (max group payload), reserved u16.
 * Frame: seq u32, timestamp_us u64, group u8, payload_len u16, payload bytes. */
#define CURVE_CVF2_FLAG_GAP 0x01U

/** Write the 16-byte CVF2 batch header; returns 16. */
size_t curve_cvf2_write_header(uint8_t *buf, uint16_t count, uint32_t first_seq,
                               uint16_t frame_bytes, bool gap);

/** Write the 15-byte CVF2 frame metadata; returns 15. */
size_t curve_cvf2_write_frame_meta(uint8_t *buf, uint32_t seq,
                                   uint64_t timestamp_us, uint8_t group,
                                   uint16_t payload_len);

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

/** Ring capacity for one frame payload (256..512, 32 KiB budget). */
uint32_t curve_ring_capacity_for(uint32_t frame_bytes);

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
