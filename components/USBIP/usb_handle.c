/**
 * @file USB_handle.c
 * @brief Handle all Standard Device Requests on endpoint 0
 * @version 0.1
 * @date 2020-01-23
 *
 * @copyright Copyright (c) 2020
 *
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "usbip_server.h"
#include "components/USBIP/usb_handle.h"
#include "components/USBIP/usb_descriptor.h"
#include "components/USBIP/MSOS20_descriptor.h"

#include "lwip/err.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif

const char *strings_list[] = {
        0, // reserved: available languages  -> iInterface
        "windowsair",
        "Wireless ESP CMSIS-DAP",
        "1234",
};
// handle functions
static void handleGetDescriptor(usbip_stage2_header *header);

////TODO: may be ok
void handleUSBControlRequest(usbip_stage2_header *header)
{
    // Table 9-3. Standard Device Requests

    switch (header->u.cmd_submit.request.bmRequestType)
    {
    case 0x00: // ignore..
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_CLEAR_FEATURE:
	        printf("* CLEAR FEATURE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_FEATURE:
	        printf("* SET FEATURE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_ADDRESS:
	        printf("* SET ADDRESS\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_DESCRIPTOR:
	        printf("* SET DESCRIPTOR\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_CONFIGURATION:
	        printf("* SET CONFIGURATION\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
    case 0x01: // ignore...
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_CLEAR_FEATURE:
	        printf("* CLEAR FEATURE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_FEATURE:
	        printf("* SET FEATURE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_INTERFACE:
	        printf("* SET INTERFACE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
    case 0x02: // ignore..
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_CLEAR_FEATURE:
	        printf("* CLEAR FEATURE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_FEATURE:
	        printf("* SET INTERFACE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;

    case 0x80: // *IMPORTANT*
#if (USE_WINUSB == 0)
    case 0x81:
#endif
    {
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_GET_CONFIGURATION:
	        printf("* GET CONIFGTRATION\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_GET_DESCRIPTOR:
            handleGetDescriptor(header); ////TODO: device_qualifier
            break;
        case USB_REQ_GET_STATUS:
	        printf("* GET STATUS\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
    }
#if (USE_WINUSB == 1)
    case 0x81: // ignore...
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_GET_INTERFACE:
	        printf("* GET INTERFACE\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_SET_SYNCH_FRAME:
	        printf("* SET SYNCH FRAME\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;
        case USB_REQ_GET_STATUS:
	        printf("* GET STATUS\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
#endif
    case 0x82: // ignore...
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_GET_STATUS:
	        printf("* GET STATUS\r\n");
            send_stage2_submit_data(header, 0, 0, 0);
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
    case 0xC0: // Microsoft OS 2.0 vendor-specific descriptor
    {
        uint16_t wIndex = header->u.cmd_submit.request.wIndex.u16;
        switch (wIndex)
        {
        case MS_OS_20_DESCRIPTOR_INDEX:
	        printf("* GET MSOS 2.0 vendor-specific descriptor\r\n");
            send_stage2_submit_data(header, 0, msOs20DescriptorSetHeader, sizeof(msOs20DescriptorSetHeader));
            break;
        case MS_OS_20_SET_ALT_ENUMERATION:
            // set alternate enumeration command
            // bAltEnumCode set to 0
	        printf("Set alternate enumeration.This should not happen.\r\n");
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d,wIndex:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest, wIndex);
            break;
        }
        break;
    }
    case 0x21: // Set_Idle for HID
        switch (header->u.cmd_submit.request.bRequest)
        {
        case USB_REQ_SET_IDLE:
	        printf("* SET IDLE\r\n");
            send_stage2_submit(header, 0, 0);
            break;

        default:
	        printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	               header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
            break;
        }
        break;
    default:
	    printf("USB unknown request, bmRequestType:%d,bRequest:%d\r\n",
	           header->u.cmd_submit.request.bmRequestType, header->u.cmd_submit.request.bRequest);
        break;
    }
}

/**
 * @brief Clip a descriptor response to the host-requested wLength (USB 2.0
 *        §9.4.3: the device must never return more bytes than wLength).
 *
 * wLength == 0 keeps the legacy behaviour (return the full descriptor) because
 * some usbip clients do not fill this field; every normal GET_DESCRIPTOR from
 * Windows/Linux sets wLength >= the descriptor length.
 */
static int usb_handle_clip_response_len(usbip_stage2_header *header, int len)
{
    uint16_t wlength = (uint16_t)header->u.cmd_submit.request.wLength.u8lo |
                       (uint16_t)((uint16_t)header->u.cmd_submit.request.wLength.u8hi << 8);

    if (len < 0) {
        return 0;
    }
    if (wlength != 0 && len > (int)wlength) {
        return (int)wlength;
    }
    return len;
}

static void handleGetDescriptor(usbip_stage2_header *header)
{
    // 9.4.3 Get Descriptor
    switch (header->u.cmd_submit.request.wValue.u8hi)
    {
    case USB_DT_DEVICE: // get device descriptor
	    printf("* GET 0x01 DEVICE DESCRIPTOR\r\n");
        send_stage2_submit_data(header, 0, &kUSBd0DeviceDescriptor[0],
                                usb_handle_clip_response_len(header, sizeof(kUSBd0DeviceDescriptor)));
        break;

    case USB_DT_CONFIGURATION: // get configuration descriptor
	    printf("* GET 0x02 CONFIGURATION DESCRIPTOR\r\n");
        ////TODO: ?
        if (header->u.cmd_submit.data_length == USB_DT_CONFIGURATION_SIZE)
        {
	        printf("Sending only first part of CONFIG\r\n");

            send_stage2_submit(header, 0, header->u.cmd_submit.data_length);
            usbip_network_send(kSock, kUSBd0ConfigDescriptor, sizeof(kUSBd0ConfigDescriptor), 0);
        }
        else
        {
	        printf("Sending ALL CONFIG\r\n");
            send_stage2_submit(header, 0, sizeof(kUSBd0ConfigDescriptor) + sizeof(kUSBd0InterfaceDescriptor));
            usbip_network_send(kSock, kUSBd0ConfigDescriptor, sizeof(kUSBd0ConfigDescriptor), 0);
            usbip_network_send(kSock, kUSBd0InterfaceDescriptor, sizeof(kUSBd0InterfaceDescriptor), 0);
        }
        break;

    case USB_DT_STRING: {
        /*
         * N2 (docs/security-review-dap.md, P0): the descriptor index is fully
         * controlled by the (unauthenticated) remote host:
         *   - the old code used wValue.u8lo directly as a strings_list[] index
         *     (array has only ARRAY_SIZE entries) -> OOB pointer read,
         *   - and encoded strlen(strings_list[idx]) into char temp_buff[64]
         *     without a length bound -> stack overflow / stack info leak.
         * Fix: whitelist the index, bound the UTF-16LE encoding to the staging
         * buffer, clip to the host wLength and STALL invalid indices.
         */
        const uint8_t str_index = header->u.cmd_submit.request.wValue.u8lo;
        const char *str;
        char temp_buff[64];
        usb_string_descriptor *desc = (usb_string_descriptor *)temp_buff;
        /* Maximum UTF-16LE characters that fit into temp_buff (31 for 64 B). */
        const size_t max_chars = (sizeof(temp_buff) - sizeof(usb_string_descriptor)) / sizeof(uint16_t);
        size_t slen, wslen, buff_len;

        _Static_assert(sizeof(temp_buff) > sizeof(usb_string_descriptor),
                       "USB string descriptor staging buffer is too small");

        if (str_index == 0) {
            /* String Descriptor Zero: LANGID list */
            printf("** REQUESTED list of supported languages\r\n");
            send_stage2_submit_data(header, 0, kLangDescriptor,
                                    usb_handle_clip_response_len(header, sizeof(kLangDescriptor)));
            break;
        }

        if (str_index == 0xEE) {
            /* MS OS 1.0 string descriptor: not provided (MSOS 2.0 is announced
             * through the BOS descriptor instead); answer with no data. */
            printf("***Unsupported String descriptor (MS OS 0xEE)***\r\n");
            send_stage2_submit(header, 0, 0);
            break;
        }

        if (str_index >= ARRAY_SIZE(strings_list) || strings_list[str_index] == NULL) {
            /* Invalid index: STALL the control request (USB 2.0 §9.4.3). */
            printf("USB invalid string descriptor index:%u (STALL)\r\n",
                   (unsigned)str_index);
            send_stage2_submit(header, -EPIPE, 0);
            break;
        }

        str = strings_list[str_index];
        slen = strlen(str);
        if (slen > max_chars) {
            /* Truncate instead of overflowing the staging buffer. */
            printf("USB string descriptor %u truncated from %u to %u chars\r\n",
                   (unsigned)str_index, (unsigned)slen, (unsigned)max_chars);
            slen = max_chars;
        }
        wslen = slen * sizeof(uint16_t);
        buff_len = sizeof(usb_string_descriptor) + wslen;
        desc->bLength = (uint8_t)buff_len;
        desc->bDescriptorType = USB_DT_STRING;
        for (size_t i = 0; i < slen; ++i) {
            /* UTF-16LE: ASCII byte followed by a zero high byte */
            desc->wData[i] = (uint16_t)(uint8_t)str[i];
        }
        send_stage2_submit_data(header, 0, temp_buff,
                                usb_handle_clip_response_len(header, (int)buff_len));
        break;
    }

    case USB_DT_INTERFACE:
	    printf("* GET 0x04 INTERFACE DESCRIPTOR (UNIMPLEMENTED)\r\n");
        ////TODO:UNIMPLEMENTED
        send_stage2_submit(header, 0, 0);
        break;

    case USB_DT_ENDPOINT:
	    printf("* GET 0x05 ENDPOINT DESCRIPTOR (UNIMPLEMENTED)\r\n");
        ////TODO:UNIMPLEMENTED
        send_stage2_submit(header, 0, 0);
        break;

    case USB_DT_DEVICE_QUALIFIER:
	    printf("* GET 0x06 DEVICE QUALIFIER DESCRIPTOR\r\n");

        usb_device_qualifier_descriptor desc;

        memset(&desc, 0, sizeof(usb_device_qualifier_descriptor));

        send_stage2_submit_data(header, 0, &desc, sizeof(usb_device_qualifier_descriptor));
        break;

    case USB_DT_OTHER_SPEED_CONFIGURATION:
	    printf("GET 0x07 [UNIMPLEMENTED] USB_DT_OTHER_SPEED_CONFIGURATION\r\n");
        ////TODO:UNIMPLEMENTED
        send_stage2_submit(header, 0, 0);
        break;

    case USB_DT_INTERFACE_POWER:
	    printf("GET 0x08 [UNIMPLEMENTED] USB_DT_INTERFACE_POWER\r\n");
        ////TODO:UNIMPLEMENTED
        send_stage2_submit(header, 0, 0);
        break;
#if (USE_WINUSB == 1)
    case USB_DT_BOS:
	    printf("* GET 0x0F BOS DESCRIPTOR\r\n");
        uint32_t bos_len = header->u.cmd_submit.request.wLength.u8lo | ((uint32_t) header->u.cmd_submit.request.wLength.u8hi << 8);
        bos_len = (sizeof(bosDescriptor) < bos_len) ? sizeof(bosDescriptor) : bos_len;
        send_stage2_submit_data(header, 0, bosDescriptor, bos_len);
        break;
#else
    case USB_DT_HID_REPORT:
        printf("* GET 0x22 HID REPORT DESCRIPTOR\r\n");
        send_stage2_submit_data(header, 0, (void *)kHidReportDescriptor, sizeof(kHidReportDescriptor));
        break;
#endif
    default:
        //// TODO: ms os 1.0 descriptor
	    printf("USB unknown Get Descriptor type:%d index:%d (STALL)\r\n",
	           (int)header->u.cmd_submit.request.wValue.u8hi,
	           (int)header->u.cmd_submit.request.wValue.u8lo);
        /* N2 hardening: answer with STALL so an unsupported/malformed
         * descriptor request cannot leave the URB waiting forever. */
        send_stage2_submit(header, -EPIPE, 0);
        break;
    }
}
