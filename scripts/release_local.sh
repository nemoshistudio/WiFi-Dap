#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# release_local.sh -- assemble a local WiFi Dap release package:
# firmware .bin files (app + bootloader + partition table + merged full image),
# the embedded Web UI gz assets, a SHA256SUMS manifest and a RELEASE-INFO file.
#
# Everything is written below build/release/ (gitignored). This script NEVER
# pushes, tags, renames remotes or triggers CI: it is the local equivalent of
# the old GitHub release job (INTEGRATION-PLAN v4 §8.4).
#
# Contract: docs/versioning.md §2.2/§4.4/§8/§9 and docs/ota-local-upload.md §1.3
# (first flash = bootloader + partition table + ota_0 app; otadata stays 0xFF,
# therefore ota_data_initial.bin is deliberately NOT part of the merged image).
#
# Usage:
#   scripts/release_local.sh                       # strict: clean tree + vX.Y.Z tag
#   scripts/release_local.sh --allow-dirty         # local/dev package (no tag needed)
#   scripts/release_local.sh --skip-build --skip-web --allow-dirty
#
# Exit: 0 package created, 1 gate failed, 2 usage error.
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
readonly SCRIPT_NAME="${0##*/}"
readonly IDF_IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.2.1}"

TARGET="esp32s3"
VERSION=""
OUT="$ROOT/build/release"
ALLOW_DIRTY=0
SKIP_BUILD=0
SKIP_WEB=0
WEB_DIST=""
WEB_SRC=""
MAKE_TAR=1
MAKE_MERGE=1

usage() {
    cat <<'EOF'
Usage: release_local.sh [options]

  -t, --target T     esp32s3 (default/release) | esp32c3 | esp32
      --version V    firmware version override (default: embedded esp_app_desc.version)
      --out DIR      package root (default: <root>/build/release; must stay under build/)
      --allow-dirty  allow a dirty worktree / missing vX.Y.Z tag (local dev package)
      --skip-build   reuse the existing build/<target> artifacts
      --skip-web     do not regenerate gz assets (use project_components/html as-is)
      --web-src D    pre-gzipped Web UI assets for gen_web_assets.sh (default web/dist-gz)
      --web-dist D   raw Web UI build output for gen_web_assets.sh (default web/dist)
      --no-merge     skip the merged full-flash image (esptool merge_bin)
      --no-tar       do not create the .tar.gz archive
  -h, --help         show this help

The package contains: firmware/{app,bootloader,partition-table,full}.bin,
firmware/flash.sh, web/{index.html.gz,ws.sharedworker.js.gz,web_version.txt},
SHA256SUMS, RELEASE-INFO.txt. Never pushes or touches git remotes.
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
    printf '[release_local] %s\n' "$*"
}
warn() {
    printf '%s: warning: %s\n' "$SCRIPT_NAME" "$*" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -t|--target)
            [ "$#" -ge 2 ] || usage_die "$1 requires an argument"
            TARGET="$2"
            shift 2
            ;;
        --version)
            [ "$#" -ge 2 ] || usage_die "--version requires an argument"
            VERSION="$2"
            shift 2
            ;;
        --version=*)
            VERSION="${1#*=}"
            shift
            ;;
        --out)
            [ "$#" -ge 2 ] || usage_die "--out requires an argument"
            OUT="$2"
            shift 2
            ;;
        --web-dist)
            [ "$#" -ge 2 ] || usage_die "--web-dist requires an argument"
            WEB_DIST="$2"
            shift 2
            ;;
        --web-src)
            [ "$#" -ge 2 ] || usage_die "--web-src requires an argument"
            WEB_SRC="$2"
            shift 2
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --skip-web)
            SKIP_WEB=1
            shift
            ;;
        --no-merge)
            MAKE_MERGE=0
            shift
            ;;
        --no-tar)
            MAKE_TAR=0
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

case "$TARGET" in
    esp32s3) CHIP="esp32s3"; BUILD_REL="build/esp32s3"; PART_CSV="partitions/partitions_8mb_ota.csv" ;;
    esp32c3) CHIP="esp32c3"; BUILD_REL="build/esp32c3"; PART_CSV="partitions/partitions_4mb_single.csv" ;;
    esp32)   CHIP="esp32";   BUILD_REL="build/esp32";   PART_CSV="partitions/partitions_4mb_single.csv" ;;
    *) usage_die "unsupported target '$TARGET' (esp32s3|esp32c3|esp32)" ;;
esac
BUILD_ABS="$ROOT/$BUILD_REL"
BIN_ABS="$BUILD_ABS/birdiebot_wifi_dap.bin"

command -v git >/dev/null 2>&1 || die "git is required"
command -v python3 >/dev/null 2>&1 || die "python3 is required"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is required"

# Output dir must stay inside the gitignored build/ tree.
case "$OUT" in
    /*) ;;
    *) OUT="$ROOT/$OUT" ;;
esac
OUT="$(realpath -m "$OUT")"
case "$OUT" in
    "$ROOT"/build/*) ;;
    *) die "output dir must live under $ROOT/build (got $OUT)" ;;
esac

# --- strict release gates -------------------------------------------------
if [ "$ALLOW_DIRTY" -eq 0 ]; then
    if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
        die "worktree is dirty; release requires a clean tree (use --allow-dirty for a local/dev package)"
    fi
    exact="$(git -C "$ROOT" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null)" \
        || die "HEAD is not exactly on a vX.Y.Z tag; release requires a tag (use --allow-dirty for a local/dev package)"
    log "release tag: $exact"
else
    warn "--allow-dirty: building a LOCAL/DEV package (no tag/clean-tree/version-equality gate)"
fi

# --- protocol version gate (docs/versioning.md §3.1/§8.2) -----------------
# C header == docs/protocol.md == web/src/api/index.ts. Release builds refuse a
# one-sided protocol bump; --allow-dirty only warns (local/dev package).
if [ -x "$SCRIPT_DIR/check-protocol-version.sh" ]; then
    if ! _proto_out="$("$SCRIPT_DIR/check-protocol-version.sh" 2>&1)"; then
        if [ "$ALLOW_DIRTY" -eq 1 ]; then
            warn "protocol version gate failed (allowed with --allow-dirty):"
            printf '%s\n' "$_proto_out" >&2
        else
            printf '%s\n' "$_proto_out" >&2
            die "protocol version gate failed (C header / docs / Web UI); fix before release"
        fi
    else
        log "protocol version gate: ok"
    fi
else
    warn "scripts/check-protocol-version.sh missing; protocol gate skipped"
fi

# --- build ----------------------------------------------------------------
if [ "$SKIP_BUILD" -eq 1 ]; then
    log "reusing $BUILD_REL (--skip-build)"
else
    log "building target $TARGET via scripts/build_all.sh"
    build_args=(-t "$TARGET")
    [ "$ALLOW_DIRTY" -eq 1 ] || build_args+=(--release)
    [ -n "$VERSION" ] && build_args+=(--fw-version "$VERSION")
    "$SCRIPT_DIR/build_all.sh" "${build_args[@]}"
fi
[ -f "$BIN_ABS" ] || die "missing app image $BIN_ABS (build target $TARGET first)"

# --- firmware version -----------------------------------------------------
read_app_desc() {
    python3 - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
if len(data) < 0x50 or data[0] != 0xE9:
    sys.exit("not an ESP-IDF app image")
if struct.unpack_from("<I", data, 0x20)[0] != 0xABCD5432:
    sys.exit("esp_app_desc magic_word mismatch")
version = struct.unpack_from("<32s", data, 0x30)[0].split(b"\0", 1)[0].decode("ascii", "replace")
if not version:
    sys.exit("empty esp_app_desc.version (PROJECT_VER not injected)")
print(version)
PY
}
embedded_version="$(read_app_desc "$BIN_ABS")" || die "cannot read embedded version from $BIN_ABS"
if [ -n "$VERSION" ] && [ "$VERSION" != "$embedded_version" ]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        warn "requested version '$VERSION' != embedded '$embedded_version' (allowed with --allow-dirty)"
    else
        die "requested version '$VERSION' != embedded esp_app_desc.version '$embedded_version'"
    fi
fi
[ -n "$VERSION" ] || VERSION="$embedded_version"
if ! "$SCRIPT_DIR/check-version-string.sh" $([ "$ALLOW_DIRTY" -eq 1 ] && echo --allow-dirty) "$VERSION" >/dev/null; then
    die "invalid firmware version '$VERSION'"
fi
[ "$ALLOW_DIRTY" -eq 1 ] || "$SCRIPT_DIR/check-version-string.sh" "$VERSION" >/dev/null || die "release version check failed: $VERSION"
log "firmware version: $VERSION (embedded esp_app_desc.version=$embedded_version)"

# --- web assets -----------------------------------------------------------
HTML_ABS="$ROOT/project_components/html"
if [ "$SKIP_WEB" -eq 1 ]; then
    log "reusing embedded web assets in project_components/html (--skip-web)"
else
    # Prerequisite (audit F3): the Web UI must be built before the release
    # pipeline can verify/package its gzip assets.
    web_src_eff="${WEB_SRC:-$ROOT/web/dist-gz}"
    web_dist_eff="${WEB_DIST:-$ROOT/web/dist}"
    case "$web_src_eff" in /*) ;; *) web_src_eff="$ROOT/$web_src_eff" ;; esac
    case "$web_dist_eff" in /*) ;; *) web_dist_eff="$ROOT/$web_dist_eff" ;; esac
    if [ ! -d "$web_src_eff" ] && [ ! -d "$web_dist_eff" ]; then
        die "Web UI build outputs missing: expected $web_src_eff or $web_dist_eff.
       Prerequisite: build the Web UI first, e.g.  (cd web && bash build.sh)
       then run:  scripts/gen_web_assets.sh --install   (explicit install)
       Or package the committed assets with --skip-web."
    fi
    # Never silently overwrite the committed html assets: only verify them and
    # tell the user to run gen_web_assets.sh --install explicitly if stale.
    gen_args=(--check)
    [ -n "$WEB_DIST" ] && gen_args+=(--dist "$WEB_DIST")
    [ -n "$WEB_SRC" ] && gen_args+=(--src "$WEB_SRC")
    if "$SCRIPT_DIR/gen_web_assets.sh" "${gen_args[@]}"; then
        log "embedded web assets verified against the current Web UI build"
    else
        die "installed project_components/html does not match the current Web UI build; run scripts/gen_web_assets.sh --install explicitly (or pass --skip-web to package the current assets)"
    fi
fi
for f in index.html.gz ws.sharedworker.js.gz; do
    [ -f "$HTML_ABS/$f" ] || die "missing embedded web asset: project_components/html/$f"
done
# Audit F4: docs/versioning.md §4.4 requires the committed web_version.txt for a
# release package (source of gz hashes + protocol version). Release mode fails;
# local/dev mode warns loudly and records the fallback in RELEASE-INFO.
if [ ! -f "$HTML_ABS/web_version.txt" ]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        warn "project_components/html/web_version.txt is missing: gz-hash verification is limited and RELEASE-INFO uses legacy version.txt metadata (allowed with --allow-dirty)"
    else
        die "project_components/html/web_version.txt is missing (docs/versioning.md §4.4); run scripts/gen_web_assets.sh --install on the release commit"
    fi
fi
web_version="unknown"
if [ -f "$HTML_ABS/web_version.txt" ]; then
    web_version="$(sed -n 's/^web_version=//p' "$HTML_ABS/web_version.txt" | head -n1)"
fi
if [ -z "$web_version" ] || [ "$web_version" = "unknown" ]; then
    # WP-122 commits a version.txt with a `web-build: { "web_version": ... }`
    # JSON blob; docs/versioning.md §4.4 web_version.txt is the canonical file,
    # so accept either when packaging.
    if [ -f "$HTML_ABS/version.txt" ]; then
        web_version="$(python3 - "$HTML_ABS/version.txt" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r'"web_version"\s*:\s*"([^"]+)"', text)
if m:
    print(m.group(1).strip())
else:
    print(text.strip().splitlines()[-1].lstrip("v") if text.strip() else "unknown")
PY
)"
    fi
fi
[ -n "$web_version" ] || web_version="unknown"

# Web protocol version for RELEASE-INFO: canonical web_version.txt first, then
# the legacy version.txt JSON blob imported from the WP-122 UI build.
protocol_printed="$(sed -n 's/^protocol_version=//p' "$HTML_ABS/web_version.txt" 2>/dev/null | head -n1 || true)"
if [ -z "$protocol_printed" ] && [ -f "$HTML_ABS/version.txt" ]; then
    protocol_printed="$(grep -oE '"protocol_version"[[:space:]]*:[[:space:]]*[0-9]+' "$HTML_ABS/version.txt" 2>/dev/null | head -n1 | grep -oE '[0-9]+$' || true)"
fi
[ -n "$protocol_printed" ] || protocol_printed='?'
web_metadata="web_version.txt"
[ -f "$HTML_ABS/web_version.txt" ] || web_metadata="version.txt (legacy fallback; web_version.txt missing)"

if [ "$ALLOW_DIRTY" -eq 0 ] && [ "v$web_version" != "$VERSION" ]; then
    die "web_version.txt web_version='$web_version' != firmware version '$VERSION' (docs/versioning.md §4.1)"
fi
[ "$ALLOW_DIRTY" -eq 1 ] && [ "v$web_version" != "$VERSION" ] && warn "web version '$web_version' != firmware '$VERSION' (allowed with --allow-dirty)"

# --- partition / app-size / version gate ---------------------------------
log "running partition + app-size gate"
python3 "$SCRIPT_DIR/check_partitions.py" --bin "$BIN_ABS" --require-bin

# --- package skeleton -----------------------------------------------------
PKG_NAME="birdiebot-wifi-dap-$VERSION"
PKG="$OUT/$PKG_NAME"
log "package dir: $PKG"
case "$PKG" in
    "$ROOT"/build/*) ;;
    *) die "refusing to remove unexpected path: $PKG" ;;
esac
rm -rf "$PKG"
mkdir -p "$PKG/firmware" "$PKG/web"

cp "$BIN_ABS" "$PKG/firmware/birdiebot_wifi_dap.bin"
cp "$BUILD_ABS/bootloader/bootloader.bin" "$PKG/firmware/bootloader.bin"
cp "$BUILD_ABS/partition_table/partition-table.bin" "$PKG/firmware/partition-table.bin"
log "copied app ($(stat -c%s "$PKG/firmware/birdiebot_wifi_dap.bin") bytes), bootloader, partition table"

# --- merged full-flash image (bootloader + ptable + app only) -------------
boot_off="0x0"
[ "$CHIP" = "esp32" ] && boot_off="0x1000"
if [ "$MAKE_MERGE" -eq 1 ]; then
    if command -v docker >/dev/null 2>&1; then
        pkg_rel="build/release/$PKG_NAME"
        merge_cmd="esptool.py --chip $CHIP merge_bin -o '$pkg_rel/firmware/birdiebot_wifi_dap_full.bin' \
            $boot_off '$pkg_rel/firmware/bootloader.bin' \
            0x8000 '$pkg_rel/firmware/partition-table.bin' \
            0x20000 '$pkg_rel/firmware/birdiebot_wifi_dap.bin'"
        log "merging full-flash image (bootloader@$boot_off, ptable@0x8000, app@0x20000; otadata left 0xFF)"
        if docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
                -v "$ROOT:/project" -w /project "$IDF_IMAGE" bash -c "$merge_cmd"; then
            log "merged image: $(stat -c%s "$PKG/firmware/birdiebot_wifi_dap_full.bin") bytes"
        elif [ "$ALLOW_DIRTY" -eq 1 ]; then
            warn "esptool merge_bin failed; continuing without merged image (--allow-dirty)"
        else
            die "esptool merge_bin failed (release requires the merged flash image)"
        fi
    elif [ "$ALLOW_DIRTY" -eq 1 ]; then
        warn "docker not available; skipping merged image (--allow-dirty)"
    else
        die "docker is required to merge the full-flash image"
    fi
else
    log "skipping merged image (--no-merge)"
fi

cat >"$PKG/firmware/flash.sh" <<EOF
#!/usr/bin/env bash
# First-flash helper for WiFi Dap $VERSION ($CHIP, $TARGET).
# Writes bootloader + partition table + ota_0 app only. otadata is left 0xFF
# so the IDF bootloader falls back to ota_0 on first boot
# (docs/ota-local-upload.md §1.3). Never flashes a merged image over otadata.
set -euo pipefail
PORT="\${1:-/dev/ttyUSB0}"
HERE="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd -P)"
esptool.py --chip $CHIP -p "\$PORT" --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size keep \
    $boot_off "\$HERE/bootloader.bin" \
    0x8000 "\$HERE/partition-table.bin" \
    0x20000 "\$HERE/birdiebot_wifi_dap.bin"
EOF
chmod +x "$PKG/firmware/flash.sh"

# --- embedded web assets --------------------------------------------------
cp "$HTML_ABS/index.html.gz" "$PKG/web/index.html.gz"
cp "$HTML_ABS/ws.sharedworker.js.gz" "$PKG/web/ws.sharedworker.js.gz"
[ -f "$HTML_ABS/web_version.txt" ] && cp "$HTML_ABS/web_version.txt" "$PKG/web/web_version.txt"
[ -f "$HTML_ABS/version.txt" ] && cp "$HTML_ABS/version.txt" "$PKG/web/version.txt"
if [ -f "$PKG/web/web_version.txt" ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            index_gz_sha256) [ "$(sha256sum "$PKG/web/index.html.gz" | awk '{print $1}')" = "$value" ] || die "index.html.gz hash mismatch vs web_version.txt" ;;
            worker_gz_sha256) [ "$(sha256sum "$PKG/web/ws.sharedworker.js.gz" | awk '{print $1}')" = "$value" ] || die "ws.sharedworker.js.gz hash mismatch vs web_version.txt" ;;
        esac
    done <"$PKG/web/web_version.txt"
    log "embedded web assets verified (index/worker gz sha256 match web_version.txt)"
fi

# --- release metadata + checksums ----------------------------------------
app_sha="$(sha256sum "$PKG/firmware/birdiebot_wifi_dap.bin" | awk '{print $1}')"
git_commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
git_describe="$(git -C "$ROOT" describe --tags --dirty --always 2>/dev/null || echo unknown)"
mode="$([ "$ALLOW_DIRTY" -eq 1 ] && echo 'local-dev' || echo 'release')"
full_size="n/a"
[ -f "$PKG/firmware/birdiebot_wifi_dap_full.bin" ] && full_size="$(stat -c%s "$PKG/firmware/birdiebot_wifi_dap_full.bin")"
{
    printf 'project=WiFi Dap\n'
    printf 'firmware_version=%s\n' "$VERSION"
    printf 'target=%s\n' "$TARGET"
    printf 'chip=%s\n' "$CHIP"
    printf 'mode=%s\n' "$mode"
    printf 'git_commit=%s\n' "$git_commit"
    printf 'git_describe=%s\n' "$git_describe"
    printf 'build_time=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'partition_table=%s\n' "$PART_CSV"
    printf 'app_bin=birdiebot_wifi_dap.bin\n'
    printf 'app_size_bytes=%s\n' "$(stat -c%s "$PKG/firmware/birdiebot_wifi_dap.bin")"
    printf 'app_sha256=%s\n' "$app_sha"
    printf 'full_bin_size_bytes=%s\n' "$full_size"
    printf 'web_version=%s\n' "$web_version"
    printf 'web_protocol_version=%s\n' "$protocol_printed"
    printf 'web_metadata=%s\n' "$web_metadata"
    if [ -f "$HTML_ABS/web_version.txt" ]; then
        grep -E '^(git_tag|git_commit|index_gz_sha256|worker_gz_sha256)=' "$HTML_ABS/web_version.txt" | sed 's/^/web_/' || true
    fi
    printf 'note=local package only; never pushed (INTEGRATION-PLAN v4 §8.4)\n'
} >"$PKG/RELEASE-INFO.txt"
log "wrote RELEASE-INFO.txt"

(
    cd "$PKG"
    find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum >SHA256SUMS
    sha256sum -c --quiet SHA256SUMS
)
log "wrote SHA256SUMS ($(wc -l <"$PKG/SHA256SUMS") files, verified)"

tar_path=""
if [ "$MAKE_TAR" -eq 1 ]; then
    tar_path="$OUT/$PKG_NAME.tar.gz"
    tar -C "$OUT" -czf "$tar_path" "$PKG_NAME"
    log "archive: $tar_path ($(stat -c%s "$tar_path") bytes)"
fi

protocol_printed="$(sed -n 's/^protocol_version=//p' "$HTML_ABS/web_version.txt" 2>/dev/null | head -n1 || true)"
if [ -z "$protocol_printed" ] && [ -f "$HTML_ABS/version.txt" ]; then
    # WP-122 UI build records protocol_version in the version.txt JSON blob.
    protocol_printed="$(grep -oE '"protocol_version"[[:space:]]*:[[:space:]]*[0-9]+' "$HTML_ABS/version.txt" 2>/dev/null | head -n1 | grep -oE '[0-9]+$' || true)"
fi
[ -n "$protocol_printed" ] || protocol_printed='?'
printf '\n[release_local] ===== local release package =====\n'
printf 'version : %s (%s)\n' "$VERSION" "$mode"
printf 'target  : %s (%s flash, %s)\n' "$TARGET" "$([ "$TARGET" = esp32s3 ] && echo 8MB || echo 4MB)" "$PART_CSV"
printf 'dir     : %s\n' "$PKG"
[ -n "$tar_path" ] && printf 'archive : %s\n' "$tar_path"
printf 'app bin : %s bytes sha256=%s\n' "$(stat -c%s "$PKG/firmware/birdiebot_wifi_dap.bin")" "$app_sha"
printf 'full bin: %s bytes\n' "$full_size"
printf 'web     : web_version=%s (protocol=%s)\n' "$web_version" "$protocol_printed"
printf 'NOTE    : local only - never pushed, no git tag/remote changes (see RELEASE-INFO.txt)\n'
