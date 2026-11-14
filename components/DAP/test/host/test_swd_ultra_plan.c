/*
 * Host-side unit test for the pure (ESP-IDF-free) logic of the ESP32-S3
 * ultra-fast SWD engine in components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h:
 * SWD request header construction, parity, ACK decoding and the timing level
 * table / level selection rules.
 *
 * The Xtensa assembly itself cannot be executed on the host - it is verified
 * by compiling the real SWD_Fast_GPIO.c with the xtensa-esp32s3-elf toolchain
 * (see docs/swd-23mhz-engine.md §validation) and by the HIL checklist.  This
 * test guards the arithmetic and the safety rules that decide *which* level
 * is used on the wire, because a wrong level is a wrong SWCLK frequency.
 *
 * Build/run: components/DAP/test/host/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "SWD_Fast_GPIO.h"

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        long long _a = (long long)(actual);                                    \
        long long _e = (long long)(expected);                                  \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__,        \
                   __LINE__, #actual, _a, _e);                                 \
            g_fail++;                                                          \
        } else {                                                               \
            g_pass++;                                                          \
        }                                                                      \
    } while (0)

/* ---- 1. parity ---------------------------------------------------------- */
static void test_parity4(void)
{
    int v;
    printf("[parity4]\n");
    for (v = 0; v < 16; v++) {
        int ones = 0;
        int i;
        for (i = 0; i < 4; i++) {
            ones += (v >> i) & 1;
        }
        CHECK_EQ(SWD_Ultra_Parity4((uint32_t)v), ones & 1);
    }
    /* Only the low nibble may be used, like the request header does. */
    CHECK_EQ(SWD_Ultra_Parity4(0xF0u), 0u);
    CHECK_EQ(SWD_Ultra_Parity4(0xF1u), 1u);
}

static void test_parity32(void)
{
    int i;
    printf("[parity32]\n");
    CHECK_EQ(SWD_Ultra_Parity32(0x00000000u), 0u);
    CHECK_EQ(SWD_Ultra_Parity32(0x00000001u), 1u);
    CHECK_EQ(SWD_Ultra_Parity32(0x00000003u), 0u);
    CHECK_EQ(SWD_Ultra_Parity32(0x7fffffffu), 1u);
    CHECK_EQ(SWD_Ultra_Parity32(0x80000000u), 1u);
    CHECK_EQ(SWD_Ultra_Parity32(0xffffffffu), 0u);
    /* self-consistency over a spread of values */
    for (i = 0; i < 32; i++) {
        uint32_t v = 0x9e3779b9u * (uint32_t)(i + 1);
        uint32_t ones = 0u;
        uint32_t t = v;
        while (t != 0u) {
            ones ^= t & 1u;
            t >>= 1;
        }
        CHECK_EQ(SWD_Ultra_Parity32(v), ones);
    }
}

/* ---- 2. request header byte -------------------------------------------- */
static void test_request_byte(void)
{
    printf("[request-byte]\n");
    /* Known request -> wire-byte pairs from the CMSIS-DAP SWD protocol and the
     * debug table in SW_DP.c.  request = APnDP | RnW<<1 | A2<<2 | A3<<3. */
    CHECK_EQ(SWD_Ultra_RequestByte(0x0u), 0x81u); /* DP write A3:A2=00 ABORT      */
    CHECK_EQ(SWD_Ultra_RequestByte(0x2u), 0xA5u); /* DP read  A3:A2=00 IDCODE     */
    CHECK_EQ(SWD_Ultra_RequestByte(0x4u), 0xA9u); /* DP write A3:A2=01 CTRL/STAT  */
    CHECK_EQ(SWD_Ultra_RequestByte(0x6u), 0x8Du); /* DP read  A3:A2=01 CTRL/STAT  */
    CHECK_EQ(SWD_Ultra_RequestByte(0x8u), 0xB1u); /* DP write A3:A2=10 SELECT     */
    CHECK_EQ(SWD_Ultra_RequestByte(0xEu), 0xBDu); /* DP read  A3:A2=11 RDBUFF     */
    CHECK_EQ(SWD_Ultra_RequestByte(0x1u), 0xA3u); /* AP write A3:A2=00 AP0        */
    CHECK_EQ(SWD_Ultra_RequestByte(0x3u), 0x87u); /* AP read  A3:A2=00 AP0        */
    CHECK_EQ(SWD_Ultra_RequestByte(0x5u), 0x8Bu); /* AP write A3:A2=01 AP4        */
    CHECK_EQ(SWD_Ultra_RequestByte(0xDu), 0xBBu); /* AP write A3:A2=11 APc        */
    CHECK_EQ(SWD_Ultra_RequestByte(0xFu), 0x9Fu); /* AP read  A3:A2=11 APc        */
    CHECK_EQ(SWD_Ultra_RequestByte(0xBu), 0xB7u); /* AP read  A3:A2=10 AP8        */
    /* Header invariants: start bit set, stop bit clear, park bit set. */
    CHECK_EQ(SWD_Ultra_RequestByte(0x0u) & 0x01u, 0x01u);
    CHECK_EQ(SWD_Ultra_RequestByte(0x0u) & 0x40u, 0x00u);
    CHECK_EQ(SWD_Ultra_RequestByte(0x0u) & 0x80u, 0x80u);
    /* The timestamp/match request bits (>= bit4) must not leak into the header. */
    CHECK_EQ(SWD_Ultra_RequestByte(0x2u | (1u << 7)), 0xA5u);
    CHECK_EQ(SWD_Ultra_RequestByte(0x2u | (1u << 4)), 0xA5u);
}

/* ---- 3. ACK decoding ---------------------------------------------------- */
static void test_ack(void)
{
    printf("[ack]\n");
    CHECK_EQ(SWD_Ultra_AckFromBits(0x1u), 0x1u); /* OK    */
    CHECK_EQ(SWD_Ultra_AckFromBits(0x2u), 0x2u); /* WAIT  */
    CHECK_EQ(SWD_Ultra_AckFromBits(0x4u), 0x4u); /* FAULT */
    CHECK_EQ(SWD_Ultra_AckFromBits(0x0u), 0x0u); /* no ACK */
    CHECK_EQ(SWD_Ultra_AckFromBits(0x7u), 0x7u); /* protocol error */
    CHECK_EQ(SWD_Ultra_AckFromBits(0xFu), 0x7u); /* only 3 bits are ACK */
}

/* ---- 4. half-period / frequency budget ---------------------------------- */
static void test_level_hz(void)
{
    printf("[level-hz]\n");
    /* 160 MHz CPU (project default): 20 / 16 / 13.33 / 10 MHz */
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 4u), 20000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 5u), 16000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 6u), 13333333u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 8u), 10000000u);
    /* 240 MHz CPU (XRDAP-class): 24 / 20 / 15 / 12 MHz */
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 5u), 24000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 6u), 20000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 8u), 15000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 10u), 12000000u);
    /* degenerate input */
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 0u), 0u);
    /* The safety floor: >= 20 ns per half period. */
    CHECK_EQ(SWD_Ultra_MinHalfCycles(160000000u), 4u);
    CHECK_EQ(SWD_Ultra_MinHalfCycles(240000000u), 5u);
    CHECK_EQ(SWD_Ultra_MinHalfCycles(80000000u), 2u);
}

/* Level tables exactly as compiled into SWD_Fast_GPIO.c for each CPU clock. */
static const uint16_t kTable160[SWD_ULTRA_LEVEL_COUNT] = { 4, 5, 6, 8 };
static const uint16_t kTable240[SWD_ULTRA_LEVEL_COUNT] = { 5, 6, 8, 10 };

static void check_table(const char *name, const uint16_t *table, uint32_t cpu_hz,
                        uint32_t expected_fastest_hz)
{
    int i;
    printf("[table %s]\n", name);
    /* Each level must respect the half-period safety floor and be sane. */
    for (i = 0; i < SWD_ULTRA_LEVEL_COUNT; i++) {
        uint32_t hz = SWD_Ultra_LevelHz(cpu_hz, table[i]);
        uint64_t half_ns = ((uint64_t)table[i] * 1000000000ull) / (uint64_t)cpu_hz;
        CHECK(half_ns >= SWD_ULTRA_MIN_HALF_NS);
        CHECK(hz >= 5000000u && hz <= 30000000u);
        /* strictly slower than the previous level */
        if (i > 0) {
            CHECK(hz < SWD_Ultra_LevelHz(cpu_hz, table[i - 1]));
        }
    }
    /* Fastest level must be the documented target for this CPU clock. */
    CHECK_EQ(SWD_Ultra_LevelHz(cpu_hz, table[0]), expected_fastest_hz);
    /* Slowest level must be usable for the 12 MHz minimum request. */
    CHECK(SWD_Ultra_LevelHz(cpu_hz, table[SWD_ULTRA_LEVEL_COUNT - 1]) <= 12000000u);
}

static void test_pick_level(void)
{
    int j;
    uint32_t req;

    printf("[pick-level 160MHz]\n");
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 25000000u), 0);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 20000000u), 0);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 19000000u), 1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 16000000u), 1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 15000000u), 2);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 13333334u), 2);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 13333333u), 2);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 13000000u), 3);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 12000000u), 3);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 10000000u), 3);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 9999999u), -1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, 0u), -1);

    printf("[pick-level 240MHz]\n");
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 25000000u), 0);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 24000000u), 0);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 23000000u), 1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 20000000u), 1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 15000000u), 2);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 12000000u), 3);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, 11999999u), -1);

    /* Invalid arguments never select a level. */
    CHECK_EQ(SWD_Ultra_PickLevel(NULL, SWD_ULTRA_LEVEL_COUNT, 160000000u, 20000000u), -1);
    CHECK_EQ(SWD_Ultra_PickLevel(kTable160, 0, 160000000u, 20000000u), -1);

    /* Safety property: never faster than requested, and always the fastest
     * level that satisfies that bound.  Sweep the whole plausible range. */
    printf("[pick-level safety sweep]\n");
    for (req = 8000000u; req <= 30000000u; req += 250000u) {
        int idx = SWD_Ultra_PickLevel(kTable160, SWD_ULTRA_LEVEL_COUNT, 160000000u, req);
        if (idx >= 0) {
            uint32_t got = SWD_Ultra_LevelHz(160000000u, kTable160[idx]);
            CHECK(got <= req);
            for (j = 0; j < SWD_ULTRA_LEVEL_COUNT; j++) {
                uint32_t other = SWD_Ultra_LevelHz(160000000u, kTable160[j]);
                if (other <= req) {
                    /* the chosen level must be at least as fast as any other
                     * allowed level */
                    CHECK(got >= other);
                }
            }
        } else {
            /* no level allowed => every level must be faster than request */
            for (j = 0; j < SWD_ULTRA_LEVEL_COUNT; j++) {
                CHECK(SWD_Ultra_LevelHz(160000000u, kTable160[j]) > req);
            }
        }
    }
    /* Same property on the 240 MHz table. */
    for (req = 10000000u; req <= 30000000u; req += 250000u) {
        int idx = SWD_Ultra_PickLevel(kTable240, SWD_ULTRA_LEVEL_COUNT, 240000000u, req);
        if (idx >= 0) {
            CHECK(SWD_Ultra_LevelHz(240000000u, kTable240[idx]) <= req);
        }
    }
}

static void test_wire_budget(void)
{
    /* SWD cycle budget per transfer used in docs/swd-23mhz-engine.md:
     * 8 header + turnaround + 3 ACK + (33 data clocks for RnW) + turnaround
     * (+ idle_cycles).  Sanity-check the rate difference the engine buys. */
    uint32_t header = 8u;
    uint32_t turnaround = 1u;
    uint32_t ack = 3u;
    uint32_t data = 33u;
    uint32_t clocks = header + turnaround + ack + data + turnaround;
    uint64_t ns_1mhz = (uint64_t)clocks * 1000ull;          /* 1 us per clock */
    uint64_t ns_20mhz = (uint64_t)clocks * 50ull;           /* 50 ns per clock */

    printf("[wire-budget]\n");
    CHECK_EQ(clocks, 46u);
    CHECK_EQ(ns_1mhz, 46000ull);
    CHECK_EQ(ns_20mhz, 2300ull);
    CHECK(ns_1mhz / ns_20mhz == 20u);
}

int main(void)
{
    printf("SWD ultra-fast engine host plan test\n");
    test_parity4();
    test_parity32();
    test_request_byte();
    test_ack();
    test_level_hz();
    check_table("160MHz", kTable160, 160000000u, 20000000u);
    check_table("240MHz", kTable240, 240000000u, 24000000u);
    test_pick_level();
    test_wire_budget();
    printf("\n== %d checks passed, %d failed ==\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
