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
    CHECK(cfg.groups[0].reg_count == 1U);
    CHECK(cfg.groups[0].regs[0] == 13U);
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

    /* 32 series accepted (Phase 2.5; 32 x size 8 = the 256 B payload cap) */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 32; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"id\":%d,\"size\":8,\"expr\":{\"kind\":\"addr\","
                                "\"address\":%u}}",
                                (i == 0) ? "" : ",", i, 0x20000000U + (uint32_t)i * 8U);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.series_count == 32U);
    CHECK(cfg.sample_bytes == 256U);
    CHECK(curve_ring_capacity_for(cfg.sample_bytes) == 30U);

    /* 33 series rejected */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 33; i++) {
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
    /* capacity = clamp(8 KiB / (15 + frame_bytes), 16, 512) */
    CHECK(curve_ring_capacity_for(16U) == 264U);  /* 8192 / 31 */
    CHECK(curve_ring_capacity_for(28U) == 190U);  /* 8192 / 43 */
    CHECK(curve_ring_capacity_for(64U) == 103U);  /* 8192 / 79 */
    CHECK(curve_ring_capacity_for(128U) == 57U);  /* 8192 / 143 */
    CHECK(curve_ring_capacity_for(256U) == 30U);  /* 8192 / 271 */
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


static void test_breakpoint_addr(void)
{
    /* The contract example address 134356037 (0x08021C45) is a Thumb address:
     * bit0 is ignored, halfword alignment applies to the masked address. */
    CHECK(curve_breakpoint_addr_ok(0x08000000U));
    CHECK(curve_breakpoint_addr_ok(0x08000001U));
    CHECK(curve_breakpoint_addr_ok(134356037U));
    CHECK(curve_breakpoint_addr_ok(0x08FFFFFEU));
    CHECK(curve_breakpoint_addr_ok(0x08FFFFFFU)); /* masks to 0x08FFFFFE */
    CHECK(!curve_breakpoint_addr_ok(0x07FFFFFEU));
    CHECK(!curve_breakpoint_addr_ok(0x09000000U));
    CHECK(!curve_breakpoint_addr_ok(0x20000000U));
    CHECK(!curve_breakpoint_addr_ok(0x00000000U));
    CHECK(curve_breakpoint_compare_value(0x08021C44U) == 0x08021C45U);
    CHECK(curve_breakpoint_compare_value(0x08021C45U) == 0x08021C45U);
    CHECK(strcmp(curve_trigger_name(0U), "periodic") == 0);
    CHECK(strcmp(curve_trigger_name(1U), "breakpoint") == 0);
    CHECK(strcmp(curve_trigger_name(9U), "periodic") == 0);
}

static void test_parse_trigger(void)
{
    static curve_config_t cfg;
    char err[96];
    char json[768];

    /* Default trigger is periodic; interval_us keeps the Phase 1 limits. */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.trigger == (uint8_t)CURVE_TRIGGER_PERIODIC);
    CHECK(cfg.breakpoint_address == 0U);
    CHECK(cfg.interval_us == 20000U);

    /* Explicit periodic with a 1 s period is out of the periodic range. */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"periodic\",\"interval_us\":1000000,"
                   "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
                   "\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* Breakpoint trigger with the contract example address; interval_us is
     * optional and defaults to 20 ms.  reg/cfa expressions are allowed
     * (halt=false is irrelevant: the FPB hit already halted the core). */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
                   "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"reg\","
                   "\"reg\":13,\"offset\":244}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.trigger == (uint8_t)CURVE_TRIGGER_BREAKPOINT);
    CHECK(cfg.breakpoint_address == 134356037U);
    CHECK(cfg.interval_us == CURVE_BP_INTERVAL_DEFAULT_US);
    CHECK(cfg.needs_regs == true);

    /* Same with halt=false: breakpoint mode ignores halt. */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"halt\":false,"
                   "\"breakpoint_address\":134356038,\"interval_us\":5000,"
                   "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"cfa\","
                   "\"offset\":0}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.interval_us == 5000U);
    CHECK(cfg.breakpoint_address == 134356038U);
    CHECK(cfg.needs_regs == true);

    /* Breakpoint throttle range: 5000..1000000. */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
                   "\"interval_us\":1000000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
                   "\"interval_us\":1000001,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
                   "\"interval_us\":4999,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* Missing / out-of-flash / misaligned-out breakpoint address -> exact
     * contract error string. */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(strcmp(err, "bad breakpoint address") == 0);
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":603979776,"
                   "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
                   "\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(strcmp(err, "bad breakpoint address") == 0);

    /* Unknown trigger string is rejected. */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"dwt\",\"interval_us\":20000,\"series\":[{\"size\":4,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
}

static void test_status_json(void)
{
    static curve_status_t st;
    char buf[1024];
    int len;

    memset(&st, 0, sizeof(st));
    st.running = true;
    st.trigger = (uint8_t)CURVE_TRIGGER_BREAKPOINT;
    st.group_count = 2U;
    st.current_group = 1U;
    st.breakpoint_address = 134356037U;
    st.hits = 12U;
    st.sampled = 11U;
    st.throttled = 0U;
    st.interval_us = 20000U;
    st.frames = 11U;
    st.overwritten = 1U;
    st.invalid = 2U;
    st.out_of_scope = 7U;
    st.errors = 0U;
    st.sample_bytes = 8U;
    st.series_count = 2U;
    st.halt = true;
    st.link_hz = 2000000U;
    st.consecutive_errors = 0U;
    st.ring_capacity = 512U;
    st.served_seq = 11U;
    st.last_tick_us = 800U;
    (void)snprintf(st.last_err, sizeof(st.last_err), "%s", "");
    (void)snprintf(st.message, sizeof(st.message), "%s", "breakpoint sampling");
    len = curve_status_format_json(&st, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(len < (int)sizeof(buf));
    CHECK(strstr(buf, "\"ok\":true") != NULL);
    CHECK(strstr(buf, "\"running\":true") != NULL);
    CHECK(strstr(buf, "\"trigger\":\"breakpoint\"") != NULL);
    CHECK(strstr(buf, "\"breakpoint_address\":134356037") != NULL);
    CHECK(strstr(buf, "\"hits\":12") != NULL);
    CHECK(strstr(buf, "\"sampled\":11") != NULL);
    CHECK(strstr(buf, "\"throttled\":0") != NULL);
    CHECK(strstr(buf, "\"interval_us\":20000") != NULL);
    CHECK(strstr(buf, "\"frames\":11") != NULL);
    CHECK(strstr(buf, "\"out_of_scope\":7") != NULL);
    CHECK(strstr(buf, "\"group_count\":2") != NULL);
    CHECK(strstr(buf, "\"current_group\":1") != NULL);
    CHECK(strstr(buf, "\"ring_frames\":512") != NULL);
    CHECK(strstr(buf, "\"served_seq\":11") != NULL);

    /* Worst-case field widths still fit the httpd JSON buffer. */
    memset(&st, 0, sizeof(st));
    st.trigger = (uint8_t)CURVE_TRIGGER_PERIODIC;
    st.interval_us = 4294967295U;
    st.frames = 4294967295U;
    st.overwritten = 4294967295U;
    st.invalid = 4294967295U;
    st.out_of_scope = 4294967295U;
    st.errors = 4294967295U;
    st.group_count = 8U;
    st.current_group = 7U;
    st.sample_bytes = 65535U;
    st.series_count = 255U;
    st.link_hz = 4294967295U;
    st.consecutive_errors = 4294967295U;
    st.ring_capacity = 4294967295U;
    st.served_seq = 4294967295U;
    st.last_tick_us = 4294967295U;
    (void)snprintf(st.last_err, sizeof(st.last_err), "%s", "ESP_ERR_INVALID_STATE");
    memset(st.message, 'x', sizeof(st.message) - 1U);
    st.message[sizeof(st.message) - 1U] = '\0';
    len = curve_status_format_json(&st, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(len < 768); /* httpd s_json size in curve_http.c */
    CHECK(strstr(buf, "\"trigger\":\"periodic\"") != NULL);
}


static void test_pc_scope_parse(void)
{
    static curve_config_t cfg;
    char err[96];
    char json[4096];

    /* Odd Thumb-style bounds (low=0x08021C45, high=0x08021F45) are normalized
     * by clearing bit0 so the runtime comparison is convention-independent. */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":134356037,\"pc_high\":134356805,"
                   "\"expr\":{\"kind\":\"reg\",\"reg\":13,\"offset\":244}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) != 0U);
    CHECK(cfg.series[0].pc_low == 0x08021C44U);
    CHECK(cfg.series[0].pc_high == 0x08021F44U);
    CHECK(cfg.pc_gate_count == 1U);
    CHECK(curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                            0x08021C44U));
    CHECK(curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                            0x08021C45U)); /* odd PC value (Thumb bit set) */
    CHECK(curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                            0x08021F42U));
    CHECK(!curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                             0x08021F44U)); /* high is exclusive */
    CHECK(!curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                             0x08021C42U));
    CHECK(!curve_pc_in_scope(cfg.series[0].pc_low, cfg.series[0].pc_high,
                             0x08030000U));

    /* Even DWARF-style bounds are stored unchanged. */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":134348800,\"pc_high\":134352896,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) != 0U);
    CHECK(cfg.series[0].pc_low == 0x08020000U);
    CHECK(cfg.series[0].pc_high == 0x08021000U);

    /* Invalid/partial/zero ranges leave the series ungated (optional field). */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":134352896,\"pc_high\":134348800,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) == 0U);
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":603979776,\"pc_high\":603980800,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) == 0U); /* not flash */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":134348800,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) == 0U); /* pc_high missing */
    (void)snprintf(json, sizeof(json),
                   "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
                   "\"pc_low\":0,\"pc_high\":0,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) == 0U);
    CHECK(cfg.pc_gate_count == 0U);

    /* Per-series gates are independent and counted; breakpoint mode supports
     * them as well (the core is halted by the FPB hit). */
    (void)snprintf(json, sizeof(json),
                   "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
                   "\"series\":["
                   "{\"size\":4,\"pc_low\":134348800,\"pc_high\":134352896,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870912}},"
                   "{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":536870916}},"
                   "{\"size\":8,\"pc_low\":134352896,\"pc_high\":134356992,"
                   "\"expr\":{\"kind\":\"addr\",\"address\":536870920}}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.pc_gate_count == 2U);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_PC_GATE) != 0U);
    CHECK((cfg.series[1].flags & CURVE_SERIES_FLAG_PC_GATE) == 0U);
    CHECK((cfg.series[2].flags & CURVE_SERIES_FLAG_PC_GATE) != 0U);
    CHECK(cfg.sample_bytes == 16U);
    CHECK(cfg.series_count == 3U);

    /* 32 series with PC gates parse into the fixed config/pool. */
    {
        char *p = json;
        size_t used = 0U;

        used += (size_t)snprintf(json, sizeof(json),
                                 "{\"interval_us\":20000,\"halt\":true,\"series\":[");
        for (int i = 0; i < 32; i++) {
            used += (size_t)snprintf(json + used, sizeof(json) - used,
                                     "%s{\"size\":2,\"pc_low\":%u,\"pc_high\":%u,"
                                     "\"expr\":{\"kind\":\"addr\",\"address\":%u}}",
                                     (i == 0) ? "" : ",",
                                     134348800U + (uint32_t)i * 16U,
                                     134348800U + (uint32_t)i * 16U + 16U,
                                     536870912U + (uint32_t)i * 2U);
        }
        (void)snprintf(json + used, sizeof(json) - used, "]}");
        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
        CHECK(cfg.series_count == 32U);
        CHECK(cfg.sample_bytes == 64U);
        CHECK(cfg.pc_gate_count == 32U);
        CHECK(cfg.pool_used == 32U);
        CHECK(curve_ring_capacity_for(cfg.sample_bytes) == 103U);
        (void)p;
    }
}


/* ---- protocol v2: groups / variants / CVF2 ------------------------------- */

static void test_groups_parse(void)
{
    static curve_config_t cfg;
    char err[96];
    static const char *json =
        "{\"interval_us\":20000,\"halt\":true,\"groups\":["
        "{\"id\":0,\"series\":["
        "{\"size\":4,\"expr\":{\"kind\":\"reg\",\"reg\":13,\"offset\":244}},"
        "{\"size\":2,\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]},"
        "{\"id\":1,\"series\":["
        "{\"size\":1,\"expr\":{\"kind\":\"addr\",\"address\":536870920}},"
        "{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":536870924}},"
        "{\"size\":8,\"expr\":{\"kind\":\"addr\",\"address\":536870928}}]}]}";

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 2U);
    CHECK(cfg.series_count == 5U);
    CHECK(cfg.sample_bytes == 13U); /* max(6, 13) */
    CHECK(cfg.groups[0].series_first == 0U);
    CHECK(cfg.groups[0].series_count == 2U);
    CHECK(cfg.groups[0].sample_bytes == 6U);
    CHECK(cfg.groups[0].pc_needed == false);
    CHECK(cfg.groups[0].reg_count == 1U);
    CHECK(cfg.groups[0].regs[0] == 13U);
    CHECK(cfg.groups[1].series_first == 2U);
    CHECK(cfg.groups[1].series_count == 3U);
    CHECK(cfg.groups[1].sample_bytes == 13U);
    CHECK(cfg.groups[1].reg_count == 0U);

    /* groups takes precedence over a legacy root series array. */
    {
        static const char *mixed =
            "{\"interval_us\":20000,\"halt\":true,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}],"
            "\"groups\":[{\"series\":[{\"size\":2,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}]}]}";

        CHECK(curve_config_parse(mixed, strlen(mixed), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.group_count == 1U);
        CHECK(cfg.series_count == 1U);
        CHECK(cfg.sample_bytes == 2U);
    }

    /* Legacy single-group request keeps working (group_count=1). */
    {
        static const char *legacy =
            "{\"interval_us\":20000,\"halt\":true,\"series\":["
            "{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":536870912}},"
            "{\"size\":4,\"pc_low\":134348800,\"pc_high\":134352896,"
            "\"expr\":{\"kind\":\"reg\",\"reg\":13,\"offset\":0}}]}";

        CHECK(curve_config_parse(legacy, strlen(legacy), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.group_count == 1U);
        CHECK(cfg.series_count == 2U);
        CHECK(cfg.groups[0].pc_needed == true);
        CHECK((cfg.series[1].flags & CURVE_SERIES_FLAG_PC_GATE) != 0U);
    }
}

static void build_addr_series(char *buf, size_t cap, size_t *pos, int index,
                              uint32_t size, uint32_t address)
{
    *pos += (size_t)snprintf(buf + *pos, cap - *pos,
                             "%s{\"size\":%u,\"expr\":{\"kind\":\"addr\","
                             "\"address\":%u}}",
                             (index == 0) ? "" : ",", (unsigned)size,
                             (unsigned)address);
}

static void test_groups_boundaries(void)
{
    static curve_config_t cfg;
    static char json[32768];
    char err[96];
    size_t pos;

    /* 9 groups rejected. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"groups\":[");
    for (int g = 0; g < 9; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"series\":[{\"size\":1,\"expr\":"
                                "{\"kind\":\"addr\",\"address\":536870912}}]}",
                                (g == 0) ? "" : ",");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* 33 series in one group rejected. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"groups\":["
                                              "{\"series\":[");
    for (int i = 0; i < 33; i++) {
        build_addr_series(json, sizeof(json), &pos, i, 1U,
                          536870912U + (uint32_t)i);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* Contract maximum: 8 groups x 32 series = 256 total series. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"groups\":[");
    for (int g = 0; g < 8; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "%s{\"series\":[",
                                (g == 0) ? "" : ",");
        for (int i = 0; i < 32; i++) {
            build_addr_series(json, sizeof(json), &pos, i, 1U,
                              536870912U + (uint32_t)(g * 32 + i));
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 8U);
    CHECK(cfg.series_count == 256U);
    CHECK(cfg.sample_bytes == 32U);
    CHECK(cfg.groups[7].series_first == 224U);
    CHECK(cfg.groups[7].series_count == 32U);
    CHECK(cfg.pool_used == 256U);

    /* One group with 32 x size 8 = the 256 B payload cap. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"series\":[");
    for (int i = 0; i < 32; i++) {
        build_addr_series(json, sizeof(json), &pos, i, 8U,
                          536870912U + (uint32_t)i * 8U);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.sample_bytes == 256U);
    CHECK(curve_ring_capacity_for(cfg.sample_bytes) == 30U);
}

static void test_variants_parse_select(void)
{
    static curve_config_t cfg;
    char err[96];
    static const char *json =
        "{\"interval_us\":20000,\"halt\":true,\"groups\":["
        "{\"series\":["
        "{\"size\":4,\"variants\":["
        "{\"pc_low\":134348800,\"pc_high\":134352896,"
        "\"expr\":{\"kind\":\"reg\",\"reg\":13,\"offset\":48}},"
        "{\"pc_low\":134352896,\"pc_high\":134356992,"
        "\"expr\":{\"kind\":\"reg\",\"reg\":7,\"offset\":-32}}]},"
        "{\"size\":2,\"expr\":{\"kind\":\"addr\",\"address\":536870912}},"
        "{\"size\":2,\"pc_low\":134348800,\"pc_high\":134352896,"
        "\"expr\":{\"kind\":\"cfa\",\"offset\":12}}]},"
        "{\"series\":[{\"size\":1,\"variants\":["
        "{\"pc_low\":134348800,\"pc_high\":134352896,"
        "\"expr\":{\"kind\":\"addr\",\"address\":536870916}}]}]}]}";

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 2U);
    CHECK(cfg.series_count == 4U);
    CHECK(cfg.variant_used == 3U);
    CHECK(cfg.groups[0].pc_needed == true);
    CHECK(cfg.groups[1].pc_needed == true);
    CHECK((cfg.series[0].flags & CURVE_SERIES_FLAG_VARIANTS) != 0U);
    CHECK(cfg.series[0].variant_count == 2U);
    CHECK(cfg.series[0].expr == CURVE_EXPR_NONE);

    /* Variant selection by PC (half-open range, Thumb bit ignored). */
    {
        const curve_series_t *variant_series = &cfg.series[0];
        uint16_t first_expr = cfg.variants[variant_series->variant_first].expr;
        uint16_t second_expr =
            cfg.variants[variant_series->variant_first + 1U].expr;

        CHECK(curve_series_expr_for_pc(&cfg, variant_series, 134348800U) ==
              first_expr);
        CHECK(curve_series_expr_for_pc(&cfg, variant_series, 134348801U) ==
              first_expr);
        CHECK(curve_series_expr_for_pc(&cfg, variant_series, 134352896U) ==
              second_expr);
        CHECK(curve_series_expr_for_pc(&cfg, variant_series, 134356992U) ==
              CURVE_EXPR_NONE); /* high exclusive / no variant matches */
        CHECK(cfg.pool[first_expr].kind == CURVE_EXPR_REG);
        CHECK(cfg.pool[first_expr].reg == 13U);
        CHECK(cfg.pool[second_expr].kind == CURVE_EXPR_REG);
        CHECK(cfg.pool[second_expr].reg == 7U);
        CHECK(curve_series_needs_pc(variant_series));
    }

    /* Plain series ignores the PC; single-style PC gate selects by range. */
    CHECK(curve_series_expr_for_pc(&cfg, &cfg.series[1], 0U) == cfg.series[1].expr);
    CHECK(!curve_series_needs_pc(&cfg.series[1]));
    CHECK(curve_series_needs_pc(&cfg.series[2]));
    CHECK(curve_series_expr_for_pc(&cfg, &cfg.series[2], 134348800U) ==
          cfg.series[2].expr);
    CHECK(curve_series_expr_for_pc(&cfg, &cfg.series[2], 134352896U) ==
          CURVE_EXPR_NONE);

    /* Variants are allowed on breakpoint trigger (core halted at the hit). */
    {
        static const char *bp =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"series\":[{\"size\":4,\"variants\":["
            "{\"pc_low\":134348800,\"pc_high\":134352896,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}]}";

        CHECK(curve_config_parse(bp, strlen(bp), &cfg, err, sizeof(err)) == ESP_OK);
        CHECK(cfg.variant_used == 1U);
    }

    /* Periodic no-halt + variants is rejected. */
    {
        static const char *nohalt =
            "{\"interval_us\":20000,\"halt\":false,\"series\":[{\"size\":4,"
            "\"variants\":[{\"pc_low\":134348800,\"pc_high\":134352896,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}]}";

        CHECK(curve_config_parse(nohalt, strlen(nohalt), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "variants require halt") != NULL);
    }

    /* 17 variants / bad range / empty variants / missing expr are rejected. */
    {
        char big[4096];
        size_t pos = (size_t)snprintf(big, sizeof(big),
                                      "{\"interval_us\":20000,\"halt\":true,"
                                      "\"series\":[{\"size\":1,\"expr\":"
                                      "{\"kind\":\"addr\",\"address\":536870912},"
                                      "\"variants\":[");
        for (int i = 0; i < 17; i++) {
            pos += (size_t)snprintf(big + pos, sizeof(big) - pos,
                                    "%s{\"pc_low\":%u,\"pc_high\":%u,\"expr\":"
                                    "{\"kind\":\"addr\",\"address\":536870912}}",
                                    (i == 0) ? "" : ",",
                                    134348800U + (uint32_t)i * 16U,
                                    134348816U + (uint32_t)i * 16U);
        }
        (void)snprintf(big + pos, sizeof(big) - pos, "]}]}");
        CHECK(curve_config_parse(big, strlen(big), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
    }
    {
        static const char *badrange =
            "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":1,"
            "\"variants\":[{\"pc_low\":603979776,\"pc_high\":603980800,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}]}";

        CHECK(curve_config_parse(badrange, strlen(badrange), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
    }
    {
        static const char *emptyvars =
            "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":1,"
            "\"variants\":[]}]}";

        CHECK(curve_config_parse(emptyvars, strlen(emptyvars), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
    }
    {
        static const char *noexpr =
            "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":1}]}";

        CHECK(curve_config_parse(noexpr, strlen(noexpr), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
    }
}

static void test_variant_pool_limit(void)
{
    static curve_config_t cfg;
    static char json[32768];
    char err[96];
    size_t pos = (size_t)snprintf(json, sizeof(json),
                                  "{\"interval_us\":20000,\"halt\":true,"
                                  "\"series\":[");
    /* 32 series x 9 variants = 288 > CURVE_MAX_VARIANTS (256). */
    for (int i = 0; i < 32; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"size\":1,\"expr\":{\"kind\":\"addr\","
                                "\"address\":536870912},\"variants\":[",
                                (i == 0) ? "" : ",");
        for (int v = 0; v < 9; v++) {
            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "%s{\"pc_low\":%u,\"pc_high\":%u,\"expr\":"
                                    "{\"kind\":\"addr\",\"address\":%u}}",
                                    (v == 0) ? "" : ",",
                                    134348800U + (uint32_t)(i * 256 + v * 16),
                                    134348816U + (uint32_t)(i * 256 + v * 16),
                                    536870912U + (uint32_t)v);
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "variants") != NULL);
}

static void test_expr_pool_limit(void)
{
    static curve_config_t cfg;
    static char json[49152];
    char err[96];
    size_t pos = (size_t)snprintf(json, sizeof(json),
                                  "{\"interval_us\":20000,\"halt\":true,"
                                  "\"groups\":[");
    /* 4 groups x 25 series x depth-8 expression = 800 nodes > 768. */
    for (int g = 0; g < 4; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"series\":[", (g == 0) ? "" : ",");
        for (int i = 0; i < 25; i++) {
            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "%s{\"size\":1,\"expr\":", (i == 0) ? "" : ",");
            for (int d = 0; d < 7; d++) {
                pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                        "{\"kind\":\"add\",\"offset\":1,\"inner\":");
            }
            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "{\"kind\":\"addr\",\"address\":536870912}");
            for (int d = 0; d < 7; d++) {
                pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "}");
            }
            pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "}");
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "expr") != NULL);
}

static void test_group_rotation(void)
{
    CHECK(curve_next_slot(0U, 1U) == 0U);
    CHECK(curve_next_slot(0U, 2U) == 1U);
    CHECK(curve_next_slot(1U, 2U) == 0U);
    CHECK(curve_next_slot(2U, 3U) == 0U);
    CHECK(curve_next_slot(0U, 8U) == 1U);
    CHECK(curve_next_slot(7U, 8U) == 0U);
    CHECK(curve_next_slot(63U, 64U) == 0U);
    CHECK(curve_next_slot(3U, 0U) == 0U);
}

/* 8 KiB ring budget: every contract payload size must fit the budget and keep a
 * useful history floor, so 'start' never needs the old ~34 KiB allocation. */
static void test_ring_budget_boundaries(void)
{
    for (uint32_t payload = 1U; payload <= CURVE_MAX_SAMPLE_BYTES; payload++) {
        uint32_t capacity = curve_ring_capacity_for(payload);
        uint32_t stride = CURVE_FRAME_META_BYTES + payload;

        CHECK(capacity >= CURVE_RING_FRAMES_MIN);
        CHECK(capacity <= CURVE_RING_FRAMES_MAX);
        CHECK((uint64_t)capacity * stride <= CURVE_RING_BYTES_BUDGET);
    }
    /* 32 x 8 B group payload: 30 frames x 271 B = 8130 B <= 8 KiB. */
    CHECK((uint64_t)curve_ring_capacity_for(256U) *
              (CURVE_FRAME_META_BYTES + 256U) <=
          CURVE_RING_BYTES_BUDGET);
    /* Monotonic: a larger payload never gets more frames. */
    for (uint32_t payload = 1U; payload < CURVE_MAX_SAMPLE_BYTES; payload++) {
        CHECK(curve_ring_capacity_for(payload) >=
              curve_ring_capacity_for(payload + 1U));
    }
}

/* P2 weighted round-robin: optional group weight 1..8 (default 1); the parser
 * expands weights into a schedule, so equal weights reproduce legacy rotation
 * and a heavier group is sampled more often per schedule cycle. */
static void test_group_weights(void)
{
    static curve_config_t cfg;
    char err[96];
    static const char *json =
        "{\"interval_us\":20000,\"halt\":true,\"groups\":["
        "{\"weight\":2,\"series\":[{\"size\":4,\"expr\":"
        "{\"kind\":\"addr\",\"address\":536870912}}]},"
        "{\"weight\":1,\"series\":[{\"size\":4,\"expr\":"
        "{\"kind\":\"addr\",\"address\":536870916}}]},"
        "{\"series\":[{\"size\":4,\"expr\":"
        "{\"kind\":\"addr\",\"address\":536870920}}]}]}";

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 3U);
    CHECK(cfg.groups[0].weight == 2U);
    CHECK(cfg.groups[1].weight == 1U);
    CHECK(cfg.groups[2].weight == 1U); /* default */
    CHECK(cfg.schedule_len == 4U);
    CHECK(cfg.schedule[0] == 0U);
    CHECK(cfg.schedule[1] == 0U);
    CHECK(cfg.schedule[2] == 1U);
    CHECK(cfg.schedule[3] == 2U);

    /* Weighted rotation: group 0 sampled twice per 4-slot cycle. */
    {
        uint8_t pos = 0U;
        uint8_t seen[CURVE_MAX_GROUPS] = {0U, 0U, 0U};
        uint8_t order[8];
        uint8_t n = 0U;

        for (uint8_t i = 0U; i < cfg.schedule_len; i++) {
            order[n++] = cfg.schedule[pos];
            seen[cfg.schedule[pos]]++;
            pos = curve_next_slot(pos, cfg.schedule_len);
        }
        CHECK(seen[0] == 2U);
        CHECK(seen[1] == 1U);
        CHECK(seen[2] == 1U);
        CHECK(order[0] == 0U && order[1] == 0U && order[2] == 1U &&
              order[3] == 2U);
        CHECK(pos == 0U);
    }

    /* Legacy single group -> schedule [0]. */
    {
        static const char *legacy =
            "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":4,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}";

        CHECK(curve_config_parse(legacy, strlen(legacy), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.schedule_len == 1U);
        CHECK(cfg.schedule[0] == 0U);
        CHECK(cfg.groups[0].weight == 1U);
    }

    /* Max weights: 8 groups x weight 8 = 64 slots (schedule array cap). */
    {
        static char big[2048];
        size_t pos = (size_t)snprintf(big, sizeof(big),
                                      "{\"interval_us\":20000,\"halt\":true,"
                                      "\"groups\":[");
        for (int g = 0; g < 8; g++) {
            pos += (size_t)snprintf(big + pos, sizeof(big) - pos,
                                    "%s{\"weight\":8,\"series\":[{\"size\":1,"
                                    "\"expr\":{\"kind\":\"addr\","
                                    "\"address\":536870912}}]}",
                                    (g == 0) ? "" : ",");
        }
        (void)snprintf(big + pos, sizeof(big) - pos, "]}");
        CHECK(curve_config_parse(big, strlen(big), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.schedule_len == 64U);
        CHECK(cfg.schedule[63] == 7U);
    }

    /* Invalid weights are rejected (0 and 9); non-number too. */
    {
        static const char *bad1 =
            "{\"interval_us\":20000,\"halt\":true,\"groups\":["
            "{\"weight\":0,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}]}]}";
        static const char *bad2 =
            "{\"interval_us\":20000,\"halt\":true,\"groups\":["
            "{\"weight\":9,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}]}]}";
        static const char *bad3 =
            "{\"interval_us\":20000,\"halt\":true,\"groups\":["
            "{\"weight\":\"fast\",\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}]}]}";

        CHECK(curve_config_parse(bad1, strlen(bad1), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(curve_config_parse(bad2, strlen(bad2), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(curve_config_parse(bad3, strlen(bad3), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
    }
}

static void test_cvf2_wire(void)
{
    uint8_t header[CURVE_FRAME_HEADER_BYTES];
    uint8_t frame[CURVE_FRAME_META_BYTES];

    CHECK(curve_cvf2_write_header(header, 3U, 0x11223344U, 256U, true) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[0] == 'C' && header[1] == 'V' && header[2] == 'F' &&
          header[3] == '2');
    CHECK(header[4] == 2U); /* CVF2 version */
    CHECK(header[5] == 0x01U); /* gap */
    CHECK(header[6] == 3U && header[7] == 0U); /* count u16 */
    CHECK(header[8] == 0x44U && header[9] == 0x33U && header[10] == 0x22U &&
          header[11] == 0x11U); /* first_seq u32 LE */
    CHECK(header[12] == 0x00U && header[13] == 0x01U); /* frame_bytes 256 */
    CHECK(header[14] == 0U && header[15] == 0U); /* reserved */

    CHECK(curve_cvf2_write_frame_meta(frame, 0x01020304U,
                                      0x1122334455667788ULL, 3U, 0x0102U) ==
          CURVE_FRAME_META_BYTES);
    CHECK(frame[0] == 0x04U && frame[1] == 0x03U && frame[2] == 0x02U &&
          frame[3] == 0x01U); /* seq */
    CHECK(frame[4] == 0x88U && frame[11] == 0x11U); /* ts u64 LE */
    CHECK(frame[12] == 3U); /* group */
    CHECK(frame[13] == 0x02U && frame[14] == 0x01U); /* payload_len u16 LE */

    /* No-gap flag variant. */
    CHECK(curve_cvf2_write_header(header, 1U, 1U, 32U, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == 0U);
}

static void test_config_static_budget(void)
{
    /* Static single instance owned by the HTTP layer; keep it well below a
     * typical 24 KiB .bss budget so 8 groups x 32 series + variants fit. */
    CHECK(sizeof(curve_config_t) > 12U * 1024U);
    CHECK(sizeof(curve_config_t) < 24U * 1024U);
    CHECK(sizeof(curve_series_t) <= 24U);
    CHECK(sizeof(curve_variant_t) <= 12U);
    CHECK(sizeof(curve_expr_t) <= 12U);
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
    test_breakpoint_addr();
    test_parse_trigger();
    test_status_json();
    test_pc_scope_parse();
    test_groups_parse();
    test_groups_boundaries();
    test_variants_parse_select();
    test_variant_pool_limit();
    test_expr_pool_limit();
    test_group_rotation();
    test_ring_budget_boundaries();
    test_group_weights();
    test_cvf2_wire();
    test_config_static_budget();

    if (g_failures == 0) {
        printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    printf("FAILED: %d/%d checks\n", g_failures, g_checks);
    return 1;
}
