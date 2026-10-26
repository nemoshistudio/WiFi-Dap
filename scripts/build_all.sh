#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build_all.sh -- one-shot local build for all WiFi Dap targets.
#
# Builds the Wave-1 targets with the Dockerized ESP-IDF 5.5.6 toolchain
# (scripts/idf-docker.sh) under the flock build semaphore (scripts/build-lock.sh):
#
#   esp32s3 : 8MB flash + partitions/partitions_8mb_ota.csv   (release target)
#   esp32c3 : 4MB flash + partitions/partitions_4mb_single.csv (compile-only)
#   esp32   : 4MB flash + partitions/partitions_4mb_single.csv (compile-only)
#   esp32c5 : 8MB flash + partitions/partitions_8mb_ota.csv    (Wi-Fi-only;
#             no USB OTG/NCM; opt-in, not in the default matrix; unified
#             ESP-IDF v5.5.6 baseline, image espressif/idf:v5.5.6 via
#             IDF_DOCKER_IMAGE_C5. Pass `-t esp32c5` explicitly.)
#
# Each target gets its own build directory (build/esp32s3, build/esp32c3,
# build/esp32) and a full log under build/logs/<target>.log. After a successful
# build the embedded esp_app_desc.version is parsed from the .bin and checked to
# be non-empty (docs/versioning.md §2.2/§2.5); a summary (version + size) is
# printed and the partition/app-size gate (scripts/check_partitions.py) runs.
#
# This script only builds locally. It never pushes, changes branches, tags or
# touches any git remote.
#
# Usage:
#   scripts/build_all.sh                          # all three targets
#   scripts/build_all.sh -t esp32s3 -j 4          # subset, 4 compile jobs
#   scripts/build_all.sh --clean                  # delete build dirs first
#   scripts/build_all.sh --no-lock -t esp32       # skip the flock semaphore
#   scripts/build_all.sh --fw-version v1.0.0      # explicit PROJECT_VER override
#
# Environment: IDF_DOCKER_IMAGE (default espressif/idf:v5.5.6) for every target;
#              IDF_DOCKER_IMAGE_C5 (default espressif/idf:v5.5.6) for esp32c5;
#              BUILD_LOCK_SLOTS / BUILD_LOCK_TIMEOUT (see build-lock.sh).
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"

TARGETS=(esp32s3 esp32c3 esp32)
JOBS=""
CLEAN=0
USE_LOCK=1
FW_VERSION=""
RELEASE=0
CHECK_PARTITIONS=1
KEEP_GOING=1

usage() {
    cat <<'EOF'
Usage: build_all.sh [options]

  -t, --targets "T..."   space/comma separated subset of:
                         esp32s3 esp32c3 esp32 [esp32c5]
                         (default: esp32s3 esp32c3 esp32; esp32c5 is opt-in and
                          builds with its own ESP-IDF image)
  -j, --jobs N           parallel compile jobs (passed to the container as
                         CMAKE_BUILD_PARALLEL_LEVEL; idf.py has no -j option)
      --clean            remove the per-target build directories first
      --no-lock          do not use scripts/build-lock.sh (not recommended)
      --fw-version V     pass -DBIRDIEBOT_FW_VERSION=V (e.g. v1.0.0)
      --release          pass -DBIRDIEBOT_FW_RELEASE=ON (reject -dirty/fallback
                         version resolution; used by scripts/release_local.sh)
      --no-check         skip the scripts/check_partitions.py gate
      --fail-fast        stop after the first failing target
  -h, --help             show this help

Outputs: build/<target>/ (artifacts), build/logs/<target>.log (full log).
Exit: 0 all requested targets built green; 1 at least one target failed.
EOF
}

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 2
}

log() {
    printf '[build_all] %s\n' "$*"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -t|--targets)
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            IFS=', ' read -r -a TARGETS <<<"$2"
            shift 2
            ;;
        -t?*|--targets=?*)
            arg="${1#*=}"
            arg="${arg#-t}"
            arg="${arg#--targets=}"
            IFS=', ' read -r -a TARGETS <<<"$arg"
            shift
            ;;
        -j|--jobs)
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            JOBS="$2"
            shift 2
            ;;
        -j?*)
            JOBS="${1#-j}"
            shift
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --no-lock)
            USE_LOCK=0
            shift
            ;;
        --release)
            RELEASE=1
            shift
            ;;
        --no-check)
            CHECK_PARTITIONS=0
            shift
            ;;
        --fail-fast)
            KEEP_GOING=0
            shift
            ;;
        --fw-version)
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            FW_VERSION="$2"
            shift 2
            ;;
        --fw-version=*)
            FW_VERSION="${1#*=}"
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

[ "${#TARGETS[@]}" -gt 0 ] || die "no targets given"
[ -x "$SCRIPT_DIR/idf-docker.sh" ] || die "missing executable $SCRIPT_DIR/idf-docker.sh"
[ -x "$SCRIPT_DIR/build-lock.sh" ] || die "missing executable $SCRIPT_DIR/build-lock.sh"
[ -f "$ROOT/CMakeLists.txt" ] || die "$ROOT is not an ESP-IDF project root (no CMakeLists.txt)"

# --- protocol version gate (docs/versioning.md §3.1/§8.2) -------------------
# C header == docs/protocol.md == web/src/api/index.ts; a one-sided bump must
# fail the build rather than produce a firmware/UI protocol mismatch.
if [ -x "$SCRIPT_DIR/check-protocol-version.sh" ]; then
    if ! _proto_out="$("$SCRIPT_DIR/check-protocol-version.sh" 2>&1)"; then
        printf '%s\n' "$_proto_out" >&2
        log "ERROR: protocol version gate failed (C header / docs / Web UI)"
        exit 1
    fi
    log "protocol version gate: ok"
else
    printf '%s: warning: scripts/check-protocol-version.sh missing; protocol gate skipped\n' "$SCRIPT_NAME" >&2
fi

defaults_file() {
    printf 'sdkconfig.defaults.%s' "$1"
}

# Unified ESP-IDF v5.5.6 baseline: every target (S3/C3/ESP32/C5) builds with
# espressif/idf:v5.5.6. IDF_DOCKER_IMAGE_C5 / IDF_DOCKER_IMAGE remain as
# override points (e.g. to pin a different patch image per target).
image_for_target() {
    case "$1" in
        esp32c5) printf '%s' "${IDF_DOCKER_IMAGE_C5:-espressif/idf:v5.5.6}" ;;
        *)       printf '%s' "${IDF_DOCKER_IMAGE:-espressif/idf:v5.5.6}" ;;
    esac
}

# Component-manager "managed_components/" is a single project-level directory.
# The project now uses one IDF family (v5.5.x), but the reset logic is kept so
# that a mixed/pinned-image invocation (e.g. IDF_DOCKER_IMAGE=v5.2.1 for one
# target) cannot leave incompatible .component_hash metadata behind: ESP-IDF
# v5.2.1 and v5.5.x write incompatible metadata formats.
idf_family_for_image() {
    case "$1" in
        *:v5.5*) printf 'v55' ;;
        *:v5.2*) printf 'v52' ;;
        *)       printf '%s' "${1##*:}" ;;
    esac
}
reset_managed_components_if_needed() {
    local target_image="$1" family stamp prev
    family="$(idf_family_for_image "$target_image")"
    stamp="$ROOT/build/.managed_components_idf_family"
    prev="$(cat "$stamp" 2>/dev/null || true)"
    if [ -n "$prev" ] && [ "$prev" != "$family" ] && [ -d "$ROOT/managed_components" ]; then
        log "IDF family changed ($prev -> $family): recreating managed_components (re-downloaded by the component manager)"
        rm -rf "$ROOT/managed_components"
    fi
    mkdir -p "$ROOT/build"
    printf '%s\n' "$family" > "$stamp"
}
build_dir_rel() {
    printf 'build/%s' "$1"
}

run_locked() {
    if [ "$USE_LOCK" -eq 1 ]; then
        "$SCRIPT_DIR/build-lock.sh" "$@"
    else
        "$@"
    fi
}

# Print "version size_bytes" for a built app image (empty version -> "EMPTY").
inspect_bin() {
    local bin="$1"
    python3 - "$bin" <<'PY'
import struct, sys
path = sys.argv[1]
data = open(path, "rb").read()
version = ""
if len(data) >= 0x50 and data[0] == 0xE9:
    if struct.unpack_from("<I", data, 0x20)[0] == 0xABCD5432:
        version = struct.unpack_from("<32s", data, 0x30)[0].split(b"\0", 1)[0].decode("ascii", "replace")
print(f"{version or 'EMPTY'} {len(data)}")
PY
}

declare -a RESULT_TARGET=() RESULT_RC=() RESULT_VER=() RESULT_SIZE=()
OVERALL_RC=0
mkdir -p "$ROOT/build/logs"

for target in "${TARGETS[@]}"; do
    case "$target" in
        esp32s3|esp32c3|esp32|esp32c5) ;;
        *) die "unsupported target '$target' (expected esp32s3|esp32c3|esp32|esp32c5)" ;;
    esac
    build_rel="$(build_dir_rel "$target")"
    build_abs="$ROOT/$build_rel"
    log_file="$ROOT/build/logs/$target.log"
    target_image="$(image_for_target "$target")"
    printf '\n[build_all] === %s (build dir %s) ===\n' "$target" "$build_rel"
    log "image: $target_image"
    reset_managed_components_if_needed "$target_image"
    log "log: $log_file"

    if [ "$CLEAN" -eq 1 ]; then
        log "removing $build_rel"
        rm -rf "$build_abs"
    fi

    rc=0
    : >"$log_file"
    # `set-target` only when the build directory is missing or was configured
    # for a different chip; repeated runs then just do an incremental build.
    need_set_target=1
    if [ -f "$build_abs/CMakeCache.txt" ] && \
       grep -qE "^IDF_TARGET:(STRING|INTERNAL)=$target$" "$build_abs/CMakeCache.txt"; then
        need_set_target=0
    fi

    # Keep each target's sdkconfig inside its own build directory. ESP-IDF
    # defaults to <project>/sdkconfig for every -B directory, so without this
    # three sequential target builds would overwrite each other's config and the
    # next incremental build fails with an IDF_TARGET mismatch.
    sdkconfig_rel="$build_rel/sdkconfig"

    if [ "$need_set_target" -eq 1 ] && [ -d "$build_abs" ] && [ ! -f "$build_abs/CMakeCache.txt" ]; then
        # Interrupted/failed configure (e.g. a CMake FATAL) can leave a build
        # dir without CMakeCache.txt; idf.py set-target then refuses to
        # fullclean it. It is our own build/<target> dir, so recreate it.
        log "removing incomplete build dir $build_rel (no CMakeCache.txt)"
        rm -rf "$build_abs"
    fi

    if [ "$need_set_target" -eq 1 ]; then
        log "idf.py set-target $target (SDKCONFIG_DEFAULTS=$(defaults_file "$target"), SDKCONFIG=$sdkconfig_rel)"
        # All -D options must precede the idf.py command (set-target), otherwise
        # idf.py treats them as unknown commands.
        set_cmd=("$SCRIPT_DIR/idf-docker.sh" -B "$build_rel"
                 --image "$target_image"
                 -D "SDKCONFIG_DEFAULTS=$(defaults_file "$target")"
                 -D "SDKCONFIG=$sdkconfig_rel")
        [ -n "$FW_VERSION" ] && set_cmd+=(-D "BIRDIEBOT_FW_VERSION=$FW_VERSION")
        set_cmd+=(-D "BIRDIEBOT_FW_RELEASE=$([ "$RELEASE" -eq 1 ] && echo ON || echo OFF)")
        set_cmd+=(set-target "$target")
        if ! run_locked "${set_cmd[@]}" >>"$log_file" 2>&1; then
            rc=1
        fi
    else
        log "reusing configured build directory (target already $target)"
    fi

    if [ "$rc" -eq 0 ]; then
        build_cmd=("$SCRIPT_DIR/idf-docker.sh" -B "$build_rel" --image "$target_image")
        # -j belongs to the idf-docker wrapper (CMAKE_BUILD_PARALLEL_LEVEL), NOT
        # to idf.py: `idf.py build -j N` fails with "No such option: -j". It must
        # precede the idf.py -D arguments so the wrapper can consume it.
        [ -n "$JOBS" ] && build_cmd+=(-j "$JOBS")
        build_cmd+=(-D "SDKCONFIG=$sdkconfig_rel"
                    -D "BIRDIEBOT_FW_RELEASE=$([ "$RELEASE" -eq 1 ] && echo ON || echo OFF)")
        build_cmd+=(build)
        log "idf.py build${JOBS:+ (parallel jobs: $JOBS)}"
        if ! run_locked "${build_cmd[@]}" >>"$log_file" 2>&1; then
            rc=1
        fi
    fi

    ver=""; size=""
    bin="$build_abs/birdiebot_wifi_dap.bin"
    if [ "$rc" -eq 0 ] && [ -f "$bin" ]; then
        read -r ver size <<<"$(inspect_bin "$bin")"
        if [ "$ver" = "EMPTY" ] || [ -z "$ver" ]; then
            log "ERROR: esp_app_desc.version is empty in $build_rel/birdiebot_wifi_dap.bin"
            log "       PROJECT_VER was not injected (see cmake/version.cmake)"
            rc=1
        fi
    elif [ "$rc" -eq 0 ]; then
        log "ERROR: build reported success but $build_rel/birdiebot_wifi_dap.bin is missing"
        rc=1
    fi

    RESULT_TARGET+=("$target")
    RESULT_RC+=("$rc")
    RESULT_VER+=("${ver:-?}")
    RESULT_SIZE+=("${size:-?}")
    if [ "$rc" -eq 0 ]; then
        python3 - "$log_file" "$ver" <<'PY'
import sys
log, ver = sys.argv[1], sys.argv[2]
with open(log, "r", errors="replace") as fh:
    lines = [ln.strip() for ln in fh if "Version:" in ln]
print("[build_all] resolved version log:")
for ln in lines[-3:]:
    print("           " + ln)
PY
        printf '[build_all] %s OK version=%s size=%s bytes\n' "$target" "$ver" "$size"
    else
        printf '[build_all] %s FAILED (see %s)\n' "$target" "$log_file" >&2
        OVERALL_RC=1
        if [ "$KEEP_GOING" -eq 0 ]; then
            break
        fi
    fi
done

printf '\n[build_all] ===== summary =====\n'
printf '%-10s %-6s %-32s %s\n' TARGET RC VERSION "SIZE(bytes)"
for i in "${!RESULT_TARGET[@]}"; do
    printf '%-10s %-6s %-32s %s\n' \
        "${RESULT_TARGET[$i]}" \
        "$([ "${RESULT_RC[$i]}" -eq 0 ] && echo OK || echo FAIL)" \
        "${RESULT_VER[$i]}" \
        "${RESULT_SIZE[$i]}"
done

if [ "$OVERALL_RC" -eq 0 ] && [ "$CHECK_PARTITIONS" -eq 1 ] && [ -f "$SCRIPT_DIR/check_partitions.py" ]; then
    printf '\n[build_all] partition / app-size gate\n'
    if ! python3 "$SCRIPT_DIR/check_partitions.py" --require-bin; then
        OVERALL_RC=1
    fi
fi

if [ "$OVERALL_RC" -eq 0 ]; then
    printf '\n[build_all] ALL TARGETS GREEN\n'
else
    printf '\n[build_all] BUILD FAILED\n' >&2
fi
exit "$OVERALL_RC"
