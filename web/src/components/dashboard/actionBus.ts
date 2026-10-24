/**
 * Tiny same-document action bus shared by the command palette and the pages.
 *
 * The palette lives in the app shell (App.vue) but some of its actions belong
 * to a page (detect target / reset target / open the first-run guide).  Instead
 * of reaching into page internals, the palette navigates to the page and then
 * publishes an action here; the mounted page consumes it.  Actions requested
 * while no listener is mounted are queued and delivered to the next listener
 * registration (drained once).
 *
 * This is deliberately not a store: the payloads are one-shot commands, not
 * application state.
 */

export type BbAction =
    | 'target-detect'
    | 'target-reset'
    | 'guided-open'
    | 'diagnostics-open'
    | 'devices-open'

type BbActionListener = (action: BbAction) => void;

const listeners = new Set<BbActionListener>();
let pending: BbAction[] = [];

/** Publish an action to the currently mounted page (or queue it). */
export function dispatchAction(action: BbAction): void {
    if (listeners.size === 0) {
        pending.push(action);
        return;
    }
    for (const listener of [...listeners]) {
        try {
            listener(action);
        } catch (error) {
            if (import.meta.env.DEV) {
                console.warn('[actionBus] listener failed', action, error);
            }
        }
    }
}

/** Register a page listener; queued actions are delivered immediately. */
export function onAction(listener: BbActionListener): () => void {
    listeners.add(listener);
    if (pending.length > 0) {
        const queued = pending;
        pending = [];
        for (const action of queued) {
            try {
                listener(action);
            } catch (error) {
                if (import.meta.env.DEV) {
                    console.warn('[actionBus] queued listener failed', action, error);
                }
            }
        }
    }
    return () => {
        listeners.delete(listener);
    };
}

/** Window event used by the navbar trigger and other shell controls. */
export const PALETTE_OPEN_EVENT = 'bb:palette-open';

/** Ask the mounted command palette to open (event is harmless if not mounted). */
export function openCommandPalette(): void {
    window.dispatchEvent(new CustomEvent(PALETTE_OPEN_EVENT));
}
