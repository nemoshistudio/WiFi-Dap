/*
 * PROVENANCE (WP-302 host regression, 2026-10-20)
 * --------------------------------------------------------------------------
 * Integrated from the dap-dev ASan host harness at /tmp/dap_host (stub
 * DAP_config.h + FreeRTOS shims, verified 36/36 at the time, 51/51 after the
 * batch-path cases were added; pre-fix DAP.c reproduces N1 as ASan
 * stack-overflow and N9 as ASan heap-buffer-overflow READ of size 1).
 * Source review: docs/security-review-dap.md (N1 P0 / N6 P2 / N9 P2/P3);
 * fixes: commit 666df38 (N1/N9/N6) merged by 89bfd0d on feature/birdiebot-v1.
 * This copy keeps the test code unchanged on purpose; only this provenance
 * header and the build paths in the instructions below were adapted.
 * It is compiled against the *current* repo DAP.c, so it is a live regression.
 */

/* Minimal host stub of components/DAP/DAP_config.h for unit-testing DAP.c
 * command dispatch / packet bounds without ESP32 hardware. */
#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__
#include <stdint.h>
#include <string.h>

#define CPU_CLOCK 240000000U
#define IO_PORT_WRITE_CYCLES 2U
#define DAP_SWD 1
#define DAP_JTAG 1
#define DAP_JTAG_DEV_CNT 8U
#define DAP_DEFAULT_PORT 1U
#define DAP_DEFAULT_SWJ_CLOCK 1000000U
#define DAP_PACKET_COUNT 255
#define DAP_PACKET_SIZE 512U

#define SWO_FUNCTION_ENABLE 0
#define SWO_UART 0
#define SWO_UART_DRIVER 0
#define SWO_UART_MAX_BAUDRATE (115200U * 40U)
#define SWO_MANCHESTER 0
#define SWO_BUFFER_SIZE 2048U
#define SWO_STREAM 0
#define TIMESTAMP_CLOCK 1000000U
#define DAP_UART 0
#define DAP_UART_DRIVER 1
#define DAP_UART_RX_BUFFER_SIZE 1024U
#define DAP_UART_TX_BUFFER_SIZE 1024U
#define DAP_UART_USB_COM_PORT 0
#define TARGET_FIXED 0

#define DELAY_SLOW_CYCLES 3U
#define DELAY_FAST_CYCLES 0U

extern uint32_t TIMESTAMP_GET(void);
#define PIN_DELAY_SLOW(delay) ((void)(delay))

static inline uint32_t PIN_SWCLK_TCK_IN(void) { return 0; }
static inline void PIN_SWCLK_TCK_SET(void) { }
static inline void PIN_SWCLK_TCK_CLR(void) { }
static inline uint32_t PIN_SWDIO_TMS_IN(void) { return 0; }
static inline void PIN_SWDIO_TMS_SET(void) { }
static inline void PIN_SWDIO_TMS_CLR(void) { }
static inline uint32_t PIN_SWDIO_IN(void) { return 0; }
static inline void PIN_SWDIO_OUT(uint32_t bit) { (void)bit; }
static inline void PIN_SWDIO_OUT_ENABLE(void) { }
static inline void PIN_SWDIO_OUT_DISABLE(void) { }
static inline uint32_t PIN_TDI_IN(void) { return 0; }
static inline void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
static inline uint32_t PIN_TDO_IN(void) { return 0; }
static inline uint32_t PIN_nTRST_IN(void) { return 0; }
static inline void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }
static inline uint32_t PIN_nRESET_IN(void) { return 0; }
static inline void PIN_nRESET_OUT(uint32_t bit) { (void)bit; }

static inline void LED_CONNECTED_OUT(uint32_t bit) { (void)bit; }
static inline void LED_RUNNING_OUT(uint32_t bit) { (void)bit; }

static inline void PORT_SWD_SETUP(void) { }
static inline void PORT_JTAG_SETUP(void) { }
static inline void PORT_OFF(void) { }
static inline void DAP_SETUP(void) { }
static inline uint8_t RESET_TARGET(void) { return 1U; }
extern void dap_os_delay(int ms);

static inline uint8_t DAP_GetVendorString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetProductString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetSerNumString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetTargetDeviceNameString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetTargetBoardVendorString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetTargetBoardNameString(char *str) { (void)str; return 0U; }
static inline uint8_t DAP_GetProductFirmwareVersionString(char *str) { (void)str; return 0U; }

#endif
