#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# idf-docker.sh -- run ESP-IDF (v5.2.1) inside a Docker container.
#
# Usage:
#   scripts/idf-docker.sh [-C <project-dir>] [-B <build-dir>] <idf.py args...>
#
# Examples:
#   scripts/idf-docker.sh --version
#   scripts/idf-docker.sh -B build/esp32s3 -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 set-target esp32s3
#   scripts/idf-docker.sh -B build/esp32s3 build
#
# The project directory (default: current working directory; overridable with -C)
# is mounted at /project inside the container and files are written with the
# host user's uid/gid. ccache lives in ~/.cache/birdiebot-ccache on the host and
# is mounted at /ccache inside the container.
#
# This script only builds locally; it never pushes, changes branches or touches
# the git remotes.
#
# Environment variables:
#   IDF_DOCKER_IMAGE      container image              (default espressif/idf:v5.2.1)
#   IDF_CCACHE_DIR        host ccache directory        (default ~/.cache/birdiebot-ccache)
#   IDF_DOCKER_EXTRA_ARGS extra "docker run" arguments (e.g. "--device=/dev/ttyUSB0")
set -euo pipefail

readonly IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.2.1}"
readonly SCRIPT_NAME="${0##*/}"

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}

log() {
    printf '[idf-docker] %s\n' "$*" >&2
}

usage() {
    cat <<'EOF'
Usage: idf-docker.sh [-C <project-dir>] [-B <build-dir>] <idf.py args...>

  -C <dir>   ESP-IDF project root to mount (default: current directory)
  -B <dir>   build directory; relative to the project root (default: idf.py's own
             default, i.e. "build"). Created if missing and kept inside the project
             mount so artifacts survive the container.
  -h         show this help

Any other argument is forwarded verbatim to idf.py, e.g.:
  idf-docker.sh --version
  idf-docker.sh -B build/esp32c3 set-target esp32c3
  idf-docker.sh -B build/esp32c3 build
  idf-docker.sh -B build/esp32c3 -j 4 build

Environment: IDF_DOCKER_IMAGE, IDF_CCACHE_DIR, IDF_DOCKER_EXTRA_ARGS.
EOF
}

# ---------------------------------------------------------------- arg parsing
PROJECT_DIR=""
BUILD_DIR=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        -C)
            [ "$#" -ge 2 ] || die "-C requires a directory argument"
            PROJECT_DIR="$2"
            shift 2
            ;;
        -C?*)
            PROJECT_DIR="${1#-C}"
            shift
            ;;
        -B)
            [ "$#" -ge 2 ] || die "-B requires a directory argument"
            BUILD_DIR="$2"
            shift 2
            ;;
        -B?*)
            BUILD_DIR="${1#-B}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            # Everything from here on belongs to idf.py (e.g. --version).
            break
            ;;
    esac
done

[ "$#" -gt 0 ] || die "no idf.py arguments given (try '$SCRIPT_NAME --version' or '$SCRIPT_NAME -h')"

# ------------------------------------------------------------- host paths
if [ -n "$PROJECT_DIR" ]; then
    [ -d "$PROJECT_DIR" ] || die "project directory not found: $PROJECT_DIR"
    PROJECT_ABS="$(cd "$PROJECT_DIR" && pwd -P)"
else
    PROJECT_ABS="$(pwd -P)"
fi

if [ "$1" != "--version" ] && [ ! -f "$PROJECT_ABS/CMakeLists.txt" ]; then
    log "warning: $PROJECT_ABS does not look like an ESP-IDF project root (no CMakeLists.txt)"
fi

HOST_HOME="${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}"
[ -n "$HOST_HOME" ] || die "cannot determine home directory; set HOME or IDF_CCACHE_DIR"
CCACHE_HOST="${IDF_CCACHE_DIR:-$HOST_HOME/.cache/birdiebot-ccache}"

# `-B`: keep the build directory inside the project mount so it persists.
IDF_ARGS=()
if [ -n "$BUILD_DIR" ]; then
    case "$BUILD_DIR" in
        /*) BUILD_ABS="$BUILD_DIR" ;;
        *)  BUILD_ABS="$PROJECT_ABS/$BUILD_DIR" ;;
    esac
    BUILD_ABS="$(realpath -m "$BUILD_ABS")"
    case "$BUILD_ABS" in
        "$PROJECT_ABS"/*) ;;
        *) die "-B build directory must live inside the project ($PROJECT_ABS): $BUILD_ABS" ;;
    esac
    BUILD_REL="${BUILD_ABS#"$PROJECT_ABS"/}"
    [ -n "$BUILD_REL" ] || die "-B must point below the project root, not to it"
    mkdir -p "$BUILD_ABS"
    IDF_ARGS+=(-B "/project/$BUILD_REL")
fi

# ------------------------------------------------------------- docker checks
command -v docker >/dev/null 2>&1 || die "docker is not installed or not in PATH"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log "image $IMAGE not found locally; pulling (this can take several minutes)..."
    docker pull "$IMAGE"
fi

mkdir -p "$CCACHE_HOST" || die "cannot create ccache directory: $CCACHE_HOST"

# ------------------------------------------------------------- docker run
DOCKER_ARGS=(
    run --rm --init
    --user "$(id -u):$(id -g)"
    -e HOME=/tmp
    -e IDF_CCACHE_ENABLE=1
    -e CCACHE_DIR=/ccache
    -e CCACHE_BASEDIR=/project
    -v "$PROJECT_ABS:/project"
    -v "$CCACHE_HOST:/ccache"
    -w /project
)

# Allocate a TTY only when attached to one, so CI / redirected output still works.
[ -t 0 ] && DOCKER_ARGS+=(-i)
[ -t 1 ] && DOCKER_ARGS+=(-t)

if [ -n "${IDF_DOCKER_EXTRA_ARGS:-}" ]; then
    read -r -a _extra <<<"${IDF_DOCKER_EXTRA_ARGS}"
    DOCKER_ARGS+=("${_extra[@]}")
fi

log "image=$IMAGE"
log "project=$PROJECT_ABS (-> /project)"
[ -n "$BUILD_DIR" ] && log "build=$BUILD_ABS (-> ${IDF_ARGS[1]})"
log "ccache=$CCACHE_HOST (-> /ccache)"

exec docker "${DOCKER_ARGS[@]}" "$IMAGE" idf.py "${IDF_ARGS[@]}" "$@"
