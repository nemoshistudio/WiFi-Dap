#ifndef __SPI_OP_H__
#define __SPI_OP_H__

#include <stdint.h>

/* WP-104: count is 32-bit on purpose.
 * DAP_SWJ_Sequence allows 1..256 bits (count byte 0 means 256) and the value
 * used to be truncated through a uint8_t parameter, turning a 256-bit sequence
 * into a 0-bit "write" (stale FIFO content). The implementations also clamp
 * count to the hardware limits below. */
#define DAP_SPI_WRITE_BITS_MAX 512U /* 64-byte SPI FIFO */
#define DAP_SPI_READ_BITS_MAX   64U /* 2 x uint32_t in DAP_SPI_ReadBits */

void DAP_SPI_WriteBits(uint32_t count, const uint8_t *buf);
void DAP_SPI_ReadBits(uint32_t count, uint8_t *buf);

void DAP_SPI_Send_Header(const uint8_t packetHeaderData, uint8_t *ack, uint8_t TrnAfterACK);
void DAP_SPI_Read_Data(uint32_t* resData, uint8_t* resParity);
void DAP_SPI_Write_Data(uint32_t data, uint8_t parity);

void DAP_SPI_Generate_Cycle(uint8_t num);
void DAP_SPI_Fast_Cycle();

void DAP_SPI_Protocol_Error_Read();
void DAP_SPI_Protocol_Error_Write();


#endif
