#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# ci_local.sh -- local presubmit gate aggregator.
#
# One command for the local equivalent of the manual GitHub Actions template
# (.github/workflows/main.yml): host tests + static contract gates, plus an
# opt-in Docker build with the partition/app-size gate. Output is TAP-ish on
# stdout (one "ok/not ok" line per stage + "#" diagnostics) and every stage
# writes a full log under build/ci/<run-id>/ (gitignored).
#
# Stages (in order):
#   1. host tests             scripts/test_host.sh
#                             (C wifi_cred_logic + DAP ASan harness + python
#                              contract suites; no ESP-IDF/Docker/hardware)
#   2. component host tests   components/*/test/host/run.sh, one stage per
#                             component. A runner that needs a Docker image is
#                             skipped when that image is not present locally,
#                             because ci_local never pulls images.
#   3. static contract gates  scripts/check-protocol-version.sh
#                             scripts/check_board_pins.py
#                             scripts/check-release-versions.sh  (--release)
#                             scripts/gen_web_assets.sh --check  (when a Web UI
#                               build exists in web/dist or web/dist-gz)
#                             web/scripts/check-i18n.mjs --strict (when
#                               web/node_modules exists)
#   4. Docker build + gate    only with --build: scripts/build_all.sh per target
#                             followed by scripts/check_partitions.py
#                             --require-bin for that target's app image.
#
# This script only reads the tree and writes into the gitignored build/ (and
# mktemp dirs inside the called scripts). It never installs packages, never
# pulls Docker images, never builds/flashes on a device, and never pushes,
# tags, commits or otherwise touches git state.
#
# Usage:
#   scripts/ci_local.sh                       # host tests + static gates
#   scripts/ci_local.sh --skip-web            # without the Web UI gates
#   scripts/ci_local.sh --skip-host           # only the static gates
#   scripts/ci_local.sh --build s3            # + S3 Docker build + partition gate
#   scripts/ci_local.sh --build "s3 c3"       # subset / multiple targets
#   scripts/ci_local.sh --build all           # default matrix (s3+c3+esp32)
#   scripts/ci_local.sh --release --build s3  # release presubmit (needs a tag)
#   scripts/ci_local.sh --dry-run             # print the plan, run nothing
#
# Exit: 0 all selected stages passed (explicit SKIPs are reported but do not
# fail the run); 1 at least one stage failed; 2 usage error.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"
readonly PYTHON_BIN="${PYTHON:-python3}"
# Same default image as idf-docker.sh/build_all.sh; only used to check presence.
readonly DEFAULT_IDF_IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.5.6}"

SKIP_HOST=0
SKIP_WEB=0
SKIP_DOCKER=0
RELEASE=0
DRY_RUN=0
BUILD_ARG=""
declare -a BUILD_LIST=()

usage() {
    cat <<'EOF'
Usage: ci_local.sh [options]

Local presubmit gate: host tests + static contract gates (+ optional Docker
build + partition gate). TAP-ish stdout, full per-stage logs under build/ci/.

  --skip-host        skip the host test stages (scripts/test_host.sh and
                     components/*/test/host/run.sh)
  --skip-web         skip the Web UI static gates (gen_web_assets.sh --check
                     and web/scripts/check-i18n.mjs --strict)
  --release          release presubmit: run scripts/check-release-versions.sh
                     (strict tag/protocol/image gate; needs HEAD on a vX.Y.Z tag)
                     and pass --release to scripts/build_all.sh
  --build TARGETS    additionally run the Docker build + partition gate for
                     TARGETS: s3|c3|esp32|c5|all, comma/space separated.
                     'all' = the default matrix s3+c3+esp32 (c5 is opt-in).
                     Requires the ESP-IDF image to be present locally; ci_local
                     never pulls images (docker pull it first).
  --skip-docker      skip stages that need Docker (component host tests whose
                     runner uses docker run; Docker builds are not allowed in
                     the same run). Useful on machines without the IDF image.
  --dry-run          print the commands that would run; execute nothing.
  -h, --help         show this help.

Stages (in order):
  1. host tests            scripts/test_host.sh
  2. component host tests  components/*/test/host/run.sh (Docker-image runners
                           are skipped when the image is absent locally)
  3. static gates          check-protocol-version.sh, check_board_pins.py,
                           check-release-versions.sh (only --release),
                           gen_web_assets.sh --check (only if web/dist* exists),
                           check-i18n.mjs --strict (only if web/node_modules exists)
  4. Docker build + gate   --build only: build_all.sh per target, then
                           check_partitions.py --require-bin per target

Output/logs:
  stdout is TAP-ish ("ok N - <stage>", "not ok N - <stage>", "# ...").
  Logs: build/ci/<UTC run-id>/<NN>-<stage>.log (+ summary.txt);
  build/ci/latest points at the newest run. Exit code is the aggregate:
  0 = pass (SKIPs do not fail), 1 = at least one stage failed, 2 = usage error.

Never installs packages, never pulls Docker images, never flashes hardware,
never pushes/tags and never modifies git state.
EOF
}

die() {
    printf '%s: error: %s\n' "$SCRIPT_NAME" "$*" >&2
    printf "try '%s --help'\n" "$SCRIPT_NAME" >&2
    exit 2
}

log() {
    printf '[ci_local] %s\n' "$*" >&2
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-host)
            SKIP_HOST=1
            shift
            ;;
        --skip-web)
            SKIP_WEB=1
            shift
            ;;
        --skip-docker)
            SKIP_DOCKER=1
            shift
            ;;
        --release)
            RELEASE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --build)
            [ "$#" -ge 2 ] || die "--build requires a target list"
            BUILD_ARG="$2"
            shift 2
            ;;
        --build=*)
            BUILD_ARG="${1#*=}"
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

[ -f "$ROOT/CMakeLists.txt" ] || die "$ROOT is not an ESP-IDF project root (no CMakeLists.txt)"

# Normalize --build targets to build_all.sh target names (deduplicated, in order).
normalize_targets() {
    local raw="$1" tok target
    local -a toks=() expanded=()
    local seen=" "
    read -r -a toks <<<"${raw//,/ }" || true
    [ "${#toks[@]}" -gt 0 ] || die "--build: empty target list"

    # 'all' = the build_all.sh default matrix (c5 stays opt-in).
    for tok in "${toks[@]}"; do
        case "$tok" in
            all) expanded+=(esp32s3 esp32c3 esp32) ;;
            *)   expanded+=("$tok") ;;
        esac
    done

    for tok in "${expanded[@]}"; do
        case "$tok" in
            s3|esp32s3)   target="esp32s3" ;;
            c3|esp32c3)   target="esp32c3" ;;
            esp32)        target="esp32" ;;
            c5|esp32c5)   target="esp32c5" ;;
            *) die "unsupported --build target '$tok' (expected s3|c3|esp32|c5|all)" ;;
        esac
        case "$seen" in
            *" $target "*) continue ;;
        esac
        case "$target" in
            esp32s3) BUILD_LIST+=(esp32s3); seen+="esp32s3 " ;;
            esp32c3) BUILD_LIST+=(esp32c3); seen+="esp32c3 " ;;
            esp32)   BUILD_LIST+=(esp32);   seen+="esp32 " ;;
            esp32c5) BUILD_LIST+=(esp32c5); seen+="esp32c5 " ;;
            *) die "internal: unhandled target '$target'" ;;
        esac
    done
}
if [ -n "$BUILD_ARG" ]; then
    normalize_targets "$BUILD_ARG"
    [ "${#BUILD_LIST[@]}" -gt 0 ] || die "--build: no targets given"
    if [ "$SKIP_DOCKER" -eq 1 ]; then
        die "--skip-docker conflicts with --build (a build needs Docker)"
    fi
fi

# ---------------------------------------------------------------------------
# TAP-ish helpers / stage bookkeeping
# ---------------------------------------------------------------------------
stage_no=0
stage_fail=0
stage_skip=0
last_stage_rc=0

CI_DIR="$ROOT/build/ci"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_DIR="$CI_DIR/$RUN_ID"

tap_ok()   { stage_no=$((stage_no + 1)); printf 'ok %d - %s\n' "$stage_no" "$1"; }
tap_fail() { stage_no=$((stage_no + 1)); stage_fail=$((stage_fail + 1)); printf 'not ok %d - %s\n' "$stage_no" "$1"; }
tap_skip() { stage_no=$((stage_no + 1)); stage_skip=$((stage_skip + 1)); printf 'ok %d - %s # SKIP %s\n' "$stage_no" "$1" "$2"; }
diag()     { printf '# %s\n' "$*"; }

# Log path for the next stage (must be called before the stage runs).
log_for() {
    printf '%s/%02d-%s.log' "$LOG_DIR" "$((stage_no + 1))" "$1"
}

show_failure_log() {
    local logfile="$1"
    [ -f "$logfile" ] || return 0
    diag "--- ${logfile#"$ROOT"/} (tail) ---"
    sed 's/^/#   /' "$logfile" | tail -n 80
}

# Shell-quoted rendering of a command, for log/dry-run display only.
cmd_str() {
    local out="" arg
    for arg in "$@"; do
        out+="$(printf '%q' "$arg") "
    done
    printf '%s' "${out% }"
}

# run_stage <name> <logfile> <cmd...>
# Executes the command with output captured to <logfile>, then prints one TAP
# line and keeps the aggregate counters. Always returns 0 (use last_stage_rc).
run_stage() {
    local name="$1" logfile="$2"
    shift 2
    if [ "$DRY_RUN" -eq 1 ]; then
        diag "dry-run: $(cmd_str "$@")"
        tap_skip "$name" "--dry-run"
        last_stage_rc=0
        return 0
    fi
    log "run: $(cmd_str "$@")"
    mkdir -p "$LOG_DIR"
    local rc=0
    "$@" >"$logfile" 2>&1 || rc=$?
    last_stage_rc=$rc
    if [ "$rc" -eq 0 ]; then
        tap_ok "$name"
        log "PASS: $name"
    else
        tap_fail "$name"
        show_failure_log "$logfile"
        log "FAIL($rc): $name (log: ${logfile#"$ROOT"/})"
    fi
    return 0
}

# stage_or_skip <name> <logfile> <skip-reason> <cmd...>
# Empty reason -> run the stage. Non-empty reason -> TAP SKIP without running
# (except under --dry-run, which still prints the would-be command).
stage_or_skip() {
    local name="$1" logfile="$2" reason="$3"
    shift 3
    if [ -n "$reason" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            diag "note: would SKIP ($reason)"
        else
            tap_skip "$name" "$reason"
            log "SKIP: $name ($reason)"
            last_stage_rc=0
            return 0
        fi
    fi
    run_stage "$name" "$logfile" "$@"
}

# ---------------------------------------------------------------------------
# Docker / component-test helpers (availability only -- never pulls an image)
# ---------------------------------------------------------------------------
runner_needs_docker() {
    grep -qE '(^|[[:space:]])docker[[:space:]]+run([[:space:]]|$)' "$1" 2>/dev/null
}

runner_docker_image() {
    local image=""
    image="$(grep -oE 'espressif/idf:[A-Za-z0-9._-]+' "$1" 2>/dev/null | head -n 1 || true)"
    printf '%s' "${image:-$DEFAULT_IDF_IMAGE}"
}

# Prints the reason a Docker image cannot be used, or nothing when it is ready.
docker_unavailable_reason() {
    local image="$1"
    if ! command -v docker >/dev/null 2>&1; then
        printf 'docker CLI not found (needs image %s)' "$image"
        return 0
    fi
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        printf 'Docker image %s not present locally (ci_local never pulls; run: docker pull %s)' \
            "$image" "$image"
        return 0
    fi
    return 0
}

idf_image_for_target() {
    case "$1" in
        esp32c5) printf '%s' "${IDF_DOCKER_IMAGE_C5:-$DEFAULT_IDF_IMAGE}" ;;
        *)       printf '%s' "$DEFAULT_IDF_IMAGE" ;;
    esac
}

# ---------------------------------------------------------------------------
# Stage 1+2: host tests and component host tests
# ---------------------------------------------------------------------------
stage_host_tests() {
    if [ "$SKIP_HOST" -eq 1 ]; then
        diag "host test stages skipped (--skip-host)"
        return 0
    fi

    stage_or_skip "host tests (scripts/test_host.sh)" \
        "$(log_for host-tests)" "" \
        bash "$SCRIPT_DIR/test_host.sh"

    local -a runners=()
    shopt -s nullglob
    runners=("$ROOT"/components/*/test/host/run.sh)
    shopt -u nullglob
    if [ "${#runners[@]}" -eq 0 ]; then
        tap_skip "component host tests" "no components/*/test/host/run.sh found"
        return 0
    fi

    local runner comp label image reason
    for runner in "${runners[@]}"; do
        comp="${runner#"$ROOT"/components/}"
        comp="${comp%%/*}"
        label="component host test: $comp"
        if [ ! -f "$runner" ]; then
            tap_skip "$label" "runner missing"
            continue
        fi
        reason=""
        if runner_needs_docker "$runner"; then
            image="$(runner_docker_image "$runner")"
            if [ "$SKIP_DOCKER" -eq 1 ]; then
                reason="needs Docker image $image (--skip-docker)"
            elif [ "$DRY_RUN" -eq 0 ]; then
                reason="$(docker_unavailable_reason "$image")"
            fi
        fi
        stage_or_skip "$label" "$(log_for "component-$comp")" "$reason" bash "$runner"
    done
}

# ---------------------------------------------------------------------------
# Stage 3: static contract gates
# ---------------------------------------------------------------------------
stage_web_gates() {
    local reason=""

    if [ ! -f "$ROOT/web/dist/index.html" ] && [ ! -f "$ROOT/web/dist-gz/index.html.gz" ]; then
        reason="no Web UI build found (web/dist, web/dist-gz); run '(cd web && bash build.sh)' first"
    elif [ ! -d "$ROOT/project_components/html" ]; then
        reason="project_components/html missing"
    elif [ ! -f "$SCRIPT_DIR/gen_web_assets.sh" ]; then
        reason="scripts/gen_web_assets.sh missing"
    fi
    stage_or_skip "static gate: embedded Web assets (gen_web_assets.sh --check)" \
        "$(log_for web-assets)" "$reason" \
        bash "$SCRIPT_DIR/gen_web_assets.sh" --check

    reason=""
    if ! command -v node >/dev/null 2>&1; then
        reason="node not found"
    elif [ ! -d "$ROOT/web/node_modules" ]; then
        reason="web/node_modules missing (run: cd web && npm ci)"
    elif [ ! -f "$ROOT/web/scripts/check-i18n.mjs" ]; then
        reason="web/scripts/check-i18n.mjs missing"
    fi
    stage_or_skip "static gate: i18n strict contract (web/scripts/check-i18n.mjs --strict)" \
        "$(log_for i18n)" "$reason" \
        node "$ROOT/web/scripts/check-i18n.mjs" --strict
}

stage_static_gates() {
    local reason=""

    if [ ! -f "$SCRIPT_DIR/check-protocol-version.sh" ]; then
        reason="scripts/check-protocol-version.sh missing"
    fi
    stage_or_skip "static gate: protocol version (C header == docs == Web UI)" \
        "$(log_for protocol-version)" "$reason" \
        bash "$SCRIPT_DIR/check-protocol-version.sh"

    reason=""
    if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
        reason="$PYTHON_BIN not found"
    elif ! command -v gcc >/dev/null 2>&1; then
        reason="gcc not found (host macro dump for the board contract)"
    elif [ ! -f "$SCRIPT_DIR/check_board_pins.py" ]; then
        reason="scripts/check_board_pins.py missing"
    fi
    stage_or_skip "static gate: board pin contract (components/board == docs/board-pins.md)" \
        "$(log_for board-pins)" "$reason" \
        "$PYTHON_BIN" "$SCRIPT_DIR/check_board_pins.py"

    if [ "$RELEASE" -eq 1 ]; then
        reason=""
        if [ ! -f "$SCRIPT_DIR/check-release-versions.sh" ]; then
            reason="scripts/check-release-versions.sh missing"
        fi
        stage_or_skip "static gate: release versions (strict tag/protocol/image gate)" \
            "$(log_for release-versions)" "$reason" \
            bash "$SCRIPT_DIR/check-release-versions.sh"
    else
        diag "release version gate not selected (pass --release to run it)"
    fi

    if [ "$SKIP_WEB" -eq 1 ]; then
        diag "web static gates skipped (--skip-web)"
    else
        stage_web_gates
    fi
}

# ---------------------------------------------------------------------------
# Stage 4: optional Docker build + partition/app-size gate (--build)
# ---------------------------------------------------------------------------
stage_build() {
    local target image reason
    for target in "${BUILD_LIST[@]}"; do
        image="$(idf_image_for_target "$target")"
        reason=""
        if [ "$DRY_RUN" -eq 0 ]; then
            reason="$(docker_unavailable_reason "$image")"
        fi
        if [ -n "$reason" ] && [ "$DRY_RUN" -eq 0 ]; then
            tap_fail "docker build: $target"
            diag "cannot build $target: $reason"
            continue
        fi

        local -a build_cmd=(bash "$SCRIPT_DIR/build_all.sh" -t "$target" --no-check)
        if [ "$RELEASE" -eq 1 ]; then
            build_cmd+=(--release)
        fi
        run_stage "docker build: $target (image $image)" \
            "$(log_for "build-$target")" "${build_cmd[@]}"
        if [ "$DRY_RUN" -eq 0 ] && [ "$last_stage_rc" -ne 0 ]; then
            tap_skip "partition/app-size gate: $target" "build failed"
            continue
        fi
        # per-target gate; build_all.sh --no-check avoids the duplicate aggregate
        # gate, and the explicit --bin keeps the check scoped to this target.
        stage_or_skip "partition/app-size gate: $target" \
            "$(log_for "partitions-$target")" "" \
            "$PYTHON_BIN" "$SCRIPT_DIR/check_partitions.py" \
            --bin "build/$target/birdiebot_wifi_dap.bin" --require-bin
    done
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    printf 'TAP version 13\n'
    diag "ci_local presubmit gate -- root: $ROOT"
    local build_desc="none"
    if [ "${#BUILD_LIST[@]}" -gt 0 ]; then
        build_desc="${BUILD_LIST[*]}"
    fi
    diag "flags: skip_host=$SKIP_HOST skip_web=$SKIP_WEB skip_docker=$SKIP_DOCKER release=$RELEASE dry_run=$DRY_RUN build=$build_desc"
    if [ "$DRY_RUN" -eq 1 ]; then
        diag "dry-run: nothing will be executed"
    else
        diag "logs: ${LOG_DIR#"$ROOT"/}"
    fi

    cd "$ROOT"

    stage_host_tests
    stage_static_gates
    if [ "${#BUILD_LIST[@]}" -gt 0 ]; then
        stage_build
    else
        diag "Docker build stage not selected (pass --build s3|c3|esp32|c5|all)"
    fi

    printf '1..%d\n' "$stage_no"
    local passed=$((stage_no - stage_fail - stage_skip))
    local status="PASS"
    if [ "$stage_fail" -ne 0 ]; then
        status="FAIL"
    elif [ "$DRY_RUN" -eq 1 ]; then
        status="DRY-RUN"
    fi
    diag "summary: stages=$stage_no passed=$passed skipped=$stage_skip failed=$stage_fail status=$status"

    if [ "$DRY_RUN" -eq 0 ]; then
        mkdir -p "$LOG_DIR"
        {
            printf 'ci_local run %s\n' "$RUN_ID"
            printf 'root=%s\n' "$ROOT"
            printf 'flags: skip_host=%s skip_web=%s skip_docker=%s release=%s build=%s\n' \
                "$SKIP_HOST" "$SKIP_WEB" "$SKIP_DOCKER" "$RELEASE" "$build_desc"
            printf 'stages=%d passed=%d skipped=%d failed=%d\n' \
                "$stage_no" "$passed" "$stage_skip" "$stage_fail"
            printf 'status=%s\n' "$status"
        } >"$LOG_DIR/summary.txt"
        ln -sfn "$RUN_ID" "$CI_DIR/latest" 2>/dev/null || true
        diag "logs written: ${LOG_DIR#"$ROOT"/} (build/ci/latest -> $RUN_ID)"
        if [ "$stage_fail" -ne 0 ]; then
            diag "failed stages are listed above as 'not ok'; see their logs under ${LOG_DIR#"$ROOT"/}"
        fi
    fi

    if [ "$status" = "FAIL" ]; then
        exit 1
    fi
    exit 0
}

main "$@"
