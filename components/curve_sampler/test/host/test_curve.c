/* Host-side regression test for the curve sampler JSON parser + expression
 * evaluator (components/curve_sampler/src/curve_json.c / curve_expr.c).
 * Not part of the firmware build; compiled in /tmp against a cJSON copy and
 * stubbed esp_err.h / tf_swd_read_mem. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "curve_priv.h"
#include "curve_ws.h"

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

    /* 64 series accepted per group (64 x size 4 = the 256 B payload cap) */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 64; i++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"id\":%d,\"size\":4,\"expr\":{\"kind\":\"addr\","
                                "\"address\":%u}}",
                                (i == 0) ? "" : ",", i, 0x20000000U + (uint32_t)i * 4U);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.series_count == 64U);
    CHECK(cfg.sample_bytes == 256U);
    CHECK(curve_ring_capacity_for(cfg.sample_bytes, false) == 30U);

    /* 65 series in one group rejected */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,\"series\":[");
    for (int i = 0; i < 65; i++) {
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
    CHECK(curve_ring_capacity_for(16U, false) == 264U);  /* 8192 / 31 */
    CHECK(curve_ring_capacity_for(28U, false) == 190U);  /* 8192 / 43 */
    CHECK(curve_ring_capacity_for(64U, false) == 103U);  /* 8192 / 79 */
    CHECK(curve_ring_capacity_for(128U, false) == 57U);  /* 8192 / 143 */
    CHECK(curve_ring_capacity_for(256U, false) == 30U);  /* 8192 / 271 */
    CHECK(curve_ring_capacity_for(0U, false) == 0U);
    CHECK(curve_ring_capacity_for(CURVE_MAX_SAMPLE_BYTES + 1U, false) == 0U);

    /* PCI capture (CVF2 PC_PRESENT) adds 4 B/frame, so the same 8 KiB budget
     * yields fewer slots: stride = 19 + frame_bytes. */
    CHECK(curve_ring_capacity_for(16U, true) == 234U);  /* 8192 / 35 */
    CHECK(curve_ring_capacity_for(64U, true) == 98U);   /* 8192 / 83 */
    CHECK(curve_ring_capacity_for(128U, true) == 55U);  /* 8192 / 147 */
    CHECK(curve_ring_capacity_for(256U, true) == 29U);  /* 8192 / 275 */
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
    char buf[2048];
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
    st.pc_available = true;
    st.last_pc = 0x08021C44U;
    st.watchpoint_armed = true;
    st.watchpoint_address = 0x20000000U;
    st.watchpoint_hits = 4U;
    st.trigger_seq = 9U;
    st.watch_count = 2U;
    st.watchpoint2_armed = true;
    st.watchpoint2_address = 0x24000000U;
    st.watchpoint2_hits = 3U;
    st.watchpoint3_armed = true;
    st.watchpoint3_address = 0x24000010U;
    st.watchpoint3_hits = 2U;
    st.watchpoint4_armed = true;
    st.watchpoint4_address = 0x08001000U;
    st.watchpoint4_hits = 1U;
    st.last_watch_index = 4U;
    st.snapshot_state = (uint8_t)CURVE_SNAPSHOT_CAPTURING;
    st.snapshot_after = 8U;
    st.snapshot_trigger_seq = 10U;
    st.snapshot_pre_frames = 5U;
    st.snapshot_post_frames = 3U;
    st.trace_active = true;
    st.trace_bytes = 1234U;
    st.trace_dropped = 56U;
    st.backtrace_valid = true;
    st.backtrace_count = 5U;
    st.condition_armed = true;
    st.condition_met = true;
    st.condition_hits = 3U;
    st.condition_trigger_seq = 11U;
    st.errors = 0U;
    st.sample_bytes = 8U;
    st.series_count = 2U;
    st.halt = true;
    st.link_hz = 2000000U;
    st.sample_every_max = 8U;
    st.series_stale = 123U;
    st.heap_free = 54321U;
    st.heap_largest = 7777U;
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
    CHECK(strstr(buf, "\"pc_available\":true") != NULL);
    CHECK(strstr(buf, "\"last_pc\":134356036") != NULL);
    CHECK(strstr(buf, "\"watchpoint_armed\":true") != NULL);
    CHECK(strstr(buf, "\"watchpoint_address\":536870912") != NULL);
    CHECK(strstr(buf, "\"watchpoint_hits\":4") != NULL);
    CHECK(strstr(buf, "\"trigger_seq\":9") != NULL);
    CHECK(strstr(buf, "\"sample_every_max\":8") != NULL);
    CHECK(strstr(buf, "\"series_stale\":123") != NULL);
    CHECK(strstr(buf, "\"heap_free\":54321") != NULL);
    CHECK(strstr(buf, "\"heap_largest\":7777") != NULL);
    CHECK(strstr(buf, "\"watch_count\":2") != NULL);
    CHECK(strstr(buf, "\"watchpoint2_armed\":true") != NULL);
    CHECK(strstr(buf, "\"watchpoint2_address\":603979776") != NULL);
    CHECK(strstr(buf, "\"watchpoint2_hits\":3") != NULL);
    CHECK(strstr(buf, "\"watchpoint3_armed\":true") != NULL);
    CHECK(strstr(buf, "\"watchpoint3_address\":603979792") != NULL);
    CHECK(strstr(buf, "\"watchpoint3_hits\":2") != NULL);
    CHECK(strstr(buf, "\"watchpoint4_armed\":true") != NULL);
    CHECK(strstr(buf, "\"watchpoint4_address\":134221824") != NULL);
    CHECK(strstr(buf, "\"watchpoint4_hits\":1") != NULL);
    CHECK(strstr(buf, "\"last_watch_index\":4") != NULL);
    CHECK(strstr(buf, "\"snapshot_state\":\"capturing\"") != NULL);
    CHECK(strstr(buf, "\"snapshot_after\":8") != NULL);
    CHECK(strstr(buf, "\"snapshot_trigger_seq\":10") != NULL);
    CHECK(strstr(buf, "\"snapshot_pre_frames\":5") != NULL);
    CHECK(strstr(buf, "\"snapshot_post_frames\":3") != NULL);
    CHECK(strstr(buf, "\"trace_active\":true") != NULL);
    CHECK(strstr(buf, "\"trace_bytes\":1234") != NULL);
    CHECK(strstr(buf, "\"trace_dropped\":56") != NULL);
    CHECK(strstr(buf, "\"backtrace_valid\":true") != NULL);
    CHECK(strstr(buf, "\"backtrace_count\":5") != NULL);
    CHECK(strstr(buf, "\"condition_armed\":true") != NULL);
    CHECK(strstr(buf, "\"condition_met\":true") != NULL);
    CHECK(strstr(buf, "\"condition_hits\":3") != NULL);
    CHECK(strstr(buf, "\"condition_trigger_seq\":11") != NULL);
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
    st.last_pc = 4294967295U;
    st.watchpoint_address = 4294967295U;
    st.watchpoint_hits = 4294967295U;
    st.trigger_seq = 4294967295U;
    st.watch_count = 2U;
    st.watchpoint2_armed = true;
    st.watchpoint2_address = 4294967295U;
    st.watchpoint2_hits = 4294967295U;
    st.last_watch_index = 4U;
    st.watchpoint3_armed = true;
    st.watchpoint3_address = 4294967295U;
    st.watchpoint3_hits = 4294967295U;
    st.watchpoint4_armed = true;
    st.watchpoint4_address = 4294967295U;
    st.watchpoint4_hits = 4294967295U;
    st.snapshot_state = (uint8_t)CURVE_SNAPSHOT_CAPTURING;
    st.snapshot_after = 255U;
    st.snapshot_trigger_seq = 4294967295U;
    st.snapshot_pre_frames = 4294967295U;
    st.snapshot_post_frames = 4294967295U;
    st.trace_active = true;
    st.trace_bytes = 4294967295U;
    st.trace_dropped = 4294967295U;
    st.backtrace_valid = true;
    st.backtrace_count = 255U;
    st.condition_hits = 4294967295U;
    st.condition_trigger_seq = 4294967295U;
    st.sample_every_max = 255U;
    st.series_stale = 4294967295U;
    st.heap_free = 4294967295U;
    st.heap_largest = 4294967295U;
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
    st.max_groups = 8U;
    st.max_series = 256U;
    st.max_series_per_group = 64U;
    st.expr_pool_max = 2048U;
    st.expr_pool_used = 2047U;
    (void)snprintf(st.last_err, sizeof(st.last_err), "%s", "ESP_ERR_INVALID_STATE");
    memset(st.message, 'x', sizeof(st.message) - 1U);
    st.message[sizeof(st.message) - 1U] = '\0';
    len = curve_status_format_json(&st, buf, sizeof(buf));
    CHECK(len > 0);
    /* Must fit the httpd static JSON buffer (CURVE_STATUS_JSON_MAX). */
    CHECK(len < (int)CURVE_STATUS_JSON_MAX);
    CHECK(len < (int)sizeof(buf));
    CHECK(strstr(buf, "\"trigger\":\"periodic\"") != NULL);
    CHECK(strstr(buf, "\"max_groups\":8") != NULL);
    CHECK(strstr(buf, "\"max_series\":256") != NULL);
    CHECK(strstr(buf, "\"max_series_per_group\":64") != NULL);
    CHECK(strstr(buf, "\"expr_pool_max\":2048") != NULL);
    CHECK(strstr(buf, "\"expr_pool_used\":2047") != NULL);
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
        CHECK(curve_ring_capacity_for(cfg.sample_bytes, false) == 103U);
        (void)p;
    }
}


/* ---- protocol v2: groups / variants / CVF2 ------------------------------- */


static void test_watchpoint_params(void)
{
    /* Valid: in DTCM/AXI/flash, naturally aligned for the size. */
    CHECK(curve_watchpoint_params_ok(0x20000000U, 4U));
    CHECK(curve_watchpoint_params_ok(0x20000002U, 2U));
    CHECK(curve_watchpoint_params_ok(0x20000001U, 1U));
    CHECK(curve_watchpoint_params_ok(0x24000008U, 4U));
    CHECK(curve_watchpoint_params_ok(0x08000000U, 4U));
    /* Bad sizes / alignment / regions. */
    CHECK(!curve_watchpoint_params_ok(0x20000000U, 3U));
    CHECK(!curve_watchpoint_params_ok(0x20000000U, 8U));
    CHECK(!curve_watchpoint_params_ok(0x20000002U, 4U));
    CHECK(!curve_watchpoint_params_ok(0x20000003U, 2U));
    CHECK(!curve_watchpoint_params_ok(0x10000000U, 4U));
    CHECK(!curve_watchpoint_params_ok(0xE0001000U, 4U));
    CHECK(strcmp(curve_watch_access_name(0U), "write") == 0);
    CHECK(strcmp(curve_watch_access_name(1U), "readwrite") == 0);
    CHECK(strcmp(curve_watch_access_name(2U), "read") == 0);
    CHECK(strcmp(curve_watch_access_name(9U), "write") == 0); /* unknown -> write */
    /* P1.8.1 DWT helpers: MASK = log2(size), FUNCTION 5/6/7. */
    CHECK(curve_dwt_mask_for_size(1U) == 0U);
    CHECK(curve_dwt_mask_for_size(2U) == 1U);
    CHECK(curve_dwt_mask_for_size(4U) == 2U);
    CHECK(curve_dwt_mask_for_size(3U) == 0U); /* invalid: no mask */
    CHECK(curve_dwt_function_for_access((uint8_t)CURVE_WATCH_ACCESS_WRITE) ==
          CURVE_DWT_FUNC_WRITE);
    CHECK(curve_dwt_function_for_access((uint8_t)CURVE_WATCH_ACCESS_READ) ==
          CURVE_DWT_FUNC_READ);
    CHECK(curve_dwt_function_for_access((uint8_t)CURVE_WATCH_ACCESS_RW) ==
          CURVE_DWT_FUNC_ACCESS);
    CHECK(curve_dwt_function_for_access(9U) == CURVE_DWT_FUNC_WRITE);
}

static void test_dwt_comparator_alloc(void)
{
    uint32_t functions[CURVE_DWT_MAX_COMP] = {0U, 0U, 0U, 0U};
    uint8_t indices[CURVE_MAX_WATCHPOINTS] = {0U, 0U, 0U, 0U};

    /* All comparators free: primary request keeps comparator 0 (old behavior). */
    functions[0] = 0U;
    functions[1] = 0U;
    CHECK(curve_dwt_alloc_comparators(4U, functions, 1U, indices));
    CHECK(indices[0] == 0U);
    /* Second watchpoint gets the next free comparator. */
    CHECK(curve_dwt_alloc_comparators(4U, functions, 2U, indices));
    CHECK(indices[0] == 0U && indices[1] == 1U);
    /* Comparator 0 busy (e.g. another probe): use the first free one. */
    functions[0] = CURVE_DWT_FUNC_WRITE;
    CHECK(curve_dwt_alloc_comparators(4U, functions, 1U, indices));
    CHECK(indices[0] == 1U);
    CHECK(curve_dwt_alloc_comparators(4U, functions, 2U, indices));
    CHECK(indices[0] == 1U && indices[1] == 2U);
    /* Only one comparator implemented: single watch OK (when free), second
     * not supported. */
    functions[0] = 0U;
    CHECK(curve_dwt_alloc_comparators(1U, functions, 1U, indices));
    CHECK(!curve_dwt_alloc_comparators(1U, functions, 2U, indices));
    /* No comparator / too many requested / bad arguments. */
    CHECK(!curve_dwt_alloc_comparators(0U, functions, 1U, indices));
    CHECK(!curve_dwt_alloc_comparators(4U, functions, 5U, indices));
    /* P1-12: up to 4 comparators can be requested. */
    CHECK(curve_dwt_alloc_comparators(4U, functions, 3U, indices));
    CHECK(curve_dwt_alloc_comparators(4U, functions, 4U, indices));
    CHECK(indices[0] == 0U && indices[1] == 1U && indices[2] == 2U &&
          indices[3] == 3U);
    CHECK(!curve_dwt_alloc_comparators(3U, functions, 4U, indices));
    CHECK(!curve_dwt_alloc_comparators(4U, NULL, 1U, indices));
    CHECK(!curve_dwt_alloc_comparators(4U, functions, 1U, NULL));
    /* NUMCOMP is clamped to the architectural maximum of 4. */
    CHECK(curve_dwt_alloc_comparators(15U, functions, 2U, indices));

    /* MATCHED-bit classification: bit n = slot n hit, not comparator index. */
    {
        uint32_t fn[CURVE_DWT_MAX_COMP] = {0U, 0U, 0U, 0U};
        uint8_t idx[CURVE_MAX_WATCHPOINTS] = {2U, 0U, 1U, 3U};

        CHECK(curve_dwt_hit_mask(fn, idx, 2U) == 0U);
        fn[2] = CURVE_DWT_FUNCTION_MATCHED | CURVE_DWT_FUNC_WRITE;
        CHECK(curve_dwt_hit_mask(fn, idx, 2U) == 0x01U);
        fn[0] = CURVE_DWT_FUNCTION_MATCHED | CURVE_DWT_FUNC_READ;
        CHECK(curve_dwt_hit_mask(fn, idx, 2U) == 0x03U);
        /* 4 slots: bit n = watchpoint slot n (not comparator index). */
        fn[1] = CURVE_DWT_FUNCTION_MATCHED | CURVE_DWT_FUNC_ACCESS;
        fn[3] = CURVE_DWT_FUNCTION_MATCHED | CURVE_DWT_FUNC_WRITE;
        CHECK(curve_dwt_hit_mask(fn, idx, 4U) == 0x0FU);
        fn[3] = 0U;
        CHECK(curve_dwt_hit_mask(fn, idx, 4U) == 0x07U);
    }
}

static void test_snapshot_state_names(void)
{
    CHECK(strcmp(curve_snapshot_state_name(0U), "off") == 0);
    CHECK(strcmp(curve_snapshot_state_name((uint8_t)CURVE_SNAPSHOT_ARMED),
                 "armed") == 0);
    CHECK(strcmp(curve_snapshot_state_name((uint8_t)CURVE_SNAPSHOT_CAPTURING),
                 "capturing") == 0);
    CHECK(strcmp(curve_snapshot_state_name((uint8_t)CURVE_SNAPSHOT_COMPLETE),
                 "complete") == 0);
    CHECK(strcmp(curve_snapshot_state_name((uint8_t)CURVE_SNAPSHOT_ABORTED),
                 "aborted") == 0);
    CHECK(strcmp(curve_snapshot_state_name(99U), "off") == 0);
}

static void test_parse_watchpoint(void)
{
    static curve_config_t cfg;
    char err[96];

    /* Minimal watchpoint request: address required, size/access default. */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.trigger == (uint8_t)CURVE_TRIGGER_WATCHPOINT);
        CHECK(cfg.watch_address == 0x20000000U);
        CHECK(cfg.watch_size == 4U);
        CHECK(cfg.watch_access == (uint8_t)CURVE_WATCH_ACCESS_WRITE);
        CHECK(cfg.interval_us == CURVE_TRIGGER_WATCH_DEFAULT_US);
        CHECK(cfg.want_pc == true);
    }
    /* 2-byte readwrite with explicit throttle. */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870914,"
            "\"watch_size\":2,\"watch_access\":\"readwrite\","
            "\"interval_us\":10000,\"want_pc\":false,"
            "\"series\":[{\"size\":4,\"pc_low\":134348800,"
            "\"pc_high\":134352896,\"expr\":{\"kind\":\"reg\",\"reg\":13,"
            "\"offset\":0}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch_size == 2U);
        CHECK(cfg.watch_access == (uint8_t)CURVE_WATCH_ACCESS_RW);
        CHECK(cfg.interval_us == 10000U);
        CHECK(cfg.want_pc == false);
    }
    /* Missing / unaligned / out-of-region address -> bad watchpoint address. */
    {
        static const char *missing =
            "{\"trigger\":\"watchpoint\",\"series\":[{\"size\":4,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *misaligned =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870913,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *outside =
            "{\"trigger\":\"watchpoint\",\"watch_address\":268435456,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(missing, strlen(missing), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watchpoint address") != NULL);
        CHECK(curve_config_parse(misaligned, strlen(misaligned), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watchpoint address") != NULL);
        CHECK(curve_config_parse(outside, strlen(outside), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watchpoint address") != NULL);
    }
    /* Invalid size / access / throttle bounds. */
    {
        static const char *badsize =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch_size\":3,\"series\":[{\"size\":4,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *badaccess =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch_access\":\"execute\",\"series\":[{\"size\":4,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *badinterval =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"interval_us\":4999,\"series\":[{\"size\":4,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870916}}]}";

        CHECK(curve_config_parse(badsize, strlen(badsize), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch_size") != NULL);
        CHECK(curve_config_parse(badaccess, strlen(badaccess), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch_access") != NULL);
        CHECK(curve_config_parse(badinterval, strlen(badinterval), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "interval_us") != NULL);
    }
    /* groups/variants keep working in watchpoint mode. */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"groups\":[{\"series\":[{\"size\":4,\"variants\":["
            "{\"pc_low\":134348800,\"pc_high\":134352896,\"expr\":"
            "{\"kind\":\"reg\",\"reg\":13,\"offset\":0}}]}]}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.variant_used == 1U);
    }

    /* P1.8.1: watch_access="read" is now accepted (old firmware returned 400). */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch_access\":\"read\","
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch_access == (uint8_t)CURVE_WATCH_ACCESS_READ);
        CHECK(!cfg.watch2_enabled);
    }
    /* P1.8.1: second comparator object, independent address/size/access. */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch_size\":4,\"watch_access\":\"read\","
            "\"watch2\":{\"address\":603979776,\"size\":2,"
            "\"access\":\"readwrite\"},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch2_enabled);
        CHECK(cfg.watch2_address == 0x24000000U);
        CHECK(cfg.watch2_size == 2U);
        CHECK(cfg.watch2_access == (uint8_t)CURVE_WATCH_ACCESS_RW);
        /* Defaults inside watch2: size 4, access write. */
        json = "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
               "\"watch2\":{\"address\":536870928},"
               "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
               "\"address\":536870916}}]}";
        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch2_enabled);
        CHECK(cfg.watch2_size == 4U);
        CHECK(cfg.watch2_access == (uint8_t)CURVE_WATCH_ACCESS_WRITE);
    }
    /* watch2 error handling: not an object / missing/bad address / bad size /
     * bad access / exact duplicate / wrong trigger mode. */
    {
        static const char *bad_object =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":42,\"series\":[{\"size\":4,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *missing =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{},\"series\":[{\"size\":4,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *outside =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":268435456},\"series\":[{\"size\":4,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870916}}]}";
        static const char *misaligned =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870915,\"size\":4},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *badsize =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870916,\"size\":8},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *badaccess =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870916,\"access\":\"execute\"},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *duplicate =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870912,\"size\":4,"
            "\"access\":\"write\"},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *wrong_trigger =
            "{\"interval_us\":20000,\"watch2\":{\"address\":536870912},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(bad_object, strlen(bad_object), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch2 must be an object") != NULL);
        CHECK(curve_config_parse(missing, strlen(missing), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watch2 address") != NULL);
        CHECK(curve_config_parse(outside, strlen(outside), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watch2 address") != NULL);
        CHECK(curve_config_parse(misaligned, strlen(misaligned), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "bad watch2 address") != NULL);
        CHECK(curve_config_parse(badsize, strlen(badsize), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch2.size") != NULL);
        CHECK(curve_config_parse(badaccess, strlen(badaccess), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch2.access") != NULL);
        CHECK(curve_config_parse(duplicate, strlen(duplicate), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "duplicates") != NULL);
        CHECK(curve_config_parse(wrong_trigger, strlen(wrong_trigger), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch2 requires") != NULL);
    }
    /* Same address with a different access is allowed (read + write split). */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch_access\":\"read\","
            "\"watch2\":{\"address\":536870912,\"size\":4,"
            "\"access\":\"write\"},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch2_enabled);
    }
}

/* P1.8.2: function snapshot + backtrace request parsing. */
/* ---- P1-12: 4 DWT comparators: watchpoints array + watch3/watch4 aliases - */

static void test_watchpoints_array(void)
{
    static curve_config_t cfg;
    char err[128];
    static const char *base =
        "{\"trigger\":\"watchpoint\",\"interval_us\":10000,\"series\":"
        "[{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":536870916}}],"
        "\"watchpoints\":[";

    /* Canonical array form: 4 comparators with independent size/access. */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"interval_us\":10000,\"series\":"
            "[{\"size\":4,\"expr\":{\"kind\":\"addr\",\"address\":536870916}}],"
            "\"watchpoints\":["
            "{\"address\":536870912,\"size\":4,\"access\":\"write\"},"
            "{\"address\":536870928,\"size\":2,\"access\":\"read\"},"
            "{\"address\":603979776,\"size\":4,\"access\":\"readwrite\"},"
            "{\"address\":134221824,\"size\":1}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch_count == 4U);
        CHECK(cfg.watch_addr[0] == 0x20000000U);
        CHECK(cfg.watch_addr[1] == 0x20000010U);
        CHECK(cfg.watch_addr[2] == 0x24000000U);
        CHECK(cfg.watch_addr[3] == 0x08001000U);
        CHECK(cfg.watch_sz[1] == 2U && cfg.watch_sz[3] == 1U);
        CHECK(cfg.watch_acc[1] == (uint8_t)CURVE_WATCH_ACCESS_READ);
        CHECK(cfg.watch_acc[2] == (uint8_t)CURVE_WATCH_ACCESS_RW);
        /* legacy slot views stay in sync */
        CHECK(cfg.watch_address == cfg.watch_addr[0]);
        CHECK(cfg.watch2_enabled);
        CHECK(cfg.watch2_address == cfg.watch_addr[1]);
        CHECK(cfg.watch2_size == cfg.watch_sz[1]);
        CHECK(cfg.watch2_access == cfg.watch_acc[1]);
    }
    /* watch3/watch4 aliases without the array (contiguous slots). */
    {
        static const char *json =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870928,\"size\":2},"
            "\"watch3\":{\"address\":603979776},"
            "\"watch4\":{\"address\":134221824,\"access\":\"read\"},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.watch_count == 4U);
        CHECK(cfg.watch_addr[3] == 0x08001000U);
        CHECK(cfg.watch_acc[3] == (uint8_t)CURVE_WATCH_ACCESS_READ);
    }
    /* Duplicate address is rejected (one comparator cannot serve twice). */
    {
        static const char *dup = NULL;
        static char buf[512];
        snprintf(buf, sizeof(buf),
                 "%s{\"address\":536870912},{\"address\":536870912}]}", base);
        dup = buf;
        CHECK(curve_config_parse(dup, strlen(dup), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "duplicates") != NULL);
    }
    /* More than 4 / no slots / mixed encodings / wrong trigger. */
    {
        static const char *too_many =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch2\":{\"address\":536870928},"
            "\"watch3\":{\"address\":603979776},"
            "\"watch4\":{\"address\":134221824},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}],"
            "\"watchpoints\":[{\"address\":536870912},"
            "{\"address\":536870928},{\"address\":603979776},"
            "{\"address\":134221824}]}";
        static const char *skip_slot =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watch4\":{\"address\":134221824},"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *empty =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"watchpoints\":[],"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *periodic =
            "{\"interval_us\":10000,\"watchpoints\":[{\"address\":536870912}],"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(too_many, strlen(too_many), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "cannot be combined") != NULL);
        CHECK(curve_config_parse(skip_slot, strlen(skip_slot), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "previous watchpoint slot") != NULL);
        CHECK(curve_config_parse(empty, strlen(empty), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watchpoints count") != NULL);
        CHECK(curve_config_parse(periodic, strlen(periodic), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "require") != NULL);
    }
}

static void test_parse_snapshot(void)
{
    static curve_config_t cfg;
    char err[96];

    /* snapshot_after is opt-in and only valid with trigger=breakpoint. */
    {
        static const char *json =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"snapshot_after\":8,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.snapshot_enabled);
        CHECK(cfg.snapshot_after == 8U);
        CHECK(!cfg.backtrace);
    }
    /* Presence with value 0 = capture only the trigger frame (still one-shot). */
    {
        static const char *json =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"snapshot_after\":0,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.snapshot_enabled);
        CHECK(cfg.snapshot_after == 0U);
    }
    /* Absent -> continuous breakpoint behavior (legacy). */
    {
        static const char *json =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(!cfg.snapshot_enabled);
        CHECK(cfg.snapshot_after == 0U);
    }
    /* Wrong trigger mode / out-of-range / wrong type. */
    {
        static const char *wrong_trigger =
            "{\"interval_us\":20000,\"snapshot_after\":4,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *too_many =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"snapshot_after\":33,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *bad_type =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"snapshot_after\":\"many\","
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(wrong_trigger, strlen(wrong_trigger), &cfg,
                                 err, sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "snapshot_after requires") != NULL);
        CHECK(curve_config_parse(too_many, strlen(too_many), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "snapshot_after must be 0..") != NULL);
        CHECK(curve_config_parse(bad_type, strlen(bad_type), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "snapshot_after must be 0..") != NULL);
    }
    /* backtrace: bool, breakpoint/watchpoint only. */
    {
        static const char *json =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"backtrace\":true,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *wrong_trigger =
            "{\"interval_us\":20000,\"backtrace\":true,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";
        static const char *bad_type =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"backtrace\":1,"
            "\"series\":[{\"size\":4,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.backtrace);
        CHECK(curve_config_parse(wrong_trigger, strlen(wrong_trigger), &cfg,
                                 err, sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "backtrace requires") != NULL);
        CHECK(curve_config_parse(bad_type, strlen(bad_type), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "backtrace must be a boolean") != NULL);
    }
}

/* P1.8.2 prototype: frame-pointer unwinder with a mock target memory. */
struct bt_mem {
    uint32_t addr;
    uint32_t value;
};

static bool bt_mock_read(void *ctx, uint32_t addr, uint32_t *value)
{
    const struct bt_mem *mem = (const struct bt_mem *)ctx;

    for (int i = 0; i < 16 && mem[i].addr != 0U; i++) {
        if (mem[i].addr == addr) {
            *value = mem[i].value;
            return true;
        }
    }
    return false;
}

static void test_backtrace_unwind(void)
{
    static const struct bt_mem mem[] = {
        {0x20001010U, 0x20001030U}, /* [fp0] = previous fp            */
        {0x20001014U, 0x08020101U}, /* [fp0+4] = return address (Thumb)*/
        {0x20001030U, 0x20001050U}, /* [fp1] = previous fp            */
        {0x20001034U, 0x08030203U}, /* [fp1+4] = return address       */
        {0x20001050U, 0x00000000U}, /* chain end (null fp)            */
        {0U, 0U},
    };
    curve_backtrace_t bt;

    /* pc + two return addresses; [fp2] is unreadable -> stop with 3 frames. */
    CHECK(curve_backtrace_capture(0x08029A45U, 0x20001010U, bt_mock_read,
                                  (void *)mem, &bt));
    CHECK(bt.count == 3U);
    CHECK(bt.pcs[0] == 0x08029A44U);
    CHECK(bt.pcs[1] == 0x08020100U);
    CHECK(bt.pcs[2] == 0x08030202U);
    CHECK(!bt.truncated);

    /* Invalid start PC / unreadable fp: capture fails, count stays 0. */
    CHECK(!curve_backtrace_capture(0x20000000U, 0x20001010U, bt_mock_read,
                                   (void *)mem, &bt));
    CHECK(bt.count == 0U);
    CHECK(curve_backtrace_capture(0x08029A45U, 0x00000000U, bt_mock_read,
                                  (void *)mem, &bt));
    CHECK(bt.count == 1U); /* the halted PC itself is still valid */
    CHECK(!curve_backtrace_capture(0x08029A45U, 0x20001010U, NULL, NULL, &bt));

    /* A return address outside flash (corrupted chain) ends the unwalk. */
    {
        static const struct bt_mem bad[] = {
            {0x20002000U, 0x20002010U},
            {0x20002004U, 0x12345679U}, /* not a Thumb flash address */
            {0U, 0U},
        };

        CHECK(curve_backtrace_capture(0x08010001U, 0x20002000U, bt_mock_read,
                                      (void *)bad, &bt));
        CHECK(bt.count == 1U);
    }
    /* Backwards/self frame link (e.g. no frame pointers) ends the unwalk. */
    {
        static const struct bt_mem loop[] = {
            {0x20003000U, 0x20003000U}, /* prev fp == fp */
            {0x20003004U, 0x08010A03U},
            {0U, 0U},
        };

        CHECK(curve_backtrace_capture(0x08010001U, 0x20003000U, bt_mock_read,
                                      (void *)loop, &bt));
        CHECK(bt.count == 1U);
    }
    /* Frame limit: a long valid chain marks truncated. */
    {
        static struct bt_mem chain[20];
        uint32_t base = 0x20004000U;

        for (int i = 0; i < 10; i++) {
            chain[i * 2].addr = base + (uint32_t)i * 0x20U;
            chain[i * 2].value = base + (uint32_t)(i + 1) * 0x20U;
            chain[i * 2 + 1].addr = base + (uint32_t)i * 0x20U + 4U;
            chain[i * 2 + 1].value = 0x08020001U + (uint32_t)i * 2U;
        }
        CHECK(curve_backtrace_capture(0x08010001U, base, bt_mock_read,
                                      (void *)chain, &bt));
        CHECK(bt.count == CURVE_BACKTRACE_MAX_FRAMES);
        CHECK(bt.truncated);
    }
}

/* P1.8.3 prototype: target-UART trace ring planning. */
static void test_trace_ring(void)
{
    static uint8_t storage[8];
    curve_trace_ring_t ring;
    uint32_t first_seq = 0U;
    bool gap = false;
    curve_trace_seg_t seg[2];

    curve_trace_ring_reset(&ring, storage, sizeof(storage));
    CHECK(curve_trace_ring_plan(&ring, 0U, &first_seq, &gap, seg) == 0U);
    /* No storage: bytes are counted as evicted, plan stays empty. */
    {
        curve_trace_ring_t none;

        curve_trace_ring_reset(&none, NULL, 0U);
        curve_trace_ring_append(&none, (const uint8_t *)"abc", 3U);
        CHECK(none.total == 3U);
        CHECK(curve_trace_ring_plan(&none, 0U, &first_seq, &gap, seg) == 0U);
    }
    /* Linear fill: since=0 starts at the oldest retained byte, no gap. */
    curve_trace_ring_append(&ring, (const uint8_t *)"abcdef", 6U);
    CHECK(ring.total == 6U);
    CHECK(ring.used == 6U);
    CHECK(curve_trace_ring_plan(&ring, 0U, &first_seq, &gap, seg) == 6U);
    CHECK(first_seq == 0U && !gap);
    CHECK(seg[0].offset == 0U && seg[0].len == 6U);
    CHECK(seg[1].len == 0U);
    /* Incremental read: only bytes newer than `since`. */
    CHECK(curve_trace_ring_plan(&ring, 4U, &first_seq, &gap, seg) == 2U);
    CHECK(first_seq == 4U && !gap);
    CHECK(seg[0].offset == 4U && seg[0].len == 2U);
    /* Nothing new / future seq. */
    CHECK(curve_trace_ring_plan(&ring, 6U, &first_seq, &gap, seg) == 0U);
    CHECK(curve_trace_ring_plan(&ring, 7U, &first_seq, &gap, seg) == 0U);
    /* Wrap: appending "ghij" fills the ring, oldest bytes are evicted. */
    curve_trace_ring_append(&ring, (const uint8_t *)"ghij", 4U);
    CHECK(ring.total == 10U);
    CHECK(ring.used == 8U);
    CHECK(curve_trace_ring_plan(&ring, 2U, &first_seq, &gap, seg) == 8U);
    CHECK(!gap && first_seq == 2U); /* since == oldest retained: no gap */
    CHECK(seg[0].len + seg[1].len == 8U);
    /* since=0 always means "from the oldest retained byte". */
    CHECK(curve_trace_ring_plan(&ring, 0U, &first_seq, &gap, seg) == 8U);
    CHECK(!gap && first_seq == 2U);
    /* A chunk larger than the ring keeps only its newest part. */
    curve_trace_ring_append(&ring, (const uint8_t *)"0123456789", 10U);
    CHECK(ring.total == 20U);
    CHECK(ring.used == 8U);
    CHECK(curve_trace_ring_plan(&ring, 12U, &first_seq, &gap, seg) == 8U);
    CHECK(first_seq == 12U && !gap);
}

static void test_trace_line_ring(void)
{
    static curve_trace_line_t storage[4];
    static curve_trace_line_t out[4];
    curve_trace_line_ring_t ring;
    uint32_t first_seq = 0U;
    bool gap = true;

    /* P1-13 trigger match helper (NULL-safe, empty pattern never matches). */
    CHECK(curve_trace_line_match("hello world", "world"));
    CHECK(curve_trace_line_match("boot ok", "boot"));
    CHECK(!curve_trace_line_match("hello", "world"));
    CHECK(!curve_trace_line_match(NULL, "x"));
    CHECK(!curve_trace_line_match("x", NULL));
    CHECK(!curve_trace_line_match("x", ""));

    curve_trace_line_ring_reset(&ring, storage, 4U);
    /* Nothing completed yet. */
    CHECK(curve_trace_line_ring_plan(&ring, 0U, 4U, out, &first_seq, &gap) ==
          0U);
    CHECK(!gap);
    CHECK(first_seq == 0U);

    for (uint32_t i = 0U; i < 6U; i++) {
        char text[16];

        (void)snprintf(text, sizeof(text), "line%u", (unsigned)i);
        curve_trace_line_ring_append(&ring, 1000U + i, text, strlen(text),
                                     false);
    }
    CHECK(ring.total == 6U);
    CHECK(ring.count == 4U);
    /* since=0 on a ring that already evicted lines -> gap, oldest retained. */
    CHECK(curve_trace_line_ring_plan(&ring, 0U, 4U, out, &first_seq, &gap) ==
          4U);
    CHECK(!gap);            /* since=0 = initial request, no gap reported */
    CHECK(first_seq == 3U); /* lines 3,4,5,6 (seq 1-based) */
    CHECK(out[0].seq == 3U);
    CHECK(strcmp(out[0].text, "line2") == 0);
    CHECK(out[3].seq == 6U);
    CHECK(out[3].t_us == 1005U);
    /* since = next expected seq -> nothing new. */
    CHECK(curve_trace_line_ring_plan(&ring, 7U, 4U, out, &first_seq, &gap) ==
          0U);
    /* Only the newest 2 lines. */
    CHECK(curve_trace_line_ring_plan(&ring, 5U, 4U, out, &first_seq, &gap) ==
          2U);
    CHECK(!gap);
    CHECK(first_seq == 5U);
    CHECK(out[1].seq == 6U);
    /* max clamps the response size. */
    CHECK(curve_trace_line_ring_plan(&ring, 3U, 1U, out, &first_seq, &gap) ==
          1U);
    CHECK(first_seq == 3U);
    /* Long line is flagged truncated and clamped to the text buffer. */
    {
        char long_text[CURVE_TRACE_LINE_TEXT_MAX + 8U];

        memset(long_text, 'a', sizeof(long_text));
        curve_trace_line_ring_append(&ring, 2000U, long_text,
                                     (uint32_t)sizeof(long_text), true);
        CHECK(curve_trace_line_ring_plan(&ring, 6U, 4U, out, &first_seq,
                                         &gap) == 2U);
        CHECK(out[1].seq == 7U);
        CHECK(out[1].len == CURVE_TRACE_LINE_TEXT_MAX);
        CHECK((out[1].flags & CURVE_TRACE_LINE_FLAG_TRUNCATED) != 0U);
        CHECK(out[1].text[CURVE_TRACE_LINE_TEXT_MAX] == '\0');
    }
    /* Capacity 0 / NULL storage are safe no-ops. */
    curve_trace_line_ring_reset(&ring, NULL, 4U);
    curve_trace_line_ring_append(&ring, 1U, "x", 1U, false);
    CHECK(curve_trace_line_ring_plan(&ring, 0U, 4U, out, &first_seq, &gap) ==
          0U);
}

static void test_ring_group_trigger_marker(void)
{
    uint8_t encoded;

    encoded = curve_ring_group_encode(3U, false, false);
    CHECK(curve_ring_group_index(encoded) == 3U);
    CHECK(!curve_ring_group_triggered(encoded));

    encoded = curve_ring_group_encode(5U, true, false);
    CHECK(curve_ring_group_index(encoded) == 5U);
    CHECK(curve_ring_group_triggered(encoded));
    CHECK(encoded == (uint8_t)(5U | CURVE_RING_GROUP_TRIGGERED));

    /* Group index is preserved for all 0..7 groups with the marker set. */
    for (uint8_t g = 0U; g < 8U; g++) {
        encoded = curve_ring_group_encode(g, true, false);
        CHECK(curve_ring_group_index(encoded) == g);
        CHECK(curve_ring_group_triggered(encoded));
        CHECK(!curve_ring_group_condition(encoded));
    }

    /* Condition marker (bit6) is independent from the trigger marker. */
    encoded = curve_ring_group_encode(6U, false, true);
    CHECK(curve_ring_group_index(encoded) == 6U);
    CHECK(!curve_ring_group_triggered(encoded));
    CHECK(curve_ring_group_condition(encoded));
    CHECK(encoded == (uint8_t)(6U | CURVE_RING_GROUP_CONDITION));

    encoded = curve_ring_group_encode(2U, true, true);
    CHECK(curve_ring_group_index(encoded) == 2U);
    CHECK(curve_ring_group_triggered(encoded));
    CHECK(curve_ring_group_condition(encoded));
}

static void test_want_pc_parse(void)
{
    static curve_config_t cfg;
    char err[96];

    /* Default: PC capture allowed when the mode can read r15. */
    {
        static const char *json =
            "{\"interval_us\":20000,\"halt\":true,\"series\":[{\"size\":1,"
            "\"expr\":{\"kind\":\"addr\",\"address\":536870912}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.want_pc == true);
    }
    /* Explicit opt-out. */
    {
        static const char *json =
            "{\"interval_us\":20000,\"halt\":true,\"want_pc\":false,"
            "\"series\":[{\"size\":1,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.want_pc == false);
    }
    /* Non-boolean want_pc is rejected. */
    {
        static const char *json =
            "{\"interval_us\":20000,\"halt\":true,\"want_pc\":1,"
            "\"series\":[{\"size\":1,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "want_pc") != NULL);
    }
    /* Breakpoint mode also defaults to capturing PC. */
    {
        static const char *json =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356037,"
            "\"series\":[{\"size\":1,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.want_pc == true);
    }
}

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
    static char json[65536];
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

    /* 65 series in one group rejected. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"groups\":["
                                              "{\"series\":[");
    for (int i = 0; i < 65; i++) {
        build_addr_series(json, sizeof(json), &pos, i, 1U,
                          536870912U + (uint32_t)i);
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);

    /* Contract maximum: 4 groups x 64 series = 256 total series. */
    pos = (size_t)snprintf(json, sizeof(json), "{\"interval_us\":20000,"
                                              "\"halt\":true,\"groups\":[");
    for (int g = 0; g < 4; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "%s{\"series\":[",
                                (g == 0) ? "" : ",");
        for (int i = 0; i < 64; i++) {
            build_addr_series(json, sizeof(json), &pos, i, 1U,
                              536870912U + (uint32_t)(g * 64 + i));
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 4U);
    CHECK(cfg.series_count == 256U);
    CHECK(cfg.sample_bytes == 64U);
    CHECK(cfg.groups[3].series_first == 192U);
    CHECK(cfg.groups[3].series_count == 64U);
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
    CHECK(curve_ring_capacity_for(cfg.sample_bytes, false) == 30U);
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
    /* 5 groups x 64 series x depth-8 compact expression (8 nodes each) =
     * 2560 unique nodes > CURVE_EXPR_POOL_MAX (2048).  The base address is
     * unique per series so DAG dedup cannot collapse the chains. */
    for (int g = 0; g < 5; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"series\":[", (g == 0) ? "" : ",");
        for (int i = 0; i < 64; i++) {
            unsigned base = 0x20000000U + (unsigned)(g * 64 + i) * 4U;
            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "%s{\"expr_s\":\"A%08X+00000001+00000001"
                                    "+00000001+00000001+00000001+00000001"
                                    "+00000001\",\"size\":1}",
                                    (i == 0) ? "" : ",", base);
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "expr") != NULL);
}

static void test_expr_dag_dedup(void)
{
    static curve_config_t cfg;
    static char json[24576];
    char err[128];
    size_t pos;

    /* Object watch: 256 fields sharing one root pointer chain, split into
     * groups of 64 members (Web/object-watch layout).  Without DAG dedup this
     * needs 1 + 256*2 nodes; dedup keeps the shared prefix. */
    pos = (size_t)snprintf(json, sizeof(json),
                           "{\"interval_us\":20000,\"halt\":true,\"groups\":[");
    for (int g = 0; g < 4; g++) {
        int count = 64;
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"series\":[", (g == 0) ? "" : ",");
        for (int i = 0; i < count; i++) {
            unsigned field_off = (unsigned)(g * 64 + i) * 4U;

            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "%s{\"expr_s\":\"D%08XD00000000A20000000\","
                                    "\"size\":4}", (i == 0) ? "" : ",",
                                    field_off);
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.series_count == 256U);
    /* A(addr) + D(offset 0) shared once, one D per field. */
    CHECK(cfg.pool_used <= 2U + 256U);
    CHECK(cfg.pool_used >= 256U);

    /* Identical sub-trees fully collapse: 64 identical reg expressions. */
    {
        static const char *dup_json =
            "{\"interval_us\":20000,\"halt\":true,\"series\":["
            "{\"expr_s\":\"D00000000A20000000\",\"size\":4},"
            "{\"expr_s\":\"D00000000A20000000\",\"size\":4},"
            "{\"expr_s\":\"D00000000A20000000\",\"size\":4}]}";

        CHECK(curve_config_parse(dup_json, strlen(dup_json), &cfg, err,
                                 sizeof(err)) == ESP_OK);
        CHECK(cfg.pool_used == 2U); /* reg node + deref node, shared */
    }
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
        uint32_t capacity = curve_ring_capacity_for(payload, false);
        uint32_t stride = CURVE_FRAME_META_BYTES + payload;

        CHECK(capacity >= CURVE_RING_FRAMES_MIN);
        CHECK(capacity <= CURVE_RING_FRAMES_MAX);
        CHECK((uint64_t)capacity * stride <= CURVE_RING_BYTES_BUDGET);
    }
    /* Same budget with per-frame PC capture (stride 19 + payload). */
    for (uint32_t payload = 1U; payload <= CURVE_MAX_SAMPLE_BYTES; payload++) {
        uint32_t capacity = curve_ring_capacity_for(payload, true);
        uint32_t stride = CURVE_FRAME_META_BYTES_PC + payload;

        CHECK(capacity >= CURVE_RING_FRAMES_MIN);
        CHECK((uint64_t)capacity * stride <= CURVE_RING_BYTES_BUDGET);
    }

    /* 32 x 8 B group payload: 30 frames x 271 B = 8130 B <= 8 KiB. */
    CHECK((uint64_t)curve_ring_capacity_for(256U, false) *
              (CURVE_FRAME_META_BYTES + 256U) <=
          CURVE_RING_BYTES_BUDGET);
    /* Monotonic: a larger payload never gets more frames. */
    for (uint32_t payload = 1U; payload < CURVE_MAX_SAMPLE_BYTES; payload++) {
        CHECK(curve_ring_capacity_for(payload, false) >=
              curve_ring_capacity_for(payload + 1U, false));
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
    uint8_t frame[CURVE_FRAME_META_BYTES_PC];

    /* PC_PRESENT clear: 15 B metadata, flags bit1 = 0. */
    CHECK(curve_cvf2_write_header(header, 3U, 0x11223344U, 256U, true, false, false, false) ==
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
                                      0x1122334455667788ULL, 3U, 0x0102U,
                                      0x08021C44U, false) ==
          CURVE_FRAME_META_BYTES);
    CHECK(frame[0] == 0x04U && frame[1] == 0x03U && frame[2] == 0x02U &&
          frame[3] == 0x01U); /* seq */
    CHECK(frame[4] == 0x88U && frame[11] == 0x11U); /* ts u64 LE */
    CHECK(frame[12] == 3U); /* group */
    CHECK(frame[13] == 0x02U && frame[14] == 0x01U); /* payload_len u16 LE */

    /* No-gap flag variant. */
    CHECK(curve_cvf2_write_header(header, 1U, 1U, 32U, false, false, false, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == 0U);

    /* PC_PRESENT: flags bit1 set, 19 B metadata with pc u32 after payload_len. */
    CHECK(curve_cvf2_write_header(header, 4U, 0x0A0B0C0DU, 64U, false, true, false, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == CURVE_CVF2_FLAG_PC_PRESENT);
    CHECK(header[12] == 64U && header[13] == 0U);

    CHECK(curve_cvf2_write_frame_meta(frame, 0x01020304U,
                                      0x1122334455667788ULL, 2U, 0x0102U,
                                      0x08021C44U, true) ==
          CURVE_FRAME_META_BYTES_PC);
    CHECK(frame[0] == 0x04U && frame[3] == 0x01U); /* seq */
    CHECK(frame[4] == 0x88U && frame[11] == 0x11U); /* ts u64 LE */
    CHECK(frame[12] == 2U); /* group */
    CHECK(frame[13] == 0x02U && frame[14] == 0x01U); /* payload_len u16 LE */
    CHECK(frame[15] == 0x44U && frame[16] == 0x1CU && frame[17] == 0x02U &&
          frame[18] == 0x08U); /* pc 0x08021C44 LE */

    /* gap + PC together: flags = 0x03. */
    CHECK(curve_cvf2_write_header(header, 1U, 1U, 1U, true, true, false, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == (CURVE_CVF2_FLAG_GAP | CURVE_CVF2_FLAG_PC_PRESENT));

    /* P1.8 trigger bit (bit2, 0x04) combines with gap/PC: flags = 0x07. */
    CHECK(curve_cvf2_write_header(header, 2U, 7U, 128U, true, true, true, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == (CURVE_CVF2_FLAG_GAP | CURVE_CVF2_FLAG_PC_PRESENT |
                        CURVE_CVF2_FLAG_TRIGGERED));
    CHECK(curve_cvf2_write_header(header, 1U, 1U, 1U, false, false, true, false) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == CURVE_CVF2_FLAG_TRIGGERED);

    /* P1.8 condition bit3 (0x08) combines with the other flags: 0x0F. */
    CHECK(curve_cvf2_write_header(header, 2U, 3U, 64U, true, true, true, true) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == (CURVE_CVF2_FLAG_GAP | CURVE_CVF2_FLAG_PC_PRESENT |
                        CURVE_CVF2_FLAG_TRIGGERED | CURVE_CVF2_FLAG_CONDITION));
    CHECK(curve_cvf2_write_header(header, 1U, 1U, 1U, false, false, false, true) ==
          CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == CURVE_CVF2_FLAG_CONDITION);
}

/* ---- P0-3: per-series sample_every (exact refresh rate) ------------------ */

static void test_sample_due(void)
{
    /* divider 1 = every tick */
    CHECK(curve_sample_due(1U, 0U));
    CHECK(curve_sample_due(1U, 7U));
    /* divider 4 = ticks 0,4,8,... */
    CHECK(curve_sample_due(4U, 0U));
    CHECK(!curve_sample_due(4U, 1U));
    CHECK(!curve_sample_due(4U, 2U));
    CHECK(!curve_sample_due(4U, 3U));
    CHECK(curve_sample_due(4U, 4U));
    CHECK(curve_sample_due(4U, 8U));
    /* divider 64 (max) */
    CHECK(curve_sample_due(64U, 0U));
    CHECK(!curve_sample_due(64U, 63U));
    CHECK(curve_sample_due(64U, 64U));
    /* divider 3 */
    CHECK(curve_sample_due(3U, 6U));
    CHECK(!curve_sample_due(3U, 5U));
}

static void test_parse_sample_every(void)
{
    static curve_config_t cfg;
    char err[96];
    const char *json =
        "{\"interval_us\":10000,\"groups\":[{\"series\":["
        "{\"size\":4,\"sample_every\":7,\"expr_s\":\"A20000000\"},"
        "{\"size\":2,\"divisor\":3,\"expr_s\":\"A20000004\"},"
        "{\"size\":1,\"expr_s\":\"A20000008\"}]}]}";

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.series_count == 3U);
    CHECK(cfg.series[0].sample_every == 7U);
    CHECK(cfg.series[1].sample_every == 3U); /* divisor alias */
    CHECK(cfg.series[2].sample_every == 1U); /* default */

    /* Out-of-range divider -> clear 400-class parse error. */
    const char *bad_zero =
        "{\"interval_us\":10000,\"series\":["
        "{\"size\":4,\"sample_every\":0,\"expr_s\":\"A20000000\"}]}";
    CHECK(curve_config_parse(bad_zero, strlen(bad_zero), &cfg, err,
                             sizeof(err)) == ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "sample_every") != NULL);
    const char *bad_big =
        "{\"interval_us\":10000,\"series\":["
        "{\"size\":4,\"sample_every\":65,\"expr_s\":\"A20000000\"}]}";
    CHECK(curve_config_parse(bad_big, strlen(bad_big), &cfg, err,
                             sizeof(err)) == ESP_ERR_INVALID_ARG);
    CHECK(strstr(err, "sample_every") != NULL);
    /* sizeof(curve_series_t) must stay 16 (reserved byte reused). */
    CHECK(sizeof(curve_series_t) == 16U);
}

/* ---- P0-3: CVF2 per-series valid/stale footer ---------------------------- */

static void test_cvf2_series_mask(void)
{
    uint8_t header[CURVE_FRAME_HEADER_BYTES];
    uint8_t mask[CURVE_CVF2_SERIES_MASK_BYTES];

    CHECK(curve_cvf2_write_header_ex(header, 2U, 5U, 16U,
                                     CURVE_CVF2_FLAG_SERIES_MASK, false, true,
                                     false, false) == CURVE_FRAME_HEADER_BYTES);
    CHECK(header[5] == (CURVE_CVF2_FLAG_SERIES_MASK |
                        CURVE_CVF2_FLAG_PC_PRESENT));
    CHECK(header[0] == 'C' && header[3] == '2');
    CHECK(header[4] == 2U); /* still CVF2 v2: no version bump */
    CHECK(header[12] == 16U && header[13] == 0U);

    /* Footer mask is a plain little-endian u64. */
    CHECK(curve_cvf2_write_series_mask(mask, 0x1122334455667788ULL) ==
          CURVE_CVF2_SERIES_MASK_BYTES);
    CHECK(mask[0] == 0x88U && mask[1] == 0x77U && mask[2] == 0x66U &&
          mask[3] == 0x55U);
    CHECK(mask[4] == 0x44U && mask[5] == 0x33U && mask[6] == 0x22U &&
          mask[7] == 0x11U);
    /* Bit semantics: bit i = group series slot i is fresh this tick. */
    uint64_t valid = 0U;
    valid |= (uint64_t)1U << 0U;  /* series 0 sampled */
    valid |= (uint64_t)1U << 5U;  /* series 5 sampled */
    CHECK((valid & 1U) != 0U);
    CHECK((valid & (1U << 1U)) == 0U); /* series 1 stale */
    /* Old decoder compatibility: flags bit4 clear means no footer at all. */
    CHECK(curve_cvf2_write_header(header, 0U, 1U, 8U, false, false, false,
                                  false) == CURVE_FRAME_HEADER_BYTES);
    CHECK((header[5] & CURVE_CVF2_FLAG_SERIES_MASK) == 0U);
}

/* ---- P0-1: compact binary (CBS1) start request --------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} binw_t;

static void binw_bytes(binw_t *w, const void *data, size_t len)
{
    if (w->len + len <= w->cap) {
        memcpy(w->buf + w->len, data, len);
    }
    w->len += len;
}

static void binw_u8(binw_t *w, uint8_t v)
{
    binw_bytes(w, &v, 1U);
}

static void binw_u16(binw_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8U) & 0xFFU)};
    binw_bytes(w, b, sizeof(b));
}

static void binw_u32(binw_t *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8U) & 0xFFU),
                    (uint8_t)((v >> 16U) & 0xFFU),
                    (uint8_t)((v >> 24U) & 0xFFU)};
    binw_bytes(w, b, sizeof(b));
}

static void binw_expr(binw_t *w, const char *text)
{
    size_t len = strlen(text);

    binw_u16(w, (uint16_t)len);
    binw_bytes(w, text, len);
}

/** Build a 4-group x 64-series CBS1 body (256 series, 4 B each). */
static size_t build_cbs1_256(uint8_t *buf, size_t cap)
{
    binw_t w = {.buf = buf, .len = 0U, .cap = cap};

    binw_bytes(&w, "CBS1", 4U);
    binw_u8(&w, CURVE_BIN_VERSION);
    binw_u8(&w, CURVE_BIN_FLAG_HALT | CURVE_BIN_FLAG_WANT_PC);
    binw_u8(&w, (uint8_t)CURVE_TRIGGER_PERIODIC);
    binw_u8(&w, 0U); /* watch_count */
    binw_u32(&w, 20000U);
    binw_u32(&w, 0U); /* breakpoint_address */
    binw_u32(&w, 0U); /* cond_address */
    binw_u32(&w, 0U); /* cond_value */
    binw_u8(&w, 0U);  /* cond_size */
    binw_u8(&w, 0U);  /* cond_op */
    binw_u8(&w, 0U);  /* cond_mode */
    binw_u8(&w, 0U);  /* cond_flags */
    binw_u8(&w, 0U);  /* snapshot_after */
    binw_u8(&w, 4U);  /* group_count */
    binw_u16(&w, 0U); /* reserved */
    for (int g = 0; g < 4; g++) {
        binw_u8(&w, 1U);  /* weight */
        binw_u8(&w, 64U); /* series_count */
        binw_u16(&w, 0U); /* reserved */
        for (int i = 0; i < 64; i++) {
            unsigned index = (unsigned)(g * 64 + i);
            char expr[12];
            uint8_t every = ((index % 2U) == 0U) ? 1U : 2U;

            binw_u8(&w, 4U);          /* size */
            binw_u8(&w, 0U);          /* flags */
            binw_u8(&w, every);       /* sample_every */
            binw_u8(&w, 0U);          /* variant_count */
            binw_u32(&w, 0U);         /* pc_low */
            binw_u32(&w, 0U);         /* pc_high */
            (void)snprintf(expr, sizeof(expr), "A%08X",
                           0x20000000U + index * 4U);
            binw_expr(&w, expr);
        }
    }
    return w.len;
}

static void test_bin_parse_256(void)
{
    static uint8_t body[CURVE_BIN_BODY_MAX];
    static curve_config_t cfg;
    char err[128];
    size_t len = build_cbs1_256(body, sizeof(body));

    CHECK(len > 0U);
    CHECK(len < (size_t)CURVE_BIN_BODY_MAX);
    CHECK(curve_bin_parse_mem(body, len, &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 4U);
    CHECK(cfg.series_count == 256U);
    CHECK(cfg.sample_bytes == 256U);
    CHECK(cfg.pool_used == 256U); /* one unique address node per series */
    CHECK(cfg.series[0].sample_every == 1U);
    CHECK(cfg.series[1].sample_every == 2U);
    CHECK(cfg.series[255].sample_every == 2U);
    CHECK(cfg.interval_us == 20000U);
    CHECK(cfg.halt);
    CHECK(cfg.want_pc);
    CHECK(cfg.needs_regs == false);
    CHECK(cfg.groups[1].series_first == 64U);
    CHECK(cfg.groups[1].series_count == 64U);
    CHECK(cfg.schedule_len == 4U);
}

static void test_bin_parse_errors(void)
{
    static uint8_t body[CURVE_BIN_BODY_MAX];
    static curve_config_t cfg;
    char err[128];
    size_t len = build_cbs1_256(body, sizeof(body));

    /* Bad magic. */
    {
        uint8_t saved = body[0];
        body[0] = 'X';
        CHECK(curve_bin_parse_mem(body, len, &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        body[0] = saved;
    }
    /* Truncated body: every short length must fail, not over-read. */
    CHECK(curve_bin_parse_mem(body, len - 1U, &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    CHECK(curve_bin_parse_mem(body, 10U, &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
    /* Unsupported version. */
    CHECK(curve_bin_parse_mem(body, len, &cfg, err, sizeof(err)) == ESP_OK);
    {
        uint8_t *version = body + 4U;
        uint8_t saved = *version;
        *version = 2U;
        CHECK(curve_bin_parse_mem(body, len, &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        *version = saved;
    }
    /* group_count = 0. */
    {
        /* group_count is the byte right before the trailing reserved u16. */
        uint8_t *group_count = body + len - (4U * (4U + 64U * 23U)) - 3U;
        uint8_t saved;

        /* Rebuild to locate it robustly instead of relying on layout math. */
        saved = *group_count;
        *group_count = 0U;
        CHECK(curve_bin_parse_mem(body, len, &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        *group_count = saved;
    }
    /* Empty body. */
    CHECK(curve_bin_parse_mem(body, 0U, &cfg, err, sizeof(err)) ==
          ESP_ERR_INVALID_ARG);
}

static void test_bin_parse_watchpoints(void)
{
    static curve_config_t cfg;
    static uint8_t body[256];
    binw_t w = {.buf = body, .len = 0U, .cap = sizeof(body)};
    char err[96];

    /* watchpoint trigger with one watchpoint: COMP/MASK params encoded. */
    binw_bytes(&w, "CBS1", 4U);
    binw_u8(&w, CURVE_BIN_VERSION);
    binw_u8(&w, CURVE_BIN_FLAG_HALT); /* not WANT_PC */
    binw_u8(&w, (uint8_t)CURVE_TRIGGER_WATCHPOINT);
    binw_u8(&w, 1U); /* watch_count */
    binw_u32(&w, 5000U);
    binw_u32(&w, 0U);
    binw_u32(&w, 0U);
    binw_u32(&w, 0U);
    binw_u8(&w, 0U);
    binw_u8(&w, 0U);
    binw_u8(&w, 0U);
    binw_u8(&w, 0U);
    binw_u8(&w, 0U);
    binw_u8(&w, 1U);  /* group_count */
    binw_u16(&w, 0U);
    /* one watchpoint entry */
    binw_u32(&w, 0x20000000U);
    binw_u8(&w, 4U);
    binw_u8(&w, (uint8_t)CURVE_WATCH_ACCESS_WRITE);
    /* one group / one series */
    binw_u8(&w, 1U);
    binw_u8(&w, 1U);
    binw_u16(&w, 0U);
    binw_u8(&w, 4U);
    binw_u8(&w, 0U);
    binw_u8(&w, 1U);
    binw_u8(&w, 0U);
    binw_u32(&w, 0U);
    binw_u32(&w, 0U);
    binw_expr(&w, "A20000000");

    CHECK(curve_bin_parse_mem(body, w.len, &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.trigger == (uint8_t)CURVE_TRIGGER_WATCHPOINT);
    CHECK(cfg.watch_address == 0x20000000U);
    CHECK(cfg.watch_size == 4U);
    CHECK(cfg.want_pc == false);
    CHECK(cfg.group_count == 1U);

    /* A watchpoint on a non-watchpoint trigger must be rejected. */
    {
        uint8_t *trigger = body + 6U;

        *trigger = (uint8_t)CURVE_TRIGGER_PERIODIC;
        CHECK(curve_bin_parse_mem(body, w.len, &cfg, err, sizeof(err)) ==
              ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "watch") != NULL);
    }
}

static void test_config_static_budget(void)
{
    /* Static single instance owned by the HTTP layer (8 groups, 256 series,
     * 768 expr nodes, 256 variants) + 16 KiB static request body. */
    CHECK(sizeof(curve_config_t) > 12U * 1024U);
    CHECK(sizeof(curve_config_t) < 24U * 1024U);
    CHECK(sizeof(curve_series_t) <= 16U);
    CHECK(sizeof(curve_variant_t) <= 12U);
    CHECK(sizeof(curve_expr_t) <= 12U);
    CHECK(CURVE_JSON_BODY_MAX >= 16384U);
    CHECK(CURVE_MAX_SERIES == 256U);
    CHECK(CURVE_MAX_SERIES_PER_GROUP == 64U);
    CHECK(CURVE_EXPR_POOL_MAX >= 768U);
}


/* ---- P1.11 WebSocket push core (subscription / cursor / backpressure) ----- */

static void test_ws_core_attach(void)
{
    static curve_ws_state_t st;

    curve_ws_state_init(&st);
    CHECK(st.attached == 0U);
    CHECK(st.subscribed == 0U);
    CHECK(curve_ws_attach(&st, 60) == 0);
    CHECK(curve_ws_attach(&st, 61) == 1);
    CHECK(curve_ws_attach(&st, 60) == 0); /* duplicate handshake is idempotent */
    CHECK(st.attached == 2U);
    CHECK(curve_ws_find(&st, 60) != NULL);
    CHECK(curve_ws_find(&st, 99) == NULL);
    CHECK(curve_ws_detach(&st, 60));
    CHECK(!curve_ws_detach(&st, 60));
    CHECK(st.attached == 1U);
    CHECK(curve_ws_find(&st, 60) == NULL);

    /* Table capacity + reuse of freed slots. */
    {
        static curve_ws_state_t full;
        int slots[CURVE_WS_MAX_CLIENTS];

        curve_ws_state_init(&full);
        for (uint32_t i = 0U; i < CURVE_WS_MAX_CLIENTS; i++) {
            slots[i] = curve_ws_attach(&full, 70 + (int)i);
            CHECK(slots[i] == (int)i);
        }
        CHECK(curve_ws_attach(&full, 80) == -1); /* full */
        CHECK(curve_ws_detach(&full, 70 + (int)(CURVE_WS_MAX_CLIENTS - 1U)));
        CHECK(curve_ws_attach(&full, 81) ==
              (int)(CURVE_WS_MAX_CLIENTS - 1U));
    }
}

static void test_ws_core_subscribe(void)
{
    static curve_ws_state_t st;

    curve_ws_state_init(&st);
    CHECK(curve_ws_attach(&st, 60) == 0);
    CHECK(st.subscribed == 0U);
    CHECK(curve_ws_subscribe(&st, 60));
    CHECK(st.subscribed == 1U);
    CHECK(curve_ws_subscribe(&st, 60)); /* idempotent */
    CHECK(st.subscribed == 1U);
    CHECK(curve_ws_unsubscribe(&st, 60));
    CHECK(st.subscribed == 0U);
    CHECK(!curve_ws_subscribe(&st, 99)); /* unknown fd */
    CHECK(curve_ws_detach(&st, 60));
    CHECK(st.subscribed == 0U);
}

static void test_ws_core_plan_push(void)
{
    static curve_ws_state_t st;
    uint32_t since = 0U;
    uint32_t first = 0U;
    bool gap = false;
    uint32_t n;

    curve_ws_state_init(&st);
    CHECK(curve_ws_attach(&st, 60) == 0);

    /* Not subscribed yet -> nothing to push. */
    CHECK(curve_ws_plan_push(&st, 60, 10U, 20U, 32U, 1000U, &since, &first,
                             &gap) == 0U);
    CHECK(curve_ws_subscribe(&st, 60));

    /* First push starts at the oldest retained frame (like HTTP since=0). */
    n = curve_ws_plan_push(&st, 60, 10U, 20U, 32U, 1000U, &since, &first, &gap);
    CHECK(n == 11U);
    CHECK(since == 0U);
    CHECK(first == 10U);
    CHECK(!gap);
    curve_ws_mark_pushed(&st, 60, first, n, 1000U);
    CHECK(st.clients[0].next_seq == 21U);
    CHECK(st.clients[0].sent_batches == 1U);
    CHECK(st.clients[0].sent_frames == 11U);

    /* Push interval backpressure: immediately after a push -> 0. */
    CHECK(curve_ws_plan_push(&st, 60, 10U, 25U, 32U, 1005U, &since, &first,
                             &gap) == 0U);
    /* After the interval, only frames after next_seq-1 are pushed. */
    n = curve_ws_plan_push(&st, 60, 10U, 30U, 32U,
                           1000U + CURVE_WS_PUSH_INTERVAL_MS, &since, &first,
                           &gap);
    CHECK(n == 10U);
    CHECK(since == 20U);
    CHECK(first == 21U);
    CHECK(!gap);
    curve_ws_mark_pushed(&st, 60, first, n, 1000U + CURVE_WS_PUSH_INTERVAL_MS);
    CHECK(st.clients[0].next_seq == 31U);

    /* Up to date -> 0. */
    CHECK(curve_ws_plan_push(&st, 60, 10U, 30U, 32U, 2000U, &since, &first,
                             &gap) == 0U);

    /* max batch cap + gap when the client cursor fell behind the ring. */
    n = curve_ws_plan_push(&st, 60, 100U, 200U, 32U, 3000U, &since, &first,
                           &gap);
    CHECK(n == 32U);
    CHECK(first == 100U); /* since 30 -> first 31 < oldest 100 -> clamped */
    CHECK(gap);
    CHECK(since == 30U);

    /* max_frames above the protocol cap is clamped to CURVE_WS_MAX_BATCH_FRAMES. */
    curve_ws_subscribe(&st, 60);
    n = curve_ws_plan_push(&st, 60, 1U, 200U, 1000U, 4000U, &since, &first,
                           &gap);
    CHECK(n == CURVE_WS_MAX_BATCH_FRAMES);

    /* Dropped batch is counted and backs off one interval. */
    curve_ws_mark_dropped(&st, 60, 4000U);
    CHECK(st.clients[0].dropped_batches == 1U);
    CHECK(curve_ws_plan_push(&st, 60, 1U, 200U, 32U, 4001U, &since, &first,
                             &gap) == 0U);
}

static void test_ws_core_ack(void)
{
    static curve_ws_state_t st;
    uint32_t since = 0U;
    uint32_t first = 0U;
    bool gap = false;
    uint32_t n;

    curve_ws_state_init(&st);
    CHECK(curve_ws_attach(&st, 60) == 0);
    CHECK(curve_ws_subscribe(&st, 60));
    n = curve_ws_plan_push(&st, 60, 1U, 10U, 32U, 1000U, &since, &first, &gap);
    CHECK(n == 10U);
    curve_ws_mark_pushed(&st, 60, first, n, 1000U);
    CHECK(st.clients[0].next_seq == 11U);

    /* Ack is monotonic and cannot exceed the planned seq. */
    CHECK(curve_ws_ack(&st, 60, 5U));
    CHECK(st.clients[0].acked_seq == 5U);
    CHECK(curve_ws_ack(&st, 60, 3U));
    CHECK(st.clients[0].acked_seq == 5U);
    CHECK(curve_ws_ack(&st, 60, 1000U));
    CHECK(st.clients[0].acked_seq == 10U);
}


/* ---- P1.8 trigger_condition (comparison / parsing / status) -------------- */

static void test_condition_match(void)
{
    /* size 1: unsigned 0xFF = 255, signed 0xFF = -1 */
    CHECK(curve_condition_match(0xFFU, 1U, false, CURVE_COND_OP_EQ, 255));
    CHECK(!curve_condition_match(0xFFU, 1U, false, CURVE_COND_OP_EQ, 254));
    CHECK(curve_condition_match(0xFFU, 1U, true, CURVE_COND_OP_EQ, -1));
    CHECK(curve_condition_match(0xFFU, 1U, true, CURVE_COND_OP_LT, 0));
    CHECK(curve_condition_match(0xFFU, 1U, false, CURVE_COND_OP_GT, 254));

    /* size 2: 0x8000 unsigned 32768 / signed -32768 */
    CHECK(curve_condition_match(0x8000U, 2U, false, CURVE_COND_OP_EQ, 32768));
    CHECK(curve_condition_match(0x8000U, 2U, true, CURVE_COND_OP_EQ, -32768));
    CHECK(curve_condition_match(0x7FFFU, 2U, true, CURVE_COND_OP_GE, 32767));
    CHECK(curve_condition_match(0x1234U, 2U, true, CURVE_COND_OP_LT, 0x1235));

    /* size 4: int32 boundaries, high bytes must not leak into a 1/2-byte read. */
    CHECK(curve_condition_match(0xFFFFFFFFU, 4U, true, CURVE_COND_OP_EQ, -1));
    CHECK(curve_condition_match(0x80000000U, 4U, true, CURVE_COND_OP_EQ,
                                INT32_MIN));
    CHECK(curve_condition_match(0x7FFFFFFFU, 4U, true, CURVE_COND_OP_EQ,
                                INT32_MAX));
    CHECK(curve_condition_match(0x000000FFU, 1U, false, CURVE_COND_OP_EQ, 255));
    CHECK(!curve_condition_match(0x0000ABCDU, 2U, false, CURVE_COND_OP_EQ,
                                 0x1234));

    /* All six ops on one value (unsigned): 10 vs 10 / 20 / 5. */
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_EQ, 10));
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_NE, 11));
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_LT, 11));
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_LE, 10));
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_GT, 9));
    CHECK(curve_condition_match(10U, 4U, false, CURVE_COND_OP_GE, 10));
    CHECK(!curve_condition_match(10U, 4U, false, CURVE_COND_OP_LT, 10));
    CHECK(!curve_condition_match(10U, 4U, false, CURVE_COND_OP_GT, 10));

    /* Unsigned comparison does not sign-extend: 0xFFFFFFFF > 0 unsigned. */
    CHECK(curve_condition_match(0xFFFFFFFFU, 4U, false, CURVE_COND_OP_GT, 0));
    CHECK(!curve_condition_match(0xFFFFFFFFU, 4U, true, CURVE_COND_OP_GT, 0));

    /* Invalid size / op never match. */
    CHECK(!curve_condition_match(0U, 3U, false, CURVE_COND_OP_EQ, 0));
    CHECK(!curve_condition_match(0U, 4U, false, (uint8_t)99, 0));
    CHECK(strcmp(curve_cond_op_name(CURVE_COND_OP_EQ), "eq") == 0);
    CHECK(strcmp(curve_cond_op_name(CURVE_COND_OP_GE), "ge") == 0);
}

static void test_parse_condition(void)
{
    static curve_config_t cfg;
    char err[128];

    /* Minimal condition: size 4, unsigned eq, mode once, no stop. */
    {
        static const char *json =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}],"
            "\"trigger_condition\":{\"address\":536870916,\"op\":\"eq\","
            "\"value\":7}}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.cond_enabled);
        CHECK(cfg.cond_address == 0x20000004U);
        CHECK(cfg.cond_size == 4U);
        CHECK(cfg.cond_op == (uint8_t)CURVE_COND_OP_EQ);
        CHECK(cfg.cond_value == 7);
        CHECK(cfg.cond_mode == (uint8_t)CURVE_COND_MODE_ONCE);
        CHECK(!cfg.cond_signed);
        CHECK(!cfg.cond_stop_after);
    }
    /* Full options: size 2, signed, continuous, stop_after, op alias. */
    {
        static const char *json =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}],"
            "\"trigger_condition\":{\"address\":536870914,"
            "\"size\":2,\"op\":\">=\",\"value\":-5,\"signed\":true,"
            "\"mode\":\"continuous\",\"stop_after\":true}}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.cond_size == 2U);
        CHECK(cfg.cond_op == (uint8_t)CURVE_COND_OP_GE);
        CHECK(cfg.cond_value == -5);
        CHECK(cfg.cond_signed);
        CHECK(cfg.cond_mode == (uint8_t)CURVE_COND_MODE_CONTINUOUS);
        CHECK(cfg.cond_stop_after);
    }
    /* op aliases map to the same six operators. */
    {
        static const char *ops[] = {"==", "!=", "<", "<=", ">", ">="};
        static const uint8_t expect[] = {
            (uint8_t)CURVE_COND_OP_EQ, (uint8_t)CURVE_COND_OP_NE,
            (uint8_t)CURVE_COND_OP_LT, (uint8_t)CURVE_COND_OP_LE,
            (uint8_t)CURVE_COND_OP_GT, (uint8_t)CURVE_COND_OP_GE,
        };
        char json[256];

        for (size_t i = 0U; i < sizeof(ops) / sizeof(ops[0]); i++) {
            (void)snprintf(json, sizeof(json),
                           "{\"interval_us\":20000,\"series\":[{\"size\":1,"
                           "\"expr\":{\"kind\":\"addr\","
                           "\"address\":536870912}}],\"trigger_condition\":"
                           "{\"address\":536870912,\"op\":\"%s\",\"value\":1}}",
                           ops[i]);
            CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
                  ESP_OK);
            CHECK(cfg.cond_op == expect[i]);
        }
    }
    /* Compatible with breakpoint + watchpoint trigger modes (and halt). */
    {
        static const char *bp =
            "{\"trigger\":\"breakpoint\",\"breakpoint_address\":134356036,"
            "\"series\":[{\"size\":1,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870920,\"op\":\"ne\",\"value\":0}}";
        static const char *wp =
            "{\"trigger\":\"watchpoint\",\"watch_address\":536870912,"
            "\"series\":[{\"size\":1,\"expr\":{\"kind\":\"addr\","
            "\"address\":536870916}}],\"trigger_condition\":"
            "{\"address\":536870920,\"size\":4,\"op\":\"lt\",\"value\":-1}}";

        CHECK(curve_config_parse(bp, strlen(bp), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.cond_enabled);
        CHECK(cfg.cond_op == (uint8_t)CURVE_COND_OP_NE);
        CHECK(curve_config_parse(wp, strlen(wp), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(cfg.cond_enabled);
        CHECK(cfg.cond_op == (uint8_t)CURVE_COND_OP_LT);
        CHECK(cfg.cond_value == -1);
    }
    /* No trigger_condition -> disabled (legacy requests unchanged). */
    {
        static const char *json =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\",\"address\":536870912}}]}";

        CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) ==
              ESP_OK);
        CHECK(!cfg.cond_enabled);
    }
    /* Invalid objects/fields -> 400-class errors. */
    {
        static const char *notobj =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":7}";
        static const char *noaddr =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":{\"op\":\"eq\","
            "\"value\":1}}";
        static const char *badaddr =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":268435456,\"op\":\"eq\",\"value\":1}}";
        static const char *badsize =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870912,\"size\":8,\"op\":\"eq\",\"value\":1}}";
        static const char *badop =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870912,\"op\":\"xor\",\"value\":1}}";
        static const char *badvalue =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870912,\"op\":\"eq\",\"value\":4294967296}}";
        static const char *badmode =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870912,\"op\":\"eq\",\"value\":1,"
            "\"mode\":\"always\"}}";
        static const char *badsigned =
            "{\"interval_us\":20000,\"series\":[{\"size\":1,\"expr\":"
            "{\"kind\":\"addr\","
            "\"address\":536870912}}],\"trigger_condition\":"
            "{\"address\":536870912,\"op\":\"eq\",\"value\":1,"
            "\"signed\":\"yes\"}}";

        CHECK(curve_config_parse(notobj, strlen(notobj), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition must be an object") != NULL);
        CHECK(curve_config_parse(noaddr, strlen(noaddr), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.address") != NULL);
        CHECK(curve_config_parse(badaddr, strlen(badaddr), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition address") != NULL);
        CHECK(curve_config_parse(badsize, strlen(badsize), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.size") != NULL);
        CHECK(curve_config_parse(badop, strlen(badop), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.op") != NULL);
        CHECK(curve_config_parse(badvalue, strlen(badvalue), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.value") != NULL);
        CHECK(curve_config_parse(badmode, strlen(badmode), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.mode") != NULL);
        CHECK(curve_config_parse(badsigned, strlen(badsigned), &cfg, err,
                                 sizeof(err)) == ESP_ERR_INVALID_ARG);
        CHECK(strstr(err, "trigger_condition.signed") != NULL);
    }
}


/* ---- P1.8 object watch: per-tick read cache + 512-series body size -------- */

static void test_expr_read_cache(void)
{
    static curve_config_t cfg;
    char err[128];
    static const char *json =
        "{\"interval_us\":20000,\"halt\":true,\"series\":["
        "{\"expr_s\":\"D00000000A20000000\",\"size\":4}]}";
    uint32_t reg_values[CURVE_MAX_CORE_REGS] = {0};
    bool reg_valid[CURVE_MAX_CORE_REGS] = {false};
    uint32_t address1 = 0U;
    uint32_t address2 = 0U;
    int reads_before;

    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    g_pointer_value = 0x20000100U;

    /* Cache disabled (default): every evaluation reads the pointer. */
    g_read_count = 0;
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address1) == CURVE_EVAL_OK);
    CHECK(address1 == 0x20000100U);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address2) == CURVE_EVAL_OK);
    CHECK(address2 == 0x20000100U);
    CHECK(g_read_count == 2);

    /* Halted tick: one shared pointer read is reused within the tick. */
    g_read_count = 0;
    curve_expr_cache_begin(true);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address1) == CURVE_EVAL_OK);
    reads_before = g_read_count;
    CHECK(reads_before == 1);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address2) == CURVE_EVAL_OK);
    CHECK(g_read_count == reads_before); /* cache hit, no extra SWD read */
    CHECK(address1 == address2);
    curve_expr_cache_end();

    /* Cache is cleared between ticks: another tick reads again. */
    g_read_count = 0;
    curve_expr_cache_begin(true);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address1) == CURVE_EVAL_OK);
    curve_expr_cache_end();
    curve_expr_cache_begin(true);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address1) == CURVE_EVAL_OK);
    curve_expr_cache_end();
    CHECK(g_read_count == 2);

    /* No-halt tick: cache disabled (core may change memory between reads). */
    g_read_count = 0;
    curve_expr_cache_begin(false);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address1) == CURVE_EVAL_OK);
    CHECK(curve_expr_eval(&cfg, cfg.series[0].expr, reg_values, reg_valid,
                          &address2) == CURVE_EVAL_OK);
    curve_expr_cache_end();
    CHECK(g_read_count == 2);
}

/* A realistic 256-series compact v3 request must fit the HTTP body budget. */
static void test_compact_256_series_body(void)
{
    static curve_config_t cfg;
    static char json[CURVE_JSON_BODY_MAX + 1U];
    char err[128];
    size_t pos = 0U;

    pos = (size_t)snprintf(json, sizeof(json),
                           "{\"interval_us\":20000,\"halt\":true,\"groups\":[");
    for (int g = 0; g < 4; g++) {
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                "%s{\"series\":[", (g == 0) ? "" : ",");
        for (int i = 0; i < 64; i++) {
            unsigned index = (unsigned)(g * 64 + i);

            pos += (size_t)snprintf(json + pos, sizeof(json) - pos,
                                    "%s{\"expr_s\":\"A%08X\",\"size\":4}",
                                    (i == 0) ? "" : ",", 0x20000000U + index * 4U);
        }
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "]}");
    }
    (void)snprintf(json + pos, sizeof(json) - pos, "]}");
    CHECK(strlen(json) < CURVE_JSON_BODY_MAX);
    CHECK(curve_config_parse(json, strlen(json), &cfg, err, sizeof(err)) == ESP_OK);
    CHECK(cfg.group_count == 4U);
    CHECK(cfg.series_count == 256U);
    CHECK(cfg.sample_bytes == 256U);
    CHECK(cfg.pool_used == 256U); /* one unique addr node per series (no dedup) */
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
    test_expr_dag_dedup();
    test_variants_parse_select();
    test_variant_pool_limit();
    test_expr_pool_limit();
    test_group_rotation();
    test_ring_budget_boundaries();
    test_group_weights();
    test_cvf2_wire();
    test_want_pc_parse();
    test_watchpoint_params();
    test_parse_watchpoint();
    test_dwt_comparator_alloc();
    test_snapshot_state_names();
    test_parse_snapshot();
    test_watchpoints_array();
    test_backtrace_unwind();
    test_trace_ring();
    test_trace_line_ring();
    test_ring_group_trigger_marker();
    test_ws_core_attach();
    test_ws_core_subscribe();
    test_ws_core_plan_push();
    test_ws_core_ack();
    test_expr_read_cache();
    test_compact_256_series_body();
    test_sample_due();
    test_parse_sample_every();
    test_cvf2_series_mask();
    test_bin_parse_256();
    test_bin_parse_errors();
    test_bin_parse_watchpoints();
    test_condition_match();
    test_parse_condition();
    test_config_static_budget();

    if (g_failures == 0) {
        printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    printf("FAILED: %d/%d checks\n", g_failures, g_checks);
    return 1;
}
