#ifndef __DAP_CONFIGURATION_H__
#define __DAP_CONFIGURATION_H__

/**
 * @brief Specify the use of WINUSB
 *
 */
#define USE_WINUSB 1

/**
 * @brief Enable this option, no need to physically connect MOSI and MISO
 *
 */
#define USE_SPI_SIO 1


/**
 * @brief Specify to enable USB 3.0
 *
 */
#define USE_USB_3_0 0


// For USB 3.0, it must be 1024 byte.
#if (USE_USB_3_0 == 1)
    #define USB_ENDPOINT_SIZE 1024U
#else
    #define USB_ENDPOINT_SIZE 512U
#endif

/// Maximum Package Size for Command and Response data.
/// This configuration settings is used to optimize the communication performance with the
/// debugger and depends on the USB peripheral. Typical vales are 64 for Full-speed USB HID or WinUSB,
/// 1024 for High-speed USB HID and 512 for High-speed USB WinUSB.

#if (USE_WINUSB == 1)
    /*
     * B1: USBIP/WinUSB bulk transport packet size.
     *
     * The USBIP transport is latency-bound over Wi-Fi: every CMSIS-DAP command
     * costs an OUT+IN URB round-trip.  OpenOCD (cmsis-dap.c) derives its
     * DAP_Transfer block size from this value:
     *     pending_queue_len = (DAP_PACKET_SIZE - 4) / 5
     * and puts up to 255 transfers into one DAP_Transfer (the CMSIS-DAP
     * DAP_Transfer count field is one byte!).  A value above 1279 would make
     * OpenOCD build count=256+ requests that the DAP core must reject/truncate,
     * so the useful maximum is 1280: (1280-4)/5 = 255 transfers per block,
     * i.e. 1020 bytes of SWD read data per URB pair instead of 816 bytes at
     * 1024 (24% fewer round-trips per byte).
     *
     * ESP32-S3 (release target): 1280; the smaller compile-only targets keep
     * 1024 to limit static RAM usage.
     */
    #include "sdkconfig.h"
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        #define DAP_PACKET_SIZE 1280U /* (1280-4)/5 = 255: max u8 DAP_Transfer count */
    #else
        #define DAP_PACKET_SIZE 1024U
    #endif
#else
    #define DAP_PACKET_SIZE 255U // 255 for USB HID
#endif


#endif
