/**
 * @file spi_switch.c
 * @author windowsair
 * @brief Switching between SPI mode and IO mode
 * @change: 2020-11-25 first version
 *          2021-2-11 Transmission mode switching test passed
 *          2022-9-15 Support ESP32C3
 *          2026-10 WP-103 merge: A 2025 dedicated-GPIO mapping (S3 CORE1_GPIO_*,
 *                    C3 CPU_GPIO_* + CSR OEN), IDF5 HAL call forms from the C
 *                    tree, ESP8266 removed, DAP_SPI_Enable/Disable removed.
 * @version 0.5
 * @date 2026-10-20
 *
 * @copyright MIT License
 *
 */
#include "sdkconfig.h"

#include <stdbool.h>

#include "cmsis-dap/include/cmsis_compiler.h"
#include "cmsis-dap/include/spi_switch.h"
#include "cmsis-dap/include/gpio_common.h"
#include "DAP_config.h"

#if defined CONFIG_IDF_TARGET_ESP32C3
// CSR_GPIO_*_USER + RV_*_CSR() for the dedicated-GPIO fast path
#include "hal/dedic_gpio_cpu_ll.h"
#endif

#if !defined CONFIG_IDF_TARGET_ESP32 && !defined CONFIG_IDF_TARGET_ESP32C3 && !defined CONFIG_IDF_TARGET_ESP32S3
#error "unsupported target: DAP supports esp32 / esp32c3 / esp32s3 only"
#endif

/*
 * The 40 MHz SPI acceleration path uses IO_MUX function numbers and the CPU
 * dedicated-GPIO channel mapping, both of which are hard-wired to specific pads
 * on each SoC. The board contract keeps the SWD pins selectable, so make an
 * unsupported pin map fail at compile time instead of silently producing wrong
 * waveforms. Keep this in sync with components/board/boards/<board>.h.
 */
#if defined CONFIG_IDF_TARGET_ESP32
  #if (PIN_SWCLK != 14) || (PIN_SWDIO_MOSI != 13)
    #error "ESP32 SPI-accelerated DAP requires PIN_SWCLK=GPIO14 and PIN_SWDIO_MOSI=GPIO13"
  #endif
#elif defined CONFIG_IDF_TARGET_ESP32C3
  #if (PIN_SWCLK != 6) || (PIN_SWDIO_MOSI != 7)
    #error "ESP32-C3 SPI-accelerated DAP requires PIN_SWCLK=GPIO6 and PIN_SWDIO_MOSI=GPIO7"
  #endif
#elif defined CONFIG_IDF_TARGET_ESP32S3
  #if (PIN_SWCLK != 12) || (PIN_SWDIO_MOSI != 11)
    #error "ESP32-S3 SPI-accelerated DAP requires PIN_SWCLK=GPIO12 and PIN_SWDIO_MOSI=GPIO11"
  #endif
#endif

#if defined CONFIG_IDF_TARGET_ESP32
    #define DAP_SPI SPI2
    // Note that the index starts at 0, so we are using function 2(SPI).
    #define FUNC_SPI 1
    #define SPI2_HOST 1
    #define SPI_LL_RST_MASK (SPI_OUT_RST | SPI_IN_RST | SPI_AHBM_RST | SPI_AHBM_FIFO_RST)
#elif defined CONFIG_IDF_TARGET_ESP32C3
    #define DAP_SPI GPSPI2
#elif defined CONFIG_IDF_TARGET_ESP32S3
    #define DAP_SPI GPSPI2
#endif


typedef enum {
    SPI_40MHz_DIV = 2,
    // SPI_80MHz_DIV = 1, //// FIXME: high speed clock
} spi_clk_div_t;



#if defined CONFIG_IDF_TARGET_ESP32
void DAP_SPI_Init()
{
    // In esp32, the driving of GPIO should be stopped,
    // otherwise there will be issue in the spi
    GPIO.out_w1tc = (0x1 << PIN_SWCLK);
    GPIO.out_w1tc = (0x1 << PIN_SWDIO_MOSI);

    // Enable spi module, We use SPI2(HSPI)
    DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI2_CLK_EN);
    DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI2_RST);


    // We will use IO_MUX to get the maximum speed.
    GPIO.func_in_sel_cfg[HSPID_IN_IDX].sig_in_sel = 0;   // IO_MUX direct connnect
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);  // MOSI
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel = 0;   // use output enable signal from peripheral
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_inv_sel = 0; // do not invert the output value
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI], FUNC_SPI);

    // GPIO.func_in_sel_cfg[HSPIQ_IN_IDX].sig_in_sel = 0;
    // PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO]);       // MISO
    // GPIO.func_out_sel_cfg[PIN_SWDIO].oen_sel = 0;
    // GPIO.func_out_sel_cfg[PIN_SWDIO].oen_inv_sel = 0;
    // PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWDIO], FUNC_SPI);

    GPIO.func_in_sel_cfg[HSPICLK_IN_IDX].sig_in_sel = 0;
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWCLK]);        // SCLK
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_sel = 0;
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_inv_sel = 0;
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_SPI);


    // Not using DMA
    // esp32 only
    DPORT_SET_PERI_REG_BITS(DPORT_SPI_DMA_CHAN_SEL_REG, 3, 0, (SPI2_HOST * 2));


    // Reset DMA
    DAP_SPI.dma_conf.val |= SPI_LL_RST_MASK;
    DAP_SPI.dma_out_link.start = 0;
    DAP_SPI.dma_in_link.start = 0;
    DAP_SPI.dma_conf.val &= ~SPI_LL_RST_MASK;
    // Disable DMA
    DAP_SPI.dma_conf.dma_continue = 0;


    // Set to Master mode
    DAP_SPI.slave.slave_mode = false;


    // use all 64 bytes of the buffer
    DAP_SPI.user.usr_mosi_highpart = false;
    DAP_SPI.user.usr_miso_highpart = false;


    // Disable cs pin
    DAP_SPI.user.cs_setup = false;
    DAP_SPI.user.cs_hold = false;

    // Disable CS signal
    DAP_SPI.pin.cs0_dis = 1;
    DAP_SPI.pin.cs1_dis = 1;
    DAP_SPI.pin.cs2_dis = 1;

    // Duplex transmit
    DAP_SPI.user.doutdin = false;  // half dulex


    // Set data bit order
    DAP_SPI.ctrl.wr_bit_order = 1;   // SWD -> LSB
    DAP_SPI.ctrl.rd_bit_order = 1;   // SWD -> LSB
    // Set data byte order
    DAP_SPI.user.wr_byte_order = 0;  // SWD -> litte_endian && Risc V -> litte_endian
    DAP_SPI.user.rd_byte_order = 0;  // SWD -> litte_endian && Risc V -> litte_endian

    // Set dummy
    DAP_SPI.user.usr_dummy = 0; // not use

    // Set spi clk: 40Mhz 50% duty
    // CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);

    // See esp32 TRM `SPI_CLOCK_REG`
    DAP_SPI.clock.clk_equ_sysclk = false;
    DAP_SPI.clock.clkdiv_pre = 0;
    DAP_SPI.clock.clkcnt_n = SPI_40MHz_DIV - 1;
    DAP_SPI.clock.clkcnt_h = SPI_40MHz_DIV / 2 - 1;
    DAP_SPI.clock.clkcnt_l = SPI_40MHz_DIV - 1;
    // Dummy is not required, but it may still need to be delayed
    // by half a clock cycle (espressif)


    // MISO delay setting
    DAP_SPI.user.ck_i_edge = true; //// TODO: may be used in slave mode?
    DAP_SPI.ctrl2.miso_delay_mode = 0;
    DAP_SPI.ctrl2.miso_delay_num = 0;


    // Set the clock polarity and phase CPOL = 1, CPHA = 0
    DAP_SPI.pin.ck_idle_edge = 1;  // HIGH while idle
    DAP_SPI.user.ck_out_edge = 0;

    // No command and addr for now
    DAP_SPI.user.usr_command = 0;
    DAP_SPI.user.usr_addr = 0;
}
#elif defined CONFIG_IDF_TARGET_ESP32C3
void DAP_SPI_Init()
{
    periph_ll_enable_clk_clear_rst(PERIPH_SPI2_MODULE);

    // In esp32, the driving of GPIO should be stopped,
    // otherwise there will be issue in the spi
    GPIO.out_w1tc.out_w1tc = (0x1 << PIN_SWCLK);
    GPIO.out_w1tc.out_w1tc = (0x1 << PIN_SWDIO_MOSI);


    // We will use IO_MUX to get the maximum speed.
    GPIO.func_in_sel_cfg[FSPID_IN_IDX].sig_in_sel = 0;   // IO_MUX direct connnect
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);  // MOSI
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel = 0;   // use output enable signal from peripheral
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_inv_sel = 0; // do not invert the output value
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI], FUNC_MTDO_FSPID);


    GPIO.func_in_sel_cfg[FSPICLK_IN_IDX].sig_in_sel = 0;
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWCLK]);        // SCLK
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_sel = 0;
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_inv_sel = 0;
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_MTCK_FSPICLK);


    // Not using DMA
    // esp32c3 only
    DAP_SPI.user.usr_conf_nxt = 0;
    DAP_SPI.slave.usr_conf = 0;
    DAP_SPI.dma_conf.dma_rx_ena = 0;
    DAP_SPI.dma_conf.dma_tx_ena = 0;

    // Set to Master mode
    DAP_SPI.slave.slave_mode = false;

    // use all 64 bytes of the buffer
    DAP_SPI.user.usr_mosi_highpart = false;
    DAP_SPI.user.usr_miso_highpart = false;

    // Disable cs pin
    DAP_SPI.user.cs_setup = false;
    DAP_SPI.user.cs_hold = false;

    // Disable CS signal
    DAP_SPI.misc.cs0_dis = 1;
    DAP_SPI.misc.cs1_dis = 1;
    DAP_SPI.misc.cs2_dis = 1;
    DAP_SPI.misc.cs3_dis = 1;
    DAP_SPI.misc.cs4_dis = 1;
    DAP_SPI.misc.cs5_dis = 1;

    // Duplex transmit
    DAP_SPI.user.doutdin = false;  // half dulex

    // Set data bit order
    DAP_SPI.ctrl.wr_bit_order = 1;   // SWD -> LSB
    DAP_SPI.ctrl.rd_bit_order = 1;   // SWD -> LSB

    // Set dummy
    DAP_SPI.user.usr_dummy = 0; // not use

    // Set spi clk: 40Mhz 50% duty
    // CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);

    // See esp32c3 TRM `SPI_CLOCK_REG`
    DAP_SPI.clock.clk_equ_sysclk = false;
    DAP_SPI.clock.clkdiv_pre = 0;
    DAP_SPI.clock.clkcnt_n = SPI_40MHz_DIV - 1;
    DAP_SPI.clock.clkcnt_h = SPI_40MHz_DIV / 2 - 1;
    DAP_SPI.clock.clkcnt_l = SPI_40MHz_DIV - 1;

    // MISO delay setting
    DAP_SPI.user.rsck_i_edge = true;
    DAP_SPI.din_mode.din0_mode = 0;
    DAP_SPI.din_mode.din1_mode = 0;
    DAP_SPI.din_mode.din2_mode = 0;
    DAP_SPI.din_mode.din3_mode = 0;
    DAP_SPI.din_num.din0_num = 0;
    DAP_SPI.din_num.din1_num = 0;
    DAP_SPI.din_num.din2_num = 0;
    DAP_SPI.din_num.din3_num = 0;

    // Set the clock polarity and phase CPOL = 1, CPHA = 0
    DAP_SPI.misc.ck_idle_edge = 1;  // HIGH while idle
    DAP_SPI.user.ck_out_edge = 0;

    // enable spi clock
    DAP_SPI.clk_gate.clk_en = 1;
    DAP_SPI.clk_gate.mst_clk_active = 1;
    DAP_SPI.clk_gate.mst_clk_sel = 1;

    // No command and addr for now
    DAP_SPI.user.usr_command = 0;
    DAP_SPI.user.usr_addr = 0;
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
void DAP_SPI_Init()
{
    periph_ll_enable_clk_clear_rst(PERIPH_SPI2_MODULE);

    // In esp32, the driving of GPIO should be stopped,
    // otherwise there will be issue in the spi
    gpio_ll_set_level(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI, 0);
    gpio_ll_set_level(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWCLK, 0);

    // We will use IO_MUX to get the maximum speed.
    gpio_ll_iomux_in(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWCLK, FSPICLK_IN_IDX);
    gpio_ll_iomux_out(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWCLK, FUNC_GPIO12_FSPICLK, 0);

    gpio_ll_iomux_in(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI, FSPID_IN_IDX);
    gpio_ll_iomux_out(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI, FUNC_GPIO11_FSPID, 0);

    // A 2025 fork: hand the pad output-enable back to the GPIO peripheral so
    // that DAP_SPI_Deinit() can drive the pads from the CPU dedicated GPIOs.
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel = 0;
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_sel = 0;

    // Not using DMA
    DAP_SPI.user.usr_conf_nxt = 0;
    DAP_SPI.slave.usr_conf = 0;
    DAP_SPI.dma_conf.dma_rx_ena = 0;
    DAP_SPI.dma_conf.dma_tx_ena = 0;

    // Set to Master mode
    DAP_SPI.slave.slave_mode = false;

    // use all 64 bytes of the buffer
    DAP_SPI.user.usr_mosi_highpart = false;
    DAP_SPI.user.usr_miso_highpart = false;

    // Disable cs pin
    DAP_SPI.user.cs_setup = false;
    DAP_SPI.user.cs_hold = false;

    // Disable CS signal
    DAP_SPI.misc.cs0_dis = 1;
    DAP_SPI.misc.cs1_dis = 1;
    DAP_SPI.misc.cs2_dis = 1;
    DAP_SPI.misc.cs3_dis = 1;
    DAP_SPI.misc.cs4_dis = 1;
    DAP_SPI.misc.cs5_dis = 1;

    // Duplex transmit
    DAP_SPI.user.doutdin = false;  // half dulex

    // Set data bit order
    DAP_SPI.ctrl.wr_bit_order = 1;   // SWD -> LSB
    DAP_SPI.ctrl.rd_bit_order = 1;   // SWD -> LSB

    // Set dummy
    DAP_SPI.user.usr_dummy = 0; // not use

    // Set spi clk: 40Mhz 50% duty
    // CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);

    // See TRM `SPI_CLOCK_REG`
    DAP_SPI.clock.clk_equ_sysclk = false;
    DAP_SPI.clock.clkdiv_pre = 0;
    DAP_SPI.clock.clkcnt_n = SPI_40MHz_DIV - 1;
    DAP_SPI.clock.clkcnt_h = SPI_40MHz_DIV / 2 - 1;
    DAP_SPI.clock.clkcnt_l = SPI_40MHz_DIV - 1;

    // MISO delay setting
    DAP_SPI.user.rsck_i_edge = true;
    DAP_SPI.din_mode.din0_mode = 0;
    DAP_SPI.din_mode.din1_mode = 0;
    DAP_SPI.din_mode.din2_mode = 0;
    DAP_SPI.din_mode.din3_mode = 0;
    DAP_SPI.din_num.din0_num = 0;
    DAP_SPI.din_num.din1_num = 0;
    DAP_SPI.din_num.din2_num = 0;
    DAP_SPI.din_num.din3_num = 0;

    // Set the clock polarity and phase CPOL = 1, CPHA = 0
    DAP_SPI.misc.ck_idle_edge = 1;  // HIGH while idle
    DAP_SPI.user.ck_out_edge = 0;

    // enable spi clock
    DAP_SPI.clk_gate.clk_en = 1;
    DAP_SPI.clk_gate.mst_clk_active = 1;
    DAP_SPI.clk_gate.mst_clk_sel = 1;

    // No command and addr for now
    DAP_SPI.user.usr_command = 0;
    DAP_SPI.user.usr_addr = 0;
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32
/**
 * @brief Switch back to plain GPIO mode
 */
__FORCEINLINE void DAP_SPI_Deinit()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], PIN_FUNC_GPIO);
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI], PIN_FUNC_GPIO); // MOSI

    // enable SWCLK output
    GPIO.enable_w1ts = (0x01 << PIN_SWCLK);

    // enable MOSI output & input
    GPIO.enable_w1ts = (0x1 << PIN_SWDIO_MOSI);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);
}
#elif defined CONFIG_IDF_TARGET_ESP32C3
/**
 * @brief Switch back to CPU-GPIO (dedicated GPIO) mode.
 *
 * C3 cannot use the normal GPIO output path for the 40 MHz bit-banging:
 * SWCLK/SWDIO are driven from the RISC-V CSR user GPIOs (bit 1 = SWCLK,
 * bit 0 = SWDIO) and SWDIO is sampled through CSR_GPIO_IN_USER bit 0.
 * This mapping is used by DAP_config.h via SWCLK_SET()/SWDIO_SET()/...
 */
__FORCEINLINE void DAP_SPI_Deinit()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], PIN_FUNC_GPIO);
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI], PIN_FUNC_GPIO); // MOSI

    GPIO.func_out_sel_cfg[PIN_SWCLK].func_sel = CPU_GPIO_OUT1_IDX;
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_sel = 0;

    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].func_sel = CPU_GPIO_OUT0_IDX;
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel = 0;

    GPIO.func_in_sel_cfg[CPU_GPIO_IN0_IDX].sig_in_sel = 1;
    GPIO.func_in_sel_cfg[CPU_GPIO_IN0_IDX].func_sel = PIN_SWDIO_MOSI;

    // enable SWCLK/MOSI output (CSR user GPIO OEN bits 0 and 1)
    RV_WRITE_CSR(CSR_GPIO_OEN_USER, 3);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI]);
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
/**
 * @brief Switch back to CPU-GPIO (dedicated GPIO) mode.
 *
 * S3 drives SWCLK/SWDIO through the CORE1 dedicated GPIO outputs (bit 1 =
 * SWCLK, bit 0 = SWDIO) and samples SWDIO through CORE1_GPIO_IN0. The pad
 * output enables stay in the GPIO peripheral (oen_sel=1), which lets
 * PIN_SWDIO_OUT_ENABLE()/DISABLE() implement the SWD turnaround.
 */
__FORCEINLINE void DAP_SPI_Deinit()
{
    gpio_ll_iomux_func_sel(GPIO_PIN_MUX_REG[PIN_SWCLK], PIN_FUNC_GPIO);
    gpio_ll_iomux_func_sel(GPIO_PIN_MUX_REG[PIN_SWDIO_MOSI], PIN_FUNC_GPIO); // MOSI

    // SWCLK output
    GPIO.func_out_sel_cfg[PIN_SWCLK].func_sel = CORE1_GPIO_OUT1_IDX;
    GPIO.func_out_sel_cfg[PIN_SWCLK].oen_sel = 1;
    gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWCLK);

    // SWDIO output
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].func_sel = CORE1_GPIO_OUT0_IDX;
    GPIO.func_out_sel_cfg[PIN_SWDIO_MOSI].oen_sel = 1;
    gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI);

    // SWDIO input
    GPIO.func_in_sel_cfg[CORE1_GPIO_IN0_IDX].sig_in_sel = 1;
    GPIO.func_in_sel_cfg[CORE1_GPIO_IN0_IDX].func_sel = PIN_SWDIO_MOSI;
    gpio_ll_input_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), PIN_SWDIO_MOSI);
}
#endif


#if defined CONFIG_IDF_TARGET_ESP32
/**
 * @brief Gain control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Acquire()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_SPI);
}


/**
 * @brief Release control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Release()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], PIN_FUNC_GPIO);
    GPIO.enable_w1ts = (0x01 << PIN_SWCLK);
}
#elif defined CONFIG_IDF_TARGET_ESP32C3
/**
 * @brief Gain control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Acquire()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_MTCK_FSPICLK);
}


/**
 * @brief Release control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Release()
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_MTCK_GPIO6);
}
#elif defined CONFIG_IDF_TARGET_ESP32S3
/**
 * @brief Gain control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Acquire()
{
    gpio_ll_iomux_func_sel(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_GPIO12_FSPICLK);
}


/**
 * @brief Release control of SPI
 *
 */
__FORCEINLINE void DAP_SPI_Release()
{
    gpio_ll_iomux_func_sel(GPIO_PIN_MUX_REG[PIN_SWCLK], FUNC_GPIO12_GPIO12);
}
#endif
