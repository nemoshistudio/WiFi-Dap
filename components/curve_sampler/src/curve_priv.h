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

/* ---- contract limits (docs/curve-local-sampler.md §1.1) ------------------ */
#define CURVE_MAX_SERIES            16U
#define CURVE_MAX_EXPR_DEPTH        8U
#define CURVE_MAX_SAMPLE_BYTES      256U
#define CURVE_MIN_INTERVAL_US       5000U
#define CURVE_MAX_INTERVAL_US       200000U
#define CURVE_SERIES_SIZE_MIN       1U
#define CURVE_SERIES_SIZE_MAX       8U
#define CURVE_JSON_BODY_MAX         4096U
#define CURVE_MAX_FRAMES_PER_REQ    64U

/* Frame ring.  The contract recommends >= 256 frames; the sampler uses up to
 * 512 when the per-frame payload is small and clamps down to 256 when the
 * payload is large so the heap cost stays bounded:
 *   capacity = clamp(CURVE_RING_BYTES_BUDGET / (12 + frame_bytes), 256, 512)
 *   16 series x 8 B  (128 B/frame) -> 256 frames / ~35 KiB
 *   4 series x 4 B   ( 16 B/frame) -> 512 frames / ~14 KiB
 * Allocated lazily on the first start and reused across sessions. */
#define CURVE_RING_FRAMES_MIN       256U
#define CURVE_RING_FRAMES_MAX       512U
#define CURVE_RING_BYTES_BUDGET     (32U * 1024U)
/* Per-frame ring metadata: seq (u32) + timestamp_us (u64). */
#define CURVE_FRAME_META_BYTES      12U
#define CURVE_FRAME_HEADER_BYTES    16U

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

typedef struct {
    uint32_t id;      /* echoed back by callers; frames are ordered by index */
    uint8_t size;     /* 1..8 bytes read from the evaluated address */
    uint16_t expr;    /* index into curve_config_t.pool */
} curve_series_t;

/** Immutable sampling configuration (parsed before the worker is started). */
typedef struct {
    uint32_t interval_us;
    bool halt;
    uint8_t series_count;
    uint16_t sample_bytes;          /* sum(series[].size) */
    bool needs_regs;                /* any expr depends on a core register */
    uint8_t reg_count;              /* unique core registers to read per tick */
    uint8_t regs[CURVE_MAX_CORE_REGS];
    curve_series_t series[CURVE_MAX_SERIES];
    /* One linear chain per series, depth <= 8 -> 16 * 8 nodes worst case. */
    curve_expr_t pool[CURVE_MAX_SERIES * CURVE_MAX_EXPR_DEPTH];
    uint16_t pool_used;
} curve_config_t;

/* ---- status / selftest snapshots ----------------------------------------- */
typedef struct {
    bool running;
    bool halt;
    uint32_t interval_us;
    uint16_t sample_bytes;
    uint8_t series_count;
    uint32_t frames;             /* frames captured since start (contract) */
    uint32_t overwritten;        /* frames dropped before a client read them */
    uint32_t ring_capacity;      /* ring slots (256..512, see above) */
    uint32_t served_seq;         /* highest seq handed to a client (0=none) */
    uint32_t invalid;            /* invalid series values, bytes zeroed */
    uint32_t errors;             /* failed sample ticks */
    uint32_t consecutive_errors;
    uint32_t link_hz;
    uint32_t dpidr;
    uint32_t last_tick_us;
    uint32_t last_error;         /* esp_err_t of the last failed operation */
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

/* ---- JSON config parser (curve_json.c) ----------------------------------- */
esp_err_t curve_config_parse(const char *json, size_t len, curve_config_t *cfg,
                             char *err, size_t err_len);

/* ---- sampling engine (curve_engine.c) ------------------------------------ */
esp_err_t curve_engine_init(void);

/** Start sampling.  Blocks until the DAP bring-up finished (or failed). */
esp_err_t curve_engine_start(const curve_config_t *cfg, char *err, size_t err_len);

/** Stop sampling: resumes the target, releases the DAP and waits for the
 *  worker to go idle.  Idempotent. */
esp_err_t curve_engine_stop(uint32_t timeout_ms, char *err, size_t err_len);

/** Phase 0 selftest (blocks the caller until it finished). */
esp_err_t curve_engine_selftest(curve_selftest_t *out, char *err, size_t err_len);

void curve_engine_status(curve_status_t *out);

/** Build a complete CVF1 response for GET /api/curve/frames.  Allocates the
 *  response buffer (caller frees) and returns *no_data=true when there is no
 *  frame newer than `since`. */
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
