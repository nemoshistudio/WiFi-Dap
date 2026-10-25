/*
 * SPDX-FileCopyrightText: 2024 kerms
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ota_upload_diag.h
 * @brief Read-only diagnostics for POST /api/ota/upload (module 5 upload).
 *
 * The unified S3 Wi-Fi+NCM image has no USB-Serial/JTAG console (the USB PHY
 * is used by TinyUSB NCM), so the upload transport keeps a snapshot of the
 * last attempt (stage, body accounting, chosen worker stack, heap at worker
 * creation, stack high-water mark, last transport failure) and
 * GET /api/target/status exposes it.  This is diagnostic state only: it never
 * changes the OTA protocol, the module-5 JSON contract or the state machine
 * in components/wt_ota.
 *
 * Why it exists (real-device finding, 2026-10-25): ESP-IDF v5.5.6's
 * httpd_req_async_handler_begin() zeroes req->aux->remaining_len, so the body
 * MUST be received before the async request copy is created.  The stage +
 * body_received/body_expected + last_error fields make that ordering visible
 * on the device without a serial console.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Numeric JSON "ota_upload_stage".  Append new stages; never renumber. */
typedef enum {
	OTA_UPLOAD_STAGE_IDLE = 0,         /* no upload attempted since boot */
	OTA_UPLOAD_STAGE_CLAIMED = 1,      /* request validated, session claimed */
	OTA_UPLOAD_STAGE_TASK_CREATED = 2, /* OTA worker created (see task_stack) */
	OTA_UPLOAD_STAGE_BODY_DONE = 3,    /* body receive loop finished */
	OTA_UPLOAD_STAGE_ASYNC_BEGIN = 4,  /* async request copy created */
	OTA_UPLOAD_STAGE_EOS = 5,          /* EOS marker queued, handler returning */
	OTA_UPLOAD_STAGE_WORKER_RUNNING = 6, /* worker started the flash session */
	OTA_UPLOAD_STAGE_DONE = 7,         /* worker finished (response sent/aborted) */
} ota_upload_stage_t;

typedef struct {
	bool active;             /*!< session claimed and worker not finished yet */
	uint8_t stage;           /*!< ota_upload_stage_t of the last attempt */
	uint32_t task_stack;     /*!< worker stack bytes used (0 = not created) */
	uint32_t heap_free;      /*!< internal 8-bit free heap at worker creation */
	uint32_t heap_largest;   /*!< internal 8-bit largest block at worker creation */
	uint32_t body_expected;  /*!< Content-Length of the last attempt */
	uint32_t body_received;  /*!< body bytes received in the last attempt */
	uint32_t stack_free_min; /*!< worker stack high-water mark (bytes free) */
	char last_error[80];     /*!< last transport failure ("" = none) */
} ota_upload_diag_t;

/** Snapshot the OTA upload diagnostics (never blocks, never allocates). */
void ota_upload_get_diag(ota_upload_diag_t *out);
