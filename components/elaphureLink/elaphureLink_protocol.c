/*
 * elaphureLink protocol (WP-108 merge of the A 2025 fork onto the IDF5 C tree).
 *
 * Protocol behaviour is preserved from A:
 *   - handshake (EL_LINK_IDENTIFIER + EL_COMMAND_HANDSHAKE) and version reply;
 *   - 0x88 vendor command channel: EL_NATIVE_COMMAND_PASSTHROUGH (raw CMSIS-DAP
 *     command forwarded to DAP_ExecuteCommand) and EL_VENDOR_SCOPE_ENTER/EXIT
 *     async scoping (OpenOCD-elaphureLink);
 *   - TCP length-field reassembly for 0x88 packets.
 *
 * Adaptations for this repository:
 *   - component include layout, explicit stdbool/stdlib/string includes;
 *   - DAP ring buffer lifecycle belongs to the DAP task (WP-112): this TCP-side
 *     module never calls free_dap_ringbuf(); it posts a DELETE_HANDLE reset via
 *     the transport API dap_handle_request_reset()/dap_handle_wait_reset() and
 *     waits (bounded) until the DAP task applied it, so the inline
 *     DAP_ExecuteCommand() calls below cannot race with the USBIP data path.
 *     A weak fallback keeps the file buildable on transport revisions that do
 *     not have the API yet (the DAP task then applies the request later);
 *   - inbound bounds: recv() is bounded by the receive buffer and every length
 *     field is validated before use; the DAP response is bounded by
 *     DAP_PACKET_SIZE (DAP_ExecuteCommand) which fits the process buffer;
 *   - no ESP8266 branch.
 */
#include "components/elaphureLink/elaphureLink_protocol.h"

#include "DAP_handle.h"
#include "main/dap_configuration.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

/* EL_PROCESS_BUFFER_SIZE: elaphureLink's own out/in buffer. Must be large
 * enough for one 0x88 vendor frame (4-byte vendor header + one DAP command
 * response) - keep the A-tree value (1500). */
#define EL_PROCESS_BUFFER_SIZE 1500U
#define EL_PROCESS_BUFFER_MAX_PAYLOAD (EL_PROCESS_BUFFER_SIZE - 4U)
#define EL_HANDSHAKE_SIZE 12U

_Static_assert(EL_PROCESS_BUFFER_SIZE >= (DAP_PACKET_SIZE + 4U),
               "elaphureLink process buffer must fit a vendor header + one DAP packet");

extern int kSock;
extern int usbip_network_send(int s, const void *dataptr, size_t size, int flags);
extern int kRestartDAPHandle; /* legacy (pre-WP-112) DAP task signal */

/* Transport API from WP-112 (main branch). Weak: on a transport revision
 * without it the pointer check falls back to the legacy behaviour above. */
extern void dap_handle_request_reset(enum reset_handle_t mode) __attribute__((weak));
extern int dap_handle_wait_reset(uint32_t timeout_ms) __attribute__((weak));

extern uint32_t DAP_ExecuteCommand(const uint8_t *request, uint8_t *response);

struct el_context {
    bool is_async;
};

static struct el_context k_el_context;
uint8_t* el_process_buffer = NULL;

void el_process_buffer_malloc()
{
    if (el_process_buffer != NULL)
        return;

    el_process_buffer = malloc(EL_PROCESS_BUFFER_SIZE);
    /* callers must handle NULL (no RAM): el_dap_data_process() then drops data */
}

void el_process_buffer_free()
{
    if (el_process_buffer != NULL) {
        free(el_process_buffer);
        el_process_buffer = NULL;
    }
}

int el_handshake_process(int fd, void *buffer, size_t len)
{
    if ((buffer == NULL) || (len != sizeof(el_request_handshake))) {
        return -1;
    }

    el_request_handshake* req = (el_request_handshake*)buffer;

    if (ntohl(req->el_link_identifier) != EL_LINK_IDENTIFIER) {
        return -1;
    }

    if (ntohl(req->command) != EL_COMMAND_HANDSHAKE) {
        return -1;
    }

    el_response_handshake res;
    res.el_link_identifier = htonl(EL_LINK_IDENTIFIER);
    res.command = htonl(EL_COMMAND_HANDSHAKE);
    res.el_dap_version = htonl(EL_DAP_VERSION);

    usbip_network_send(fd, &res, sizeof(el_response_handshake), 0);

    return 0;
}

/* Ask the DAP task to release the USBIP ring buffers before elaphureLink runs
 * DAP commands inline in this (TCP) task. Never free them from here: the DAP
 * task may still be receiving from them (WP-112 ring buffer ownership). */
static void el_acquire_dap_handle(void)
{
    if (dap_handle_request_reset != NULL) {
        dap_handle_request_reset(DELETE_HANDLE);
        if (dap_handle_wait_reset != NULL) {
            /* bounded wait: a slow DAP task must not hang the TCP session */
            (void)dap_handle_wait_reset(1000U);
        }
    } else {
        /* Legacy transport: the DAP task observes DELETE_HANDLE in its own
         * context on the next notification; do not wait and do not free. */
        kRestartDAPHandle = DELETE_HANDLE;
    }
}

void el_dap_data_process(void* buffer, size_t len)
{
    int res;

    if (el_process_buffer == NULL || buffer == NULL || len == 0U ||
        len > EL_PROCESS_BUFFER_SIZE) {
        return;
    }

    res = DAP_ExecuteCommand(buffer, (uint8_t *)el_process_buffer);
    res &= 0xFFFF;

    /* DAP_ExecuteCommand caps the command response at DAP_PACKET_SIZE; the
     * vendor path adds the 4-byte 0x88 status/echo header. Keep one extra
     * safety net in case a handler ever violates that contract. */
    if (res <= 0 || (size_t)res > EL_PROCESS_BUFFER_SIZE) {
        return;
    }

    usbip_network_send(kSock, el_process_buffer, res, 0);
}

static inline int recv_all(int fd, uint8_t *buf, size_t size, int flag)
{
    const size_t total = size;
    int ret;

    if (size == 0)
        return 0;

    do {
        ret = recv(fd, buf, size, flag);
        if (ret <= 0)
            return ret;

        buf += ret;
        size -= ret;
    } while (size);

    return total;
}

/* Reassemble 0x88 vendor frames from the TCP byte stream. Each frame is
 * [0x88][type][len_hi][len_lo][payload...] with a big-endian payload length;
 * a recv() may return a partial frame or several frames at once. */
static int el_vendor_command_pre_process(uint8_t *base, int recved_len)
{
    int offset = 0;
    int payload_len, remain_len, packet_len;
    uint16_t *payload;
    int ret;

    if (recved_len <= 0 || recved_len > (int)EL_PROCESS_BUFFER_SIZE) {
        return -1;
    }

    while (recved_len - offset >= 4) {
        payload = (uint16_t *)(base + offset + 2);
        payload_len = ntohs(*payload);
        if (payload_len > (int)EL_PROCESS_BUFFER_MAX_PAYLOAD) {
            return -1; /* malformed length field */
        }
        packet_len = 4 + payload_len;

        if (offset + packet_len > recved_len)
            break;

        el_dap_data_process(base + offset, (size_t)packet_len);
        offset += packet_len;
    }

    // already process done
    remain_len = recved_len - offset;
    if (remain_len == 0)
        return 1;

    memmove(base, base + offset, (size_t)remain_len);
    if (remain_len < 4) {
        ret = recv(kSock, base + remain_len, (size_t)(4 - remain_len), 0);
        if (ret <= 0)
            return ret;
        offset = 4;
        remain_len = 0;
    } else {
        offset = remain_len;
        remain_len -= 4;
    }

    payload = (uint16_t *)(base + 2);
    payload_len = ntohs(*payload);
    if (payload_len > (int)EL_PROCESS_BUFFER_MAX_PAYLOAD) {
        return -1; /* malformed length field */
    }
    if (payload_len - remain_len > 0) {
        ret = recv(kSock, base + offset, (size_t)(payload_len - remain_len), 0);
        if (ret <= 0)
            return ret;
    }

    el_dap_data_process(base, (size_t)(4 + payload_len));

    return 1;
}

int el_dap_work(uint8_t* base, size_t len)
{
    uint16_t *length, payload_len;
    uint8_t *data;
    int sz, ret;

    /* len is the caller's receive buffer capacity (tcp_server passes 1500). */
    if (base == NULL || len < EL_HANDSHAKE_SIZE || len > EL_PROCESS_BUFFER_SIZE) {
        return -1;
    }

    memset(&k_el_context, 0, sizeof(struct el_context));

    // read command code and protocol version
    data = base + 4;
    sz = 8;
    do {
        ret = recv(kSock, data, (size_t)sz, 0);
        if (ret <= 0)
            return ret;
        sz -= ret;
        data += ret;
    } while (sz > 0);

    ret = el_handshake_process(kSock, base, EL_HANDSHAKE_SIZE);
    if (ret)
        return ret;

    /* DAP task owns the USBIP ring buffers: ask it to release them and wait
     * until that happened before using DAP_ExecuteCommand inline. */
    el_acquire_dap_handle();
    el_process_buffer_malloc();
    if (el_process_buffer == NULL) {
        return -1;
    }

    // data process
    while (1) {
        ret = recv(kSock, base, len, 0);
        if (ret <= 0)
            return ret;
        if ((size_t)ret > len) {
            return -1; /* cannot happen with a well-formed socket call */
        }

        if (*base == EL_VENDOR_COMMAND_PERFIX) {
            ret = el_vendor_command_pre_process(base, ret);
            if (ret <= 0)
                return ret;
        } else {
            el_dap_data_process(base, (size_t)ret);
        }

        if (k_el_context.is_async) {
            do {
                ret = recv_all(kSock, base, 4, 0);
                if (ret <= 0)
                    return ret;
                length = (uint16_t *)(base + 2);
                payload_len = ntohs(*length);
                if (payload_len > (uint16_t)EL_PROCESS_BUFFER_MAX_PAYLOAD) {
                    return -1;
                }

                ret = recv_all(kSock, base + 4, (size_t)payload_len, 0);
                if (ret <= 0)
                    return ret;
                el_dap_data_process(base, (size_t)(4 + payload_len));
            } while (k_el_context.is_async);
        }
    }

    return 0;
}

uint32_t el_native_command_passthrough(const uint8_t *request, uint8_t *response)
{
    /* Response layout expected by the caller: [0x88 echo][status 3 bytes][DAP
     * response]. The caller buffer is not smaller than DAP_PACKET_SIZE on any
     * transport (USBIP/WebSocket), so bound the payload to keep the whole
     * vendor response inside one DAP packet even if a malformed host command
     * would produce a bigger DAP response. Execute into a staging buffer first
     * so a future handler bug cannot overflow the caller's buffer. */
    /* Static to keep 512 bytes off the TCP task stack (the tcp_server task is
     * 4 KB and the DAP command path below it is deep). Only one transport
     * session can run at a time, so this buffer is not re-entered. */
    static uint8_t staging[DAP_PACKET_SIZE];
    int ret;

    request += 2; // skip header (length field)

    ret = DAP_ExecuteCommand(request, staging);
    ret &= 0xFFFF;
    if (ret < 0) {
        ret = 0;
    }
    if (ret > (int)(DAP_PACKET_SIZE - 4U)) {
        ret = (int)(DAP_PACKET_SIZE - 4U);
    }

    response[0] = 0x00; // status
    response[1] = (ret >> 8) & 0xFF;
    response[2] = ret & 0xFF;
    memcpy(response + 3, staging, (size_t)ret);

    /* bytes written by this function: 3 status/length bytes + ret payload
     * bytes (the 0x88 echo byte is counted by DAP_ProcessVendorCommand) */
    return (uint32_t)ret + 4U;
}

uint32_t el_vendor_command(const uint8_t *request, uint8_t *response)
{
    uint8_t type;
    uint32_t ret = 0;
    uint32_t request_count = 0;
    uint32_t response_count = 0;

    type = *request++;

    switch (type) {
    case EL_NATIVE_COMMAND_PASSTHROUGH:
        /* request layout here: [length_hi][length_lo][DAP payload...];
         * total request = 0x88 + type + 2 length bytes + payload */
        {
            uint32_t payload_len = ((uint32_t)request[0] << 8) | (uint32_t)request[1];
            request_count = 4U + payload_len;
        }
        ret = el_native_command_passthrough(request, response);
        response_count = ret;
        break;
    case EL_VENDOR_SCOPE_ENTER:
        memset(&k_el_context, 0, sizeof(struct el_context));
        k_el_context.is_async = true;
        *response++ = 0; // status
        *response++ = 0;
        *response++ = 0;
        request_count = 2U;  // 0x88 + type
        response_count = 4U; // 0x88 echo + 3 status bytes
        break;
    case EL_VENDOR_SCOPE_EXIT:
        k_el_context.is_async = false;
        *response++ = 0; // status
        *response++ = 0;
        *response++ = 0;
        request_count = 2U;
        response_count = 4U;
        break;
    default:
        /* unknown vendor sub-command: echo 0x88 only, consume 0x88 + type */
        request_count = 2U;
        response_count = 1U;
        break;
    }

    return ((request_count & 0xFFFFU) << 16) | (response_count & 0xFFFFU);
}
