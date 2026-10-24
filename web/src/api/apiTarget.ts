/**
 * Web-side target flashing API (docs/stm32-web-flash-plan.md §6/§8, W4 frontend task).
 *
 * Endpoints (firmware side owned by web-dev/dap-dev):
 * - GET  /api/target/info    -> target detection info
 * - POST /api/target/flash   -> octet-stream 裸镜像 (+ X-Target-SHA256 header；
 *                                镜像由页面把 .bin/.elf/.hex/.srec 统一转换后发送)
 * - POST /api/target/reset   -> {"run":true} SWD reset/run
 * - GET  /api/target/status  -> optional progress snapshot (graceful if unsupported)
 *
 * Plain HTTP page: no auth/token headers (login/auth UI was removed by requirement).
 */

import {parseDetectionConfidence, type DetectionConfidence} from '@/utils/targetProfile'

export interface TargetInfo {
    detected: boolean
    family?: string
    series?: string
    device?: string
    supported?: boolean
    swd_clock_hz?: number
    cpuid?: number | string
    idcode?: number | string
    uid?: string
    flash_base?: number
    flash_size?: number
    /** DBGMCU DEV_ID (12 bit, v3 detection detail). */
    dev_id?: number
    /** Flash size in KiB (v3). */
    flash_kib?: number
    /** Narrowed candidate model group, e.g. "STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]". */
    model_candidates?: string
    /** Cortex core from CPUID, e.g. "Cortex-M4". */
    core?: string
    /** Detection confidence: unknown / group / flash-narrowed. */
    detection_confidence?: DetectionConfidence
    running?: boolean
    err?: string
    code?: number
    httpStatus?: number
}

export interface TargetApiError {
    httpStatus: number
    code?: number
    err: string
}

export interface TargetStatus {
    state?: string
    offset?: number
    total?: number
    percent?: number
    err?: string
    heapFree?: number
    heapLargest?: number
    dapSessionActive?: boolean
    swdClockHz?: number
}

export interface TargetFlashSuccess extends TargetInfo {
    ok?: boolean
    written?: number
    total?: number
    sha256?: string
}

export interface TargetFlashHandlers {
    onUploadProgress?: (loaded: number, total: number) => void
    /** Browser upload finished; device-side flash/verify may still be running. */
    onUploadComplete?: () => void
    onSuccess: (result: TargetFlashSuccess) => void
    onFailure: (failure: TargetApiError) => void
}

async function parseJson(response: Response): Promise<Record<string, unknown> | null> {
    try {
        const text = await response.text()
        if (!text) {
            return null
        }
        const parsed = JSON.parse(text)
        return typeof parsed === 'object' && parsed !== null ? parsed as Record<string, unknown> : null
    } catch {
        return null
    }
}

function errorFrom(body: Record<string, unknown> | null, status: number, fallback: string): TargetApiError {
    return {
        httpStatus: status,
        code: typeof body?.code === 'number' ? body.code as number : undefined,
        err: typeof body?.err === 'string' && body.err ? body.err as string : fallback,
    }
}

function toNumber(value: unknown): number | undefined {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value
    }
    if (typeof value === 'string' && value.trim() !== '') {
        const parsed = Number(value)
        return Number.isFinite(parsed) ? parsed : undefined
    }
    return undefined
}

/** GET /api/target/info: detect target chip and return its identity. */
export async function getTargetInfo(): Promise<TargetInfo> {
    const response = await fetch('/api/target/info', {method: 'GET', cache: 'no-store'})
    const body = await parseJson(response)
    if (!response.ok) {
        const failure = errorFrom(body, response.status, `HTTP ${response.status}`)
        return {detected: false, err: failure.err, code: failure.code, httpStatus: response.status}
    }
    const family = typeof body?.family === 'string' ? body.family as string : undefined
    const series = typeof body?.series === 'string' ? body.series as string : undefined
    const device = typeof body?.device === 'string' ? body.device as string : undefined
    const idcode = body?.idcode !== undefined && body?.idcode !== null ? body.idcode as number | string : undefined
    // Prefer explicit `detected`; otherwise infer from family/idcode for tolerant backends.
    const detected = typeof body?.detected === 'boolean'
        ? body.detected as boolean
        : Boolean(family || idcode)
    return {
        detected,
        family,
        series,
        device,
        supported: typeof body?.supported === 'boolean' ? body.supported as boolean : undefined,
        swd_clock_hz: toNumber(body?.swd_clock_hz),
        cpuid: body?.cpuid as number | string | undefined,
        idcode,
        uid: typeof body?.uid === 'string' ? body.uid as string : undefined,
        flash_base: toNumber(body?.flash_base),
        flash_size: toNumber(body?.flash_size),
        dev_id: toNumber(body?.dev_id),
        flash_kib: toNumber(body?.flash_kib),
        model_candidates: typeof body?.model_candidates === 'string' ? body.model_candidates as string : undefined,
        core: typeof body?.core === 'string' ? body.core as string : undefined,
        detection_confidence: body?.detection_confidence !== undefined
            ? parseDetectionConfidence(body.detection_confidence)
            : undefined,
        running: typeof body?.running === 'boolean' ? body.running as boolean : undefined,
    }
}

/** GET /api/target/status (optional): returns null when unsupported/unavailable. */
export async function getTargetStatus(): Promise<TargetStatus | null> {
    try {
        const response = await fetch('/api/target/status', {method: 'GET', cache: 'no-store'})
        if (!response.ok) {
            return null
        }
        const body = await parseJson(response)
        if (!body) {
            return null
        }
        return {
            state: typeof body.state === 'string' ? body.state as string : undefined,
            offset: toNumber(body.offset),
            total: toNumber(body.total),
            percent: toNumber(body.percent),
            err: typeof body.err === 'string' ? body.err as string : undefined,
            heapFree: toNumber(body.heap_free),
            heapLargest: toNumber(body.heap_largest),
            dapSessionActive: typeof body.dap_session_active === 'boolean' ? body.dap_session_active : undefined,
            swdClockHz: toNumber(body.swd_clock_hz),
        }
    } catch {
        return null
    }
}

/**
 * POST /api/target/flash (XHR for upload progress).
 * `body` 为转换后的裸 Flash 镜像 Blob（从 flash_base 开始），浏览器自动
 * 设置 Content-Length（无 chunked upload）；设备端契约不变。
 */
export function flashTarget(body: Blob, sha256Hex: string, handlers: TargetFlashHandlers): { abort(): void } {
    const xhr = new XMLHttpRequest()
    let finished = false

    function fail(failure: TargetApiError) {
        if (finished) {
            return
        }
        finished = true
        handlers.onFailure(failure)
    }

    xhr.open('POST', '/api/target/flash', true)
    xhr.setRequestHeader('Content-Type', 'application/octet-stream')
    // Header is required by the firmware endpoint so it can verify the image before writing.
    xhr.setRequestHeader('X-Target-SHA256', sha256Hex)

    xhr.upload.onprogress = (event: ProgressEvent) => {
        if (event.lengthComputable && handlers.onUploadProgress) {
            handlers.onUploadProgress(event.loaded, event.total)
        }
    }
    xhr.upload.onload = () => {
        if (handlers.onUploadComplete) {
            handlers.onUploadComplete()
        }
    }

    xhr.onload = () => {
        if (finished) {
            return
        }
        let body: Record<string, unknown> | null = null
        try {
            body = xhr.responseText ? JSON.parse(xhr.responseText) as Record<string, unknown> : null
        } catch {
            body = null
        }
        const ok = xhr.status >= 200 && xhr.status < 300 && body?.ok !== false
        if (!ok) {
            fail(errorFrom(body, xhr.status, xhr.status ? `HTTP ${xhr.status}` : 'request failed'))
            return
        }
        finished = true
        handlers.onSuccess({
            ok: true,
            detected: true,
            family: typeof body?.family === 'string' ? body.family as string : undefined,
            idcode: body?.idcode as number | string | undefined,
            uid: typeof body?.uid === 'string' ? body.uid as string : undefined,
            written: toNumber(body?.written),
            total: toNumber(body?.total),
            sha256: typeof body?.sha256 === 'string' ? body.sha256 as string : undefined,
        })
    }
    xhr.onerror = () => fail({httpStatus: 0, err: 'network error'})
    xhr.ontimeout = () => fail({httpStatus: 0, err: 'request timeout'})
    xhr.onabort = () => fail({httpStatus: 0, err: 'aborted'})

    xhr.send(body)

    return {
        abort() {
            if (!finished) {
                finished = true
                xhr.abort()
            }
        },
    }
}

/** POST /api/target/reset {"run":true}: SWD reset and run the target. */
export async function resetTarget(run = true): Promise<{ ok: boolean; err?: string; code?: number; httpStatus: number }> {
    try {
        const response = await fetch('/api/target/reset', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({run}),
        })
        const body = await parseJson(response)
        if (!response.ok || body?.ok === false) {
            const failure = errorFrom(body, response.status, `HTTP ${response.status}`)
            return {ok: false, err: failure.err, code: failure.code, httpStatus: response.status}
        }
        return {ok: true, httpStatus: response.status}
    } catch (error) {
        return {ok: false, err: error instanceof Error ? error.message : 'network error', httpStatus: 0}
    }
}

/** GET /api/target/swd-clock: manually selected SWD clock (0 = auto). */
export async function getSwdClock(): Promise<number | null> {
    try {
        const response = await fetch('/api/target/swd-clock', {method: 'GET', cache: 'no-store'})
        const body = await parseJson(response)
        if (!response.ok || !body) {
            return null
        }
        return toNumber(body.hz) ?? 0
    } catch {
        return null
    }
}

/**
 * POST /api/target/swd-clock. `hz=0` restores automatic 2/1/0.5 MHz probing;
 * manual values are validated by the firmware (100 kHz..10 MHz).
 */
export async function setSwdClock(hz: number): Promise<{ok: boolean; hz: number; err?: string}> {
    try {
        const response = await fetch('/api/target/swd-clock', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({hz}),
        })
        const body = await parseJson(response)
        if (!response.ok || !body) {
            return {ok: false, hz: 0, err: `HTTP ${response.status}`}
        }
        return {
            ok: response.ok,
            hz: toNumber(body.hz) ?? 0,
            err: typeof body.err === 'string' ? body.err : undefined,
        }
    } catch (error) {
        return {ok: false, hz: 0, err: error instanceof Error ? error.message : 'network error'}
    }
}
