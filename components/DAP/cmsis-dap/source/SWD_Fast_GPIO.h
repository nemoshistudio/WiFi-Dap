/**
 * @file SWD_Fast_GPIO.h
 * @brief ESP32-S3 dedicated-GPIO SWD engine - public API (M5: all SWD rates).
 *
 * M5 (user request): on the ESP32-S3 **every** SWD operation goes through this
 * engine: `SWD_Transfer` at any adapter speed (about 100 kHz .. 20 MHz on the
 * project's 160 MHz CPU build, up to 24 MHz on a 240 MHz build),
 * `SWJ_Sequence` (line reset / JTAG-to-SWD / dormant wake-up) and
 * `SWD_Sequence` (capture).  The legacy GPIO normal / GPIO fast / 40 MHz SPI
 * SWD backends are **not used on the ESP32-S3 any more**; they remain only for
 * the non-S3 targets (ESP32, C3, C5) and for JTAG on every target.
 *
 * Why: hardware bring-up on the STM32H723 (docs/dap-tcp-backend.md §12) showed
 * the legacy GPIO 2/4 MHz SWD modes failing on that target and the 10 MHz SPI
 * mode only partially working, while this engine was stable at 12/16 MHz and
 * marginal at 20 MHz - so a single, controlled SWD backend is safer than
 * "fast engine for >= 12 MHz, untested legacy path below".
 *
 * Two bit engines, one protocol layer:
 *  - Fast path: fully unrolled Xtensa assembly, one `wur.gpio_out` per SWCLK
 *    edge (SWCLK + SWDIO in one instruction), every half period a fixed count
 *    of single-cycle instructions -> f = CPU_CLOCK / (2 * half_cycles).
 *    Levels are compiled as {4,5,6,7,8,10,12,16,20} half cycles on the
 *    160 MHz build (20/16/13.3/11.4/10/8/6.67/5/4 MHz) and
 *    {5,6,7,8,10,12,16,20} on the 240 MHz build (24/20/17.1/15/12/10/7.5/6 MHz).
 *  - Paced path (all rates below the slowest fast level): the C loops place
 *    every SWCLK edge on an absolute cycle-counter grid
 *    (edge(N+1) = edge(N) + half_cycles), so the frequency stays exact while
 *    interrupts only add jitter.  Used down to 100 kHz and below.
 *  - Level selection never runs faster than the host request
 *    (`actual_hz <= requested_hz`, "slower rather than faster").  The mapping
 *    keeps the error within 0 .. -25 % of the request, which the M5 brief
 *    accepts; above the fastest level the fastest level is used.
 *
 * Fail-safe: `SWD_UltraFast_Available()` validates the CORE1 dedicated-GPIO
 * routing and the calling core.  If the engine cannot run (wrong core, pins not
 * routed, CPU too slow, Kconfig off) the SWD entry points return
 * DAP_TRANSFER_ERROR / DAP_ERROR - there is **no** fallback to the legacy SWD
 * backends on the ESP32-S3 (that is the M5 contract).
 *
 * This header intentionally has no ESP-IDF dependency except the compile-time
 * CONFIG_* macros, so the pure level/plan/request logic is unit tested on the
 * host (components/DAP/test/host/).  See docs/swd-23mhz-engine.md.
 */

#ifndef SWD_FAST_GPIO_H
#define SWD_FAST_GPIO_H

#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Compile-time routing: is this build an ESP32-S3 with the engine enabled?   */
/* ------------------------------------------------------------------------- */
/* CONFIG_IDF_TARGET_ESP32S3 / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ are undefined in
 * host test builds; `#if` treats undefined identifiers as 0, so the macro is
 * 0 there.  The engine needs >= 150 MHz CPU (half periods of >= 4 cycles). */
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_BIRDIEBOT_SWD_ULTRA_FAST) && \
    (CONFIG_BIRDIEBOT_SWD_ULTRA_FAST) && (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ >= 150)
#define SWD_ULTRA_FAST_ENGINE_ENABLED 1
#else
#define SWD_ULTRA_FAST_ENGINE_ENABLED 0
#endif

/** Maximum number of compiled fast timing levels (160 or 240 MHz build). */
#define SWD_ULTRA_FAST_LEVEL_MAX 9

/**
 * Safety floor for one SWCLK half period, in nanoseconds.
 *
 * XRDAP-ESP's hardware-validated 23.7 MHz engine runs with half = 21.1 ns and
 * refuses the next faster level because the target's data is not valid early
 * enough.  We keep a small extra margin: the fastest level is only used when
 * half_cycles * 1e9 / cpu_hz >= 20 ns.
 */
#define SWD_ULTRA_MIN_HALF_NS 20u

/**
 * Slowest half period used for SWJ/SWD sequences (clock cycles).  Sequences
 * (line reset, JTAG-to-SWD switch, dormant wake-up, capture) are not
 * throughput critical, so they run at <= 8 MHz on the 160 MHz build even when
 * a faster adapter speed is configured.  This keeps the line-reset waveform
 * deterministic and avoids re-testing sequence timing in the fast path.
 */
#define SWD_ULTRA_SEQ_MIN_HALF 10u

/** Smallest half period (in CPU cycles) accepted for @p cpu_hz. */
static inline uint32_t SWD_Ultra_MinHalfCycles(uint32_t cpu_hz)
{
    /* ceil(SWD_ULTRA_MIN_HALF_NS * cpu_hz / 1e9), computed in 64 bits:
     * 20 ns * 240 MHz = 4.8 -> 5; 20 ns * 160 MHz = 3.2 -> 4. */
    uint64_t cycles = ((uint64_t)SWD_ULTRA_MIN_HALF_NS * (uint64_t)cpu_hz + 999999999ull) / 1000000000ull;
    return (uint32_t)cycles;
}

/** SWCLK frequency produced by a half period of @p half_cycles at @p cpu_hz. */
static inline uint32_t SWD_Ultra_LevelHz(uint32_t cpu_hz, uint32_t half_cycles)
{
    if (half_cycles == 0u) {
        return 0u;
    }
    return cpu_hz / (2u * half_cycles);
}

/** Half period used by SWJ_Sequence / SWD_Sequence (never faster than 8 MHz @160). */
static inline uint32_t SWD_Ultra_SequenceHalf(uint32_t half_cycles)
{
    return (half_cycles < SWD_ULTRA_SEQ_MIN_HALF) ? SWD_ULTRA_SEQ_MIN_HALF : half_cycles;
}

/** Even parity of a 4-bit value (SWD request header parity). */
static inline uint32_t SWD_Ultra_Parity4(uint32_t value)
{
    value &= 0x0fu;
    return (0x6996u >> value) & 1u; /* 4-bit parity lookup, no libcall */
}

/** Even parity of a 32-bit value (SWD data phase parity, local/no libcall). */
static inline uint32_t SWD_Ultra_Parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 1u;
}

/**
 * Build the 8-bit SWD request header byte from a CMSIS-DAP transfer request
 * (DAP_TRANSFER_APnDP/A2/A3/RnW bits, LSB first on the wire):
 *   bit0 start=1 | b1 APnDP | b2 RnW | b3 A2 | b4 A3 | b5 parity | b6 stop=0 | b7 park=1
 */
static inline uint32_t SWD_Ultra_RequestByte(uint32_t request)
{
    uint32_t nibble = request & 0x0fu;
    return 0x81u | (nibble << 1) | (SWD_Ultra_Parity4(nibble) << 5);
}

/** Convert the 3 sampled ACK bits (bit0 = first ACK bit) to a CMSIS-DAP ACK. */
static inline uint32_t SWD_Ultra_AckFromBits(uint32_t bits)
{
    return bits & 0x07u;
}

/** Result of @ref SWD_Ultra_Plan: which bit engine and half period to use. */
typedef struct {
    int32_t fast_level;     /**< >= 0: unrolled level index; -1: paced loop  */
    uint32_t half_cycles;   /**< half period the engine will run with         */
    uint32_t actual_hz;     /**< cpu_hz / (2 * half_cycles)                   */
} swd_ultra_plan_t;

/**
 * Choose the bit engine and half period for @p requested_hz.
 *
 * Rules (host tested, see components/DAP/test/host/):
 *  - never faster than requested: actual_hz <= requested_hz;
 *  - prefer the fastest unrolled level whose half period is >= the wanted one;
 *  - when no unrolled level is slow enough, use the paced loop with
 *    half = ceil(cpu_hz / (2 * requested_hz)) (frequency exact, jitter only);
 *  - above the fastest level use the fastest level;
 *  - keep the error within 0 .. -25 % for requests between the slowest
 *    supported rate and the fastest level.
 *
 * @param cpu_hz      CPU clock the fast levels are compiled for
 * @param requested_hz host adapter speed request (upper bound, > 0)
 * @param fast_halfs  compiled fast-level half periods in CPU cycles, fastest first
 * @param count       number of entries (1..SWD_ULTRA_FAST_LEVEL_MAX)
 * @param plan        output
 * @return 0 on success, -1 on invalid arguments
 */
static inline int SWD_Ultra_Plan(uint32_t cpu_hz, uint32_t requested_hz,
                                 const uint16_t *fast_halfs, int count,
                                 swd_ultra_plan_t *plan)
{
    uint64_t wanted;
    int i;

    if ((cpu_hz == 0u) || (requested_hz == 0u) || (fast_halfs == 0) ||
        (count <= 0) || (count > SWD_ULTRA_FAST_LEVEL_MAX) || (plan == 0)) {
        return -1;
    }
    /* Wanted half period, rounded UP so the engine is never faster than asked. */
    wanted = ((uint64_t)cpu_hz + (2ull * (uint64_t)requested_hz) - 1ull) /
             (2ull * (uint64_t)requested_hz);
    if (wanted == 0ull) {
        wanted = 1ull;
    }
    if (wanted > 0xffffffffull) {
        wanted = 0xffffffffull;
    }
    plan->fast_level = -1;
    plan->half_cycles = (uint32_t)wanted;
    for (i = 0; i < count; i++) {
        if ((uint32_t)fast_halfs[i] >= (uint32_t)wanted) {
            plan->fast_level = i;
            plan->half_cycles = fast_halfs[i];
            break;
        }
    }
    plan->actual_hz = SWD_Ultra_LevelHz(cpu_hz, plan->half_cycles);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Engine API (implemented in SWD_Fast_GPIO.c; stubs on other targets)        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Check whether the engine can be used right now.
 *
 * Returns 1 only when: the build is an ESP32-S3 with BIRDIEBOT_SWD_ULTRA_FAST
 * enabled, DAP_SPI_Deinit() has routed the pads to the CORE1 dedicated-GPIO
 * channels (validated by reading the GPIO matrix registers), and the calling
 * code runs on the dedicated-GPIO core (core 1 on the dual-core S3 build).
 */
int SWD_UltraFast_Available(void);

/**
 * @brief Lazy configuration: if the engine is not configured yet, configure it
 *        for the project default SWD clock (DAP_DEFAULT_SWJ_CLOCK).
 *
 * Used by the transfer/sequence entry points so that a host which sends
 * SWJ_Sequence or a transfer before DAP_SWJ_Clock still gets working clocks at
 * the default 2 MHz (the same effective default as before M5).
 *
 * @return 1 when the engine is configured and usable, 0 otherwise.
 */
int SWD_UltraFast_EnsureConfigured(void);

/**
 * @brief Configure the engine for a requested SWCLK ceiling.
 *
 * @param requested_hz host adapter speed (upper bound, not a target; 0 means
 *        the project default DAP_DEFAULT_SWJ_CLOCK)
 * @return actual SWCLK in Hz (<= requested_hz), or 0 when the engine cannot
 *         run (caller must report a DAP error; no legacy fallback on S3).
 */
uint32_t SWD_UltraFast_Enter(uint32_t requested_hz);

/** @brief Leave engine mode; idles SWCLK/SWDIO high and re-enables SWDIO output. */
void SWD_UltraFast_Exit(void);

/** @brief 1 while the engine is configured. */
int SWD_UltraFast_Active(void);

/** @brief Actual SWCLK of the configured mode in Hz, or 0 when inactive. */
uint32_t SWD_UltraFast_ActualHz(void);

/**
 * @brief Run one full CMSIS-DAP SWD transfer on the engine.
 *
 * Same contract as SWD_Transfer() in SW_DP.c:
 *  - request: DAP_TRANSFER_APnDP/RnW/A2/A3 (+ optional TIMESTAMP) bits;
 *  - returns DAP_TRANSFER_OK / WAIT / FAULT / ERROR;
 *  - honors DAP_Data.swd_conf.turnaround and .data_phase, the transfer idle
 *    cycles and DAP_TRANSFER_TIMESTAMP (captured at the same point in the
 *    sequence as the legacy GPIO backend did).
 * Must be called on the dedicated-GPIO core (see SWD_UltraFast_Available);
 * returns DAP_TRANSFER_ERROR otherwise.
 */
uint8_t SWD_Transfer_Ultra(uint32_t request, uint32_t *data);

/**
 * @brief SWJ_Sequence equivalent on the engine (line reset, JTAG-to-SWD,
 *        dormant wake-up; the SWDIO/TMS line is driven by the host).
 *
 * @param count_bits 1..256 bits, LSB first
 * @param data       bit data, (count_bits + 7) / 8 bytes
 * @return 1 on success, 0 when the engine is unavailable (caller reports a
 *         DAP error; there is no legacy fallback on S3).
 */
int SWD_UltraFast_SequenceWrite(uint32_t count_bits, const uint8_t *data);

/**
 * @brief SWD_Sequence capture (DIN) equivalent on the engine.
 *
 * Releases SWDIO, samples @p count_bits bits LSB first (like the legacy
 * SWD_Sequence GPIO path), re-enables SWDIO afterwards.
 *
 * @param count_bits 1..64 bits
 * @param data       output bytes, (count_bits + 7) / 8 bytes; must be non-NULL
 * @return 1 on success, 0 when the engine is unavailable.
 */
int SWD_UltraFast_SequenceRead(uint32_t count_bits, uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif /* SWD_FAST_GPIO_H */
