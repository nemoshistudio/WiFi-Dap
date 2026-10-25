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
# Git metadata: a linked git worktree (git worktree add) stores only a `.git`
# *file* in the worktree, pointing at `<main>/.git/worktrees/<name>` outside the
# project directory. Without that gitdir the containerized CMake/git calls fail
# (`git describe`, `git show -s --format=%cd` for FW_UPD_DATE) and PROJECT_VER /
# FW_UPD_DATE silently become empty. Before starting the container we therefore
# resolve `git rev-parse --git-common-dir` / `--git-dir` on the host and bind-
# mount those directories read-only at their *original absolute paths* so git
# inside the container can follow the worktree `.git` file. If the resolution
# fails (no git, source archive) we log a warning and continue; the build then
# uses cmake/version.cmake's version.txt / v1.0.0 fallback.
#
# This script only builds locally; it never pushes, changes branches or touches
# the git remotes.
#
# Parallel builds: ESP-IDF 5.2.1's idf.py has no -j/--jobs option; the build
# tool parallelism is controlled by the CMAKE_BUILD_PARALLEL_LEVEL environment
# variable (honoured by `cmake --build`). This wrapper exposes that as `-j N`
# so callers never pass an unsupported flag to idf.py.
#
# Environment variables:
#   IDF_DOCKER_IMAGE             container image         (default espressif/idf:v5.2.1;
#                                overridden by the -I/--image option)
#   IDF_CCACHE_DIR               host ccache directory   (default ~/.cache/birdiebot-ccache)
#   IDF_DOCKER_EXTRA_ARGS        extra "docker run" args (e.g. "--device=/dev/ttyUSB0")
#   CMAKE_BUILD_PARALLEL_LEVEL   forwarded into the container when set on the host
#                                (the -j option takes precedence)
set -euo pipefail

# Default image; can be overridden by IDF_DOCKER_IMAGE or the -I/--image option.
IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.2.1}"
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
Usage: idf-docker.sh [-C <project-dir>] [-B <build-dir>] [-j <jobs>] [-I <image>] <idf.py args...>

  -C <dir>   ESP-IDF project root to mount (default: current directory)
  -B <dir>   build directory; relative to the project root (default: idf.py's own
             default, i.e. "build"). Created if missing and kept inside the project
             mount so artifacts survive the container.
  -j <jobs>  parallel build jobs inside the container (sets
             CMAKE_BUILD_PARALLEL_LEVEL; idf.py itself has no -j option)
  -I <image>, --image <image>
             explicit ESP-IDF docker image/tag for this invocation, e.g.
             "espressif/idf:v5.5.6". Overrides IDF_DOCKER_IMAGE (which still
             provides the default). Useful for per-target images (esp32c5).
  --git-probe  print the container's git metadata (describe/commit/date) and exit;
             use this to verify that a linked worktree's gitdir is reachable.
  -h         show this help

Options may be given in any order before the idf.py command. Tokens that are
not recognized wrapper options are forwarded to idf.py verbatim; -D/--define-
cache-entry is recognized explicitly so wrapper options (e.g. -j) may follow it.
  idf-docker.sh --version
  idf-docker.sh -B build/esp32c3 build
  idf-docker.sh -B build/esp32c3 -j 8 build
  idf-docker.sh -B build/x -D SDKCONFIG=build/x/sdkconfig build
  idf-docker.sh --version
  idf-docker.sh -B build/esp32c3 set-target esp32c3
  idf-docker.sh -B build/esp32c3 -j 8 build
  idf-docker.sh -I espressif/idf:v5.5.6 -B build/esp32c5 set-target esp32c5

Environment: IDF_DOCKER_IMAGE (default image), IDF_CCACHE_DIR,
             IDF_DOCKER_EXTRA_ARGS, CMAKE_BUILD_PARALLEL_LEVEL
             (forwarded; -j wins; -I/--image overrides the image).
EOF
}

# ---------------------------------------------------------------- arg parsing
PROJECT_DIR=""
BUILD_DIR=""
GIT_PROBE=0
JOBS=""
EXTRA_IDF_ARGS=()

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
        -j|--jobs)
            [ "$#" -ge 2 ] || die "$1 requires a positive integer"
            JOBS="$2"
            shift 2
            ;;
        --jobs=*)
            JOBS="${1#*=}"
            shift
            ;;
        -j?*)
            JOBS="${1#-j}"
            shift
            ;;
        -D|--define-cache-entry)
            # Recognized so wrapper options may appear after idf.py -D args;
            # forwarded verbatim to idf.py.
            [ "$#" -ge 2 ] || die "$1 requires an argument"
            EXTRA_IDF_ARGS+=("$1" "$2")
            shift 2
            ;;
        -D?*)
            EXTRA_IDF_ARGS+=("$1")
            shift
            ;;
        -I|--image)
            [ "$#" -ge 2 ] || die "$1 requires a container image argument"
            IMAGE="$2"
            shift 2
            ;;
        --image=*)
            IMAGE="${1#*=}"
            shift
            ;;
        --git-probe)
            GIT_PROBE=1
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

# Validate the parallel level early: idf.py has no -j, it maps to
# CMAKE_BUILD_PARALLEL_LEVEL inside the container.
if [ -n "$JOBS" ] && ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    die "-j/--jobs must be a positive integer (got '$JOBS')"
fi

[ -n "$IMAGE" ] || die "container image must not be empty (-I/--image / IDF_DOCKER_IMAGE)"

if [ "$GIT_PROBE" -eq 0 ]; then
    [ "$#" -gt 0 ] || die "no idf.py arguments given (try '$SCRIPT_NAME --version' or '$SCRIPT_NAME -h')"
else
    [ "$#" -eq 0 ] || die "--git-probe does not take idf.py arguments (got: $*)"
fi

# ------------------------------------------------------------- host paths
if [ -n "$PROJECT_DIR" ]; then
    [ -d "$PROJECT_DIR" ] || die "project directory not found: $PROJECT_DIR"
    PROJECT_ABS="$(cd "$PROJECT_DIR" && pwd -P)"
else
    PROJECT_ABS="$(pwd -P)"
fi

if [ "$GIT_PROBE" -eq 0 ] && [ "$1" != "--version" ] && [ ! -f "$PROJECT_ABS/CMakeLists.txt" ]; then
    log "warning: $PROJECT_ABS does not look like an ESP-IDF project root (no CMakeLists.txt)"
fi

HOST_HOME="${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}"
[ -n "$HOST_HOME" ] || die "cannot determine home directory; set HOME or IDF_CCACHE_DIR"
CCACHE_HOST="${IDF_CCACHE_DIR:-$HOST_HOME/.cache/birdiebot-ccache}"

# `-B`: keep the build directory inside the project mount so it persists.
IDF_ARGS=()
if [ "${#EXTRA_IDF_ARGS[@]}" -gt 0 ]; then
    IDF_ARGS+=("${EXTRA_IDF_ARGS[@]}")
fi
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

# ------------------------------------------------------------- git metadata
# Linked worktrees (git worktree add) keep only a `.git` file inside the project
# directory; the real gitdir lives outside the /project mount. Resolve both the
# common dir and the worktree admin dir on the host and bind-mount them read-only
# at the same absolute paths, otherwise git inside the container cannot follow
# the `.git` file. Resolution failure is only a warning: cmake/version.cmake has
# a version.txt -> v1.0.0 fallback for source archives / no-git builds.
GIT_MOUNT_ARGS=()
GIT_MOUNTED_DIRS=()
GIT_MOUNT_NOTES=()

_normalize_dir() {
    local dir="$1"
    [ -n "$dir" ] || return 1
    case "$dir" in
        /*) ;;
        *) dir="$PROJECT_ABS/$dir" ;;
    esac
    dir="$(realpath -m "$dir" 2>/dev/null || true)"
    [ -n "$dir" ] && [ -d "$dir" ] && printf '%s\n' "$dir"
}

_git_dir_from_rev_parse() {
    local what="$1" out=""
    out="$(git -C "$PROJECT_ABS" rev-parse --path-format=absolute "--$what" 2>/dev/null || true)"
    if [ -z "$out" ]; then
        # git < 2.31: no --path-format; rev-parse may print a relative path.
        out="$(git -C "$PROJECT_ABS" rev-parse "--$what" 2>/dev/null || true)"
    fi
    _normalize_dir "$out"
}

_add_git_mount() {
    local dir="$1" covered=""
    [ -n "$dir" ] || return 0
    # Already inside the /project mount? Nothing to do.
    case "$dir" in
        "$PROJECT_ABS" | "$PROJECT_ABS"/*) return 0 ;;
    esac
    for covered in "${GIT_MOUNTED_DIRS[@]:-}"; do
        [ -n "$covered" ] || continue
        case "$dir" in
            "$covered" | "$covered"/*) return 0 ;;
        esac
    done
    GIT_MOUNT_ARGS+=(-v "$dir:$dir:ro")
    GIT_MOUNTED_DIRS+=("$dir")
    GIT_MOUNT_NOTES+=("$dir")
}

if command -v git >/dev/null 2>&1; then
    GIT_COMMON_DIR="$(_git_dir_from_rev_parse git-common-dir || true)"
    GIT_DIR="$(_git_dir_from_rev_parse git-dir || true)"
    if [ -n "$GIT_COMMON_DIR" ] || [ -n "$GIT_DIR" ]; then
        # Parent first so a worktree admin dir (usually inside the common dir)
        # is not mounted twice.
        _add_git_mount "$GIT_COMMON_DIR"
        _add_git_mount "$GIT_DIR"
        if [ "${#GIT_MOUNTED_DIRS[@]}" -eq 0 ]; then
            log "git metadata: not a linked worktree (gitdir lives inside the project mount)"
        fi
    else
        log "warning: cannot resolve git metadata dirs for $PROJECT_ABS; containerized git may be unavailable"
    fi
else
    log "warning: git not installed on the host; containerized git may be unavailable (version fallback applies)"
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

# ESP-IDF v5.5+ component manager keeps its "root managed components" (managed
# dependencies of IDF itself) under $IDF_TOOLS_PATH/root_managed_components.
# The official image sets IDF_TOOLS_PATH=/opt/esp, which is root-owned, so a
# non-root container user (this wrapper runs with the host uid/gid) cannot
# create it and CMake fails with "PermissionError: ... /opt/esp/
# root_managed_components". Mount a writable host directory over that path;
# older IDF images (e.g. v5.2.1) never touch it, so this is harmless there.
IDF_ROOT_MANAGED_DIR="${IDF_ROOT_MANAGED_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/birdiebot-idf-root-managed}"
mkdir -p "$IDF_ROOT_MANAGED_DIR" || die "cannot create $IDF_ROOT_MANAGED_DIR"
DOCKER_ARGS+=(-v "$IDF_ROOT_MANAGED_DIR:/opt/esp/root_managed_components")

# Parallel build level: -j N (or the host CMAKE_BUILD_PARALLEL_LEVEL) is passed
# into the container as CMAKE_BUILD_PARALLEL_LEVEL, which `cmake --build` (used by
# idf.py build) honours. Do not pass -j to idf.py - IDF 5.2.1 does not accept it.
PARALLEL_LEVEL=""
if [ -n "$JOBS" ]; then
    PARALLEL_LEVEL="$JOBS"
elif [ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]; then
    PARALLEL_LEVEL="$CMAKE_BUILD_PARALLEL_LEVEL"
fi
if [ -n "$PARALLEL_LEVEL" ]; then
    if ! [[ "$PARALLEL_LEVEL" =~ ^[1-9][0-9]*$ ]]; then
        die "invalid parallel build level '$PARALLEL_LEVEL' (positive integer expected)"
    fi
    DOCKER_ARGS+=(-e "CMAKE_BUILD_PARALLEL_LEVEL=$PARALLEL_LEVEL")
fi

# Read-only git metadata mounts for linked worktrees (see the git metadata block
# above). Mounted at the same absolute path so the worktree `.git` file resolves.
if [ "${#GIT_MOUNT_ARGS[@]}" -gt 0 ]; then
    DOCKER_ARGS+=("${GIT_MOUNT_ARGS[@]}")
fi

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
[ -n "$PARALLEL_LEVEL" ] && log "parallel build level=$PARALLEL_LEVEL (CMAKE_BUILD_PARALLEL_LEVEL)"
if [ "${#GIT_MOUNT_NOTES[@]}" -gt 0 ]; then
    log "git metadata (ro): ${GIT_MOUNT_NOTES[*]}"
fi

if [ "$GIT_PROBE" -eq 1 ]; then
    # Verifies that a linked worktree's gitdir is reachable from inside the
    # container (used to debug empty PROJECT_VER / FW_UPD_DATE). Exits non-zero
    # if git describe or the commit-date query fails.
    read -r -d '' PROBE_CMD <<'PROBE' || true
fail=0
probe() {
    printf '%-24s: ' "$1"; shift
    if out="$("$@" 2>&1)"; then printf '%s\n' "$out"; else printf 'FAILED (%s)\n' "$out"; fail=1; fi
}
printf '%-24s: %s\n' "git-version" "$(git --version 2>&1)"
probe "git-common-dir" git -C /project rev-parse --path-format=absolute --git-common-dir
probe "git-dir" git -C /project rev-parse --path-format=absolute --git-dir
probe "describe --tags" git -C /project describe --tags --always --dirty
probe "describe v-match" git -C /project describe --tags --dirty --always --match 'v[0-9]*'
probe "rev-parse --short" git -C /project rev-parse --short HEAD
probe "commit-date" git -C /project show -s --format=%cd --date=format:%Y-%m-%d
exit "$fail"
PROBE
    log "git probe (container)"
    exec docker "${DOCKER_ARGS[@]}" "$IMAGE" bash -c "$PROBE_CMD"
fi

exec docker "${DOCKER_ARGS[@]}" "$IMAGE" idf.py "${IDF_ARGS[@]}" "$@"
