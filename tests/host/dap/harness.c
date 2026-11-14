/*
 * PROVENANCE (WP-302 host regression, 2026-10-20)
 * --------------------------------------------------------------------------
 * Integrated from the dap-dev ASan host harness at /tmp/dap_host (stub
 * DAP_config.h + FreeRTOS shims, verified 36/36 at the time, 51/51 after the
 * batch-path cases were added; pre-fix DAP.c reproduces N1 as ASan
 * stack-overflow and N9 as ASan heap-buffer-overflow READ of size 1).
 * Source review: docs/security-review-dap.md (N1 P0 / N6 P2 / N9 P2/P3);
 * fixes: commit 666df38 (N1/N9/N6) merged by 89bfd0d on feature/birdiebot-v1.
 * This copy keeps the test code unchanged on purpose; only this provenance
 * header and the build paths in the instructions below were adapted.
 * It is compiled against the *current* repo DAP.c, so it is a live regression.
 */

/*
 * Host regression harness for the DAP command layer fixes from the security
 * review (N1: 0x7E unbounded mutual recursion; N9: TransferBlock write
 * 1-byte OOB read; N6: SWJ_Pins wait unit conversion).
 *
 * It compiles components/DAP/cmsis-dap/source/DAP.c against a stub DAP_config.h
 * (no ESP32 hardware) with ASan/UBSan, so the packet-boundary logic can be
 * reproduced and regression-tested on a PC.
 *
 * Build/run (integrated location):
 *   tests/host/dap/run.sh            # ASan+UBSan, compiles the repo DAP.c
 *   SANITIZE=0 tests/host/dap/run.sh # plain build (no sanitizer runtime needed)
 * It is also driven by scripts/test_host.sh as the "DAP harness" suite.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DAP_config.h"
#include "cmsis-dap/include/DAP.h"

/* -------- hardware/side stubs normally provided by DAP_config.h or SW_DP.c -- */
static uint32_t g_timestamp = 0;
uint32_t TIMESTAMP_GET(void) { return g_timestamp++; }

uint8_t SWD_TransferSpeed = kTransfer_GPIO_normal;
uint8_t SWD_TransferAbort; /* not used, keep linker quiet if referenced */

void SWJ_Sequence(uint32_t count, const uint8_t *data) { (void)count; (void)data; }
void SWD_Sequence(uint32_t info, const uint8_t *swdo, uint8_t *swdi) { (void)info; (void)swdo; (void)swdi; }
void JTAG_Sequence(uint32_t info, const uint8_t *tdi, uint8_t *tdo) { (void)info; (void)tdi; (void)tdo; }
void JTAG_IR(uint32_t ir) { (void)ir; }
uint32_t JTAG_ReadIDCode(void) { return 0x2BA01477U; }
void JTAG_WriteAbort(uint32_t data) { (void)data; }
uint8_t JTAG_Transfer(uint32_t request, uint32_t *data) { (void)request; if (data) *data = 0U; return DAP_TRANSFER_OK; }
uint8_t SWD_Transfer(uint32_t request, uint32_t *data) { (void)request; if (data) *data = 0x12345678U; return DAP_TRANSFER_OK; }
void DAP_SPI_Init(void) { }
void DAP_SPI_Deinit(void) { }
uint32_t DAP_ProcessVendorCommand(const uint8_t *request, uint8_t *response)
{
    *response = *request; /* echo only */
    return ((1U << 16) | 1U);
}

/* ------------------------------------------------------------------ tests -- */
static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        g_fail++;                                                            \
    } else {                                                                 \
        g_pass++;                                                            \
    }                                                                        \
} while (0)

static uint16_t resp_len(uint32_t n) { return (uint16_t)(n & 0xFFFFU); }

/* N1: top-level 0x7E must execute the batch instead of recursing. */
static void test_n1_top_level_0x7e(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_QueueCommands;
    req[1] = 1U;                       /* one sub-command */
    req[2] = ID_DAP_Disconnect;        /* harmless, no target access */

    n = DAP_ProcessCommand(req, resp);
    printf("[N1] top-level 0x7E -> resp_len=%u echo=0x%02X count=%u status=0x%02X\n",
           resp_len(n), resp[0], resp[1], resp[2]);
    CHECK(resp[0] == ID_DAP_QueueCommands);  /* 0x7E echoed, not 0x7F */
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == ID_DAP_Disconnect);
    CHECK(resp[3] == DAP_OK);
    CHECK(resp_len(n) == 4U);

    free(req); free(resp);
}

/* N1: a 0x7E/0x7F sub-command inside a batch must be rejected, not dispatched
 * again (this was the unbounded recursion path). */
static void test_n1_nested_execute_rejected(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_ExecuteCommands;
    req[1] = 2U;
    req[2] = ID_DAP_QueueCommands;      /* nested 0x7E */
    req[3] = 0U;
    req[4] = ID_DAP_ExecuteCommands;    /* nested 0x7F */
    req[5] = 0U;

    n = DAP_ProcessCommand(req, resp);
    printf("[N1] nested 0x7E/0x7F -> resp_len=%u count=%u r2=0x%02X s2=0x%02X r3=0x%02X s3=0x%02X\n",
           resp_len(n), resp[1], resp[2], resp[3], resp[4], resp[5]);
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == 2U);               /* both sub-commands were processed */
    CHECK(resp[2] == ID_DAP_QueueCommands);
    CHECK(resp[3] == ID_DAP_Invalid);
    CHECK(resp[4] == ID_DAP_ExecuteCommands);
    CHECK(resp[5] == ID_DAP_Invalid);
    CHECK(resp_len(n) == 6U);

    free(req); free(resp);
}

/* N1: DAP_ExecuteCommand itself must accept a leading 0x7E (normalized alias). */
static void test_n1_execute_entry_0x7e(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_QueueCommands;
    req[1] = 1U;
    req[2] = ID_DAP_Disconnect;

    n = DAP_ExecuteCommand(req, resp);
    printf("[N1] ExecuteCommand(0x7E) -> resp_len=%u echo=0x%02X count=%u\n",
           resp_len(n), resp[0], resp[1]);
    CHECK(resp[0] == ID_DAP_QueueCommands);
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == ID_DAP_Disconnect);
    CHECK(resp[3] == DAP_OK);

    free(req); free(resp);
}

/* N9: a write TransferBlock with count=127 used to read 1 byte past a 512-byte
 * request packet (ASan: heap-buffer-overflow). It must now be rejected. */
static void test_n9_write_block_127_rejected(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);   /* exact size on purpose */
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_TransferBlock;
    req[1] = 0U;                 /* DAP index */
    req[2] = 127U;               /* request_count low */
    req[3] = 0U;                 /* request_count high */
    req[4] = 0x00U;              /* write request */

    n = DAP_ProcessCommand(req, resp);
    printf("[N9] write block count=127 -> resp_len=%u count=%u%u value=0x%02X\n",
           resp_len(n), resp[1], resp[2], resp[3]);
    CHECK(resp[0] == ID_DAP_TransferBlock);
    CHECK(resp[1] == 0U);        /* response count */
    CHECK(resp[2] == 0U);
    CHECK(resp[3] == DAP_ERROR); /* rejected before touching the packet tail */
    CHECK(resp_len(n) == 4U);

    free(req); free(resp);
}

/* N9: read direction with count=127 fits the response exactly (4 + 4*127 = 512). */
static void test_n9_read_block_127_accepted(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_TransferBlock;
    req[1] = 0U;
    req[2] = 127U;
    req[3] = 0U;
    req[4] = DAP_TRANSFER_RnW;   /* read block */
    DAP_Data.debug_port = DAP_PORT_SWD;

    n = DAP_ProcessCommand(req, resp);
    printf("[N9] read block count=127 -> resp_len=%u count=%u%u r=%02X %02X %02X %02X %02X dbg(read=%d)\n",
           resp_len(n), resp[1], resp[2], resp[0], resp[1], resp[2], resp[3], resp[4],
           (req[4] & DAP_TRANSFER_RnW) != 0U);
    CHECK(resp[0] == ID_DAP_TransferBlock);
    CHECK((uint16_t)(resp[1] | (resp[2] << 8)) == 127U); /* response count */
    CHECK(resp_len(n) <= DAP_PACKET_SIZE);
    CHECK(resp_len(n) == 512U);  /* echo + handler(3 + 4*127) = exactly one packet */

    free(req); free(resp);
}

/* N9: read count=128 must be rejected (response would not fit one packet). */
static void test_n9_read_block_128_rejected(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_TransferBlock;
    req[1] = 0U;
    req[2] = 128U;
    req[3] = 0U;
    req[4] = DAP_TRANSFER_RnW;

    n = DAP_ProcessCommand(req, resp);
    printf("[N9] read block count=128 -> value=0x%02X resp_len=%u\n", resp[3], resp_len(n));
    CHECK(resp[3] == DAP_ERROR);
    CHECK(resp_len(n) == 4U);

    free(req); free(resp);
}

/* N9: the write direction boundary count=126 must be accepted (5 + 4*126 = 509
 * bytes fit into the 512-byte packet; count=127 does not and is rejected). */
static void test_n9_write_block_126_accepted(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_TransferBlock;
    req[1] = 0U;
    req[2] = 126U;
    req[3] = 0U;
    req[4] = 0x00U;                 /* write block */
    DAP_Data.debug_port = DAP_PORT_SWD;
    for (int i = 0; i < 126 * 4; i++) {
        req[5 + i] = (uint8_t)i;    /* payload stays inside the 512-byte packet */
    }

    n = DAP_ProcessCommand(req, resp);
    printf("[N9] write block count=126 -> resp_len=%u count=%u%u value=0x%02X\n",
           resp_len(n), resp[1], resp[2], resp[3]);
    CHECK(resp[0] == ID_DAP_TransferBlock);
    CHECK(resp[1] == 126U);         /* handler reported 126 completed writes */
    CHECK(resp[2] == 0U);           /* count high byte */
    CHECK(resp[3] == DAP_TRANSFER_OK);
    CHECK(resp_len(n) == 4U);       /* echo + 3-byte response header */

    free(req); free(resp);
}


/* N9 via the ExecuteCommands(0x7F) batch path: the direction-aware cap must
 * also apply to a TransferBlock that arrives as a sub-command. */
static void test_n9_transferblock_in_0x7f_batch(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    /* batch with write block count=127 -> must be rejected by the DAP layer */
    req[0] = ID_DAP_ExecuteCommands;
    req[1] = 1U;
    req[2] = ID_DAP_TransferBlock;
    req[3] = 0U;     /* index */
    req[4] = 127U;   /* count low */
    req[5] = 0U;     /* count high */
    req[6] = 0x00U;  /* write */
    DAP_Data.debug_port = DAP_PORT_SWD;
    n = DAP_ProcessCommand(req, resp);
    printf("[N9] 0x7F batch write count=127 -> count=%u r2=%02X r3=%02X r4=%02X resp_len=%u\n",
           resp[1], resp[2], resp[3], resp[4], resp_len(n));
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == ID_DAP_TransferBlock);
    CHECK(resp[3] == 0U && resp[4] == 0U);
    CHECK(resp[5] == DAP_ERROR);
    CHECK(resp_len(n) <= DAP_PACKET_SIZE);

    /* batch with read block count=126 -> fits after the 2-byte batch header */
    memset(req, 0, DAP_PACKET_SIZE);
    req[0] = ID_DAP_ExecuteCommands;
    req[1] = 1U;
    req[2] = ID_DAP_TransferBlock;
    req[3] = 0U;
    req[4] = 126U;
    req[5] = 0U;
    req[6] = DAP_TRANSFER_RnW;
    n = DAP_ProcessCommand(req, resp);
    printf("[N9] 0x7F batch read count=126 -> count=%u r2=%02X r3=%02X resp_len=%u\n",
           resp[2] | (resp[3] << 8), resp[2], resp[3], resp_len(n));
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == ID_DAP_TransferBlock);
    CHECK((uint16_t)(resp[3] | (resp[4] << 8)) == 126U); /* handler count */
    CHECK(resp_len(n) == 510U);  /* 2 batch + echo + handler(3 + 4*126) = 510 */

    free(req); free(resp);
}

/* N9 via the QueueCommands(0x7E) top-level alias: sub-command cap still applies. */
static void test_n9_transferblock_in_0x7e_alias(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    req[0] = ID_DAP_QueueCommands;  /* 0x7E alias of 0x7F */
    req[1] = 1U;
    req[2] = ID_DAP_TransferBlock;
    req[3] = 0U;
    req[4] = 127U;
    req[5] = 0U;
    req[6] = 0x00U;                 /* write, count=127 -> rejected */
    n = DAP_ProcessCommand(req, resp);
    printf("[N9] 0x7E alias write count=127 -> count=%u r2=%02X r3=%02X r4=%02X\n",
           resp[1], resp[2], resp[3], resp[4]);
    CHECK(resp[0] == ID_DAP_QueueCommands);
    CHECK(resp[1] == 1U);
    CHECK(resp[2] == ID_DAP_TransferBlock);
    CHECK(resp[5] == DAP_ERROR);

    free(req); free(resp);
}

/* N6: SWJ_Pins "Pin Wait" is microseconds (CMSIS-DAP spec; max 3,000,000 us).
 * Select nRESET expecting high while PIN_nRESET_IN() returns 0, so the handler
 * polls until the timeout elapses; TIMESTAMP_GET() counts 1 us per call. */
static void test_n6_swj_pins_wait_microseconds(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t wait_us;
    uint32_t before;

    req[0] = ID_DAP_SWJ_Pins;
    req[1] = 0x80U;              /* output value: nRESET high */
    req[2] = 0x80U;              /* selected pin: nRESET (PIN_nRESET_IN()==0) */
    wait_us = 100U;              /* 100 us */
    req[3] = (uint8_t)(wait_us >> 0);
    req[4] = (uint8_t)(wait_us >> 8);
    req[5] = (uint8_t)(wait_us >> 16);
    req[6] = (uint8_t)(wait_us >> 24);

    g_timestamp = 0;
    before = g_timestamp;
    (void)DAP_ProcessCommand(req, resp);
    printf("[N6] SWJ_Pins wait=100us -> TIMESTAMP_GET calls=%u (expected ~100)\n",
           g_timestamp - before);
    CHECK((g_timestamp - before) >= 95U);
    CHECK((g_timestamp - before) <= 120U);

    /* The 3 s protocol clamp must still hold. */
    wait_us = 0xFFFFFFFFU;
    req[3] = (uint8_t)(wait_us >> 0);
    req[4] = (uint8_t)(wait_us >> 8);
    req[5] = (uint8_t)(wait_us >> 16);
    req[6] = (uint8_t)(wait_us >> 24);
    g_timestamp = 0;
    (void)DAP_ProcessCommand(req, resp);
    printf("[N6] SWJ_Pins wait=0xFFFFFFFF -> ticks=%u (expected ~3000000)\n", g_timestamp);
    CHECK(g_timestamp >= 3000000U);
    CHECK(g_timestamp <= 3000010U);

    free(req); free(resp);
}


/* W5 batch-packing fix: DAP_ResponseMaxSize(ID_DAP_Transfer) must use the exact
 * response size (3 + 4/read item, not 3 + 8/item).  With the exact estimate a
 * 0x7F batch of write transfers fits 63 sub-commands in a 512-byte packet;
 * before the fix the guard stopped at ~45 (each write was counted as 11 bytes
 * instead of 4), so OpenOCD's queued mode needed extra re-sends. */
static void test_w5_batch_exact_write_packing(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;
    const uint32_t subs = 63U; /* 2 + 63*8 = 506 bytes, fits one packet */
    uint8_t *p = req;

    DAP_Data.debug_port = DAP_PORT_SWD;
    *p++ = ID_DAP_ExecuteCommands;
    *p++ = (uint8_t)subs;
    for (uint32_t i = 0; i < subs; i++) {
        *p++ = ID_DAP_Transfer; /* 0x01 */
        *p++ = 0U;              /* DAP index */
        *p++ = 1U;              /* count = 1 */
        *p++ = 0x00U;           /* write DP register */
        *p++ = 0U; *p++ = 0U; *p++ = 0U; *p++ = 0U; /* data */
    }

    n = DAP_ProcessCommand(req, resp);
    printf("[W5] 0x7F batch of %u write transfers -> processed=%u resp_len=%u\n",
           (unsigned)subs, resp[1], resp_len(n));
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == (uint8_t)subs);   /* exact estimate: all fit */
    CHECK(resp_len(n) == (uint16_t)(2U + subs * 3U)); /* batch hdr + echo+count+status per write sub */
    CHECK(resp_len(n) <= DAP_PACKET_SIZE);

    free(req); free(resp);
}

/* W5 batch-packing fix: exact read-item sizing (response = 3 + 4*read items).
 * 73 single-read DAP_Transfer sub-commands: the 73rd would make the batch
 * response 513 bytes, so it must stop at 72 (old estimate stopped at ~46). */
static void test_w5_batch_exact_read_packing(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;
    const uint32_t subs = 73U; /* 2 + 73*4 = 294 bytes request */
    uint8_t *p = req;

    DAP_Data.debug_port = DAP_PORT_SWD;
    *p++ = ID_DAP_ExecuteCommands;
    *p++ = (uint8_t)subs;
    for (uint32_t i = 0; i < subs; i++) {
        *p++ = ID_DAP_Transfer;          /* 0x01 */
        *p++ = 0U;                       /* DAP index */
        *p++ = 1U;                       /* count = 1 */
        *p++ = DAP_TRANSFER_RnW;         /* read DP register */
    }

    n = DAP_ProcessCommand(req, resp);
    printf("[W5] 0x7F batch of %u read transfers -> processed=%u resp_len=%u\n",
           (unsigned)subs, resp[1], resp_len(n));
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == 72U);   /* 2 + 72*7 = 506; the 73rd would exceed 512 */
    CHECK(resp_len(n) == 506U);
    CHECK(resp_len(n) <= DAP_PACKET_SIZE);

    free(req); free(resp);
}

/* W5 batch-packing fix: a batched read TransferBlock count=127 has a 512-byte
 * response; with the old estimator (which read the wrong offsets and treated
 * the block as a write, 3 bytes) it was admitted and overflowed the 512-byte
 * batch response by 2 bytes (ASan).  It must be rejected before execution. */
static void test_w5_batch_read_block_overflow_rejected(void)
{
    uint8_t *req = calloc(1, DAP_PACKET_SIZE);
    uint8_t *resp = calloc(1, DAP_PACKET_SIZE);
    uint32_t n;

    DAP_Data.debug_port = DAP_PORT_SWD;
    req[0] = ID_DAP_ExecuteCommands;
    req[1] = 1U;
    req[2] = ID_DAP_TransferBlock; /* 0x06 */
    req[3] = 0U;                   /* DAP index */
    req[4] = 127U;                 /* count low */
    req[5] = 0U;                   /* count high */
    req[6] = DAP_TRANSFER_RnW;     /* read */

    n = DAP_ProcessCommand(req, resp);
    printf("[W5] 0x7F batch read block count=127 -> processed=%u resp_len=%u\n",
           resp[1], resp_len(n));
    CHECK(resp[0] == ID_DAP_ExecuteCommands);
    CHECK(resp[1] == 0U);          /* rejected before touching the response tail */
    CHECK(resp_len(n) == 2U);

    free(req); free(resp);
}

/* CMSIS-DAP v2.1 DAP_Info(CAPABILITIES) length contract:
 * when DAP_UART is not supported the response is one data byte; the second
 * (UART capabilities) byte must not be sent.  Stock OpenOCD 0.11 treats a
 * two-byte response as malformed ("SWD not supported"). */
static void test_info_capabilities_one_byte(void)
{
    uint8_t req[2] = {ID_DAP_Info, DAP_ID_CAPABILITIES};
    uint8_t resp[8] = {0};
    uint32_t n;

    n = DAP_ProcessCommand(req, resp);
    printf("[INFO] capabilities -> len=%u caps=0x%02X\n",
           (unsigned)resp[1], (unsigned)resp[2]);

    CHECK(resp[0] == ID_DAP_Info);
    CHECK(resp[1] == 1U); /* one-byte capabilities when DAP_UART == 0 */
    CHECK(resp[2] == (uint8_t)((1U << 0) | (1U << 1) | (1U << 4) | (1U << 5)));
    CHECK((resp[2] & (1U << 7)) == 0U); /* UART capability bit must be clear */
    CHECK(resp_len(n) == 3U);           /* echo + length + one data byte */
}

int main(void)
{
    DAP_Setup();
    DAP_Data.debug_port = DAP_PORT_SWD; /* transfer tests need an active port */

    printf("== DAP command-layer security regression (host, ASan/UBSan) ==\n");
    printf("   [cfg] DAP_SWD=%d DAP_JTAG=%d debug_port=%u packet=%u\n", DAP_SWD, DAP_JTAG, (unsigned)DAP_Data.debug_port, (unsigned)DAP_PACKET_SIZE);
#ifndef REPRO_N9_ONLY
    test_n1_top_level_0x7e();
    test_n1_nested_execute_rejected();
    test_n1_execute_entry_0x7e();
#endif
    test_n9_write_block_127_rejected();
    test_n9_read_block_127_accepted();
    test_n9_read_block_128_rejected();
    test_n9_write_block_126_accepted();
    test_n9_transferblock_in_0x7f_batch();
    test_n9_transferblock_in_0x7e_alias();
    test_n6_swj_pins_wait_microseconds();
    test_w5_batch_exact_write_packing();
    test_w5_batch_exact_read_packing();
    test_w5_batch_read_block_overflow_rejected();
    test_info_capabilities_one_byte();

    printf("== %d checks passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
