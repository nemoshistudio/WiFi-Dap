#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-release-versions.sh -- cross-check the three release version numbers
# (firmware tag, Web UI package version, protocol integer) and, optionally, the
# version embedded in a built firmware image.
#
# Contract: docs/versioning.md §8.3 (release check) and §9 (release checklist).
#
# Usage:
#   scripts/check-release-versions.sh                    # strict release gate
#   scripts/check-release-versions.sh --bin build/esp32s3/birdiebot_wifi_dap.bin
#   scripts/check-release-versions.sh --allow-dev        # local/dev build gate
#
# Strict mode requires:
#   * HEAD exactly on an annotated tag vX.Y.Z (`git describe --exact-match`)
#   * web/package.json version == X.Y.Z
#   * the firmware version string is valid and not -dirty
#   * scripts/check-protocol-version.sh passes (if that script exists)
#   * --bin image (when given) embeds exactly vX.Y.Z as esp_app_desc.version
#
# --allow-dev relaxes the tag and fw==web assertions (records a WARNING) so the
# same gate can run on development branches without a tag.
#
# This script only reads files and git metadata; it never pushes, builds or
# flashes.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"

ALLOW_DEV=0
BIN=""

usage() {
    cat <<'EOF'
Usage: check-release-versions.sh [options]

  --allow-dev     relax the exact-tag and fw==web checks (development gate)
  --bin PATH      also verify the esp_app_desc.version embedded in PATH
  -h, --help      show this help
EOF
}

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}

warn() {
    printf '%s: warning: %s\n' "$SCRIPT_NAME" "$*" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --allow-dev)
            ALLOW_DEV=1
            shift
            ;;
        --bin)
            [ "$#" -ge 2 ] || die "--bin requires a path"
            BIN="$2"
            shift 2
            ;;
        --bin=*)
            BIN="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument '$1' (try '$SCRIPT_NAME --help')"
            ;;
    esac
done

command -v git >/dev/null 2>&1 || die "git is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"
[ -f "$ROOT/web/package.json" ] || die "missing $ROOT/web/package.json"

# --- firmware version ------------------------------------------------------
fw_version=""
exact_tag=""
if exact_tag="$(git -C "$ROOT" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null)"; then
    fw_version="$exact_tag"
else
    exact_tag=""
    if [ "$ALLOW_DEV" -eq 1 ]; then
        _dev="$(git -C "$ROOT" describe --tags --dirty --always --match 'v[0-9]*' 2>/dev/null || true)"
        if [[ "$_dev" =~ ^v[0-9]+\.[0-9]+\.[0-9]+ ]]; then
            fw_version="$_dev"
        elif [ -f "$ROOT/version.txt" ]; then
            fw_version="$(tr -d '[:space:]' <"$ROOT/version.txt")"
        else
            fw_version="v1.0.0"
        fi
        warn "not on an exact vX.Y.Z tag; development version '$fw_version' (allow-dev)"
    else
        die "HEAD is not exactly on a vMAJOR.MINOR.PATCH tag (git describe --exact-match failed); release requires a tag or --allow-dev"
    fi
fi

if [ "$ALLOW_DEV" -eq 1 ]; then
    "$SCRIPT_DIR/check-version-string.sh" --allow-dirty "$fw_version" >/dev/null \
        || die "invalid firmware version '$fw_version'"
else
    "$SCRIPT_DIR/check-version-string.sh" "$fw_version" >/dev/null \
        || die "invalid firmware version '$fw_version'"
fi

# --- web UI version --------------------------------------------------------
web_version="$(python3 - "$ROOT/web/package.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)
print(str(data.get("version", "")).strip())
PY
)"
web_re='^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$'
[[ "$web_version" =~ $web_re ]] || die "invalid web/package.json version '$web_version'"
if [ "v$web_version" = "$fw_version" ]; then
    : # consistent
elif [ "$ALLOW_DEV" -eq 1 ]; then
    warn "firmware $fw_version != web/package.json version v$web_version (allowed with --allow-dev)"
else
    die "firmware version '$fw_version' != web/package.json version 'v$web_version' (docs/versioning.md §4.1)"
fi

# --- protocol version ------------------------------------------------------
protocol_num="?"
proto_header="$ROOT/project_components/wt_common/wt_protocol_version.h"
if [ -f "$proto_header" ]; then
    protocol_num="$(grep -oE 'BIRDIEBOT_PROTOCOL_VERSION[[:space:]]+[0-9]+' "$proto_header" \
        | head -n1 | grep -oE '[0-9]+$' || true)"
    [ -n "$protocol_num" ] || protocol_num="?"
fi
if [ -x "$SCRIPT_DIR/check-protocol-version.sh" ]; then
    "$SCRIPT_DIR/check-protocol-version.sh"
elif [ -f "$SCRIPT_DIR/check-protocol-version.sh" ]; then
    bash "$SCRIPT_DIR/check-protocol-version.sh"
else
    warn "scripts/check-protocol-version.sh not present yet (WP-131); C header <-> TS constant check skipped"
fi

# --- embedded firmware version (optional) ----------------------------------
bin_note="not checked"
if [ -n "$BIN" ]; then
    case "$BIN" in
        /*) BIN_ABS="$BIN" ;;
        *) BIN_ABS="$ROOT/$BIN" ;;
    esac
    [ -f "$BIN_ABS" ] || die "app image not found: $BIN_ABS"
    embedded="$(python3 - "$BIN_ABS" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if len(data) < 0x50 or data[0] != 0xE9:
    sys.exit("not an ESP-IDF app image")
if struct.unpack_from("<I", data, 0x20)[0] != 0xABCD5432:
    sys.exit("esp_app_desc magic_word mismatch")
print(struct.unpack_from("<32s", data, 0x30)[0].split(b"\0", 1)[0].decode("ascii", "replace"))
PY
)" || die "cannot read esp_app_desc.version from $BIN_ABS"
    [ -n "$embedded" ] || die "$BIN_ABS has an EMPTY esp_app_desc.version (PROJECT_VER not injected)"
    if [ "$embedded" = "$fw_version" ]; then
        bin_note="embedded=$embedded ok"
    elif [ "$ALLOW_DEV" -eq 1 ]; then
        warn "embedded version '$embedded' != resolved '$fw_version' (allowed with --allow-dev)"
        bin_note="embedded=$embedded (dev mismatch)"
    else
        die "embedded esp_app_desc.version '$embedded' != release tag '$fw_version'"
    fi
fi

printf 'release versions ok: fw=%s web=%s protocol=%s%s%s\n' \
    "$fw_version" "$web_version" "$protocol_num" \
    "$([ "$ALLOW_DEV" -eq 1 ] && echo ' [dev mode]' || true)" \
    "$([ -n "$BIN" ] && echo " bin: $bin_note" || true)"
