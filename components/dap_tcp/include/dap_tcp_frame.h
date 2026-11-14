/*
 * SPDX-License-Identifier: MIT
 *
 * CMSIS-DAP over TCP frame codec and streaming parser (pure C, host-testable).
 *
 * Wire format -- the one used by OpenOCD's `cmsis-dap backend tcp`
 * (src/jtag/drivers/cmsis_dap_tcp.c) and by the XRDAP probes' TCP port 3241
 * (XRDAP-Core docs/protocol.md section 2):
 *
 *   "DAP\0"(4) + length(2, little-endian) + type(1) + reserved(1) + payload(length)
 *
 *   offset 0..3  magic  'D' 'A' 'P' 0x00
 *   offset 4..5  payload length, 1..1024, little-endian
 *   offset 6     frame type (1 request, 2 response, 3 control request,
 *                4 control response)
 *   offset 7     reserved, the probe writes 0 and ignores it on receive
 *
 * A probe accepts request types 1 and 3 only.  A bad signature, any other
 * type, a zero length or a length above the advertised DAP packet size is a
 * protocol error: the connection is closed, there is no resynchronisation (a
 * TCP stream carries no byte stuffing and the peer reconnects).
 *
 * This module has no ESP-IDF, lwIP or socket dependency so the framing rules
 * are regression-tested on the build host:
 *   components/dap_tcp/test/host/run.sh
 */
#ifndef DAP_TCP_FRAME_H
#define DAP_TCP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Frame header bytes: "DAP\0" + u16 length + u8 type + u8 reserved. */
#define DAP_TCP_HEADER_LEN 8u
/** Signature bytes at the start of every frame. */
#define DAP_TCP_MAGIC_LEN 4u
/** Offset of the little-endian payload length field. */
#define DAP_TCP_OFF_LENGTH 4u
/** Offset of the frame type byte. */
#define DAP_TCP_OFF_TYPE 6u
/** Offset of the reserved byte. */
#define DAP_TCP_OFF_RESERVED 7u

/**
 * Largest payload accepted or produced on the TCP link.
 *
 * Fixed by the two peers this backend targets:
 *  - OpenOCD's TCP backend allocates CMSIS_DAP_PACKET_SIZE = 1024 bytes for
 *    one CMSIS-DAP packet and treats a larger peer response as fatal.
 *  - XRDAP-Core rejects TCP frames whose length is 0 or above 1024.
 *
 * The CMSIS-DAP engine's own DAP_PACKET_SIZE may be larger on the ESP32-S3
 * (1280 by default); the integration must cap what it emits on this link to
 * DAP_TCP_MAX_PAYLOAD (docs/dap-tcp-backend.md section 7.3).
 */
#define DAP_TCP_MAX_PAYLOAD 1024u
/** Largest complete frame: header + maximum payload. */
#define DAP_TCP_MAX_FRAME_LEN (DAP_TCP_HEADER_LEN + DAP_TCP_MAX_PAYLOAD)

/** Frame type byte values (XRDAP-Core tcp_frame.hpp / OpenOCD packet types). */
typedef enum {
    DAP_TCP_FRAME_TYPE_INVALID = 0,          /**< Not a valid frame type. */
    DAP_TCP_FRAME_TYPE_REQUEST = 1,          /**< CMSIS-DAP request packet. */
    DAP_TCP_FRAME_TYPE_RESPONSE = 2,         /**< CMSIS-DAP response packet. */
    DAP_TCP_FRAME_TYPE_CONTROL_REQUEST = 3,  /**< XR control request (section 3.7.1). */
    DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE = 4, /**< XR control response (section 3.7.1). */
} dap_tcp_frame_type_t;

/** Header decode failures; the string form is used in server logs. */
typedef enum {
    DAP_TCP_DECODE_OK = 0,
    DAP_TCP_DECODE_ERR_MAGIC,  /**< Signature is not "DAP\0". */
    DAP_TCP_DECODE_ERR_TYPE,   /**< Wrong direction (e.g. response at a probe). */
    DAP_TCP_DECODE_ERR_LENGTH, /**< Payload length 0 or above the limit. */
} dap_tcp_decode_result_t;

/** A decoded 8-byte header. */
typedef struct {
    dap_tcp_frame_type_t type; /**< Frame type (data/control, request/response). */
    uint16_t length;           /**< Payload bytes that follow the header. */
} dap_tcp_frame_header_t;

/**
 * Human-readable decode failure for logs and test diagnostics.
 *
 * @param result decode result; an unknown value yields "unknown".
 * @return static string, never NULL.
 */
const char *dap_tcp_decode_result_str(dap_tcp_decode_result_t result);

/**
 * @brief Write an 8-byte frame header.
 *
 * Layout-only helper: it does not validate @p payload_len.  Callers pass a
 * length that already passed the protocol limit (1..DAP_TCP_MAX_PAYLOAD).
 *
 * @param out         at least DAP_TCP_HEADER_LEN writable bytes.
 * @param type        frame type to store.
 * @param payload_len payload bytes that will follow the header.
 */
void dap_tcp_encode_header(uint8_t *out, dap_tcp_frame_type_t type, uint16_t payload_len);

/**
 * @brief Write one complete frame (header + payload) into a caller buffer.
 *
 * @param out         destination buffer.
 * @param cap         destination capacity in bytes.
 * @param type        frame type to store.
 * @param payload     payload bytes; must not be NULL.
 * @param payload_len payload length, must be 1..DAP_TCP_MAX_PAYLOAD.
 * @return total frame bytes written, or 0 when the arguments cannot form a
 *         valid frame (too small @p cap, length 0 or above the limit).
 */
size_t dap_tcp_encode_frame(uint8_t *out, size_t cap, dap_tcp_frame_type_t type,
                            const uint8_t *payload, uint16_t payload_len);

/**
 * @brief Decode a header received by a probe (request types only).
 *
 * Accepts DAP_TCP_FRAME_TYPE_REQUEST and DAP_TCP_FRAME_TYPE_CONTROL_REQUEST.
 *
 * @param hdr         the received DAP_TCP_HEADER_LEN bytes.
 * @param max_payload peer packet-size limit; the length must be 1..max_payload.
 * @param out         decoded header on DAP_TCP_DECODE_OK.
 * @return DAP_TCP_DECODE_OK, or the first violation found.
 */
dap_tcp_decode_result_t dap_tcp_decode_request_header(const uint8_t *hdr,
                                                      uint16_t max_payload,
                                                      dap_tcp_frame_header_t *out);

/**
 * @brief Decode a header received by a host (response types only).
 *
 * Accepts DAP_TCP_FRAME_TYPE_RESPONSE and DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE.
 * Used by host-side tooling and by the framing tests; the probe itself only
 * decodes request headers.
 *
 * @param hdr         the received DAP_TCP_HEADER_LEN bytes.
 * @param max_payload peer packet-size limit; the length must be 1..max_payload.
 * @param out         decoded header on DAP_TCP_DECODE_OK.
 * @return DAP_TCP_DECODE_OK, or the first violation found.
 */
dap_tcp_decode_result_t dap_tcp_decode_response_header(const uint8_t *hdr,
                                                       uint16_t max_payload,
                                                       dap_tcp_frame_header_t *out);

/**
 * @brief Is this frame type sent by a peer (request side)?
 *
 * @return true for DAP_TCP_FRAME_TYPE_REQUEST / CONTROL_REQUEST.
 */
bool dap_tcp_is_request_type(dap_tcp_frame_type_t type);

/**
 * @brief Response type that answers a request type.
 *
 * @return DAP_TCP_FRAME_TYPE_RESPONSE for a REQUEST,
 *         DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE for a CONTROL_REQUEST,
 *         DAP_TCP_FRAME_TYPE_INVALID for anything else.
 */
dap_tcp_frame_type_t dap_tcp_response_type_of(dap_tcp_frame_type_t request_type);

/** Streaming parser result. */
typedef enum {
    /** Input was consumed; feed more bytes to finish the current frame. */
    DAP_TCP_PARSE_NEED_MORE = 0,
    /** One complete frame is buffered: read it with the accessors, then
     *  call dap_tcp_parser_consume(). */
    DAP_TCP_PARSE_FRAME_READY = 1,
    /** Invalid header; the connection must be closed. dap_tcp_parser_error()
     *  names the violation. */
    DAP_TCP_PARSE_ERROR = 2,
} dap_tcp_parse_result_t;

/**
 * Streaming frame parser: turns a byte stream into complete frames.
 *
 * The parser owns a DAP_TCP_MAX_FRAME_LEN buffer, so a frame split across any
 * number of reads (half packet) and several frames arriving in one read
 * (sticky / pipelined packets) are both handled without dynamic memory.
 *
 * Usage:
 * @code
 *   dap_tcp_parser_t parser;
 *   dap_tcp_parser_init(&parser);
 *   size_t off = 0;
 *   while (off < got) {
 *       size_t used = 0;
 *       dap_tcp_parse_result_t rc =
 *           dap_tcp_parser_feed(&parser, rx + off, got - off, &used);
 *       off += used;
 *       if (rc == DAP_TCP_PARSE_FRAME_READY) {
 *           handle(dap_tcp_parser_type(&parser),
 *                  dap_tcp_parser_payload(&parser),
 *                  dap_tcp_parser_payload_len(&parser));
 *           dap_tcp_parser_consume(&parser);
 *       } else if (rc == DAP_TCP_PARSE_ERROR) {
 *           close_connection();   // no resynchronisation
 *           break;
 *       } else {
 *           break;                // NEED_MORE: every byte was consumed
 *       }
 *   }
 * @endcode
 *
 * @note Not thread-safe.  The server uses one instance in its single client
 *       task; a future multi-client design needs one parser per session.
 */
typedef struct {
    uint8_t buf[DAP_TCP_MAX_FRAME_LEN]; /**< Header + payload accumulation area. */
    uint16_t len;                       /**< Bytes buffered for the current frame. */
    uint16_t payload_len;               /**< Expected payload once the header is valid. */
    uint8_t header_valid;               /**< 1 once the 8-byte header decoded OK. */
    uint8_t type;                       /**< Decoded frame type while header_valid. */
    dap_tcp_decode_result_t error;      /**< Set when the last feed returned ERROR. */
} dap_tcp_parser_t;

/**
 * @brief Reset a parser to the start of a frame (also clears a sticky error).
 *
 * @param parser parser to initialise; must not be NULL.
 */
void dap_tcp_parser_init(dap_tcp_parser_t *parser);

/**
 * @brief Feed received bytes into the parser.
 *
 * The parser takes at most the bytes needed to complete the next frame;
 * bytes beyond that stay in the caller's buffer and must be presented again
 * after the frame is consumed.  When DAP_TCP_PARSE_FRAME_READY is returned,
 * call dap_tcp_parser_consume() before feeding again.
 *
 * @param parser   parser initialised with dap_tcp_parser_init().
 * @param data     received bytes; may be NULL when @p len is 0.
 * @param len      number of received bytes.
 * @param consumed receives the number of bytes taken from @p data (never more
 *                 than @p len); must not be NULL.
 * @return DAP_TCP_PARSE_FRAME_READY when a frame is buffered,
 *         DAP_TCP_PARSE_ERROR on an invalid header,
 *         DAP_TCP_PARSE_NEED_MORE otherwise (all @p len bytes consumed).
 */
dap_tcp_parse_result_t dap_tcp_parser_feed(dap_tcp_parser_t *parser, const uint8_t *data,
                                           size_t len, size_t *consumed);

/**
 * @brief Payload of the buffered frame (valid after DAP_TCP_PARSE_FRAME_READY).
 *
 * @return pointer into the parser buffer, never NULL.
 */
const uint8_t *dap_tcp_parser_payload(const dap_tcp_parser_t *parser);

/**
 * @brief Payload length of the buffered frame (0 before a frame is ready).
 */
uint16_t dap_tcp_parser_payload_len(const dap_tcp_parser_t *parser);

/**
 * @brief Frame type of the buffered frame (INVALID before a frame is ready).
 */
dap_tcp_frame_type_t dap_tcp_parser_type(const dap_tcp_parser_t *parser);

/**
 * @brief Last decode error (meaningful after DAP_TCP_PARSE_ERROR).
 */
dap_tcp_decode_result_t dap_tcp_parser_error(const dap_tcp_parser_t *parser);

/**
 * @brief Drop the buffered frame and prepare for the next one.
 *
 * No-op unless a complete frame is buffered.  Bytes of a following partial
 * frame are kept.
 *
 * @param parser parser that returned DAP_TCP_PARSE_FRAME_READY.
 */
void dap_tcp_parser_consume(dap_tcp_parser_t *parser);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DAP_TCP_FRAME_H */
