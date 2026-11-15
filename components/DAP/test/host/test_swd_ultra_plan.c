/*
 * Host-side unit test for the pure (ESP-IDF-free) logic of the ESP32-S3
 * dedicated-GPIO SWD engine (components/DAP/cmsis-dap/source/SWD_Fast_GPIO.h):
 * SWD request header construction, parity, ACK decoding, the fast/paced level
 * plan (all rates 100 kHz .. 24 MHz), and the timing-budget arithmetic used in
 * docs/swd-23mhz-engine.md.
 *
 * The Xtensa assembly itself cannot be executed on the host - it is verified
 * by compiling the real SWD_Fast_GPIO.c with the xtensa-esp32s3-elf toolchain
 * (the ESP-IDF build does that) and by the HIL checklist.  This test guards the
 * arithmetic and the safety rules that decide *which* bit engine and half
 * period is used on the wire, because a wrong level is a wrong SWCLK.
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
    CHECK_EQ(SWD_Ultra_Parity4(0xF0u), 0u); /* only the low nibble counts */
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
    /* Timestamp/match request bits (>= bit4) must not leak into the header. */
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

/* ---- 4. half-period / frequency arithmetic ------------------------------ */
static void test_level_hz(void)
{
    printf("[level-hz]\n");
    /* 160 MHz CPU (project default): 20/16/13.3/11.4/10/8/6.67/5/4 MHz */
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 4u), 20000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 5u), 16000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 6u), 13333333u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 7u), 11428571u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 8u), 10000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 10u), 8000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 12u), 6666666u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 16u), 5000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 20u), 4000000u);
    /* 240 MHz CPU: 24/20/17.1/15/12/10/7.5/6 MHz */
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 5u), 24000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 6u), 20000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 12u), 10000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(240000000u, 20u), 6000000u);
    CHECK_EQ(SWD_Ultra_LevelHz(160000000u, 0u), 0u);
    /* Safety floor: fastest level must be at least SWD_ULTRA_MIN_HALF_NS. */
    CHECK_EQ(SWD_Ultra_MinHalfCycles(160000000u), 4u);
    CHECK_EQ(SWD_Ultra_MinHalfCycles(240000000u), 5u);
    CHECK_EQ(SWD_Ultra_MinHalfCycles(80000000u), 2u);
    /* Sequence half clamp: never faster than SWD_ULTRA_SEQ_MIN_HALF. */
    CHECK_EQ(SWD_Ultra_SequenceHalf(4u), SWD_ULTRA_SEQ_MIN_HALF);
    CHECK_EQ(SWD_Ultra_SequenceHalf(10u), 10u);
    CHECK_EQ(SWD_Ultra_SequenceHalf(800u), 800u);
}

/* Fast level tables exactly as compiled into SWD_Fast_GPIO.c. */
static const uint16_t kTable160[] = { 4, 5, 6, 7, 8, 10, 12, 16, 20 };
static const uint16_t kTable240[] = { 5, 6, 7, 8, 10, 12, 16, 20 };
enum {
    kTable160Count = (int)(sizeof(kTable160) / sizeof(kTable160[0])),
    kTable240Count = (int)(sizeof(kTable240) / sizeof(kTable240[0]))
};

static void check_table(const char *name, const uint16_t *table, int count,
                        uint32_t cpu_hz, uint32_t expected_fastest_hz)
{
    int i;
    printf("[table %s]\n", name);
    CHECK_EQ(SWD_Ultra_LevelHz(cpu_hz, table[0]), expected_fastest_hz);
    for (i = 0; i < count; i++) {
        uint32_t hz = SWD_Ultra_LevelHz(cpu_hz, table[i]);
        uint64_t half_ns = ((uint64_t)table[i] * 1000000000ull) / (uint64_t)cpu_hz;
        /* Safety floor */
        CHECK(half_ns >= SWD_ULTRA_MIN_HALF_NS);
        CHECK(table[i] >= SWD_Ultra_MinHalfCycles(cpu_hz));
        /* strictly slower than the previous level */
        if (i > 0) {
            CHECK(hz < SWD_Ultra_LevelHz(cpu_hz, table[i - 1]));
        }
    }
    /* Slowest fast level must be <= 6 MHz so the paced path takes over for the
     * true low-speed requests (and covers everything below). */
    CHECK(SWD_Ultra_LevelHz(cpu_hz, table[count - 1]) <= 6000000u);
}

static void plan_check(uint32_t cpu_hz, uint32_t requested_hz, int32_t expected_level,
                       uint32_t expected_half, uint32_t expected_hz)
{
    swd_ultra_plan_t plan;
    const uint16_t *table = (cpu_hz == 160000000u) ? kTable160 : kTable240;
    int count = (cpu_hz == 160000000u) ? kTable160Count : kTable240Count;
    int rc;

    memset(&plan, 0xff, sizeof(plan));
    rc = SWD_Ultra_Plan(cpu_hz, requested_hz, table, count, &plan);
    CHECK_EQ(rc, 0);
    CHECK_EQ(plan.fast_level, expected_level);
    CHECK_EQ(plan.half_cycles, expected_half);
    CHECK_EQ(plan.actual_hz, expected_hz);
    /* The engine is never faster than requested. */
    CHECK(plan.actual_hz <= requested_hz);
}

static void test_plan_mapping(void)
{
    printf("[plan-160MHz]\n");
    plan_check(160000000u, 20000000u, 0, 4, 20000000u);
    plan_check(160000000u, 19000000u, 1, 5, 16000000u);
    plan_check(160000000u, 16000000u, 1, 5, 16000000u);
    plan_check(160000000u, 15000000u, 2, 6, 13333333u);
    plan_check(160000000u, 12000000u, 3, 7, 11428571u);
    plan_check(160000000u, 10000000u, 4, 8, 10000000u);
    plan_check(160000000u, 8000000u, 5, 10, 8000000u);
    plan_check(160000000u, 6666667u, 6, 12, 6666666u);
    plan_check(160000000u, 5000000u, 7, 16, 5000000u);
    plan_check(160000000u, 4000000u, 8, 20, 4000000u);
    plan_check(160000000u, 3900000u, -1, 21, 3809523u);
    plan_check(160000000u, 2000000u, -1, 40, 2000000u);
    plan_check(160000000u, 1000000u, -1, 80, 1000000u);
    plan_check(160000000u, 500000u, -1, 160, 500000u);
    plan_check(160000000u, 100000u, -1, 800, 100000u);
    plan_check(160000000u, 24000000u, 0, 4, 20000000u); /* over-max clamps */

    printf("[plan-240MHz]\n");
    plan_check(240000000u, 24000000u, 0, 5, 24000000u);
    plan_check(240000000u, 23000000u, 1, 6, 20000000u);
    plan_check(240000000u, 20000000u, 1, 6, 20000000u);
    plan_check(240000000u, 18000000u, 2, 7, 17142857u);
    plan_check(240000000u, 17000000u, 3, 8, 15000000u);
    plan_check(240000000u, 15000000u, 3, 8, 15000000u);
    plan_check(240000000u, 12000000u, 4, 10, 12000000u);
    plan_check(240000000u, 10000000u, 5, 12, 10000000u);
    plan_check(240000000u, 7500000u, 6, 16, 7500000u);
    plan_check(240000000u, 6000000u, 7, 20, 6000000u);
    plan_check(240000000u, 5000000u, -1, 24, 5000000u);
    plan_check(240000000u, 100000u, -1, 1200, 100000u);

    /* Invalid arguments never produce a plan. */
    {
        swd_ultra_plan_t plan;
        CHECK_EQ(SWD_Ultra_Plan(160000000u, 1000000u, NULL, 9, &plan), -1);
        CHECK_EQ(SWD_Ultra_Plan(160000000u, 1000000u, kTable160, 0, &plan), -1);
        CHECK_EQ(SWD_Ultra_Plan(160000000u, 1000000u, kTable160, 9, NULL), -1);
        CHECK_EQ(SWD_Ultra_Plan(0u, 1000000u, kTable160, 9, &plan), -1);
        CHECK_EQ(SWD_Ultra_Plan(160000000u, 0u, kTable160, 9, &plan), -1);
        CHECK_EQ(SWD_Ultra_Plan(160000000u, 1000000u, kTable160,
                                SWD_ULTRA_FAST_LEVEL_MAX + 1, &plan), -1);
    }
}

/* Safety sweep: for every request from 100 kHz to 24 MHz the plan must be
 * valid, never faster than the request, within -25 % up to the fastest level,
 * and monotone (a higher request never selects a slower actual clock). */
static void test_plan_safety(const char *name, uint32_t cpu_hz, const uint16_t *table,
                             int count, uint32_t fastest_hz)
{
    uint32_t prev_actual = 0u;
    uint64_t req;
    int checked = 0;

    printf("[plan-safety %s]\n", name);
    for (req = 100000u; req <= 24000000u; req += 25000u) {
        swd_ultra_plan_t plan;
        int rc = SWD_Ultra_Plan(cpu_hz, (uint32_t)req, table, count, &plan);
        CHECK_EQ(rc, 0);
        CHECK(plan.half_cycles >= 1u);
        CHECK(plan.actual_hz <= (uint32_t)req);
        if ((uint32_t)req <= fastest_hz) {
            CHECK(plan.actual_hz * 4u >= (uint32_t)req * 3u); /* >= -25 % */
        } else {
            CHECK_EQ(plan.actual_hz, fastest_hz); /* over-max: clamp to fastest */
        }
        CHECK(plan.actual_hz >= prev_actual); /* monotone non-decreasing */
        if (plan.fast_level >= 0) {
            CHECK_EQ(plan.half_cycles, table[plan.fast_level]);
        } else {
            /* paced: half = ceil(cpu / (2 * request)) */
            CHECK_EQ(plan.half_cycles,
                     (uint32_t)(((uint64_t)cpu_hz + (2ull * req) - 1ull) / (2ull * req)));
        }
        prev_actual = plan.actual_hz;
        checked++;
    }
    printf("  (%d requests checked)\n", checked);
}

static void test_wire_budget(void)
{
    /* SWD cycle budget per 32-bit transfer used in docs/swd-23mhz-engine.md:
     * 8 header + turnaround + 3 ACK + 33 data + turnaround = 46 SWCLK. */
    const uint32_t clocks = 46u;
    printf("[wire-budget]\n");
    CHECK_EQ(clocks, 46u);
    /* At 100 kHz a word costs 460 us; at 16 MHz ~2.9 us. */
    CHECK_EQ((uint64_t)clocks * 10ull, 460ull);      /* us at 100 kHz */
    CHECK_EQ((uint64_t)clocks * 1000ull / 16000ull, 2ull); /* floor us at 16 MHz */
    /* Bit-clock ratio between the recommended adapter speed and 100 kHz. */
    CHECK_EQ(16000000u / 100000u, 160u);
}

int main(void)
{
    printf("SWD dedicated-GPIO engine host plan test (M5: all rates)\n");
    test_parity4();
    test_parity32();
    test_request_byte();
    test_ack();
    test_level_hz();
    check_table("160MHz", kTable160, kTable160Count, 160000000u, 20000000u);
    check_table("240MHz", kTable240, kTable240Count, 240000000u, 24000000u);
    test_plan_mapping();
    test_plan_safety("160MHz", 160000000u, kTable160, kTable160Count, 20000000u);
    test_plan_safety("240MHz", 240000000u, kTable240, kTable240Count, 24000000u);
    test_wire_budget();
    printf("\n== %d checks passed, %d failed ==\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
