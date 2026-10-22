#!/usr/bin/env bash
# Host-side regression test for the curve sampler JSON parser + expression
# evaluator (components/curve_sampler/src/curve_json.c / curve_expr.c).
#
# Runs on the build host, no target hardware / no firmware flash needed.
# cJSON is taken from the ESP-IDF Docker image so the exact library version the
# firmware uses is tested; esp_err.h is stubbed under ./mock.
#
# Usage: components/curve_sampler/test/host/run.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd "$HERE/../../../.." && pwd -P)"
IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.2.1}"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

mkdir -p "$BUILD_DIR/cjson"
# --entrypoint cat: bypass the ESP-IDF image entrypoint banner on stdout.
docker run --rm --entrypoint cat "$IMAGE" \
    /opt/esp/idf/components/json/cJSON/cJSON.c >"$BUILD_DIR/cjson/cJSON.c"
docker run --rm --entrypoint cat "$IMAGE" \
    /opt/esp/idf/components/json/cJSON/cJSON.h >"$BUILD_DIR/cjson/cJSON.h"

gcc -std=gnu11 -Wall -Wextra -O2 \
    -I "$HERE/mock" \
    -I "$BUILD_DIR/cjson" \
    -I "$ROOT/components/curve_sampler/src" \
    "$HERE/test_curve.c" \
    "$ROOT/components/curve_sampler/src/curve_json.c" \
    "$ROOT/components/curve_sampler/src/curve_expr.c" \
    "$ROOT/components/curve_sampler/src/curve_ring.c" \
    "$BUILD_DIR/cjson/cJSON.c" \
    -lm -o "$BUILD_DIR/test_curve"

"$BUILD_DIR/test_curve"
