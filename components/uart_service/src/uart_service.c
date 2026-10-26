/**
 * @file uart_service.c
 * @brief WiFi Dap — single-owner UART service implementation (WP-203)
 *
 * See uart_service.h for the sharing model.  This file is the only place in
 * the firmware that installs the target UART driver, so the TCP bridge, the
 * Web JSON module and the WS binary router can all use the same UART without
 * double-initializing the driver.
 *
 * Threading summary:
 *   - s_init_mutex protects the one-time installation.
 *   - s_cfg_mutex protects line configuration changes/applies.
 *   - s_state_mux protects small state/counters and the subscriber table.
 *   - RX/TX tasks are the only writers of their own directions' counters.
 */
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps (C5/S3 PSRAM task stacks) */

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "birdiebot_board.h"
#include "uart_service.h"

#define TAG "uart_service"

/* ------------------------------------------------------------------ */
/* Board contract (components/board/include/birdiebot_board.h)        */
/* ------------------------------------------------------------------ */
#define UART_SERVICE_PORT_DEV   ((uart_port_t)BIRDIEBOT_UART_BRIDGE_UART_NUM)
#define UART_SERVICE_TX_PIN_DEV UART_BRIDGE_TX_PIN
#define UART_SERVICE_RX_PIN_DEV UART_BRIDGE_RX_PIN
#define UART_SERVICE_BAUD_DEF   UART_BRIDGE_BAUDRATE

/* ------------------------------------------------------------------ */
/* Service sizing                                                     */
/* ------------------------------------------------------------------ */
#define UART_SERVICE_RX_BUF_SIZE     1024 /* driver RX ring */
#define UART_SERVICE_TX_BUF_SIZE     512  /* driver TX ring */
#define UART_SERVICE_EVENT_QUEUE_LEN 20
#define UART_SERVICE_RX_CHUNK        256
#define UART_SERVICE_TX_CHUNK        256
#define UART_SERVICE_TX_QUEUE_LEN    8

#define UART_SERVICE_RX_TASK_STACK 3072
#define UART_SERVICE_TX_TASK_STACK 3072
#define UART_SERVICE_RX_TASK_PRIO  10
#define UART_SERVICE_TX_TASK_PRIO  9

/*
 * Task stack placement.
 *
 * ESP32-C5 real hardware: the internal heap left after Wi-Fi/web/DAP is too
 * small for the two 3 KiB UART task stacks (uart_service_init failed with
 * ESP_ERR_NO_MEM when creating "uart_rx"; docs/esp32-c5-port.md §11.9).
 * The UART bridge tasks are low-frequency background tasks, so their stacks
 * may live in PSRAM (the UART driver RX/TX/event queues stay in internal RAM -
 * they are DMA related).  Only the stack is moved; if PSRAM is unavailable
 * at runtime the helper falls back to internal RAM.
 *
 * The task stacks are deleted with vTaskDeleteWithCaps() when they were
 * allocated by xTaskCreateWithCaps().
 */
static bool s_rx_task_ext_stack;
static bool s_tx_task_ext_stack;

#if defined(CONFIG_SPIRAM) && (defined(CONFIG_IDF_TARGET_ESP32C5) || \
                                defined(CONFIG_IDF_TARGET_ESP32S3))
#define UART_SERVICE_EXT_STACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static BaseType_t uart_service_task_create(TaskFunction_t fn, const char *name,
                                           uint32_t stack_depth, UBaseType_t prio,
                                           TaskHandle_t *handle, bool *ext_stack)
{
    BaseType_t rc = xTaskCreateWithCaps(fn, name, stack_depth, NULL, prio,
                                        handle, UART_SERVICE_EXT_STACK_CAPS);
    if (rc == pdPASS) {
        *ext_stack = true;
        return rc;
    }
    ESP_LOGW(TAG, "%s: PSRAM task stack unavailable, using internal RAM", name);
    *ext_stack = false;
    return xTaskCreate(fn, name, stack_depth, NULL, prio, handle);
}

static void uart_service_task_delete(TaskHandle_t handle, bool ext_stack)
{
    if (handle == NULL) {
        return;
    }
    if (ext_stack) {
        vTaskDeleteWithCaps(handle);
    } else {
        vTaskDelete(handle);
    }
}
#else
static BaseType_t uart_service_task_create(TaskFunction_t fn, const char *name,
                                           uint32_t stack_depth, UBaseType_t prio,
                                           TaskHandle_t *handle, bool *ext_stack)
{
    *ext_stack = false;
    return xTaskCreate(fn, name, stack_depth, NULL, prio, handle);
}

static void uart_service_task_delete(TaskHandle_t handle, bool ext_stack)
{
    (void)ext_stack;
    if (handle != NULL) {
        vTaskDelete(handle);
    }
}
#endif

typedef struct uart_service_sub_t {
    uart_service_rx_cb_t cb;
    void *arg;
} uart_service_sub_t;

typedef struct uart_service_tx_item_t {
    uint16_t len;
    uint8_t data[UART_SERVICE_TX_CHUNK];
} uart_service_tx_item_t;

static SemaphoreHandle_t s_init_mutex = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_initialized = false;
static bool s_enabled = true;
static uint8_t s_data_type = UART_SERVICE_DEFAULT_DATA_TYPE;
static uart_service_config_t s_cfg = {
    .baud = UART_SERVICE_BAUD_DEF,
    .data_bits = 8,
    .parity = UART_SERVICE_PARITY_NONE,
    .stop_bits = UART_SERVICE_STOP_BITS_1,
    .flow_ctrl = 0,
};

static QueueHandle_t s_event_queue = NULL;
static QueueHandle_t s_tx_queue = NULL;
static TaskHandle_t s_rx_task_handle = NULL;
static TaskHandle_t s_tx_task_handle = NULL;

static uart_service_sub_t s_subscribers[UART_SERVICE_MAX_SUBSCRIBERS];
static uint8_t s_subscriber_count = 0;

static uart_service_status_t s_stats;

/* Pre-scheduler safe critical section (global constructors may register
 * subscribers before the FreeRTOS scheduler is started). */
static inline bool uart_service_lock_available(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

static inline void uart_service_lock(void)
{
    if (uart_service_lock_available()) {
        portENTER_CRITICAL(&s_state_mux);
    }
}

static inline void uart_service_unlock(void)
{
    if (uart_service_lock_available()) {
        portEXIT_CRITICAL(&s_state_mux);
    }
}

static void uart_service_stats_add(volatile uint32_t *counter, uint32_t value)
{
    uart_service_lock();
    *counter += value;
    uart_service_unlock();
}

/* ------------------------------------------------------------------ */
/* Protocol <-> ESP-IDF configuration mapping                         */
/* ------------------------------------------------------------------ */
static bool uart_service_data_bits_valid(uint8_t data_bits)
{
    return data_bits >= 5 && data_bits <= 8;
}

static bool uart_service_parity_valid(uint8_t parity)
{
    return parity <= UART_SERVICE_PARITY_EVEN;
}

static bool uart_service_stop_bits_valid(uint8_t stop_bits)
{
    return stop_bits == UART_SERVICE_STOP_BITS_1 ||
           stop_bits == UART_SERVICE_STOP_BITS_1_5 ||
           stop_bits == UART_SERVICE_STOP_BITS_2;
}

static uart_word_length_t uart_service_data_bits_to_driver(uint8_t data_bits)
{
    switch (data_bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t uart_service_parity_to_driver(uint8_t parity)
{
    switch (parity) {
    case UART_SERVICE_PARITY_ODD:
        return UART_PARITY_ODD;
    case UART_SERVICE_PARITY_EVEN:
        return UART_PARITY_EVEN;
    default:
        return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t uart_service_stop_bits_to_driver(uint8_t stop_bits)
{
    switch (stop_bits) {
    case UART_SERVICE_STOP_BITS_1_5:
        return UART_STOP_BITS_1_5;
    case UART_SERVICE_STOP_BITS_2:
        return UART_STOP_BITS_2;
    default:
        return UART_STOP_BITS_1;
    }
}

static void uart_service_build_driver_config(const uart_service_config_t *cfg, uart_config_t *driver_cfg)
{
    memset(driver_cfg, 0, sizeof(*driver_cfg));
    driver_cfg->baud_rate = (int)cfg->baud;
    driver_cfg->data_bits = uart_service_data_bits_to_driver(cfg->data_bits);
    driver_cfg->parity = uart_service_parity_to_driver(cfg->parity);
    driver_cfg->stop_bits = uart_service_stop_bits_to_driver(cfg->stop_bits);
    driver_cfg->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    driver_cfg->source_clk = UART_SCLK_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* RX fan-out                                                         */
/* ------------------------------------------------------------------ */
static void uart_service_rx_dispatch(const uint8_t *data, size_t len)
{
    uart_service_rx_cb_t cbs[UART_SERVICE_MAX_SUBSCRIBERS];
    void *args[UART_SERVICE_MAX_SUBSCRIBERS];
    uint8_t count = 0;

    if (!s_enabled || len == 0) {
        uart_service_stats_add(&s_stats.rx_dropped, (uint32_t)len);
        return;
    }

    uart_service_lock();
    for (uint8_t i = 0; i < UART_SERVICE_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[i].cb != NULL) {
            cbs[count] = s_subscribers[i].cb;
            args[count] = s_subscribers[i].arg;
            count++;
        }
    }
    uart_service_unlock();

    if (count == 0) {
        /* Nobody consumes UART RX (e.g. only the JSON control plane is used). */
        uart_service_stats_add(&s_stats.rx_dropped, (uint32_t)len);
        return;
    }

    for (uint8_t i = 0; i < count; ++i) {
        cbs[i](UART_SERVICE_DEFAULT_INSTANCE, data, len, args[i]);
    }
}

static void uart_service_rx_task(void *arg)
{
    (void)arg;
    uart_event_t event;
    uint8_t buf[UART_SERVICE_RX_CHUNK];

    while (1) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
        case UART_DATA: {
            size_t remaining = event.size;
            while (remaining > 0) {
                size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                int read = uart_read_bytes(UART_SERVICE_PORT_DEV, buf, want, pdMS_TO_TICKS(20));
                if (read <= 0) {
                    break;
                }
                remaining -= (size_t)read;
                uart_service_stats_add(&s_stats.rx_bytes, (uint32_t)read);
                uart_service_rx_dispatch(buf, (size_t)read);
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            ESP_LOGW(TAG, "UART RX overflow (%d), input flushed", event.type);
            uart_service_stats_add(&s_stats.rx_overflows, 1);
            uart_flush_input(UART_SERVICE_PORT_DEV);
            xQueueReset(s_event_queue);
            break;
        case UART_PARITY_ERR:
        case UART_FRAME_ERR:
            uart_service_stats_add(&s_stats.framing_errors, 1);
            break;
        default:
            break;
        }
    }
}

static void uart_service_tx_task(void *arg)
{
    (void)arg;
    uart_service_tx_item_t item;

    while (1) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_enabled) {
            uart_service_stats_add(&s_stats.tx_dropped, item.len);
            continue;
        }

        size_t written = 0;
        while (written < item.len) {
            int n = uart_write_bytes(UART_SERVICE_PORT_DEV, item.data + written, item.len - written);
            if (n <= 0) {
                uart_service_stats_add(&s_stats.tx_dropped, (uint32_t)(item.len - written));
                ESP_LOGW(TAG, "UART TX failed, dropped %u bytes", (unsigned)(item.len - written));
                break;
            }
            written += (size_t)n;
            uart_service_stats_add(&s_stats.tx_bytes, (uint32_t)n);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Installation                                                       */
/* ------------------------------------------------------------------ */
static esp_err_t uart_service_do_init(void)
{
    esp_err_t err;
    uart_config_t driver_cfg;

    if (uart_is_driver_installed(UART_SERVICE_PORT_DEV)) {
        /* Single-owner contract: someone else installed the UART driver, which
         * must not happen in this firmware.  Refuse to double-initialize. */
        ESP_LOGE(TAG, "UART%d driver already installed by another owner",
                 (int)UART_SERVICE_PORT_DEV);
        return ESP_ERR_INVALID_STATE;
    }

    uart_service_build_driver_config(&s_cfg, &driver_cfg);

    err = uart_param_config(UART_SERVICE_PORT_DEV, &driver_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(UART_SERVICE_PORT_DEV,
                       UART_SERVICE_TX_PIN_DEV, UART_SERVICE_RX_PIN_DEV,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_SERVICE_PORT_DEV,
                              UART_SERVICE_RX_BUF_SIZE, UART_SERVICE_TX_BUF_SIZE,
                              UART_SERVICE_EVENT_QUEUE_LEN, &s_event_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_tx_queue = xQueueCreate(UART_SERVICE_TX_QUEUE_LEN, sizeof(uart_service_tx_item_t));
    if (s_tx_queue == NULL) {
        ESP_LOGE(TAG, "failed to create UART TX queue");
        uart_driver_delete(UART_SERVICE_PORT_DEV);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_cfg_mutex = xSemaphoreCreateMutex();
    if (s_cfg_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create UART config mutex");
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        uart_driver_delete(UART_SERVICE_PORT_DEV);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (uart_service_task_create(uart_service_rx_task, "uart_rx",
                                 UART_SERVICE_RX_TASK_STACK,
                                 UART_SERVICE_RX_TASK_PRIO, &s_rx_task_handle,
                                 &s_rx_task_ext_stack) != pdPASS) {
        ESP_LOGE(TAG, "failed to create UART RX task");
        vSemaphoreDelete(s_cfg_mutex);
        s_cfg_mutex = NULL;
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        uart_driver_delete(UART_SERVICE_PORT_DEV);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (uart_service_task_create(uart_service_tx_task, "uart_tx",
                                 UART_SERVICE_TX_TASK_STACK,
                                 UART_SERVICE_TX_TASK_PRIO, &s_tx_task_handle,
                                 &s_tx_task_ext_stack) != pdPASS) {
        ESP_LOGE(TAG, "failed to create UART TX task");
        uart_service_task_delete(s_rx_task_handle, s_rx_task_ext_stack);
        s_rx_task_handle = NULL;
        vSemaphoreDelete(s_cfg_mutex);
        s_cfg_mutex = NULL;
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        uart_driver_delete(UART_SERVICE_PORT_DEV);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    uart_service_lock();
    s_initialized = true;
    s_stats.initialized = true;
    s_stats.enabled = true;
    s_stats.data_type = s_data_type;
    s_stats.subscriber_count = s_subscriber_count;
    uart_service_unlock();

    ESP_LOGI(TAG, "UART%d ready: TX=%d RX=%d %u-%u%s%u",
             (int)UART_SERVICE_PORT_DEV, UART_SERVICE_TX_PIN_DEV, UART_SERVICE_RX_PIN_DEV,
             (unsigned)s_cfg.baud, (unsigned)s_cfg.data_bits,
             s_cfg.parity == UART_SERVICE_PARITY_NONE ? "N" :
             (s_cfg.parity == UART_SERVICE_PARITY_ODD ? "O" : "E"),
             (unsigned)s_cfg.stop_bits);
    /* Bring-up diagnostics: which allocator served the task stacks and how much
     * internal RAM is left once the UART bridge is up (the C5 failure mode was
     * silently returning ESP_ERR_NO_MEM here). */
    ESP_LOGI(TAG, "UART task stacks: rx=%s tx=%s; internal free=%u largest=%u",
             s_rx_task_ext_stack ? "PSRAM" : "internal",
             s_tx_task_ext_stack ? "PSRAM" : "internal",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));

    return ESP_OK;
}

esp_err_t uart_service_init(void)
{
    esp_err_t err = ESP_OK;

    if (s_initialized) {
        return ESP_OK;
    }

    uart_service_lock();
    if (s_init_mutex == NULL) {
        s_init_mutex = xSemaphoreCreateMutex();
    }
    uart_service_unlock();

    if (s_init_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_init_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_initialized) {
        err = uart_service_do_init();
    }

    xSemaphoreGive(s_init_mutex);
    return err;
}

bool uart_service_is_initialized(void)
{
    return s_initialized;
}

/* ------------------------------------------------------------------ */
/* Instance identity                                                  */
/* ------------------------------------------------------------------ */
int uart_service_instance_count(void)
{
    return UART_SERVICE_INSTANCE_NUM;
}

int uart_service_default_instance(void)
{
    return UART_SERVICE_DEFAULT_INSTANCE;
}

uart_port_t uart_service_port(void)
{
    return UART_SERVICE_PORT_DEV;
}

int uart_service_tx_pin(void)
{
    return UART_SERVICE_TX_PIN_DEV;
}

int uart_service_rx_pin(void)
{
    return UART_SERVICE_RX_PIN_DEV;
}

/* ------------------------------------------------------------------ */
/* Data path                                                          */
/* ------------------------------------------------------------------ */
esp_err_t uart_service_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    esp_err_t err;
    size_t offset = 0;
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = uart_service_init();
    if (err != ESP_OK) {
        uart_service_stats_add(&s_stats.tx_dropped, (uint32_t)len);
        return err;
    }

    if (!s_enabled || s_tx_queue == NULL) {
        uart_service_stats_add(&s_stats.tx_dropped, (uint32_t)len);
        return ESP_ERR_INVALID_STATE;
    }

    while (offset < len) {
        size_t chunk = (len - offset) > UART_SERVICE_TX_CHUNK ? UART_SERVICE_TX_CHUNK : (len - offset);
        uart_service_tx_item_t item;

        item.len = (uint16_t)chunk;
        memcpy(item.data, data + offset, chunk);

        if (xQueueSend(s_tx_queue, &item, wait) != pdTRUE) {
            uart_service_stats_add(&s_stats.tx_dropped, (uint32_t)(len - offset));
            return ESP_ERR_NO_MEM;
        }
        offset += chunk;
        wait = 0; /* only the first chunk waits for backpressure */
    }

    return ESP_OK;
}

int uart_service_subscribe_rx(uart_service_rx_cb_t cb, void *arg)
{
    int slot = -1;

    if (cb == NULL) {
        return -1;
    }

    uart_service_lock();
    for (uint8_t i = 0; i < UART_SERVICE_MAX_SUBSCRIBERS; ++i) {
        if (s_subscribers[i].cb == NULL) {
            s_subscribers[i].cb = cb;
            s_subscribers[i].arg = arg;
            s_subscriber_count++;
            s_stats.subscriber_count = s_subscriber_count;
            slot = (int)i;
            break;
        }
    }
    uart_service_unlock();

    return slot;
}

void uart_service_unsubscribe_rx(int slot)
{
    if (slot < 0 || slot >= UART_SERVICE_MAX_SUBSCRIBERS) {
        return;
    }

    uart_service_lock();
    if (s_subscribers[slot].cb != NULL) {
        s_subscribers[slot].cb = NULL;
        s_subscribers[slot].arg = NULL;
        if (s_subscriber_count > 0) {
            s_subscriber_count--;
        }
        s_stats.subscriber_count = s_subscriber_count;
    }
    uart_service_unlock();
}

/* ------------------------------------------------------------------ */
/* Configuration / status                                             */
/* ------------------------------------------------------------------ */
esp_err_t uart_service_get_config(uart_service_config_t *cfg)
{
    esp_err_t err;

    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = uart_service_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *cfg = s_cfg;
    xSemaphoreGive(s_cfg_mutex);

    return ESP_OK;
}

esp_err_t uart_service_set_baud(uint32_t baud)
{
    esp_err_t err;

    if (baud == 0 || baud > 5000000) {
        return ESP_ERR_INVALID_ARG;
    }

    err = uart_service_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    err = uart_set_baudrate(UART_SERVICE_PORT_DEV, baud);
    if (err == ESP_OK) {
        s_cfg.baud = baud;
    }
    xSemaphoreGive(s_cfg_mutex);

    return err;
}

esp_err_t uart_service_set_config(const uart_service_config_t *cfg)
{
    esp_err_t err;
    uart_config_t driver_cfg;
    uart_service_config_t new_cfg;

    if (cfg == NULL || cfg->baud == 0 || cfg->baud > 5000000 ||
        !uart_service_data_bits_valid(cfg->data_bits) ||
        !uart_service_parity_valid(cfg->parity) ||
        !uart_service_stop_bits_valid(cfg->stop_bits)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (cfg->flow_ctrl != 0) {
        /* No RTS/CTS pins in the board pin map. */
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = uart_service_init();
    if (err != ESP_OK) {
        return err;
    }

    new_cfg = *cfg;

    if (xSemaphoreTake(s_cfg_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uart_service_build_driver_config(&new_cfg, &driver_cfg);
    err = uart_param_config(UART_SERVICE_PORT_DEV, &driver_cfg);
    if (err == ESP_OK) {
        s_cfg = new_cfg;
    }
    xSemaphoreGive(s_cfg_mutex);

    return err;
}

esp_err_t uart_service_set_flow_ctrl(uint8_t flow_ctrl)
{
    if (flow_ctrl != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

esp_err_t uart_service_set_enabled(bool enabled)
{
    esp_err_t err = uart_service_init();

    if (err != ESP_OK) {
        return err;
    }

    uart_service_lock();
    s_enabled = enabled;
    s_stats.enabled = enabled;
    uart_service_unlock();

    if (!enabled) {
        /* Drop bytes that are already queued/buffered so that enabling the
         * port again does not replay stale target output. */
        if (s_tx_queue != NULL) {
            xQueueReset(s_tx_queue);
        }
        uart_flush_input(UART_SERVICE_PORT_DEV);
    }

    return ESP_OK;
}

bool uart_service_is_enabled(void)
{
    return s_enabled;
}

void uart_service_get_status(uart_service_status_t *status)
{
    if (status == NULL) {
        return;
    }

    uart_service_lock();
    *status = s_stats;
    status->initialized = s_initialized;
    status->enabled = s_enabled;
    status->data_type = s_data_type;
    status->subscriber_count = s_subscriber_count;
    uart_service_unlock();
}

uint8_t uart_service_get_data_type(void)
{
    return s_data_type;
}

esp_err_t uart_service_set_data_type(uint8_t data_type)
{
    if (data_type != UART_SERVICE_DEFAULT_DATA_TYPE) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uart_service_lock();
    s_data_type = data_type;
    s_stats.data_type = data_type;
    uart_service_unlock();

    return ESP_OK;
}
