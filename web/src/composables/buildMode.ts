/**
 * Build-time feature switches (Vite env, injected by scripts/gen-env.mjs).
 *
 * Trial mode was removed by contract (docs/i18n-keys.md §10.2); do not add it back.
 * The OTA/Update page was removed by contract (docs/feature-flags.md, item A1);
 * remaining flags default to enabled so a plain `npm run build` keeps those UIs.
 */

export function isDevMode() {
    return import.meta.env.VITE_APP_MODE === 'dev';
}

export function isDataFlowEnabled() {
    return import.meta.env.VITE_ENABLE_DATA_FLOW !== 'false';
}

export function isUartEnabled() {
    return import.meta.env.VITE_ENABLE_UART !== 'false';
}
