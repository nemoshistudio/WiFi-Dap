#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-version-string.sh -- validate a WiFi Dap firmware version string.
#
# Contract: docs/versioning.md §2.1 (version rules) and §8.1 (this checker).
#
# Accepted release form : vMAJOR.MINOR.PATCH
# Accepted dev form     : vMAJOR.MINOR.PATCH[-N-g<sha7+>][-dirty]
# A release build must be exactly vMAJOR.MINOR.PATCH on the release tag; the
# release scripts call this with --release (default) and a `-dirty` suffix is
# rejected. Use --allow-dirty for local/dev builds.
#
# Usage:
#   scripts/check-version-string.sh v1.0.0                 # release check
#   scripts/check-version-string.sh --allow-dirty v1.0.0-3-gabc1234-dirty
#
# Exit: 0 valid, 1 invalid, 2 usage error.
set -euo pipefail

SCRIPT_NAME="${0##*/}"
ALLOW_DIRTY=0
VERSION=""

usage() {
    cat <<'EOF'
Usage: check-version-string.sh [--allow-dirty] [--dev] <version>

  --allow-dirty   accept a -dirty suffix (local development builds)
  --dev           alias for --allow-dirty; also accepts tag+commit dev strings
  -h, --help      show this help

Release builds must match: ^v[0-9]+\.[0-9]+\.[0-9]+$
Dev builds may match:     ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?(-dirty)?$
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --allow-dirty|--dev)
            ALLOW_DIRTY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            printf '%s: error: unknown option %s\n' "$SCRIPT_NAME" "$1" >&2
            exit 2
            ;;
        *)
            [ -z "$VERSION" ] || { printf '%s: error: more than one version given\n' "$SCRIPT_NAME" >&2; exit 2; }
            VERSION="$1"
            shift
            ;;
    esac
done

[ -n "$VERSION" ] || { printf '%s: error: missing version argument\n' "$SCRIPT_NAME" >&2; usage >&2; exit 2; }

dev_re='^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?(-dirty)?$'
release_re='^v[0-9]+\.[0-9]+\.[0-9]+$'

if [[ ! "$VERSION" =~ $dev_re ]]; then
    printf '%s: bad version: %s (expected vMAJOR.MINOR.PATCH[-N-g<sha>][-dirty])\n' "$SCRIPT_NAME" "$VERSION" >&2
    exit 1
fi

if [[ "$VERSION" == *-dirty ]]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        printf 'version ok (dev, dirty allowed): %s\n' "$VERSION"
        exit 0
    fi
    printf '%s: release version must not be dirty: %s\n' "$SCRIPT_NAME" "$VERSION" >&2
    exit 1
fi

if [ "$ALLOW_DIRTY" -eq 0 ] && [[ ! "$VERSION" =~ $release_re ]]; then
    printf '%s: release version must be an exact tag (vMAJOR.MINOR.PATCH), got: %s\n' "$SCRIPT_NAME" "$VERSION" >&2
    exit 1
fi

printf 'version ok: %s\n' "$VERSION"
