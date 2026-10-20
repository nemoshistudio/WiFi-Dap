#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build-lock.sh -- flock-based build semaphore: limit concurrent builds.
#
# Usage:
#   scripts/build-lock.sh <command...>
#
# Runs <command...> while holding one of N build slots (default 3). If all slots
# are taken, waits for one to become free (with a timeout) before starting, so at
# most N builds run at the same time on this machine. Locks live in
# /tmp/birdiebot-buildlock and are released automatically when a process exits.
#
# Examples:
#   scripts/build-lock.sh scripts/idf-docker.sh -B build/esp32s3 build
#   scripts/build-lock.sh make -j4
#
# Environment variables:
#   BUILD_LOCK_DIR      lock directory       (default /tmp/birdiebot-buildlock)
#   BUILD_LOCK_SLOTS    max concurrent jobs  (default 3)
#   BUILD_LOCK_TIMEOUT  max wait in seconds  (default 1800; 0 = wait forever)
#   BUILD_LOCK_POLL     poll interval secs   (default 2)
#
# Exit status: the command's exit status; 2 = usage error; 75 = timed out
# waiting for a free slot; 130/143 = interrupted/terminated.
#
# This script only orchestrates local processes; it never pushes or mutates git.
set -euo pipefail

readonly LOCK_DIR="${BUILD_LOCK_DIR:-/tmp/birdiebot-buildlock}"
readonly SLOTS="${BUILD_LOCK_SLOTS:-3}"
readonly TIMEOUT="${BUILD_LOCK_TIMEOUT:-1800}"
readonly POLL="${BUILD_LOCK_POLL:-2}"
readonly SCRIPT_NAME="${0##*/}"

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage: build-lock.sh <command...>

Run <command...> while holding one of N build slots (default 3, env
BUILD_LOCK_SLOTS). When every slot is busy the command queues until a slot is
free. Locks live in /tmp/birdiebot-buildlock (env BUILD_LOCK_DIR) and are
released automatically when the process exits.

Environment:
  BUILD_LOCK_DIR      lock directory       (default /tmp/birdiebot-buildlock)
  BUILD_LOCK_SLOTS    max concurrent jobs  (default 3)
  BUILD_LOCK_TIMEOUT  max wait in secs     (default 1800, 0 = wait forever)
  BUILD_LOCK_POLL     poll interval secs   (default 2)

Examples:
  scripts/build-lock.sh scripts/idf-docker.sh -B build/esp32s3 build
  BUILD_LOCK_TIMEOUT=300 scripts/build-lock.sh make
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            die "unknown option '$1' (use '--' before commands that start with '-')"
            ;;
        *)
            break
            ;;
    esac
done

[ "$#" -gt 0 ] || die "no command given (try '$SCRIPT_NAME -h')"

case "$SLOTS" in
    ''|*[!0-9]*) die "BUILD_LOCK_SLOTS must be a positive integer, got '$SLOTS'" ;;
esac
[ "$SLOTS" -ge 1 ] || die "BUILD_LOCK_SLOTS must be >= 1"
case "$TIMEOUT" in
    ''|*[!0-9]*) die "BUILD_LOCK_TIMEOUT must be an integer, got '$TIMEOUT'" ;;
esac
case "$POLL" in
    ''|*[!0-9]*) die "BUILD_LOCK_POLL must be an integer, got '$POLL'" ;;
esac
[ "$POLL" -ge 1 ] || POLL=1

mkdir -p "$LOCK_DIR" || die "cannot create lock directory: $LOCK_DIR"

ACQUIRED_FD=""
ACQUIRED_SLOT=""

release_slot() {
    if [ -n "$ACQUIRED_FD" ]; then
        # Closing the descriptor releases the flock.
        eval "exec ${ACQUIRED_FD}>&-" 2>/dev/null || true
        ACQUIRED_FD=""
    fi
}
trap release_slot EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# Try to grab any free slot without blocking. Returns 0 on success.
try_acquire() {
    local i fd
    for ((i = 1; i <= SLOTS; i++)); do
        exec {fd}>"$LOCK_DIR/slot$i.lock"
        if flock -n "$fd"; then
            ACQUIRED_FD="$fd"
            ACQUIRED_SLOT="$i"
            return 0
        fi
        eval "exec ${fd}>&-"
    done
    return 1
}

waited=0
queued_msg=0
while ! try_acquire; do
    if [ "$TIMEOUT" -gt 0 ] && [ "$waited" -ge "$TIMEOUT" ]; then
        printf '%s: error: timed out after %ss waiting for a free build slot (%s slots busy, lock dir %s)\n' \
            "$SCRIPT_NAME" "$TIMEOUT" "$SLOTS" "$LOCK_DIR" >&2
        printf '%s: hint: increase BUILD_LOCK_TIMEOUT or check for stale/abandoned builds\n' \
            "$SCRIPT_NAME" >&2
        exit 75
    fi
    if [ "$queued_msg" -eq 0 ]; then
        if [ "$TIMEOUT" -eq 0 ]; then
            timeout_desc="no timeout"
        else
            timeout_desc="timeout ${TIMEOUT}s"
        fi
        printf '%s: all %s build slots busy; queued in %s (%s)\n' \
            "$SCRIPT_NAME" "$SLOTS" "$LOCK_DIR" "$timeout_desc" >&2
        queued_msg=1
    fi
    sleep "$POLL"
    waited=$((waited + POLL))
done

printf '%s: acquired slot %s/%s: %s\n' "$SCRIPT_NAME" "$ACQUIRED_SLOT" "$SLOTS" "$*" >&2

rc=0
"$@" || rc=$?

printf '%s: released slot %s/%s (exit %s)\n' "$SCRIPT_NAME" "$ACQUIRED_SLOT" "$SLOTS" "$rc" >&2
exit "$rc"
