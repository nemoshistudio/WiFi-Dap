/*
 * Host-side regression test for the CMSIS-DAP over TCP frame codec and
 * streaming parser (components/dap_tcp/src/dap_tcp_frame.c).
 *
 * Covers the wire format shared with OpenOCD's `cmsis-dap backend tcp` and
 * the XRDAP probes (XRDAP-Core docs/protocol.md section 2):
 *   - 8-byte header encode/decode: "DAP\0" + u16 LE length + type + reserved
 *   - direction rules: a probe accepts type 1 (request) / 3 (control request)
 *   - invalid headers: bad signature, wrong type, length 0 or above 1024
 *   - streaming parser: half packets, sticky / pipelined packets, byte-at-a-time
 *   - the 1024-byte payload limit and both maximum-size frames in one stream
 *   - deterministic random chunking over 128 frames (payload integrity)
 *
 * No target hardware, no ESP-IDF, no sockets.  Run:
 *   components/dap_tcp/test/host/run.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dap_tcp_frame.h"

/* Layout invariants asserted at compile time (also checked at run time). */
_Static_assert(DAP_TCP_HEADER_LEN == 8u, "header is 8 bytes");
_Static_assert(DAP_TCP_MAX_FRAME_LEN == (DAP_TCP_HEADER_LEN + DAP_TCP_MAX_PAYLOAD),
               "max frame = header + max payload");
_Static_assert(DAP_TCP_MAX_PAYLOAD == 1024u, "OpenOCD TCP backend packet size");

#define MAX_FRAMES 128u

static unsigned checks;
static unsigned failures;

static void check(const char *name, int cond, const char *detail)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        failures++;
        printf("  FAIL %s%s%s\n", name, detail != NULL ? " :: " : "",
               detail != NULL ? detail : "");
    }
}

static void check_u32(const char *name, uint32_t got, uint32_t want)
{
    char detail[96];
    const int ok = (got == want);
    if (!ok) {
        snprintf(detail, sizeof(detail), "got %lu, want %lu", (unsigned long)got,
                 (unsigned long)want);
    }
    check(name, ok, ok ? NULL : detail);
}

/* ------------------------------------------------------------------------- */
/* Deterministic pseudo-random numbers (reproducible chunking fuzz)          */
/* ------------------------------------------------------------------------- */

static uint32_t rng_state = 0x12345678u;

static uint32_t rng_next(void)
{
    /* Numerical Recipes LCG: deterministic and good enough for chunk sizes. */
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static uint32_t fnv1a(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ------------------------------------------------------------------------- */
/* Raw header helpers                                                        */
/* ------------------------------------------------------------------------- */

/* Build a header without validation, for rejection tests. */
static void raw_header(uint8_t *hdr, const char magic[4], uint16_t length, uint8_t type,
                       uint8_t reserved)
{
    memcpy(hdr, magic, 4);
    hdr[DAP_TCP_OFF_LENGTH] = (uint8_t)(length & 0xFFu);
    hdr[DAP_TCP_OFF_LENGTH + 1u] = (uint8_t)(length >> 8);
    hdr[DAP_TCP_OFF_TYPE] = type;
    hdr[DAP_TCP_OFF_RESERVED] = reserved;
}

/* "DAP\0" + payload, for the parser tests. */
static size_t make_frame(uint8_t *out, dap_tcp_frame_type_t type, const uint8_t *payload,
                         uint16_t len)
{
    return dap_tcp_encode_frame(out, DAP_TCP_MAX_FRAME_LEN, type, payload, len);
}

/* One request frame carrying DAP_Info(0xFF): payload 0x00 0xFF. */
static const uint8_t dap_info_payload[] = {0x00u, 0xFFu};
static const uint8_t dap_info_frame[] = {0x44u, 0x41u, 0x50u, 0x00u, 0x02u, 0x00u,
                                         0x01u, 0x00u, 0x00u, 0xFFu};

/* ------------------------------------------------------------------------- */
/* 1. Header codec                                                           */
/* ------------------------------------------------------------------------- */

static void header_codec_ok(void)
{
    uint8_t frame[DAP_TCP_MAX_FRAME_LEN];
    dap_tcp_frame_header_t hdr;
    uint8_t patched[DAP_TCP_HEADER_LEN];

    printf("header codec\n");

    check_u32("encode request frame length",
              (uint32_t)dap_tcp_encode_frame(frame, sizeof(frame),
                                             DAP_TCP_FRAME_TYPE_REQUEST, dap_info_payload,
                                             (uint16_t)sizeof(dap_info_payload)),
              (uint32_t)sizeof(dap_info_frame));
    check("encode request frame bytes (DAP\\0 + 02 00 + type1 + 00)",
          memcmp(frame, dap_info_frame, sizeof(dap_info_frame)) == 0, NULL);

    dap_tcp_encode_header(frame, DAP_TCP_FRAME_TYPE_RESPONSE, 0x0102u);
    check("encode length little-endian (0x0102 -> 02 01)",
          frame[DAP_TCP_OFF_LENGTH] == 0x02u && frame[DAP_TCP_OFF_LENGTH + 1u] == 0x01u,
          NULL);
    check("encode type byte", frame[DAP_TCP_OFF_TYPE] == 2u, NULL);
    check("encode reserved byte is 0", frame[DAP_TCP_OFF_RESERVED] == 0u, NULL);
    check("encode magic", frame[0] == 'D' && frame[1] == 'A' && frame[2] == 'P' &&
                              frame[3] == 0u,
          NULL);

    dap_tcp_encode_header(frame, DAP_TCP_FRAME_TYPE_CONTROL_REQUEST, 1024u);
    check("encode max length 1024 -> 00 04", frame[4] == 0x00u && frame[5] == 0x04u, NULL);

    check("decode request header OK",
          dap_tcp_decode_request_header(dap_info_frame, DAP_TCP_MAX_PAYLOAD, &hdr) ==
                  DAP_TCP_DECODE_OK &&
              hdr.type == DAP_TCP_FRAME_TYPE_REQUEST && hdr.length == 2u,
          NULL);

    dap_tcp_encode_header(patched, DAP_TCP_FRAME_TYPE_CONTROL_REQUEST, 7u);
    check("decode control request header OK",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
                  DAP_TCP_DECODE_OK &&
              hdr.type == DAP_TCP_FRAME_TYPE_CONTROL_REQUEST && hdr.length == 7u,
          NULL);

    raw_header(patched, "DAP", 2u, 1u, 0u);
    patched[DAP_TCP_OFF_RESERVED] = 0xABu;
    check("reserved byte not checked (0xAB accepted)",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_OK,
          NULL);

    raw_header(patched, "DAP", 2u, 1u, 0u);
    patched[3] = 0x01u;
    check("bad magic (0x01 at offset 3)",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_MAGIC,
          NULL);
    raw_header(patched, "DAP", 2u, 1u, 0u);
    patched[2] = 'Q';
    check("bad magic (P -> Q)",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_MAGIC,
          NULL);

    for (uint8_t type = 0u; type <= 8u; type++) {
        uint8_t bad[DAP_TCP_HEADER_LEN];
        raw_header(bad, "DAP", 2u, type, 0u);
        const dap_tcp_decode_result_t got =
            dap_tcp_decode_request_header(bad, DAP_TCP_MAX_PAYLOAD, &hdr);
        const int want_err = (type == 1u || type == 3u) ? 0 : 1;
        char name[64];
        snprintf(name, sizeof(name), "request direction: type %u %s", (unsigned)type,
                 want_err ? "rejected" : "accepted");
        check(name, want_err ? got == DAP_TCP_DECODE_ERR_TYPE : got == DAP_TCP_DECODE_OK,
              NULL);
    }

    raw_header(patched, "DAP", 0u, 1u, 0u);
    check("zero length rejected",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_LENGTH,
          NULL);
    raw_header(patched, "DAP", 1025u, 1u, 0u);
    check("length 1025 rejected",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_LENGTH,
          NULL);
    raw_header(patched, "DAP", 0xFFFFu, 1u, 0u);
    check("length 0xFFFF rejected",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_LENGTH,
          NULL);
    raw_header(patched, "DAP", 1024u, 1u, 0u);
    check("length 1024 accepted at the default limit",
          dap_tcp_decode_request_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
                  DAP_TCP_DECODE_OK &&
              hdr.length == 1024u,
          NULL);
    check("length 1024 rejected with a 512-byte limit",
          dap_tcp_decode_request_header(patched, 512u, &hdr) == DAP_TCP_DECODE_ERR_LENGTH,
          NULL);

    raw_header(patched, "DAP", 2u, 2u, 0u);
    check("decode response header OK (type 2)",
          dap_tcp_decode_response_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
                  DAP_TCP_DECODE_OK &&
              hdr.type == DAP_TCP_FRAME_TYPE_RESPONSE,
          NULL);
    raw_header(patched, "DAP", 2u, 4u, 0u);
    check("decode control response header OK (type 4)",
          dap_tcp_decode_response_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
                  DAP_TCP_DECODE_OK &&
              hdr.type == DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE,
          NULL);
    raw_header(patched, "DAP", 2u, 1u, 0u);
    check("response direction rejects type 1",
          dap_tcp_decode_response_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_TYPE,
          NULL);
    raw_header(patched, "DAP", 2u, 3u, 0u);
    check("response direction rejects type 3",
          dap_tcp_decode_response_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_TYPE,
          NULL);
    raw_header(patched, "DAP", 0u, 2u, 0u);
    check("response direction rejects zero length",
          dap_tcp_decode_response_header(patched, DAP_TCP_MAX_PAYLOAD, &hdr) ==
              DAP_TCP_DECODE_ERR_LENGTH,
          NULL);

    check("response type of request",
          dap_tcp_response_type_of(DAP_TCP_FRAME_TYPE_REQUEST) ==
              DAP_TCP_FRAME_TYPE_RESPONSE,
          NULL);
    check("response type of control request",
          dap_tcp_response_type_of(DAP_TCP_FRAME_TYPE_CONTROL_REQUEST) ==
              DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE,
          NULL);
    check("response type of response is invalid",
          dap_tcp_response_type_of(DAP_TCP_FRAME_TYPE_RESPONSE) ==
              DAP_TCP_FRAME_TYPE_INVALID,
          NULL);
    check("response type of control response is invalid",
          dap_tcp_response_type_of(DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE) ==
              DAP_TCP_FRAME_TYPE_INVALID,
          NULL);

    check("is_request_type(1)", dap_tcp_is_request_type(DAP_TCP_FRAME_TYPE_REQUEST), NULL);
    check("is_request_type(3)", dap_tcp_is_request_type(DAP_TCP_FRAME_TYPE_CONTROL_REQUEST),
          NULL);
    check("is_request_type(2) false", !dap_tcp_is_request_type(DAP_TCP_FRAME_TYPE_RESPONSE),
          NULL);
    check("is_request_type(4) false",
          !dap_tcp_is_request_type(DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE), NULL);
    check("is_request_type(0) false", !dap_tcp_is_request_type(DAP_TCP_FRAME_TYPE_INVALID),
          NULL);

    check("decode result string OK non-NULL", dap_tcp_decode_result_str(DAP_TCP_DECODE_OK) != NULL,
          NULL);
    check("decode result strings differ",
          strcmp(dap_tcp_decode_result_str(DAP_TCP_DECODE_ERR_MAGIC),
                 dap_tcp_decode_result_str(DAP_TCP_DECODE_ERR_TYPE)) != 0 &&
              strcmp(dap_tcp_decode_result_str(DAP_TCP_DECODE_ERR_LENGTH),
                     dap_tcp_decode_result_str(DAP_TCP_DECODE_OK)) != 0,
          NULL);
}

static void encode_frame_ok(void)
{
    uint8_t out[DAP_TCP_MAX_FRAME_LEN];
    uint8_t payload[4] = {0x11u, 0x22u, 0x33u, 0x44u};

    printf("frame encode\n");

    check_u32("encode frame 4-byte payload returns header+payload",
              (uint32_t)dap_tcp_encode_frame(out, sizeof(out), DAP_TCP_FRAME_TYPE_REQUEST,
                                             payload, 4u),
              12u);
    check("encode frame copies payload",
          memcmp(out + DAP_TCP_HEADER_LEN, payload, sizeof(payload)) == 0, NULL);
    check_u32("encode frame rejects too-small capacity",
              (uint32_t)dap_tcp_encode_frame(out, 11u, DAP_TCP_FRAME_TYPE_REQUEST, payload, 4u),
              0u);
    check_u32("encode frame rejects zero payload length",
              (uint32_t)dap_tcp_encode_frame(out, sizeof(out), DAP_TCP_FRAME_TYPE_REQUEST,
                                             payload, 0u),
              0u);
    check_u32("encode frame rejects above the 1024 limit",
              (uint32_t)dap_tcp_encode_frame(out, sizeof(out), DAP_TCP_FRAME_TYPE_REQUEST,
                                             payload, 1025u),
              0u);
    check_u32("encode frame rejects NULL payload",
              (uint32_t)dap_tcp_encode_frame(out, sizeof(out), DAP_TCP_FRAME_TYPE_REQUEST,
                                             NULL, 4u),
              0u);
    check_u32("encode frame 1-byte payload returns 9",
              (uint32_t)dap_tcp_encode_frame(out, sizeof(out), DAP_TCP_FRAME_TYPE_RESPONSE,
                                             payload, 1u),
              9u);
}

/* ------------------------------------------------------------------------- */
/* 2. Parser basics, half packets and sticky packets                         */
/* ------------------------------------------------------------------------- */

static void parser_basics_ok(void)
{
    dap_tcp_parser_t parser;
    uint8_t frame[DAP_TCP_MAX_FRAME_LEN];
    size_t used = 0;

    printf("parser basics\n");

    dap_tcp_parser_init(&parser);
    check("init: payload_len 0", dap_tcp_parser_payload_len(&parser) == 0u, NULL);
    check("init: type invalid", dap_tcp_parser_type(&parser) == DAP_TCP_FRAME_TYPE_INVALID,
          NULL);
    check("init: error OK", dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_OK, NULL);
    check("feed empty is NEED_MORE",
          dap_tcp_parser_feed(&parser, NULL, 0u, &used) == DAP_TCP_PARSE_NEED_MORE &&
              used == 0u,
          NULL);

    const size_t frame_len = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, dap_info_payload,
                                        (uint16_t)sizeof(dap_info_payload));
    check_u32("single-frame input returns FRAME_READY bytes consumed",
              dap_tcp_parser_feed(&parser, frame, frame_len, &used) == DAP_TCP_PARSE_FRAME_READY
                  ? (uint32_t)used
                  : 0u,
              (uint32_t)frame_len);
    check("frame type request", dap_tcp_parser_type(&parser) == DAP_TCP_FRAME_TYPE_REQUEST,
          NULL);
    check_u32("frame payload length", dap_tcp_parser_payload_len(&parser), 2u);
    check("frame payload bytes",
          memcmp(dap_tcp_parser_payload(&parser), dap_info_payload,
                 sizeof(dap_info_payload)) == 0,
          NULL);
    dap_tcp_parser_consume(&parser);
    check("consume resets payload length", dap_tcp_parser_payload_len(&parser) == 0u, NULL);
    check("parser is re-armed after consume",
          dap_tcp_parser_feed(&parser, frame, frame_len, &used) == DAP_TCP_PARSE_FRAME_READY,
          NULL);

    /* Control request (type 3) is accepted by the probe-side parser. */
    dap_tcp_parser_init(&parser);
    const size_t ctrl_len = make_frame(frame, DAP_TCP_FRAME_TYPE_CONTROL_REQUEST, dap_info_payload,
                                       (uint16_t)sizeof(dap_info_payload));
    check("control request frame accepted",
          dap_tcp_parser_feed(&parser, frame, ctrl_len, &used) ==
                  DAP_TCP_PARSE_FRAME_READY &&
              dap_tcp_parser_type(&parser) == DAP_TCP_FRAME_TYPE_CONTROL_REQUEST,
          NULL);

    /* A response frame has the wrong direction for a probe. */
    dap_tcp_parser_init(&parser);
    make_frame(frame, DAP_TCP_FRAME_TYPE_RESPONSE, dap_info_payload,
               (uint16_t)sizeof(dap_info_payload));
    check("response frame rejected by probe parser",
          dap_tcp_parser_feed(&parser, frame, frame_len, &used) == DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_TYPE,
          NULL);

    /* Reserved byte is ignored on receive. */
    dap_tcp_parser_init(&parser);
    memcpy(frame, dap_info_frame, sizeof(dap_info_frame));
    frame[DAP_TCP_OFF_RESERVED] = 0x7Fu;
    check("reserved byte 0x7F ignored",
          dap_tcp_parser_feed(&parser, frame, sizeof(dap_info_frame), &used) ==
              DAP_TCP_PARSE_FRAME_READY,
          NULL);
}

static void parser_split_ok(void)
{
    dap_tcp_parser_t parser;
    uint8_t payload[13];
    uint8_t frame[DAP_TCP_MAX_FRAME_LEN];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(0xA0u + i);
    }
    const size_t frame_len = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, payload,
                                        (uint16_t)sizeof(payload));

    printf("half packets (every split point)\n");

    for (size_t split = 0; split <= frame_len; split++) {
        size_t used = 0;
        char name[96];
        dap_tcp_parser_init(&parser);

        const dap_tcp_parse_result_t first =
            dap_tcp_parser_feed(&parser, frame, split, &used);
        int ok;
        if (split == frame_len) {
            ok = (first == DAP_TCP_PARSE_FRAME_READY && used == split);
        } else {
            ok = (first == DAP_TCP_PARSE_NEED_MORE && used == split);
        }

        if (ok && split < frame_len) {
            used = 0;
            const dap_tcp_parse_result_t second =
                dap_tcp_parser_feed(&parser, frame + split, frame_len - split, &used);
            ok = (second == DAP_TCP_PARSE_FRAME_READY &&
                  used == frame_len - split &&
                  dap_tcp_parser_payload_len(&parser) == sizeof(payload) &&
                  memcmp(dap_tcp_parser_payload(&parser), payload, sizeof(payload)) == 0);
        }
        snprintf(name, sizeof(name), "split at byte %lu/%lu", (unsigned long)split,
                 (unsigned long)frame_len);
        check(name, ok, NULL);
    }

    /* Maximum-size payload split across two reads. */
    dap_tcp_parser_init(&parser);
    static uint8_t big_payload[DAP_TCP_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof(big_payload); i++) {
        big_payload[i] = (uint8_t)(i * 31u + 7u);
    }
    const size_t big_len = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, big_payload,
                                      (uint16_t)sizeof(big_payload));
    size_t used = 0;
    check_u32("1024-byte frame total is 1032", (uint32_t)big_len,
              (uint32_t)DAP_TCP_MAX_FRAME_LEN);
    check("1024-byte frame: first 7 bytes need more",
          dap_tcp_parser_feed(&parser, frame, 7u, &used) == DAP_TCP_PARSE_NEED_MORE &&
              used == 7u,
          NULL);
    check("1024-byte frame: remaining bytes complete it",
          dap_tcp_parser_feed(&parser, frame + 7u, big_len - 7u, &used) ==
                  DAP_TCP_PARSE_FRAME_READY &&
              dap_tcp_parser_payload_len(&parser) == DAP_TCP_MAX_PAYLOAD &&
              memcmp(dap_tcp_parser_payload(&parser), big_payload,
                     sizeof(big_payload)) == 0,
          NULL);

    /* One read per byte over a full 1024-byte frame. */
    dap_tcp_parser_init(&parser);
    unsigned ready_count = 0;
    for (size_t i = 0; i < big_len; i++) {
        used = 0;
        const dap_tcp_parse_result_t rc = dap_tcp_parser_feed(&parser, frame + i, 1u, &used);
        if (rc == DAP_TCP_PARSE_FRAME_READY) {
            ready_count++;
            dap_tcp_parser_consume(&parser);
        }
    }
    check_u32("byte-at-a-time yields exactly one frame", ready_count, 1u);
}

static void parser_sticky_ok(void)
{
    dap_tcp_parser_t parser;
    uint8_t frame_a[DAP_TCP_MAX_FRAME_LEN];
    uint8_t frame_b[DAP_TCP_MAX_FRAME_LEN];
    uint8_t frame_c[DAP_TCP_MAX_FRAME_LEN];
    static uint8_t stream[3u * DAP_TCP_MAX_FRAME_LEN];
    const uint8_t payload_a[] = {0x01u};
    const uint8_t payload_b[] = {0x02u, 0x03u, 0x04u};
    const uint8_t payload_c[] = {0x05u, 0x06u};

    printf("sticky packets (pipelined frames)\n");

    const size_t len_a = make_frame(frame_a, DAP_TCP_FRAME_TYPE_REQUEST, payload_a,
                                    (uint16_t)sizeof(payload_a));
    const size_t len_b = make_frame(frame_b, DAP_TCP_FRAME_TYPE_CONTROL_REQUEST, payload_b,
                                    (uint16_t)sizeof(payload_b));
    const size_t len_c = make_frame(frame_c, DAP_TCP_FRAME_TYPE_REQUEST, payload_c,
                                    (uint16_t)sizeof(payload_c));
    memcpy(stream, frame_a, len_a);
    memcpy(stream + len_a, frame_b, len_b);
    memcpy(stream + len_a + len_b, frame_c, len_c);
    const size_t total = len_a + len_b + len_c;

    /* Three frames in one read: one FRAME_READY per frame with the exact
     * consumed count, then the remaining bytes are presented again. */
    dap_tcp_parser_init(&parser);
    size_t off = 0;
    unsigned frames = 0;
    size_t used = 0;
    while (off < total) {
        const dap_tcp_parse_result_t rc =
            dap_tcp_parser_feed(&parser, stream + off, total - off, &used);
        if (rc != DAP_TCP_PARSE_FRAME_READY) {
            break;
        }
        frames++;
        if (frames == 1u) {
            check_u32("first frame consumes only its own bytes", (uint32_t)used,
                      (uint32_t)len_a);
        }
        dap_tcp_parser_consume(&parser);
        off += used;
    }
    check_u32("three pipelined frames recovered", frames, 3u);
    check_u32("pipeline off ends at stream end", (uint32_t)off, (uint32_t)total);

    /* Byte-at-a-time over the same stream recovers the same frames in order. */
    dap_tcp_parser_init(&parser);
    const uint8_t *const expected_payloads[] = {payload_a, payload_b, payload_c};
    const size_t expected_lens[] = {sizeof(payload_a), sizeof(payload_b),
                                    sizeof(payload_c)};
    const dap_tcp_frame_type_t expected_types[] = {
        DAP_TCP_FRAME_TYPE_REQUEST, DAP_TCP_FRAME_TYPE_CONTROL_REQUEST,
        DAP_TCP_FRAME_TYPE_REQUEST};
    unsigned recovered = 0;
    int order_ok = 1;
    for (size_t i = 0; i < total; i++) {
        used = 0;
        const dap_tcp_parse_result_t rc = dap_tcp_parser_feed(&parser, stream + i, 1u, &used);
        if (rc != DAP_TCP_PARSE_FRAME_READY) {
            continue;
        }
        if (recovered < 3u) {
            order_ok &= (dap_tcp_parser_payload_len(&parser) == expected_lens[recovered]) &&
                        (dap_tcp_parser_type(&parser) == expected_types[recovered]) &&
                        (memcmp(dap_tcp_parser_payload(&parser), expected_payloads[recovered],
                                expected_lens[recovered]) == 0);
        }
        recovered++;
        dap_tcp_parser_consume(&parser);
    }
    check_u32("byte-at-a-time over a 3-frame stream", recovered, 3u);
    check("byte-at-a-time frame order and payloads", order_ok, NULL);

    /* A read that contains the tail of frame A and all of frame B: frame A
     * comes out first, the rest of that read stays with the caller. */
    dap_tcp_parser_init(&parser);
    used = 0;
    check("split header of frame A needs more",
          dap_tcp_parser_feed(&parser, stream, 3u, &used) == DAP_TCP_PARSE_NEED_MORE &&
              used == 3u,
          NULL);
    used = 0;
    check("tail of A + all of B returns A first",
          dap_tcp_parser_feed(&parser, stream + 3u, len_a - 3u + len_b, &used) ==
                  DAP_TCP_PARSE_FRAME_READY &&
              used == len_a - 3u && dap_tcp_parser_payload_len(&parser) == 1u &&
              memcmp(dap_tcp_parser_payload(&parser), payload_a, sizeof(payload_a)) == 0,
          NULL);
    dap_tcp_parser_consume(&parser);
    used = 0;
    check("B completes from the rest of that read",
          dap_tcp_parser_feed(&parser, stream + len_a, len_b, &used) ==
                      DAP_TCP_PARSE_FRAME_READY &&
              used == len_b &&
              memcmp(dap_tcp_parser_payload(&parser), payload_b, sizeof(payload_b)) == 0,
          NULL);
    dap_tcp_parser_consume(&parser);
    used = 0;
    check("C completes from the rest of the stream",
          dap_tcp_parser_feed(&parser, stream + len_a + len_b, len_c, &used) ==
                  DAP_TCP_PARSE_FRAME_READY &&
              memcmp(dap_tcp_parser_payload(&parser), payload_c, sizeof(payload_c)) == 0,
          NULL);
}

/* ------------------------------------------------------------------------- */
/* 3. Invalid headers and limits                                             */
/* ------------------------------------------------------------------------- */

static void parser_invalid_ok(void)
{
    dap_tcp_parser_t parser;
    uint8_t frame[DAP_TCP_MAX_FRAME_LEN];
    uint8_t bad[DAP_TCP_HEADER_LEN];
    size_t used = 0;

    printf("invalid headers and limits\n");

    raw_header(bad, "XAP", 2u, 1u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects bad magic",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_MAGIC,
          NULL);

    raw_header(bad, "DAP", 0u, 1u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects zero length",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_LENGTH,
          NULL);

    raw_header(bad, "DAP", 1025u, 1u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects 1025-byte header before buffering the payload",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_LENGTH,
          NULL);

    raw_header(bad, "DAP", 0xFFFFu, 1u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects 0xFFFF length",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR,
          NULL);

    raw_header(bad, "DAP", 2u, 2u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects response type 2",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_TYPE,
          NULL);

    raw_header(bad, "DAP", 2u, 0u, 0u);
    dap_tcp_parser_init(&parser);
    check("parser rejects type 0",
          dap_tcp_parser_feed(&parser, bad, sizeof(bad), &used) == DAP_TCP_PARSE_ERROR,
          NULL);

    /* Error detection works while the header arrives one byte at a time. */
    raw_header(bad, "DAP", 2u, 4u, 0u);
    dap_tcp_parser_init(&parser);
    int saw_error = 0;
    for (size_t i = 0; i < sizeof(bad); i++) {
        used = 0;
        const dap_tcp_parse_result_t rc = dap_tcp_parser_feed(&parser, bad + i, 1u, &used);
        if (rc == DAP_TCP_PARSE_ERROR) {
            saw_error = (dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_TYPE);
            break;
        }
    }
    check("byte-at-a-time invalid header reported", saw_error, NULL);

    dap_tcp_parser_init(&parser);
    check("re-init clears the sticky error",
          dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_OK &&
              dap_tcp_parser_feed(&parser, NULL, 0u, &used) == DAP_TCP_PARSE_NEED_MORE,
          NULL);

    /* A valid frame followed by a bad header in the same read: the frame is
     * delivered first, the error is reported on the following feed. */
    const size_t frame_len = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, dap_info_payload,
                                        (uint16_t)sizeof(dap_info_payload));
    uint8_t stream[sizeof(dap_info_frame) + DAP_TCP_HEADER_LEN];
    memcpy(stream, frame, frame_len);
    raw_header(stream + frame_len, "XAP", 2u, 1u, 0u);
    dap_tcp_parser_init(&parser);
    used = 0;
    check("valid frame before garbage is delivered",
          dap_tcp_parser_feed(&parser, stream, sizeof(stream), &used) ==
                  DAP_TCP_PARSE_FRAME_READY &&
              used == frame_len,
          NULL);
    dap_tcp_parser_consume(&parser);
    used = 0;
    check("garbage after the frame is reported",
          dap_tcp_parser_feed(&parser, stream + frame_len, DAP_TCP_HEADER_LEN, &used) ==
                  DAP_TCP_PARSE_ERROR &&
              dap_tcp_parser_error(&parser) == DAP_TCP_DECODE_ERR_MAGIC,
          NULL);

    /* Two maximum-size frames in one read: the parser takes one at a time and
     * never overflows its fixed buffer. */
    static uint8_t big_payload_a[DAP_TCP_MAX_PAYLOAD];
    static uint8_t big_payload_b[DAP_TCP_MAX_PAYLOAD];
    static uint8_t two_frames[2u * DAP_TCP_MAX_FRAME_LEN];
    for (size_t i = 0; i < DAP_TCP_MAX_PAYLOAD; i++) {
        big_payload_a[i] = (uint8_t)(i ^ 0x5Au);
        big_payload_b[i] = (uint8_t)(i * 7u + 3u);
    }
    const size_t big_len = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, big_payload_a,
                                      (uint16_t)sizeof(big_payload_a));
    memcpy(two_frames, frame, big_len);
    const size_t big_len2 = make_frame(frame, DAP_TCP_FRAME_TYPE_REQUEST, big_payload_b,
                                       (uint16_t)sizeof(big_payload_b));
    memcpy(two_frames + big_len, frame, big_len2);
    check_u32("two maximum-size frames concatenate to 2064 bytes",
              (uint32_t)(big_len + big_len2), 2064u);
    dap_tcp_parser_init(&parser);
    used = 0;
    const int first_ok =
        dap_tcp_parser_feed(&parser, two_frames, big_len + big_len2, &used) ==
            DAP_TCP_PARSE_FRAME_READY &&
        used == big_len && dap_tcp_parser_payload_len(&parser) == DAP_TCP_MAX_PAYLOAD &&
        memcmp(dap_tcp_parser_payload(&parser), big_payload_a, sizeof(big_payload_a)) == 0;
    check("first maximum-size frame limited to 1032 bytes", first_ok, NULL);
    dap_tcp_parser_consume(&parser);
    used = 0;
    const int second_ok =
        dap_tcp_parser_feed(&parser, two_frames + big_len, big_len2, &used) ==
            DAP_TCP_PARSE_FRAME_READY &&
        used == big_len2 && dap_tcp_parser_payload_len(&parser) == DAP_TCP_MAX_PAYLOAD &&
        memcmp(dap_tcp_parser_payload(&parser), big_payload_b, sizeof(big_payload_b)) == 0;
    check("second maximum-size frame recovered", second_ok, NULL);
}

/* ------------------------------------------------------------------------- */
/* 4. Deterministic random chunking over many frames                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint16_t len;
    uint8_t type;
    uint32_t hash;
} frame_meta_t;

static void parser_random_chunks_ok(void)
{
    static uint8_t stream[MAX_FRAMES * DAP_TCP_MAX_FRAME_LEN];
    static frame_meta_t expected[MAX_FRAMES];
    static frame_meta_t seen[MAX_FRAMES];
    static uint8_t payload[DAP_TCP_MAX_PAYLOAD];
    static uint8_t frame[DAP_TCP_MAX_FRAME_LEN];

    printf("deterministic random chunking (%u frames)\n", (unsigned)MAX_FRAMES);

    size_t total = 0;
    size_t last_frame_offset = 0;
    for (unsigned i = 0; i < MAX_FRAMES; i++) {
        uint16_t len = (uint16_t)(1u + (rng_next() % DAP_TCP_MAX_PAYLOAD));
        if (i == 0u) {
            len = DAP_TCP_MAX_PAYLOAD; /* pin the boundary in the fuzz stream */
        }
        if (i == 1u) {
            len = 1u;
        }
        const uint8_t type = (i % 2u) == 0u ? (uint8_t)DAP_TCP_FRAME_TYPE_REQUEST
                                            : (uint8_t)DAP_TCP_FRAME_TYPE_CONTROL_REQUEST;
        for (uint16_t k = 0; k < len; k++) {
            payload[k] = (uint8_t)rng_next();
        }
        const size_t frame_len =
            make_frame(frame, (dap_tcp_frame_type_t)type, payload, len);
        if (frame_len == 0u || total + frame_len > sizeof(stream)) {
            check("fuzz stream built", 0, "frame encoding/stream overflow");
            return;
        }
        if (i == MAX_FRAMES - 1u) {
            last_frame_offset = total;
        }
        memcpy(stream + total, frame, frame_len);
        total += frame_len;

        expected[i].len = len;
        expected[i].type = type;
        expected[i].hash = fnv1a(payload, len);
    }
    check("fuzz stream built", total > 0u, NULL);

    /* Feed random chunk sizes in 1..257 bytes. */
    dap_tcp_parser_t parser;
    dap_tcp_parser_init(&parser);
    size_t off = 0;
    unsigned recovered = 0;
    int saw_error = 0;
    while (off < total) {
        size_t want = 1u + (rng_next() % 257u);
        if (want > total - off) {
            want = total - off;
        }
        size_t used = 0;
        const dap_tcp_parse_result_t rc =
            dap_tcp_parser_feed(&parser, stream + off, want, &used);
        off += used;
        if (rc == DAP_TCP_PARSE_FRAME_READY) {
            if (recovered < MAX_FRAMES) {
                seen[recovered].len = dap_tcp_parser_payload_len(&parser);
                seen[recovered].type = (uint8_t)dap_tcp_parser_type(&parser);
                seen[recovered].hash =
                    fnv1a(dap_tcp_parser_payload(&parser), seen[recovered].len);
            }
            recovered++;
            dap_tcp_parser_consume(&parser);
        } else if (rc == DAP_TCP_PARSE_ERROR) {
            saw_error = 1;
            break;
        }
    }
    check("random chunking: no protocol error", !saw_error, NULL);
    check_u32("random chunking: frame count", recovered, MAX_FRAMES);

    unsigned mismatches = 0;
    for (unsigned i = 0; i < MAX_FRAMES && i < recovered; i++) {
        if (seen[i].len != expected[i].len || seen[i].type != expected[i].type ||
            seen[i].hash != expected[i].hash) {
            mismatches++;
        }
    }
    check_u32("random chunking: payload/type/order mismatches", mismatches, 0u);

    /* Reproduce a deterministic bad-header case with random chunking: corrupt
     * the last frame's signature and expect an error, not a crash. */
    dap_tcp_parser_init(&parser);
    stream[last_frame_offset] = 'X';
    off = 0;
    saw_error = 0;
    unsigned delivered_before_error = 0;
    while (off < total) {
        size_t want = 1u + (rng_next() % 97u);
        if (want > total - off) {
            want = total - off;
        }
        size_t used = 0;
        const dap_tcp_parse_result_t rc =
            dap_tcp_parser_feed(&parser, stream + off, want, &used);
        off += used;
        if (rc == DAP_TCP_PARSE_FRAME_READY) {
            delivered_before_error++;
            dap_tcp_parser_consume(&parser);
        } else if (rc == DAP_TCP_PARSE_ERROR) {
            saw_error = 1;
            break;
        }
    }
    check("corrupted signature detected", saw_error, NULL);
    check_u32("all frames before the corrupted one were delivered",
              delivered_before_error, MAX_FRAMES - 1u);
}

int main(void)
{
    printf("dap_tcp_frame host tests\n");

    header_codec_ok();
    encode_frame_ok();
    parser_basics_ok();
    parser_split_ok();
    parser_sticky_ok();
    parser_invalid_ok();
    parser_random_chunks_ok();

    printf("\n%u checks, %u failure(s)\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
