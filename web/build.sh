#!/usr/bin/env bash
#
# WiFi Dap Web UI build (WP-131).
#
# Output (consumed by scripts/gen_web_assets.sh / WP-122, do NOT write
# project_components/html directly):
#   web/dist/     raw Vite output (single-file index.html + ws.sharedworker.js)
#   web/dist-gz/  every dist file gzip -9 compressed, same relative path + .gz
#
# Optional environment:
#   SKIP_NPM_CI=1         reuse an existing node_modules (local iteration only)
#   VITE_ENABLE_OTA=true  enable the OTA local-upload UI for this build (default: off).
#                         scripts/gen-env.mjs passes VITE_* process env to Vite, so the
#                         Update page / nav entry / OTA WS module follow this flag at
#                         build time. Firmware must implement OTA(5) + POST /api/ota/upload.

set -euo pipefail

cd "$(dirname "$0")"

# Contract checks owned by WP-132 (docs/i18n-keys.md §7.2). --strict 意味着
# 页面级硬编码 CJK / 法文残留 / 动态拼接 key 也作为失败项。
if [[ -f scripts/check-i18n.mjs ]]; then
    echo "[build] running i18n contract check (--strict)"
    node scripts/check-i18n.mjs --strict
else
    echo "[build] ERROR: scripts/check-i18n.mjs missing (WP-132 contract check required)" >&2
    exit 1
fi

if [[ "${SKIP_NPM_CI:-0}" != "1" ]]; then
    echo "[build] npm ci"
    # Upstream package-lock.json (D 9c6db50) is missing optional platform
    # packages (esbuild/lightningcss), which makes npm ci fail on npm >= 10.
    # Refresh the lock once, then retry the clean install. A corrected lock is
    # allowed to be committed (WP-131).
    if ! npm ci --no-audit --no-fund; then
        echo "[build] npm ci failed; refreshing package-lock.json and retrying"
        npm install --package-lock-only --no-audit --no-fund
        npm ci --no-audit --no-fund
    fi
fi

echo "[build] npm run build (vue-tsc type-check + vite build)"
npm run build

if [[ ! -f dist/index.html ]]; then
    echo "[build] ERROR: dist/index.html was not produced" >&2
    exit 1
fi
if [[ ! -f dist/ws.sharedworker.js ]]; then
    echo "[build] WARNING: dist/ws.sharedworker.js not found; check vite worker output naming" >&2
fi

echo "[build] gzip -9 -> dist-gz/"
rm -rf dist-gz
mkdir -p dist-gz
while IFS= read -r -d '' file; do
    rel="${file#dist/}"
    mkdir -p "dist-gz/$(dirname "$rel")"
    gzip -9 -n -c "$file" > "dist-gz/${rel}.gz"
done < <(find dist -type f ! -name '*.gz' -print0)

echo "[build] artifacts:"
find dist dist-gz -maxdepth 1 -type f -printf '  %-32p %10s bytes\n' | sort
echo "[build] done"
