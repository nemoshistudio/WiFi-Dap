/*
 * SPDX-License-Identifier: MIT
 *
 * CMSIS-DAP over TCP frame codec and streaming parser.
 *
 * Wire format and the framing rules are documented in dap_tcp_frame.h and
 * docs/dap-tcp-backend.md.  This file is pure C on purpose: no ESP-IDF, no
 * lwIP, no sockets, so components/dap_tcp/test/host/run.sh can exercise every
 * rule on the build host.
 */
#include "dap_tcp_frame.h"

#include <string.h>

const char *dap_tcp_decode_result_str(dap_tcp_decode_result_t result)
{
    switch (result) {
    case DAP_TCP_DECODE_OK:
        return "ok";
    case DAP_TCP_DECODE_ERR_MAGIC:
        return "bad signature (expected \"DAP\\0\")";
    case DAP_TCP_DECODE_ERR_TYPE:
        return "wrong frame type";
    case DAP_TCP_DECODE_ERR_LENGTH:
        return "payload length not in 1..1024";
    default:
        return "unknown";
    }
}

void dap_tcp_encode_header(uint8_t *out, dap_tcp_frame_type_t type, uint16_t payload_len)
{
    out[0] = (uint8_t)'D';
    out[1] = (uint8_t)'A';
    out[2] = (uint8_t)'P';
    out[3] = 0u;
    out[DAP_TCP_OFF_LENGTH] = (uint8_t)(payload_len & 0xFFu);
    out[DAP_TCP_OFF_LENGTH + 1u] = (uint8_t)(payload_len >> 8);
    out[DAP_TCP_OFF_TYPE] = (uint8_t)type;
    out[DAP_TCP_OFF_RESERVED] = 0u;
}

size_t dap_tcp_encode_frame(uint8_t *out, size_t cap, dap_tcp_frame_type_t type,
                            const uint8_t *payload, uint16_t payload_len)
{
    if (out == NULL || payload_len == 0u || payload_len > DAP_TCP_MAX_PAYLOAD) {
        return 0u;
    }
    if (payload == NULL) {
        return 0u;
    }
    const size_t total = (size_t)DAP_TCP_HEADER_LEN + (size_t)payload_len;
    if (cap < total) {
        return 0u;
    }
    dap_tcp_encode_header(out, type, payload_len);
    memcpy(out + DAP_TCP_HEADER_LEN, payload, (size_t)payload_len);
    return total;
}

/*
 * Shared header validation.  want_request selects the accepted direction:
 *   probe receive: types 1 (DAP request) and 3 (control request)
 *   host receive:  types 2 (DAP response) and 4 (control response)
 * The reserved byte is intentionally not checked: the format reserves it for
 * future use, both reference implementations write 0 and ignore it on receive.
 */
static dap_tcp_decode_result_t decode_header(const uint8_t *hdr, uint16_t max_payload,
                                             bool want_request,
                                             dap_tcp_frame_header_t *out)
{
    if (hdr[0] != (uint8_t)'D' || hdr[1] != (uint8_t)'A' || hdr[2] != (uint8_t)'P' ||
        hdr[3] != 0u) {
        return DAP_TCP_DECODE_ERR_MAGIC;
    }

    const uint8_t type = hdr[DAP_TCP_OFF_TYPE];
    const bool is_request =
        (type == (uint8_t)DAP_TCP_FRAME_TYPE_REQUEST ||
         type == (uint8_t)DAP_TCP_FRAME_TYPE_CONTROL_REQUEST);
    const bool is_response =
        (type == (uint8_t)DAP_TCP_FRAME_TYPE_RESPONSE ||
         type == (uint8_t)DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE);
    if (want_request != is_request || (!want_request && !is_response)) {
        return DAP_TCP_DECODE_ERR_TYPE;
    }

    const uint16_t length =
        (uint16_t)((uint16_t)hdr[DAP_TCP_OFF_LENGTH] |
                   ((uint16_t)hdr[DAP_TCP_OFF_LENGTH + 1u] << 8));
    if (length == 0u || length > max_payload) {
        return DAP_TCP_DECODE_ERR_LENGTH;
    }

    out->type = (dap_tcp_frame_type_t)type;
    out->length = length;
    return DAP_TCP_DECODE_OK;
}

dap_tcp_decode_result_t dap_tcp_decode_request_header(const uint8_t *hdr,
                                                      uint16_t max_payload,
                                                      dap_tcp_frame_header_t *out)
{
    return decode_header(hdr, max_payload, true, out);
}

dap_tcp_decode_result_t dap_tcp_decode_response_header(const uint8_t *hdr,
                                                       uint16_t max_payload,
                                                       dap_tcp_frame_header_t *out)
{
    return decode_header(hdr, max_payload, false, out);
}

bool dap_tcp_is_request_type(dap_tcp_frame_type_t type)
{
    return type == DAP_TCP_FRAME_TYPE_REQUEST ||
           type == DAP_TCP_FRAME_TYPE_CONTROL_REQUEST;
}

dap_tcp_frame_type_t dap_tcp_response_type_of(dap_tcp_frame_type_t request_type)
{
    switch (request_type) {
    case DAP_TCP_FRAME_TYPE_REQUEST:
        return DAP_TCP_FRAME_TYPE_RESPONSE;
    case DAP_TCP_FRAME_TYPE_CONTROL_REQUEST:
        return DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE;
    default:
        return DAP_TCP_FRAME_TYPE_INVALID;
    }
}

/* ------------------------------------------------------------------------- */
/* Streaming parser                                                          */
/* ------------------------------------------------------------------------- */

/* Total bytes of the frame being assembled (header + expected payload). */
static size_t parser_expected_total(const dap_tcp_parser_t *parser)
{
    return (size_t)DAP_TCP_HEADER_LEN + (size_t)parser->payload_len;
}

void dap_tcp_parser_init(dap_tcp_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
}

dap_tcp_parse_result_t dap_tcp_parser_feed(dap_tcp_parser_t *parser, const uint8_t *data,
                                           size_t len, size_t *consumed)
{
    size_t used = 0u;

    for (;;) {
        /* Validate the header as soon as its 8 bytes are buffered. */
        if (parser->header_valid == 0u && (size_t)parser->len >= (size_t)DAP_TCP_HEADER_LEN) {
            dap_tcp_frame_header_t hdr;
            const dap_tcp_decode_result_t rc =
                dap_tcp_decode_request_header(parser->buf, DAP_TCP_MAX_PAYLOAD, &hdr);
            if (rc != DAP_TCP_DECODE_OK) {
                parser->error = rc;
                *consumed = used;
                return DAP_TCP_PARSE_ERROR;
            }
            parser->header_valid = 1u;
            parser->type = (uint8_t)hdr.type;
            parser->payload_len = hdr.length;
        }

        /* Complete frame buffered?  Leave the bytes in place for the accessors
         * until the caller consumes them. */
        if (parser->header_valid != 0u && (size_t)parser->len >= parser_expected_total(parser)) {
            *consumed = used;
            return DAP_TCP_PARSE_FRAME_READY;
        }

        if (used >= len) {
            *consumed = used;
            return DAP_TCP_PARSE_NEED_MORE;
        }

        /* Copy only the bytes still missing for the current step: either the
         * 8-byte header or the payload announced by a valid header.  That keeps
         * the parser from swallowing the start of the next pipelined frame:
         * consumed bytes always end exactly at a frame boundary. */
        const size_t needed = parser->header_valid != 0u
                                  ? parser_expected_total(parser) - (size_t)parser->len
                                  : (size_t)DAP_TCP_HEADER_LEN - (size_t)parser->len;
        size_t take = len - used;
        if (take > needed) {
            take = needed;
        }
        const size_t space = sizeof(parser->buf) - (size_t)parser->len;
        if (take > space) {
            /* Defensive: a validated header can never require more than
             * DAP_TCP_MAX_FRAME_LEN, so this cannot happen normally. */
            take = space;
        }
        if (take == 0u) {
            parser->error = DAP_TCP_DECODE_ERR_LENGTH;
            *consumed = used;
            return DAP_TCP_PARSE_ERROR;
        }
        memcpy(parser->buf + parser->len, data + used, take);
        parser->len = (uint16_t)((size_t)parser->len + take);
        used += take;
    }
}

const uint8_t *dap_tcp_parser_payload(const dap_tcp_parser_t *parser)
{
    return parser->buf + DAP_TCP_HEADER_LEN;
}

uint16_t dap_tcp_parser_payload_len(const dap_tcp_parser_t *parser)
{
    return parser->header_valid != 0u ? parser->payload_len : 0u;
}

dap_tcp_frame_type_t dap_tcp_parser_type(const dap_tcp_parser_t *parser)
{
    return parser->header_valid != 0u ? (dap_tcp_frame_type_t)parser->type
                                      : DAP_TCP_FRAME_TYPE_INVALID;
}

dap_tcp_decode_result_t dap_tcp_parser_error(const dap_tcp_parser_t *parser)
{
    return parser->error;
}

void dap_tcp_parser_consume(dap_tcp_parser_t *parser)
{
    if (parser->header_valid == 0u) {
        return;
    }
    const size_t total = parser_expected_total(parser);
    if ((size_t)parser->len < total) {
        return; /* frame not complete yet */
    }
    if ((size_t)parser->len > total) {
        memmove(parser->buf, parser->buf + total, (size_t)parser->len - total);
    }
    parser->len = (uint16_t)((size_t)parser->len - total);
    parser->header_valid = 0u;
    parser->payload_len = 0u;
    parser->type = 0u;
}
