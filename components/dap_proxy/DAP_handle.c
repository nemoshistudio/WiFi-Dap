/**
 * @file DAP_handle.c
 * @brief Handle DAP packets and transaction push
 * @version 0.6
 * @change: 2020.02.04 first version
 *          2020.11.11 support WinUSB mode
 *          2021.02.17 support SWO
 *          2021.10.03 try to handle unlink behavior
 *          2025.xx.xx A (wireless-esp8266-dap fork): status LED init,
 *                     DAP_BUFFER_NUM 10/20 selection
 *
 * WiFi Dap merge notes (WP-112)
 * ------------------------------
 *  - Merged from the A 2025 version (dap_led_init, DAP_BUFFER_NUM) with the
 *    C/IDF5 platform layer (printf/component includes/BSD sockets).  The A tree
 *    configured DAP_BUFFER_NUM from USE_MDNS/USE_OTA; the merged tree always
 *    keeps the Wi-Fi/mDNS/web stack, so the 10-slot layout is used.
 *  - Ring buffer lifecycle is owned by this task only (see DAP_handle.h).
 *    The TCP/WebSocket tasks post reset requests; they never free the buffers.
 *  - Host-side response clipping keeps an over-long DAP response (P0 in
 *    analysis/REPORT.md) from being pushed to the USBIP/WS transports.
 *  - The A-tree 8266 target is not supported by this project; no 8266 branch.
 *
 * @copyright Copyright (c) 2021
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "usbip_server.h"
#include "DAP_handle.h"
#include "main/dap_configuration.h"
#include "cmsis-dap/include/DAP.h"
#include "cmsis-dap/include/swo.h"
#include "cmsis-dap/include/gpio_op.h"
#include "DAP_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>

#include "lwip/err.h"
#include "lwip/sockets.h"

/* A 2025: 10 slots when the Wi-Fi/mDNS/OTA stack is used, 20 otherwise.  The
 * merged tree always runs the Wi-Fi + mDNS + web stack, so use 10 slots:
 * 2 directions * 516 B * 10 = ~10 KB of ring buffer RAM. */
#define DAP_BUFFER_NUM 10

/* Bounded wait used when pushing a response into the OUT ring buffer, so that a
 * reset request (ring buffer ownership change) is always observed. */
#define DAP_RINGBUF_SEND_TIMEOUT_MS 20
/* Upper bound for keeping a full response in the DAP task while the host does
 * not drain the OUT ring; after this the response is dropped instead of
 * dead-locking the DAP/TCP tasks (analysis/REPORT.md P1 #4). */
#define DAP_RINGBUF_SEND_RETRIES 500

#if (USE_WINUSB == 1)
typedef struct
{
    uint32_t length;
    uint8_t buf[DAP_PACKET_SIZE];
} DapPacket_t;
#else
typedef struct
{
    uint8_t buf[DAP_PACKET_SIZE];
} DapPacket_t;
#endif

#define DAP_HANDLE_SIZE (sizeof(DapPacket_t))


int kRestartDAPHandle = NO_SIGNAL;
TaskHandle_t kDAPTaskHandle = NULL;


static DapPacket_t DAPDataProcessed;
static int dap_respond = 0;

// SWO Trace
static uint8_t *swo_data_to_send = NULL;
static uint32_t swo_data_num;

// DAP handle
static RingbufHandle_t dap_dataIN_handle = NULL;
static RingbufHandle_t dap_dataOUT_handle = NULL;
static SemaphoreHandle_t data_response_mux = NULL;
/* Signalled by the DAP task after a requested ring buffer (re)configuration has
 * been applied; see dap_handle_wait_reset(). */
static SemaphoreHandle_t dap_reset_done = NULL;


void malloc_dap_ringbuf(void) {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
    {
        if (dap_dataIN_handle == NULL) {
            dap_dataIN_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
        }
        if (dap_dataOUT_handle == NULL) {
            dap_dataOUT_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
        }

        xSemaphoreGive(data_response_mux);
    }
}

void free_dap_ringbuf(void) {
    if (data_response_mux && xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
        if (dap_dataIN_handle) {
            vRingbufferDelete(dap_dataIN_handle);
        }
        if (dap_dataOUT_handle) {
            vRingbufferDelete(dap_dataOUT_handle);
        }

        dap_dataIN_handle = dap_dataOUT_handle = NULL;
        xSemaphoreGive(data_response_mux);
    }

}

void dap_handle_request_reset(enum reset_handle_t mode)
{
    /* Drop a stale completion token before posting, so that a following
     * dap_handle_wait_reset() only observes the result of this request. */
    if (dap_reset_done)
        (void)xSemaphoreTake(dap_reset_done, 0);

    kRestartDAPHandle = mode;

    if (kDAPTaskHandle)
        xTaskNotifyGive(kDAPTaskHandle);
}

int dap_handle_wait_reset(uint32_t timeout_ms)
{
    if (dap_reset_done == NULL) {
        /* The DAP task has not been started yet; it applies the pending request
         * before it starts processing DAP commands. */
        return 0;
    }

    if (xSemaphoreTake(dap_reset_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        printf("DAP: timeout waiting for ring buffer reset\r\n");
        return -1;
    }

    return 0;
}

void handle_dap_data_request(usbip_stage2_header *header, uint32_t length)
{
    uint8_t *data_in = (uint8_t *)header;
    data_in = &(data_in[sizeof(usbip_stage2_header)]);
    // Point to the beginning of the URB packet

#if (USE_WINUSB == 1)
    send_stage2_submit_data_fast(header, NULL, 0);

    /* The ring buffers can legitimately be absent while a WebSocket/elaphureLink
     * session owns the DAP data path (DAP_handle.h).  The transport is about to
     * be torn down in that case, so only answer the zero-length RET_SUBMIT and
     * do not touch the ring buffer. */
    if (dap_dataIN_handle == NULL) {
        printf("DAP: data IN ring buffer not available, request dropped\r\n");
        return;
    }

    // always send constant size buf -> cuz we don't care about the IN packet size
    // and to unify the style, we set aside the length of the section
    xRingbufferSend(dap_dataIN_handle, data_in - sizeof(uint32_t), DAP_HANDLE_SIZE, portMAX_DELAY);
    if (kDAPTaskHandle)
        xTaskNotifyGive(kDAPTaskHandle);

#else
    send_stage2_submit_data_fast(header, NULL, 0);

    if (dap_dataIN_handle == NULL) {
        printf("DAP: data IN ring buffer not available, request dropped\r\n");
        return;
    }

    xRingbufferSend(dap_dataIN_handle, data_in, DAP_HANDLE_SIZE, portMAX_DELAY);
    if (kDAPTaskHandle)
        xTaskNotifyGive(kDAPTaskHandle);

#endif
}

void handle_swo_trace_response(usbip_stage2_header *header)
{
#if (SWO_FUNCTION_ENABLE == 1)
    if (kSwoTransferBusy)
    {
        // busy indicates that there is data to be send
        printf("swo use data\r\n");
        send_stage2_submit_data(header, 0, (void *)swo_data_to_send, swo_data_num);
        SWO_TransferComplete();
    }
    else
    {
        // nothing to send.
        send_stage2_submit(header, 0, 0);
    }
#else
    send_stage2_submit(header, 0, 0);
#endif
}

// SWO Data Queue Transfer
//   buf:    pointer to buffer with data
//   num:    number of bytes to transfer
void SWO_QueueTransfer(uint8_t *buf, uint32_t num)
{
    swo_data_to_send = buf;
    swo_data_num = num;
}

/**
 * @brief Push one processed DAP response into the OUT ring buffer.
 *
 * The send is bounded so that a pending reset request (the only way the ring
 * buffers are released) is always observed; if the host never drains the OUT
 * ring the response is dropped after a bounded time instead of dead-locking the
 * DAP and TCP tasks.  Returns true when the response was queued.
 */
static bool dap_push_response(const DapPacket_t *packet)
{
    int retries = 0;

    while (dap_dataOUT_handle != NULL &&
           xRingbufferSend(dap_dataOUT_handle, (void *)packet, DAP_HANDLE_SIZE,
                           pdMS_TO_TICKS(DAP_RINGBUF_SEND_TIMEOUT_MS)) != pdTRUE)
    {
        if (kRestartDAPHandle != NO_SIGNAL) {
            printf("DAP: response dropped, ring buffer reset requested\r\n");
            return false;
        }
        if (++retries >= DAP_RINGBUF_SEND_RETRIES) {
            printf("DAP: OUT ring buffer full for too long, response dropped\r\n");
            return false;
        }
    }

    return dap_dataOUT_handle != NULL;
}

void DAP_Thread(void *argument)
{
    size_t packetSize;
    int resLength;
    DapPacket_t *item;

    dap_dataIN_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
    dap_dataOUT_handle = xRingbufferCreate(DAP_HANDLE_SIZE * DAP_BUFFER_NUM, RINGBUF_TYPE_BYTEBUF);
    data_response_mux = xSemaphoreCreateMutex();
    dap_reset_done = xSemaphoreCreateBinary();
    kDAPTaskHandle = xTaskGetCurrentTaskHandle();

    /* A 2025: initialize the CMSIS-DAP status LEDs (board pins, no-op when the
     * selected board has no status LEDs). */
    dap_led_init();

    if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL ||
        data_response_mux == NULL)
    {
        printf("Can not create DAP ringbuf/mux!\r\n");
        vTaskDelete(NULL);
    }
    for (;;)
    {

        while (1)
        {
            if (kRestartDAPHandle != NO_SIGNAL)
            {
                /* The DAP task owns the ring buffer lifecycle: free/recreate
                 * only here, never in the TCP/WebSocket tasks. */
                enum reset_handle_t mode = (enum reset_handle_t)kRestartDAPHandle;

                free_dap_ringbuf();

                if (mode == RESET_HANDLE) {
                    malloc_dap_ringbuf();
                    if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL)
                    {
                        printf("Can not create DAP ringbuf/mux!\r\n");
                        vTaskDelete(NULL);
                    }
                }

                kRestartDAPHandle = NO_SIGNAL;

                if (dap_reset_done)
                    xSemaphoreGive(dap_reset_done);
            }

            ulTaskNotifyTake(pdFALSE, portMAX_DELAY); // wait event


            if (dap_dataIN_handle == NULL || dap_dataOUT_handle == NULL) {
                continue; // may be use elaphureLink, wait...
            }


            packetSize = 0;
            item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataIN_handle, &packetSize,
                                                         pdMS_TO_TICKS(1), DAP_HANDLE_SIZE);
            if (packetSize == 0)
            {
                break;
            }

            else if (packetSize < DAP_HANDLE_SIZE)
            {
                printf("Wrong data in packet size:%d , data in remain: %d\r\n", packetSize,
                       (int) xRingbufferGetMaxItemSize(dap_dataIN_handle));
                vRingbufferReturnItem(dap_dataIN_handle, (void *)item);
                break;
                // This may not happen because there is a semaphore acquisition
            }

            if (item->buf[0] == ID_DAP_QueueCommands)
            {
                item->buf[0] = ID_DAP_ExecuteCommands;
            }

            resLength = DAP_ProcessCommand((uint8_t *)item->buf, (uint8_t *)DAPDataProcessed.buf); // use first 4 byte to save length
            resLength &= 0xFFFF;                                                                   // res length in lower 16 bits

            vRingbufferReturnItem(dap_dataIN_handle, (void *)item); // process done.

            /* Host-side clip for an over-long DAP response (P0 in
             * analysis/REPORT.md).  The DAP command layer is responsible for
             * refusing over-long counts; this guard only prevents an over-long
             * response from being handed to the USBIP/WS transports. */
            if (resLength > (int)DAP_PACKET_SIZE) {
                printf("DAP: response length %d exceeds DAP_PACKET_SIZE %u, clipped\r\n",
                       resLength, (unsigned)DAP_PACKET_SIZE);
                resLength = (int)DAP_PACKET_SIZE;
            }

            // now prepare to reply
#if (USE_WINUSB == 1)
            DAPDataProcessed.length = resLength;
#endif
            if (dap_push_response(&DAPDataProcessed))
            {
                if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
                {
                    ++dap_respond;
                    xSemaphoreGive(data_response_mux);
                }
            }
        }
    }
}

int fast_reply(uint8_t *buf, uint32_t length, int dap_req_num)
{
    usbip_stage2_header *buf_header = (usbip_stage2_header *)buf;

    if (dap_req_num > 0) {
        DapPacket_t *item;
        size_t packetSize = 0;

        /* No ring buffer means the DAP data path is currently owned by a
         * WebSocket/elaphureLink session (see DAP_handle.h).  Answer with an
         * empty RET_SUBMIT so the host URB is not left hanging forever. */
        if (dap_dataOUT_handle == NULL) {
            printf("fast_reply: DAP OUT ring buffer not available\r\n");
            buf_header->base.command = PP_HTONL(USBIP_STAGE2_RSP_SUBMIT);
            buf_header->base.direction = PP_HTONL(USBIP_DIR_OUT);
            buf_header->u.ret_submit.status = 0;
            buf_header->u.ret_submit.data_length = 0;
            buf_header->u.ret_submit.error_count = 0;
            return 0;
        }

        item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataOUT_handle, &packetSize,
                                                     portMAX_DELAY, DAP_HANDLE_SIZE);
        if (packetSize == DAP_HANDLE_SIZE) {
#if (USE_WINUSB == 1)
            send_stage2_submit_data_fast((usbip_stage2_header *)buf, item->buf, item->length);
#else
            send_stage2_submit_data_fast((usbip_stage2_header *)buf, item->buf, DAP_HANDLE_SIZE);
#endif

            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE) {
                --dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
            return 1;
        } else if (packetSize > 0) {
            printf("Wrong data out packet size:%d!\r\n", packetSize);
            /* Do not leak the ring buffer item and do not leave the host URB
             * hanging (analysis/REPORT.md P2 #7). */
            vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
        }
        ////TODO: fast reply
    } else {
        buf_header->base.command = PP_HTONL(USBIP_STAGE2_RSP_SUBMIT);
        buf_header->base.direction = PP_HTONL(USBIP_DIR_OUT);
        buf_header->u.ret_submit.status = 0;
        buf_header->u.ret_submit.data_length = 0;
        buf_header->u.ret_submit.error_count = 0;
        usbip_network_send(kSock, buf, 48, 0);
        return 1;
    }

    return 0;
}

void handle_dap_unlink()
{
    // `USBIP_CMD_UNLINK` means calling `usb_unlink_urb()` or `usb_kill_urb()`.
    // Note that execution of an URB is inherently an asynchronous operation, and there may be
    // synchronization problems in the following solutions.

    // One of the reasons this happens is that the host wants to abort the URB transfer operation
    // as soon as possible. USBIP network fluctuations will also cause this error, but I don't know
    // whether this is the main reason.

    // Unlink may be applied to zero length URB of "DIR_IN", or a URB containing data.
    // In the case of unlink, for the new "DIR_IN" request, it may always return an older response,
    // which will lead to panic. This code is a compromise for eliminating the lagging response
    // caused by UNLINK. It will clean up the buffers that may have data for return to the host.
    // In general, we assume that there is at most one piece of data that has not yet been returned.
    if (dap_respond > 0 && dap_dataOUT_handle != NULL)
    {
        DapPacket_t *item;
        size_t packetSize = 0;
        item = (DapPacket_t *)xRingbufferReceiveUpTo(dap_dataOUT_handle, &packetSize,
                                                     pdMS_TO_TICKS(10), DAP_HANDLE_SIZE);
        if (packetSize == DAP_HANDLE_SIZE)
        {
            if (xSemaphoreTake(data_response_mux, portMAX_DELAY) == pdTRUE)
            {
                --dap_respond;
                xSemaphoreGive(data_response_mux);
            }

            vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
        }
        else if (packetSize > 0)
        {
            /* Return non-full items as well instead of leaking ring space
             * (analysis/REPORT.md P2 #7). */
            vRingbufferReturnItem(dap_dataOUT_handle, (void *)item);
        }
    }
}
