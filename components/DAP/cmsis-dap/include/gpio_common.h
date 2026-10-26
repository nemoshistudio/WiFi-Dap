#ifndef __GPIO_COMMON_H__
#define __GPIO_COMMON_H__

#include "sdkconfig.h"
#include "esp_idf_version.h"

/*
 * Target low-level register/LL headers used by the DAP GPIO/SPI bit-banging
 * layer.
 *
 * WP-102 merge decision (A 2025 fork + C IDF5 baseline):
 *   - ESP8266 is intentionally dropped (project supports
 *     esp32/esp32c3/esp32s3/esp32c5, see docs/board-pins.md), so the old
 *     ESP8266 blocks are gone.
 *   - Include paths follow the IDF5 layout (C tree): plain component include
 *     names ("soc/xxx.h"), not the IDF4 "soc/esp32c3/include/soc/xxx.h" form
 *     that the A fork used.
 *   - "soc/gpio_sig_map.h" is added explicitly because the S3/C3 dedicated-GPIO
 *     fast path needs CORE1_GPIO_* / CPU_GPIO_* signal indices (A relied on
 *     transitive includes that differ between IDF versions).
 */
#if defined CONFIG_IDF_TARGET_ESP32
    #include "soc/io_mux_reg.h"
    #include "soc/gpio_struct.h"
    #include "soc/gpio_periph.h"
    #include "soc/gpio_sig_map.h"
    #include "soc/dport_access.h"
    #include "soc/dport_reg.h"
    #include "soc/periph_defs.h"
    #include "soc/spi_struct.h"
    #include "soc/spi_reg.h"
    #include "hal/gpio_ll.h"
    #include "hal/gpio_types.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    #include "soc/gpio_struct.h"
    #include "soc/gpio_periph.h"
    #include "soc/gpio_sig_map.h"
    #include "hal/gpio_ll.h"
    #include "hal/gpio_hal.h"
    #include "hal/clk_gate_ll.h"
    #include "soc/dport_access.h"
    #include "soc/periph_defs.h"
    #include "soc/usb_serial_jtag_reg.h"
    #include "soc/io_mux_reg.h"
    #include "soc/spi_struct.h"
    #include "soc/spi_reg.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
    /* ESP32-C5 (IDF >= v5.5): no hal/gpio_hal.h and no clk_gate_ll periph
     * helpers; the register headers live in soc/.../register and are exposed
     * through the soc component include dirs. The DAP C5 code uses the C5
     * gpio_ll HAL + dedicated-GPIO CSR user registers (same RISC-V mechanism
     * as C3) and the SPI2 register block (see docs/esp32-c5-port.md). */
    #include "soc/gpio_struct.h"
    #include "soc/gpio_sig_map.h"
    #include "hal/gpio_ll.h"
    #include "hal/spi_ll.h"
    #include "soc/dport_access.h"
    #include "soc/periph_defs.h"
    #include "soc/io_mux_reg.h"
    #include "soc/spi_struct.h"
    #include "soc/spi_reg.h"
    /* The shared S3-style branches in DAP_config.h call
     * GPIO_HAL_GET_HW(GPIO_PORT_0); C5 has a single GPIO port and no
     * hal/gpio_hal.h, so provide the same accessor here. */
    #ifndef GPIO_HAL_GET_HW
    #define GPIO_HAL_GET_HW(port) (&GPIO)
    #endif
#else
    #error "unsupported target: DAP supports esp32 / esp32c3 / esp32s3 / esp32c5 only"
#endif


#endif
