/**
 * @file tcp_server.c
 * @brief Handle main tcp tasks (USBIP / elaphureLink / legacy WebSocket-DAP)
 * @version 0.2
 * @date 2020-01-22
 *
 * WiFi Dap merge notes (WP-112)
 * ------------------------------
 *  - Logic merged from the A 2025 version / base (identical) with the
 *    C/IDF5 platform layer (BSD sockets, printf, component includes).
 *  - The legacy WebSocket-DAP dispatch (A: CONFIG_USE_WEBSOCKET_DAP) is gated
 *    by the BIRDIEBOT_DAP_WS_ENABLE Kconfig option (default y,
 *    analysis/INTEGRATION-PLAN.md P7.1); elaphureLink and USBIP stay
 *    unconditional.
 *  - DAP data path lifecycle is owned by the DAP task (see DAP_handle.h).  This
 *    task only posts dap_handle_request_reset(RESET_HANDLE); it never allocates
 *    or frees DAP ring buffers (task-66: the rings live for the whole boot).
 *  - The listen socket is closed on every error path (fd leak, REPORT P3 #11)
 *    and kSock is reset to -1 after closing an accepted connection.
 *
 * @copyright Copyright (c) 2020
 *
 */
#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "usbip_server.h"
#include "DAP_handle.h"

#include "components/elaphureLink/elaphureLink_protocol.h"
#include "proxy_server_conf.h"

/* C1 hardening (second-session fix): bounded wait for the end-of-session DAP
 * reset to be applied before the next connection is accepted.  See cleanup. */
#if defined(CONFIG_BIRDIEBOT_WIRED_TRANSPORT_NCM)
#define DAP_CLEANUP_RESET_WAIT_MS 1500U
#else
#define DAP_CLEANUP_RESET_WAIT_MS 250U
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/sockets.h"

#if CONFIG_BIRDIEBOT_DAP_WS_ENABLE
#include "websocket_server.h"
#endif

int kSock = -1;

/* Drop the active DAP proxy session.  kSock is only assigned by the
 * tcp_server task and reset to -1 before the fd is closed; shutdown() from
 * another task wakes the blocked recv()/usbip loop so the session handler
 * returns through its normal cleanup path (RESET_HANDLE hand-off included). */
void tcp_server_kick_session(void)
{
    int fd = kSock;
    if (fd >= 0) {
        printf("tcp_server: kicking active DAP session fd=%d\r\n", fd);
        shutdown(fd, SHUT_RDWR);
    }
}

/* Read-only counterpart of tcp_server_kick_session(): report whether a proxy
 * session socket is currently accepted.  This intentionally does NOT touch the
 * socket or the DAP state, so diagnostics (GET /api/target/status) can be
 * sampled while an OpenOCD/Ozone/WS-DAP client is connected. */
bool tcp_server_session_active(void)
{
    return kSock >= 0;
}

/*
 * A3: larger per-socket TCP buffers for the USBIP data path.
 *
 * USBIP is a request/response protocol with small URBs (48 B header + up to
 * DAP_PACKET_SIZE payload) and a Wi-Fi round-trip per transaction, so a deep
 * socket buffer absorbs scheduling jitter between the tcp_server task (core 1,
 * prio 14) and lwIP/Wi-Fi.  Keep both values in one place for A/B tuning.
 *
 * Note: lwIP implements SO_RCVBUF (per-socket recv window) but SO_SNDBUF is a
 * no-op in this stack version; the send side stays governed by
 * CONFIG_LWIP_TCP_SND_BUF_DEFAULT (see the A3 measurement notes).
 */
#define DAP_PROXY_SOCK_SNDBUF (32 * 1024)
#define DAP_PROXY_SOCK_RCVBUF (32 * 1024)

/* Apply the A3 socket-buffer tuning; failures are logged but not fatal (the
 * stack may clamp or not implement a given option). */
static void tcp_server_tune_socket(int sock)
{
    int size;

    size = DAP_PROXY_SOCK_SNDBUF;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (void *)&size, sizeof(size)) != 0) {
        printf("tcp_server: SO_SNDBUF(%d) failed: errno %d\r\n", size, errno);
    }

    size = DAP_PROXY_SOCK_RCVBUF;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void *)&size, sizeof(size)) != 0) {
        printf("tcp_server: SO_RCVBUF(%d) failed: errno %d\r\n", size, errno);
    }
}

/* C1: the host can disappear without a TCP FIN when the USB-NCM link is
 * re-enumerated or the host network configuration is lost (observed during the
 * NCM flash-hang investigation: the device kept the old session, kSock stayed
 * open and the next attach could not be served until the socket timed out).
 * Default lwIP keepalive is ~2 h; use a few seconds so a dead USBIP peer is
 * torn down and the next attach can reset the DAP data path.
 * Keepalive only probes an idle peer: a live OpenOCD session answers the probes,
 * so an idle-but-healthy session is not dropped.  TCP_KEEP* are optional lwIP
 * options; when not available only SO_KEEPALIVE is enabled. */
static void tcp_server_enable_fast_keepalive(int sock)
{
    int on = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    {
        int idle = 5, intvl = 2, cnt = 3;
        (void)setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (void *)&idle, sizeof(idle));
        (void)setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *)&intvl, sizeof(intvl));
        (void)setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (void *)&cnt, sizeof(cnt));
    }
#endif
    (void)on;
}

void tcp_server_task(void *pvParameters)
{
    static uint8_t tcp_rx_buffer[4096]; /* static: keep it off the task stack */
    char addr_str[128];
    enum usbip_server_state_t usbip_state = WAIT_DEVLIST;
    uint8_t *data;
    int addr_family;
    int ip_protocol;
    int header;
    int ret, sz;

    int on = 1;
    while (1)
    {

#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons(DAP_PROXY_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else // IPV6
        struct sockaddr_in6 destAddr;
        bzero(&destAddr.sin6_addr.un, sizeof(destAddr.sin6_addr.un));
        destAddr.sin6_family = AF_INET6;
        destAddr.sin6_port = htons(DAP_PROXY_PORT);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
        inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif

        int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (listen_sock < 0)
        {
            printf("Unable to create socket: errno %d\r\n", errno);
            break;
        }
        printf("Socket created\r\n");

        setsockopt(listen_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&on, sizeof(on));
        setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));
        tcp_server_tune_socket(listen_sock); /* A3 */

        int err = bind(listen_sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err != 0)
        {
            printf("Socket unable to bind: errno %d\r\n", errno);
            close(listen_sock);
            break;
        }
        printf("Socket binded\r\n");

        err = listen(listen_sock, 1);
        if (err != 0)
        {
            printf("Error occured during listen: errno %d\r\n", errno);
            close(listen_sock);
            break;
        }
        printf("Socket listening\r\n");

#ifdef CONFIG_EXAMPLE_IPV6
        struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
#else
        struct sockaddr_in sourceAddr;
#endif
        uint32_t addrLen = sizeof(sourceAddr);
        while (1)
        {
            kSock = accept(listen_sock, (struct sockaddr *)&sourceAddr, &addrLen);
            if (kSock < 0)
            {
                printf("Unable to accept connection: errno %d\r\n", errno);
                /* Do not leak the listen socket when accept() fails
                 * (analysis/REPORT.md P3 #11). */
                close(listen_sock);
                break;
            }
            setsockopt(kSock, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on));
            tcp_server_tune_socket(kSock); /* A3 */
            tcp_server_enable_fast_keepalive(kSock); /* C1: stale-session cleanup */
            printf("Socket accepted\r\n");

            // Read header
            sz = 4;
            data = &tcp_rx_buffer[0];
            do {
                ret = recv(kSock, data, sz, 0);
                if (ret <= 0)
                    goto cleanup;
                sz -= ret;
                data += ret;
            } while (sz > 0);

            header = *((int *)(tcp_rx_buffer));
            header = ntohl(header);

            if (header == EL_LINK_IDENTIFIER) {
                el_dap_work(tcp_rx_buffer, sizeof(tcp_rx_buffer));
            } else if ((header & 0xFFFF) == 0x8003 ||
                       (header & 0xFFFF) == 0x8005) { // usbip OP_REQ_DEVLIST/OP_REQ_IMPORT
                if ((header & 0xFFFF) == 0x8005)
                    usbip_state = WAIT_DEVLIST;
                else
                    usbip_state = WAIT_IMPORT;
                usbip_worker(tcp_rx_buffer, sizeof(tcp_rx_buffer), &usbip_state);
            } else if (header == 0x47455420) { // string "GET "
#if CONFIG_BIRDIEBOT_DAP_WS_ENABLE
                websocket_worker(kSock, tcp_rx_buffer, sizeof(tcp_rx_buffer));
#else
                printf("WebSocket-DAP is disabled (CONFIG_BIRDIEBOT_DAP_WS_ENABLE=n)\r\n");
#endif
            } else {
                printf("Unknown protocol\n");
            }

cleanup:
            if (kSock != -1)
            {
                printf("Shutting down socket and restarting...\r\n");
                //shutdown(kSock, 0);
                close(kSock);
                kSock = -1;

                // Restart DAP Handle
                el_process_buffer_free();

                /* The DAP task owns the ring buffer lifecycle; this task only
                 * posts the request (see DAP_handle.h). */
                dap_handle_request_reset(RESET_HANDLE);
                /*
                 * C1 hardening: normally the DAP task (prio 16 on the same
                 * core) preempts us inside dap_handle_request_reset()'s
                 * xTaskNotifyGive(), so the reset -- a cheap drain, no
                 * allocation since task-66 -- is applied before we return to
                 * accept().  If the DAP task is still busy with a long DAP
                 * command, a new USBIP session could otherwise queue commands
                 * that the pending drain then discards, leaving the new
                 * session's bulk-IN URB unanswered ("CMSIS-DAP command
                 * CMD_INFO failed").  Wait (bounded) for the drain to be
                 * applied; continue regardless on timeout.
                 */
                if (dap_handle_wait_reset(DAP_CLEANUP_RESET_WAIT_MS) != 0) {
                    printf("tcp_server: DAP session reset not applied within %u ms\r\n",
                           (unsigned)DAP_CLEANUP_RESET_WAIT_MS);
                }

                //shutdown(listen_sock, 0);
                //close(listen_sock);
                //vTaskDelay(5);
            }
        }
    }
    vTaskDelete(NULL);
}
