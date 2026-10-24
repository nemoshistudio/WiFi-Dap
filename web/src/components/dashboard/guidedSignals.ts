/**
 * Guided-flow auto-advance signals (P1-18).
 *
 * The first-run guide must advance from *real* events instead of a "I did it"
 * checkbox. This module is the single observable signal bus shared by the app
 * shell, the target page and optional integrations:
 *
 *  - The target page emits `target-detected` / `flash-success` directly after a
 *    successful detection / verified flash (it owns those actions).
 *  - The curve page (out of this task's write scope) publishes
 *    `window.__dapCommandIndex` + the `dap:command-index` event after ELF
 *    parsing; a non-empty index is a real "ELF loaded" signal. Empty arrays
 *    reset the signal (ELF cleared) instead of keeping a stale "done" state.
 *  - Sampling start is not observable from the current curve page revision.
 *    Future versions can dispatch `dap:sample-start`; if nothing arrives the
 *    guide marks the step as "manual confirmation required" (explicit button),
 *    never as auto-done.
 *
 * Signals are session state, not persisted: on reload the guide re-evaluates
 * real events. Only the user's explicit manual confirmations are persisted by
 * GuidedFlow itself.
 */

import {reactive} from 'vue';
import {COMMAND_INDEX_EVENT} from '@/utils/commandIndex';

export type GuidedSignalKind = 'target-detected' | 'elf-loaded' | 'sampling-started' | 'flash-success';

export interface GuidedSignalState {
    /** Timestamp of the last real signal per step; null = not observed. */
    lastAt: Record<GuidedSignalKind, number | null>;
    /** Optional human-readable source for diagnostics (never shown as fake data). */
    source: Partial<Record<GuidedSignalKind, string>>;
}

export const guidedSignalState = reactive<GuidedSignalState>({
    lastAt: {
        'target-detected': null,
        'elf-loaded': null,
        'sampling-started': null,
        'flash-success': null,
    },
    source: {},
});

/** Generic window event other modules (App / future pages) can dispatch. */
export const GUIDED_SIGNAL_EVENT = 'bb:guided-signal';

export function emitGuidedSignal(kind: GuidedSignalKind, source = ''): void {
    guidedSignalState.lastAt[kind] = Date.now();
    if (source) {
        guidedSignalState.source[kind] = source;
    }
}

export function resetGuidedSignal(kind: GuidedSignalKind): void {
    guidedSignalState.lastAt[kind] = null;
    delete guidedSignalState.source[kind];
}

/** Reset the session-observable signals (used by "重置引导"). */
export function resetAllGuidedSignals(): void {
    for (const kind of Object.keys(guidedSignalState.lastAt) as GuidedSignalKind[]) {
        resetGuidedSignal(kind);
    }
}

let bridgeStarted = false;

/** Register once from App.vue: listens for real page/window events. */
export function ensureGuidedSignalBridge(): void {
    if (bridgeStarted || typeof window === 'undefined') {
        return;
    }
    bridgeStarted = true;

    // ELF loaded: the curve page publishes its parsed index (real data).
    window.addEventListener(COMMAND_INDEX_EVENT, (event: Event) => {
        const detail = (event as CustomEvent<{variables?: unknown[]; functions?: unknown[]} | undefined>).detail;
        const raw = detail !== undefined ? detail : window.__dapCommandIndex;
        const variables = raw && typeof raw === 'object' && Array.isArray((raw as {variables?: unknown[]}).variables)
            ? (raw as {variables: unknown[]}).variables
            : [];
        const functions = raw && typeof raw === 'object' && Array.isArray((raw as {functions?: unknown[]}).functions)
            ? (raw as {functions: unknown[]}).functions
            : [];
        if (variables.length > 0 || functions.length > 0) {
            emitGuidedSignal('elf-loaded', 'dap:command-index');
        } else {
            // ELF cleared/unloaded: do not keep a stale auto-done state.
            resetGuidedSignal('elf-loaded');
        }
    });

    // Future/optional curve-page events (harmless when never dispatched).
    const simpleEvents: Array<[string, GuidedSignalKind, boolean]> = [
        ['dap:elf-loaded', 'elf-loaded', false],
        ['dap:sample-start', 'sampling-started', false],
        ['dap:sampling-start', 'sampling-started', false],
        ['dap:flash-success', 'flash-success', false],
        ['bb:target-detected', 'target-detected', false],
        ['bb:flash-success', 'flash-success', false],
    ];
    for (const [eventName, kind, reset] of simpleEvents) {
        window.addEventListener(eventName, () => {
            if (reset) {
                resetGuidedSignal(kind);
            } else {
                emitGuidedSignal(kind, eventName);
            }
        });
    }

    // Generic envelope: new CustomEvent('bb:guided-signal', {detail:{kind}})
    window.addEventListener(GUIDED_SIGNAL_EVENT, (event: Event) => {
        const detail = (event as CustomEvent<{kind?: string; source?: string} | undefined>).detail;
        const kind = detail?.kind;
        if (kind === 'target-detected' || kind === 'elf-loaded'
            || kind === 'sampling-started' || kind === 'flash-success') {
            emitGuidedSignal(kind, detail?.source || GUIDED_SIGNAL_EVENT);
        }
    });

    // Pick up an index that was published before the bridge registered.
    if (window.__dapCommandIndex !== undefined) {
        const raw = window.__dapCommandIndex as {variables?: unknown[]; functions?: unknown[]} | null;
        const count = (raw && Array.isArray(raw.variables) ? raw.variables.length : 0)
            + (raw && Array.isArray(raw.functions) ? raw.functions.length : 0);
        if (count > 0) {
            emitGuidedSignal('elf-loaded', 'window.__dapCommandIndex');
        }
    }
}
