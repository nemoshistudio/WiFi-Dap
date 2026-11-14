/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WiFi Dap probe identity: one runtime source of truth for the probe serial
 * number, chip name, board name and firmware version.
 *
 * The serial is derived from the factory WiFi STA MAC so that several probes
 * can be distinguished by host tools (OpenOCD `adapter serial`, pyOCD `-u`,
 * Keil).  It is exposed through:
 *   - CMSIS-DAP DAP_Info SER_NUM (components/DAP/DAP_config.h),
 *   - the USB/IP virtual-device string descriptor (components/USBIP),
 *   - mDNS TXT records (project_components/wt_mdns),
 *   - the boot log and the existing SYSTEM/GET_SYS_INFO.sn field (12 hex chars,
 *     unchanged protocol contract).
 *
 * The formatting helper lives in birdiebot_probe_identity_format.c and is
 * ESP-IDF-free so it can be unit-tested on the host.
 */

#ifndef BIRDIEBOT_PROBE_IDENTITY_H
#define BIRDIEBOT_PROBE_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Serial prefix; the final serial is "WiFi-Dap-" + 12 uppercase hex. */
#define BIRDIEBOT_PROBE_SERIAL_PREFIX "WiFi-Dap-"
/** Number of hex characters (full 6-byte WiFi STA MAC). */
#define BIRDIEBOT_PROBE_SERIAL_HEX_CHARS 12
/** Serial string length without the trailing NUL ("WiFi-Dap-" + MAC). */
#define BIRDIEBOT_PROBE_SERIAL_LEN \
  (sizeof(BIRDIEBOT_PROBE_SERIAL_PREFIX) - 1U + BIRDIEBOT_PROBE_SERIAL_HEX_CHARS)
/** Buffer capacity including the trailing NUL. */
#define BIRDIEBOT_PROBE_SERIAL_CAPACITY (BIRDIEBOT_PROBE_SERIAL_LEN + 1U)

/**
 * @brief Format a probe serial from a 6-byte MAC (pure, no ESP-IDF).
 *
 * Output is "WiFi-Dap-<12 uppercase hex>", e.g.
 * "WiFi-Dap-001122334455".
 *
 * @param mac     6-byte MAC address (big-endian byte order as reported by
 *                esp_read_mac()).
 * @param out     Destination buffer.
 * @param capacity Destination capacity in bytes (must be at least
 *                BIRDIEBOT_PROBE_SERIAL_CAPACITY).
 * @return Number of characters written without the NUL, or 0 on error.
 */
size_t birdiebot_probe_format_serial(const uint8_t mac[6], char *out,
                                     size_t capacity);

/**
 * @brief Initialise the identity (idempotent; called from app_main()).
 *
 * Reads the factory WiFi STA MAC (fallback serial all-zero MAC with a warning)
 * and the running app description version.  Getters are safe to call before
 * this function: they lazily initialise.
 */
void birdiebot_probe_identity_init(void);

/** @return "WiFi-Dap-<12HEX>" (never NULL, stable for the boot). */
const char *birdiebot_probe_serial(void);

/** @return Human-readable chip name, e.g. "ESP32-S3". */
const char *birdiebot_probe_chip(void);

/** @return Selected board id, e.g. "wifi_dap_s3". */
const char *birdiebot_probe_board(void);

/** @return Running firmware version from esp_app_desc (e.g. v1.2.0-...). */
const char *birdiebot_probe_fw_version(void);

#ifdef __cplusplus
}
#endif

#endif /* BIRDIEBOT_PROBE_IDENTITY_H */
