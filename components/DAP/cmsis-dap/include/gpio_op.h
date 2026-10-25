/**
 * @file gpio_op.h
 * @author windowsair
 * @brief esp GPIO operation
 * @version 0.2
 * @date 2021-03-03
 *
 * @copyright Copyright (c) 2021
 *
 * WP-102 merge (A 2025 fork + C IDF5):
 *  - A's dedicated-GPIO hooks (S3 Xtensa `ee.*` instructions, C3 RISC-V CSR
 *    user GPIO) are kept: DAP_config.h uses SWCLK_SET/SWDIO_SET/... when
 *    defined, which is how the 2025 fork reaches its fast SWCLK rates.
 *  - IDF5 API forms from the C tree are kept (gpio_ll HAL calls instead of
 *    the IDF4 GPIO register struct where the C tree already converted them).
 *  - ESP8266 branches are intentionally removed.
 */

#ifndef __GPIO_OP_H__
#define __GPIO_OP_H__

#include "sdkconfig.h"
#include "cmsis-dap/include/cmsis_compiler.h"
#include "cmsis-dap/include/gpio_common.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || \
    defined(CONFIG_IDF_TARGET_ESP32C5)
// Dedicated-GPIO CPU instruction helpers:
//   S3: dedic_gpio_cpu_ll_read_in() (Xtensa `ee.get_gpio_in`)
//   C3/C5: CSR_GPIO_*_USER + RV_*_CSR macros (riscv/csr.h)
#include "hal/dedic_gpio_cpu_ll.h"
#endif


#if defined CONFIG_IDF_TARGET_ESP32
__STATIC_INLINE __UNUSED void GPIO_FUNCTION_SET(int io_num)
{
    // function number 2 is GPIO_FUNC for each pin
    // Note that the index starts at 0, so we are using function 3.
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[io_num], PIN_FUNC_GPIO);
}
#elif defined CONFIG_IDF_TARGET_ESP32C3
__STATIC_INLINE __UNUSED void GPIO_FUNCTION_SET(int io_num)
{
  // Disable USB Serial JTAG if pins 18 or pins 19 needs to select an IOMUX function
  if (io_num == IO_MUX_GPIO18_REG || io_num == IO_MUX_GPIO19_REG) {
      CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  }
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[io_num], PIN_FUNC_GPIO);
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
__STATIC_INLINE __UNUSED void GPIO_FUNCTION_SET(int io_num)
{
  /* IDF 5.5 unified the GPIO LL API: gpio_ll_func_sel(hw, gpio_num, func)
   * replaces the old gpio_ll_iomux_func_sel(pin_mux_reg, func). */
  gpio_ll_func_sel(GPIO_HAL_GET_HW(GPIO_PORT_0), (uint8_t)io_num, PIN_FUNC_GPIO);
}
#elif defined CONFIG_IDF_TARGET_ESP32C5
__STATIC_INLINE __UNUSED void GPIO_FUNCTION_SET(int io_num)
{
  /* C5 IO_MUX: GPIO function is 1 (PIN_FUNC_GPIO in soc/io_mux_reg.h).
   * gpio_ll_func_sel() also detaches GPIO13/14 from the USB Serial/JTAG pads
   * when they are re-used as GPIO (not used by the C5 reference board). */
  gpio_ll_func_sel(&GPIO, (uint8_t)io_num, PIN_FUNC_GPIO);
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32
__STATIC_INLINE __UNUSED void GPIO_SET_DIRECTION_NORMAL_OUT(int io_num)
{
    GPIO.enable_w1ts = (0x1 << io_num);
    // PP out
    GPIO.pin[io_num].pad_driver = 0;
}
#elif defined CONFIG_IDF_TARGET_ESP32C3
__STATIC_INLINE __UNUSED void GPIO_SET_DIRECTION_NORMAL_OUT(int io_num)
{
    GPIO.enable_w1ts.enable_w1ts = (0x1 << io_num);
    // PP out
    GPIO.pin[io_num].pad_driver = 0;
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
__STATIC_INLINE __UNUSED void GPIO_SET_DIRECTION_NORMAL_OUT(int io_num)
{
    gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num);
    // PP out
    gpio_ll_od_disable(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num);
}
#elif defined CONFIG_IDF_TARGET_ESP32C5
__STATIC_INLINE __UNUSED void GPIO_SET_DIRECTION_NORMAL_OUT(int io_num)
{
    gpio_ll_output_enable(&GPIO, (uint32_t)io_num);
    // PP out
    gpio_ll_od_disable(&GPIO, (uint32_t)io_num);
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_HIGH(int io_num)
{
  GPIO.out_w1ts |= (0x1 << io_num);
}
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_LOW(int io_num)
{
  GPIO.out_w1tc |= (0x1 << io_num);
}
#elif defined CONFIG_IDF_TARGET_ESP32C3 || defined CONFIG_IDF_TARGET_ESP32S3
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_HIGH(int io_num)
{
  gpio_ll_set_level(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num, 1);
}
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_LOW(int io_num)
{
  gpio_ll_set_level(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num, 0);
}
#elif defined CONFIG_IDF_TARGET_ESP32C5
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_HIGH(int io_num)
{
  gpio_ll_set_level(&GPIO, (uint32_t)io_num, 1);
}
__STATIC_INLINE __UNUSED void GPIO_SET_LEVEL_LOW(int io_num)
{
  gpio_ll_set_level(&GPIO, (uint32_t)io_num, 0);
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32
__STATIC_INLINE __UNUSED int GPIO_GET_LEVEL(int io_num)
{
  return ((GPIO.in >> io_num) & 0x1) ? 1 : 0;
}
#elif defined CONFIG_IDF_TARGET_ESP32C3 || defined CONFIG_IDF_TARGET_ESP32S3
__STATIC_INLINE __UNUSED int GPIO_GET_LEVEL(int io_num)
{
  return gpio_ll_get_level(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num);
}
#elif defined CONFIG_IDF_TARGET_ESP32C5
__STATIC_INLINE __UNUSED int GPIO_GET_LEVEL(int io_num)
{
  return gpio_ll_get_level(&GPIO, (uint32_t)io_num);
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32 || defined CONFIG_IDF_TARGET_ESP32C3
__STATIC_INLINE __UNUSED void GPIO_PULL_UP_ONLY_SET(int io_num)
{
  // disable pull down
  REG_CLR_BIT(GPIO_PIN_MUX_REG[io_num], FUN_PD);
  // enable pull up
  REG_SET_BIT(GPIO_PIN_MUX_REG[io_num], FUN_PU);
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
__STATIC_INLINE __UNUSED void GPIO_PULL_UP_ONLY_SET(int io_num)
{
  // disable pull down
  gpio_ll_pulldown_dis(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num);
  // enable pull up
  gpio_ll_pullup_en(GPIO_HAL_GET_HW(GPIO_PORT_0), io_num);
}
#elif defined CONFIG_IDF_TARGET_ESP32C5
__STATIC_INLINE __UNUSED void GPIO_PULL_UP_ONLY_SET(int io_num)
{
  // disable pull down
  gpio_ll_pulldown_dis(&GPIO, (uint32_t)io_num);
  // enable pull up
  gpio_ll_pullup_en(&GPIO, (uint32_t)io_num);
}
#endif


/*
 * Dedicated-GPIO fast path hooks (A 2025 fork).
 *
 * The SWD clock / data pins are mapped to the CPU dedicated GPIO outputs in
 * spi_switch.c's DAP_SPI_Deinit():
 *
 *   ESP32-S3: SWCLK -> CORE1_GPIO_OUT1 (bit 1), SWDIO -> CORE1_GPIO_OUT0 (bit 0)
 *             input:  CORE1_GPIO_IN0 <- SWDIO pad
 *   ESP32-C3: SWCLK -> CPU_GPIO_OUT1  (bit 1), SWDIO -> CPU_GPIO_OUT0  (bit 0)
 *             input:  CPU_GPIO_IN0  <- SWDIO pad, OEN CSR bits 0/1
 *
 * These macros are optional: DAP_config.h falls back to the generic GPIO
 * accessors when they are not defined (see PIN_SWCLK_TCK_SET etc.).
 */
#if defined CONFIG_IDF_TARGET_ESP32S3
// `ee.set_bit_gpio_out 0x2` == set dedicated output bit 1 (SWCLK)
#define SWCLK_SET() do { asm volatile("ee.set_bit_gpio_out 0x2"); } while (0)
#define SWCLK_CLR() do { asm volatile("ee.clr_bit_gpio_out 0x2"); } while (0)
// bit 0 (SWDIO)
#define SWDIO_SET() do { asm volatile("ee.set_bit_gpio_out 0x1"); } while (0)
#define SWDIO_CLR() do { asm volatile("ee.clr_bit_gpio_out 0x1"); } while (0)
#define SWDIO_GET_IN() ({ dedic_gpio_cpu_ll_read_in() & 0x1; })
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C5)
/* ESP32-C3 / ESP32-C5: RISC-V CPU dedicated-GPIO CSR user registers
 * (hal/dedic_gpio_cpu_ll.h: CSR_GPIO_OEN_USER/IN_USER/OUT_USER).
 * C5 = 8 dedicated GPIO channels, same CSR mechanism as C3. */
#define SWCLK_SET() do { RV_SET_CSR(CSR_GPIO_OUT_USER, 2); } while (0)
#define SWCLK_CLR() do { RV_CLEAR_CSR(CSR_GPIO_OUT_USER, 2); } while (0)
#define SWDIO_SET() do { RV_SET_CSR(CSR_GPIO_OUT_USER, 1); } while (0)
#define SWDIO_CLR() do { RV_CLEAR_CSR(CSR_GPIO_OUT_USER, 1); } while (0)
#define SWDIO_GET_IN() ({ RV_READ_CSR(CSR_GPIO_IN_USER) & 0x1; })
// OEN user CSR bit 0 (SWDIO only; SWCLK is always driven while in GPIO mode)
#define SWDIO_OUT_ENABLE() do { RV_SET_CSR(CSR_GPIO_OEN_USER, 1); } while (0)
#define SWDIO_OUT_DISABLE() do { RV_CLEAR_CSR(CSR_GPIO_OEN_USER, 1); } while (0)
#endif


#endif
