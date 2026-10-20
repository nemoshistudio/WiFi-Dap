#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# gen_web_assets.sh -- convert/verify the built Web UI assets that get embedded
# into the firmware, write reproducible gzip hashes + build metadata, and
# (only with an explicit --install) update project_components/html/.
#
# Inputs (produced by web/build.sh, WP-131):
#   web/dist-gz/   every dist file gzip -9 -n compressed, same relative path + .gz
#                  (preferred: byte-for-byte reproducible, hashes preserved)
#   web/dist/      raw Vite output (fallback: this script gzips it itself)
#
# Outputs:
#   <staging>/     default build/web-assets/ (gitignored; never silently
#                  overwrites the committed project_components/html assets)
#   project_components/html/   ONLY with --install (or --install-dir DIR)
#
# The script only transforms + hashes files; it never runs npm and never builds
# the Web UI (that is web/build.sh / WP-131). --check writes nothing.
#
# Rule (Lead/WP-122): imported gz assets are committed; regeneration/installation
# must be explicit (--install) so merging other domains never conflicts silently.
#
# Contract: docs/versioning.md §4.4 (embedded gz names + web_version.txt).
#
# Usage:
#   scripts/gen_web_assets.sh                    # stage build/web-assets, print hashes
#   scripts/gen_web_assets.sh --check            # verify installed html == web build
#   scripts/gen_web_assets.sh --install          # explicitly install staged assets
#   scripts/gen_web_assets.sh --src web/dist-gz --install
#   scripts/gen_web_assets.sh --dist web/dist --out /tmp/webtest
#
# Exit: 0 success/check ok, 1 failure, 2 usage error.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"

SRC=""
DIST=""
OUT="build/web-assets"
INSTALL=0
INSTALL_DIR="project_components/html"
CHECK=0
WEB_VERSION=""
PROTOCOL_VERSION=""
CROSS_CHECK=1
declare -a EXTRA_INPUTS=()

usage() {
    cat <<'EOF'
Usage: gen_web_assets.sh [options]

  --src DIR              pre-gzipped assets (default: <root>/web/dist-gz)
  --dist DIR             raw Vite output used as fallback / hash cross-check
                         (default: <root>/web/dist)
  --out DIR              staging output dir (default: <root>/build/web-assets)
  --install              copy staged assets into --install-dir (explicit opt-in;
                         default behaviour never touches the committed html)
  --install-dir DIR      install target (default: project_components/html)
  --check                verify that <install-dir> matches the current web build
                         (gz sha256 identical); writes nothing
  --web-version V        Web UI semver (default: build-info.json[.gz], then
                         web/package.json)
  --protocol-version N   protocol integer (default: wt_protocol_version.h,
                         then build-info, then 1)
  --gzip FILE            extra raw file (relative to --dist) to gzip+embed
  --no-cross-check       do not verify web/dist-gz against web/dist
  -h, --help             show this help

Embedded files (project_components/html/*.gz): index.html.gz and
ws.sharedworker.js.gz are mandatory; build-info.json.gz is included when
present. web_version.txt records the reproducible hashes (docs/versioning.md
§4.4). Unmanaged files already in --install-dir are left untouched.
EOF
}

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}
usage_die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 2
}
log() {
    printf '[gen_web_assets] %s\n' "$*"
}
warn() {
    printf '%s: warning: %s\n' "$SCRIPT_NAME" "$*" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --src|--dist-gz)
            [ "$#" -ge 2 ] || usage_die "$1 requires a directory"
            SRC="$2"
            shift 2
            ;;
        --src=*|--dist-gz=*)
            SRC="${1#*=}"
            shift
            ;;
        --dist)
            [ "$#" -ge 2 ] || usage_die "--dist requires a directory"
            DIST="$2"
            shift 2
            ;;
        --dist=*)
            DIST="${1#*=}"
            shift
            ;;
        --out)
            [ "$#" -ge 2 ] || usage_die "--out requires a directory"
            OUT="$2"
            shift 2
            ;;
        --out=*)
            OUT="${1#*=}"
            shift
            ;;
        --install)
            INSTALL=1
            shift
            ;;
        --install-dir)
            [ "$#" -ge 2 ] || usage_die "--install-dir requires a directory"
            INSTALL_DIR="$2"
            shift 2
            ;;
        --install-dir=*)
            INSTALL_DIR="${1#*=}"
            shift
            ;;
        --check)
            CHECK=1
            shift
            ;;
        --web-version)
            [ "$#" -ge 2 ] || usage_die "--web-version requires a value"
            WEB_VERSION="$2"
            shift 2
            ;;
        --web-version=*)
            WEB_VERSION="${1#*=}"
            shift
            ;;
        --protocol-version)
            [ "$#" -ge 2 ] || usage_die "--protocol-version requires a value"
            PROTOCOL_VERSION="$2"
            shift 2
            ;;
        --protocol-version=*)
            PROTOCOL_VERSION="${1#*=}"
            shift
            ;;
        --gzip)
            [ "$#" -ge 2 ] || usage_die "--gzip requires a file"
            EXTRA_INPUTS+=("$2")
            shift 2
            ;;
        --gzip=*)
            EXTRA_INPUTS+=("${1#*=}")
            shift
            ;;
        --no-cross-check)
            CROSS_CHECK=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage_die "unknown argument '$1'"
            ;;
    esac
done

command -v python3 >/dev/null 2>&1 || die "python3 is required"
command -v gzip >/dev/null 2>&1 || die "gzip is required"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"

abspath() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s\n' "$ROOT/$1" ;;
    esac
}
SRC_ABS="$(abspath "${SRC:-web/dist-gz}")"
DIST_ABS="$(abspath "${DIST:-web/dist}")"
OUT_ABS="$(abspath "$OUT")"
INSTALL_ABS="$(abspath "$INSTALL_DIR")"

src_mode=0
if [ -d "$SRC_ABS" ] && compgen -G "$SRC_ABS/*.gz" >/dev/null 2>&1; then
    src_mode=1
elif [ -n "${SRC:-}" ]; then
    die "--src $SRC_ABS contains no *.gz files (pre-gzipped input expected)"
elif [ -d "$DIST_ABS" ]; then
    src_mode=0
else
    die "no web build output found: tried $SRC_ABS and $DIST_ABS (run web/build.sh first)"
fi
if [ "$src_mode" -eq 1 ]; then
    log "input: pre-gzipped $SRC_ABS"
else
    log "input: raw $DIST_ABS (will gzip with -9 -n)"
fi

# --- web UI version --------------------------------------------------------
json_field() {
    # json_field <file> <key>: print a top-level string/number field (best effort).
    python3 - "$1" "$2" <<'PY' 2>/dev/null || true
import json, sys
try:
    data = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    sys.exit(0)
value = data.get(sys.argv[2])
if isinstance(value, (str, int)):
    print(str(value).strip())
PY
}

WEB_VERSION_FROM_INFO=""
PROTOCOL_FROM_INFO=""
read_build_info() {
    local file="$1"
    [ -f "$file" ] || return 0
    WEB_VERSION_FROM_INFO="$(json_field "$file" web_version)"
    [ -n "$WEB_VERSION_FROM_INFO" ] || WEB_VERSION_FROM_INFO="$(json_field "$file" webVersion)"
    PROTOCOL_FROM_INFO="$(json_field "$file" protocol_version)"
    if [ -n "$WEB_VERSION_FROM_INFO" ]; then
        log "build-info $file: web_version=$WEB_VERSION_FROM_INFO protocol=${PROTOCOL_FROM_INFO:-?}"
    fi
}
if [ "$src_mode" -eq 1 ] && [ -f "$SRC_ABS/build-info.json.gz" ]; then
    _tmp_info="$(mktemp)"
    if gzip -dc "$SRC_ABS/build-info.json.gz" >"$_tmp_info" 2>/dev/null; then
        read_build_info "$_tmp_info"
    fi
    rm -f "$_tmp_info"
fi
[ -f "$DIST_ABS/build-info.json" ] && read_build_info "$DIST_ABS/build-info.json"

if [ -z "$WEB_VERSION" ]; then
    WEB_VERSION="$WEB_VERSION_FROM_INFO"
fi
if [ -z "$WEB_VERSION" ] && [ -f "$ROOT/web/package.json" ]; then
    WEB_VERSION="$(json_field "$ROOT/web/package.json" version)"
fi
[ -n "$WEB_VERSION" ] || die "cannot determine Web UI version (pass --web-version)"
if ! [[ "$WEB_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
    die "invalid Web UI version '$WEB_VERSION' (docs/versioning.md App. A: X.Y.Z[-suffix])"
fi

# --- protocol version ------------------------------------------------------
if [ -z "$PROTOCOL_VERSION" ]; then
    PROTO_HEADER="$ROOT/project_components/wt_common/wt_protocol_version.h"
    if [ -f "$PROTO_HEADER" ]; then
        PROTOCOL_VERSION="$(grep -oE 'BIRDIEBOT_PROTOCOL_VERSION[[:space:]]+[0-9]+' "$PROTO_HEADER" \
            | head -n1 | grep -oE '[0-9]+$' || true)"
    fi
fi
if [ -z "$PROTOCOL_VERSION" ]; then
    PROTOCOL_VERSION="$PROTOCOL_FROM_INFO"
fi
if [ -z "$PROTOCOL_VERSION" ]; then
    PROTOCOL_VERSION=1
    warn "protocol version header not found and --protocol-version not given; assuming 1 (contract v1)"
fi
[[ "$PROTOCOL_VERSION" =~ ^[1-9][0-9]*$ ]] || die "invalid protocol version '$PROTOCOL_VERSION'"

# --- git provenance / build time ------------------------------------------
git_tag="v1.0.0-unknown"
git_commit="unknown"
if command -v git >/dev/null 2>&1; then
    _tag="$(git -C "$ROOT" describe --tags --dirty --always --match 'v[0-9]*' 2>/dev/null || true)"
    [[ "$_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+ ]] && git_tag="$_tag"
    _commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"
    [ -n "$_commit" ] && git_commit="$_commit"
fi
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    build_time="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u +%Y-%m-%dT%H:%M:%SZ)"
else
    build_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
fi

# --- staging directory -----------------------------------------------------
# Safety: never use the committed install dir as the staging dir without the
# explicit --install switch (staging clears managed *.gz there).
if [ "$OUT_ABS" = "$INSTALL_ABS" ] && [ "$INSTALL" -eq 0 ]; then
    die "--out equals --install-dir ($OUT_ABS); pass --install to modify the committed html assets explicitly"
fi
if [ "$CHECK" -eq 1 ] && [ "$INSTALL" -eq 0 ]; then
    STAGE="$(mktemp -d "${TMPDIR:-/tmp}/gen-web-assets.XXXXXX")"
    CLEAN_STAGE=1
else
    STAGE="$OUT_ABS"
    CLEAN_STAGE=0
    mkdir -p "$STAGE"
    rm -f "$STAGE"/*.gz "$STAGE/web_version.txt" 2>/dev/null || true
fi
cleanup() {
    if [ "$CLEAN_STAGE" -eq 1 ]; then rm -rf "$STAGE"; fi
}
trap cleanup EXIT

# gzip_plain <src> <dst>: deterministic gzip, prints sha256.
gzip_plain() {
    gzip -9 -n -c "$1" >"$2"
    gzip -t "$2" || die "gzip self-test failed for $2"
    sha256sum "$2" | awk '{print $1}'
}

# --- build the staged asset set -------------------------------------------
declare -a STAGED_NAMES=()
if [ "$src_mode" -eq 1 ]; then
    while IFS= read -r -d '' gz; do
        name="$(basename "$gz")"
        cp "$gz" "$STAGE/$name.tmp"
        mv "$STAGE/$name.tmp" "$STAGE/$name"
        STAGED_NAMES+=("$name")
    done < <(find "$SRC_ABS" -maxdepth 1 -type f -name '*.gz' -print0 | sort -z)
else
    declare -a PLAIN=("$DIST_ABS/index.html")
    [ -f "$DIST_ABS/ws.sharedworker.js" ] && PLAIN+=("$DIST_ABS/ws.sharedworker.js")
    [ -f "$DIST_ABS/build-info.json" ] && PLAIN+=("$DIST_ABS/build-info.json")
    for extra in "${EXTRA_INPUTS[@]:-}"; do
        [ -n "$extra" ] || continue
        PLAIN+=("$DIST_ABS/$extra")
    done
    for src_file in "${PLAIN[@]}"; do
        [ -f "$src_file" ] || die "missing web build input: $src_file"
        name="$(basename "$src_file").gz"
        gzip_plain "$src_file" "$STAGE/$name" >/dev/null
        STAGED_NAMES+=("$name")
    done
fi

for required in index.html.gz ws.sharedworker.js.gz; do
    printf '%s\n' "${STAGED_NAMES[@]}" | grep -qx "$required" \
        || die "staged asset set is missing $required (check web/build.sh output)"
done

# Cross-check dist-gz against dist (catches a stale gz directory).
if [ "$src_mode" -eq 1 ] && [ "$CROSS_CHECK" -eq 1 ] && [ -d "$DIST_ABS" ]; then
    cross_fail=0
    for name in "${STAGED_NAMES[@]}"; do
        plain="$DIST_ABS/${name%.gz}"
        [ -f "$plain" ] || continue
        want="$(sha256sum "$STAGE/$name" | awk '{print $1}')"
        _tmp="$(mktemp)"
        gzip -9 -n -c "$plain" >"$_tmp"
        got="$(sha256sum "$_tmp" | awk '{print $1}')"
        rm -f "$_tmp"
        if [ "$want" != "$got" ]; then
            printf '%s: ERROR: %s is stale vs %s (sha256 %s != %s)\n' \
                "$SCRIPT_NAME" "$name" "${plain#"$ROOT"/}" "$want" "$got" >&2
            cross_fail=1
        fi
    done
    [ "$cross_fail" -eq 0 ] || die "web/dist-gz does not match web/dist (rebuild the Web UI)"
    log "cross-check ok: web/dist-gz matches deterministic gzip of web/dist"
fi

# --- web_version.txt ------------------------------------------------------
declare -A HASHES=()
for name in "${STAGED_NAMES[@]}"; do
    HASHES["$name"]="$(sha256sum "$STAGE/$name" | awk '{print $1}')"
done
key_for() {
    case "$1" in
        index.html.gz) printf 'index_gz_sha256' ;;
        ws.sharedworker.js.gz) printf 'worker_gz_sha256' ;;
        *) printf '%s_gz_sha256' "$(printf '%s' "${1%.gz}" | sed 's/[^A-Za-z0-9]/_/g')" ;;
    esac
}
meta="$STAGE/web_version.txt"
{
    printf 'web_version=%s\n' "$WEB_VERSION"
    printf 'protocol_version=%s\n' "$PROTOCOL_VERSION"
    printf 'git_tag=%s\n' "$git_tag"
    printf 'git_commit=%s\n' "$git_commit"
    printf 'build_time=%s\n' "$build_time"
    printf 'index_gz_sha256=%s\n' "${HASHES[index.html.gz]}"
    printf 'worker_gz_sha256=%s\n' "${HASHES[ws.sharedworker.js.gz]}"
    for name in "${STAGED_NAMES[@]}"; do
        case "$name" in
            index.html.gz|ws.sharedworker.js.gz) continue ;;
        esac
        printf '%s=%s\n' "$(key_for "$name")" "${HASHES[$name]}"
    done
} >"$meta"

# --- check mode: installed assets == web build? ---------------------------
meta_value() {
    sed -n "s/^$2=//p" "$1" | head -n1
}

if [ "$CHECK" -eq 1 ]; then
    rc=0
    if [ ! -d "$INSTALL_ABS" ]; then
        printf '%s: MISSING install dir %s\n' "$SCRIPT_NAME" "$INSTALL_ABS" >&2
        rc=1
    else
        for name in "${STAGED_NAMES[@]}"; do
            installed="$INSTALL_ABS/$name"
            if [ ! -f "$installed" ]; then
                printf '%s: MISSING installed asset %s\n' "$SCRIPT_NAME" "$name" >&2
                rc=1
            elif [ "$(sha256sum "$installed" | awk '{print $1}')" != "${HASHES[$name]}" ]; then
                printf '%s: HASH MISMATCH %s (installed != web build)\n' "$SCRIPT_NAME" "$name" >&2
                rc=1
            else
                log "check ok: $name sha256=${HASHES[$name]}"
            fi
        done
        if [ -f "$INSTALL_ABS/web_version.txt" ]; then
            # Content identity: version/protocol/hashes must match exactly.
            for key in web_version protocol_version index_gz_sha256 worker_gz_sha256; do
                want="$(meta_value "$meta" "$key")"
                got="$(meta_value "$INSTALL_ABS/web_version.txt" "$key")"
                if [ "$want" != "$got" ]; then
                    printf '%s: METADATA MISMATCH %s: want '%s' got '%s'\n' \
                        "$SCRIPT_NAME" "$key" "$want" "$got" >&2
                    rc=1
                fi
            done
            # git_tag/git_commit are build provenance, not content identity: a
            # commit that only touches firmware/scripts does not make the
            # installed gz stale, so a difference is a warning (not a failure).
            for key in git_tag git_commit; do
                want="$(meta_value "$meta" "$key")"
                got="$(meta_value "$INSTALL_ABS/web_version.txt" "$key")"
                if [ "$want" != "$got" ]; then
                    warn "provenance differs for $key: installed '$got' vs current build '$want' (content hashes match; rebuild+--install only when the Web UI changes)"
                fi
            done
            log "check ok: web_version.txt content metadata matches (hashes/version/protocol; build_time+provenance ignored)"
        else
            warn "installed project_components/html/web_version.txt is missing (docs/versioning.md §4.4; run --install to create it)"
        fi
        for gz in "$INSTALL_ABS"/*.gz; do
            [ -e "$gz" ] || continue
            name="$(basename "$gz")"
            if ! printf '%s\n' "${STAGED_NAMES[@]}" | grep -qx "$name"; then
                warn "install dir has an extra unmanaged asset: $name (left untouched)"
            fi
        done
    fi
    [ "$rc" -eq 0 ] || die "--check failed: installed web assets are not the current web build (run '$SCRIPT_NAME --install' explicitly)"
    log "OK: project_components/html matches the current web build"
    exit 0
fi

# --- install mode ---------------------------------------------------------
log "staged assets in $STAGE (web_version=$WEB_VERSION protocol=$PROTOCOL_VERSION tag=$git_tag)"
if [ "$INSTALL" -eq 1 ]; then
    mkdir -p "$INSTALL_ABS"
    for name in "${STAGED_NAMES[@]}" web_version.txt; do
        src_file="$STAGE/$name"
        dst_file="$INSTALL_ABS/$name"
        old=""
        [ -f "$dst_file" ] && old="$(sha256sum "$dst_file" | awk '{print $1}')"
        new="$(sha256sum "$src_file" | awk '{print $1}')"
        if [ "$old" = "$new" ]; then
            log "unchanged: $name"
        else
            cp "$src_file" "$dst_file.tmp"
            mv "$dst_file.tmp" "$dst_file"
            log "installed: $name${old:+ ($old -> $new)}"
        fi
    done
    for gz in "$INSTALL_ABS"/*.gz; do
        [ -e "$gz" ] || continue
        name="$(basename "$gz")"
        if ! printf '%s\n' "${STAGED_NAMES[@]}" | grep -qx "$name"; then
            warn "unmanaged asset left in place: $name (not part of the current web build)"
        fi
    done
    log "OK: installed embedded assets into $INSTALL_ABS (explicit --install)"
else
    log "dry run: project_components/html was not modified"
    log "to update the committed embedded assets run: $SCRIPT_NAME --install"
fi

log "hash manifest ($STAGE/web_version.txt):"
sed 's/^/  /' "$STAGE/web_version.txt"
