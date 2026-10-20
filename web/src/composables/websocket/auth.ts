/**
 * Web auth token helpers (docs/protocol.md §1, docs/ota-local-upload.md).
 *
 * Auth is disabled by default (`BIRDIEBOT_WEB_AUTH=n`), so when no token is
 * present nothing changes in the WS handshake. When auth is enabled the pairing
 * code may arrive either as `?token=<pair_code>` on the page URL or from a
 * previous login stored in localStorage.
 */

export const AUTH_TOKEN_STORAGE_KEY = 'birdiebot.token';

const MAX_TOKEN_LENGTH = 256;

/** Trim and sanity-check a pairing code (empty string means "no token"). */
export function normalizeAuthToken(value: unknown): string {
    if (typeof value !== 'string') {
        return '';
    }
    const token = value.trim();
    if (!token || token.length > MAX_TOKEN_LENGTH) {
        return '';
    }
    return token;
}

/**
 * Resolve the token used for this page session:
 * 1. `?token=` from the URL (also persisted for reconnects/reloads);
 * 2. `localStorage['birdiebot.token']` written by a previous login.
 * Storage failures (private mode / disabled storage) are non-fatal.
 */
export function resolveAuthToken(): string {
    let urlToken = '';
    try {
        urlToken = normalizeAuthToken(new URLSearchParams(window.location.search).get('token'));
    } catch {
        urlToken = '';
    }

    try {
        if (urlToken) {
            localStorage.setItem(AUTH_TOKEN_STORAGE_KEY, urlToken);
            return urlToken;
        }
        return normalizeAuthToken(localStorage.getItem(AUTH_TOKEN_STORAGE_KEY));
    } catch {
        return urlToken;
    }
}

/**
 * Build the `/ws` URL, appending `?token=` only when a token exists.
 * 设备默认以 HTTP 提供页面（ws://）；页面若被反代到 HTTPS 则自动用 wss://。
 */
export function buildWebSocketUrl(host: string, token: string): string {
    const secure = typeof window !== 'undefined' && window.location.protocol === 'https:';
    const base = `${secure ? 'wss' : 'ws'}://${host}/ws`;
    if (!token) {
        return base;
    }
    return `${base}?token=${encodeURIComponent(token)}`;
}

/** First-frame auth request: `{"module":0,"cmd":0,"token":"..."}`. */
export function buildAuthRequest(token: string): string {
    return JSON.stringify({module: 0, cmd: 0, token});
}

/**
 * HTTP 鉴权头（docs/protocol.md §1 / docs/ota-local-upload.md §8）。
 * 无 token（默认实验室模式）时不附加任何头，请求与未开鉴权固件兼容；
 * 开鉴权后 OTA 上传等 /api 请求必须带 `Authorization: Bearer <pair_code>`。
 */
export function buildAuthHeaders(): Record<string, string> {
    const token = resolveAuthToken();
    return token ? {Authorization: `Bearer ${token}`} : {};
}

/** 当前页面是否已有配对码（用于 UI 提示“实验室模式/已登录”）。 */
export function hasAuthToken(): boolean {
    return resolveAuthToken().length > 0;
}
