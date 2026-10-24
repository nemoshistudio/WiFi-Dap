/**
 * Device link-quality telemetry (P0-8).
 *
 * The Web Shell must only display values the firmware or the browser can
 * actually measure. This module is the single place that:
 *
 *  - fetches `GET /api/target/status` (the read-only diagnostics endpoint the
 *    firmware already exposes: flash engine + heap + dap_session_active +
 *    swd_clock_hz + NCM/DHCP fields on ESP32-S3 NCM builds), and
 *  - parses optional link fields an additive firmware revision may publish
 *    (`wifi_rssi`, `dap_latency_*`, ...). Missing fields stay `null`; nothing is
 *    synthesized.
 *
 * The browser-side round-trip time is measured here with `performance.now()`
 * around the same-origin fetch. It is therefore a real (browser-measured) HTTP
 * RTT to the device, not a firmware-reported value and not a fake ping.
 */

import {deviceHttpUrl} from '@/utils/deviceHost';

/** Raw, field-tolerant snapshot of GET /api/target/status (plus optional link fields). */
export interface LinkStatusSnapshot {
    /** true = the endpoint answered with a JSON object; false = 404/network error. */
    available: boolean;
    httpStatus: number;
    /** Browser-measured round trip for this request (ms); null when no response arrived. */
    rttMs: number | null;
    /** Fetch/parse error text ('' on success). */
    err: string;
    /** Flash engine status (same meaning as apiTarget.TargetStatus). */
    state?: string;
    offset?: number;
    total?: number;
    percent?: number;
    errCode?: number;
    /** Additive diagnostics (null = field missing on this firmware). */
    heapFree: number | null;
    heapLargest: number | null;
    dapSessionActive: boolean | null;
    swdClockHz: number | null;
    /** Manual SWD clock selection: 0 = auto (field is always reported). */
    swdClockManualHz: number | null;
    /** USB-NCM link (only present when the firmware is built with NCM). */
    wiredNcmActive: boolean | null;
    wiredDhcpActive: boolean | null;
    wiredDhcpError: number | null;
    wiredDhcpState: number | null;
    wiredIp: string;
    /** Optional link fields an additive firmware revision may report. */
    wifiRssi: number | null;
    wifiConnected: boolean | null;
    wifiSsid: string;
    wifiChannel: number | null;
    /** Firmware DAP transaction statistics (P0-8/E; null when not reported). */
    dapLatencyAvgUs: number | null;
    dapLatencyMaxUs: number | null;
    dapXferLastUs: number | null;
    dapXferCount: number | null;
    dapRingActive: boolean | null;
    dapRingPreallocated: boolean | null;
    dapRingRequestBytes: number | null;
    dapRingResponseBytes: number | null;
    /** Last flash-engine failure text (field `last_error`, omitted when none). */
    lastError: string;
    /** Parsed body (diagnostics JSON only; never used to derive UI fields directly). */
    raw: Record<string, unknown> | null;
}

const STATUS_TIMEOUT_MS = 4000;

export function emptyLinkStatus(): LinkStatusSnapshot {
    return {
        available: false,
        httpStatus: 0,
        rttMs: null,
        err: '',
        heapFree: null,
        heapLargest: null,
        dapSessionActive: null,
        swdClockHz: null,
        swdClockManualHz: null,
        wiredNcmActive: null,
        wiredDhcpActive: null,
        wiredDhcpError: null,
        wiredDhcpState: null,
        wiredIp: '',
        wifiRssi: null,
        wifiConnected: null,
        wifiSsid: '',
        wifiChannel: null,
        dapLatencyAvgUs: null,
        dapLatencyMaxUs: null,
        dapXferLastUs: null,
        dapXferCount: null,
        dapRingActive: null,
        dapRingPreallocated: null,
        dapRingRequestBytes: null,
        dapRingResponseBytes: null,
        lastError: '',
        raw: null,
    };
}

function toNumber(value: unknown): number | undefined {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value;
    }
    if (typeof value === 'string' && value.trim() !== '') {
        const parsed = Number(value);
        return Number.isFinite(parsed) ? parsed : undefined;
    }
    return undefined;
}

function numberOrNull(value: unknown): number | null {
    const parsed = toNumber(value);
    return parsed === undefined ? null : parsed;
}

function booleanOrNull(value: unknown): boolean | null {
    return typeof value === 'boolean' ? value : null;
}

function stringOrEmpty(value: unknown): string {
    return typeof value === 'string' ? value : '';
}

/** Read a nested field without throwing (e.g. `wifi.rssi`). */
function nested(body: Record<string, unknown> | null, path: string): unknown {
    if (!body) {
        return undefined;
    }
    let current: unknown = body;
    for (const key of path.split('.')) {
        if (typeof current !== 'object' || current === null) {
            return undefined;
        }
        current = (current as Record<string, unknown>)[key];
    }
    return current;
}

function firstDefined(body: Record<string, unknown> | null, keys: string[]): unknown {
    for (const key of keys) {
        const value = key.includes('.') ? nested(body, key) : (body ? body[key] : undefined);
        if (value !== undefined && value !== null) {
            return value;
        }
    }
    return undefined;
}

/**
 * GET /api/target/status.
 *
 * Tolerates older firmware (missing additive fields) and newer firmware
 * (additive fields are simply picked up when recognised; unknown ones remain in
 * `raw`). A 404 body is treated as "endpoint unsupported", never as a value.
 */
export async function fetchLinkStatus(): Promise<LinkStatusSnapshot> {
    const snapshot = emptyLinkStatus();
    const controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const timer = controller
        ? setTimeout(() => controller.abort(), STATUS_TIMEOUT_MS)
        : 0;
    const startedAt = typeof performance !== 'undefined' ? performance.now() : Date.now();
    try {
        const response = await fetch(deviceHttpUrl('/api/target/status'), {
            method: 'GET',
            cache: 'no-store',
            signal: controller ? controller.signal : undefined,
        });
        if (typeof performance !== 'undefined') {
            snapshot.rttMs = Math.round(performance.now() - startedAt);
        }
        snapshot.httpStatus = response.status;
        if (!response.ok) {
            // 404 = firmware revision without the optional status endpoint.
            snapshot.err = response.status === 404 ? '' : `HTTP ${response.status}`;
            return snapshot;
        }
        const text = await response.text();
        let body: Record<string, unknown> | null = null;
        if (text) {
            try {
                const parsed = JSON.parse(text) as unknown;
                if (typeof parsed === 'object' && parsed !== null) {
                    body = parsed as Record<string, unknown>;
                }
            } catch {
                body = null;
            }
        }
        if (!body) {
            snapshot.err = 'invalid JSON';
            return snapshot;
        }
        snapshot.available = true;
        snapshot.raw = body;
        snapshot.state = typeof body.state === 'string' ? body.state : undefined;
        snapshot.offset = toNumber(body.offset) ?? toNumber(body.written);
        snapshot.total = toNumber(body.total);
        snapshot.percent = toNumber(body.percent);
        snapshot.errCode = toNumber(body.err);

        snapshot.heapFree = numberOrNull(firstDefined(body, ['heap_free', 'heapFree']));
        snapshot.heapLargest = numberOrNull(firstDefined(body, ['heap_largest', 'heapLargest']));
        snapshot.dapSessionActive = booleanOrNull(firstDefined(body, [
            'dap_session_active', 'dapSessionActive',
        ]));
        // `swd_clock_actual_hz` is the clock the firmware really used (newer
        // revisions); fall back to the legacy last-probe value.
        snapshot.swdClockHz = numberOrNull(firstDefined(body, [
            'swd_clock_actual_hz', 'swd_clock_hz', 'swdClockHz',
        ]));
        snapshot.wiredNcmActive = booleanOrNull(firstDefined(body, [
            'wired_ncm_active', 'ncm_active', 'usb_ncm_active',
        ]));
        snapshot.wiredDhcpActive = booleanOrNull(firstDefined(body, [
            'wired_dhcp_active', 'dhcp_active', 'ncm_dhcp_active',
        ]));
        snapshot.wiredDhcpError = numberOrNull(firstDefined(body, [
            'wired_dhcp_error', 'dhcp_error', 'ncm_dhcp_error',
        ]));
        snapshot.wiredDhcpState = numberOrNull(firstDefined(body, [
            'wired_dhcp_state', 'dhcp_state', 'ncm_dhcp_state',
        ]));
        snapshot.wiredIp = stringOrEmpty(firstDefined(body, [
            'wired_ip', 'ncm_ip', 'usb_ncm_ip',
        ]));
        snapshot.wifiRssi = numberOrNull(firstDefined(body, [
            'wifi_rssi', 'rssi', 'wifi.rssi', 'link.wifi.rssi',
        ]));
        snapshot.wifiConnected = booleanOrNull(firstDefined(body, [
            'wifi_connected', 'sta_connected', 'wifi.connected', 'link.wifi.connected',
        ]));
        snapshot.swdClockManualHz = numberOrNull(firstDefined(body, [
            'swd_clock_manual_hz', 'swd_clock_manual',
        ]));
        snapshot.wifiSsid = stringOrEmpty(firstDefined(body, ['wifi_ssid', 'wifi.ssid']));
        snapshot.wifiChannel = numberOrNull(firstDefined(body, ['wifi_channel', 'wifi.channel']));
        snapshot.dapLatencyAvgUs = numberOrNull(firstDefined(body, [
            'dap_xfer_avg_us', 'dap_latency_avg_us', 'dap_latency_us_avg', 'dap_txn_avg_us',
            'dap.latency_avg_us', 'dap.avg_us',
        ]));
        snapshot.dapLatencyMaxUs = numberOrNull(firstDefined(body, [
            'dap_xfer_max_us', 'dap_latency_max_us', 'dap_latency_us_max', 'dap_txn_max_us',
            'dap.latency_max_us', 'dap.max_us',
        ]));
        snapshot.dapXferLastUs = numberOrNull(firstDefined(body, [
            'dap_xfer_last_us', 'dap_last_us',
        ]));
        snapshot.dapXferCount = numberOrNull(firstDefined(body, [
            'dap_xfer_count', 'dap_txn_count',
        ]));
        snapshot.dapRingActive = booleanOrNull(firstDefined(body, ['dap_ring_active']));
        snapshot.dapRingPreallocated = booleanOrNull(firstDefined(body, [
            'dap_ring_preallocated',
        ]));
        snapshot.dapRingRequestBytes = numberOrNull(firstDefined(body, [
            'dap_ring_request_bytes',
        ]));
        snapshot.dapRingResponseBytes = numberOrNull(firstDefined(body, [
            'dap_ring_response_bytes',
        ]));
        snapshot.lastError = stringOrEmpty(body.last_error);
        return snapshot;
    } catch (error) {
        if (typeof performance !== 'undefined') {
            snapshot.rttMs = Math.round(performance.now() - startedAt);
        }
        const aborted = error instanceof Error && error.name === 'AbortError';
        snapshot.err = aborted ? 'timeout' : (error instanceof Error ? error.message : 'network error');
        return snapshot;
    } finally {
        if (timer) {
            clearTimeout(timer);
        }
    }
}
