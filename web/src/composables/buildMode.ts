/**
 * Build-time feature switches (Vite env, injected by scripts/gen-env.mjs).
 *
 * Trial mode was removed by contract (docs/i18n-keys.md §10.2); do not add it back.
 * Feature flags default to enabled so a plain `npm run build` keeps the full UI.
 */

export function isDevMode() {
    return import.meta.env.VITE_APP_MODE === 'dev';
}

/**
 * OTA page/module switch.
 *
 * Enabled by default (P0 web platform): `npm run build` ships the OTA page and
 * registers the OTA WS module. A trimmed build can opt out explicitly with
 * `VITE_ENABLE_OTA=false` (docs/versioning.md §6, docs/ota-local-upload.md).
 */
export function isOTAEnabled() {
    return import.meta.env.VITE_ENABLE_OTA !== 'false';
}

export function isDataFlowEnabled() {
    return import.meta.env.VITE_ENABLE_DATA_FLOW !== 'false';
}

export function isUartEnabled() {
    return import.meta.env.VITE_ENABLE_UART !== 'false';
}
