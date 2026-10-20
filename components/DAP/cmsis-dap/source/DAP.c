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
 * Project:      CMSIS-DAP Source
 * Title:        DAP.c CMSIS-DAP Commands
 *
 *---------------------------------------------------------------------------*/

/*
 * WP-105 merge notes:
 *  - A 2025 fork baseline (the fork itself only removed the __WEAK vendor
 *    default implementation) + C-tree IDF5/FreeRTOS adaptations.
 *  - Hardening added for the network-exposed command path:
 *      * DAP_Transfer request pre-validation (packet bounds + response size)
 *      * DAP_TransferBlock request_count cap (128 KB response/request writes
 *        were reachable from a 2-byte count before)
 *      * JTAG_Configure count <= DAP_JTAG_DEV_CNT
 *      * SWD_Sequence / JTAG_Sequence request+response packet bounds
 *      * DAP_ExecuteCommand accumulated-response cap (DAP_PACKET_SIZE)
 *      * real DAP_TransferAbort(0x07) handling
 *      * 0x7E/0x7F handling in DAP_ProcessCommand (Atomic Commands capability
 *        is advertised, but the USBIP path called DAP_ProcessCommand directly)
 *  - SWJ clock: BUS_CLOCK_FIXED for esp32/s3 corrected to 80 MHz APB and the
 *    timestamp unit mismatch fixed in DAP_config.h (TIMESTAMP_CLOCK=1 MHz).
 */

#include <string.h>

#include "DAP_config.h"
#include "cmsis-dap/include/DAP.h"
#include "cmsis-dap/include/dap_stats.h"
#include "cmsis-dap/include/spi_switch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if (DAP_PACKET_SIZE < 64U)
#error "Minimum Packet Size is 64!"
#endif
#if (DAP_PACKET_SIZE > 32768U)
#error "Maximum Packet Size is 32768!"
#endif
#if (DAP_PACKET_COUNT < 1U)
#error "Minimum Packet Count is 1!"
#endif
#if (DAP_PACKET_COUNT > 255U)
#error "Maximum Packet Count is 255!"
#endif


// Clock Macros

#define MAX_SWJ_CLOCK(delay_cycles) \
  ((CPU_CLOCK/2U) / (IO_PORT_WRITE_CYCLES + delay_cycles))

#define CLOCK_DELAY(swj_clock) \
 (((CPU_CLOCK/2U) / swj_clock) - IO_PORT_WRITE_CYCLES)


         DAP_Data_t DAP_Data;           // DAP Data
volatile uint8_t    DAP_TransferAbort;  // Transfer Abort Flag

/* WP-105 packet/response bound helpers (defined below) */
static uint8_t  DAP_SequenceRequestIsValid(const uint8_t *request,
                                           uint32_t count_mask,
                                           uint32_t capture_flag,
                                           uint8_t request_always);
static uint8_t  DAP_TransferRequestIsValid(const uint8_t *request);
static uint32_t DAP_ResponseMaxSize(const uint8_t *request);

/* N1 (security review): depth-aware command dispatch. Only a top-level command
 * may enter DAP_ExecuteCommands; sub-commands run with depth >= 1 and must not
 * nest another 0x7E/0x7F (before this guard, DAP_ProcessCommand(0x7E) called
 * DAP_ExecuteCommand which fell back to DAP_ProcessCommand: unbounded mutual
 * recursion from a single remote packet). */
static uint32_t DAP_ProcessCommandDepth(const uint8_t *request, uint8_t *response, uint8_t depth);
static uint32_t DAP_ExecuteCommandDepth(const uint8_t *request, uint8_t *response, uint8_t depth);


static const char DAP_FW_Ver [] = DAP_FW_VER;



// Get DAP Information
//   id:      info identifier
//   info:    pointer to info data
//   return:  number of bytes in info data
static uint8_t DAP_Info(uint8_t id, uint8_t *info) {
  uint8_t length = 0U;

  switch (id) {
    case DAP_ID_VENDOR:
      length = DAP_GetVendorString((char *)info);
      break;
    case DAP_ID_PRODUCT:
      length = DAP_GetProductString((char *)info);
      break;
    case DAP_ID_SER_NUM:
      length = DAP_GetSerNumString((char *)info);
      break;
    case DAP_ID_DAP_FW_VER:
      length = (uint8_t)sizeof(DAP_FW_Ver);
      memcpy(info, DAP_FW_Ver, length);
      break;
    case DAP_ID_DEVICE_VENDOR:
      length = DAP_GetTargetDeviceVendorString((char *)info);
      break;
    case DAP_ID_DEVICE_NAME:
      length = DAP_GetTargetDeviceNameString((char *)info);
      break;
    case DAP_ID_BOARD_VENDOR:
      length = DAP_GetTargetBoardVendorString((char *)info);
      break;
    case DAP_ID_BOARD_NAME:
      length = DAP_GetTargetBoardNameString((char *)info);
      break;
    case DAP_ID_PRODUCT_FW_VER:
      length = DAP_GetProductFirmwareVersionString((char *)info);
      break;
    case DAP_ID_CAPABILITIES:
      info[0] = ((DAP_SWD  != 0)         ? (1U << 0) : 0U) |
                ((DAP_JTAG != 0)         ? (1U << 1) : 0U) |
                ((SWO_UART != 0)         ? (1U << 2) : 0U) |
                ((SWO_MANCHESTER != 0)   ? (1U << 3) : 0U) |
                /* Atomic Commands  */     (1U << 4)       |
                ((TIMESTAMP_CLOCK != 0U) ? (1U << 5) : 0U) |
                ((SWO_STREAM != 0U)      ? (1U << 6) : 0U) |
                ((DAP_UART != 0U)        ? (1U << 7) : 0U);

      info[1] = ((DAP_UART_USB_COM_PORT != 0) ? (1U << 0) : 0U);
      length = 2U;
      break;
    case DAP_ID_TIMESTAMP_CLOCK:
#if (TIMESTAMP_CLOCK != 0U)
      info[0] = (uint8_t)(TIMESTAMP_CLOCK >>  0);
      info[1] = (uint8_t)(TIMESTAMP_CLOCK >>  8);
      info[2] = (uint8_t)(TIMESTAMP_CLOCK >> 16);
      info[3] = (uint8_t)(TIMESTAMP_CLOCK >> 24);
      length = 4U;
#endif
      break;
    case DAP_ID_UART_RX_BUFFER_SIZE:
#if (DAP_UART != 0)
      info[0] = (uint8_t)(DAP_UART_RX_BUFFER_SIZE >>  0);
      info[1] = (uint8_t)(DAP_UART_RX_BUFFER_SIZE >>  8);
      info[2] = (uint8_t)(DAP_UART_RX_BUFFER_SIZE >> 16);
      info[3] = (uint8_t)(DAP_UART_RX_BUFFER_SIZE >> 24);
      length = 4U;
#endif
      break;
    case DAP_ID_UART_TX_BUFFER_SIZE:
#if (DAP_UART != 0)
      info[0] = (uint8_t)(DAP_UART_TX_BUFFER_SIZE >>  0);
      info[1] = (uint8_t)(DAP_UART_TX_BUFFER_SIZE >>  8);
      info[2] = (uint8_t)(DAP_UART_TX_BUFFER_SIZE >> 16);
      info[3] = (uint8_t)(DAP_UART_TX_BUFFER_SIZE >> 24);
      length = 4U;
#endif
      break;
    case DAP_ID_SWO_BUFFER_SIZE:
#if ((SWO_UART != 0) || (SWO_MANCHESTER != 0))
      info[0] = (uint8_t)(SWO_BUFFER_SIZE >>  0);
      info[1] = (uint8_t)(SWO_BUFFER_SIZE >>  8);
      info[2] = (uint8_t)(SWO_BUFFER_SIZE >> 16);
      info[3] = (uint8_t)(SWO_BUFFER_SIZE >> 24);
      length = 4U;
#endif
      break;
    case DAP_ID_PACKET_SIZE:
      info[0] = (uint8_t)(DAP_PACKET_SIZE >> 0);
      info[1] = (uint8_t)(DAP_PACKET_SIZE >> 8);
      length = 2U;
      break;
    case DAP_ID_PACKET_COUNT:
      info[0] = DAP_PACKET_COUNT;
      length = 1U;
      break;
    default:
      break;
  }

  return (length);
}


// Delay for specified time
//    delay:  delay time in ms
void Delayms(uint32_t delay) {
  delay *= ((CPU_CLOCK/1000U) + (DELAY_SLOW_CYCLES-1U)) / DELAY_SLOW_CYCLES;
  PIN_DELAY_SLOW(delay);
}


// Process Delay command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_Delay(const uint8_t *request, uint8_t *response) {
  uint32_t delay;

  delay  = (uint32_t)(*(request+0)) |
           (uint32_t)(*(request+1) << 8);
  delay *= ((CPU_CLOCK/1000000U) + (DELAY_SLOW_CYCLES-1U)) / DELAY_SLOW_CYCLES;

  PIN_DELAY_SLOW(delay);

  *response = DAP_OK;
  return ((2U << 16) | 1U);
}


// Process Host Status command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_HostStatus(const uint8_t *request, uint8_t *response) {

  switch (*request) {
    case DAP_DEBUGGER_CONNECTED:
      LED_CONNECTED_OUT((*(request+1) & 1U));
      break;
    case DAP_TARGET_RUNNING:
      LED_RUNNING_OUT((*(request+1) & 1U));
      break;
    default:
      *response = DAP_ERROR;
      return ((2U << 16) | 1U);
  }

  *response = DAP_OK;
  return ((2U << 16) | 1U);
}


// Process Connect command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_Connect(const uint8_t *request, uint8_t *response) {
  uint32_t port;

  if (*request == DAP_PORT_AUTODETECT) {
    port = DAP_DEFAULT_PORT;
  } else {
    port = *request;
  }

  switch (port) {
#if (DAP_SWD != 0)
    case DAP_PORT_SWD:
      DAP_Data.debug_port = DAP_PORT_SWD;
      if (SWD_TransferSpeed != kTransfer_SPI)
        PORT_SWD_SETUP();
      break;
#endif
#if (DAP_JTAG != 0)
    case DAP_PORT_JTAG:
      DAP_Data.debug_port = DAP_PORT_JTAG;
      PORT_JTAG_SETUP();
      break;
#endif
    default:
      port = DAP_PORT_DISABLED;
      break;
  }

  *response = (uint8_t)port;
  return ((1U << 16) | 1U);
}


// Process Disconnect command and prepare response
//   response: pointer to response data
//   return:   number of bytes in response
static uint32_t DAP_Disconnect(uint8_t *response) {

  DAP_Data.debug_port = DAP_PORT_DISABLED;
  PORT_OFF();

  *response = DAP_OK;
  return (1U);
}


// Process Reset Target command and prepare response
//   response: pointer to response data
//   return:   number of bytes in response
/** Optional AIRCR.SYSRESETREQ workaround for targets whose nRESET is not
 * wired. main/dap_configuration.h used to define this as 0 (A tree) while the
 * C tree made the write unconditional; keep the A behaviour (the WiFi Dap
 * board has a real nRESET line) but allow enabling it from the config. */
#ifndef USE_FORCE_SYSRESETREQ_AFTER_FLASH
#define USE_FORCE_SYSRESETREQ_AFTER_FLASH 0
#endif

static uint32_t DAP_ResetTarget(uint8_t *response) {

#if (USE_FORCE_SYSRESETREQ_AFTER_FLASH)
  if (DAP_Data.debug_port == DAP_PORT_SWD) {
    /* Workaround for software reset when nRESET is not connected */
    uint8_t ack;
    uint32_t AIRCR_REG_ADDR = 0xE000ED0C;
    uint32_t AIRCR_RESET_VAL = 0x05FA << 16 | 1 << 2; /* Vector key | SYSRESETREQ bit */
    uint8_t req = DAP_TRANSFER_APnDP | 0 | DAP_TRANSFER_A2 | 0;
    ack = SWD_Transfer(req, &AIRCR_REG_ADDR);
    if (ack == DAP_TRANSFER_OK) {
      dap_os_delay(2);
      req = DAP_TRANSFER_APnDP | 0 | DAP_TRANSFER_A2 | DAP_TRANSFER_A3;
      SWD_Transfer(req, &AIRCR_RESET_VAL);
    }
  }
#endif


  *(response+1) = RESET_TARGET();
  *(response+0) = DAP_OK;
  return (2U);
}


// Process SWJ Pins command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_SWJ_Pins(const uint8_t *request, uint8_t *response) {
#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
  if (SWD_TransferSpeed == kTransfer_SPI)
    DAP_SPI_Deinit();

  uint32_t value;
  uint32_t select;
  uint32_t wait;
  uint32_t timestamp;

  value  = (uint32_t) *(request+0);
  select = (uint32_t) *(request+1);
  wait   = (uint32_t)(*(request+2) <<  0) |
           (uint32_t)(*(request+3) <<  8) |
           (uint32_t)(*(request+4) << 16) |
           ((uint32_t)*(request+5) << 24);

  if ((select & (1U << DAP_SWJ_SWCLK_TCK)) != 0U) {
    if ((value & (1U << DAP_SWJ_SWCLK_TCK)) != 0U) {
      PIN_SWCLK_TCK_SET();
    } else {
      PIN_SWCLK_TCK_CLR();
    }
  }
  if ((select & (1U << DAP_SWJ_SWDIO_TMS)) != 0U) {
    if ((value & (1U << DAP_SWJ_SWDIO_TMS)) != 0U) {
      PIN_SWDIO_TMS_SET();
    } else {
      PIN_SWDIO_TMS_CLR();
    }
  }
  if ((select & (1U << DAP_SWJ_TDI)) != 0U) {
    PIN_TDI_OUT(value >> DAP_SWJ_TDI);
  }
  if ((select & (1U << DAP_SWJ_nTRST)) != 0U) {
    PIN_nTRST_OUT(value >> DAP_SWJ_nTRST);
  }
  if ((select & (1U << DAP_SWJ_nRESET)) != 0U){
    PIN_nRESET_OUT(value >> DAP_SWJ_nRESET);
  }

  if (wait != 0U) {
#if (TIMESTAMP_CLOCK != 0U)
    /* Security review N6: the CMSIS-DAP "Pin Wait" field is a timeout in
     * MICROSECONDS (0 = no wait, max 3,000,000 us = 3 s), not milliseconds:
     * https://arm-software.github.io/CMSIS-DAP/latest/group__DAP__SWJ__Pins.html
     * TIMESTAMP_GET() returns microseconds with TIMESTAMP_CLOCK = 1 MHz, so the
     * old `wait *= TIMESTAMP_CLOCK / 1000000` (= *1) was already correct for
     * this configuration. Use an exact 64-bit us->tick conversion anyway so
     * the value stays correct for any future TIMESTAMP_CLOCK (e.g. a 32768 Hz
     * RTC where the old integer division would have produced 0). */
    if (wait > 3000000U) {
      wait = 3000000U;
    }
    wait = (uint32_t)(((uint64_t) wait * (uint64_t) TIMESTAMP_CLOCK) / 1000000U);
    if (wait == 0U) {
      wait = 1U;
    }
#else
    wait  = 1U;
#endif
    timestamp = TIMESTAMP_GET();
    do {
      if ((select & (1U << DAP_SWJ_SWCLK_TCK)) != 0U) {
        if ((value >> DAP_SWJ_SWCLK_TCK) ^ PIN_SWCLK_TCK_IN()) {
          continue;
        }
      }
      if ((select & (1U << DAP_SWJ_SWDIO_TMS)) != 0U) {
        if ((value >> DAP_SWJ_SWDIO_TMS) ^ PIN_SWDIO_TMS_IN()) {
          continue;
        }
      }
      if ((select & (1U << DAP_SWJ_TDI)) != 0U) {
        if ((value >> DAP_SWJ_TDI) ^ PIN_TDI_IN()) {
          continue;
        }
      }
      if ((select & (1U << DAP_SWJ_nTRST)) != 0U) {
        if ((value >> DAP_SWJ_nTRST) ^ PIN_nTRST_IN()) {
          continue;
        }
      }
      if ((select & (1U << DAP_SWJ_nRESET)) != 0U) {
        if ((value >> DAP_SWJ_nRESET) ^ PIN_nRESET_IN()) {
          continue;
        }
      }
      break;
    } while ((TIMESTAMP_GET() - timestamp) < wait);
  }

  value = (PIN_SWCLK_TCK_IN() << DAP_SWJ_SWCLK_TCK) |
          (PIN_SWDIO_TMS_IN() << DAP_SWJ_SWDIO_TMS) |
          (PIN_TDI_IN()       << DAP_SWJ_TDI)       |
          (PIN_TDO_IN()       << DAP_SWJ_TDO)       |
          (PIN_nTRST_IN()     << DAP_SWJ_nTRST)     |
          (PIN_nRESET_IN()    << DAP_SWJ_nRESET);

  *response = (uint8_t)value;
#else
  *response = 0U;
#endif

  if (SWD_TransferSpeed == kTransfer_SPI) // restore
    DAP_SPI_Init();

  return ((6U << 16) | 1U);
}

extern uint8_t SWD_TransferSpeed;

// Process SWJ Clock command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_SWJ_Clock(const uint8_t *request, uint8_t *response) {
#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
  uint32_t clock;
  uint32_t delay;

  clock = (uint32_t)(*(request+0) <<  0) |
          (uint32_t)(*(request+1) <<  8) |
          (uint32_t)(*(request+2) << 16) |
          ((uint32_t)*(request+3) << 24);

  if (clock == 0U) {
    *response = DAP_ERROR;
    return ((4U << 16) | 1U);
  }

  // Note: the supported targets are esp32 (APB 80 MHz), esp32c3 (80 MHz) and
  // esp32s3 (80 MHz); see BUS_CLOCK_FIXED below.

  // clock >= 10MHz -> use 40MHz SPI
  if (clock >= 10000000) {
    if (DAP_Data.debug_port != DAP_PORT_JTAG) {
      DAP_SPI_Init();
      SWD_TransferSpeed = kTransfer_SPI;
    } else {
      SWD_TransferSpeed = kTransfer_GPIO_fast;
    }
    DAP_Data.fast_clock  = 1U;
    DAP_Data.clock_delay = 1U;

  } else if (clock >= 2000000) {
    // clock >= 2MHz -> Use GPIO with no program delay
    DAP_SPI_Deinit();
    DAP_Data.fast_clock  = 1U;
    DAP_Data.clock_delay = 1U;
    SWD_TransferSpeed = kTransfer_GPIO_fast;
  } else {
    // clock < 2MHz -> Use GPIO with delay
    DAP_SPI_Deinit();
    DAP_Data.fast_clock  = 0U;
    SWD_TransferSpeed = kTransfer_GPIO_normal;


/* WP-105: the low-speed software delay loop is calibrated against the APB
 * clock. ESP32/S3 APB is 80 MHz (the tree had 100 MHz here, which made the
 * generated SWCLK ~25% slower than the host requested). C3 APB is 80 MHz. */
#define BUS_CLOCK_FIXED 80000000

    delay = ((BUS_CLOCK_FIXED/2U) + (clock - 1U)) / clock;
    if (delay > IO_PORT_WRITE_CYCLES) {
      delay -= IO_PORT_WRITE_CYCLES;
      delay  = (delay + (DELAY_SLOW_CYCLES - 1U)) / DELAY_SLOW_CYCLES;
    } else {
      delay  = 1U;
    }

    DAP_Data.clock_delay = delay;
  }

  *response = DAP_OK;
#else
  *response = DAP_ERROR;
#endif

  return ((4U << 16) | 1U);
}


// Process SWJ Sequence command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_SWJ_Sequence(const uint8_t *request, uint8_t *response) {
  uint32_t count;

  count = *request++;
  if (count == 0U) {
    count = 256U;
  }

#if ((DAP_SWD != 0) || (DAP_JTAG != 0))
  SWJ_Sequence(count, request);
  *response = DAP_OK;
#else
  *response = DAP_ERROR;
#endif

  count = (count + 7U) >> 3;

  return (((count + 1U) << 16) | 1U);
}


// Process SWD Configure command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_SWD_Configure(const uint8_t *request, uint8_t *response) {
#if (DAP_SWD != 0)
  uint8_t value;

  value = *request;
  DAP_Data.swd_conf.turnaround = (value & 0x03U) + 1U;
  DAP_Data.swd_conf.data_phase = (value & 0x04U) ? 1U : 0U;

  *response = DAP_OK;
#else
  *response = DAP_ERROR;
#endif

  return ((1U << 16) | 1U);
}


// Process SWD Sequence command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_SWD_Sequence(const uint8_t *request, uint8_t *response) {
  uint32_t sequence_info;
  uint32_t sequence_count;
  uint32_t request_count;
  uint32_t response_count;
  uint32_t count;

#if (DAP_SWD != 0)
  *response++ = DAP_OK;
#else
  *response++ = DAP_ERROR;
#endif
  request_count  = 1U;
  response_count = 1U;

  /* WP-105: reject malformed/oversized sequences before touching the target */
  if (DAP_SequenceRequestIsValid(request, SWD_SEQUENCE_CLK, SWD_SEQUENCE_DIN, 0U) == 0U) {
    *response = DAP_ERROR;
    return ((2U << 16) | 1U);
  }

  sequence_count = *request++;
  while (sequence_count--) {
    sequence_info = *request++;
    count = sequence_info & SWD_SEQUENCE_CLK;
    if (count == 0U) {
      count = 64U;
    }
    count = (count + 7U) / 8U;
#if (DAP_SWD != 0)
    ////FIXME: ?
    // if ((sequence_info & SWD_SEQUENCE_DIN) != 0U) {
    //   PIN_SWDIO_OUT_DISABLE();
    // } else {
    //   PIN_SWDIO_OUT_ENABLE();
    // }
    SWD_Sequence(sequence_info, request, response);
    // if (sequence_count == 0U) {
    //   PIN_SWDIO_OUT_ENABLE();
    // }
#endif
    if ((sequence_info & SWD_SEQUENCE_DIN) != 0U) {
      request_count++;
#if (DAP_SWD != 0)
      response += count;
      response_count += count;
#endif
    } else {
      request += count;
      request_count += count + 1U;
    }
  }

  return ((request_count << 16) | response_count);
}


// Process JTAG Sequence command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_JTAG_Sequence(const uint8_t *request, uint8_t *response) {
  uint32_t sequence_info;
  uint32_t sequence_count;
  uint32_t request_count;
  uint32_t response_count;
  uint32_t count;

#if (DAP_JTAG != 0)
  *response++ = DAP_OK;
#else
  *response++ = DAP_ERROR;
#endif
  request_count  = 1U;
  response_count = 1U;

  /* WP-105: reject malformed/oversized sequences before touching the target */
  if (DAP_SequenceRequestIsValid(request, JTAG_SEQUENCE_TCK, JTAG_SEQUENCE_TDO, 1U) == 0U) {
    *response = DAP_ERROR;
    return ((2U << 16) | 1U);
  }

  sequence_count = *request++;
  while (sequence_count--) {
    sequence_info = *request++;
    count = sequence_info & JTAG_SEQUENCE_TCK;
    if (count == 0U) {
      count = 64U;
    }
    count = (count + 7U) / 8U;
#if (DAP_JTAG != 0)
    JTAG_Sequence(sequence_info, request, response);
#endif
    request += count;
    request_count += count + 1U;
#if (DAP_JTAG != 0)
    if ((sequence_info & JTAG_SEQUENCE_TDO) != 0U) {
      response += count;
      response_count += count;
    }
#endif
  }

  return ((request_count << 16) | response_count);
}


// Process JTAG Configure command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_JTAG_Configure(const uint8_t *request, uint8_t *response) {
  uint32_t count;
#if (DAP_JTAG != 0)
  uint32_t length;
  uint32_t bits;
  uint32_t n;

  count = *request++;
  /* WP-105: a malformed host packet could claim up to 255 devices and write
   * past DAP_Data.jtag_dev.ir_length[DAP_JTAG_DEV_CNT]. Reject instead. */
  if (count > DAP_JTAG_DEV_CNT) {
    DAP_Data.jtag_dev.count = 0U;
    *response = DAP_ERROR;
    return (((count + 1U) << 16) | 1U);
  }
  DAP_Data.jtag_dev.count = (uint8_t)count;

  bits = 0U;
  for (n = 0U; n < count; n++) {
    length = *request++;
    DAP_Data.jtag_dev.ir_length[n] =  (uint8_t)length;
    DAP_Data.jtag_dev.ir_before[n] = (uint16_t)bits;
    bits += length;
  }
  for (n = 0U; n < count; n++) {
    bits -= DAP_Data.jtag_dev.ir_length[n];
    DAP_Data.jtag_dev.ir_after[n] = (uint16_t)bits;
  }

  *response = DAP_OK;
#else
  count = *request;
  *response = DAP_ERROR;
#endif

  return (((count + 1U) << 16) | 1U);
}


// Process JTAG IDCODE command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_JTAG_IDCode(const uint8_t *request, uint8_t *response) {
#if (DAP_JTAG != 0)
  uint32_t data;

  if (DAP_Data.debug_port != DAP_PORT_JTAG) {
    goto id_error;
  }

  // Device index (JTAP TAP)
  DAP_Data.jtag_dev.index = *request;
  if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {
    goto id_error;
  }

  // Select JTAG chain
  JTAG_IR(JTAG_IDCODE);

  // Read IDCODE register
  data = JTAG_ReadIDCode();

  // Store Data
  *(response+0) =  DAP_OK;
  *(response+1) = (uint8_t)(data >>  0);
  *(response+2) = (uint8_t)(data >>  8);
  *(response+3) = (uint8_t)(data >> 16);
  *(response+4) = (uint8_t)(data >> 24);

  return ((1U << 16) | 5U);

id_error:
#endif
  *response = DAP_ERROR;
  return ((1U << 16) | 1U);
}


// Process Transfer Configure command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_TransferConfigure(const uint8_t *request, uint8_t *response) {

  DAP_Data.transfer.idle_cycles =            *(request+0);
  DAP_Data.transfer.retry_count = (uint16_t) *(request+1) |
                                  (uint16_t)(*(request+2) << 8);
  DAP_Data.transfer.match_retry = (uint16_t) *(request+3) |
                                  (uint16_t)(*(request+4) << 8);

  *response = DAP_OK;
  return ((5U << 16) | 1U);
}


// Process SWD Transfer command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
#if (DAP_SWD != 0)
static uint32_t DAP_SWD_Transfer(const uint8_t *request, uint8_t *response) {
  const
  uint8_t  *request_head;
  uint32_t  request_count;
  uint32_t  request_value;
  uint8_t  *response_head;
  uint32_t  response_count;
  uint32_t  response_value;
  uint32_t  post_read;
  uint32_t  check_write;
  uint32_t  match_value;
  uint32_t  match_retry;
  uint32_t  retry;
  uint32_t  data;
#if (TIMESTAMP_CLOCK != 0U)
  uint32_t  timestamp;
#endif

  request_head   = request;

  response_count = 0U;
  response_value = 0U;
  response_head  = response;
  response      += 2;

  DAP_TransferAbort = 0U;

  post_read   = 0U;
  check_write = 0U;

  request++;            // Ignore DAP index

  request_count = *request++;

  for (; request_count != 0U; request_count--) {
    request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
      // Read register
      if (post_read) {
        // Read was posted before
        retry = DAP_Data.transfer.retry_count;
        if ((request_value & (DAP_TRANSFER_APnDP | DAP_TRANSFER_MATCH_VALUE)) == DAP_TRANSFER_APnDP) {
          // Read previous AP data and post next AP read
          do {
            response_value = SWD_Transfer(request_value, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        } else {
          // Read previous AP data
          do {
            response_value = SWD_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          post_read = 0U;
        }
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
        // Store previous AP data
        *response++ = (uint8_t) data;
        *response++ = (uint8_t)(data >>  8);
        *response++ = (uint8_t)(data >> 16);
        *response++ = (uint8_t)(data >> 24);
#if (TIMESTAMP_CLOCK != 0U)
        if (post_read) {
          // Store Timestamp of next AP read
          if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
            timestamp = DAP_Data.timestamp;
            *response++ = (uint8_t) timestamp;
            *response++ = (uint8_t)(timestamp >>  8);
            *response++ = (uint8_t)(timestamp >> 16);
            *response++ = (uint8_t)(timestamp >> 24);
          }
        }
#endif
      }
      if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        // Read with value match
        match_value = (uint32_t)(*(request+0) <<  0) |
                      (uint32_t)(*(request+1) <<  8) |
                      (uint32_t)(*(request+2) << 16) |
                      ((uint32_t)*(request+3) << 24);
        request += 4;
        match_retry = DAP_Data.transfer.match_retry;
        if ((request_value & DAP_TRANSFER_APnDP) != 0U) {
          // Post AP read
          retry = DAP_Data.transfer.retry_count;
          do {
            response_value = SWD_Transfer(request_value, NULL);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          if (response_value != DAP_TRANSFER_OK) {
            break;
          }
        }
        do {
          // Read register until its value matches or retry counter expires
          retry = DAP_Data.transfer.retry_count;
          do {
            response_value = SWD_Transfer(request_value, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          if (response_value != DAP_TRANSFER_OK) {
            break;
          }
        } while (((data & DAP_Data.transfer.match_mask) != match_value) && match_retry-- && !DAP_TransferAbort);
        if ((data & DAP_Data.transfer.match_mask) != match_value) {
          response_value |= DAP_TRANSFER_MISMATCH;
        }
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
      } else {
        // Normal read
        retry = DAP_Data.transfer.retry_count;
        if ((request_value & DAP_TRANSFER_APnDP) != 0U) {
          // Read AP register
          if (post_read == 0U) {
            // Post AP read
            do {
              response_value = SWD_Transfer(request_value, NULL);
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
            if (response_value != DAP_TRANSFER_OK) {
              break;
            }
#if (TIMESTAMP_CLOCK != 0U)
            // Store Timestamp
            if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
              timestamp = DAP_Data.timestamp;
              *response++ = (uint8_t) timestamp;
              *response++ = (uint8_t)(timestamp >>  8);
              *response++ = (uint8_t)(timestamp >> 16);
              *response++ = (uint8_t)(timestamp >> 24);
            }
#endif
            post_read = 1U;
          }
        } else {
          // Read DP register
          do {
            response_value = SWD_Transfer(request_value, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          if (response_value != DAP_TRANSFER_OK) {
            break;
          }
#if (TIMESTAMP_CLOCK != 0U)
          // Store Timestamp
          if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
            timestamp = DAP_Data.timestamp;
            *response++ = (uint8_t) timestamp;
            *response++ = (uint8_t)(timestamp >>  8);
            *response++ = (uint8_t)(timestamp >> 16);
            *response++ = (uint8_t)(timestamp >> 24);
          }
#endif
          // Store data
          *response++ = (uint8_t) data;
          *response++ = (uint8_t)(data >>  8);
          *response++ = (uint8_t)(data >> 16);
          *response++ = (uint8_t)(data >> 24);
        }
      }
      check_write = 0U;
    } else {
      // Write register
      if (post_read) {
        // Read previous data
        retry = DAP_Data.transfer.retry_count;
        do {
          response_value = SWD_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
        } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
        // Store previous data
        *response++ = (uint8_t) data;
        *response++ = (uint8_t)(data >>  8);
        *response++ = (uint8_t)(data >> 16);
        *response++ = (uint8_t)(data >> 24);
        post_read = 0U;
      }
      // Load data
      data = (uint32_t)(*(request+0) <<  0) |
             (uint32_t)(*(request+1) <<  8) |
             (uint32_t)(*(request+2) << 16) |
             ((uint32_t)*(request+3) << 24);
      request += 4;
      if ((request_value & DAP_TRANSFER_MATCH_MASK) != 0U) {
        // Write match mask
        DAP_Data.transfer.match_mask = data;
        response_value = DAP_TRANSFER_OK;
      } else {
        // Write DP/AP register
        retry = DAP_Data.transfer.retry_count;
        do {
          response_value = SWD_Transfer(request_value, &data);
        } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
#if (TIMESTAMP_CLOCK != 0U)
        // Store Timestamp
        if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
          timestamp = DAP_Data.timestamp;
          *response++ = (uint8_t) timestamp;
          *response++ = (uint8_t)(timestamp >>  8);
          *response++ = (uint8_t)(timestamp >> 16);
          *response++ = (uint8_t)(timestamp >> 24);
        }
#endif
        check_write = 1U;
      }
    }
    response_count++;
    if (DAP_TransferAbort) {
      break;
    }
  }

  for (; request_count != 0U; request_count--) {
    // Process canceled requests
    request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
      // Read register
      if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        // Read with value match
        request += 4;
      }
    } else {
      // Write register
      request += 4;
    }
  }

  if (response_value == DAP_TRANSFER_OK) {
    if (post_read) {
      // Read previous data
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = SWD_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      // Store previous data
      *response++ = (uint8_t) data;
      *response++ = (uint8_t)(data >>  8);
      *response++ = (uint8_t)(data >> 16);
      *response++ = (uint8_t)(data >> 24);
    } else if (check_write) {
      // Check last write
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = SWD_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
    }
  }

end:
  *(response_head+0) = (uint8_t)response_count;
  *(response_head+1) = (uint8_t)response_value;

  return (((uint32_t)(request - request_head) << 16) | (uint32_t)(response - response_head));
}
#endif


// Process JTAG Transfer command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
#if (DAP_JTAG != 0)
static uint32_t DAP_JTAG_Transfer(const uint8_t *request, uint8_t *response) {
  const
  uint8_t  *request_head;
  uint32_t  request_count;
  uint32_t  request_value;
  uint32_t  request_ir;
  uint8_t  *response_head;
  uint32_t  response_count;
  uint32_t  response_value;
  uint32_t  post_read;
  uint32_t  match_value;
  uint32_t  match_retry;
  uint32_t  retry;
  uint32_t  data;
  uint32_t  ir;
#if (TIMESTAMP_CLOCK != 0U)
  uint32_t  timestamp;
#endif

  request_head   = request;

  response_count = 0U;
  response_value = 0U;
  response_head  = response;
  response      += 2;

  DAP_TransferAbort = 0U;

  ir        = 0U;
  post_read = 0U;

  // Device index (JTAP TAP)
  DAP_Data.jtag_dev.index = *request++;
  if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {
    goto end;
  }

  request_count = *request++;

  for (; request_count != 0U; request_count--) {
    request_value = *request++;
    request_ir = (request_value & DAP_TRANSFER_APnDP) ? JTAG_APACC : JTAG_DPACC;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
      // Read register
      if (post_read) {
        // Read was posted before
        retry = DAP_Data.transfer.retry_count;
        if ((ir == request_ir) && ((request_value & DAP_TRANSFER_MATCH_VALUE) == 0U)) {
          // Read previous data and post next read
          do {
            response_value = JTAG_Transfer(request_value, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        } else {
          // Select JTAG chain
          if (ir != JTAG_DPACC) {
            ir = JTAG_DPACC;
            JTAG_IR(ir);
          }
          // Read previous data
          do {
            response_value = JTAG_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          post_read = 0U;
        }
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
        // Store previous data
        *response++ = (uint8_t) data;
        *response++ = (uint8_t)(data >>  8);
        *response++ = (uint8_t)(data >> 16);
        *response++ = (uint8_t)(data >> 24);
#if (TIMESTAMP_CLOCK != 0U)
        if (post_read) {
          // Store Timestamp of next AP read
          if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
            timestamp = DAP_Data.timestamp;
            *response++ = (uint8_t) timestamp;
            *response++ = (uint8_t)(timestamp >>  8);
            *response++ = (uint8_t)(timestamp >> 16);
            *response++ = (uint8_t)(timestamp >> 24);
          }
        }
#endif
      }
      if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        // Read with value match
        match_value = (uint32_t)(*(request+0) <<  0) |
                      (uint32_t)(*(request+1) <<  8) |
                      (uint32_t)(*(request+2) << 16) |
                      ((uint32_t)*(request+3) << 24);
        request += 4;
        match_retry  = DAP_Data.transfer.match_retry;
        // Select JTAG chain
        if (ir != request_ir) {
          ir = request_ir;
          JTAG_IR(ir);
        }
        // Post DP/AP read
        retry = DAP_Data.transfer.retry_count;
        do {
          response_value = JTAG_Transfer(request_value, NULL);
        } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
        do {
          // Read register until its value matches or retry counter expires
          retry = DAP_Data.transfer.retry_count;
          do {
            response_value = JTAG_Transfer(request_value, &data);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          if (response_value != DAP_TRANSFER_OK) {
            break;
          }
        } while (((data & DAP_Data.transfer.match_mask) != match_value) && match_retry-- && !DAP_TransferAbort);
        if ((data & DAP_Data.transfer.match_mask) != match_value) {
          response_value |= DAP_TRANSFER_MISMATCH;
        }
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
      } else {
        // Normal read
        if (post_read == 0U) {
          // Select JTAG chain
          if (ir != request_ir) {
            ir = request_ir;
            JTAG_IR(ir);
          }
          // Post DP/AP read
          retry = DAP_Data.transfer.retry_count;
          do {
            response_value = JTAG_Transfer(request_value, NULL);
          } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
          if (response_value != DAP_TRANSFER_OK) {
            break;
          }
#if (TIMESTAMP_CLOCK != 0U)
          // Store Timestamp
          if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
            timestamp = DAP_Data.timestamp;
            *response++ = (uint8_t) timestamp;
            *response++ = (uint8_t)(timestamp >>  8);
            *response++ = (uint8_t)(timestamp >> 16);
            *response++ = (uint8_t)(timestamp >> 24);
          }
#endif
          post_read = 1U;
        }
      }
    } else {
      // Write register
      if (post_read) {
        // Select JTAG chain
        if (ir != JTAG_DPACC) {
          ir = JTAG_DPACC;
          JTAG_IR(ir);
        }
        // Read previous data
        retry = DAP_Data.transfer.retry_count;
        do {
          response_value = JTAG_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
        } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
        // Store previous data
        *response++ = (uint8_t) data;
        *response++ = (uint8_t)(data >>  8);
        *response++ = (uint8_t)(data >> 16);
        *response++ = (uint8_t)(data >> 24);
        post_read = 0U;
      }
      // Load data
      data = (uint32_t)(*(request+0) <<  0) |
             (uint32_t)(*(request+1) <<  8) |
             (uint32_t)(*(request+2) << 16) |
             ((uint32_t)*(request+3) << 24);
      request += 4;
      if ((request_value & DAP_TRANSFER_MATCH_MASK) != 0U) {
        // Write match mask
        DAP_Data.transfer.match_mask = data;
        response_value = DAP_TRANSFER_OK;
      } else {
        // Select JTAG chain
        if (ir != request_ir) {
          ir = request_ir;
          JTAG_IR(ir);
        }
        // Write DP/AP register
        retry = DAP_Data.transfer.retry_count;
        do {
          response_value = JTAG_Transfer(request_value, &data);
        } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
        if (response_value != DAP_TRANSFER_OK) {
          break;
        }
#if (TIMESTAMP_CLOCK != 0U)
        // Store Timestamp
        if ((request_value & DAP_TRANSFER_TIMESTAMP) != 0U) {
          timestamp = DAP_Data.timestamp;
          *response++ = (uint8_t) timestamp;
          *response++ = (uint8_t)(timestamp >>  8);
          *response++ = (uint8_t)(timestamp >> 16);
          *response++ = (uint8_t)(timestamp >> 24);
        }
#endif
      }
    }
    response_count++;
    if (DAP_TransferAbort) {
      break;
    }
  }

  for (; request_count != 0U; request_count--) {
    // Process canceled requests
    request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
      // Read register
      if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        // Read with value match
        request += 4;
      }
    } else {
      // Write register
      request += 4;
    }
  }

  if (response_value == DAP_TRANSFER_OK) {
    // Select JTAG chain
    if (ir != JTAG_DPACC) {
      ir = JTAG_DPACC;
      JTAG_IR(ir);
    }
    if (post_read) {
      // Read previous data
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = JTAG_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      // Store previous data
      *response++ = (uint8_t) data;
      *response++ = (uint8_t)(data >>  8);
      *response++ = (uint8_t)(data >> 16);
      *response++ = (uint8_t)(data >> 24);
    } else {
      // Check last write
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = JTAG_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
    }
  }

end:
  *(response_head+0) = (uint8_t)response_count;
  *(response_head+1) = (uint8_t)response_value;

  return (((uint32_t)(request - request_head) << 16) | (uint32_t)(response - response_head));
}
#endif


// Process Dummy Transfer command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_Dummy_Transfer(const uint8_t *request, uint8_t *response) {
  const
  uint8_t  *request_head;
  uint32_t  request_count;
  uint32_t  request_value;

  request_head  =  request;

  request++;            // Ignore DAP index

  request_count = *request++;

  for (; request_count != 0U; request_count--) {
    // Process dummy requests
    request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
      // Read register
      if ((request_value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        // Read with value match
        request += 4;
      }
    } else {
      // Write register
      request += 4;
    }
  }

  *(response+0) = 0U;   // Response count
  *(response+1) = 0U;   // Response value

  return (((uint32_t)(request - request_head) << 16) | 2U);
}


// Process Transfer command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_Transfer(const uint8_t *request, uint8_t *response) {
  uint32_t num;

  /* WP-105: validate the host packet before any target access. A 255-item
   * read+timestamp transfer would otherwise write ~2 KB into the 512-byte
   * response buffer (and a write transfer could read past the request). */
  if (DAP_TransferRequestIsValid(request) == 0U) {
    *(response + 0) = 0U;        // response count
    *(response + 1) = DAP_ERROR; // response value
    return ((2U << 16) | 2U);    // consume index+count only
  }

  DAP_STATS_TRANSFER((uint32_t)request[2]);

  switch (DAP_Data.debug_port) {
#if (DAP_SWD != 0)
    case DAP_PORT_SWD:
      num = DAP_SWD_Transfer(request, response);
      break;
#endif
#if (DAP_JTAG != 0)
    case DAP_PORT_JTAG:
      num = DAP_JTAG_Transfer(request, response);
      break;
#endif
    default:
      num = DAP_Dummy_Transfer(request, response);
      break;
  }

  return (num);
}


// Process SWD Transfer Block command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response
#if (DAP_SWD != 0)
static uint32_t DAP_SWD_TransferBlock(const uint8_t *request, uint8_t *response) {
  uint32_t  request_count;
  uint32_t  request_value;
  uint32_t  response_count;
  uint32_t  response_value;
  uint8_t  *response_head;
  uint32_t  retry;
  uint32_t  data;

  response_count = 0U;
  response_value = 0U;
  response_head  = response;
  response      += 3;

  DAP_TransferAbort = 0U;

  request++;            // Ignore DAP index

  request_count = (uint32_t)(*(request+0) << 0) |
                  (uint32_t)(*(request+1) << 8);
  request += 2;
  if (request_count == 0U) {
    goto end;
  }

  request_value = *request++;
  if ((request_value & DAP_TRANSFER_RnW) != 0U) {
    // Read register block
    if ((request_value & DAP_TRANSFER_APnDP) != 0U) {
      // Post AP read
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = SWD_Transfer(request_value, NULL);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
    }
    while (request_count--) {
      // Read DP/AP register
      if ((request_count == 0U) && ((request_value & DAP_TRANSFER_APnDP) != 0U)) {
        // Last AP read
        request_value = DP_RDBUFF | DAP_TRANSFER_RnW;
      }
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = SWD_Transfer(request_value, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      // Store data
      *response++ = (uint8_t) data;
      *response++ = (uint8_t)(data >>  8);
      *response++ = (uint8_t)(data >> 16);
      *response++ = (uint8_t)(data >> 24);
      response_count++;
    }
  } else {
    // Write register block
    while (request_count--) {
      // Load data
      data = (uint32_t)(*(request+0) <<  0) |
             (uint32_t)(*(request+1) <<  8) |
             (uint32_t)(*(request+2) << 16) |
             ((uint32_t)*(request+3) << 24);
      request += 4;
      // Write DP/AP register
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = SWD_Transfer(request_value, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      response_count++;
    }
    // Check last write
    retry = DAP_Data.transfer.retry_count;
    do {
      response_value = SWD_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
    } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
  }

end:
  *(response_head+0) = (uint8_t)(response_count >> 0);
  *(response_head+1) = (uint8_t)(response_count >> 8);
  *(response_head+2) = (uint8_t) response_value;

  return ((uint32_t)(response - response_head));
}
#endif


// Process JTAG Transfer Block command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response
#if (DAP_JTAG != 0)
static uint32_t DAP_JTAG_TransferBlock(const uint8_t *request, uint8_t *response) {
  uint32_t  request_count;
  uint32_t  request_value;
  uint32_t  response_count;
  uint32_t  response_value;
  uint8_t  *response_head;
  uint32_t  retry;
  uint32_t  data;
  uint32_t  ir;

  response_count = 0U;
  response_value = 0U;
  response_head  = response;
  response      += 3;

  DAP_TransferAbort = 0U;

  // Device index (JTAP TAP)
  DAP_Data.jtag_dev.index = *request++;
  if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {
    goto end;
  }

  request_count = (uint32_t)(*(request+0) << 0) |
                  (uint32_t)(*(request+1) << 8);
  request += 2;
  if (request_count == 0U) {
    goto end;
  }

  request_value = *request++;

  // Select JTAG chain
  ir = (request_value & DAP_TRANSFER_APnDP) ? JTAG_APACC : JTAG_DPACC;
  JTAG_IR(ir);

  if ((request_value & DAP_TRANSFER_RnW) != 0U) {
    // Post read
    retry = DAP_Data.transfer.retry_count;
    do {
      response_value = JTAG_Transfer(request_value, NULL);
    } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
    if (response_value != DAP_TRANSFER_OK) {
      goto end;
    }
    // Read register block
    while (request_count--) {
      // Read DP/AP register
      if (request_count == 0U) {
        // Last read
        if (ir != JTAG_DPACC) {
          JTAG_IR(JTAG_DPACC);
        }
        request_value = DP_RDBUFF | DAP_TRANSFER_RnW;
      }
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = JTAG_Transfer(request_value, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      // Store data
      *response++ = (uint8_t) data;
      *response++ = (uint8_t)(data >>  8);
      *response++ = (uint8_t)(data >> 16);
      *response++ = (uint8_t)(data >> 24);
      response_count++;
    }
  } else {
    // Write register block
    while (request_count--) {
      // Load data
      data = (uint32_t)(*(request+0) <<  0) |
             (uint32_t)(*(request+1) <<  8) |
             (uint32_t)(*(request+2) << 16) |
             ((uint32_t)*(request+3) << 24);
      request += 4;
      // Write DP/AP register
      retry = DAP_Data.transfer.retry_count;
      do {
        response_value = JTAG_Transfer(request_value, &data);
      } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
      if (response_value != DAP_TRANSFER_OK) {
        goto end;
      }
      response_count++;
    }
    // Check last write
    if (ir != JTAG_DPACC) {
      JTAG_IR(JTAG_DPACC);
    }
    retry = DAP_Data.transfer.retry_count;
    do {
      response_value = JTAG_Transfer(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);
    } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);
  }

end:
  *(response_head+0) = (uint8_t)(response_count >> 0);
  *(response_head+1) = (uint8_t)(response_count >> 8);
  *(response_head+2) = (uint8_t) response_value;

  return ((uint32_t)(response - response_head));
}
#endif


// Process Transfer Block command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_TransferBlock(const uint8_t *request, uint8_t *response) {
  uint32_t num;
  uint32_t request_count;
  uint32_t max_count;

  request_count = (uint32_t)(*(request + 1)) | ((uint32_t)(*(request + 2)) << 8);

  /* WP-105 + security review N9: the cap must be direction dependent because
   * the request/response sizes differ:
   *   read  : response = echo(1) + count(2) + value(1) + 4*count
   *           -> count <= (DAP_PACKET_SIZE-4)/4 = 127
   *   write : request  = cmd(1) + index(1) + count(2) + value(1) + 4*count
   *           -> count <= (DAP_PACKET_SIZE-5)/4 = 126
   * The previous single 127 cap let the write direction read 1 byte past the
   * 512-byte request packet (count=127 => 513 bytes) - N9. */
  max_count = ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) ?
              ((DAP_PACKET_SIZE - 4U) / 4U) : ((DAP_PACKET_SIZE - 5U) / 4U);
  if (request_count > max_count) {
    *(response + 0) = 0U;        // response count [7:0]
    *(response + 1) = 0U;        // response count [15:8]
    *(response + 2) = DAP_ERROR; // response value
    return ((4U << 16) | 3U);
  }

  DAP_STATS_BLOCK(request_count, (*(request + 3) & DAP_TRANSFER_RnW) != 0U);

  switch (DAP_Data.debug_port) {
#if (DAP_SWD != 0)
    case DAP_PORT_SWD:
      num = DAP_SWD_TransferBlock (request, response);
      break;
#endif
#if (DAP_JTAG != 0)
    case DAP_PORT_JTAG:
      num = DAP_JTAG_TransferBlock(request, response);
      break;
#endif
    default:
      *(response+0) = 0U;       // Response count [7:0]
      *(response+1) = 0U;       // Response count[15:8]
      *(response+2) = 0U;       // Response value
      num = 3U;
      break;
  }

  if ((*(request+3) & DAP_TRANSFER_RnW) != 0U) {
    // Read register block
    num |=  4U << 16;
  } else {
    // Write register block
    num |= (4U + (((uint32_t)(*(request+1)) | (uint32_t)(*(request+2) << 8)) * 4)) << 16;
  }

  return (num);
}


// Process SWD Write ABORT command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response
#if (DAP_SWD != 0)
static uint32_t DAP_SWD_WriteAbort(const uint8_t *request, uint8_t *response) {
  uint32_t data;

  // Load data (Ignore DAP index)
  data = (uint32_t)(*(request+1) <<  0) |
         (uint32_t)(*(request+2) <<  8) |
         (uint32_t)(*(request+3) << 16) |
         ((uint32_t)*(request+4) << 24);

  // Write Abort register
  SWD_Transfer(DP_ABORT, &data);

  *response = DAP_OK;
  return (1U);
}
#endif


// Process JTAG Write ABORT command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response
#if (DAP_JTAG != 0)
static uint32_t DAP_JTAG_WriteAbort(const uint8_t *request, uint8_t *response) {
  uint32_t data;

  // Device index (JTAP TAP)
  DAP_Data.jtag_dev.index = *request;
  if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {
    *response = DAP_ERROR;
    return (1U);
  }

  // Select JTAG chain
  JTAG_IR(JTAG_ABORT);

  // Load data
  data = (uint32_t)(*(request+1) <<  0) |
         (uint32_t)(*(request+2) <<  8) |
         (uint32_t)(*(request+3) << 16) |
         ((uint32_t)*(request+4) << 24);

  // Write Abort register
  JTAG_WriteAbort(data);

  *response = DAP_OK;
  return (1U);
}
#endif


// Process Write ABORT command and prepare response
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_WriteAbort(const uint8_t *request, uint8_t *response) {
  uint32_t num;

  switch (DAP_Data.debug_port) {
#if (DAP_SWD != 0)
    case DAP_PORT_SWD:
      num = DAP_SWD_WriteAbort (request, response);
      break;
#endif
#if (DAP_JTAG != 0)
    case DAP_PORT_JTAG:
      num = DAP_JTAG_WriteAbort(request, response);
      break;
#endif
    default:
      *response = DAP_ERROR;
      num = 1U;
      break;
  }
  return ((5U << 16) | num);
}


/* WP-105: validate a SWD/JTAG Sequence request (request points at the
 * sequence_count byte) against a single DAP packet: the request data scan and
 * the worst-case response must both fit into DAP_PACKET_SIZE. The response of
 * a Sequence command is 1 status byte + div_round_up(count,8) bytes per
 * capturing sequence; the old code let a 255-sequence packet write ~2 KB into
 * a 512-byte response buffer.
 *   count_mask      - SWD_SEQUENCE_CLK / JTAG_SEQUENCE_TCK
 *   capture_flag    - SWD_SEQUENCE_DIN / JTAG_SEQUENCE_TDO
 *   request_always  - 1 for JTAG (TDI bytes always present in the request)
 *                     0 for SWD (request data only for non-capturing sequences)
 * return: 1 = valid, 0 = malformed / oversized. */
static uint8_t DAP_SequenceRequestIsValid(const uint8_t *request,
                                          uint32_t count_mask,
                                          uint32_t capture_flag,
                                          uint8_t request_always)
{
  uint32_t sequence_count = *request++;
  uint32_t request_len = 0U;   /* bytes after sequence_count */
  uint32_t response_len = 1U;  /* status byte */
  uint32_t i;

  for (i = 0U; i < sequence_count; i++) {
    uint32_t info, bytes;

    if (request_len + 1U > DAP_PACKET_SIZE - 2U) {
      return 0U;
    }
    info = request[request_len++];
    bytes = info & count_mask;
    bytes = (bytes != 0U) ? ((bytes + 7U) / 8U) : 8U;

    if (request_always != 0U) {
      if (request_len + bytes > DAP_PACKET_SIZE - 2U) {
        return 0U;
      }
      request_len += bytes;
      if ((info & capture_flag) != 0U) {
        response_len += bytes;
      }
    } else if ((info & capture_flag) != 0U) {
      response_len += bytes;
    } else {
      if (request_len + bytes > DAP_PACKET_SIZE - 2U) {
        return 0U;
      }
      request_len += bytes;
    }
  }

  return ((response_len <= (DAP_PACKET_SIZE - 1U)) &&
          (request_len <= (DAP_PACKET_SIZE - 2U))) ? 1U : 0U;
}

/* WP-105: validate a DAP_Transfer request (request points at the DAP index
 * byte). Walk every request item so that a malformed host packet can neither
 * read past the request packet nor produce a response larger than one packet.
 * A read item produces 4 data bytes (+4 timestamp); a write/match item carries
 * 4 bytes of data in the request. */
static uint8_t DAP_TransferRequestIsValid(const uint8_t *request)
{
  uint32_t count = *(request + 1);
  uint32_t request_len = 2U;   /* index + count */
  uint32_t response_len = 2U;  /* response count + response value */
  uint32_t i;

  for (i = 0U; i < count; i++) {
    uint8_t value;

    if (request_len + 1U > DAP_PACKET_SIZE - 1U) {
      return 0U;
    }
    value = request[request_len++];

    if ((value & DAP_TRANSFER_RnW) != 0U) {
      if ((value & DAP_TRANSFER_MATCH_VALUE) != 0U) {
        /* Read with value match: the match value is in the request and the
         * matched data is NOT returned in the response (only the status). */
        if (request_len + 4U > DAP_PACKET_SIZE - 1U) {
          return 0U;
        }
        request_len += 4U;
      } else {
        /* Read data (4 bytes) + optional timestamp (4 bytes) */
        response_len += 4U;
        if ((value & DAP_TRANSFER_TIMESTAMP) != 0U) {
          response_len += 4U;
        }
      }
    } else {
      /* Write data (4 bytes in request); a write with TIMESTAMP also returns
       * a 4-byte timestamp in the response. */
      if (request_len + 4U > DAP_PACKET_SIZE - 1U) {
        return 0U;
      }
      request_len += 4U;
      if (((value & DAP_TRANSFER_MATCH_MASK) == 0U) &&
          ((value & DAP_TRANSFER_TIMESTAMP) != 0U)) {
        response_len += 4U;
      }
    }
  }

  if (request_len > DAP_PACKET_SIZE - 1U) {
    return 0U;
  }
  return (response_len <= (DAP_PACKET_SIZE - 1U)) ? 1U : 0U;
}

/* WP-105: the __WEAK default DAP_ProcessVendorCommand() implementation was
 * removed (as in the A tree); DAP_vendor.c provides the real handler and the
 * weak fallback is dead code that hides a missing vendor implementation. */

/* WP-105: conservative worst-case response size (in bytes, including the
 * command echo byte) of a single DAP command. DAP_ExecuteCommand uses it to
 * stop before the accumulated 0x7F response overflows the DAP packet. */
static uint32_t DAP_ResponseMaxSize(const uint8_t *request)
{
  uint32_t cmd = request[0];
  uint32_t count;

  if ((cmd >= ID_DAP_Vendor0) && (cmd <= ID_DAP_Vendor31)) {
    /* Vendor handler decides its own size (elaphureLink may write a whole
     * packet); only allow a vendor command as a single command. */
    return DAP_PACKET_SIZE;
  }

  switch (cmd) {
    case ID_DAP_Info:
      return 2U + 64U; /* echo + length + max string (60 + NUL) */
    case ID_DAP_Transfer: {
      /* Exact response size: echo + count + status + 4 bytes per read item
       * (+4 per timestamped read).  The old worst case used 8 bytes per item
       * (2x the real read size), which made DAP_ExecuteCommands stop queued
       * batches early: with a 1 KiB packet OpenOCD can queue ~30-50% more
       * DAP_Transfer commands per transfer with the exact estimate. */
      uint32_t items = request[2];
      const uint8_t *item = request + 3;
      const uint8_t *limit = request + DAP_PACKET_SIZE;
      uint32_t size = 3U; /* echo + response count + response value */

      for (uint32_t i = 0; i < items; i++) {
        uint8_t rq;

        /* Malformed/oversized request: stop estimating and make the batch
         * guard reject it (the handler validates the command when executed
         * outside a batch). */
        if (item >= limit) {
          return DAP_PACKET_SIZE;
        }
        rq = *item++;
        if ((rq & DAP_TRANSFER_RnW) != 0U) {
          size += 4U;
          if ((rq & DAP_TRANSFER_TIMESTAMP) != 0U) {
            size += 4U;
          }
          if ((rq & DAP_TRANSFER_MATCH_VALUE) != 0U) {
            item += 4U;
          }
        } else {
          item += 4U; /* write data word */
          if ((rq & DAP_TRANSFER_MATCH_VALUE) != 0U) {
            item += 4U;
          }
        }
      }
      return size;
    }
    case ID_DAP_TransferBlock:
      /* Command start layout: [cmd][DAP index][count lo][count hi][request].
       * The previous estimator read request[1]/request[2]/request[3], i.e. it
       * treated the DAP index as the count low byte and the count low byte as
       * the direction: a queued read batch could then be admitted with an
       * actual response of up to DAP_PACKET_SIZE+2 bytes (1-2 byte response
       * buffer overflow, caught by the host ASan regression). */
      count = (uint32_t)request[2] | ((uint32_t)request[3] << 8);
      /* echo + count(2) + status(1) + 4*count (read) / echo+count+status (write) */
      return ((request[4] & DAP_TRANSFER_RnW) != 0U) ? (4U + count * 4U) : 4U;
    case ID_DAP_SWD_Sequence:
    case ID_DAP_JTAG_Sequence:
      /* echo + status + div_round_up(bits,8) per capturing sequence; upper
       * bound 8 bytes per sequence (handlers reject anything above a packet) */
      return 2U + ((uint32_t)request[1] * 8U);
    case ID_DAP_JTAG_IDCODE:
      return 6U;
    case ID_DAP_QueueCommands:
    case ID_DAP_ExecuteCommands:
      /* N1: a nested batch is rejected by the depth guard below and produces
       * just the echo + ID_DAP_Invalid status (2 bytes). Estimate accordingly
       * so a legitimate batch is not truncated by the accumulate guard. */
      return 2U;
    case ID_DAP_ResetTarget:
      return 3U;
    case ID_DAP_HostStatus:
    case ID_DAP_Connect:
    case ID_DAP_Disconnect:
    case ID_DAP_Delay:
    case ID_DAP_SWJ_Pins:
    case ID_DAP_SWJ_Clock:
    case ID_DAP_SWJ_Sequence:
    case ID_DAP_SWD_Configure:
    case ID_DAP_JTAG_Configure:
    case ID_DAP_TransferConfigure:
    case ID_DAP_TransferAbort:
    case ID_DAP_WriteABORT:
      return 2U;
    default:
      return 8U;
  }
}

// Process DAP command request and prepare response (depth-aware internal)
//   depth: 0 = top-level transport command, >=1 = sub-command of a batch
//   request:  pointer to request data
//   response: pointer to response data
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_ProcessCommandDepth(const uint8_t *request, uint8_t *response, uint8_t depth) {
  uint32_t num;

  DAP_STATS_COUNT_CMD(*request);

  if ((*request >= ID_DAP_Vendor0) && (*request <= ID_DAP_Vendor31)) {
    return DAP_ProcessVendorCommand(request, response);
  }

  *response++ = *request;

  switch (*request++) {
    case ID_DAP_Info:
      num = DAP_Info(*request, response+1);
      *response = (uint8_t)num;
      return ((2U << 16) + 2U + num);

    case ID_DAP_HostStatus:
      num = DAP_HostStatus(request, response);
      break;

    case ID_DAP_Connect:
      num = DAP_Connect(request, response);
      break;
    case ID_DAP_Disconnect:
      num = DAP_Disconnect(response);
      break;

    case ID_DAP_Delay:
      num = DAP_Delay(request, response);
      break;

    case ID_DAP_ResetTarget:
      num = DAP_ResetTarget(response);
      break;

    case ID_DAP_SWJ_Pins:
      num = DAP_SWJ_Pins(request, response);
      break;
    case ID_DAP_SWJ_Clock:
      num = DAP_SWJ_Clock(request, response);
      break;
    case ID_DAP_SWJ_Sequence:
      num = DAP_SWJ_Sequence(request, response);
      break;

    case ID_DAP_SWD_Configure:
      num = DAP_SWD_Configure(request, response);
      break;
    case ID_DAP_SWD_Sequence:
      num = DAP_SWD_Sequence(request, response);
      break;

    case ID_DAP_JTAG_Sequence:
      num = DAP_JTAG_Sequence(request, response);
      break;
    case ID_DAP_JTAG_Configure:
      num = DAP_JTAG_Configure(request, response);
      break;
    case ID_DAP_JTAG_IDCODE:
      num = DAP_JTAG_IDCode(request, response);
      break;

    case ID_DAP_TransferConfigure:
      num = DAP_TransferConfigure(request, response);
      break;
    case ID_DAP_Transfer:
      num = DAP_Transfer(request, response);
      break;
    case ID_DAP_TransferBlock:
      num = DAP_TransferBlock(request, response);
      break;

    case ID_DAP_WriteABORT:
      num = DAP_WriteAbort(request, response);
      break;

    case ID_DAP_TransferAbort:
      /* WP-105: real 0x07 implementation. The flag was already consumed by
       * every Transfer retry loop but nothing ever set it; a host calling
       * DAP_TransferAbort could not actually stop a pending transfer. */
      DAP_TransferAbort = 1U;
      *response = DAP_OK;
      num = 1U;
      break;

    case ID_DAP_QueueCommands:
    case ID_DAP_ExecuteCommands:
      /* WP-105: the USBIP transport rewrites 0x7E to 0x7F and calls
       * DAP_ProcessCommand directly, so 0x7E/0x7F must be handled here for the
       * advertised "Atomic Commands" capability to be real. DAP_ExecuteCommand()
       * writes the command/count header itself: pass the un-advanced pointers
       * (request/response were already incremented by the echo).
       *
       * N1 (security review): a Queue/Execute command nested inside a batch
       * must be rejected, not dispatched again. Otherwise
       * DAP_ProcessCommand(0x7E) -> DAP_ExecuteCommand(0x7E) ->
       * DAP_ProcessCommand(0x7E) ... recurses until the task stack dies
       * (reachable unauthenticated via the DAP proxy port). The reference
       * implementation also answers ID_DAP_Invalid for a nested batch. */
      if (depth != 0U) {
        /* Reject, but consume the command byte + count byte so the enclosing
         * batch continues at the next sub-command. */
        *response = ID_DAP_Invalid;
        return ((2U << 16) | 2U);
      }
      return DAP_ExecuteCommandDepth(request - 1U, response - 1U, depth);

#if ((SWO_UART != 0) || (SWO_MANCHESTER != 0))
    case ID_DAP_SWO_Transport:
      num = SWO_Transport(request, response);
      break;
    case ID_DAP_SWO_Mode:
      num = SWO_Mode(request, response);
      break;
    case ID_DAP_SWO_Baudrate:
      num = SWO_Baudrate(request, response);
      break;
    case ID_DAP_SWO_Control:
      num = SWO_Control(request, response);
      break;
    case ID_DAP_SWO_Status:
      num = SWO_Status(response);
      break;
    case ID_DAP_SWO_ExtendedStatus:
      num = SWO_ExtendedStatus(request, response);
      break;
    case ID_DAP_SWO_Data:
      num = SWO_Data(request, response);
      break;
#endif

#if (DAP_UART != 0)
    case ID_DAP_UART_Transport:
      num = UART_Transport(request, response);
      break;
    case ID_DAP_UART_Configure:
      num = UART_Configure(request, response);
      break;
    case ID_DAP_UART_Control:
      num = UART_Control(request, response);
      break;
    case ID_DAP_UART_Status:
      num = UART_Status(response);
      break;
    case ID_DAP_UART_Transfer:
      num = UART_Transfer(request, response);
      break;
#endif

    default:
      *(response-1) = ID_DAP_Invalid;
      return ((1U << 16) | 1U);
  }

  return ((1U << 16) + 1U + num);
}

// Process DAP command request and prepare response (public transport entry)
uint32_t DAP_ProcessCommand(const uint8_t *request, uint8_t *response) {
  uint32_t ret;

  DAP_STATS_TOP_BEGIN();
  ret = DAP_ProcessCommandDepth(request, response, 0U);
  DAP_STATS_TOP_END(request[0]);
  return ret;
}


// Execute DAP command (process request and prepare response, depth-aware)
//   request:  pointer to request data
//   response: pointer to response data
//   depth:    batch nesting depth (0 = top-level)
//   return:   number of bytes in response (lower 16 bits)
//             number of bytes in request (upper 16 bits)
static uint32_t DAP_ExecuteCommandDepth(const uint8_t *request, uint8_t *response, uint8_t depth) {
  uint32_t cnt, num, n, processed, used, cnt_requested;
  uint8_t *response_count;
  uint8_t command;
  uint8_t hit_cap = 0U;

  command = *request;
  if ((command == ID_DAP_QueueCommands) || (command == ID_DAP_ExecuteCommands)) {
    /* N1: 0x7E (QueueCommands) is an alias of 0x7F here - this firmware has no
     * queue storage and the USBIP transport already rewrites 0x7E to 0x7F.
     * Echo the byte the host actually sent so both hosts keep working. */
    *response++ = *request++;
    response_count = response;  /* count byte written before parsing */
    cnt = *request++;
    cnt_requested = cnt;
    *response++ = (uint8_t)cnt;
    num = (2U << 16) | 2U;
    processed = 0U;
    used = 2U;                  /* command + count bytes already written */

    while (cnt--) {
      uint32_t max_size = DAP_ResponseMaxSize(request);

      /* WP-105: a batch of individually valid commands can still overflow the
       * single-packet response. Stop before writing anything for the command
       * that would exceed DAP_PACKET_SIZE and report the processed count. */
      if ((max_size > DAP_PACKET_SIZE) || ((used + max_size) > DAP_PACKET_SIZE)) {
        hit_cap = 1U;
        break;
      }

      /* N1: sub-commands run with depth >= 1, so a nested 0x7E/0x7F returns
       * ID_DAP_Invalid instead of recursing into another batch. */
      n = DAP_ProcessCommandDepth(request, response, (uint8_t)(depth + 1U));
      num += n;
      used += (uint16_t) n;
      processed++;
      request  += (uint16_t)(n >> 16);
      response += (uint16_t) n;
    }

    *response_count = (uint8_t)processed;
    DAP_STATS_BATCH(cnt_requested, processed, hit_cap);
    return (num);
  }

  return DAP_ProcessCommandDepth(request, response, depth);
}

// Execute DAP command (public transport entry)
uint32_t DAP_ExecuteCommand(const uint8_t *request, uint8_t *response) {
  uint32_t ret;

  DAP_STATS_TOP_BEGIN();
  ret = DAP_ExecuteCommandDepth(request, response, 0U);
  DAP_STATS_TOP_END(request[0]);
  return ret;
}


// Setup DAP
void DAP_Setup(void) {

  // Default settings
  DAP_Data.debug_port  = 0U;
  DAP_Data.fast_clock  = 0U;
  DAP_Data.clock_delay = CLOCK_DELAY(DAP_DEFAULT_SWJ_CLOCK);
  DAP_Data.transfer.idle_cycles = 0U;
  DAP_Data.transfer.retry_count = 100U;
  DAP_Data.transfer.match_retry = 0U;
  DAP_Data.transfer.match_mask  = 0x00000000U;
#if (DAP_SWD != 0)
  DAP_Data.swd_conf.turnaround  = 1U;
  DAP_Data.swd_conf.data_phase  = 0U;
#endif
#if (DAP_JTAG != 0)
  DAP_Data.jtag_dev.count = 0U;
#endif

  DAP_SETUP();  // Device specific setup
}

void dap_os_delay(int ms)
{
  if (CONFIG_FREERTOS_HZ >= 1000) {
    vTaskDelay(pdMS_TO_TICKS(ms));
  } else {
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
  }
}