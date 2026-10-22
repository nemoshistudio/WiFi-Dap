/* Host-side regression test for the curve sampler JSON parser + expression
 * evaluator (components/curve_sampler/src/curve_json.c / curve_expr.c).
 * Not part of the firmware build; compiled in /tmp against a cJSON copy and
 * stubbed esp_err.h / tf_swd_read_mem. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "curve_priv.h"

/* ---- tf_swd mock -------------------------------------------------------- */
static uint32_t g_pointer_value;
static uint32_t g_last_read_addr;
static int g_read_count;

esp_err_t tf_swd_read_mem(uint32_t addr, void *buf, size_t len)
{
    g_last_read_addr = addr;
    g_read_count++;
    if (len == 4 && buf != NULL) {
        memcpy(buf, &g_pointer_value, 4);
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ---- tiny test framework ------------------------------------------------- */
static int g_failures;
static int g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static void test_addr_filter(void)
{
    CHECK(curve_addr_range_ok(0x20000000U, 4U));
    CHECK(curve_addr_range_ok(0x2001FFFCU, 4U));
    CHECK(!curve_addr_range_ok(0x2001FFFEU, 4U));
    CHECK(!curve_addr_range_ok(0x20020000U, 4U));
    CHECK(!curve_addr_range_ok(0x1FFFFFFCU, 4U));
    CHECK(curve_addr_range_ok(0x24000000U, 8U));
    CHECK(curve_addr_range_ok(0x2407FFFCU, 4U));
    CHECK(!curve_addr_range_ok(0x24080000U, 4U));
    CHECK(curve_addr_range_ok(0x08000000U, 4U));
    CHECK(curve_addr_range_ok(0x08FFFFFCU, 4U));
    CHECK(!curve_addr_range_ok(0x09000000U, 4U));
    CHECK(!curve_addr_range_ok(0xFFFFFFFEU, 4U)); /* u32 overflow wrap */
    CHECK(!curve_addr_range_ok(0x40000000U, 4U));
    CHECK(!curve_addr_range_ok(0x20000000U, 0U));
}

static void test_parse_contract_example(void)
{
    static const char *json =
        "{\"interval_us\":20000,\"halt\":true,"
        "\"series\":["
        "{\"id\":0,\"size\":4,\"expr\":{\"kind\":\"reg\",\"reg\":13,\"offset\":244}},"
        "{\"id\":1,\"size\":8,\"expr\":{\"kind\":\"deref\","
        "\"inner\":{\"kind\":\"addr\",\"address\":603991984},\"offset\":220}}]}";
    static curve_config_t cfg;
    char err[64] = {0};

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.interval_us == 20000U);
    CHECK(cfg.halt == true);
    CHECK(cfg.series_count == 2U);
    CHECK(cfg.sample_bytes == 12U);
    CHECK(cfg.needs_regs == true);
    CHECK(cfg.reg_count == 1U);
    CHECK(cfg.regs[0] == 13U);
    CHECK(cfg.series[0].size == 4U);
    CHECK(cfg.series[0].expr != CURVE_EXPR_NONE);
    CHECK(cfg.pool[cfg.series[0].expr].kind == CURVE_EXPR_REG);
    CHECK(cfg.pool[cfg.series[0].expr].reg == 13U);
    CHECK(cfg.pool[cfg.series[0].expr].offset == 244);
    CHECK(cfg.pool[cfg.series[1].expr].kind == CURVE_EXPR_DEREF);
    CHECK(cfg.pool[cfg.series[1].expr].inner != CURVE_EXPR_NONE);
    CHECK(cfg.pool[cfg.pool[cfg.series[1].expr].inner].kind == CURVE_EXPR_ADDR);
    CHECK(cfg.pool[cfg.pool[cfg.series[1].expr].inner].address == 603991984U);
}

static void test_parse_limits(void)
{
    static curve_config_t cfg;
    char err[64];
    char json[4096];

    /* interval too small / too large */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":4999,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":200001,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* size 0 and 9 invalid */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":0,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":9,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* reg > 15 */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"reg\",\"reg\":16}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* halt=false with a register expression is rejected */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":false,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"reg\",\"reg\":13}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* unsupported kind */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"nope\"}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* add without inner */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"add\",\"offset\":4}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
}

static void test_parse_series_counts(void)
{
    static curve_config_t cfg;
    char err[64];
    char json[4096];
    size_t pos;

    /* 16 series accepted */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 16; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"id\":%d,\"size\":8,\"expr\":{\"kind\":\"addr\","
                                "\"address\":%u}}",
                                (i == 0) ? "" : ",", i, 0x20000000U + (uint32_t)i * 8U);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.series_count == 16U);
    CHECK(cfg.sample_bytes == 128U);

    /* 17 series rejected */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 17; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"size\":1,\"expr\":{\"kind\":\"addr\","
                                "\"address\":%u}}",
                                (i == 0) ? "" : ",", 0x20000000U + (uint32_t)i);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
}

static void test_parse_depth(void)
{
    static curve_config_t cfg;
    char err[64];
    char json[4096];
    size_t pos;

    /* depth 8: add( add( ... addr ...)) with 7 add wrappers + addr = 8 nodes */
    pos = (size_t)snprintf(json, sizeof(json),
                           "{\"interval_us\":20000,\"series\":[{\"size\":4,\"expr\":");
    for (int i = 0; i < 7; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "{\"kind\":\"add\",\"offset\":4,\"inner\":");
    }
    pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                            "{\"kind\":\"addr\",\"address\":536870912}");
    for (int i = 0; i < 7; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);

    /* depth 9: 8 adds + addr = 9 nodes -> rejected */
    pos = (size_t)snprintf(json, sizeof(json),
                           "{\"interval_us\":20000,\"series\":[{\"size\":4,\"expr\":");
    for (int i = 0; i < 8; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "{\"kind\":\"add\",\"offset\":4,\"inner\":");
    }
    pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                            "{\"kind\":\"addr\",\"address\":536870912}");
    for (int i = 0; i < 8; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
}

static void eval_expr(const curve_config_t *cfg, const curve_expr_t *expr,
                      const uint32_t *regs, const bool *valid, uint32_t *addr)
{
    uint16_t index = (uint16_t)(expr - cfg->pool);
    g_read_count = 0;
    CHECK(curve_expr_eval(cfg, index, regs, valid, addr) == CURVE_EVAL_OK);
}

static void test_eval(void)
{
    static curve_config_t cfg;
    char err[64];
    static const char *json =
        "{\"interval_us\":20000,\"series\":["
        "{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":603991984}},"
        "{\"size\":4,\"expr\":{\"kind\":\"reg\",\"reg\":7,\"offset\":-8}},"
        "{\"size\":4,\"expr\":{\"kind\":\"cfa\",\"offset\":12}},"
        "{\"size\":4,\"expr\":{\"kind\":\"add\","
        "\"inner\":{\"kind\":\"addr\",\"address\":536870912},\"offset\":16}},"
        "{\"size\":4,\"expr\":{\"kind\":\"deref\","
        "\"inner\":{\"kind\":\"addr\",\"address\":603991984},\"offset\":220}}]}";
    uint32_t regs[CURVE_MAX_CORE_REGS] = {0};
    bool valid[CURVE_MAX_CORE_REGS] = {false};
    uint32_t addr = 0U;

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    regs[7] = 0x20000010U;
    regs[13] = 0x20001000U;
    valid[7] = true;
    valid[13] = true;

    eval_expr(&cfg, &cfg.pool[cfg.series[0].expr], regs, valid, &addr);
    CHECK(addr == 603991984U);
    eval_expr(&cfg, &cfg.pool[cfg.series[1].expr], regs, valid, &addr);
    CHECK(addr == 0x20000008U);
    eval_expr(&cfg, &cfg.pool[cfg.series[2].expr], regs, valid, &addr);
    CHECK(addr == 0x2000100CU);
    eval_expr(&cfg, &cfg.pool[cfg.series[3].expr], regs, valid, &addr);
    CHECK(addr == 0x20000010U);
    g_pointer_value = 0x24000000U;
    eval_expr(&cfg, &cfg.pool[cfg.series[4].expr], regs, valid, &addr);
    CHECK(addr == 0x240000DCU);
    CHECK(g_read_count == 1);

    /* pointer value outside the allowed windows -> INVALID, and the address
     * of the pointer fetch must still be validated before any bus access */
    g_pointer_value = 0x40000000U;
    g_read_count = 0;
    CHECK(curve_expr_eval(&cfg, cfg.series[4].expr, regs, valid, &addr) ==
          CURVE_EVAL_OK); /* pointer load itself is valid; final addr is filtered later */
    CHECK(addr == 0x400000DCU);

    /* invalid pointer fetch address: addr 0x40000000 not allowed -> INVALID
     * without a bus read */
    (void)snprintf(err, sizeof(err), "%s", "");
    {
        curve_config_t c2;
        static const char *json2 =
            "{\"interval_us\":20000,\"series\":[{\"size\":4,"
            "\"expr\":{\"kind\":\"deref\",\"inner\":{\"kind\":\"addr\","
            "\"address\":1073741824},\"offset\":0}}]}";
        CHECK(curve_config_parse(json2, strlen(json2), &c2, err, sizeof(err)) == ESP_OK);
        g_read_count = 0;
        CHECK(curve_expr_eval(&c2, c2.series[0].expr, regs, valid, &addr) ==
              CURVE_EVAL_INVALID);
        CHECK(g_read_count == 0);
    }

    /* register not marked valid -> INVALID */
    valid[7] = false;
    CHECK(curve_expr_eval(&cfg, cfg.series[1].expr, regs, valid, &addr) ==
          CURVE_EVAL_INVALID);
}


static void test_ring_capacity(void)
{
    /* capacity = clamp(32 KiB / (12 + frame_bytes), 256, 512) */
    CHECK(curve_ring_capacity_for(16U) == 512U);
    CHECK(curve_ring_capacity_for(28U) == 512U);
    CHECK(curve_ring_capacity_for(64U) == 431U);  /* 32768 / 76 */
    CHECK(curve_ring_capacity_for(128U) == 256U); /* 234 -> clamp to 256 */
    CHECK(curve_ring_capacity_for(0U) == 0U);
    CHECK(curve_ring_capacity_for(CURVE_MAX_SAMPLE_BYTES + 1U) == 0U);
}

/* Client polls promptly (every 3 frames, 32 per poll): physical ring reuse is
 * constant but no frame is ever dropped before being delivered, so
 * `overwritten` must stay 0 even after many ring wraps. */
static void test_ring_steady_consumer(void)
{
    static curve_ring_t ring;
    uint32_t slot = 0U;
    uint32_t seq = 0U;
    uint32_t first = 0U;
    uint32_t count;
    bool gap = false;

    curve_ring_reset(&ring, 256U);
    for (uint32_t i = 0U; i < 1000U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        CHECK(seq == i + 1U);
        curve_ring_commit_write(&ring);
        if ((i % 3U) == 2U) {
            count = curve_ring_plan_read(&ring, ring.served_seq, 32U, &first, &gap);
            CHECK(count > 0U);
            CHECK(!gap);
            CHECK(first == ring.served_seq + 1U);
            curve_ring_mark_served(&ring, first, count);
        }
    }
    /* Final poll catches the frames written after the last modulo poll. */
    count = curve_ring_plan_read(&ring, ring.served_seq, 32U, &first, &gap);
    CHECK(count > 0U);
    curve_ring_mark_served(&ring, first, count);
    CHECK(ring.overwritten == 0U);
    CHECK(ring.frames_total == 1000U);
    CHECK(ring.count == 256U);
    CHECK(ring.served_seq == 1000U);
    /* Nothing new after the client caught up -> 204 path (count 0). */
    CHECK(curve_ring_plan_read(&ring, ring.served_seq, 32U, &first, &gap) == 0U);
}

/* Client does not poll while the ring fills: only frames evicted before being
 * delivered count as overwritten (the "client may have missed" semantics). */
static void test_ring_slow_consumer(void)
{
    static curve_ring_t ring;
    uint32_t slot = 0U;
    uint32_t seq = 0U;
    uint32_t first = 0U;
    uint32_t count;
    bool gap = false;

    curve_ring_reset(&ring, 256U);
    for (uint32_t i = 0U; i < 300U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        curve_ring_commit_write(&ring);
    }
    /* 1..44 were evicted before any read: 44 real losses. */
    CHECK(ring.overwritten == 44U);
    CHECK(ring.frames_total == 300U);
    CHECK(ring.count == 256U);

    /* since=0 starts at the oldest retained frame (45). */
    count = curve_ring_plan_read(&ring, 0U, 64U, &first, &gap);
    CHECK(count == 64U);
    CHECK(first == 45U);
    CHECK(!gap);
    curve_ring_mark_served(&ring, first, count);
    CHECK(ring.served_seq == 108U);

    /* Drain the retained backlog in batches (as the Web client does). */
    for (;;) {
        count = curve_ring_plan_read(&ring, ring.served_seq, 64U, &first, &gap);
        if (count == 0U) {
            break;
        }
        CHECK(!gap);
        CHECK(first == ring.served_seq + 1U);
        curve_ring_mark_served(&ring, first, count);
    }
    CHECK(ring.served_seq == 300U);
    CHECK(ring.overwritten == 44U); /* reads do not create or clear losses */

    /* 100 more frames while nobody polls: the oldest retained frame is now
     * 45 and it was already served, so nothing new is counted yet. */
    for (uint32_t i = 0U; i < 100U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        curve_ring_commit_write(&ring);
    }
    CHECK(ring.overwritten == 44U);

    /* Client asks from a seq that has been overwritten: gap flag + oldest. */
    count = curve_ring_plan_read(&ring, 100U, 32U, &first, &gap);
    CHECK(count == 32U);
    CHECK(first > 100U);
    CHECK(gap);

    /* Keep writing 60 more frames without reading: those evicted seq > 300 are
     * genuinely missed now (served=300, ring full from seq 145..400). */
    for (uint32_t i = 0U; i < 60U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        curve_ring_commit_write(&ring);
    }
    /* Frame 301 was evicted when sequencer wrote 557 (501..556 writes not
     * reached); with 460 total writes nothing newer than served was evicted
     * yet, so overwritten is still 44. */
    CHECK(ring.overwritten == 44U);
}

/* Client consumed, then stops; frames evicted after the served cursor are
 * counted exactly once. */
static void test_ring_real_losses_after_served(void)
{
    static curve_ring_t ring;
    uint32_t slot = 0U;
    uint32_t seq = 0U;
    uint32_t first = 0U;
    uint32_t count;
    bool gap = false;

    curve_ring_reset(&ring, 256U);
    for (uint32_t i = 0U; i < 256U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        curve_ring_commit_write(&ring);
    }
    /* Drain everything (256 frames, first=1). */
    for (;;) {
        count = curve_ring_plan_read(&ring, ring.served_seq, 64U, &first, &gap);
        if (count == 0U) {
            break;
        }
        curve_ring_mark_served(&ring, first, count);
    }
    CHECK(ring.served_seq == 256U);
    CHECK(ring.overwritten == 0U);

    /* 300 more frames with no client read: evicted 1..256 are served (no
     * count), evicted 257..300 are real losses (44). */
    for (uint32_t i = 0U; i < 300U; i++) {
        curve_ring_begin_write(&ring, &slot, &seq);
        curve_ring_commit_write(&ring);
    }
    CHECK(ring.overwritten == 44U);
    CHECK(ring.served_seq == 256U);
    CHECK(ring.frames_total == 556U);

    /* A subsequent frames?since=256 returns the retained 301..556 batch from
     * the oldest retained frame with gap set. */
    count = curve_ring_plan_read(&ring, 256U, 64U, &first, &gap);
    CHECK(count == 64U);
    CHECK(first == 301U);
    CHECK(gap);
}

int main(void)
{
    test_addr_filter();
    test_parse_contract_example();
    test_parse_limits();
    test_parse_series_counts();
    test_parse_depth();
    test_eval();
    test_ring_capacity();
    test_ring_steady_consumer();
    test_ring_slow_consumer();
    test_ring_real_losses_after_served();

    if (g_failures == 0) {
        printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    printf("FAILED: %d/%d checks\n", g_failures, g_checks);
    return 1;
}
