/*
 * SPDX-License-Identifier: MIT
 *
 * WiFi Dap - target flash engine (Web-side direct flashing)
 *
 * The Web UI uploads a target firmware image (STM32 .bin) to the ESP32 over
 * HTTP/WS; this component streams it over the existing CMSIS-DAP command engine
 * to the target's SWD port.  No staging partition is used: data is programmed
 * as it arrives (only a small alignment buffer is kept in RAM).
 *
 * Supported target families (first hardware: STM32F407):
 *   - STM32F4/F2 (Cortex-M4/M3, FLASH at 0x08000000): sectors 16K/64K/128K,
 *     x64 parallel programming (PSIZE=3) with automatic x32 fallback;
 *   - STM32F7 single-bank (Cortex-M7, 1 MiB F74x/F75x, 512 KiB F72x/F73x):
 *     sectors 32K/128K/256K (or 16K/64K/128K), same x64 strategy;
 *   - STM32H7 (Cortex-M7, dual bank 0x08000000/0x08100000): 128K sectors,
 *     256-bit (32-byte aligned) flash word programming;
 *   - STM32G4 (Cortex-M4): 2 KiB pages (4 KiB in G47x/G48x single-bank mode),
 *     FLASH_OPTR.DBANK dual-bank selection, 64-bit double-word programming;
 *   - STM32L4 (Cortex-M4): 2 KiB pages, 4/8 KiB pages on L4R/S/P/Q, DBANK
 *     dual-bank option, 64-bit double-word programming;
 *   - STM32G0 (Cortex-M0+): 2 KiB pages, G0B/G0C dual-bank option (BKER bit
 *     13), 64-bit double-word programming;
 *   - STM32WB / STM32WL (Cortex-M4, FLASH interface at 0x58004000): 4 KiB
 *     (WB5x) / 2 KiB (WLEx) pages, 64-bit double-word programming;
 *   - STM32F0/F1/F3 (Cortex-M0/M3/M4): 1/2 KiB pages, 16-bit half-word
 *     programming (F1 XL-density two-bank addressing included).
 * Other targets are detected and reported as identified-but-unsupported.
 *
 * Thread-safety / concurrency contract:
 *   - All API functions are thread-safe (internal mutex + busy state).
 *   - A second call while an operation is in progress returns
 *     ESP_ERR_INVALID_STATE (no blocking).  wt_tf_get_status() is always
 *     callable and returns a snapshot; it never blocks on a running flash.
 *   - wt_tf_abort() may be called while a flash operation is running: it sets
 *     the abort flag (returns ESP_OK); the running operation then stops at the
 *     next block boundary, locks the flash, resets the target, releases the
 *     DAP and reports WT_TF_ERROR with "aborted" in the status message.
 *
 * DAP ownership:
 *   Every operation that talks to the target takes exclusive ownership of the
 *   CMSIS-DAP engine: it pauses the UART bridge (weak symbol, when linked),
 *   posts dap_handle_request_reset(DELETE_HANDLE) + dap_handle_wait_reset(3000)
 *   and then drives the target with DAP_ExecuteCommand().  On success, error
 *   or abort it always releases the engine with
 *   dap_handle_request_reset(RESET_HANDLE) and resumes the UART bridge, so the
 *   normal USBIP / WebSocket-DAP / elaphureLink data path is restored.
 *   WiFi/AP/httpd/WebSocket are never stopped by this component.
 *
 * Contract for the Web layer (web-dev):
 *   wt_tf_flash_begin() -> wt_tf_flash_feed()* -> wt_tf_flash_end()
 *   (or wt_tf_abort()).  flash_feed() may be called with arbitrary chunk sizes;
 *   returns quickly unless an internal programming block is due.
 *   Erase (in flash_begin) and verify (in flash_end) can take seconds: the Web
 *   layer must run the sequence off the httpd task (request_runner) so that WS
 *   heartbeats and normal JSON commands keep flowing.
 */
#ifndef WT_TARGET_FLASH_H
#define WT_TARGET_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Flash engine state (mirrored in wt_tf_status_t.state). */
typedef enum {
    WT_TF_IDLE = 0,        /**< no operation in progress */
    WT_TF_ERASING,         /**< erasing the sectors that cover the image */
    WT_TF_PROGRAMMING,     /**< streaming/programming the image */
    WT_TF_VERIFYING,       /**< reading the flash back and comparing SHA-256 */
    WT_TF_RESETTING,       /**< target reset / run release in progress */
    WT_TF_DONE,            /**< last flash operation completed and verified */
    WT_TF_ERROR,           /**< last operation failed (see status.err/message) */
} wt_tf_state_t;

/** Detection confidence for the DEV_ID/model match (v3).  Values mirror the
 *  Web/API `detection_confidence` strings:
 *    WT_TF_CONF_UNKNOWN        <-> "unknown"        (no DEV_ID match at all)
 *    WT_TF_CONF_GROUP          <-> "group"          (DEV_ID group only, no FSIZE)
 *    WT_TF_CONF_FLASH_NARROWED <-> "flash-narrowed" (DEV_ID + valid FSIZE used)
 */
typedef enum {
    WT_TF_CONF_UNKNOWN = 0,
    WT_TF_CONF_GROUP = 1,
    WT_TF_CONF_FLASH_NARROWED = 2,
} wt_tf_confidence_t;

/** Detected target description (filled by wt_tf_probe() / wt_tf_flash_begin()). */
typedef struct {
    bool detected;             /**< true when a real STM32 was identified, even
                                    when its series is not flashable; false
                                    only when no target answered on SWD */
    char family[16];           /**< "stm32f4"/"stm32h7"/... (lower-case series)
                                    or "stm32" for an unknown DEV_ID */
    uint32_t dpidr;            /**< SW-DP IDCODE register */
    uint32_t cpuid;            /**< CPUID (0xE000ED00) */
    uint32_t idcode;           /**< DBGMCU IDCODE (DEV_ID + REV_ID) */
    uint8_t uid[12];           /**< 96-bit unique device ID (zeros if the
                                    UID location is unknown/unreadable) */
    uint32_t flash_base;       /**< main flash base (0x08000000) */
    uint32_t flash_size;       /**< main flash size in bytes (0 when unknown) */
    uint32_t max_image_size;   /**< largest image accepted by flash_begin() */

    /* Extended identification (v2; old fields above keep their meaning). */
    char series[16];           /**< "STM32H7"/"STM32F4"/.../"unknown" */
    char device[56];           /**< candidate model list, e.g.
                                    "STM32H723/H725/H730/H733/H735"; unknown
                                    DEV_ID -> "unknown (DEV_ID=0x483)" */
    bool supported;            /**< true only for families with a working
                                    flash implementation (see the family list
                                    at the top of this header) */
    uint32_t swd_clock_hz;     /**< SWD clock the probe link came up with
                                    (H7 boards may fall back to 1 MHz/500 kHz) */

    /* Detection detail (v3, additive; old fields above keep their meaning).
     * DEV_ID alone identifies a device group, never a unique order code: the
     * candidate list is narrowed by the FSIZE read at runtime but package /
     * exact order code still require a user-confirmed MCU/board profile. */
    uint16_t dev_id;           /**< DBGMCU DEV_ID = IDCODE bits[11:0]; 0 if unknown */
    uint32_t flash_kib;        /**< flash size in KiB (0 when FSIZE unreadable) */
    char model_candidates[80]; /**< narrowed candidate group, e.g.
                                    "STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]" */
    char core[16];             /**< Cortex core from CPUID, e.g. "Cortex-M4" */
    uint8_t confidence;        /**< wt_tf_confidence_t (unknown/group/flash-narrowed) */
} wt_tf_info_t;

/** Operation status snapshot. */
typedef struct {
    wt_tf_state_t state;   /**< current state */
    uint32_t total;        /**< image size announced to flash_begin() */
    uint32_t written;      /**< bytes programmed so far */
    uint32_t verified;     /**< bytes read back + verified so far */
    int percent;           /**< 0..100 overall progress */
    int err;               /**< ESP_ERR_* of the last failure (0 = none) */
    char message[64];      /**< short human-readable status/error text */
} wt_tf_status_t;

/**
 * @brief Initialize the flash engine (idempotent).
 *
 * @return ESP_OK, or ESP_FAIL when internal resources cannot be created.
 */
esp_err_t wt_tf_init(void);

/**
 * @brief SWD connect + identify the target family/flash layout.
 *
 * Does not halt the core; takes and releases the DAP engine.
 *
 * @param[out] info detection result (may be NULL).
 * @return ESP_OK when a target was identified (check info->supported for
 *         flashability; an unknown DEV_ID still returns ESP_OK with
 *         family="stm32"), ESP_ERR_NOT_FOUND when nothing answered on SWD,
 *         other ESP_ERR_* on transport failures.
 */
esp_err_t wt_tf_probe(wt_tf_info_t *info);

/**
 * Force the SWD clock used by target detection/flash and local sampling.
 * 0 = automatic 2 MHz -> 1 MHz -> 500 kHz probe; manual values must be
 * 100 kHz..10 MHz. The setting is process-wide and intentionally not
 * persisted across reboot.
 */
/**
 * Force the SWD clock used by the next target probe / flash session.
 * 0 restores the automatic walk.  Accepted range: 100 kHz..10 MHz on targets
 * without the ESP32-S3 M5 engine, 100 kHz..24 MHz when it is enabled
 * (160 MHz CPU clamps at 20 MHz, 240 MHz variant reaches 24 MHz).
 */
esp_err_t wt_tf_set_swd_clock(uint32_t clock_hz);

/** Currently selected manual SWD clock, or 0 for automatic probing. */
uint32_t wt_tf_get_swd_clock(void);

/**
 * @brief Actual SWD clock the last successful link bring-up used.
 *
 * With automatic probing this is the first clock of the 4M/2M/1M/500k ladder
 * that identified the target; it differs from wt_tf_get_swd_clock() (which
 * reports the manual selection, 0 = auto).  0 means no successful bring-up
 * since boot.
 */
uint32_t wt_tf_get_actual_swd_clock(void);

/**
 * @brief Read-only link/health diagnostics for GET /api/target/status.
 *
 * Everything here is a real measurement or driver state; fields that could not
 * be measured are reported as 0/false instead of being fabricated.  This API
 * never takes the DAP engine and never blocks on a running flash session.
 */
typedef struct {
    /* Wi-Fi station link (read-only esp_wifi query; WiFi may also be off) */
    bool wifi_connected;
    int8_t wifi_rssi;              /**< dBm, valid when wifi_connected */
    uint8_t wifi_channel;          /**< primary channel, valid when connected */
    char wifi_ssid[33];            /**< connected SSID, empty when not connected */
    /* Internal heap snapshot (same metric as the Web heap guard) */
    uint32_t heap_free;
    uint32_t heap_largest;
    /* SWD link actually used by the last successful bring-up */
    uint32_t swd_clock_actual_hz;  /**< 0 until a target was probed */
    uint32_t swd_clock_manual_hz;  /**< 0 = auto */
    /* USBIP<->DAP ring buffers (P0-2 early reservation) */
    bool dap_ring_preallocated;    /**< backed by the early static reservation */
    bool dap_ring_active;          /**< rings currently created */
    uint32_t dap_ring_request_bytes;
    uint32_t dap_ring_response_bytes;
    /* Device-side USBIP<->DAP transaction latency (never a network RTT) */
    uint32_t dap_xfer_count;
    uint32_t dap_xfer_avg_us;
    uint32_t dap_xfer_max_us;
    uint32_t dap_xfer_last_us;
} wt_tf_link_diag_t;

/** Copy the link diagnostics snapshot (see wt_tf_link_diag_t; never blocks). */
void wt_tf_get_link_diag(wt_tf_link_diag_t *out);

/**
 * @brief Start a flash session: probe, halt the core, erase the sectors that
 *        cover the image.  Keeps the DAP engine until flash_end()/abort().
 *
 * @param image_size number of bytes that will be streamed by flash_feed().
 * @return ESP_OK / ESP_ERR_INVALID_STATE (busy or already active) /
 *         ESP_ERR_INVALID_SIZE (image larger than the target flash) / ...
 */
esp_err_t wt_tf_flash_begin(uint32_t image_size);

/**
 * @brief Stream image bytes.  Data may be split arbitrarily; the engine
 *        buffers to the family alignment and programs in blocks.
 *
 * @return ESP_OK / ESP_ERR_INVALID_STATE (no active session) /
 *         ESP_ERR_INVALID_SIZE (more than image_size bytes) / ESP_FAIL.
 */
esp_err_t wt_tf_flash_feed(const void *data, size_t len);

/**
 * @brief Flush the last partial block, lock the flash, read the whole image
 *        back and verify SHA-256, then reset the target and release the DAP.
 *
 * @return ESP_OK when the readback SHA-256 matches the streamed data; error
 *         otherwise (status carries the message and err).
 */
esp_err_t wt_tf_flash_end(void);

/**
 * @brief Abort a running session (may be called from another task).
 *
 * @return ESP_OK when an operation was active and the abort was requested;
 *         ESP_ERR_INVALID_STATE when nothing is running.
 */
esp_err_t wt_tf_abort(void);

/**
 * @brief Reset the target over SWD (AIRCR.SYSRESETREQ).
 *
 * @param run true: leave the target running after reset; false: keep it halted.
 */
esp_err_t wt_tf_reset_target(bool run);

/**
 * @brief Copy the current status snapshot (never blocks on a running flash).
 */
void wt_tf_get_status(wt_tf_status_t *out);

/* ------------------------------------------------------------------------- */
/* P1-19: nearby-device discovery (mDNS _http._tcp, read-only)                */
/* ------------------------------------------------------------------------- */

/** Maximum nearby devices returned by one discovery query. */
#define WT_TF_DISCOVER_MAX          8U
#define WT_TF_DISCOVER_NAME_MAX     64U
#define WT_TF_DISCOVER_HOST_MAX     64U
#define WT_TF_DISCOVER_TARGET_MAX   32U
#define WT_TF_DISCOVER_VERSION_MAX  24U
/** Default blocking mDNS query timeout (ms); /api/discover uses <= 2 s. */
#define WT_TF_DISCOVER_TIMEOUT_MS   2000U

/** One nearby DAP discovered through mDNS `_http._tcp`.  Only real values are
 *  stored: `has_rssi=false` means the mDNS answer did not carry an RSSI (the
 *  JSON formatter then omits the field instead of inventing one). */
typedef struct {
    char name[WT_TF_DISCOVER_NAME_MAX];       /**< TXT "name" or instance name */
    char hostname[WT_TF_DISCOVER_HOST_MAX];   /**< SRV target hostname (no trailing '.')
                                                   or empty when unknown */
    char ip[16];                              /**< first IPv4 A record, "" when none */
    uint16_t port;                            /**< SRV port */
    bool has_rssi;                            /**< mDNS has no RSSI by default */
    int8_t rssi;                              /**< dBm when has_rssi */
    /* Optional TXT enrichment; empty when the advertiser does not publish it. */
    char target[WT_TF_DISCOVER_TARGET_MAX];   /**< TXT "target"/"chip" */
    char version[WT_TF_DISCOVER_VERSION_MAX]; /**< TXT "version"/"fw" */
} wt_tf_discover_device_t;

/**
 * @brief Query nearby DAPs via mDNS `_http._tcp` (blocking, read-only).
 *
 * Filters the device's own mDNS answer, keeps only results with an IPv4
 * address and deduplicates by (hostname, port).  Does not take the DAP engine
 * and never touches the target.
 *
 * @param out           caller array (WT_TF_DISCOVER_MAX entries are enough)
 * @param max_devices   array capacity
 * @param timeout_ms    query timeout (clamped to 100..5000 ms; 0 = default 2 s)
 * @return device count >= 0 on success (0 = no mDNS answer / mDNS not ready),
 *         -(int)ESP_ERR_INVALID_ARG (bad arguments) or
 *         -(int)ESP_ERR_INVALID_STATE (another query is already running).
 */
int wt_tf_discover(wt_tf_discover_device_t *out, size_t max_devices,
                   uint32_t timeout_ms);

/**
 * @brief Format the discovery response JSON (pure, host-testable).
 *
 * Output: `{"ok":true,"devices":[{"name":..,"hostname":..,"ip":..,"port":..,
 *          "rssi":..?,"target":..?,"version":..?}, ...]}`.
 * `rssi`/`target`/`version` are omitted when not measured/published; invalid
 * UTF-8 is passed through unchanged (mDNS TXT is UTF-8), control characters
 * are \u-escaped so the response is always valid JSON.
 *
 * @return number of bytes written (excluding NUL), or -1 when buf is too small.
 */
int wt_tf_discover_format_json(const wt_tf_discover_device_t *devices,
                               size_t count, char *buf, size_t buf_len);

/**
 * @brief Query + format the /api/discover response in one call.
 *
 * Single-query guard: while another discovery is running this returns
 * ESP_ERR_INVALID_STATE (the HTTP layer answers 429).  mDNS not running /
 * no answer still returns ESP_OK with `{"ok":true,"devices":[]}`.
 *
 * @param out_len written JSON length (excluding NUL); may be NULL.
 */
esp_err_t wt_tf_discover_json(char *buf, size_t buf_len, uint32_t timeout_ms,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* WT_TARGET_FLASH_H */
