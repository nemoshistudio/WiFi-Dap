/*
 * SPDX-License-Identifier: MIT
 *
 * CMSIS-DAP over TCP server skeleton (M3, TCP 3241, default off).
 *
 * One debugger at a time; frames are the OpenOCD `cmsis-dap backend tcp` /
 * XRDAP format decoded by dap_tcp_frame.c.  The server owns the socket and the
 * session loop only: request packets are handed to the weak hook
 * birdiebot_dap_tcp_execute(), which the default build does not implement
 * (the connection is closed with a warning instead of touching the DAP engine).
 *
 * The DAP engine must be owned exclusively by whoever implements the hook:
 * CONFIG_BIRDIEBOT_DAP_TCP_ENABLE is off by default and the wiring steps are in
 * docs/dap-tcp-backend.md section 7.
 *
 * With CONFIG_BIRDIEBOT_DAP_TCP_ENABLE=n this file compiles to the stubs so
 * dap_tcp_server_*() can be called unconditionally.
 */
#include <sdkconfig.h>

#include "dap_tcp_server.h"

#if defined(CONFIG_BIRDIEBOT_DAP_TCP_ENABLE) && CONFIG_BIRDIEBOT_DAP_TCP_ENABLE

#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "esp_log.h"

#include "dap_tcp_frame.h"

/*
 * The server runs one client at a time, so the parser and the buffers are
 * per-server statics instead of per-connection heap allocations: the frame
 * parser already needs ~1 KiB and the response area another ~1 KiB, which is
 * significant on the smaller targets.  Ownership: only dap_tcp_server_task()
 * touches them.
 */
static dap_tcp_parser_t s_parser;
static uint8_t s_rx[DAP_TCP_MAX_FRAME_LEN];
static uint8_t s_tx[DAP_TCP_MAX_FRAME_LEN];

static TaskHandle_t s_task;
static volatile bool s_client_connected;
static bool s_running;

#define DAP_TCP_TAG "dap_tcp"

/* While bind() or accept() fails (network stack still coming up, transient
 * lwIP error) retry instead of killing the only server task. */
#define DAP_TCP_RETRY_MS 2000
/* A connected debugger that stops reading must not block the only session
 * forever; a blocked response write is bounded by this send timeout. */
#define DAP_TCP_SEND_TIMEOUT_S 10
/* Keepalive for a peer that vanishes without FIN (Wi-Fi drop, host crash):
 * probe after 5 s idle, every 2 s, drop after 3 misses (about 11 s), so the
 * single client slot is released without waiting for the lwIP default. */
#define DAP_TCP_KEEP_IDLE_S 5
#define DAP_TCP_KEEP_INTVL_S 2
#define DAP_TCP_KEEP_CNT 3
#define DAP_TCP_ACCEPT_BACKLOG 1
#define DAP_TCP_CLIENT_ADDR_STR 32

/* ------------------------------------------------------------------------- */
/* Weak integration hooks (no DAP engine in the skeleton)                    */
/* ------------------------------------------------------------------------- */

__attribute__((weak)) int birdiebot_dap_tcp_execute(const uint8_t *request,
                                                    uint16_t request_len,
                                                    uint8_t *response,
                                                    uint16_t response_cap,
                                                    uint16_t *response_len)
{
    (void)request;
    (void)request_len;
    (void)response;
    (void)response_cap;
    (void)response_len;
    /* No DAP bridge is wired into this build: report "not handled" so the
     * server closes the connection instead of sending a bogus response. */
    return -1;
}

__attribute__((weak)) void birdiebot_dap_tcp_link_changed(bool connected)
{
    (void)connected;
}

/* ------------------------------------------------------------------------- */
/* Socket helpers                                                            */
/* ------------------------------------------------------------------------- */

/* Listen on INADDR_ANY:port.  Returns the listening fd or -1 (errno set). */
static int open_listener(uint16_t port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, DAP_TCP_ACCEPT_BACKLOG) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

/* Per-client socket options.  Failures are not fatal (the session still works
 * with defaults), so they are intentionally ignored. */
static void configure_client(int fd)
{
    int one = 1;

    /* CMSIS-DAP request/response packets are tiny; Nagle + delayed ACK would
     * add tens of milliseconds to every SWD round-trip, so disable Nagle. */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    {
        int idle = DAP_TCP_KEEP_IDLE_S;
        int intvl = DAP_TCP_KEEP_INTVL_S;
        int cnt = DAP_TCP_KEEP_CNT;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    }
#endif

#if defined(SO_SNDTIMEO)
    {
        struct timeval send_timeout;
        send_timeout.tv_sec = DAP_TCP_SEND_TIMEOUT_S;
        send_timeout.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    }
#endif
}

/* Send exactly len bytes; false when the peer is gone / the send timed out. */
static bool send_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        const int n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Frame handling                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Handle the frame currently buffered in s_parser.
 *
 * Returns true to keep the session, false to close the connection.
 */
static bool handle_frame(int fd)
{
    const dap_tcp_frame_type_t type = dap_tcp_parser_type(&s_parser);
    const uint8_t *request = dap_tcp_parser_payload(&s_parser);
    const uint16_t request_len = dap_tcp_parser_payload_len(&s_parser);
    uint16_t response_len = 0;
    dap_tcp_frame_type_t response_type = DAP_TCP_FRAME_TYPE_INVALID;

    if (type == DAP_TCP_FRAME_TYPE_REQUEST) {
        /* Leave DAP_TCP_HEADER_LEN bytes in front of the payload for the
         * response header: one send() call per response. */
        const int rc = birdiebot_dap_tcp_execute(request, request_len,
                                                 s_tx + DAP_TCP_HEADER_LEN,
                                                 DAP_TCP_MAX_PAYLOAD, &response_len);
        if (rc != 0) {
            ESP_LOGW(DAP_TCP_TAG,
                     "no DAP bridge wired (birdiebot_dap_tcp_execute returned %d); "
                     "closing the client",
                     rc);
            return false;
        }
        if (response_len == 0) {
            /* Command without a response packet (e.g. DAP_TransferAbort 0x07):
             * no TCP frame goes back, the session stays open. */
            return true;
        }
        if (response_len > DAP_TCP_MAX_PAYLOAD) {
            ESP_LOGE(DAP_TCP_TAG, "DAP response too large (%u > %u bytes); closing the client",
                     (unsigned)response_len, (unsigned)DAP_TCP_MAX_PAYLOAD);
            return false;
        }
        response_type = DAP_TCP_FRAME_TYPE_RESPONSE;
    } else {
        /* Control request (frame type 3): this firmware has no XR control
         * port, so answer "op + 0xF2 (unsupported)" as specified in
         * XRDAP-Core docs/protocol.md section 3.7.1.  The parser guarantees
         * request_len >= 1. */
        s_tx[DAP_TCP_HEADER_LEN] = request[0];
        s_tx[DAP_TCP_HEADER_LEN + 1u] = 0xF2u;
        response_len = 2;
        response_type = DAP_TCP_FRAME_TYPE_CONTROL_RESPONSE;
    }

    dap_tcp_encode_header(s_tx, response_type, response_len);
    if (!send_all(fd, s_tx, (size_t)DAP_TCP_HEADER_LEN + (size_t)response_len)) {
        ESP_LOGW(DAP_TCP_TAG, "response send failed (errno %d); closing the client", errno);
        return false;
    }
    return true;
}

/*
 * Serve one connected debugger until it disconnects or the stream is invalid.
 * Frames may arrive split across reads (half packet) or several per read
 * (sticky packets); dap_tcp_parser_feed() handles both.
 */
static void serve_client(int fd)
{
    dap_tcp_parser_init(&s_parser);

    for (;;) {
        const int n = recv(fd, s_rx, sizeof(s_rx), 0);
        if (n == 0) {
            return; /* peer closed the connection */
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(DAP_TCP_TAG, "recv failed (errno %d); closing the client", errno);
            return;
        }

        size_t off = 0;
        while (off < (size_t)n) {
            size_t used = 0;
            const dap_tcp_parse_result_t rc =
                dap_tcp_parser_feed(&s_parser, s_rx + off, (size_t)n - off, &used);
            off += used;

            if (rc == DAP_TCP_PARSE_FRAME_READY) {
                if (!handle_frame(fd)) {
                    return;
                }
                dap_tcp_parser_consume(&s_parser);
            } else if (rc == DAP_TCP_PARSE_ERROR) {
                /* Bad signature / type / length: close, never resynchronise. */
                ESP_LOGW(DAP_TCP_TAG, "invalid frame header (%s); closing the client",
                         dap_tcp_decode_result_str(dap_tcp_parser_error(&s_parser)));
                return;
            } else {
                break; /* NEED_MORE: every byte of this read was consumed */
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Server task / public API                                                  */
/* ------------------------------------------------------------------------- */

static void dap_tcp_server_task(void *arg)
{
    (void)arg;
    const uint16_t port = (uint16_t)CONFIG_BIRDIEBOT_DAP_TCP_PORT;

    for (;;) {
        const int listen_fd = open_listener(port);
        if (listen_fd < 0) {
            ESP_LOGW(DAP_TCP_TAG, "listen on port %u failed (errno %d); retrying in %d ms",
                     (unsigned)port, errno, DAP_TCP_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(DAP_TCP_RETRY_MS));
            continue;
        }
        ESP_LOGI(DAP_TCP_TAG, "CMSIS-DAP over TCP listening on port %u (single client)",
                 (unsigned)port);

        for (;;) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            const int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ESP_LOGW(DAP_TCP_TAG, "accept failed (errno %d); re-opening the listener",
                         errno);
                break;
            }

            char addr_str[DAP_TCP_CLIENT_ADDR_STR] = "?";
            inet_ntoa_r(peer.sin_addr, addr_str, sizeof(addr_str) - 1);
            ESP_LOGI(DAP_TCP_TAG, "client %s:%u connected", addr_str,
                     (unsigned)ntohs(peer.sin_port));

            configure_client(fd);
            s_client_connected = true;
            birdiebot_dap_tcp_link_changed(true);

            serve_client(fd);

            birdiebot_dap_tcp_link_changed(false);
            s_client_connected = false;
            close(fd);
            ESP_LOGI(DAP_TCP_TAG, "client disconnected; waiting for the next one");
        }

        close(listen_fd);
        vTaskDelay(pdMS_TO_TICKS(DAP_TCP_RETRY_MS));
    }
}

esp_err_t dap_tcp_server_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(dap_tcp_server_task, "dap_tcp",
                                CONFIG_BIRDIEBOT_DAP_TCP_TASK_STACK, NULL,
                                CONFIG_BIRDIEBOT_DAP_TCP_TASK_PRIORITY, &s_task,
                                CONFIG_BIRDIEBOT_DAP_TCP_CORE) != pdPASS) {
        ESP_LOGE(DAP_TCP_TAG, "task create failed (stack %d bytes)",
                 CONFIG_BIRDIEBOT_DAP_TCP_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    return ESP_OK;
}

bool dap_tcp_server_running(void)
{
    return s_running;
}

bool dap_tcp_server_client_connected(void)
{
    return s_client_connected;
}

uint16_t dap_tcp_server_port(void)
{
    return (uint16_t)CONFIG_BIRDIEBOT_DAP_TCP_PORT;
}

#else /* !CONFIG_BIRDIEBOT_DAP_TCP_ENABLE */

/*
 * Feature compiled out (the shipped default): the API stays linkable as no-op
 * stubs so call sites do not need #if guards.  Nothing listens on the port.
 */
esp_err_t dap_tcp_server_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool dap_tcp_server_running(void)
{
    return false;
}

bool dap_tcp_server_client_connected(void)
{
    return false;
}

uint16_t dap_tcp_server_port(void)
{
    return 0;
}

#endif /* CONFIG_BIRDIEBOT_DAP_TCP_ENABLE */
