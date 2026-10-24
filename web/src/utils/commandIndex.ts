/**
 * Command palette ELF index bridge (P1-17).
 *
 * Web-curve owns ELF parsing and publishes the currently loaded ELF via:
 *
 *   window.__dapCommandIndex = {
 *     variables: [{name, path, address, type}],   // address: u32 number
 *     functions: [{name, address}],
 *   }
 *   window.dispatchEvent(new CustomEvent('dap:command-index', {detail: index}))
 *
 * `path` is the DebugCurve variable-tree unique key (qualified name, possibly
 * de-duplicated with `@0x..`); it is the value the curve page matches
 * `?focusVar=<path>` against. This module never writes the index (web-curve
 * owns it) and tolerates missing/empty indexes: the palette then simply shows
 * no variable/function section.
 *
 * Optional `window.__dapCommandIndexVersion` (number, bumped on every update)
 * is used for cheap de-duplication if present.
 */

import {shallowReactive} from 'vue';

export const COMMAND_INDEX_EVENT = 'dap:command-index';
/** Fallback event the curve page may listen to (per web-curve contract). */
export const FOCUS_VARIABLE_EVENT = 'dap:focus-variable';

export interface CommandIndexVariable {
    name: string;
    path: string;
    address: number;
    type: string;
}

export interface CommandIndexFunction {
    name: string;
    address: number;
}

export interface CommandIndexSnapshot {
    available: boolean;
    variables: CommandIndexVariable[];
    functions: CommandIndexFunction[];
    updatedAt: number | null;
    version: number;
}

declare global {
    interface Window {
        /** Published by DebugCurve after ELF parsing (read-only for the shell). */
        __dapCommandIndex?: unknown;
        __dapCommandIndexVersion?: number;
        /** Published by DebugCurve: number of parsed variables (optional). */
        __dapCommandIndexCount?: number;
    }
}

export const commandIndexState = shallowReactive<CommandIndexSnapshot>({
    available: false,
    variables: [],
    functions: [],
    updatedAt: null,
    version: 0,
});

function asRecord(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

function asString(value: unknown): string {
    return typeof value === 'string' ? value : '';
}

function asAddress(value: unknown): number {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value >>> 0;
    }
    if (typeof value === 'string') {
        const trimmed = value.trim();
        if (/^0x[0-9a-f]+$/i.test(trimmed)) {
            return Number.parseInt(trimmed.slice(2), 16) >>> 0;
        }
        const parsed = Number(trimmed);
        if (Number.isFinite(parsed)) {
            return parsed >>> 0;
        }
    }
    return 0;
}

/** Tolerant normalization of the curve-page index (missing fields are dropped). */
export function normalizeCommandIndex(raw: unknown): CommandIndexSnapshot {
    const record = asRecord(raw);
    const variablesRaw = record && Array.isArray(record.variables) ? record.variables : [];
    const functionsRaw = record && Array.isArray(record.functions) ? record.functions : [];
    const variableSeen = new Set<string>();
    const variables: CommandIndexVariable[] = [];
    for (const item of variablesRaw) {
        const entry = asRecord(item);
        if (!entry) {
            continue;
        }
        const name = asString(entry.name);
        const path = asString(entry.path) || name;
        if (!path || variableSeen.has(path)) {
            continue;
        }
        variableSeen.add(path);
        variables.push({
            name: name || path,
            path,
            address: asAddress(entry.address),
            type: asString(entry.type),
        });
    }
    const functionSeen = new Set<string>();
    const functions: CommandIndexFunction[] = [];
    for (const item of functionsRaw) {
        const entry = asRecord(item);
        if (!entry) {
            continue;
        }
        const name = asString(entry.name);
        if (!name || functionSeen.has(name)) {
            continue;
        }
        functionSeen.add(name);
        functions.push({name, address: asAddress(entry.address)});
    }
    return {
        available: variables.length > 0 || functions.length > 0,
        variables,
        functions,
        updatedAt: Date.now(),
        version: typeof window !== 'undefined' && typeof window.__dapCommandIndexVersion === 'number'
            ? window.__dapCommandIndexVersion
            : 0,
    };
}

function applyIndex(raw: unknown): void {
    const next = normalizeCommandIndex(raw);
    // Skip identical updates (the curve page may re-dispatch on unrelated renders).
    if (next.available === commandIndexState.available
        && next.version === commandIndexState.version
        && next.variables.length === commandIndexState.variables.length
        && next.functions.length === commandIndexState.functions.length
        && next.updatedAt !== null
        && commandIndexState.updatedAt !== null
        && next.variables[0]?.path === commandIndexState.variables[0]?.path) {
        return;
    }
    commandIndexState.available = next.available;
    commandIndexState.variables = next.variables;
    commandIndexState.functions = next.functions;
    commandIndexState.updatedAt = next.updatedAt;
    commandIndexState.version = next.version;
}

let bridgeStarted = false;

/** Subscribe once (App.vue / CommandPalette mount): event + initial window value. */
export function ensureCommandIndexBridge(): void {
    if (bridgeStarted || typeof window === 'undefined') {
        return;
    }
    bridgeStarted = true;
    window.addEventListener(COMMAND_INDEX_EVENT, (event: Event) => {
        const detail = (event as CustomEvent<unknown>).detail;
        applyIndex(detail !== undefined ? detail : window.__dapCommandIndex);
    });
    if (window.__dapCommandIndex !== undefined) {
        applyIndex(window.__dapCommandIndex);
    }
}

/** Current index object (for the palette; reactive state is preferred in UI). */
export function readCommandIndex(): unknown {
    return typeof window === 'undefined' ? undefined : window.__dapCommandIndex;
}

export interface IndexMatch<T> {
    item: T;
    score: number;
}

function scoreText(haystack: string, needle: string): number {
    const index = haystack.indexOf(needle);
    if (index < 0) {
        return -1;
    }
    // Prefer prefix/name matches over matches deep inside a path.
    return index === 0 ? 100 : 40 - Math.min(index, 30);
}

/** Search variables by name/path/type (bounded result count). */
export function searchVariables(
    variables: CommandIndexVariable[],
    query: string,
    limit = 8,
): CommandIndexVariable[] {
    const needle = query.trim().toLowerCase();
    if (needle.length < 2 || variables.length === 0) {
        return [];
    }
    const matches: IndexMatch<CommandIndexVariable>[] = [];
    for (const variable of variables) {
        const name = variable.name.toLowerCase();
        const path = variable.path.toLowerCase();
        const score = Math.max(
            scoreText(name, needle),
            scoreText(path, needle) > 0 ? scoreText(path, needle) - 10 : -1,
            variable.type.toLowerCase().includes(needle) ? 25 : -1,
        );
        if (score >= 0) {
            matches.push({item: variable, score});
            // Keep the scan bounded for huge ELF indexes (reference ~ 2e4 rows).
            if (matches.length >= limit * 8) {
                break;
            }
        }
    }
    matches.sort((a, b) => b.score - a.score || a.item.name.localeCompare(b.item.name));
    return matches.slice(0, limit).map((match) => match.item);
}

/** Search functions by name (bounded result count). */
export function searchFunctions(
    functions: CommandIndexFunction[],
    query: string,
    limit = 6,
): CommandIndexFunction[] {
    const needle = query.trim().toLowerCase();
    if (needle.length < 2 || functions.length === 0) {
        return [];
    }
    const matches: IndexMatch<CommandIndexFunction>[] = [];
    for (const fn of functions) {
        const score = scoreText(fn.name.toLowerCase(), needle);
        if (score >= 0) {
            matches.push({item: fn, score});
            if (matches.length >= limit * 8) {
                break;
            }
        }
    }
    matches.sort((a, b) => b.score - a.score || a.item.name.localeCompare(b.item.name));
    return matches.slice(0, limit).map((match) => match.item);
}

/** `0x0800xxxx` display helper (register/stack variables may report 0). */
export function formatCommandAddress(address: number): string {
    if (!address) {
        return '';
    }
    return `0x${(address >>> 0).toString(16).toUpperCase().padStart(8, '0')}`;
}

/** Ask the curve page to focus a variable (query + optional fallback event). */
export function focusVariable(path: string): void {
    if (typeof window === 'undefined') {
        return;
    }
    window.dispatchEvent(new CustomEvent(FOCUS_VARIABLE_EVENT, {detail: {path}}));
}
