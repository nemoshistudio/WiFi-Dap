/**
 * Device discovery + multi-device bookkeeping (P1-19).
 *
 * Backend contract (P1-19): `GET /api/discover` should return nearby DAP
 * devices. The current firmware revision may not implement it, so this module
 * degrades gracefully:
 *
 *   - 404 / network error -> `supported = false` / `error` set; the UI keeps
 *     showing only the device that served this page, previously used devices
 *     and manually added devices (persisted in localStorage). It never invents
 *     "nearby" devices or mDNS results.
 *   - success -> the returned list is shown as-is (name/IP/target/version/RSSI
 *     are only displayed when the field is present in the response).
 *
 * "Switch device" opens the other device's own Web UI (`http://<host>/#/target`)
 * after an explicit user click. The page never changes host network
 * configuration, routes or interfaces.
 */

import {reactive} from 'vue';
import {deviceHttpUrl, normalizeDeviceHost} from '@/utils/deviceHost';

export type DeviceSource = 'current' | 'manual' | 'recent' | 'discovered';

/** A device row shown in the dashboard / used for switching. */
export interface DeviceRecord {
    /** Stable id: `host:<host[:port]>` (also the localStorage key). */
    id: string;
    /** `host[:port]` used to open the device Web UI. */
    host: string;
    /** Display name: manual name, alias or host. */
    name: string;
    source: DeviceSource;
    /** Optional fields, only when the backend really reported them. */
    target: string;
    version: string;
    rssi: number | null;
    lastSeenAt: number | null;
    /** Manual entry can be renamed/removed; discovered rows only keep an alias. */
    removable: boolean;
}

interface SavedDevice {
    host: string;
    name: string;
    addedAt: number;
}

interface PersistedDevices {
    saved: SavedDevice[];
    aliases: Record<string, string>;
    /** Last successfully opened device host (for the "recent" row). */
    recent: string[];
}

export interface DiscoveryState {
    /** null = not probed yet; false = firmware has no /api/discover (graceful). */
    supported: boolean | null;
    loading: boolean;
    /** HTTP 429: device busy with another mDNS query; transient, not "unsupported". */
    busy: boolean;
    /** Discovered rows (memory only; never persisted as fake list). */
    devices: DeviceRecord[];
    error: string;
    updatedAt: number | null;
    saved: SavedDevice[];
    aliases: Record<string, string>;
    recent: string[];
}

const STORAGE_KEY = 'birdiebot.devices.v1';
const DISCOVER_TIMEOUT_MS = 3000;

function loadPersisted(): PersistedDevices {
    const fallback: PersistedDevices = {saved: [], aliases: {}, recent: []};
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) {
            return fallback;
        }
        const parsed = JSON.parse(raw) as Partial<PersistedDevices>;
        const saved = Array.isArray(parsed.saved)
            ? parsed.saved.filter((item): item is SavedDevice => (
                typeof item === 'object' && item !== null
                && typeof (item as SavedDevice).host === 'string'
                && (item as SavedDevice).host.length > 0
            )).map((item) => ({
                host: item.host,
                name: typeof item.name === 'string' ? item.name : '',
                addedAt: typeof item.addedAt === 'number' ? item.addedAt : 0,
            }))
            : [];
        const aliases: Record<string, string> = {};
        if (typeof parsed.aliases === 'object' && parsed.aliases !== null) {
            for (const [key, value] of Object.entries(parsed.aliases)) {
                if (typeof value === 'string' && value.length > 0) {
                    aliases[key] = value;
                }
            }
        }
        const recent = Array.isArray(parsed.recent)
            ? parsed.recent.filter((item): item is string => typeof item === 'string').slice(0, 8)
            : [];
        return {saved, aliases, recent};
    } catch {
        return fallback;
    }
}

function persist(state: DiscoveryState): void {
    try {
        localStorage.setItem(STORAGE_KEY, JSON.stringify({
            saved: state.saved,
            aliases: state.aliases,
            recent: state.recent,
        }));
    } catch {
        // Private mode: discovery/aliases stay session-only.
    }
}

const persisted = loadPersisted();

export const discoveryState = reactive<DiscoveryState>({
    supported: null,
    loading: false,
    busy: false,
    devices: [],
    error: '',
    updatedAt: null,
    saved: persisted.saved,
    aliases: persisted.aliases,
    recent: persisted.recent,
});

/** Current page host (the device serving this page); '' outside a browser. */
export function currentDeviceHost(): string {
    if (typeof window === 'undefined') {
        return '';
    }
    return window.location.host;
}

function asRecord(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

function asString(value: unknown): string {
    return typeof value === 'string' ? value.trim() : '';
}

function asNumberOrNull(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value;
    }
    if (typeof value === 'string' && value.trim() !== '') {
        const parsed = Number(value);
        return Number.isFinite(parsed) ? parsed : null;
    }
    return null;
}

/** Extract the device array from the tolerant `GET /api/discover` body. */
function extractDeviceArray(body: unknown): unknown[] {
    if (Array.isArray(body)) {
        return body;
    }
    const record = asRecord(body);
    if (!record) {
        return [];
    }
    for (const key of ['devices', 'list', 'items', 'nearby', 'nodes']) {
        if (Array.isArray(record[key])) {
            return record[key] as unknown[];
        }
    }
    return [];
}

/**
 * Map one backend device entry. Entries without a usable host are skipped
 * (they cannot be opened/switched to), which is reported via the returned null.
 */
function parseDeviceEntry(entry: unknown): DeviceRecord | null {
    const record = asRecord(entry);
    if (!record) {
        return null;
    }
    const rawHost = asString(record.ip) || asString(record.address) || asString(record.host)
        || asString(record.hostname) || asString(record.mdns) || asString(record.name);
    const port = asNumberOrNull(record.port);
    let host = normalizeDeviceHost(rawHost);
    if (!host) {
        return null;
    }
    if (port !== null && port > 0 && port <= 65535 && !/:(\d{1,5})$/.test(host)) {
        host = `${host}:${Math.trunc(port)}`;
    }
    const name = asString(record.name) || asString(record.hostname) || asString(record.mdns) || host;
    return {
        id: `host:${host}`,
        host,
        name,
        source: 'discovered',
        target: asString(record.target) || asString(record.target_name) || asString(record.chip),
        version: asString(record.version) || asString(record.fw_version) || asString(record.firmware)
            || asString(record.web_version),
        rssi: asNumberOrNull(record.rssi) ?? asNumberOrNull(record.signal),
        lastSeenAt: Date.now(),
        removable: false,
    };
}

/**
 * GET /api/discover; tolerant to 404/network errors (never throws).
 *
 * The current firmware contract (fw-core2, task-60) has no discovery endpoint,
 * so a session that already saw a 404 will not probe again on every page mount;
 * an explicit user "refresh discovery" (`force`) still retries once.
 */
export async function refreshDiscoveredDevices(options: { force?: boolean } = {}): Promise<void> {
    if (discoveryState.loading) {
        return;
    }
    if (!options.force && discoveryState.supported === false) {
        return;
    }
    discoveryState.loading = true;
    discoveryState.error = '';
    discoveryState.busy = false;
    const controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const timer = controller ? setTimeout(() => controller.abort(), DISCOVER_TIMEOUT_MS) : 0;
    try {
        const response = await fetch(deviceHttpUrl('/api/discover'), {
            method: 'GET',
            cache: 'no-store',
            signal: controller ? controller.signal : undefined,
        });
        if (response.status === 404) {
            // Firmware revision without discovery: graceful manual/recent mode.
            discoveryState.supported = false;
            discoveryState.devices = [];
            return;
        }
        if (response.status === 429) {
            // docs/discover.md §1: one mDNS query at a time; busy is transient.
            // Keep `supported` and the last list, never report "unsupported".
            discoveryState.busy = true;
            discoveryState.error = 'HTTP 429';
            return;
        }
        if (!response.ok) {
            // 403 = Host/Origin guard (normal same-origin page never hits it) and
            // other HTTP errors: keep the last known list, surface the error.
            discoveryState.error = `HTTP ${response.status}`;
            return;
        }
        const text = await response.text();
        let body: unknown = null;
        if (text) {
            try {
                body = JSON.parse(text) as unknown;
            } catch {
                body = null;
            }
        }
        const bodyRecord = asRecord(body);
        if (bodyRecord && bodyRecord.ok === false) {
            discoveryState.supported = true;
            if (bodyRecord.code === 6) {
                // BUSY code even on a 200 envelope: transient, keep last list.
                discoveryState.busy = true;
                discoveryState.error = 'HTTP 429';
            } else {
                discoveryState.error = asString(bodyRecord.err) || 'discover failed';
                discoveryState.devices = [];
            }
            return;
        }
        const entries = extractDeviceArray(body);
        const devices = entries
            .map((entry) => parseDeviceEntry(entry))
            .filter((item): item is DeviceRecord => item !== null);
        discoveryState.supported = true;
        discoveryState.busy = false;
        discoveryState.devices = devices;
        discoveryState.updatedAt = Date.now();
    } catch (error) {
        const aborted = error instanceof Error && error.name === 'AbortError';
        discoveryState.error = aborted ? 'timeout' : (error instanceof Error ? error.message : 'network error');
        // supported stays null/known: a transient network error must not be
        // reported as "firmware missing the endpoint".
    } finally {
        discoveryState.loading = false;
        if (timer) {
            clearTimeout(timer);
        }
    }
}

/** Alias for an already-discovered/current host (rename without fake entries). */
export function setDeviceAlias(host: string, alias: string): void {
    const clean = alias.trim();
    if (!host) {
        return;
    }
    if (clean) {
        discoveryState.aliases[host] = clean;
    } else {
        delete discoveryState.aliases[host];
    }
    persist(discoveryState);
}

/** Add a manual device (validated host). Returns false for invalid/duplicate hosts. */
export function addManualDevice(inputHost: string, name = ''): boolean {
    const host = normalizeDeviceHost(inputHost);
    if (!host) {
        return false;
    }
    if (discoveryState.saved.some((item) => item.host === host) || host === currentDeviceHost()) {
        return false;
    }
    discoveryState.saved = [
        ...discoveryState.saved,
        {host, name: name.trim(), addedAt: Date.now()},
    ];
    persist(discoveryState);
    return true;
}

/** Remove a manually added device (discovered rows cannot be removed). */
export function removeManualDevice(host: string): void {
    discoveryState.saved = discoveryState.saved.filter((item) => item.host !== host);
    persist(discoveryState);
}

/** Rename a saved manual device (alias for discovered/current rows). */
export function renameManualDevice(host: string, name: string): void {
    const clean = name.trim();
    if (!clean) {
        return;
    }
    let found = false;
    discoveryState.saved = discoveryState.saved.map((item) => {
        if (item.host === host) {
            found = true;
            return {...item, name: clean};
        }
        return item;
    });
    if (!found) {
        setDeviceAlias(host, clean);
        return;
    }
    persist(discoveryState);
}

export function deviceDisplayName(host: string): string {
    return preferredDeviceName(host, host);
}

/** Alias/manual name if present, else the backend-provided fallback name. */
function preferredDeviceName(host: string, fallback: string): string {
    const saved = discoveryState.saved.find((item) => item.host === host);
    if (saved?.name) {
        return saved.name;
    }
    return discoveryState.aliases[host] || fallback;
}

/**
 * All devices the UI knows about, deduped by host and ordered:
 * current page -> discovered -> manual -> recent.
 */
export function knownDevices(): DeviceRecord[] {
    const byHost = new Map<string, DeviceRecord>();
    const current = currentDeviceHost();
    if (current) {
        byHost.set(current, {
            id: `host:${current}`,
            host: current,
            name: deviceDisplayName(current),
            source: 'current',
            target: '',
            version: '',
            rssi: null,
            // The current page has no "last seen" concept; do not fabricate one.
            lastSeenAt: null,
            removable: false,
        });
    }
    for (const device of discoveryState.devices) {
        if (!byHost.has(device.host)) {
            // Prefer the backend-provided name; a local alias/rename overrides it.
            byHost.set(device.host, {
                ...device,
                name: preferredDeviceName(device.host, device.name || device.host),
            });
        }
    }
    for (const item of discoveryState.saved) {
        if (!byHost.has(item.host)) {
            byHost.set(item.host, {
                id: `host:${item.host}`,
                host: item.host,
                name: preferredDeviceName(item.host, item.name || item.host),
                source: 'manual',
                target: '',
                version: '',
                rssi: null,
                lastSeenAt: item.addedAt || null,
                removable: true,
            });
        }
    }
    for (const host of discoveryState.recent) {
        if (!byHost.has(host)) {
            byHost.set(host, {
                id: `host:${host}`,
                host,
                name: preferredDeviceName(host, host),
                source: 'recent',
                target: '',
                version: '',
                rssi: null,
                lastSeenAt: null,
                removable: false,
            });
        }
    }
    return [...byHost.values()];
}

/**
 * Open another device's Web UI on explicit user click. Returns false when the
 * host equals the current page (nothing to switch to) or is not a valid host.
 * Deliberately a browser navigation, not a background network reconfiguration.
 */
export function openDevice(host: string): boolean {
    const normalized = normalizeDeviceHost(host);
    if (!normalized || normalized === currentDeviceHost()) {
        return false;
    }
    const scheme = typeof window !== 'undefined' && window.location.protocol === 'https:' ? 'https' : 'http';
    if (typeof window !== 'undefined') {
        discoveryState.recent = [normalized, ...discoveryState.recent.filter((item) => item !== normalized)].slice(0, 8);
        persist(discoveryState);
        window.location.href = `${scheme}://${normalized}/#/target`;
    }
    return true;
}
