/*
Copyright (c) 2015, SuperHouse Automation Pty Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * @file uart_bridge.c
 * @author windowsair
 * @brief UART TCP bridge (TCP 1346) — client of components/uart_service
 * @version 0.3
 * @date 2021-11-16
 *
 * WiFi Dap merge notes
 * ---------------------
 *  - A 2025 implementation of the fork's main/uart_bridge.c, componentized on
 *    the C/IDF5 platform layer (printf / LWIP netconn API), with the
 *    2025 connection-close fix (double netconn_delete) and single event-queue
 *    allocation kept from WP-114.
 *  - WP-203 refactor: this file no longer installs or reads the UART driver
 *    directly.  components/uart_service is the single UART owner (pins/port
 *    from components/board); the bridge
 *      TX: uart_service_write()           (TCP -> target UART)
 *      RX: uart_service_subscribe_rx()    (target UART -> own stream buffer
 *                                          -> drain loop -> TCP client)
 *    That removes the double driver initialization and lets the Web UART
 *    (JSON module 4 + WS binary router) share the same UART simultaneously.
 *  - Port 1346 and CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE (default y) are
 *    unchanged; when the bridge is disabled it does not touch the UART, so the
 *    Web UART keeps working.
 *  - The first packet of a new connection may still be a decimal baud rate.
 *
 * @copyright Copyright (c) 2021
 *
 */

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/netbuf.h"
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/ip.h"

#include "birdiebot_board.h"
#include "uart_service.h"
#include "uart_tcp_bridge.h"

#if CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE

#define EVENTS_QUEUE_SIZE 10
#define UART_BUF_SIZE     512
#define BRIDGE_RX_STREAM_SIZE 2048

#ifdef CALLBACK_DEBUG
#define debug(s, ...) printf("%s: " s "\n", "Cb:", ##__VA_ARGS__)
#else
#define debug(s, ...)
#endif

static const char *UART_TAG = "UART";
#define NETCONN_EVT_WIFI_DISCONNECTED (NETCONN_EVT_ERROR + 1)
/* Bridge-internal event: new bytes are waiting in uart_rx_stream. */
#define BRIDGE_EVT_UART_DATA          (NETCONN_EVT_ERROR + 2)

static QueueHandle_t uart_server_events = NULL;
typedef struct
{
    struct netconn *nc;
    uint8_t type;
} netconn_events;

static uint8_t uart_read_buffer[UART_BUF_SIZE];
/* Target UART -> TCP staging buffer filled by the uart_service RX callback. */
static StreamBufferHandle_t uart_rx_stream = NULL;
static int uart_rx_sub_slot = -1;
// use lwip buffer to write back
static struct netconn *uart_netconn = NULL;
static bool is_conn_valid = false; // lock free
static bool is_first_time_recv = false;

/* A2: pause/resume control (wireless-serial island).  The bridge task is the
 * only owner of s_listen_netconn; the control objects are created by the task
 * at startup, so pause()/resume() called before the task exists simply record
 * the requested state (nothing is listening yet). */
#define BRIDGE_CTL_TIMEOUT_MS 2000

static TaskHandle_t s_task_handle = NULL;
static struct netconn *s_listen_netconn = NULL; /* TCP 1346 listening netconn */
static SemaphoreHandle_t s_ctl_mux = NULL;      /* serializes pause()/resume() */
static SemaphoreHandle_t s_pause_ack = NULL;    /* task -> pause(): bridge idle  */
static SemaphoreHandle_t s_resume_req = NULL;   /* resume() -> task: wake up      */
static SemaphoreHandle_t s_resume_ack = NULL;   /* task -> resume(): listening   */
static volatile bool s_paused = false;
static volatile bool s_resume_listen_ok = false;

void uart_bridge_init(void)
{
    /* Created exactly once (fixes the duplicated queue creation of the A/C
     * tree); safe to call more than once. */
    if (uart_server_events == NULL) {
        uart_server_events = xQueueCreate(EVENTS_QUEUE_SIZE, sizeof(netconn_events));
    }
}

void uart_bridge_close(void)
{
    netconn_events events;

    if (uart_server_events == NULL) {
        return;
    }

    memset(&events, 0, sizeof(events));
    events.type = NETCONN_EVT_WIFI_DISCONNECTED;
    /* Do not block the caller (WiFi event handler) for long: the event only
     * carries "close the current bridge connection". */
    (void)xQueueSend(uart_server_events, &events, pdMS_TO_TICKS(100));
}

static void uart_bridge_reset(void)
{
    uart_netconn = NULL;
    is_conn_valid = false;
    is_first_time_recv = false;

    if (uart_rx_stream != NULL) {
        (void)xStreamBufferReset(uart_rx_stream);
    }
}

static int num_digits(int n) {
    if (n < 10)
        return 1;
    if (n < 100)
        return 2;
    if (n < 1000)
        return 3;
    if (n < 10000)
        return 4;
    if (n < 100000)
        return 5;
    if (n < 1000000)
        return 6;

    return 7;
}

/**
 * RX callback registered with uart_service.  Runs in the uart_service RX task
 * context: copy the data into the bridge stream buffer and wake the bridge
 * task, never block.
 */
static void uart_bridge_uart_rx_cb(uint8_t sub_mod, const uint8_t *data, size_t len, void *arg)
{
    netconn_events events;

    (void)sub_mod;
    (void)arg;

    if (!is_conn_valid || uart_rx_stream == NULL || data == NULL || len == 0) {
        return;
    }

    /* No client / stream full: drop.  The bridge is a best-effort raw pipe and
     * must never apply backpressure to the uart_service RX fan-out. */
    (void)xStreamBufferSend(uart_rx_stream, data, len, 0);

    memset(&events, 0, sizeof(events));
    events.type = BRIDGE_EVT_UART_DATA;
    (void)xQueueSend(uart_server_events, &events, 0);
}

/*
 * This function will be call in Lwip in each event on netconn
 */
static void netCallback(struct netconn *conn, enum netconn_evt evt, uint16_t length) {
    // Show some callback information (debug)
    debug("sock:%lu\tsta:%u\tevt:%u\tlen:%u\ttyp:%u\tfla:%02x\terr:%d",
          (uint32_t)conn, conn->state, evt, length, conn->type, conn->flags, conn->pending_err);

    netconn_events events;

    // If netconn got error, it is close or deleted, dont do treatments on it.
    if (conn->pending_err) {
        return;
    }
    // Treatments only on rcv events.
    switch (evt) {
    case NETCONN_EVT_RCVPLUS:
        events.nc = conn;
        events.type = evt;
        break;
    default:
        return;
        break;
    }

    // Send the event to the queue
    (void)xQueueSend(uart_server_events, &events, pdMS_TO_TICKS(1000));
}

/*
 *  Initialize a server netconn and listen port
 */
static void set_tcp_server_netconn(struct netconn **nc, uint16_t port, netconn_callback callback) {
    if (nc == NULL) {
        ESP_LOGE(UART_TAG, "%s: netconn missing .\n", __FUNCTION__);
        return;
    }
    *nc = netconn_new_with_callback(NETCONN_TCP, netCallback);
    if (!*nc) {
        ESP_LOGE(UART_TAG, "Status monitor: Failed to allocate netconn.\n");
        return;
    }
    /* A2: allow an immediate re-bind of TCP 1346 after pause/resume.  Closing
     * the session puts the previous client connection into TIME_WAIT; without
     * SO_REUSEADDR the lwIP bind would fail with ERR_USE for the whole MSL
     * window and the bridge would stay offline after resume. */
    if ((*nc)->pcb.tcp != NULL) {
        ip_set_option((*nc)->pcb.tcp, SOF_REUSEADDR);
    }
    netconn_set_nonblocking(*nc, NETCONN_FLAG_NON_BLOCKING);
    // netconn_set_recvtimeout(*nc, 10);
    netconn_bind(*nc, IP_ADDR_ANY, port);
    netconn_listen(*nc);
}

/*
 *  Close and delete a socket properly
 */
static void close_tcp_netconn(struct netconn *nc) {
    nc->pending_err = ERR_CLSD; // It is hacky way to be sure than callback will don't do treatment on a netconn closed and deleted
    netconn_close(nc);
    netconn_delete(nc);
}

/**
 * Open (or re-open after resume) the TCP 1346 listening netconn.
 * Returns true on success.  Bounded retries with vTaskDelay(): after a
 * pause/resume cycle the lwIP socket memory may still be reclaimed, so a
 * failing first attempt is retried instead of giving up.
 */
static bool uart_bridge_open_listen(void)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        set_tcp_server_netconn(&s_listen_netconn, BIRDIEBOT_UART_BRIDGE_PORT, netCallback);
        if (s_listen_netconn != NULL) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(UART_TAG, "cannot open TCP %d listener", BIRDIEBOT_UART_BRIDGE_PORT);
    return false;
}

/**
 * Bring up the bridge data path on top of the UART service.
 * The RX subscription is registered before uart_service_init() so that no byte
 * received between initialization and subscription is lost.
 */
static esp_err_t uart_bridge_setup(void)
{
    esp_err_t err;

    if (uart_rx_stream == NULL) {
        uart_rx_stream = xStreamBufferCreate(BRIDGE_RX_STREAM_SIZE, 1);
        if (uart_rx_stream == NULL) {
            ESP_LOGE(UART_TAG, "failed to create UART RX stream buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    if (uart_rx_sub_slot < 0) {
        uart_rx_sub_slot = uart_service_subscribe_rx(uart_bridge_uart_rx_cb, NULL);
        if (uart_rx_sub_slot < 0) {
            ESP_LOGE(UART_TAG, "failed to subscribe to uart_service RX");
            return ESP_ERR_NO_MEM;
        }
    }

    err = uart_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(UART_TAG, "uart_service_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(UART_TAG, "bridge ready: uart%d TX=%d RX=%d port=%d (uart_service)",
             (int)uart_service_port(), uart_service_tx_pin(), uart_service_rx_pin(),
             BIRDIEBOT_UART_BRIDGE_PORT);

    return ESP_OK;
}

/**
 * Forward the staged target UART bytes to the current TCP client.
 * Runs in the bridge task only; a client disconnect drops the rest.
 */
static void uart_bridge_flush_to_tcp(void)
{
    size_t len;

    if (!is_conn_valid || uart_netconn == NULL || uart_rx_stream == NULL) {
        return;
    }

    while ((len = xStreamBufferReceive(uart_rx_stream, uart_read_buffer,
                                       sizeof(uart_read_buffer), 0)) > 0) {
        if (netconn_write(uart_netconn, uart_read_buffer, len, NETCONN_COPY) != ERR_OK) {
            ESP_LOGW(UART_TAG, "netconn_write failed, connection reset");
            break;
        }
    }
}

/* Apply the optional "first packet is the baud rate" convention. */
static bool uart_bridge_maybe_set_baud(const char *buffer, uint16_t len) {
    if (!is_first_time_recv) {
        return false;
    }

    is_first_time_recv = false;

    if (len > 1 && len < 8) {
        char tmp_buff[8];
        memcpy(tmp_buff, buffer, len);
        tmp_buff[len] = '\0';
        int baudrate = atoi(tmp_buff);
        if (baudrate > 0 && baudrate < 2000000 && num_digits(baudrate) == (int)len) {
            ESP_LOGI(UART_TAG, "change baud:%d", baudrate);
            if (uart_service_set_baud((uint32_t)baudrate) != ESP_OK) {
                ESP_LOGW(UART_TAG, "uart_service_set_baud failed");
            }
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------------------------ */
/* A2: pause / resume of the wireless-serial (TCP 1346) island               */
/* ------------------------------------------------------------------------ */

/* Release the listening socket and the current client connection, and drop
 * stale bridge events.  Runs in the bridge task only. */
static void uart_bridge_release_session(void)
{
    if (is_conn_valid) {
        close_tcp_netconn(uart_netconn);
        uart_bridge_reset();
    }

    if (s_listen_netconn != NULL) {
        close_tcp_netconn(s_listen_netconn);
        s_listen_netconn = NULL;
    }

    if (uart_server_events != NULL) {
        xQueueReset(uart_server_events);
    }
}

/**
 * Body of the paused state: close the bridge sockets, acknowledge pause(), then
 * block on a semaphore (no busy-wait) until resume() and re-create the TCP 1346
 * listener.  Everything outside this component (Wi-Fi, lwIP, HTTP/WebSocket,
 * OTA, USBIP) keeps running.
 */
static void uart_bridge_park_until_resume(void)
{
    uart_bridge_release_session();

    /* Drain a possibly stale token from an earlier request, then report idle. */
    (void)xSemaphoreTake(s_pause_ack, 0);
    ESP_LOGI(UART_TAG, "paused: TCP %d closed, UART forwarding parked",
             BIRDIEBOT_UART_BRIDGE_PORT);
    xSemaphoreGive(s_pause_ack);

    xSemaphoreTake(s_resume_req, portMAX_DELAY);

    s_resume_listen_ok = uart_bridge_open_listen();
    (void)xSemaphoreTake(s_resume_ack, 0);
    if (s_resume_listen_ok) {
        ESP_LOGI(UART_TAG, "resumed: listening on TCP %d", BIRDIEBOT_UART_BRIDGE_PORT);
    }
    xSemaphoreGive(s_resume_ack);
}

esp_err_t wt_uart_bridge_pause(void)
{
    esp_err_t err = ESP_OK;

    /* Bridge task not started yet: nothing is listening or forwarding. */
    if (s_ctl_mux == NULL || s_task_handle == NULL) {
        s_paused = true;
        return ESP_OK;
    }

    xSemaphoreTake(s_ctl_mux, portMAX_DELAY);
    if (!s_paused) {
        (void)xSemaphoreTake(s_pause_ack, 0); /* stale token from an early race */
        s_paused = true;
        if (xSemaphoreTake(s_pause_ack, pdMS_TO_TICKS(BRIDGE_CTL_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(UART_TAG, "pause: bridge task did not acknowledge in %d ms",
                     BRIDGE_CTL_TIMEOUT_MS);
            err = ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreGive(s_ctl_mux);

    return err;
}

esp_err_t wt_uart_bridge_resume(void)
{
    esp_err_t err = ESP_OK;

    if (s_ctl_mux == NULL || s_task_handle == NULL) {
        s_paused = false;
        return ESP_OK;
    }

    xSemaphoreTake(s_ctl_mux, portMAX_DELAY);
    if (s_paused) {
        (void)xSemaphoreTake(s_resume_ack, 0);
        s_paused = false;
        xSemaphoreGive(s_resume_req);
        if (xSemaphoreTake(s_resume_ack, pdMS_TO_TICKS(BRIDGE_CTL_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(UART_TAG, "resume: bridge task did not acknowledge in %d ms",
                     BRIDGE_CTL_TIMEOUT_MS);
            err = ESP_ERR_TIMEOUT;
        } else if (!s_resume_listen_ok) {
            err = ESP_FAIL;
        }
    }
    xSemaphoreGive(s_ctl_mux);

    return err;
}

bool wt_uart_bridge_is_paused(void)
{
    return s_paused;
}

void uart_bridge_task(void) {
    struct netbuf *netbuf = NULL;  // To store incoming Data
    struct netconn *nc_in = NULL;  // To accept incoming netconn
    char *buffer;
    uint16_t len_buf;

    uart_bridge_init();

    if (uart_bridge_setup() != ESP_OK) {
        ESP_LOGE(UART_TAG, "bridge setup failed, task disabled");
        if (uart_rx_sub_slot >= 0) {
            uart_service_unsubscribe_rx(uart_rx_sub_slot);
            uart_rx_sub_slot = -1;
        }
        vTaskDelete(NULL);
        return;
    }

    /* A2: pause/resume control objects (one-time; owned by this task). */
    s_ctl_mux = xSemaphoreCreateMutex();
    s_pause_ack = xSemaphoreCreateBinary();
    s_resume_req = xSemaphoreCreateBinary();
    s_resume_ack = xSemaphoreCreateBinary();
    s_task_handle = xTaskGetCurrentTaskHandle();
    if (s_ctl_mux == NULL || s_pause_ack == NULL || s_resume_req == NULL ||
        s_resume_ack == NULL) {
        ESP_LOGE(UART_TAG, "failed to create pause/resume control objects");
    }

    if (!uart_bridge_open_listen()) {
        ESP_LOGE(UART_TAG, "failed to create the bridge server socket");
        if (uart_rx_sub_slot >= 0) {
            uart_service_unsubscribe_rx(uart_rx_sub_slot);
            uart_rx_sub_slot = -1;
        }
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* A2: paused -> park the task on a semaphore (never busy-wait). */
        if (s_paused) {
            uart_bridge_park_until_resume();
            continue;
        }

        /* A2: a previous resume may have failed to re-open the listener. */
        if (s_listen_netconn == NULL) {
            if (!uart_bridge_open_listen()) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            continue;
        }

        netconn_events events;
        int ret = xQueueReceive(uart_server_events, &events, pdMS_TO_TICKS(100));

        if (ret != pdTRUE) {
            // timeout: fallback drain (the RX callback normally wakes us)
            if (is_conn_valid) {
                uart_bridge_flush_to_tcp();
            }
        } else if (events.type == NETCONN_EVT_WIFI_DISCONNECTED) { // WIFI disconnected
            if (is_conn_valid) {
                close_tcp_netconn(uart_netconn);
                uart_bridge_reset();
            }
        } else if (events.type == BRIDGE_EVT_UART_DATA) { // new target UART bytes
            if (is_conn_valid) {
                uart_bridge_flush_to_tcp();
            }
        } else if (events.nc != NULL && events.nc->state == NETCONN_LISTEN) {
            if (is_conn_valid) {
                /* Only one client is served at a time: reject the new one. */
                (void)netconn_accept(events.nc, &nc_in);
                if (nc_in)
                    close_tcp_netconn(nc_in);
                continue;
            }

            int err = netconn_accept(events.nc, &nc_in);
            if (err != ERR_OK) {
                if (nc_in)
                    netconn_delete(nc_in);
                continue;
            }

            ip_addr_t client_addr; // Address port
            uint16_t client_port;  // Client port
            netconn_peer(nc_in, &client_addr, &client_port);
            ESP_LOGI(UART_TAG, "bridge accepted");

            uart_netconn = nc_in;
            is_conn_valid = true;
            is_first_time_recv = true;
        } else if (events.nc != NULL && events.nc->state != NETCONN_LISTEN) {
            uart_netconn = events.nc;
            // forward any pending target UART bytes first
            uart_bridge_flush_to_tcp();

            // try to get data
            if ((netconn_recv(events.nc, &netbuf)) == ERR_OK) // data incoming ?
            {
                do {
                    netbuf_data(netbuf, (void *)&buffer, &len_buf);
                    // write to the target UART via the shared uart_service
                    if (!uart_bridge_maybe_set_baud(buffer, len_buf)) {
                        if (uart_service_write((const uint8_t *)buffer, len_buf,
                                               pdMS_TO_TICKS(20)) != ESP_OK) {
                            ESP_LOGW(UART_TAG, "uart_service_write dropped %u bytes",
                                     (unsigned)len_buf);
                        }
                    }
                } while (netbuf_next(netbuf) >= 0);
                netbuf_delete(netbuf);
            } else {
                /* A 2025 close fix: only close a connection that is still
                 * regarded as valid.  The old code closed the netconn even when
                 * its pending_err already marked it closed/deleted (double
                 * netconn_delete, analysis/REPORT.md P3). */
                if (is_conn_valid) {
                    close_tcp_netconn(events.nc);
                    uart_bridge_reset();
                }
            }
        }
    }
}

#else /* !CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE */

/*
 * The bridge is disabled: keep the symbols used by app_main() so the fixed
 * task creation and any API caller still link.  The UART service is NOT
 * touched, so the Web UART (JSON module 4 + WS binary router) works normally.
 */
static bool s_paused_disabled = false;

void uart_bridge_init(void) {}

void uart_bridge_close(void) {}

esp_err_t wt_uart_bridge_pause(void) {
    s_paused_disabled = true;
    return ESP_OK;
}

esp_err_t wt_uart_bridge_resume(void) {
    s_paused_disabled = false;
    return ESP_OK;
}

bool wt_uart_bridge_is_paused(void) {
    return s_paused_disabled;
}

void uart_bridge_task(void) {
    ESP_LOGW("UART", "UART TCP bridge disabled (CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n)");
    vTaskDelete(NULL);
}

#endif /* CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE */
