/**
 * @file SWD_Fast_GPIO.c
 * @brief ESP32-S3 dedicated-GPIO SWD engine - all SWD rates (M5).
 *
 * M5 (user request): the ESP32-S3 no longer uses the old GPIO/SPI SWD
 * backends.  Every SWD transfer (about 100 kHz .. 20 MHz on the project's
 * 160 MHz CPU build, up to 24 MHz at 240 MHz), every SWDJ line reset /
 * JTAG-to-SWD / dormant wake-up sequence and every SWD_Sequence capture goes
 * through this engine.  Full rationale, timing budget, wiring patch and HIL
 * checklist: docs/swd-23mhz-engine.md.
 *
 * Two bit engines, one protocol layer
 * -----------------------------------
 * 1. Fast path - fully unrolled Xtensa assembly for half periods of
 *    4..20 CPU cycles.  One `wur.gpio_out` writes SWCLK and SWDIO together, so
 *    every edge is one instruction and the bit clock is exactly
 *    CPU_CLOCK / (2 * half_cycles).  The level set is chosen so that a host
 *    request is served by the fastest level that is not faster than the
 *    request; above the fastest level the fastest level is used and below the
 *    slowest level the paced path takes over.
 * 2. Paced path - C loops for all slower rates (and for every protocol clock
 *    that is not part of a data bit: turnaround, idle cycles, WAIT/FAULT dummy
 *    phases, error back-off).  Every half period is scheduled from the actual
 *    previous edge (`t_next = t_edge + half`), and the cycle counter is only
 *    used to wait, so a delayed edge (interrupt) can never produce a too-short
 *    next half period: the engine is always *slower* than requested, never
 *    faster.  Frequency error from the loop overhead is a few percent down to
 *    ~15-20 % at the boundary (about 3.5 MHz), inside the "0 .. -25 %" budget.
 *
 * Protocol coverage (semantics of the legacy SWD_Transfer_GPIO / SWJ_Sequence /
 * SWD_Sequence, documented in doc comments below):
 *  - 8-bit request header, turnaround, 3 ACK bits, 32-bit data + parity,
 *    parity check, timestamp, idle cycles, WAIT/FAULT and protocol-error
 *    handling;
 *  - SWJ_Sequence: 1..256 bits, LSB first, host drives SWDIO (line reset,
 *    0xE79E JTAG-to-SWD, dormant wake-up);
 *  - SWD_Sequence (DIN): 1..64 captured bits, host releases SWDIO.
 *
 * Fail-safe: `SWD_UltraFast_Available()` validates the CORE1 dedicated-GPIO
 * pin routing and the calling core.  If it fails, the SWD entry points report
 * errors (DAP_TRANSFER_ERROR / 0) and there is **no** fallback to the legacy
 * GPIO backends on the ESP32-S3.  On other targets (ESP32/C3/C5), when the
 * Kconfig option is off or on JTAG this file compiles to stubs and the legacy
 * paths in SW_DP.c are used unchanged.
 *
 * Hardware status: the fast path 12/16 MHz levels were validated on an
 * STM32H723 in the M4/M5 hardware window (docs/dap-tcp-backend.md §12);
 * 20 MHz is marginal on that target/wiring (recommended max 16 MHz), and the
 * paced low-rate path plus the sequence migration are **not yet HIL
 * validated** - see the checklist in docs/swd-23mhz-engine.md.
 */

#include "sdkconfig.h"

#include <stdint.h>

#include "SWD_Fast_GPIO.h"

#if SWD_ULTRA_FAST_ENGINE_ENABLED
/* DAP_config.h must come first: DAP.h keys its SWD members/prototypes off
 * DAP_SWD (defined in DAP_config.h), exactly like SW_DP.c does. */
#include "DAP_config.h"
#endif

#include "cmsis-dap/include/DAP.h"

/* ======================================================================== */
/* Stub implementation (other targets / Kconfig disabled / CPU < 150 MHz)   */
/* ======================================================================== */
#if !SWD_ULTRA_FAST_ENGINE_ENABLED

int SWD_UltraFast_Available(void)
{
    return 0;
}

int SWD_UltraFast_EnsureConfigured(void)
{
    return 0;
}

uint32_t SWD_UltraFast_Enter(uint32_t requested_hz)
{
    (void)requested_hz;
    return 0u;
}

void SWD_UltraFast_Exit(void)
{
}

int SWD_UltraFast_Active(void)
{
    return 0;
}

uint32_t SWD_UltraFast_ActualHz(void)
{
    return 0u;
}

uint8_t SWD_Transfer_Ultra(uint32_t request, uint32_t *data)
{
    (void)request;
    (void)data;
    return DAP_TRANSFER_ERROR;
}

int SWD_UltraFast_SequenceWrite(uint32_t count_bits, const uint8_t *data)
{
    (void)count_bits;
    (void)data;
    return 0;
}

int SWD_UltraFast_SequenceRead(uint32_t count_bits, uint8_t *data)
{
    (void)count_bits;
    (void)data;
    return 0;
}

#else /* SWD_ULTRA_FAST_ENGINE_ENABLED */

/* ======================================================================== */
/* ESP32-S3 dedicated-GPIO SWD engine                                       */
/* ======================================================================== */

#include "cmsis-dap/include/gpio_common.h"  /* soc/..., hal/gpio_ll.h      */
#include "esp_attr.h"                       /* IRAM_ATTR                   */
#include "esp_cpu.h"                        /* esp_cpu_get_core_id(),
                                             * esp_cpu_get_cycle_count()  */

/* Dedicated-GPIO channel bits, fixed by DAP_SPI_Deinit() (spi_switch.c):
 *   bit0 = SWDIO (CORE1_GPIO_OUT0), bit1 = SWCLK (CORE1_GPIO_OUT1)
 *   input bit0 = SWDIO (CORE1_GPIO_IN0)
 * The engine writes the whole 8-channel output register, so it must stay the
 * only user of the CORE1 dedicated GPIO outputs (nothing else in this firmware
 * uses them). */
#define SWD_UD_SWDIO_OUT 0x1u
#define SWD_UD_SWCLK_OUT 0x2u
#define SWD_UD_IDLE      (SWD_UD_SWDIO_OUT | SWD_UD_SWCLK_OUT)
#define SWD_UD_SWDIO_IN  0x1u

/* CPU cycles to wait after re-enabling the SWDIO pad output before driving the
 * first data bit.  GPIO_ENABLE is on the APB bus; the read-back in swd_ud_oen()
 * serialises the write, this is the remaining pad/driver settle margin.  It
 * also works around the A-fork regression where per-transaction OEN toggling
 * without a settle broke the *second* SWD transaction (JUNK ACK). */
#define SWD_UD_OEN_SETTLE_CYCLES 16u

/* Default SWD clock when the host transfers before sending DAP_SWJ_Clock:
 * the project default 2 MHz (DAP_DEFAULT_SWJ_CLOCK, board contract). */
#ifndef DAP_DEFAULT_SWJ_CLOCK
#define DAP_DEFAULT_SWJ_CLOCK 2000000u
#endif
#define SWD_UD_DEFAULT_CLOCK_HZ ((uint32_t)DAP_DEFAULT_SWJ_CLOCK)

/* Paced path: cycles between the read sample and the rising edge.  Covers the
 * wait-loop overshoot plus the sample/branch instructions so a read bit is
 * always sampled before its rising edge. */
#define SWD_UD_PACED_GUARD 6u

/* SWDIO output-enable strategy.
 *
 * 0 (default, protocol-correct): release SWDIO during ACK/read-data/read
 *   turnaround and drive it for the header/write-data phase (XRDAP-ESP's
 *   hardware-validated approach).
 * 1 (A 2025 fork workaround): never touch the pad output enable; SWDIO stays
 *   driven all the time.  The old GPIO fast path on this board behaves like
 *   this (DAP_config.h PIN_SWDIO_OUT_ENABLE/DISABLE are no-ops for S3) and it
 *   is known to pass OpenOCD bring-up, but it violates the SWD turnaround.
 *   Kept as a Kconfig escape hatch for the HIL A/B run.
 */
#if defined(CONFIG_BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON) && (CONFIG_BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON)
#define SWD_UD_OEN_STAY_ON 1
#else
#define SWD_UD_OEN_STAY_ON 0
#endif

/* ---- fast timing levels ------------------------------------------------- */
/* The fastest level is limited by SWD_ULTRA_MIN_HALF_NS (>= 20 ns half): 4
 * CPU cycles on the 160 MHz build (20 MHz), 5 on the 240 MHz build (24 MHz).
 * The slowest fast level is 20 half cycles (4 MHz @160, 6 MHz @240); below
 * that the paced path is used.  The set is chosen so the "pick the fastest
 * level that is not faster than the request" rule keeps the error within
 * 0 .. -25 % for requests >= 4 MHz. */
#if CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ >= 230
#define SWD_UD_CPU_HZ 240000000u
static const uint16_t s_fast_half[] = { 5, 6, 7, 8, 10, 12, 16, 20 };
#define SWD_UD_DEFINE_ALL_LEVELS()                                                     \
    SWD_UD_LEVEL_FUNCS(0, 5, IRAM_ATTR)                                                \
    SWD_UD_LEVEL_FUNCS(1, 6, )                                                         \
    SWD_UD_LEVEL_FUNCS(2, 7, )                                                         \
    SWD_UD_LEVEL_FUNCS(3, 8, )                                                         \
    SWD_UD_LEVEL_FUNCS(4, 10, )                                                        \
    SWD_UD_LEVEL_FUNCS(5, 12, )                                                        \
    SWD_UD_LEVEL_FUNCS(6, 16, )                                                        \
    SWD_UD_LEVEL_FUNCS(7, 20, )
#define SWD_UD_ALL_LEVEL_OPS()                                                         \
    SWD_UD_OPS_ENTRY(0), SWD_UD_OPS_ENTRY(1), SWD_UD_OPS_ENTRY(2),                     \
    SWD_UD_OPS_ENTRY(3), SWD_UD_OPS_ENTRY(4), SWD_UD_OPS_ENTRY(5),                     \
    SWD_UD_OPS_ENTRY(6), SWD_UD_OPS_ENTRY(7)
#else
/* 160 MHz CPU (project default). */
#define SWD_UD_CPU_HZ 160000000u
static const uint16_t s_fast_half[] = { 4, 5, 6, 7, 8, 10, 12, 16, 20 };
#define SWD_UD_DEFINE_ALL_LEVELS()                                                     \
    SWD_UD_LEVEL_FUNCS(0, 4, IRAM_ATTR)                                                \
    SWD_UD_LEVEL_FUNCS(1, 5, )                                                         \
    SWD_UD_LEVEL_FUNCS(2, 6, )                                                         \
    SWD_UD_LEVEL_FUNCS(3, 7, )                                                         \
    SWD_UD_LEVEL_FUNCS(4, 8, )                                                         \
    SWD_UD_LEVEL_FUNCS(5, 10, )                                                        \
    SWD_UD_LEVEL_FUNCS(6, 12, )                                                        \
    SWD_UD_LEVEL_FUNCS(7, 16, )                                                        \
    SWD_UD_LEVEL_FUNCS(8, 20, )
#define SWD_UD_ALL_LEVEL_OPS()                                                         \
    SWD_UD_OPS_ENTRY(0), SWD_UD_OPS_ENTRY(1), SWD_UD_OPS_ENTRY(2),                     \
    SWD_UD_OPS_ENTRY(3), SWD_UD_OPS_ENTRY(4), SWD_UD_OPS_ENTRY(5),                     \
    SWD_UD_OPS_ENTRY(6), SWD_UD_OPS_ENTRY(7), SWD_UD_OPS_ENTRY(8)
#endif

/* ======================================================================== */
/* State and low-level helpers                                              */
/* ======================================================================== */

typedef void (*swd_ud_write_fn)(uint32_t value, uint32_t parity_bit);
typedef uint32_t (*swd_ud_read_fn)(uint32_t lo, uint32_t hi, uint32_t *raw);

struct swd_ud_ops;
typedef void (*swd_ud_clocks_fn)(uint32_t count, uint32_t lo, uint32_t hi);

struct swd_ud_ops {
    swd_ud_write_fn w8;     /* request header, 8 bits        */
    swd_ud_write_fn w33;    /* write data, 32 bits + parity  */
    swd_ud_read_fn r3;      /* ACK, 3 bits                   */
    swd_ud_read_fn r33;     /* read data, 32 bits + parity   */
};

static volatile int s_active;                  /* engine configured              */
static uint32_t s_actual_hz;                   /* effective SWCLK when active    */
static uint32_t s_half;                        /* current half period (cycles)   */
static const struct swd_ud_ops *s_ops;         /* selected bit engine            */
#if !SWD_UD_OEN_STAY_ON
static int s_oen_on = 1;                       /* SWDIO pad output enable state  */
#endif

/** Write the whole CORE1 dedicated-GPIO output register (one instruction). */
static inline __attribute__((always_inline)) void swd_ud_out(uint32_t value)
{
    asm volatile("wur.gpio_out %0" : : "r"(value) : "memory");
}

/** Read the CORE1 dedicated-GPIO input register (one instruction). */
static inline __attribute__((always_inline)) uint32_t swd_ud_in(void)
{
    uint32_t value;
    asm volatile("ee.get_gpio_in %0" : "=r"(value) : : "memory");
    return value;
}

/** Current CPU cycle counter (rsr.ccount); wraps every ~26 s at 160 MHz. */
static inline __attribute__((always_inline)) uint32_t swd_ud_now(void)
{
    return esp_cpu_get_cycle_count();
}

/** Busy-wait until the cycle counter reaches @p target (signed diff = wrap safe). */
static inline __attribute__((always_inline)) void swd_ud_wait_until(uint32_t target)
{
    while ((int32_t)(swd_ud_now() - target) < 0) {
    }
}

/** Enable/release the SWDIO pad output (see SWD_UD_OEN_STAY_ON). */
static inline __attribute__((always_inline)) void swd_ud_oen(int on)
{
#if SWD_UD_OEN_STAY_ON
    (void)on;
#else
    if (s_oen_on == on) {
        return;
    }
    if (on) {
        GPIO.enable_w1ts = (1u << PIN_SWDIO_MOSI);
    } else {
        GPIO.enable_w1tc = (1u << PIN_SWDIO_MOSI);
    }
    /* Reading the same peripheral waits for the write buffer, so the pad
     * direction is applied when this read returns. */
    (void)GPIO.enable;
    s_oen_on = on;
#endif
}

/** Small settle margin after driving SWDIO again (SWD_UD_OEN_SETTLE_CYCLES). */
static inline __attribute__((always_inline)) void swd_ud_oen_settle(void)
{
#if SWD_UD_OEN_STAY_ON
    /* Nothing to settle: the pad output was never released. */
#else
    asm volatile(
        "nop; nop; nop; nop; nop; nop; nop; nop;"
        "nop; nop; nop; nop; nop; nop; nop; nop;" : : : "memory");
#endif
}

/* ======================================================================== */
/* Paced bit engine (cycle-counter scheduled edges)                        */
/* ======================================================================== */

/**
 * One SWCLK cycle on the paced path.
 * The next edge is scheduled relative to the actual current edge, so a late
 * edge (interrupt) stretches this half period instead of shrinking the next
 * one - the engine is always slower than requested, never faster.
 */
static IRAM_ATTR void swd_ud_paced_clock(uint32_t half, uint32_t lo, uint32_t hi)
{
    uint32_t t;

    swd_ud_out(lo);                     /* falling edge */
    t = swd_ud_now();
    swd_ud_wait_until(t + half);
    swd_ud_out(hi);                     /* rising edge */
    t = swd_ud_now();
    swd_ud_wait_until(t + half);        /* high half */
}

/** Clock `count` SWCLK cycles with SWDIO constant lo/hi (turnaround, idle,
 *  WAIT/FAULT dummy phases, error back-off).  Uses the configured half period
 *  so every protocol phase stays at (or slower than) the requested clock. */
static IRAM_ATTR void swd_ud_clocks(uint32_t count, uint32_t lo, uint32_t hi)
{
    const uint32_t half = s_half;

    while (count != 0u) {
        swd_ud_paced_clock(half, lo, hi);
        count--;
    }
}

/** Write `count` bits of `value` (LSB first) + one parity bit when enabled. */
static IRAM_ATTR void swd_ud_paced_write_bits(uint32_t half, uint32_t value,
                                              uint32_t count, uint32_t parity_en,
                                              uint32_t parity_bit)
{
    const uint32_t total = count + parity_en;
    uint32_t i;

    for (i = 0; i < total; i++) {
        uint32_t bit = (i < count) ? ((value >> i) & 1u) : parity_bit;
        uint32_t t;

        swd_ud_out(bit);                        /* falling edge, SWDIO = bit */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);
        swd_ud_out(bit | SWD_UD_SWCLK_OUT);     /* rising edge, target samples */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);            /* high half */
    }
}

/** Read `count` bits; bits 0..31 accumulate into the return value, a 33rd bit
 *  is stored as 0/1 into raw[0] (data-phase parity). */
static IRAM_ATTR uint32_t swd_ud_paced_read_bits(uint32_t half, uint32_t count,
                                                 uint32_t lo, uint32_t hi,
                                                 uint32_t *raw)
{
    uint32_t acc = 0u;
    uint32_t i;

    for (i = 0; i < count; i++) {
        uint32_t t;
        uint32_t bit;

        swd_ud_out(lo);                         /* falling edge (SWDIO released) */
        t = swd_ud_now();
        swd_ud_wait_until(t + half - SWD_UD_PACED_GUARD);
        bit = swd_ud_in() & SWD_UD_SWDIO_IN;    /* sample just before rising edge */
        if (i < 32u) {
            acc |= bit << i;
        } else if (raw != 0) {
            raw[0] = bit;
        }
        swd_ud_wait_until(t + half);
        swd_ud_out(hi);                         /* rising edge */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);            /* high half */
    }
    return acc;
}

/* Paced wrappers with the same signatures as the unrolled level functions. */
static IRAM_ATTR void swd_ud_paced_w8(uint32_t value, uint32_t parity_unused)
{
    (void)parity_unused;
    swd_ud_paced_write_bits(s_half, value, 8u, 0u, 0u);
}

static IRAM_ATTR void swd_ud_paced_w33(uint32_t value, uint32_t parity_bit)
{
    swd_ud_paced_write_bits(s_half, value, 32u, 1u, parity_bit);
}

static IRAM_ATTR uint32_t swd_ud_paced_r3(uint32_t lo, uint32_t hi, uint32_t *raw)
{
    (void)raw; /* the ACK phase has no parity bit */
    return swd_ud_paced_read_bits(s_half, 3u, lo, hi, 0);
}

static IRAM_ATTR uint32_t swd_ud_paced_r33(uint32_t lo, uint32_t hi, uint32_t *raw)
{
    return swd_ud_paced_read_bits(s_half, 33u, lo, hi, raw);
}

static const struct swd_ud_ops s_paced_ops = {
    swd_ud_paced_w8, swd_ud_paced_w33, swd_ud_paced_r3, swd_ud_paced_r33,
};

/* ======================================================================== */
/* Unrolled bit engine (fast path)                                          */
/*                                                                          */
/* Every half period is exactly HW instructions.  All instructions used in    */
/* the loops are single-cycle on the Xtensa LX7 core: `wur.gpio_out`,        */
/* `ee.get_gpio_in`, `extui`, `slli`, `or`, `addi`, `mov`, `s32i` and nop.   */
/* ======================================================================== */

/*
 * Write BITS data bits of `value` (LSB first) + one parity bit when PAR=1.
 *
 * Per bit (two half periods):
 *   low  half: wur.gpio_out a          (falling edge + data bit)
 *              addi h, a, SWCLK        (high-level value: a|0x2, a is 0/1)
 *              (HW-2) nops
 *   high half: wur.gpio_out h          (rising edge, target samples here)
 *              extui a, value, i+1, 1  (next data bit; parity/end variant)
 *              (HW-2) nops
 *
 * `a` holds the SWDIO level of the current bit (0/1 == dedicated bit 0),
 * `h` the same bit plus the SWCLK bit, so no masks/shifts are needed.
 */
#define SWD_UD_DEF_WRITE(HW, BITS, PAR, FN, ATTR)                                 \
    static ATTR void FN(uint32_t value, uint32_t parity_bit)                      \
    {                                                                             \
        uint32_t a;                                                               \
        uint32_t h;                                                               \
        asm volatile(                                                             \
            "extui %[a], %[v], 0, 1\n"                                            \
            ".set swd_ud_i, 0\n"                                                  \
            ".rept %c[n]\n"                                                       \
            "wur.gpio_out %[a]\n"                                                 \
            "addi %[h], %[a], 2\n"                                                \
            ".rept %c[hw] - 2\n nop\n .endr\n"                                    \
            "wur.gpio_out %[h]\n"                                                 \
            ".if swd_ud_i + 1 < %c[bits]\n"                                       \
            "extui %[a], %[v], swd_ud_i + 1, 1\n"                                 \
            ".rept %c[hw] - 2\n nop\n .endr\n"                                    \
            ".elseif %c[par]\n"                                                   \
            "mov %[a], %[pbit]\n"                                                 \
            ".rept %c[hw] - 2\n nop\n .endr\n"                                    \
            ".else\n"                                                             \
            ".rept %c[hw] - 1\n nop\n .endr\n"                                    \
            ".endif\n"                                                            \
            ".set swd_ud_i, swd_ud_i + 1\n"                                       \
            ".endr\n"                                                             \
            : [a] "=&r"(a), [h] "=&r"(h)                                          \
            : [v] "r"(value), [pbit] "r"(parity_bit),                             \
              [n] "i"(BITS + PAR), [bits] "i"(BITS), [par] "i"(PAR),              \
              [hw] "i"(HW)                                                        \
            : "memory");                                                          \
    }

/*
 * Read COUNT bits.  Bits 0..31 are accumulated into the returned word; bits
 * >= 32 are stored as the raw input-register value into raw[bit-32] (used for
 * the data-phase parity bit).  Each clock:
 *   low  half: wur.gpio_out lo (falling edge, SWDIO released)
 *              (HW-2) nops
 *              ee.get_gpio_in x        (sample, last instruction before rising)
 *   high half: wur.gpio_out hi (rising edge)
 *              extui/slli/or accumulate (or s32i raw for bit >= 32)
 *              padding to HW instructions
 */
#define SWD_UD_DEF_READ(HW, COUNT, FN, ATTR)                                      \
    static ATTR uint32_t FN(uint32_t lo, uint32_t hi, uint32_t *raw)              \
    {                                                                             \
        uint32_t acc;                                                             \
        uint32_t x;                                                               \
        uint32_t t;                                                               \
        asm volatile(                                                             \
            "movi %[acc], 0\n"                                                    \
            ".set swd_ud_i, 0\n"                                                  \
            ".rept %c[n]\n"                                                       \
            "wur.gpio_out %[lo]\n"                                                \
            ".rept %c[hw] - 2\n nop\n .endr\n"                                    \
            "ee.get_gpio_in %[x]\n"                                               \
            "wur.gpio_out %[hi]\n"                                                \
            ".if swd_ud_i < 32\n"                                                 \
            "extui %[t], %[x], 0, 1\n"                                            \
            ".if swd_ud_i > 0\n"                                                  \
            "slli %[t], %[t], swd_ud_i\n"                                         \
            "or %[acc], %[acc], %[t]\n"                                           \
            ".rept %c[hw] - 4\n nop\n .endr\n"                                    \
            ".else\n"                                                             \
            "or %[acc], %[acc], %[t]\n"                                           \
            ".rept %c[hw] - 3\n nop\n .endr\n"                                    \
            ".endif\n"                                                            \
            ".else\n"                                                             \
            "s32i %[x], %[p], 4 * (swd_ud_i - 32)\n"                              \
            ".rept %c[hw] - 2\n nop\n .endr\n"                                    \
            ".endif\n"                                                            \
            ".set swd_ud_i, swd_ud_i + 1\n"                                       \
            ".endr\n"                                                             \
            : [acc] "=&r"(acc), [x] "=&r"(x), [t] "=&r"(t)                        \
            : [lo] "r"(lo), [hi] "r"(hi), [p] "r"(raw), [n] "i"(COUNT),           \
              [hw] "i"(HW)                                                        \
            : "memory");                                                          \
        return acc;                                                               \
    }

/* Only the fastest level lives in IRAM: no instruction-cache miss can stretch
 * its half period.  The slower levels stay in flash - an i-cache miss only
 * stretches one half period and SWD is fully synchronous, which the upstream
 * XRDAP S3 engine relies on as well (and the M4/H723 hardware window showed the
 * 12/16 MHz levels, then flash-resident, were stable). */
#define SWD_UD_LEVEL_FUNCS(IDX, HW, ATTR)                                         \
    SWD_UD_DEF_WRITE(HW, 8, 0, swd_ud_w8_##IDX, ATTR)                             \
    SWD_UD_DEF_WRITE(HW, 32, 1, swd_ud_w33_##IDX, ATTR)                           \
    SWD_UD_DEF_READ(HW, 3, swd_ud_r3_##IDX, ATTR)                                 \
    SWD_UD_DEF_READ(HW, 33, swd_ud_r33_##IDX, ATTR)

SWD_UD_DEFINE_ALL_LEVELS()

#define SWD_UD_OPS_ENTRY(IDX)                                                     \
    { swd_ud_w8_##IDX, swd_ud_w33_##IDX, swd_ud_r3_##IDX, swd_ud_r33_##IDX }

static const struct swd_ud_ops s_fast_ops[] = {
    SWD_UD_ALL_LEVEL_OPS(),
};

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int SWD_UltraFast_Available(void)
{
    /* The dedicated-GPIO output/input registers used here are the CORE1 ones
     * (spi_switch.c maps the pads to CORE1_GPIO_OUT0/1 + CORE1_GPIO_IN0), so
     * the engine must run on core 1 of the dual-core S3 build (main.c pins
     * the DAP task, the TCP/WS-DAP transport, the target-flash worker and the
     * curve sampler there for the same reason). */
    if (esp_cpu_get_core_id() != 1) {
        return 0;
    }
    /* DAP_SPI_Deinit() must have routed the pads to the dedicated-GPIO
     * channels; otherwise writing the CPU GPIO registers would toggle
     * nothing (the A-fork "JUNK ACK after a core migration" failure). */
    if (GPIO.func_out_sel_cfg[PIN_SWCLK].func_sel != CORE1_GPIO_OUT1_IDX) {
        return 0;
    }
    if (GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].func_sel != CORE1_GPIO_OUT0_IDX) {
        return 0;
    }
    /* SWDIO pad OEN must come from the GPIO peripheral enable register so the
     * turnaround control in swd_ud_oen() actually releases the pad. */
    if (GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel != 1u) {
        return 0;
    }
    if (GPIO.func_in_sel_cfg[CORE1_GPIO_IN0_IDX].sig_in_sel != 1u) {
        return 0;
    }
    if (GPIO.func_in_sel_cfg[CORE1_GPIO_IN0_IDX].func_sel != PIN_SWDIO_MOSI) {
        return 0;
    }
    return 1;
}

uint32_t SWD_UltraFast_Enter(uint32_t requested_hz)
{
    swd_ultra_plan_t plan;

    if (SWD_UltraFast_Available() == 0) {
        return 0u;
    }
    if (requested_hz == 0u) {
        requested_hz = SWD_UD_DEFAULT_CLOCK_HZ;
    }
    if (SWD_Ultra_Plan(SWD_UD_CPU_HZ, requested_hz, s_fast_half,
                       (int)(sizeof(s_fast_half) / sizeof(s_fast_half[0])),
                       &plan) != 0) {
        return 0u;
    }
    if (plan.half_cycles == 0u) {
        return 0u;
    }
    if (plan.fast_level >= 0) {
        s_ops = &s_fast_ops[plan.fast_level];
    } else {
        s_ops = &s_paced_ops;
    }
    s_half = plan.half_cycles;
    s_actual_hz = SWD_Ultra_LevelHz(SWD_UD_CPU_HZ, s_half);

    /* Idle line state, same as the legacy backend left it after a transfer:
     * SWCLK high, SWDIO high, host driving SWDIO. */
#if !SWD_UD_OEN_STAY_ON
    s_oen_on = 0; /* force the enable below to be applied */
#endif
    swd_ud_oen(1);
    swd_ud_out(SWD_UD_IDLE);
    s_active = 1;

    return s_actual_hz;
}

int SWD_UltraFast_EnsureConfigured(void)
{
    if (s_active != 0) {
        return 1;
    }
    return (SWD_UltraFast_Enter(SWD_UD_DEFAULT_CLOCK_HZ) != 0u) ? 1 : 0;
}

void SWD_UltraFast_Exit(void)
{
    if (s_active == 0) {
        return;
    }
    /* Leave a safe idle line for whatever legacy backend runs next (JTAG). */
    swd_ud_oen(1);
    swd_ud_out(SWD_UD_IDLE);
    s_active = 0;
    s_actual_hz = 0u;
    s_ops = 0;
}

int SWD_UltraFast_Active(void)
{
    return s_active;
}

uint32_t SWD_UltraFast_ActualHz(void)
{
    return (s_active != 0) ? s_actual_hz : 0u;
}

/* ======================================================================== */
/* SWD transfer                                                             */
/* ======================================================================== */

uint8_t IRAM_ATTR SWD_Transfer_Ultra(uint32_t request, uint32_t *data)
{
    const struct swd_ud_ops *ops = s_ops;
    uint32_t turnaround;
    uint32_t idle_cycles;
    uint32_t ack;
    uint32_t value;
    uint32_t parity;
    uint32_t raw = 0u;

    if ((s_active == 0) || (ops == 0)) {
        return DAP_TRANSFER_ERROR;      /* no legacy fallback on ESP32-S3 */
    }
    if (((request & DAP_TRANSFER_RnW) == 0u) && (data == NULL)) {
        return DAP_TRANSFER_ERROR;
    }
    turnaround = DAP_Data.swd_conf.turnaround;
    idle_cycles = DAP_Data.transfer.idle_cycles;

    /* Packet request: 8 bits, host driving SWDIO. */
    swd_ud_oen(1);
    ops->w8(SWD_Ultra_RequestByte(request), 0u);

    /* Turnaround: host releases SWDIO, target may start driving. */
    swd_ud_oen(0);
    swd_ud_clocks(turnaround, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);

    /* Acknowledge response: 3 bits sampled just before the rising edges. */
    ack = ops->r3(SWD_UD_SWDIO_OUT, SWD_UD_IDLE, &raw);

    if (ack == DAP_TRANSFER_OK) {
        if ((request & DAP_TRANSFER_RnW) != 0u) {
            /* Read data: 32 bits + parity sampled by the host, then one
             * turnaround before the host drives again. */
            value = ops->r33(SWD_UD_SWDIO_OUT, SWD_UD_IDLE, &raw);
            swd_ud_clocks(turnaround, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);
            swd_ud_oen(1);
            parity = raw & 1u;
            if (((parity ^ SWD_Ultra_Parity32(value)) & 1u) != 0u) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data != NULL) {
                *data = value;
            }
            if ((request & DAP_TRANSFER_TIMESTAMP) != 0u) {
                DAP_Data.timestamp = TIMESTAMP_GET();
            }
        } else {
            /* Write data: turnaround, host drives 32 bits + parity. */
            swd_ud_clocks(turnaround, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);
            swd_ud_oen(1);
            swd_ud_oen_settle();
            value = *data;
            ops->w33(value, SWD_Ultra_Parity32(value));
            if ((request & DAP_TRANSFER_TIMESTAMP) != 0u) {
                DAP_Data.timestamp = TIMESTAMP_GET();
            }
        }
        /* Idle cycles after the data phase (SWDIO low), then the idle level. */
        if (idle_cycles != 0u) {
            swd_ud_out(0u);
            swd_ud_clocks(idle_cycles, 0u, SWD_UD_SWCLK_OUT);
        }
        swd_ud_out(SWD_UD_IDLE);
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        /* WAIT/FAULT: no data phase unless data_phase is configured; keep the
         * protocol timing of the legacy GPIO backend (dummy data,
         * turnaround, idle). */
        if ((DAP_Data.swd_conf.data_phase != 0u) && ((request & DAP_TRANSFER_RnW) != 0u)) {
            swd_ud_clocks(33u, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);
        }
        swd_ud_clocks(turnaround, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);
        swd_ud_oen(1);
        if ((DAP_Data.swd_conf.data_phase != 0u) && ((request & DAP_TRANSFER_RnW) == 0u)) {
            swd_ud_out(0u);
            swd_ud_clocks(33u, 0u, SWD_UD_SWCLK_OUT);
        }
        swd_ud_out(SWD_UD_IDLE);
        return (uint8_t)ack;
    }

    /* Protocol error: the target may still be in the data phase, so back off
     * the data phase with SWDIO released, then idle. */
    swd_ud_clocks(turnaround + 33u, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);
    swd_ud_oen(1);
    swd_ud_out(SWD_UD_IDLE);
    return (uint8_t)ack;
}

/* ======================================================================== */
/* SWJ_Sequence / SWD_Sequence                                              */
/* ======================================================================== */

int SWD_UltraFast_SequenceWrite(uint32_t count_bits, const uint8_t *data)
{
    uint32_t half;
    uint32_t i;

    if ((data == NULL) || (count_bits == 0u) || (count_bits > 256u)) {
        return 0;
    }
    if (SWD_UltraFast_EnsureConfigured() == 0) {
        return 0;                       /* no legacy fallback on ESP32-S3 */
    }
    half = SWD_Ultra_SequenceHalf(s_half);
    swd_ud_oen(1);
    for (i = 0; i < count_bits; i++) {
        uint32_t bit = (data[i >> 3] >> (i & 7u)) & 1u;
        uint32_t t;

        swd_ud_out(bit);                        /* falling edge, SWDIO = bit */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);
        swd_ud_out(bit | SWD_UD_SWCLK_OUT);     /* rising edge */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);            /* high half */
    }
    return 1;
}

int SWD_UltraFast_SequenceRead(uint32_t count_bits, uint8_t *data)
{
    uint32_t half;
    uint32_t bytes;
    uint32_t i;

    if ((data == NULL) || (count_bits == 0u) || (count_bits > 64u)) {
        return 0;
    }
    if (SWD_UltraFast_EnsureConfigured() == 0) {
        return 0;                       /* no legacy fallback on ESP32-S3 */
    }
    bytes = (count_bits + 7u) / 8u;
    for (i = 0; i < bytes; i++) {
        data[i] = 0u;
    }
    half = SWD_Ultra_SequenceHalf(s_half);
    swd_ud_oen(0);                       /* host releases SWDIO for the capture */
    for (i = 0; i < count_bits; i++) {
        uint32_t t;
        uint32_t bit;

        swd_ud_out(SWD_UD_SWDIO_OUT);           /* falling edge, SWDIO released */
        t = swd_ud_now();
        swd_ud_wait_until(t + half - SWD_UD_PACED_GUARD);
        bit = swd_ud_in() & SWD_UD_SWDIO_IN;    /* sample before the rising edge */
        data[i >> 3] |= (uint8_t)(bit << (i & 7u));
        swd_ud_wait_until(t + half);
        swd_ud_out(SWD_UD_IDLE);                /* rising edge */
        t = swd_ud_now();
        swd_ud_wait_until(t + half);            /* high half */
    }
    swd_ud_oen(1);
    swd_ud_oen_settle();
    swd_ud_out(SWD_UD_IDLE);
    return 1;
}

#endif /* SWD_ULTRA_FAST_ENGINE_ENABLED */
