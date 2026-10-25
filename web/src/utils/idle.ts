/**
 * Startup helpers for non-first-screen work (P1-16).
 *
 * The Web UI is shipped as a single-file build, so route/module code splitting
 * is intentionally not used. Instead, non-critical *work* (ELF-index bridges,
 * favicon fetch, ...) is deferred until after the first paint via
 * first paint via `requestIdleCallback`, with a timeout fallback for browsers
 * without it. This keeps the initial HTTP/WS burst limited to what the first
 * screen actually needs.
 */

type IdleTask = () => void;

/** Run a task when the browser is idle (or after `timeoutMs` at the latest). */
export function runWhenIdle(task: IdleTask, timeoutMs = 2000): void {
    if (typeof window === 'undefined') {
        return;
    }
    const globalWindow = window as Window & {
        requestIdleCallback?: (callback: () => void, options?: { timeout: number }) => number;
    };
    if (typeof globalWindow.requestIdleCallback === 'function') {
        try {
            globalWindow.requestIdleCallback(() => task(), {timeout: timeoutMs});
            return;
        } catch {
            // Fall through to the timer fallback.
        }
    }
    window.setTimeout(() => task(), 0);
}

/** Two rAFs: run after the next painted frame (used for after-first-paint work). */
export function runAfterPaint(task: IdleTask): void {
    if (typeof window === 'undefined') {
        return;
    }
    window.requestAnimationFrame(() => {
        window.requestAnimationFrame(() => task());
    });
}
