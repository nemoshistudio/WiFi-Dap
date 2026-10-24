#ifndef __USBIP_SERVER_H__
#define __USBIP_SERVER_H__
#include <stdint.h>
#include <stddef.h>

#include "components/USBIP/usbip_defs.h"

enum usbip_server_state_t
{
    WAIT_DEVLIST = 0,
    WAIT_IMPORT,
    WAIT_URB,
};

extern int kSock;

int usbip_worker(uint8_t *base, uint32_t length, enum usbip_server_state_t *state);
void send_stage2_submit_data(usbip_stage2_header *req_header, int32_t status, const void * const data, int32_t data_length);
void send_stage2_submit(usbip_stage2_header *req_header, int32_t status, int32_t data_length);
void send_stage2_submit_data_fast(usbip_stage2_header *req_header, const void *const data, int32_t data_length);
/* RET_SUBMIT reply for a staging slot that already holds the 48-byte header
 * followed by the DAP response payload at offset sizeof(usbip_stage2_header).
 * Patches the header in place and sends header+payload as one TCP write, so no
 * payload copy is needed (used by the B1 pipeline hot path). */
void send_stage2_submit_data_inplace(usbip_stage2_header *req_header, int32_t data_length);
/* RET_UNLINK reply (also used by the B1 pipeline for cancelled URBs). */
void send_stage2_unlink(usbip_stage2_header *req_header);
int usbip_network_send(int s, const void *dataptr, size_t size, int flags);

#endif