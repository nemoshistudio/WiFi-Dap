/**
 * @file uart_service.h
 * @brief WiFi Dap — single-owner UART service (WP-203)
 *
 * The UART service is the only place that installs and owns the target UART
 * driver (board UART instance, default 115200-8N1, pins from
 * components/board/include/birdiebot_board.h).  Its clients are:
 *   - components/uart_tcp_bridge   (raw TCP bridge on port 1346)
 *   - components/uart_service      (JSON module 4: UART, docs/protocol.md §2.4)
 *   - project_components/web_server/uri_modules/uri_bin_router.c
 *                                  (WS binary data path, docs/ws-binary-router.md)
 *
 * Sharing model
 * -------------
 *   TX: uart_service_write() enqueues into a static TX queue and a dedicated
 *       task drains it into the UART driver (backpressure: queue full ->
 *       ESP_ERR_NO_MEM and tx_dropped is counted; callers must not block the
 *       httpd/WS task).
 *   RX: the service RX task reads the driver event queue and fans every chunk
 *       out to the registered rx subscribers (uart_tcp_bridge and the WS binary
 *       router).  Callbacks run in the service RX task context and must return
 *       quickly (copy/queue only).
 *
 * Lifecycle: uart_service_init() is lazy, idempotent and thread-safe; it is
 * called by the first client (TCP bridge task, Web JSON request, ...).  When
 * CONFIG_BIRDIEBOT_UART_BRIDGE_ENABLE=n the TCP bridge is not created but the
 * UART service (and therefore Web UART) keeps working.
 *
 * Contract: docs/protocol.md §2.4 (JSON), docs/ws-binary-router.md (binary).
 */
#ifndef UART_SERVICE_H_GUARD
#define UART_SERVICE_H_GUARD

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JSON module id (docs/protocol.md §2): UART = 4. */
#define UART_SERVICE_MODULE_ID 4

/* One physical UART instance is exposed to the protocol (sub_mod == 0). */
#define UART_SERVICE_INSTANCE_NUM     1
#define UART_SERVICE_DEFAULT_INSTANCE 0

/* Maximum number of RX consumers (TCP bridge + WS binary router + spare). */
#define UART_SERVICE_MAX_SUBSCRIBERS 4

/* UART(4) GET/SET_CONFIG wire encoding (docs/protocol.md §2.4). */
#define UART_SERVICE_STOP_BITS_1   1
#define UART_SERVICE_STOP_BITS_1_5 15
#define UART_SERVICE_STOP_BITS_2   2

/* UART(4) GET/SET_DATA_TYPE: only WT_DATA_RAW is supported (protocol.md §3). */
#define UART_SERVICE_DEFAULT_DATA_TYPE 0x20

/** Parity values as used on the wire (docs/protocol.md §2.4). */
typedef enum uart_service_parity_e {
    UART_SERVICE_PARITY_NONE = 0,
    UART_SERVICE_PARITY_ODD = 1,
    UART_SERVICE_PARITY_EVEN = 2,
} uart_service_parity_e;

/** Line configuration exposed by UART(4) GET_CONFIG/SET_CONFIG. */
typedef struct uart_service_config_t {
    uint32_t baud;      /**< baud rate (default 115200) */
    uint8_t data_bits;  /**< 5..8 (default 8) */
    uint8_t parity;     /**< uart_service_parity_e (default NONE) */
    uint8_t stop_bits;  /**< 1 / 15 (1.5) / 2 (default 1) */
    uint8_t flow_ctrl;  /**< 0 = none; RTS/CTS unsupported (no board pins) */
} uart_service_config_t;

/** Runtime status / counters (UART(4) GET_STATUS and diagnostics). */
typedef struct uart_service_status_t {
    bool initialized;
    bool enabled;              /**< data path enabled (SET_STATUS) */
    uint8_t data_type;         /**< binary data type, WT_DATA_RAW */
    uint8_t subscriber_count;  /**< registered RX consumers */
    uint32_t rx_bytes;         /**< bytes received from the target */
    uint32_t tx_bytes;         /**< bytes written to the target */
    uint32_t rx_dropped;       /**< no subscriber / disabled delivery drops */
    uint32_t tx_dropped;       /**< TX queue full or UART write failures */
    uint32_t rx_overflows;     /**< UART driver FIFO/buffer overflows */
    uint32_t framing_errors;   /**< UART parity/frame errors */
} uart_service_status_t;

/**
 * RX fan-out callback.  Runs in the uart_service RX task context, must not
 * block: copy the data into your own queue/ring buffer and return.
 *
 * @param sub_mod UART instance (always UART_SERVICE_DEFAULT_INSTANCE for now)
 * @param data    received bytes (valid only for the duration of the call)
 * @param len     number of bytes
 * @param arg     opaque pointer registered with the callback
 */
typedef void (*uart_service_rx_cb_t)(uint8_t sub_mod, const uint8_t *data, size_t len, void *arg);

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

/** Install the UART driver, TX/RX tasks and default configuration.  Idempotent
 *  and thread-safe.  Returns ESP_OK when the service is ready. */
esp_err_t uart_service_init(void);

bool uart_service_is_initialized(void);

/* ------------------------------------------------------------------------ */
/* Instance identity (DATA_FLOW module 2 / UART module 4)                   */
/* ------------------------------------------------------------------------ */

/** Number of protocol-visible instances (always 1). */
int uart_service_instance_count(void);
/** Default instance index (0). */
int uart_service_default_instance(void);
/** ESP-IDF UART port owned by the service (board UART instance). */
uart_port_t uart_service_port(void);
/** Board TX / RX GPIO numbers. */
int uart_service_tx_pin(void);
int uart_service_rx_pin(void);

/* ------------------------------------------------------------------------ */
/* Data path                                                                */
/* ------------------------------------------------------------------------ */

/**
 * Queue bytes for transmission to the target UART.
 *
 * @param data       bytes to send (copied; caller keeps ownership)
 * @param len        number of bytes
 * @param timeout_ms maximum time to wait for queue space (0 = do not block,
 *                   required from httpd/WS callback context)
 * @return ESP_OK when all bytes were queued, ESP_ERR_NO_MEM when the queue is
 *         full (tx_dropped is counted), ESP_ERR_INVALID_STATE when the service
 *         is disabled/unavailable.
 */
esp_err_t uart_service_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * Register an RX consumer.  Safe to call before uart_service_init() (the
 * WebSocket/WS-DAP constructors do) and from FreeRTOS tasks.
 *
 * @return subscriber slot index >= 0, or -1 when the table is full/invalid.
 */
int uart_service_subscribe_rx(uart_service_rx_cb_t cb, void *arg);

/** Remove a previously registered RX consumer. */
void uart_service_unsubscribe_rx(int slot);

/* ------------------------------------------------------------------------ */
/* Configuration / status                                                   */
/* ------------------------------------------------------------------------ */

esp_err_t uart_service_get_config(uart_service_config_t *cfg);
/** Change baud rate only (keeps other line parameters). */
esp_err_t uart_service_set_baud(uint32_t baud);
/** Apply a full line configuration (baud/data bits/parity/stop bits). */
esp_err_t uart_service_set_config(const uart_service_config_t *cfg);
/** Only flow control "none" is supported (no RTS/CTS pins in the pin map). */
esp_err_t uart_service_set_flow_ctrl(uint8_t flow_ctrl);

/** Enable/disable the data path (UART(4) SET_STATUS); the driver stays
 *  installed.  While disabled TX writes are rejected and RX data is dropped. */
esp_err_t uart_service_set_enabled(bool enabled);
bool uart_service_is_enabled(void);

void uart_service_get_status(uart_service_status_t *status);

uint8_t uart_service_get_data_type(void);
/** Only UART_SERVICE_DEFAULT_DATA_TYPE (RAW 0x20) is supported. */
esp_err_t uart_service_set_data_type(uint8_t data_type);

#ifdef __cplusplus
}
#endif

#endif /* UART_SERVICE_H_GUARD */
