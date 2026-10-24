/**
 * Shared device-host helpers for the Web Shell (dashboard / discovery / status).
 *
 * Production: the page was served by the device, so every HTTP call is
 * same-origin. `npm run dev` has no device origin, so the explicit
 * `VITE_DEVICE_HOST_NAME` override is used instead (same convention as
 * App.vue / apiDap.ts). Never fabricate a host: callers show "unknown" when the
 * value is empty.
 */

/** Device host used for the WebSocket/HTTP API (host[:port]). */
export function resolveDeviceHost(): string {
    if (import.meta.env.VITE_APP_MODE === 'dev') {
        return import.meta.env.VITE_DEVICE_HOST_NAME || 'dap.local';
    }
    if (typeof window === 'undefined') {
        return '';
    }
    return window.location.host;
}

/** Turn an API path into a URL that works in both prod (same origin) and dev. */
export function deviceHttpUrl(path: string): string {
    if (import.meta.env.VITE_APP_MODE === 'dev') {
        const host = import.meta.env.VITE_DEVICE_HOST_NAME;
        if (host) {
            return `http://${host.replace(/^https?:\/\//, '')}${path}`;
        }
    }
    return path;
}

/**
 * Normalize a user-supplied device address for manual add / device switching:
 * strip scheme and path, keep optional `:port`, reject spaces. Returns '' when
 * the input cannot be a host (the UI then reports an invalid-address error).
 */
export function normalizeDeviceHost(input: string): string {
    const raw = input.trim();
    if (!raw || /\s/.test(raw)) {
        return '';
    }
    const withoutScheme = raw.replace(/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//, '');
    const hostPart = withoutScheme.split('/')[0];
    if (!hostPart) {
        return '';
    }
    // IPv4[:port], [IPv6][:port] or hostname[:port]
    if (/^\[[0-9a-fA-F:]+\](:\d{1,5})?$/.test(hostPart)) {
        return hostPart;
    }
    if (/^(\d{1,3}\.){3}\d{1,3}(:\d{1,5})?$/.test(hostPart)) {
        const octets = hostPart.split(':')[0].split('.').map((part) => Number(part));
        if (octets.every((part) => Number.isInteger(part) && part >= 0 && part <= 255)) {
            return hostPart;
        }
        return '';
    }
    if (/^([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)*[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(:\d{1,5})?$/.test(hostPart)) {
        return hostPart;
    }
    return '';
}

/** True when a host string points at the wired USB-NCM device subnet. */
export function isWiredNcmHost(host: string): boolean {
    const hostname = host.replace(/:\d+$/, '');
    return /^192\.168\.7\.\d{1,3}$/.test(hostname);
}
