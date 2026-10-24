/**
 * Clipboard helper for the diagnostics / command palette.
 *
 * Plain-HTTP LAN pages are not a secure context, so `navigator.clipboard` is
 * not always available; fall back to the legacy textarea + execCommand path
 * (same approach as TargetFlash.vue, kept local to the new components so the
 * existing page code is untouched).
 */
export async function copyTextToClipboard(text: string): Promise<boolean> {
    try {
        if (navigator.clipboard && window.isSecureContext) {
            await navigator.clipboard.writeText(text);
            return true;
        }
    } catch {
        // Fall through to the legacy path.
    }
    try {
        const area = document.createElement('textarea');
        area.value = text;
        area.setAttribute('readonly', '');
        area.style.position = 'fixed';
        area.style.opacity = '0';
        document.body.appendChild(area);
        area.select();
        const ok = document.execCommand('copy');
        document.body.removeChild(area);
        return ok;
    } catch {
        return false;
    }
}
