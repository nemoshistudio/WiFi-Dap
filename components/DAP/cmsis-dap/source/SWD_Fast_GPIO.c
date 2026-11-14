/**
 * @file SWD_Fast_GPIO.c
 * @brief ESP32-S3 ultra-fast SWD bit engine (Xtensa dedicated GPIO, opt-in).
 *
 * M4 (user request): a dedicated-GPIO SWD engine in the same class as
 * XRDAP-ESP's 23.7 MHz `swd_fast.cpp`, wired into the CMSIS-DAP core as a
 * new transfer backend selected by DAP_SWJ_Clock (see docs/swd-23mhz-engine.md
 * for the exact integration patch, timing budget and HIL checklist).
 *
 * Why this is fast (and why it is deterministic)
 * ----------------------------------------------
 *  - DAP_SPI_Deinit() (spi_switch.c) routes the SWD pads to the S3 per-core
 *    dedicated GPIO block: SWCLK -> CORE1_GPIO_OUT1 (bit 1), SWDIO ->
 *    CORE1_GPIO_OUT0 (bit 0), SWDIO input -> CORE1_GPIO_IN0 (bit 0).  The
 *    Xtensa instructions `wur.gpio_out` (write all dedicated outputs) and
 *    `ee.get_gpio_in` (read all dedicated inputs) are single CPU
 *    instructions that bypass the GPIO matrix.
 *  - One `wur.gpio_out` writes SWCLK *and* SWDIO together, so every edge is
 *    one instruction with no host/target skew and a 50 % duty cycle.
 *  - All bit loops are unrolled at assembly time (`asm volatile` + `.rept`);
 *    each SWCLK half period is exactly `half` instructions, so the bit clock
 *    is CPU_CLOCK / (2 * half) independent of compiler scheduling.  The
 *    half-period instruction budget is fixed the same way XRDAP does it.
 *  - The fastest level is chosen so that one half period is always
 *    >= 20 ns (SWD_ULTRA_MIN_HALF_NS).  On the project's 160 MHz CPU build
 *    that is half = 4 cycles -> 20 MHz; on a 240 MHz build half = 5 cycles
 *    -> 24 MHz (XRDAP measured 23.7 MHz for the same 5-cycle half period).
 *    Levels: 160 MHz {20, 16, 13.33, 10} MHz, 240 MHz {24, 20, 15, 12} MHz.
 *  - The engine never exceeds the host's adapter-speed request (unlike the
 *    legacy SPI backend, which always runs at 40 MHz).
 *
 * Protocol coverage (same semantics as SWD_Transfer_GPIO in SW_DP.c)
 * ------------------------------------------------------------------
 *  - 8-bit request header (start/APnDP/RnW/A2/A3/parity/stop/park, LSB first);
 *  - DAP_Data.swd_conf.turnaround turnaround clocks (host releases SWDIO);
 *  - 3 ACK bits sampled just before the rising edge;
 *  - read: 32 data bits + parity sampled, parity checked, data returned even
 *    on a parity error (ACK becomes DAP_TRANSFER_ERROR), then turnaround;
 *  - write: turnaround, 32 data bits + parity driven by the host;
 *  - DAP_TRANSFER_TIMESTAMP captured at the same point as the GPIO backend;
 *  - DAP_Data.transfer.idle_cycles clocked low after a completed transfer;
 *  - WAIT/FAULT: optional dummy data phase (data_phase), turnaround and the
 *    idle level; no timestamp, same ACK returned;
 *  - protocol error (ACK not OK/WAIT/FAULT): turnaround + 33 back-off clocks
 *    with SWDIO released, then the idle level.
 *
 * Fail-safe / fallback
 * --------------------
 *  - Compiled only for ESP32-S3 with BIRDIEBOT_SWD_ULTRA_FAST enabled; every
 *    other target (ESP32/C3/C5) and a disabled Kconfig build get the small
 *    stub implementation at the bottom of this file, so SW_DP.c can always
 *    reference the API.
 *  - SWD_UltraFast_Available() validates the dedicated-GPIO pin routing in
 *    the GPIO matrix registers and the calling core; DAP_SWJ_Clock must call
 *    it before selecting the backend, otherwise the existing GPIO fast path
 *    is kept (Lead wiring in docs/swd-23mhz-engine.md).
 *
 * Hardware status: **not yet validated on the real board**.  The code follows
 * XRDAP-ESP's hardware-proven approach (same SoC, same dedicated-GPIO
 * mechanism, same per-half instruction counts) and the repo's existing
 * dedicated-GPIO mapping, but the actual waveform/clock/stability on this
 * board must be measured - see the HIL checklist in docs/swd-23mhz-engine.md.
 * The engine is opt-in per clock request; the project default SWD clock
 * remains 2 MHz, and setting BIRDIEBOT_SWD_ULTRA_FAST=n restores the old
 * behaviour exactly.
 */

#include "sdkconfig.h"

#include <stdint.h>

/* The engine is only built for the ESP32-S3 and only makes sense at >=150 MHz
 * CPU (the half-period safety floor needs at least 4 cycles).  Everything
 * else - other targets, Kconfig off, slow CPU - uses the stubs below. */
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_BIRDIEBOT_SWD_ULTRA_FAST) && \
    (CONFIG_BIRDIEBOT_SWD_ULTRA_FAST) && (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ >= 150)
#define SWD_UD_ENABLED 1
#else
#define SWD_UD_ENABLED 0
#endif

#if SWD_UD_ENABLED
/* DAP_config.h must come first: DAP.h keys its SWD members and prototypes off
 * DAP_SWD (defined in DAP_config.h), exactly like SW_DP.c does. */
#include "DAP_config.h"
#endif

#include "cmsis-dap/include/DAP.h"
#include "SWD_Fast_GPIO.h"

/* ======================================================================== */
/* Stub implementation (all other targets / Kconfig disabled)               */
/* ======================================================================== */
#if !SWD_UD_ENABLED

int SWD_UltraFast_Available(void)
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
    /* Never selected by SWD_Transfer(): DAP_SWJ_Clock only picks the ultra
     * backend after SWD_UltraFast_Available() said yes.  Keep the value
     * harmless if a future caller gets it wrong. */
    return DAP_TRANSFER_ERROR;
}

#else /* SWD_UD_ENABLED */

/* ======================================================================== */
/* ESP32-S3 dedicated-GPIO ultra-fast engine                                */
/* ======================================================================== */

#include "cmsis-dap/include/gpio_common.h"  /* soc/..., hal/gpio_ll.h               */
#include "esp_attr.h"                       /* IRAM_ATTR                            */
#include "esp_cpu.h"                        /* esp_cpu_get_core_id()                */

/* Dedicated-GPIO channel bits, fixed by DAP_SPI_Deinit() (spi_switch.c):
 *   bit0 = SWDIO (CORE1_GPIO_OUT0), bit1 = SWCLK (CORE1_GPIO_OUT1)
 *   input bit0 = SWDIO (CORE1_GPIO_IN0)
 * The engine writes the whole 8-channel output register, so it must stay the
 * only user of the CORE1 dedicated GPIO outputs (nothing else in this firmware
 * uses them). */
#define SWD_UD_SWDIO_OUT 0x1u
#define SWD_UD_SWCLK_OUT 0x2u
#define SWD_UD_IDLE      (SWD_UD_SWDIO_OUT | SWD_UD_SWCLK_OUT)

/* Number of CPU cycles to wait after re-enabling the SWDIO pad output before
 * driving the first data bit.  The GPIO_ENABLE register is on the APB bus and
 * the write takes a dozen or so CPU cycles; the read-back in swd_ud_oen()
 * serialises the write, this is the remaining pad/driver settle margin.  It
 * also works around the A-fork regression where per-transaction OEN toggling
 * without a settle broke the *second* SWD transaction (JUNK ACK). */
#define SWD_UD_OEN_SETTLE_CYCLES 16u

/* SWDIO output-enable strategy.
 *
 * 0 (default, protocol-correct): release SWDIO during ACK/read-data/read
 *   turnaround and drive it for the header/write-data phase.  This is what
 *   XRDAP-ESP's hardware-validated engine does.
 * 1 (A 2025 fork workaround): never touch the pad output enable; SWDIO stays
 *   driven all the time.  That is what the current GPIO fast path does on
 *   this board (DAP_config.h PIN_SWDIO_OUT_ENABLE/DISABLE are no-ops for S3)
 *   and it is known to pass OpenOCD bring-up, but it violates the SWD
 *   turnaround and is not what we want at 20 MHz.  Kept as a Kconfig escape
 *   hatch for the HIL A/B run.
 */
#if defined(CONFIG_BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON) && (CONFIG_BIRDIEBOT_SWD_ULTRA_OEN_STAY_ON)
#define SWD_UD_OEN_STAY_ON 1
#else
#define SWD_UD_OEN_STAY_ON 0
#endif

#if CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ >= 230
/* 240 MHz CPU: half periods 5/6/8/10 cycles -> 24 / 20 / 15 / 12 MHz. */
#define SWD_UD_CPU_HZ 240000000u
#define SWD_UD_HALF_0 5
#define SWD_UD_HALF_1 6
#define SWD_UD_HALF_2 8
#define SWD_UD_HALF_3 10
#else
/* 160 MHz CPU (project default): 4/5/6/8 cycles -> 20 / 16 / 13.3 / 10 MHz. */
#define SWD_UD_CPU_HZ 160000000u
#define SWD_UD_HALF_0 4
#define SWD_UD_HALF_1 5
#define SWD_UD_HALF_2 6
#define SWD_UD_HALF_3 8
#endif

static const uint16_t s_level_half[SWD_ULTRA_LEVEL_COUNT] = {
    SWD_UD_HALF_0, SWD_UD_HALF_1, SWD_UD_HALF_2, SWD_UD_HALF_3,
};

/* ======================================================================== */
/* Unrolled bit engines                                                     */
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

/* Level 0 is the fastest level, i.e. the one used for a >= 20 MHz host
 * request, so it lives in IRAM: no instruction-cache miss can stretch a half
 * period of the hot path.  The slower levels stay in flash - an i-cache miss
 * only stretches one half period and SWD is fully synchronous, which the
 * upstream XRDAP S3 engine relies on as well. */
#define SWD_UD_LEVEL_FUNCS(IDX, HW, ATTR)                                         \
    SWD_UD_DEF_WRITE(HW, 8, 0, swd_ud_w8_##IDX, ATTR)                             \
    SWD_UD_DEF_WRITE(HW, 32, 1, swd_ud_w33_##IDX, ATTR)                           \
    SWD_UD_DEF_READ(HW, 3, swd_ud_r3_##IDX, ATTR)                                 \
    SWD_UD_DEF_READ(HW, 33, swd_ud_r33_##IDX, ATTR)

SWD_UD_LEVEL_FUNCS(0, SWD_UD_HALF_0, IRAM_ATTR)
SWD_UD_LEVEL_FUNCS(1, SWD_UD_HALF_1, )
SWD_UD_LEVEL_FUNCS(2, SWD_UD_HALF_2, )
SWD_UD_LEVEL_FUNCS(3, SWD_UD_HALF_3, )

typedef void (*swd_ud_write_fn)(uint32_t value, uint32_t parity_bit);
typedef uint32_t (*swd_ud_read_fn)(uint32_t lo, uint32_t hi, uint32_t *raw);

struct swd_ud_level {
    uint16_t half;          /* CPU cycles per SWCLK half period */
    uint16_t reserved;
    swd_ud_write_fn w8;     /* request header, 8 bits            */
    swd_ud_write_fn w33;    /* write data, 32 bits + parity      */
    swd_ud_read_fn r3;      /* ACK, 3 bits                       */
    swd_ud_read_fn r33;     /* read data, 32 bits + parity       */
};

static const struct swd_ud_level s_levels[SWD_ULTRA_LEVEL_COUNT] = {
    { SWD_UD_HALF_0, 0, swd_ud_w8_0, swd_ud_w33_0, swd_ud_r3_0, swd_ud_r33_0 },
    { SWD_UD_HALF_1, 0, swd_ud_w8_1, swd_ud_w33_1, swd_ud_r3_1, swd_ud_r33_1 },
    { SWD_UD_HALF_2, 0, swd_ud_w8_2, swd_ud_w33_2, swd_ud_r3_2, swd_ud_r33_2 },
    { SWD_UD_HALF_3, 0, swd_ud_w8_3, swd_ud_w33_3, swd_ud_r3_3, swd_ud_r33_3 },
};

/* ======================================================================== */
/* Engine state and helpers                                                 */
/* ======================================================================== */

static volatile int s_active;                       /* engine configured              */
static uint32_t s_actual_hz;                        /* effective SWCLK when active    */
static const struct swd_ud_level *s_cur;            /* selected level                 */
#if !SWD_UD_OEN_STAY_ON
static int s_oen_on = 1;                            /* SWDIO pad output enable state  */
#endif

/** Write the whole CORE1 dedicated-GPIO output register (one instruction). */
static inline __attribute__((always_inline)) void swd_ud_out(uint32_t value)
{
    asm volatile("wur.gpio_out %0" : : "r"(value) : "memory");
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

/** Small settle margin after driving SWDIO again (see SWD_UD_OEN_SETTLE_CYCLES). */
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

/** Clock `count` SWCLK cycles with SWDIO constant `lo`/`hi` (idle, turnaround,
 *  WAIT/FAULT dummy phases, protocol-error back-off).  These phases have no
 *  setup/hold requirement, so a handful of cycles per half is fine and keeps
 *  the unrolled bit engines small. */
static IRAM_ATTR void swd_ud_clocks(uint32_t count, uint32_t lo, uint32_t hi)
{
    while (count != 0u) {
        swd_ud_out(lo);
        asm volatile("nop; nop; nop; nop;" : : : "memory");
        swd_ud_out(hi);
        asm volatile("nop; nop; nop; nop;" : : : "memory");
        count--;
    }
}

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

int SWD_UltraFast_Available(void)
{
    /* The dedicated-GPIO output/input registers used here are the CORE1 ones
     * (spi_switch.c maps the pads to CORE1_GPIO_OUT0/1 + CORE1_GPIO_IN0), so
     * the engine must run on core 1 of the dual-core S3 build (main.c pins
     * the DAP task and the curve sampler there for the same reason). */
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
    int level;

    if (SWD_UltraFast_Available() == 0) {
        return 0u;
    }
    level = SWD_Ultra_PickLevel(s_level_half, SWD_ULTRA_LEVEL_COUNT, SWD_UD_CPU_HZ,
                                requested_hz);
    if (level < 0) {
        return 0u;
    }
    s_cur = &s_levels[level];
    s_actual_hz = SWD_Ultra_LevelHz(SWD_UD_CPU_HZ, s_cur->half);

    /* Idle line state, same as the GPIO backend leaves it after a transfer:
     * SWCLK high, SWDIO high, host driving SWDIO. */
#if !SWD_UD_OEN_STAY_ON
    s_oen_on = 0; /* force the enable below to be applied */
#endif
    swd_ud_oen(1);
    swd_ud_out(SWD_UD_IDLE);
    s_active = 1;

    return s_actual_hz;
}

void SWD_UltraFast_Exit(void)
{
    if (s_active == 0) {
        return;
    }
    /* Leave a safe idle line for whatever backend runs next. */
    swd_ud_oen(1);
    swd_ud_out(SWD_UD_IDLE);
    s_active = 0;
    s_actual_hz = 0u;
    s_cur = NULL;
}

int SWD_UltraFast_Active(void)
{
    return s_active;
}

uint32_t SWD_UltraFast_ActualHz(void)
{
    return (s_active != 0) ? s_actual_hz : 0u;
}

uint8_t IRAM_ATTR SWD_Transfer_Ultra(uint32_t request, uint32_t *data)
{
    const struct swd_ud_level *lvl = s_cur;
    uint32_t turnaround;
    uint32_t idle_cycles;
    uint32_t ack;
    uint32_t value;
    uint32_t parity;
    uint32_t raw = 0u;

    if ((s_active == 0) || (lvl == NULL)) {
        return DAP_TRANSFER_ERROR;
    }
    if (((request & DAP_TRANSFER_RnW) == 0u) && (data == NULL)) {
        return DAP_TRANSFER_ERROR;
    }
    turnaround = DAP_Data.swd_conf.turnaround;
    idle_cycles = DAP_Data.transfer.idle_cycles;

    /* Packet request: 8 bits, host driving SWDIO. */
    swd_ud_oen(1);
    lvl->w8(SWD_Ultra_RequestByte(request), 0u);

    /* Turnaround: host releases SWDIO, target may start driving.
     * The first clock also gives the pad output enable time to drop. */
    swd_ud_oen(0);
    swd_ud_clocks(turnaround, SWD_UD_SWDIO_OUT, SWD_UD_IDLE);

    /* Acknowledge response: 3 bits sampled just before the rising edges. */
    ack = lvl->r3(SWD_UD_SWDIO_OUT, SWD_UD_IDLE, &raw);

    if (ack == DAP_TRANSFER_OK) {
        if ((request & DAP_TRANSFER_RnW) != 0u) {
            /* Read data: 32 bits + parity sampled by the host, then one
             * turnaround before the host drives again. */
            value = lvl->r33(SWD_UD_SWDIO_OUT, SWD_UD_IDLE, &raw);
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
            lvl->w33(value, SWD_Ultra_Parity32(value));
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
         * protocol timing of the GPIO backend (dummy data, turnaround, idle). */
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

#endif /* SWD_UD_ENABLED */
