/*
 * Copyright (c) 2013-2017 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * ----------------------------------------------------------------------
 *
 * $Date:        16. June 2021
 * $Revision:    V2.1.0
 *
 * Project:      CMSIS-DAP Configuration
 * Title:        DAP_config.h CMSIS-DAP Configuration File (Template)
 *
 *---------------------------------------------------------------------------*/

/**
 * @file DAP_config.h
 * @author windowsair
 * @brief Adaptation of GPIO and SPI pin
 * @change: 2021-2-10 Support GPIO and SPI
 *          2021-2-18 Try to support SWO
 *          2024-1-28 Update to CMSIS-DAP v2.1.0
 * @version 0.4
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2021-2026
 *
 * WP-101 merge (A 2025 fork + C IDF5 baseline):
 *  - Board pins come from components/board/include/birdiebot_board.h only
 *    (no more per-target PIN_* pin tables in this file).
 *  - A's dedicated-GPIO hooks (SWCLK_SET/SWDIO_SET/...), SWDIO direction
 *    control, pad drive strength and DAP.h low-speed timing fixes are kept.
 *  - C's IDF5 API forms and rtc/esp_timer based timestamp are adapted: the
 *    timestamp source is esp_timer_get_time() (1 us ticks) so that
 *    TIMESTAMP_CLOCK and TIMESTAMP_GET share one unit (DAP_SWJ_Pins wait).
 *  - C3 CPU_CLOCK typo fixed (160 MHz), DEFAULT SWJ clock = 1 MHz.
 *  - ESP8266 support removed.
 */


#ifndef __DAP_CONFIG_H__
#define __DAP_CONFIG_H__

#include <stdint.h>
#include <string.h>

#include "main/dap_configuration.h"

/* Single source of truth for SWD/JTAG/LED pins (WP-006 / docs/board-pins.md).
 * Requires "board" and "esp_timer" in components/DAP/CMakeLists.txt REQUIRES. */
#include "birdiebot_board.h"

/* Runtime probe identity (unique serial, chip/board/fw strings).  The serial is
 * used by DAP_GetSerNumString() below so that host tools can select one probe
 * among several (OpenOCD `adapter serial`, pyOCD `-u`, Keil). */
#include "birdiebot_probe_identity.h"

#include "cmsis-dap/include/cmsis_compiler.h"
#include "cmsis-dap/include/gpio_op.h"
#include "cmsis-dap/include/spi_switch.h"

/* TIMESTAMP_GET() source: esp_timer_get_time() returns microseconds */
#include "esp_timer.h"

#if !defined CONFIG_IDF_TARGET_ESP32 && !defined CONFIG_IDF_TARGET_ESP32C3 && \
    !defined CONFIG_IDF_TARGET_ESP32S3 && !defined CONFIG_IDF_TARGET_ESP32C5
  #error "unsupported target: DAP supports esp32 / esp32c3 / esp32s3 / esp32c5 only (ESP8266 was removed)"
#endif

/* The selected board must match the IDF target (Kconfig already restricts the
 * choices; this catches a stale sdkconfig after set-target). */
#if defined CONFIG_IDF_TARGET_ESP32 && (BIRDIEBOT_BOARD_TARGET_ID != 1)
  #error "selected board is not an ESP32 board"
#elif defined CONFIG_IDF_TARGET_ESP32C3 && (BIRDIEBOT_BOARD_TARGET_ID != 2)
  #error "selected board is not an ESP32-C3 board"
#elif defined CONFIG_IDF_TARGET_ESP32S3 && (BIRDIEBOT_BOARD_TARGET_ID != 3)
  #error "selected board is not an ESP32-S3 board"
#elif defined CONFIG_IDF_TARGET_ESP32C5 && (BIRDIEBOT_BOARD_TARGET_ID != 4)
  #error "selected board is not an ESP32-C5 board"
#endif



//**************************************************************************************************
/**
\defgroup DAP_Config_Debug_gr CMSIS-DAP Debug Unit Information
\ingroup DAP_ConfigIO_gr
@{
Provides definitions about the hardware and configuration of the Debug Unit.

This information includes:
 - Definition of Cortex-M processor parameters used in CMSIS-DAP Debug Unit.
 - Debug Unit Identification strings (Vendor, Product, Serial Number).
 - Debug Unit communication packet size.
 - Debug Access Port supported modes and settings (JTAG/SWD and SWO).
 - Optional information about a connected Target Device (for Evaluation Boards).
*/

//#ifdef _RTE_
//#include "RTE_Components.h"
//#include CMSIS_device_header
//#else
//#include "device.h"                             // Debug Unit Cortex-M Processor Header File
//#endif

/// Processor Clock of the Cortex-M MCU used in the Debug Unit.
/// This value is used to calculate the SWD/JTAG clock speed.
#if defined CONFIG_IDF_TARGET_ESP32
  #define CPU_CLOCK 240000000
  // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<240MHz
#elif defined CONFIG_IDF_TARGET_ESP32C3
  /* WP-101: the C tree had 16000000 (16 MHz) here, a typo. The C3 CPU runs at
   * 160 MHz; this value feeds Delayms()/CLOCK_DELAY() and the default SWD
   * clock calculation. */
  #define CPU_CLOCK 160000000
  // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<160MHz
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
  #define CPU_CLOCK 240000000
#elif defined CONFIG_IDF_TARGET_ESP32C5
  /* ESP32-C5 HP RISC-V core runs up to 240 MHz (docs/esp32-c5-port.md). */
  #define CPU_CLOCK 240000000
#endif



//#define MAX_USER_CLOCK 16000000 ///< Specifies the max Debug Clock in Hz.

/// Number of processor cycles for I/O Port write operations.
/// This value is used to calculate the SWD/JTAG clock speed that is generated with I/O
/// Port write operations in the Debug Unit by a Cortex-M MCU. Most Cortex-M processors
/// require 2 processor cycles for a I/O Port Write operation.  If the Debug Unit uses
/// a Cortex-M0+ processor with high-speed peripheral I/O only 1 processor cycle might be
/// required.
#define IO_PORT_WRITE_CYCLES 2U ///< I/O Cycles: 2=default, 1=Cortex-M0+ fast I/0.

/// Indicate that Serial Wire Debug (SWD) communication mode is available at the Debug Access Port.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_SWD 1 ///< SWD Mode:  1 = available, 0 = not available.

/// Indicate that JTAG communication mode is available at the Debug Port.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_JTAG 1 ///< JTAG Mode: 1 = available, 0 = not available.

/// Configure maximum number of JTAG devices on the scan chain connected to the Debug Access Port.
/// This setting impacts the RAM requirements of the Debug Unit. Valid range is 1 .. 255.
#define DAP_JTAG_DEV_CNT 8U ///< Maximum number of JTAG devices on scan chain.

/// Default communication mode on the Debug Access Port.
/// Used for the command \ref DAP_Connect when Port Default mode is selected.
#define DAP_DEFAULT_PORT 1U ///< Default JTAG/SWJ Port Mode: 1 = SWD, 2 = JTAG.

/// Default communication speed on the Debug Access Port for SWD and JTAG mode.
/// Used to initialize the default SWD/JTAG clock frequency.
/// The command \ref DAP_SWJ_Clock can be used to overwrite this default setting.
/* Project contract (docs/board-pins.md §5): 2 MHz default (hardware-verified
 * stable). Keep a literal fallback in case an old board header is used without
 * the board contract macro. */
#ifndef BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ
  #define BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ 2000000U
#endif
#define DAP_DEFAULT_SWJ_CLOCK BIRDIEBOT_SWD_DEFAULT_CLOCK_HZ ///< Default SWD/JTAG clock frequency in Hz.

/// Maximum Package Buffers for Command and Response data.
/// This configuration settings is used to optimize the communication performance with the
/// debugger and depends on the USB peripheral. For devices with limited RAM or USB buffer the
/// setting can be reduced (valid range is 1 .. 255).
#define DAP_PACKET_COUNT 255 ///< Specifies number of packets buffered.

/// Indicates that the SWO function(UART SWO & Streaming Trace) is available
#define SWO_FUNCTION_ENABLE 0 ///< SWO function:  1 = available, 0 = not available.


/// Indicate that UART Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_UART SWO_FUNCTION_ENABLE ///< SWO UART:  1 = available, 0 = not available.

/// USART Driver instance number for the UART SWO.
#define SWO_UART_DRIVER 0 ///< USART Driver instance number (Driver_USART#).

/// Maximum SWO UART Baudrate.
#define SWO_UART_MAX_BAUDRATE (115200U * 40U) ///< SWO UART Maximum Baudrate in Hz.
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<< 5MHz
//// TODO: uncertain value

/// Indicate that Manchester Serial Wire Output (SWO) trace is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define SWO_MANCHESTER 0 ///< SWO Manchester:  1 = available, 0 = not available.
// (windowsair)Do not modify. Not support.


/// SWO Trace Buffer Size.
#define SWO_BUFFER_SIZE 2048U ///< SWO Trace Buffer Size in bytes (must be 2^n).

/// SWO Streaming Trace.
#define SWO_STREAM SWO_FUNCTION_ENABLE ///< SWO Streaming Trace: 1 = available, 0 = not available.

/// Clock frequency of the Test Domain Timer. Timer value is returned with \ref TIMESTAMP_GET.
/// WP-101: TIMESTAMP_GET() now returns esp_timer_get_time() microseconds, so the
/// declared clock must be 1 MHz (it used to claim 5 MHz while rtc_time_get()
/// returned RTC slow-clock ticks, i.e. a ~150x unit mismatch in SWJ_Pins waits).
#define TIMESTAMP_CLOCK 1000000U ///< Timestamp clock in Hz (0 = timestamps not supported).
// <<<<<<<<<<<<<<<<<<<<<1MHz

/// Indicate that UART Communication Port is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_UART                0               ///< DAP UART:  1 = available, 0 = not available.

/// USART Driver instance number for the UART Communication Port.
#define DAP_UART_DRIVER         1               ///< USART Driver instance number (Driver_USART#).

/// UART Receive Buffer Size.
#define DAP_UART_RX_BUFFER_SIZE 1024U           ///< Uart Receive Buffer Size in bytes (must be 2^n).

/// UART Transmit Buffer Size.
#define DAP_UART_TX_BUFFER_SIZE 1024U           ///< Uart Transmit Buffer Size in bytes (must be 2^n).

/// Indicate that UART Communication via USB COM Port is available.
/// This information is returned by the command \ref DAP_Info as part of <b>Capabilities</b>.
#define DAP_UART_USB_COM_PORT   0               ///< USB COM Port:  1 = available, 0 = not available.

/// Debug Unit is connected to fixed Target Device.
/// The Debug Unit may be part of an evaluation board and always connected to a fixed
/// known device. In this case a Device Vendor, Device Name, Board Vendor and Board Name strings
/// are stored and may be used by the debugger or IDE to configure device parameters.
#define TARGET_FIXED            1               ///< Target: 1 = known, 0 = unknown;

#define TARGET_DEVICE_VENDOR    ""                 ///< String indicating the Silicon Vendor
#define TARGET_DEVICE_NAME      ""                 ///< String indicating the Target Device
#define TARGET_BOARD_VENDOR     "WiFi Dap"          ///< String indicating the Board Vendor
#define TARGET_BOARD_NAME       "WiFi Dap" ///< String indicating the Board Name

#if TARGET_FIXED != 0
#include <string.h>
static const char TargetDeviceVendor [] = TARGET_DEVICE_VENDOR;
static const char TargetDeviceName   [] = TARGET_DEVICE_NAME;
static const char TargetBoardVendor  [] = TARGET_BOARD_VENDOR;
static const char TargetBoardName    [] = TARGET_BOARD_NAME;
#endif

/**
 * @brief Get Vendor ID string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length. (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetVendorString(char *str)
{
  // In fact, Keil can get the corresponding information through USB
  // without filling in this information.
  // (void)str;
  strcpy(str, "windowsair");
  return (sizeof("windowsair"));
}

/**
 * @brief Get Product ID string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length. (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetProductString(char *str)
{
  //(void)str;
  strcpy(str, "CMSIS-DAP v2");
  return (sizeof("CMSIS-DAP v2"));
}

/**
 * @brief Get Serial Number string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length. (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str)
{
  /* Unique per-device serial (WiFi-Dap-<12HEX>) from probe_identity.
   * The old hard-coded "1234" made several probes indistinguishable for
   * OpenOCD `adapter serial` / pyOCD `-u` / Keil.  DAP_Info strings are
   * limited to 60 characters + NUL. */
  const char *serial = birdiebot_probe_serial();
  size_t len = strlen(serial);

  if (len > 60U) {
    len = 60U;
  }
  memcpy(str, serial, len);
  str[len] = '\0';
  return (uint8_t)(len + 1U);
}

/**
 * @brief Get Target Device Vendor string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetDeviceVendor);
  len = (uint8_t)(strlen(TargetDeviceVendor) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/**
 * @brief Get Target Device Name string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetDeviceName);
  len = (uint8_t)(strlen(TargetDeviceName) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/**
 * @brief Get Target Board Vendor string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetBoardVendor);
  len = (uint8_t)(strlen(TargetBoardVendor) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/**
 * @brief Get Target Board Name string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString (char *str) {
#if TARGET_FIXED != 0
  uint8_t len;

  strcpy(str, TargetBoardName);
  len = (uint8_t)(strlen(TargetBoardName) + 1U);
  return (len);
#else
  (void)str;
  return (0U);
#endif
}

/**
 * @brief Get Product Firmware Version string.
 *
 * @param str Pointer to buffer to store the string (max 60 characters).
 * @return String length (including terminating NULL character) or 0 (no string).
 */
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString (char *str) {
  (void)str;
  return (0U);
}

///@}


/*
 * Pin assignment (WP-101):
 *   PIN_SWCLK, PIN_SWDIO_MOSI, PIN_TDO, PIN_TDI, PIN_nTRST, PIN_nRESET,
 *   PIN_LED_CONNECTED, PIN_LED_RUNNING, BIRDIEBOT_BOARD_HAS_STATUS_LEDS
 * are provided by components/board/include/birdiebot_board.h according to the
 * Kconfig-selected board. Do not add per-target pin tables here: the build
 * must not hard-code pins outside components/board/boards/<board>.h
 * (docs/board-pins.md §2 "宏命名契约").
 *
 * Notes:
 *  - USE_SPI_SIO=1 (main/dap_configuration.h) means the SWDIO pin is a single
 *    bidirectional line: input sampling and output driving both use
 *    PIN_SWDIO_MOSI. PIN_SWDIO is only a compatibility alias in the board
 *    header (the A fork's separate ESP32 MISO pin is not used).
 *  - LED macros are "_" (undeclared) on boards without LEDs: always guard with
 *    BIRDIEBOT_BOARD_HAS_STATUS_LEDS / BIRDIEBOT_BOARD_HAS_WIFI_LED.
 */


//**************************************************************************************************
/**
\defgroup DAP_Config_PortIO_gr CMSIS-DAP Hardware I/O Pin Access
\ingroup DAP_ConfigIO_gr
@{

Standard I/O Pins of the CMSIS-DAP Hardware Debug Port support standard JTAG mode
and Serial Wire Debug (SWD) mode. In SWD mode only 2 pins are required to implement the debug
interface of a device. The following I/O Pins are provided:

JTAG I/O Pin                 | SWD I/O Pin          | CMSIS-DAP Hardware pin mode
---------------------------- | -------------------- | ---------------------------------------------
TCK: Test Clock              | SWCLK: Clock         | Output Push/Pull
TMS: Test Mode Select        | SWDIO: Data I/O      | Output Push/Pull; Input (for receiving data)
TDI: Test Data Input         |                      | Output Push/Pull
TDO: Test Data Output        |                      | Input
nTRST: Test Reset (optional) |                      | Output Open Drain with pull-up resistor
nRESET: Device Reset         | nRESET: Device Reset | Output Open Drain with pull-up resistor


DAP Hardware I/O Pin Access Functions
-------------------------------------
The various I/O Pins are accessed by functions that implement the Read, Write, Set, or Clear to
these I/O Pins.

For the SWDIO I/O Pin there are additional functions that are called in SWD I/O mode only.
This functions are provided to achieve faster I/O that is possible with some advanced GPIO
peripherals that can independently write/read a single I/O pin without affecting any other pins
of the same I/O port. The following SWDIO I/O Pin functions are provided:
 - \ref PIN_SWDIO_OUT_ENABLE to enable the output mode from the DAP hardware.
 - \ref PIN_SWDIO_OUT_DISABLE to enable the input mode to the DAP hardware.
 - \ref PIN_SWDIO_IN to read from the SWDIO I/O pin with utmost possible speed.
 - \ref PIN_SWDIO_OUT to write to the SWDIO I/O pin with utmost possible speed.
*/

/**
 * @brief Setup JTAG I/O pins: TCK, TMS, TDI, TDO, nTRST, and nRESET.
 * Configures the DAP Hardware I/O pins for JTAG mode:
 * - TCK, TMS, TDI, nTRST, nRESET to ***output*** mode and set to high level.
 * - TDO to ***input*** mode.
 *
 */
__STATIC_INLINE void PORT_JTAG_SETUP(void)
{
  /* WP-101: never inherit the SPI/IO_MUX state from a previous SWD session.
   * DAP_SPI_Deinit() puts SWCLK/SWDIO back to GPIO mode and (on C3/S3) maps
   * them to the CPU dedicated-GPIO channels used by the PIN_* accessors below;
   * without this, JTAG cold-start on S3/C3 would write CPU GPIO registers that
   * are not routed to the pads (analysis R9). */
  DAP_SPI_Deinit();

  /* TCK/TMS are the shared SWCLK/SWDIO pins and are already configured by
   * DAP_SPI_Deinit(); configure the JTAG-only pins here. */
#if defined CONFIG_IDF_TARGET_ESP32
  // PIN_TDO output disable
  GPIO.enable_w1tc = (0x1 << PIN_TDO);
  // PIN_TDO input enable
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_TDO]);

  // gpio_set_direction(PIN_TDI, GPIO_MODE_OUTPUT);
  GPIO.enable_w1ts = (0x1 << PIN_TDI);
  GPIO.pin[PIN_TDI].pad_driver = 0;
  REG_CLR_BIT(GPIO_PIN_MUX_REG[PIN_TDI], FUN_PD); // disable pull down

  // gpio_set_direction(PIN_nTRST, GPIO_MODE_OUTPUT_OD);
  // gpio_set_direction(PIN_nRESET, GPIO_MODE_OUTPUT_OD);
  GPIO.enable_w1tc = (0x1 << PIN_nTRST);
  GPIO.pin[PIN_nTRST].pad_driver = 1;
  GPIO.enable_w1tc = (0x1 << PIN_nRESET);
  GPIO.pin[PIN_nRESET].pad_driver = 1;

  // gpio_set_pull_mode(PIN_nTRST, GPIO_PULLUP_ONLY);
  // gpio_set_pull_mode(PIN_nRESET, GPIO_PULLUP_ONLY);
  GPIO_PULL_UP_ONLY_SET(PIN_nTRST);
  GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32C3
  // PIN_TDO output disable
  GPIO.enable_w1tc.enable_w1tc = (0x1 << PIN_TDO);
  // PIN_TDO input enable
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_TDO]);

  // gpio_set_direction(PIN_TDI, GPIO_MODE_OUTPUT);
  GPIO.enable_w1ts.enable_w1ts = (0x1 << PIN_TDI);
  GPIO.pin[PIN_TDI].pad_driver = 0;
  REG_CLR_BIT(GPIO_PIN_MUX_REG[PIN_TDI], FUN_PD); // disable pull down

  // gpio_set_direction(PIN_nTRST, GPIO_MODE_OUTPUT_OD);
  // gpio_set_direction(PIN_nRESET, GPIO_MODE_OUTPUT_OD);
  GPIO.enable_w1tc.enable_w1tc = (0x1 << PIN_nTRST);
  GPIO.pin[PIN_nTRST].pad_driver = 1;
  GPIO.enable_w1tc.enable_w1tc = (0x1 << PIN_nRESET);
  GPIO.pin[PIN_nRESET].pad_driver = 1;

  // gpio_set_pull_mode(PIN_nTRST, GPIO_PULLUP_ONLY);
  // gpio_set_pull_mode(PIN_nRESET, GPIO_PULLUP_ONLY);
  GPIO_PULL_UP_ONLY_SET(PIN_nTRST);
  GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
  // PIN_TDO output disable
  gpio_ll_output_disable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_TDO);
  // PIN_TDO input enable
  gpio_ll_input_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_TDO);

  // PIN_TDI output
  gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_TDI);
  gpio_ll_od_disable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_TDI);
  gpio_ll_pulldown_dis(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_TDI);

  gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nTRST);
  gpio_ll_od_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nTRST);
  gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);
  gpio_ll_od_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);

  GPIO_PULL_UP_ONLY_SET(PIN_nTRST);
  GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
#endif
}

/**
 * @brief Setup SWD I/O pins: SWCLK, SWDIO, and nRESET.
 * Configures the DAP Hardware I/O pins for Serial Wire Debug (SWD) mode:
 * - SWCLK, SWDIO, nRESET to output mode and set to default high level.
 * - TDI, nTRST to HighZ mode (pins are unused in SWD mode).
 *
 */
__STATIC_INLINE void PORT_SWD_SETUP(void)
{
  // We will switch to the specific mode when setting the transfer rate.

  // Now we need to set it to ordinary GPIO mode. In most implementations,
  // the DAP will then read the status of the PIN via the `SWJ_PIN` command.
  DAP_SPI_Deinit();
}

/**
 * @brief Disable JTAG/SWD I/O Pins.
 * Disables the DAP Hardware I/O pins which configures:
 * - TCK/SWCLK, TMS/SWDIO, TDI, TDO, nTRST, nRESET to High-Z mode.
 *
 */
__STATIC_INLINE void PORT_OFF(void)
{
  // Will be called when the DAP disconnected.
  // WP-101: DAP_SPI_Disable() was a no-op and was removed together with
  // DAP_SPI_Enable() (WP-103). The pins are put back to GPIO mode by
  // DAP_SPI_Deinit() on the next SWJ_Clock/Connect.
#if defined CONFIG_IDF_TARGET_ESP32
  // gpio_set_direction(PIN_nRESET, GPIO_MODE_OUTPUT_OD);
  GPIO.enable_w1tc = (0x1 << PIN_nRESET);
  GPIO.pin[PIN_nRESET].pad_driver = 1;

  // gpio_set_pull_mode(PIN_nRESET, GPIO_PULLUP_ONLY);
  GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32C3
  // gpio_set_direction(PIN_nRESET, GPIO_MODE_OUTPUT_OD);
  GPIO.enable_w1tc.enable_w1tc = (0x1 << PIN_nRESET);
  GPIO.pin[PIN_nRESET].pad_driver = 1;

  // gpio_set_pull_mode(PIN_nTRST, GPIO_PULLUP_ONLY);
  GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
	gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);
	gpio_ll_od_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);
	GPIO_PULL_UP_ONLY_SET(PIN_nRESET);
	gpio_ll_set_level(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET, 1);
#endif
}

// SWCLK/TCK I/O pin -------------------------------------

/**
 * @brief SWCLK/TCK I/O pin: Get Input.
 *
 * @return Current status of the SWCLK/TCK DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void)
{
  ////TODO: can we set to 0?
  return 0;
}

/**
 * @brief SWCLK/TCK I/O pin: Set Output to High.
 *
 *  Set the SWCLK/TCK DAP hardware I/O pin to high level.
 */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void)
{
#if defined SWCLK_SET
  // A 2025 fork: CPU dedicated-GPIO single-instruction output (S3 ee.* / C3 CSR)
  SWCLK_SET();
#else
  GPIO_SET_LEVEL_HIGH(PIN_SWCLK);
#endif
}

/**
 * @brief SWCLK/TCK I/O pin: Set Output to Low.
 *
 *  Set the SWCLK/TCK DAP hardware I/O pin to low level.
 */
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void)
{
#if defined SWCLK_CLR
  SWCLK_CLR();
#else
  GPIO_SET_LEVEL_LOW(PIN_SWCLK);
#endif
}

// SWDIO/TMS Pin I/O --------------------------------------

/**
 * @brief SWDIO/TMS I/O pin: Get Input.
 *
 * @return Current status of the SWDIO/TMS DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void)
{
  // Note that we only use mosi in GPIO mode (USE_SPI_SIO=1 / single-wire SWDIO)
#if defined SWDIO_GET_IN
  return SWDIO_GET_IN();
#else
  return GPIO_GET_LEVEL(PIN_SWDIO_MOSI);
#endif
}

/**
 * @brief SWDIO/TMS I/O pin: Set Output to High.
 *
 * Set the SWDIO/TMS DAP hardware I/O pin to high level.
 */
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void)
{
#if defined SWDIO_SET
  SWDIO_SET();
#else
  GPIO_SET_LEVEL_HIGH(PIN_SWDIO_MOSI);
#endif
}

/**
 * @brief SWDIO/TMS I/O pin: Set Output to Low.
 *
 * Set the SWDIO/TMS DAP hardware I/O pin to low level.
 */
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void)
{
#if defined SWDIO_CLR
  SWDIO_CLR();
#else
  GPIO_SET_LEVEL_LOW(PIN_SWDIO_MOSI);
#endif
}

/**
 * @brief SWDIO I/O pin: Get Input (used in SWD mode only).
 *
 * @return Current status of the SWDIO DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void)
{
  return PIN_SWDIO_TMS_IN();
}

/**
 * @brief SWDIO I/O pin: Set Output (used in SWD mode only).
 *
 * @param bit Output value for the SWDIO DAP hardware I/O pin.
 *
 */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit)
{
  /**
    * Important: Use only one bit (bit0) of param!
	  * Sometimes the func "SWD_TransferFunction" of SW_DP.c will
	  * issue "2" as param instead of "0". Zach Lee
	  */
  if ((bit & 1U) == 1)
  {
    PIN_SWDIO_TMS_SET();
  }
  else
  {
    PIN_SWDIO_TMS_CLR();
  }
}

/**
 * @brief SWDIO I/O pin: Switch to Output mode (used in SWD mode only).
 * Configure the SWDIO DAP hardware I/O pin to output mode. This function is
 * called prior \ref PIN_SWDIO_OUT function calls.
 */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void)
{
  // set \ref gpio_set_direction -> OUTPUT
#if defined CONFIG_IDF_TARGET_ESP32
  GPIO.enable_w1ts = (0x01 << PIN_SWDIO_MOSI);
#elif defined CONFIG_IDF_TARGET_ESP32C3
  // A 2025 fork: dedicated-GPIO OEN user CSR bit 0
  SWDIO_OUT_ENABLE();
#elif defined CONFIG_IDF_TARGET_ESP32C5
  /* ESP32-C5 uses the same RISC-V dedicated-GPIO OEN CSR as C3. */
  SWDIO_OUT_ENABLE();
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
  /* A 2025 behaviour: SWDIO pad OEN stays enabled from DAP_SPI_Deinit().
   * Toggling it here was tested on hardware and broke the second SWD
   * transaction (JUNK ACK); keep the proven implementation. */
#endif
}

/**
 * @brief SWDIO I/O pin: Switch to Input mode (used in SWD mode only).
 * Configure the SWDIO DAP hardware I/O pin to input mode. This function is
 * called prior \ref PIN_SWDIO_IN function calls.
 */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void)
{
  // set \ref gpio_set_direction -> INPUT
#if defined CONFIG_IDF_TARGET_ESP32
  // Note that the input of esp32 is not always connected.
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);
  GPIO.enable_w1tc = (0x01 << PIN_SWDIO_MOSI);
#elif defined CONFIG_IDF_TARGET_ESP32C3
  // A 2025 fork: dedicated-GPIO OEN user CSR bit 0
  SWDIO_OUT_DISABLE();
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);
#elif defined CONFIG_IDF_TARGET_ESP32C5
  /* ESP32-C5: RISC-V dedicated-GPIO OEN CSR + GPIO matrix input enable. */
  SWDIO_OUT_DISABLE();
  gpio_ll_input_enable(&GPIO, PIN_SWDIO_MOSI);
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
  /* A 2025 behaviour: see PIN_SWDIO_OUT_ENABLE above. */
#endif
}

// TDI Pin I/O ---------------------------------------------

/**
 * @brief TDI I/O pin: Get Input.
 *
 * @return Current status of the TDI DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void)
{
  return GPIO_GET_LEVEL(PIN_TDI);
}

/**
 * @brief TDI I/O pin: Set Output.
 *
 * @param bit Output value for the TDI DAP hardware I/O pin.
 *
 */
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit)
{
  if ((bit & 1U) == 1)
  {
    //set bit
    GPIO_SET_LEVEL_HIGH(PIN_TDI);
  }
  else
  {
    //reset bit
    GPIO_SET_LEVEL_LOW(PIN_TDI);
  }
}

// TDO Pin I/O ---------------------------------------------

/**
 * @brief TDO I/O pin: Get Input.
 *
 * @return Current status of the TDO DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void)
{
#if defined CONFIG_IDF_TARGET_ESP32
  return ((GPIO.in >> PIN_TDO) & 0x1) ? 1 : 0;
#else
  return GPIO_GET_LEVEL(PIN_TDO);
#endif
}

// nTRST Pin I/O -------------------------------------------

/**
 * @brief nTRST I/O pin: Get Input.
 *
 * @return Current status of the nTRST DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void)
{
  return 0;  // not available
}

/**
 * @brief nTRST I/O pin: Set Output.
 *
 * @param bit JTAG TRST Test Reset pin status:
 *         - 0: issue a JTAG TRST Test Reset.
           - 1: release JTAG TRST Test Reset.
 */
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit)
{
  // Vendor reset sequence
  ; // not available
}

// nRESET Pin I/O------------------------------------------

/**
 * @brief nRESET I/O pin: Get Input.
 *
 * @return Current status of the nRESET DAP hardware I/O pin.
 */
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void)
{
  return GPIO_GET_LEVEL(PIN_nRESET);
}

/**
 * @brief nRESET I/O pin: Set Output.
 *
 * @param bit target device hardware reset pin status:
 *            - 0: issue a device hardware reset.
 *            - 1: release device hardware reset.
 */
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
  // Vendor reset sequence
  //// FIXME: unavailable
  if ((bit & 1U) == 1)
  {
    //set bit
    GPIO_SET_LEVEL_HIGH(PIN_nRESET);
#if defined CONFIG_IDF_TARGET_ESP32
    GPIO.enable_w1tc = (0x01 << PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32C3
    GPIO.enable_w1tc.enable_w1tc |= (0x01 << PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
	gpio_ll_output_disable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);
#endif
  }
  else
  {
    //reset bit
#if defined CONFIG_IDF_TARGET_ESP32
    GPIO.enable_w1ts = (0x01 << PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32C3
    GPIO.enable_w1ts.enable_w1ts |= (0x01 << PIN_nRESET);
#elif defined CONFIG_IDF_TARGET_ESP32S3 || defined CONFIG_IDF_TARGET_ESP32C5
    gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_nRESET);
#endif
    GPIO_SET_LEVEL_LOW(PIN_nRESET);
  }
}

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_LEDs_gr CMSIS-DAP Hardware Status LEDs
\ingroup DAP_ConfigIO_gr
@{

CMSIS-DAP Hardware may provide LEDs that indicate the status of the CMSIS-DAP Debug Unit.

It is recommended to provide the following LEDs for status indication:
 - Connect LED: is active when the DAP hardware is connected to a debugger.
 - Running LED: is active when the debugger has put the target device into running state.
*/

/** Debug Unit: Set status of Connected LED.
\param bit status of the Connect LED.
           - 1: Connect LED ON: debugger is connected to CMSIS-DAP Debug Unit.
           - 0: Connect LED OFF: debugger is not connected to CMSIS-DAP Debug Unit.
*/

/**
 * @brief Debug Unit: Set status of Connected LED.
 *
 * @param bit status of the Connect LED.
 *        - 1: Connect LED ON: debugger is connected to CMSIS-DAP Debug Unit.
 *        - 0: Connect LED OFF: debugger is not connected to CMSIS-DAP Debug Unit.
 */
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit)
{
#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
  if (bit)
  {
    GPIO_SET_LEVEL_HIGH(PIN_LED_CONNECTED);
  }
  else
  {
    GPIO_SET_LEVEL_LOW(PIN_LED_CONNECTED);
  }
#else
  /* no Connected LED on the selected board */
  (void)(bit);
#endif
}

/**
 * @brief Debug Unit: Set status Target Running LED.
 *
 * @param bit status of the Target Running LED.
 *        - 1: Target Running LED ON: program execution in target started.
 *        - 0: Target Running LED OFF: program execution in target stopped.
 */
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit)
{
#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
  if (bit)
  {
    GPIO_SET_LEVEL_HIGH(PIN_LED_RUNNING);
  }
  else
  {
    GPIO_SET_LEVEL_LOW(PIN_LED_RUNNING);
  }
#else
  /* no Running LED on the selected board */
  (void)(bit);
#endif
}

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_Timestamp_gr CMSIS-DAP Timestamp
\ingroup DAP_ConfigIO_gr
@{
Access function for Test Domain Timer.

The value of the Test Domain Timer in the Debug Unit is returned by the function \ref TIMESTAMP_GET. By
default, the DWT timer is used.  The frequency of this timer is configured with \ref TIMESTAMP_CLOCK.

*/

/**
 * @brief Get timestamp of Test Domain Timer.
 *
 * @return Current timestamp value.
 */
__STATIC_INLINE uint32_t TIMESTAMP_GET(void)
{
  /* WP-101: esp_timer_get_time() returns microseconds since boot, matching
   * TIMESTAMP_CLOCK=1 MHz. The 64-bit value is truncated to 32 bits like the
   * CMSIS-DAP reference timer (all in-protocol users only look at differences).
   * Note: this is called from the DAP task context only (DAP_SWJ_Pins wait). */
  return (uint32_t)esp_timer_get_time();
}

///@}

//**************************************************************************************************
/**
\defgroup DAP_Config_Initialization_gr CMSIS-DAP Initialization
\ingroup DAP_ConfigIO_gr
@{

CMSIS-DAP Hardware I/O and LED Pins are initialized with the function \ref DAP_SETUP.
*/

/** Setup of the Debug Unit I/O pins and LEDs (called when Debug Unit is initialized).
This function performs the initialization of the CMSIS-DAP Hardware I/O Pins and the
Status LEDs. In detail the operation of Hardware I/O and LED pins are enabled and set:
 - I/O clock system enabled.
 - all I/O pins: input buffer enabled, output pins are set to HighZ mode.
 - for nTRST, nRESET a weak pull-up (if available) is enabled.
 - LED output pins are enabled and LEDs are turned off.
*/
__STATIC_INLINE void DAP_SETUP(void)
{
  // Connecting non-SWD pins to GPIO
  GPIO_FUNCTION_SET(PIN_TDO);
  GPIO_FUNCTION_SET(PIN_TDI);
  GPIO_FUNCTION_SET(PIN_nTRST);
  GPIO_FUNCTION_SET(PIN_nRESET);

  /**
   * The drive strength has a significant impact on signal integrity.
   * In actual use, it is necessary to perform signal measurements
   * on the board and select the appropriate drive strength.
   * (A 2025 fork: 5 mA for esp32c3/esp32s3; ESP32-C5 keeps the same setting;
   *  C5 is hardware-validated, see docs/hil-gate.md §11.)
   */
#if defined CONFIG_IDF_TARGET_ESP32C3 || defined CONFIG_IDF_TARGET_ESP32S3 || \
    defined CONFIG_IDF_TARGET_ESP32C5
  gpio_ll_set_drive_capability(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWCLK, GPIO_DRIVE_CAP_0);
  gpio_ll_set_drive_capability(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI, GPIO_DRIVE_CAP_0);
#endif

  // Configure status LEDs as output and turn them off (board-dependent).
#if BIRDIEBOT_BOARD_HAS_STATUS_LEDS
  GPIO_FUNCTION_SET(PIN_LED_CONNECTED);
  GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_CONNECTED);
  GPIO_FUNCTION_SET(PIN_LED_RUNNING);
  GPIO_SET_DIRECTION_NORMAL_OUT(PIN_LED_RUNNING);
  LED_CONNECTED_OUT(0);
  LED_RUNNING_OUT(0);
#endif

  PORT_OFF();
}

/* minimum 1 tick delay */
extern void dap_os_delay(int ms);
/** Reset Target Device with custom specific I/O pin or command sequence.
This function allows the optional implementation of a device specific reset sequence.
It is called when the command \ref DAP_ResetTarget and is for example required
when a device needs a time-critical unlock sequence that enables the debug port.
\return 0 = no device specific reset sequence is implemented.\n
        1 = a device specific reset sequence is implemented.
*/
__STATIC_INLINE uint8_t RESET_TARGET(void)
{

  PIN_nRESET_OUT(0);
  dap_os_delay(2);
  PIN_nRESET_OUT(1);
  dap_os_delay(2);
  return (1U); // OK
}

///@}

#endif /* __DAP_CONFIG_H__ */
