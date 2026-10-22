import { FAVICON_32 } from "@/assets/brand/brand-assets";

/**
 * Apply the WiFi Dap brand favicon at runtime.
 *
 * The favicon is already inlined in web/index.html for the first paint (the
 * firmware serves a single self-contained index.html, so it cannot be an
 * external file). This keeps the historical `changeFavicon()` hook working and
 * gives future dynamic favicons (e.g. status-colored) one place to change it.
 */
export function changeFavicon() {
    const link = document.getElementById('favicon');
    if (!(link instanceof HTMLLinkElement)) {
        return;
    }
    link.type = 'image/png';
    link.href = FAVICON_32;
}
