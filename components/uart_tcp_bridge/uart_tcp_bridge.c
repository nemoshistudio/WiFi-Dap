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
 * @brief UART TCP bridge
 * @version 0.2
 * @date 2021-11-16
 *
 * WiFi Dap merge notes (WP-114)
 * ------------------------------
 *  - Implementation taken from the A 2025 tree (main/uart_bridge.c, which is
 *    base + the ESP32-S3 UART1 TX=10/RX=9 support and the 2025 connection-close
 *    fix) and componentized on top of the C/IDF5 platform layer (printf/LWIP
 *    netconn API/driver/uart.h).
 *  - All pins/ports come from components/board/include/birdiebot_board.h:
 *    BIRDIEBOT_UART_BRIDGE_UART_NUM, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN,
 *    BIRDIEBOT_UART_BRIDGE_PORT (1346) and UART_BRIDGE_BAUDRATE (115200).
 *    The C tree's duplicated-and-unused UART_PORT/UART_HW/PORT_* macro block is
 *    gone; so is the ESP8266 branch (the project builds ESP32/C3/S3 only).
 *  - Double delete fixed: the close-on-receive-error path uses the A 2025
 *    `if (is_conn_valid) { close_tcp_netconn(); uart_bridge_reset(); }` guard.
 *    The old C code could call close_tcp_netconn() on a netconn whose
 *    pending_err already marked it closed/deleted (double netconn_delete).
 *  - Event queue created exactly once: uart_bridge_init() is idempotent and is
 *    the only place that allocates it (the old code created it in both
 *    uart_bridge_init() and uart_bridge_task(), leaking the first queue).
 *  - ESP_ERROR_CHECK() removed from the runtime uart_get_buffered_data_len()
 *    calls (P3 in analysis/REPORT.md): a transient UART error no longer aborts
 *    the device; only the one-time setup uses ESP_ERROR_CHECK().
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

#include "driver/uart.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/netbuf.h"
#include "lwip/api.h"
#include "lwip/tcp.h"

#include "birdiebot_board.h"
#include "uart_tcp_bridge.h"

#if CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE

/* The bridge owns one UART instance for both directions (board contract). */
#define UART_BRIDGE_UART BIRDIEBOT_UART_BRIDGE_UART_NUM

#define EVENTS_QUEUE_SIZE 10
#define UART_BUF_SIZE     512

#ifdef CALLBACK_DEBUG
#define debug(s, ...) printf("%s: " s "\n", "Cb:", ##__VA_ARGS__)
#else
#define debug(s, ...)
#endif

static const char *UART_TAG = "UART";
#define NETCONN_EVT_WIFI_DISCONNECTED (NETCONN_EVT_ERROR + 1)

static QueueHandle_t uart_server_events = NULL;
typedef struct
{
    struct netconn *nc;
    uint8_t type;
} netconn_events;

static uint8_t uart_read_buffer[UART_BUF_SIZE];
// use lwip buffer to write back
static struct netconn *uart_netconn = NULL;
static bool is_conn_valid = false; // lock free
static bool is_first_time_recv = false;

void uart_bridge_init(void)
{
    /* Fixes the duplicated queue creation of the A/C tree: only this function
     * allocates the event queue, and it is safe to call more than once. */
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

static void uart_bridge_setup(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE};

    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_UART, &uart_config));
    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_UART, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_UART,
                                 UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(UART_TAG, "bridge ready: uart%d TX=%d RX=%d port=%d baud=%d",
             (int)UART_BRIDGE_UART, UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN,
             BIRDIEBOT_UART_BRIDGE_PORT, UART_BRIDGE_BAUDRATE);
}

/*
 *  Forward whatever the target UART has buffered to the TCP client.
 *  A UART read error is logged (not fatal): aborting the device on a transient
 *  driver error was one of the P3 findings in analysis/REPORT.md.
 */
static void uart_bridge_flush_to_tcp(struct netconn *conn) {
    size_t uart_buf_len = 0;

    if (conn == NULL) {
        return;
    }

    if (uart_get_buffered_data_len(UART_BRIDGE_UART, &uart_buf_len) != ESP_OK) {
        ESP_LOGW(UART_TAG, "uart_get_buffered_data_len failed");
        return;
    }

    if (uart_buf_len > UART_BUF_SIZE) {
        uart_buf_len = UART_BUF_SIZE;
    }
    if (uart_buf_len == 0) {
        return;
    }

    uart_buf_len = uart_read_bytes(UART_BRIDGE_UART, uart_read_buffer, uart_buf_len, pdMS_TO_TICKS(5));
    netconn_write(conn, uart_read_buffer, uart_buf_len, NETCONN_COPY);
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
            uart_set_baudrate(UART_BRIDGE_UART, baudrate);
            return true;
        }
    }

    return false;
}

void uart_bridge_task(void) {
    struct netconn *nc = NULL;     // To create servers
    struct netbuf *netbuf = NULL;  // To store incoming Data
    struct netconn *nc_in = NULL;  // To accept incoming netconn
    char *buffer;
    uint16_t len_buf;

    uart_bridge_init();
    uart_bridge_setup();

    set_tcp_server_netconn(&nc, BIRDIEBOT_UART_BRIDGE_PORT, netCallback);
    if (nc == NULL) {
        ESP_LOGE(UART_TAG, "failed to create the bridge server socket");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        netconn_events events;
        int ret = xQueueReceive(uart_server_events, &events, pdMS_TO_TICKS(100));

        if (ret != pdTRUE) {
            // timeout
            if (is_conn_valid) {
                uart_bridge_flush_to_tcp(uart_netconn);
            }
        } else if (events.type == NETCONN_EVT_WIFI_DISCONNECTED) { // WIFI disconnected
            if (is_conn_valid) {
                close_tcp_netconn(uart_netconn);
                uart_bridge_reset();
            }
        } else if (events.nc->state == NETCONN_LISTEN) {
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
        } else if (events.nc->state != NETCONN_LISTEN) {
            uart_netconn = events.nc;
            // read data from UART and forward it
            uart_bridge_flush_to_tcp(events.nc);

            // try to get data
            if ((netconn_recv(events.nc, &netbuf)) == ERR_OK) // data incoming ?
            {
                do {
                    netbuf_data(netbuf, (void *)&buffer, &len_buf);
                    // write to uart
                    if (!uart_bridge_maybe_set_baud(buffer, len_buf)) {
                        uart_write_bytes(UART_BRIDGE_UART, (const char *)buffer, len_buf);
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
 * task creation and any API caller still link.  The task exits immediately.
 */
void uart_bridge_init(void) {}

void uart_bridge_close(void) {}

void uart_bridge_task(void) {
    ESP_LOGW("UART", "UART TCP bridge disabled (CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n)");
    vTaskDelete(NULL);
}

#endif /* CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE */
