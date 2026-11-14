/**
 * @file SWD_Fast_GPIO.h
 * @brief ESP32-S3 ultra-fast (dedicated-GPIO / Xtensa asm) SWD backend - public API.
 *
 * M4 (user request): raise the SWD bit clock from the existing bit-bang fast
 * path (~12-13 MHz effective on the current 160 MHz ESP32-S3 build) towards
 * 20 MHz (160 MHz CPU) / 24 MHz (240 MHz CPU), i.e. the same order of
 * magnitude as XRDAP-ESP's 23.7 MHz dedicated-GPIO engine.
 *
 * Design (full rationale: docs/swd-23mhz-engine.md):
 *  - The S3's dedicated GPIO channels are wired by DAP_SPI_Deinit()
 *    (spi_switch.c): SWCLK -> CORE1_GPIO_OUT1 (bit 1), SWDIO ->
 *    CORE1_GPIO_OUT0 (bit 0), sample input -> CORE1_GPIO_IN0 (bit 0).
 *    One `wur.gpio_out` instruction drives BOTH pins, so a SWCLK edge and
 *    its SWDIO data bit are one instruction (no host/target turnaround
 *    skew, 50 % duty).
 *  - Every half period is a fixed, unrolled instruction count (`.rept` in
 *    inline asm), so the SWCLK frequency does not depend on compiler
 *    scheduling: f = CPU_CLOCK / (2 * half_cycles).
 *  - Levels are safe by construction: the slowest level is 12 MHz, and the
 *    fastest level is the fastest one whose half period is still
 *    >= SWD_ULTRA_MIN_HALF_NS (20 ns, slightly more margin than XRDAP's
 *    hardware-proven 23.7 MHz = 21.1 ns half period).  That is 20 MHz on
 *    the project's default 160 MHz CPU build and 24 MHz on a 240 MHz build;
 *    the request handed to @ref SWD_UltraFast_Enter is an upper bound, the
 *    engine never runs faster than requested.
 *  - Everything outside the bit loops (turnaround, idle, WAIT/FAULT dummy
 *    phases, protocol-error back-off) is plain code: SWD is synchronous and
 *    tolerates slow clocks there, so those paths stay small and simple.
 *  - Opt-in and fail-safe: DAP_SWJ_Clock only selects this backend when
 *    BIRDIEBOT_SWD_ULTRA_FAST is enabled, the request is >=
 *    BIRDIEBOT_SWD_ULTRA_MIN_CLOCK_HZ, and @ref SWD_UltraFast_Available
 *    proves the pin mapping and the calling core are right.  Any failure
 *    returns 0 and the caller keeps/falls back to the GPIO fast backend.
 *  - Default build: Kconfig defaults it to y (user request "enable the
 *    high-speed SWD engine by default"), but **the default SWD clock stays
 *    2 MHz**: the engine only engages when the host (OpenOCD adapter speed,
 *    target_flash, curve sampler) explicitly asks for >= 12 MHz.
 *
 * This header is intentionally free of ESP-IDF dependencies (only
 * <stdint.h>) so that the pure level/request/parity logic below is unit
 * tested on the host (components/DAP/test/host/).
 */

#ifndef SWD_FAST_GPIO_H
#define SWD_FAST_GPIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** Number of unrolled timing levels compiled into the engine. */
#define SWD_ULTRA_LEVEL_COUNT 4

/**
 * Safety floor for one SWCLK half period, in nanoseconds.
 *
 * XRDAP-ESP's hardware-validated 23.7 MHz engine runs with half = 21.1 ns
 * and explicitly refuses the next faster level because the target's data
 * is not valid early enough.  We keep a small extra margin: a level is only
 * used when half_cycles * 1e9 / cpu_hz >= 20 ns.
 */
#define SWD_ULTRA_MIN_HALF_NS 20u

/** Smallest half period (in CPU cycles) accepted for @p cpu_hz. */
static inline uint32_t SWD_Ultra_MinHalfCycles(uint32_t cpu_hz)
{
    /* ceil(SWD_ULTRA_MIN_HALF_NS * cpu_hz / 1e9), computed in 64 bits:
     * 20 ns * 240 MHz = 4.8 -> 5; 20 ns * 160 MHz = 3.2 -> 4. */
    uint64_t cycles = ((uint64_t)SWD_ULTRA_MIN_HALF_NS * (uint64_t)cpu_hz + 999999999ull) / 1000000000ull;
    return (uint32_t)cycles;
}

/** SWCLK frequency produced by a level with @p half_cycles per half period. */
static inline uint32_t SWD_Ultra_LevelHz(uint32_t cpu_hz, uint32_t half_cycles)
{
    if (half_cycles == 0u) {
        return 0u;
    }
    return cpu_hz / (2u * half_cycles);
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

/**
 * Pick the fastest level that is not faster than @p requested_hz.
 *
 * @param half_cycles  level half periods in CPU cycles, fastest first
 * @param count        number of entries (must be <= SWD_ULTRA_LEVEL_COUNT)
 * @param cpu_hz       CPU clock in Hz the levels are compiled for
 * @param requested_hz host adapter speed request (upper bound)
 * @return level index, or -1 when no level is slow enough (caller must fall
 *         back to the plain GPIO backend)
 */
static inline int SWD_Ultra_PickLevel(const uint16_t *half_cycles, int count,
                                      uint32_t cpu_hz, uint32_t requested_hz)
{
    int best = -1;
    uint32_t best_hz = 0u;
    int i;

    if (half_cycles == 0 || count <= 0 || requested_hz == 0u) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        uint32_t hz = SWD_Ultra_LevelHz(cpu_hz, half_cycles[i]);
        if (hz == 0u || hz > requested_hz) {
            continue; /* faster than the host asked for: not allowed */
        }
        if (hz > best_hz) {
            best_hz = hz;
            best = i;
        }
    }
    return best;
}

/* ------------------------------------------------------------------------- */
/* Engine API (implemented in SWD_Fast_GPIO.c)                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Check whether the ultra-fast backend can be used right now.
 *
 * Returns 1 only when: BIRDIEBOT_SWD_ULTRA_FAST is enabled, the target is an
 * ESP32-S3, DAP_SPI_Deinit() has routed the pads to the CORE1 dedicated-GPIO
 * channels (validated by reading the GPIO matrix registers), the calling
 * task runs on the dedicated-GPIO core (CORE1, i.e. core 1 on a dual-core
 * S3 build), and the CPU clock is >= 160 MHz.
 */
int SWD_UltraFast_Available(void);

/**
 * @brief Enter ultra-fast mode for the requested SWCLK ceiling.
 *
 * @param requested_hz Host adapter speed (upper bound, not a target).
 * @return actual SWCLK in Hz (<= requested_hz), or 0 when the backend cannot
 *         serve that request; the caller must then fall back to the GPIO
 *         fast backend.
 */
uint32_t SWD_UltraFast_Enter(uint32_t requested_hz);

/** @brief Leave ultra-fast mode; idles SWCLK/SWDIO high and re-enables SWDIO output. */
void SWD_UltraFast_Exit(void);

/** @brief 1 while ultra-fast mode is configured and usable. */
int SWD_UltraFast_Active(void);

/** @brief Actual SWCLK of the configured level in Hz, or 0 when inactive. */
uint32_t SWD_UltraFast_ActualHz(void);

/**
 * @brief Run one full CMSIS-DAP SWD transfer on the ultra-fast backend.
 *
 * Same contract as SWD_Transfer()/SWD_Transfer_GPIO() in SW_DP.c:
 *  - request: DAP_TRANSFER_APnDP/RnW/A2/A3 (+ optional TIMESTAMP) bits;
 *  - returns DAP_TRANSFER_OK / WAIT / FAULT / ERROR;
 *  - honors DAP_Data.swd_conf.turnaround and .data_phase, the transfer idle
 *    cycles and DAP_TRANSFER_TIMESTAMP (captures DAP_Data.timestamp at the
 *    same point in the sequence as the GPIO backend).
 * Must be called on the dedicated-GPIO core (see SWD_UltraFast_Available).
 */
uint8_t SWD_Transfer_Ultra(uint32_t request, uint32_t *data);

#ifdef __cplusplus
}
#endif

#endif /* SWD_FAST_GPIO_H */
