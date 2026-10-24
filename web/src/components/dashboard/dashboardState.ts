/**
 * Shared telemetry for the P0 dashboard / diagnostics / command palette / link health.
 *
 * Data sources (all read-only, no fabricated fields):
 *  - WebSocket state: pinia `useWsStore` (App.vue routes ControlEvent here).
 *  - Protocol handshake: pinia `useSystemStore` (useSystemModule).
 *  - WIFI module: query cmd 1/5/6 and observe responses through the additive
 *    `addJsonMsgListener` in router/msgRouter.  The WIFI module's single
 *    registered handler still belongs to Wifi.vue; this observer never
 *    registers/unregisters a module and therefore cannot break that page.
 *  - Target / link: `GET /api/target/status` snapshots through
 *    `utils/linkMetrics.ts` (flash engine + heap + dap_session_active +
 *    swd_clock_hz + optional USB-NCM/DHCP fields on NCM builds). Firmware
 *    revisions without the endpoint are reported as "unsupported", never with
 *    invented values. The browser-side HTTP round-trip time is measured by the
 *    browser itself around that request.
 *  - Physical link: window.location.hostname (192.168.7.x => wired USB-NCM).
 *
 * The backend may not expose every diagnostic. Missing fields stay `null` and
 * the UI shows "unknown"; the dashboard never invents heap/session/latency.
 */
import { reactive } from 'vue';
import { addCtrlMsgListener, addJsonMsgListener } from '@/router/msgRouter';
import { ControlEvent, ControlMsgType, PROTOCOL_VERSION, WtModuleID, type ApiJsonMsg } from '@/api';
import {
    WifiCmd,
    wifi_ap_get_info,
    wifi_get_mode,
    wifi_sta_get_ap_info,
    type IWifiMode,
    type WifiInfo,
} from '@/api/apiWifi';
import { type TargetInfo, type TargetStatus } from '@/api/apiTarget';
import type { DapKickResult } from '@/api/apiDap';
import { parseDetectionConfidence } from '@/utils/targetProfile';
import { useWsStore } from '@/stores/websocket';
import { useSystemStore } from '@/stores/useSystemStore';
import { deviceHttpUrl, isWiredNcmHost, resolveDeviceHost } from '@/utils/deviceHost';
import { fetchLinkStatus, type LinkStatusSnapshot } from '@/utils/linkMetrics';

export { resolveDeviceHost };
export type { LinkStatusSnapshot };

export type LinkKind = 'wired' | 'wifi' | 'unknown';

export interface LinkInfo {
    kind: LinkKind;
    hostname: string;
    /** true when the page itself was served over the wired USB-NCM subnet. */
    hostIsWired: boolean;
}

export interface WifiTelemetry {
    /** A cmd-1 (STA info) response has been received this session. */
    staKnown: boolean;
    /** true = STA connected, false = STA known-disconnected, null = unknown. */
    staConnected: boolean | null;
    ssid: string;
    rssi: number | null;
    ip: string;
    gateway: string;
    /** A cmd-5 (AP info) response has been received. */
    apKnown: boolean;
    apSsid: string;
    apIp: string;
    /** A cmd-6 (mode/status) response has been received. */
    modeKnown: boolean;
    /** Raw WifiStatus enum from the firmware (0 null / 1 STA / 2 AP / 3 APSTA). */
    status: number | null;
    /** Raw WifiMode enum from the firmware. */
    mode: number | null;
    lastError: string;
    updatedAt: number | null;
}

/** Extended, field-tolerant /api/target/status detail (P0-8). */
export interface TargetStatusDetail {
    /** Endpoint answered with JSON (false = 404/network error). */
    available: boolean;
    httpStatus: number;
    /** Browser-measured RTT of the status request itself (ms). */
    rttMs: number | null;
    state: string;
    offset: number | null;
    total: number | null;
    percent: number | null;
    heapFree: number | null;
    heapLargest: number | null;
    dapSessionActive: boolean | null;
    swdClockHz: number | null;
    /** Manual SWD clock setting from the firmware (0 = auto). */
    swdClockManualHz: number | null;
    wiredNcmActive: boolean | null;
    wiredDhcpActive: boolean | null;
    wiredDhcpError: number | null;
    wiredDhcpState: number | null;
    wiredIp: string;
    /** Optional firmware link fields (null when the revision does not report them). */
    wifiRssi: number | null;
    wifiConnected: boolean | null;
    wifiSsid: string;
    wifiChannel: number | null;
    dapLatencyAvgUs: number | null;
    dapLatencyMaxUs: number | null;
    dapXferLastUs: number | null;
    dapXferCount: number | null;
    dapRingActive: boolean | null;
    dapRingPreallocated: boolean | null;
    dapRingRequestBytes: number | null;
    dapRingResponseBytes: number | null;
    lastFlashError: string;
    error: string;
    updatedAt: number | null;
    /** Consecutive failed status requests (0 = last request succeeded). */
    failures: number;
}

export interface TargetTelemetry {
    probed: boolean;
    info: TargetInfo | null;
    httpStatus: number | null;
    lastError: string;
    probedAt: number | null;
    status: TargetStatus | null;
    statusKnown: boolean;
    statusError: string;
    statusAt: number | null;
    /** Extended status fields (heap / DAP session / SWD clock / NCM / RTT). */
    detail: TargetStatusDetail;
}

export interface DapTelemetry {
    lastKick: DapKickResult | null;
    lastKickAt: number | null;
    lastKickError: string;
}

/** Rolling browser-measured HTTP RTT samples to /api/target/status. */
export interface LinkTelemetry {
    samples: number[];
    last: number | null;
    min: number | null;
    avg: number | null;
    max: number | null;
    updatedAt: number | null;
    /** True while the page-level polling loop is running. */
    polling: boolean;
}

const RTT_WINDOW = 10;

function emptyWifi(): WifiTelemetry {
    return {
        staKnown: false,
        staConnected: null,
        ssid: '',
        rssi: null,
        ip: '',
        gateway: '',
        apKnown: false,
        apSsid: '',
        apIp: '',
        modeKnown: false,
        status: null,
        mode: null,
        lastError: '',
        updatedAt: null,
    };
}

function emptyStatusDetail(): TargetStatusDetail {
    return {
        available: false,
        httpStatus: 0,
        rttMs: null,
        state: '',
        offset: null,
        total: null,
        percent: null,
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
        lastFlashError: '',
        error: '',
        updatedAt: null,
        failures: 0,
    };
}

export const dashboardState = reactive<{
    wifi: WifiTelemetry;
    target: TargetTelemetry;
    dap: DapTelemetry;
    link: LinkTelemetry;
}>({
    wifi: emptyWifi(),
    target: {
        probed: false,
        info: null,
        httpStatus: null,
        lastError: '',
        probedAt: null,
        status: null,
        statusKnown: false,
        statusError: '',
        statusAt: null,
        detail: emptyStatusDetail(),
    },
    dap: {
        lastKick: null,
        lastKickAt: null,
        lastKickError: '',
    },
    link: {
        samples: [],
        last: null,
        min: null,
        avg: null,
        max: null,
        updatedAt: null,
        polling: false,
    },
});

/* ------------------------------------------------------------------------- */
/* Link detection                                                            */
/* ------------------------------------------------------------------------- */

/**
 * Current physical link of the page itself.
 *
 * - 192.168.7.x          -> wired USB-NCM (device 192.168.7.1 / host 192.168.7.2)
 * - dev server / file:// -> unknown (the WS may connect out to another host)
 * - other HTTP hostnames -> WiFi (the device served this page)
 *
 * This is intentionally read-only: the UI never switches networks/routes.
 */
export function getLinkInfo(): LinkInfo {
    if (typeof window === 'undefined') {
        return { kind: 'unknown', hostname: '', hostIsWired: false };
    }
    const hostname = window.location.hostname;
    const protocol = window.location.protocol;
    const hostIsWired = isWiredNcmHost(hostname);
    if (protocol !== 'http:' && protocol !== 'https:') {
        return { kind: 'unknown', hostname, hostIsWired };
    }
    if (hostIsWired) {
        return { kind: 'wired', hostname, hostIsWired: true };
    }
    if (hostname === '' || hostname === 'localhost' || hostname === '127.0.0.1'
        || hostname === '::1' || hostname === '[::1]') {
        return { kind: 'unknown', hostname, hostIsWired: false };
    }
    if (import.meta.env.VITE_APP_MODE === 'dev') {
        return { kind: 'unknown', hostname, hostIsWired: false };
    }
    return { kind: 'wifi', hostname, hostIsWired: false };
}

/* ------------------------------------------------------------------------- */
/* WIFI module observer (additive; does not register the module)              */
/* ------------------------------------------------------------------------- */

function resetWifi(): void {
    Object.assign(dashboardState.wifi, emptyWifi());
}

function applyStaInfo(msg: ApiJsonMsg): void {
    const wifi = dashboardState.wifi;
    wifi.staKnown = true;
    if (msg.err) {
        wifi.lastError = msg.err;
        return;
    }
    const info = msg as WifiInfo;
    // Same convention as Wifi.vue: rssi === 0 means "no STA connection".
    if (info.rssi === 0) {
        wifi.staConnected = false;
        wifi.ssid = '';
        wifi.rssi = null;
        wifi.ip = '';
        wifi.gateway = '';
    } else {
        wifi.staConnected = true;
        wifi.ssid = typeof info.ssid === 'string' ? info.ssid : '';
        wifi.rssi = typeof info.rssi === 'number' ? info.rssi : null;
        wifi.ip = typeof info.ip === 'string' ? info.ip : '';
        wifi.gateway = typeof info.gateway === 'string' ? info.gateway : '';
    }
    wifi.updatedAt = Date.now();
}

function applyApInfo(msg: ApiJsonMsg): void {
    const wifi = dashboardState.wifi;
    wifi.apKnown = true;
    if (msg.err) {
        wifi.lastError = msg.err;
        return;
    }
    const info = msg as WifiInfo;
    wifi.apSsid = typeof info.ssid === 'string' ? info.ssid : '';
    wifi.apIp = typeof info.ip === 'string' ? info.ip : '';
    wifi.updatedAt = Date.now();
}

function applyModeInfo(msg: ApiJsonMsg): void {
    const wifi = dashboardState.wifi;
    wifi.modeKnown = true;
    if (msg.err) {
        wifi.lastError = msg.err;
        return;
    }
    const info = msg as IWifiMode;
    wifi.status = typeof info.status === 'number' ? info.status : null;
    wifi.mode = typeof info.mode === 'number' ? info.mode : null;
    wifi.updatedAt = Date.now();
}

function onWifiJson(msg: ApiJsonMsg): void {
    // Only react to the commands this observer itself requests; Wifi.vue keeps
    // its own handler and may receive scan/saved-list responses too.
    switch (msg.cmd as WifiCmd) {
        case WifiCmd.WIFI_API_JSON_STA_GET_AP_INFO:
            applyStaInfo(msg);
            break;
        case WifiCmd.WIFI_API_JSON_AP_GET_INFO:
            applyApInfo(msg);
            break;
        case WifiCmd.WIFI_API_JSON_GET_MODE:
        case WifiCmd.WIFI_API_JSON_SET_MODE:
            applyModeInfo(msg);
            break;
        default:
            break;
    }
}

/** Query STA info + AP info + AP/STA mode over the existing WebSocket. */
export function refreshWifi(): void {
    const ws = useWsStore();
    if (ws.state !== ControlEvent.CONNECTED) {
        return;
    }
    wifi_sta_get_ap_info();
    wifi_ap_get_info();
    wifi_get_mode();
}

function onTelemetryJson(msg: ApiJsonMsg): void {
    if (msg.module !== WtModuleID.WIFI) {
        return;
    }
    onWifiJson(msg);
}

function onTelemetryCtrl(msg: { type: ControlMsgType; data: ControlEvent | string }): void {
    if (msg.type !== ControlMsgType.WS_EVENT) {
        return;
    }
    if (msg.data === ControlEvent.CONNECTED) {
        refreshWifi();
        // Seed the link dashboard immediately after a (re)connect: parallel with
        // the WS handshake queries instead of waiting for a manual refresh.
        void refreshTargetStatus({ force: true });
        // Resume the page-level link poll if the target page is still mounted.
        if (linkPollingWanted && !linkPollTimer) {
            startLinkPolling();
        }
    } else if (msg.data === ControlEvent.DISCONNECTED) {
        resetWifi();
        // Pause polling while offline but remember that the page wants it, so a
        // reconnect resumes without a manual refresh.
        pauseLinkPolling();
    }
}

let telemetryStarted = false;

/** Register the process-wide observers once (called from App.vue). */
export function ensureDashboardTelemetry(): void {
    if (telemetryStarted) {
        return;
    }
    telemetryStarted = true;
    addJsonMsgListener(onTelemetryJson);
    addCtrlMsgListener(onTelemetryCtrl);
    refreshWifi();
}

/* ------------------------------------------------------------------------- */
/* Target snapshots / link health                                            */
/* ------------------------------------------------------------------------- */

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

function numberOrNull(value: number | undefined | null): number | null {
    return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

/** Record the latest detection result (called by TargetFlash after detect). */
export function recordTargetDetection(info: TargetInfo | null, errorText = ''): void {
    const target = dashboardState.target;
    target.probed = true;
    target.probedAt = Date.now();
    target.info = info;
    target.httpStatus = typeof info?.httpStatus === 'number' ? info.httpStatus : null;
    target.lastError = errorText || info?.err || '';
}

/** Record the latest /api/target/status snapshot (legacy-compatible API). */
export function recordTargetStatus(status: TargetStatus | null, errorText = ''): void {
    const target = dashboardState.target;
    target.status = status;
    target.statusKnown = status !== null;
    target.statusError = errorText;
    target.statusAt = Date.now();
}

function pushRttSample(rttMs: number | null): void {
    const link = dashboardState.link;
    if (rttMs === null) {
        return;
    }
    const samples = [...link.samples, rttMs].slice(-RTT_WINDOW);
    link.samples = samples;
    link.last = rttMs;
    link.min = Math.min(...samples);
    link.max = Math.max(...samples);
    link.avg = Math.round(samples.reduce((sum, value) => sum + value, 0) / samples.length);
    link.updatedAt = Date.now();
}

function applyLinkSnapshot(snapshot: LinkStatusSnapshot): void {
    const target = dashboardState.target;
    const detail = target.detail;
    detail.available = snapshot.available;
    detail.httpStatus = snapshot.httpStatus;
    detail.rttMs = snapshot.rttMs;
    detail.state = snapshot.state ?? '';
    detail.offset = numberOrNull(snapshot.offset);
    detail.total = numberOrNull(snapshot.total);
    detail.percent = numberOrNull(snapshot.percent);
    detail.heapFree = snapshot.heapFree;
    detail.heapLargest = snapshot.heapLargest;
    detail.dapSessionActive = snapshot.dapSessionActive;
    detail.swdClockHz = snapshot.swdClockHz;
    detail.swdClockManualHz = snapshot.swdClockManualHz;
    detail.wiredNcmActive = snapshot.wiredNcmActive;
    detail.wiredDhcpActive = snapshot.wiredDhcpActive;
    detail.wiredDhcpError = snapshot.wiredDhcpError;
    detail.wiredDhcpState = snapshot.wiredDhcpState;
    detail.wiredIp = snapshot.wiredIp;
    detail.wifiRssi = snapshot.wifiRssi;
    detail.wifiConnected = snapshot.wifiConnected;
    detail.wifiSsid = snapshot.wifiSsid;
    detail.wifiChannel = snapshot.wifiChannel;
    detail.dapLatencyAvgUs = snapshot.dapLatencyAvgUs;
    detail.dapLatencyMaxUs = snapshot.dapLatencyMaxUs;
    detail.dapXferLastUs = snapshot.dapXferLastUs;
    detail.dapXferCount = snapshot.dapXferCount;
    detail.dapRingActive = snapshot.dapRingActive;
    detail.dapRingPreallocated = snapshot.dapRingPreallocated;
    detail.dapRingRequestBytes = snapshot.dapRingRequestBytes;
    detail.dapRingResponseBytes = snapshot.dapRingResponseBytes;
    detail.lastFlashError = snapshot.lastError;
    detail.updatedAt = snapshot.available ? Date.now() : detail.updatedAt;
    if (snapshot.available) {
        detail.failures = 0;
        detail.error = '';
        target.statusError = '';
    } else if (snapshot.err) {
        detail.failures += 1;
        detail.error = snapshot.err;
        target.statusError = snapshot.err;
    } else if (snapshot.httpStatus === 404) {
        target.statusError = '';
    }
    // Legacy fields consumed by the diagnostics panel / flash progress checks.
    if (snapshot.available) {
        target.status = {
            state: snapshot.state,
            offset: snapshot.offset,
            total: snapshot.total,
            percent: snapshot.percent,
            err: snapshot.lastError || (snapshot.errCode ? String(snapshot.errCode) : undefined),
            heapFree: snapshot.heapFree ?? undefined,
            heapLargest: snapshot.heapLargest ?? undefined,
            dapSessionActive: snapshot.dapSessionActive ?? undefined,
            swdClockHz: snapshot.swdClockHz ?? undefined,
        };
    } else if (snapshot.httpStatus === 404) {
        target.status = null;
    }
    target.statusKnown = snapshot.available;
    target.statusAt = Date.now();
    pushRttSample(snapshot.rttMs);
}

let statusInFlight: Promise<TargetStatus | null> | null = null;
let statusLastFetchAt = 0;

/** Minimum interval between two status polls (dedupes dashboard/diagnostics refreshes). */
const STATUS_MIN_INTERVAL_MS = 1200;

/**
 * Refresh `GET /api/target/status` with de-duplication.
 *
 * Multiple dashboard cards / pages ask for the same snapshot; concurrent calls
 * share one request and a non-forced call within `STATUS_MIN_INTERVAL_MS` is
 * served from the last snapshot. This is the P1-16 "no duplicate polling" path.
 * `force` is used by explicit user refreshes and the 1 Hz flash-progress loop.
 */
export function refreshTargetStatus(options: { force?: boolean } = {}): Promise<TargetStatus | null> {
    const force = options.force === true;
    if (statusInFlight && !force) {
        return statusInFlight;
    }
    if (!force && statusLastFetchAt > 0 && Date.now() - statusLastFetchAt < STATUS_MIN_INTERVAL_MS) {
        return Promise.resolve(dashboardState.target.status);
    }
    statusLastFetchAt = Date.now();
    const request = (async (): Promise<TargetStatus | null> => {
        const snapshot = await fetchLinkStatus();
        applyLinkSnapshot(snapshot);
        return dashboardState.target.status;
    })();
    statusInFlight = request;
    void request.finally(() => {
        if (statusInFlight === request) {
            statusInFlight = null;
        }
    });
    return request;
}

/** True when the firmware answered /api/target/status at least once. */
export function isTargetStatusAvailable(): boolean {
    return dashboardState.target.detail.available;
}

let linkPollTimer = 0;
let linkPollingWanted = false;

/**
 * Start the single page-level link-health poll (only mounted by the target
 * page; other pages must not poll in the background).
 */
export function startLinkPolling(intervalMs = 12000): void {
    linkPollingWanted = true;
    if (linkPollTimer) {
        return;
    }
    dashboardState.link.polling = true;
    void refreshTargetStatus();
    linkPollTimer = window.setInterval(() => {
        void refreshTargetStatus({ force: true });
    }, intervalMs);
}

/** Clear the timer but keep the "page wants polling" flag (WS reconnect path). */
function pauseLinkPolling(): void {
    if (linkPollTimer) {
        window.clearInterval(linkPollTimer);
        linkPollTimer = 0;
    }
    dashboardState.link.polling = false;
}

export function stopLinkPolling(): void {
    linkPollingWanted = false;
    pauseLinkPolling();
}

/** Explicit dashboard refresh: link status + WiFi queries in parallel. */
export async function refreshDashboard(): Promise<void> {
    refreshWifi();
    await refreshTargetStatus({ force: true });
}

/* ------------------------------------------------------------------------- */
/* Target probe for the diagnostics panel                                    */
/* ------------------------------------------------------------------------- */

function normalizeProbeBody(body: Record<string, unknown> | null, httpStatus: number): TargetInfo {
    if (!body) {
        return { detected: false, httpStatus, err: httpStatus > 0 ? `HTTP ${httpStatus}` : 'network error' };
    }
    const family = typeof body.family === 'string' ? body.family : undefined;
    const idcode = body.idcode;
    const detected = typeof body.detected === 'boolean'
        ? body.detected
        : Boolean(family || idcode);
    return {
        detected,
        family,
        series: typeof body.series === 'string' ? body.series : undefined,
        device: typeof body.device === 'string' ? body.device : undefined,
        supported: typeof body.supported === 'boolean' ? body.supported : undefined,
        swd_clock_hz: toNumber(body.swd_clock_hz),
        cpuid: body.cpuid as number | string | undefined,
        idcode: idcode as number | string | undefined,
        uid: typeof body.uid === 'string' ? body.uid : undefined,
        flash_base: toNumber(body.flash_base),
        flash_size: toNumber(body.flash_size),
        dev_id: toNumber(body.dev_id),
        flash_kib: toNumber(body.flash_kib),
        model_candidates: typeof body.model_candidates === 'string' ? body.model_candidates : undefined,
        core: typeof body.core === 'string' ? body.core : undefined,
        detection_confidence: body.detection_confidence !== undefined
            ? parseDetectionConfidence(body.detection_confidence)
            : undefined,
        running: typeof body.running === 'boolean' ? body.running : undefined,
        err: typeof body.err === 'string' && body.err ? body.err : undefined,
        code: toNumber(body.code),
        httpStatus,
    };
}

/**
 * Active SWD probe for the diagnostics panel (explicit user action only).
 * Returns the raw device error so the panel can report the failing stage.
 */
export async function probeTargetForDiagnostics(): Promise<{ ok: boolean; err: string }> {
    try {
        const response = await fetch(deviceHttpUrl('/api/target/info'), {
            method: 'GET',
            cache: 'no-store',
        });
        let body: Record<string, unknown> | null = null;
        try {
            const text = await response.text();
            if (text) {
                const parsed = JSON.parse(text);
                if (typeof parsed === 'object' && parsed !== null) {
                    body = parsed as Record<string, unknown>;
                }
            }
        } catch {
            body = null;
        }
        const info = normalizeProbeBody(body, response.status);
        recordTargetDetection(info, '');
        if (!info.detected) {
            const err = info.err || `HTTP ${response.status}`;
            dashboardState.target.lastError = err;
            return { ok: false, err };
        }
        if (!response.ok) {
            const err = info.err || `HTTP ${response.status}`;
            dashboardState.target.lastError = err;
            return { ok: false, err };
        }
        // The caller refreshes /api/target/status once after the probe (the probe
        // itself must not trigger a second request, P1-16).
        return { ok: true, err: '' };
    } catch (error) {
        const err = error instanceof Error ? error.message : 'network error';
        recordTargetDetection(null, err);
        return { ok: false, err };
    }
}

/** Record a DAP kick result (from the palette or the diagnostics panel). */
export function recordDapKick(result: DapKickResult): void {
    dashboardState.dap.lastKick = result;
    dashboardState.dap.lastKickAt = Date.now();
    dashboardState.dap.lastKickError = result.ok ? '' : (result.err || `HTTP ${result.httpStatus}`);
}

/* ------------------------------------------------------------------------- */
/* Diagnostics JSON                                                          */
/* ------------------------------------------------------------------------- */

function targetSummary(): Record<string, unknown> {
    const target = dashboardState.target;
    const info = target.info;
    return {
        detected: info?.detected ?? null,
        family: info?.family ?? null,
        series: info?.series ?? null,
        device: info?.device ?? null,
        supported: info?.supported ?? null,
        dev_id: info?.dev_id ?? null,
        flash_kib: info?.flash_kib ?? null,
        flash_size: info?.flash_size ?? null,
        flash_base: info?.flash_base ?? null,
        uid: info?.uid ?? null,
        core: info?.core ?? null,
        model_candidates: info?.model_candidates ?? null,
        last_error: target.lastError || null,
        http_status: target.httpStatus,
        probed_at: target.probedAt ? new Date(target.probedAt).toISOString() : null,
        status: target.status
            ? {
                state: target.status.state ?? null,
                percent: target.status.percent ?? null,
                err: target.status.err ?? null,
            }
            : null,
        status_error: target.statusError || null,
        status_at: target.statusAt ? new Date(target.statusAt).toISOString() : null,
        // Extended read-only diagnostics (P0-8); null = firmware did not report.
        status_detail: {
            available: target.detail.available,
            http_status: target.detail.httpStatus,
            error: target.detail.error || null,
            failures: target.detail.failures,
            heap_free: target.detail.heapFree,
            heap_largest: target.detail.heapLargest,
            dap_session_active: target.detail.dapSessionActive,
            swd_clock_hz: target.detail.swdClockHz,
            swd_clock_manual_hz: target.detail.swdClockManualHz,
            wired_ncm_active: target.detail.wiredNcmActive,
            wired_dhcp_active: target.detail.wiredDhcpActive,
            wired_dhcp_error: target.detail.wiredDhcpError,
            wired_dhcp_state: target.detail.wiredDhcpState,
            wired_ip: target.detail.wiredIp || null,
            wifi_rssi: target.detail.wifiRssi,
            wifi_connected: target.detail.wifiConnected,
            wifi_ssid: target.detail.wifiSsid || null,
            wifi_channel: target.detail.wifiChannel,
            dap_latency_avg_us: target.detail.dapLatencyAvgUs,
            dap_latency_max_us: target.detail.dapLatencyMaxUs,
            dap_xfer_last_us: target.detail.dapXferLastUs,
            dap_xfer_count: target.detail.dapXferCount,
            dap_ring_active: target.detail.dapRingActive,
            dap_ring_preallocated: target.detail.dapRingPreallocated,
            dap_ring_request_bytes: target.detail.dapRingRequestBytes,
            dap_ring_response_bytes: target.detail.dapRingResponseBytes,
            last_flash_error: target.detail.lastFlashError || null,
        },
    };
}

/** Machine-readable diagnostics snapshot (no WiFi passwords, no invented fields). */
export function buildDiagnosticsSnapshot(): Record<string, unknown> {
    const ws = useWsStore();
    const system = useSystemStore();
    const wifi = dashboardState.wifi;
    const dap = dashboardState.dap;
    const detail = dashboardState.target.detail;
    const link = getLinkInfo();
    const rtt = dashboardState.link;
    return {
        generated_at: new Date().toISOString(),
        web: {
            version: import.meta.env.VITE_APP_WEB_VERSION || 'unknown',
            git_tag: import.meta.env.VITE_APP_GIT_TAG || 'unknown',
            protocol_version: PROTOCOL_VERSION,
        },
        websocket: {
            state: ws.state,
            host: resolveDeviceHost(),
        },
        link: {
            kind: link.kind,
            hostname: link.hostname,
            host_is_wired: link.hostIsWired,
            // Browser-measured HTTP RTT to the device (not a firmware field).
            http_rtt_ms: {
                last: rtt.last,
                min: rtt.min,
                avg: rtt.avg,
                max: rtt.max,
                samples: rtt.samples.length,
                at: rtt.updatedAt ? new Date(rtt.updatedAt).toISOString() : null,
            },
            status_endpoint: {
                available: detail.available,
                http_status: detail.httpStatus,
                error: detail.error || null,
                failures: detail.failures,
            },
        },
        protocol: {
            state: system.protocolState,
            firmware_version: system.fwProtocolVersion,
            web_version: PROTOCOL_VERSION,
        },
        dap: {
            // read-only tcp_server_session_active() from /api/target/status;
            // null when the firmware does not report it.
            session_busy: detail.dapSessionActive,
            dap_latency_avg_us: detail.dapLatencyAvgUs,
            dap_latency_max_us: detail.dapLatencyMaxUs,
            last_kick: dap.lastKick
                ? {
                    ok: dap.lastKick.ok,
                    supported: dap.lastKick.supported,
                    http_status: dap.lastKick.httpStatus,
                    kicked: dap.lastKick.kicked,
                    err: dap.lastKick.err ?? null,
                }
                : null,
            last_kick_at: dap.lastKickAt ? new Date(dap.lastKickAt).toISOString() : null,
            last_kick_error: dap.lastKickError || null,
        },
        wifi: {
            available: wifi.staKnown || wifi.apKnown || wifi.modeKnown,
            sta_connected: wifi.staConnected,
            ssid: wifi.staConnected ? wifi.ssid : null,
            rssi: wifi.staConnected ? wifi.rssi : null,
            ip: wifi.staConnected ? wifi.ip : null,
            gateway: wifi.staConnected ? wifi.gateway : null,
            ap_enabled: wifi.status === null ? null : (wifi.status === 2 || wifi.status === 3),
            ap_ssid: wifi.apKnown ? wifi.apSsid : null,
            mode: wifi.mode,
            status: wifi.status,
            // Firmware-reported link fields (optional; null when absent).
            firmware_rssi: detail.wifiRssi,
            firmware_connected: detail.wifiConnected,
            firmware_ssid: detail.wifiSsid || null,
            firmware_channel: detail.wifiChannel,
            last_error: wifi.lastError || null,
            updated_at: wifi.updatedAt ? new Date(wifi.updatedAt).toISOString() : null,
        },
        wired_ncm: {
            active: detail.wiredNcmActive,
            dhcp_active: detail.wiredDhcpActive,
            dhcp_state: detail.wiredDhcpState,
            dhcp_error: detail.wiredDhcpError,
            device_ip: detail.wiredIp || null,
        },
        target: targetSummary(),
        heap: {
            // Only reported when the firmware exposes heap diagnostics.
            available: detail.heapFree !== null || detail.heapLargest !== null,
            free: detail.heapFree,
            largest: detail.heapLargest,
        },
    };
}

export function buildDiagnosticsJson(): string {
    return JSON.stringify(buildDiagnosticsSnapshot(), null, 2);
}
