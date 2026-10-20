/*
 * SPDX-FileCopyrightText: 2024 kerms <kerms@niazo.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wt_protocol_version.h
 * @brief WiFi Dap JSON/WS protocol version - single source of truth.
 *
 * Contract: docs/versioning.md §3.1 (frozen v1, 2026-10-20).
 *
 * Rules:
 *  - This constant is the ONLY place where the wire protocol version lives on
 *    the firmware side; the Web UI keeps the mirrored constant
 *    `web/src/api/protocol.ts:PROTOCOL_VERSION` and build scripts compare both.
 *  - Bump it (+1) ONLY for breaking wire changes; see docs/versioning.md §3.3.
 *  - Exposed to clients via SYSTEM/GET_FM_INFO.protocol_version (WP-125).
 *  - Other modules must not report the protocol version on their own.
 *
 * The value is intentionally a plain macro so it can be used from C and from
 * CMake/build script greps (`scripts/check-protocol-version.sh`).
 */
#ifndef WT_PROTOCOL_VERSION_H_GUARD
#define WT_PROTOCOL_VERSION_H_GUARD

#define BIRDIEBOT_PROTOCOL_VERSION 1

#endif /* WT_PROTOCOL_VERSION_H_GUARD */
