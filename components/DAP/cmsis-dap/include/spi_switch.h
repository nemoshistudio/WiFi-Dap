#ifndef __SPI_SWITCH_H__
#define __SPI_SWITCH_H__

/* WP-103: A removed the DAP_SPI_Enable()/DAP_SPI_Disable() leftovers (they
 * were no-ops on every supported target and the calls were removed from
 * SW_DP.c / DAP_config.h). Do not reintroduce them. */
void DAP_SPI_Init();
void DAP_SPI_Deinit();

void DAP_SPI_Acquire();
void DAP_SPI_Release();

#endif
