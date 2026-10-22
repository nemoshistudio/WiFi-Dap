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
 *   - STM32F4 (Cortex-M4, FLASH at 0x08000000): sectors 16K/64K/128K,
 *     x64 parallel programming (PSIZE=3) with automatic x32 fallback;
 *   - STM32H7 (Cortex-M7, dual bank 0x08000000/0x08100000): 128K sectors,
 *     256-bit (32-byte aligned) flash word programming.
 * Other targets are detected but reported as "unknown" (probe fails).
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
                                    flash implementation (F4/H7 today) */
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

#ifdef __cplusplus
}
#endif

#endif /* WT_TARGET_FLASH_H */
