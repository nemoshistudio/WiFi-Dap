#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-protocol-version.sh -- verify the single protocol version number is the
# same in all three sources required by docs/versioning.md §3.1/§8.2:
#
#   1. firmware  : project_components/wt_common/wt_protocol_version.h
#                  -> #define BIRDIEBOT_PROTOCOL_VERSION <N>
#   2. contract  : docs/protocol.md
#                  -> `protocol_version = <N>`
#   3. Web UI    : web/src/api/index.ts
#                  -> export const PROTOCOL_VERSION = <N>
#
# (docs/versioning.md §8.2 still cites the old path web/src/api/protocol.ts;
# the merged Web UI keeps the constant in web/src/api/index.ts - docs are being
# synced by the Lead/doc role, this script follows the code.)
#
# Any one-sided protocol bump must fail the build: docs/versioning.md §8.2
# "任何一端升版但另一端未改 → 构建失败".
#
# Usage:
#   scripts/check-protocol-version.sh
#
# Exit codes: 0 = all three sources agree; 1 = mismatch or missing/unparsable
# source; 2 = usage error (unexpected arguments).
#
# Wired in as a pre-flight by scripts/build_all.sh (fatal) and
# scripts/release_local.sh (fatal in release mode, warning with --allow-dirty).
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"

readonly C_HEADER="$ROOT/project_components/wt_common/wt_protocol_version.h"
readonly DOC="$ROOT/docs/protocol.md"
readonly TS_FILE="$ROOT/web/src/api/index.ts"

die_usage() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    printf 'usage: %s   (no arguments)\n' "$SCRIPT_NAME" >&2
    exit 2
}

die_check() {
    printf '%s: FAIL: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}

[ "$#" -eq 0 ] || die_usage "unexpected argument(s): $*"

# --- firmware C header -----------------------------------------------------
c_val=""
if [ ! -f "$C_HEADER" ]; then
    die_check "missing firmware header: ${C_HEADER#"$ROOT"/}"
fi
c_val="$(grep -oE '^[[:space:]]*#define[[:space:]]+BIRDIEBOT_PROTOCOL_VERSION[[:space:]]+[0-9]+' "$C_HEADER" \
    | head -n1 | grep -oE '[0-9]+$' || true)"
[ -n "$c_val" ] || die_check "cannot parse BIRDIEBOT_PROTOCOL_VERSION from ${C_HEADER#"$ROOT"/}"

# --- docs contract ---------------------------------------------------------
if [ ! -f "$DOC" ]; then
    die_check "missing contract document: ${DOC#"$ROOT"/}"
fi
mapfile -t doc_vals < <(grep -oE 'protocol_version[[:space:]]*=[[:space:]]*[0-9]+' "$DOC" \
    | grep -oE '[0-9]+$' | sort -u)
[ "${#doc_vals[@]}" -gt 0 ] || die_check "cannot find 'protocol_version = <N>' in ${DOC#"$ROOT"/}"
if [ "${#doc_vals[@]}" -gt 1 ]; then
    die_check "inconsistent 'protocol_version = ...' values in ${DOC#"$ROOT"/}: ${doc_vals[*]}"
fi
doc_val="${doc_vals[0]}"

# --- Web UI TypeScript -----------------------------------------------------
if [ ! -f "$TS_FILE" ]; then
    die_check "missing Web UI protocol constant: ${TS_FILE#"$ROOT"/}"
fi
ts_val="$(grep -oE '^[[:space:]]*export[[:space:]]+const[[:space:]]+PROTOCOL_VERSION[[:space:]]*=[[:space:]]*[0-9]+' "$TS_FILE" \
    | head -n1 | grep -oE '[0-9]+$' || true)"
[ -n "$ts_val" ] || die_check "cannot parse PROTOCOL_VERSION from ${TS_FILE#"$ROOT"/}"

# --- compare ---------------------------------------------------------------
c_desc="project_components/wt_common/wt_protocol_version.h"
doc_desc="docs/protocol.md"
ts_desc="web/src/api/index.ts"

printf 'protocol version sources:\n'
printf '  %-52s %s\n' "$c_desc" "$c_val"
printf '  %-52s %s\n' "$doc_desc" "$doc_val"
printf '  %-52s %s\n' "$ts_desc" "$ts_val"

if [ "$c_val" = "$doc_val" ] && [ "$c_val" = "$ts_val" ]; then
    printf 'protocol version ok: %s (firmware == docs == web)\n' "$c_val"
    exit 0
fi

printf '%s: FAIL: protocol version mismatch: firmware=%s docs=%s web=%s\n' \
    "$SCRIPT_NAME" "$c_val" "$doc_val" "$ts_val" >&2
printf '  bump all three sources in the same merge window (docs/versioning.md §3.3)\n' >&2
exit 1
