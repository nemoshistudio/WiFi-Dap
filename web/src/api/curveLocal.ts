/**
 * 设备本地采样（ESP32 SWD 闭环）HTTP API 与 CVF1 帧解码。
 *
 * 契约：docs/curve-local-sampler.md §1.1 / §2。
 * - POST /api/curve/start  {interval_us,halt,series:[{id,size,expr}]}
 * - POST /api/curve/stop
 * - GET  /api/curve/status
 * - GET  /api/curve/frames?since=&max=  -> CVF1 二进制（204/空 = 无新帧）
 *
 * 生产环境使用同源相对路径；Vite dev 模式用 VITE_DEVICE_HOST_NAME 指向设备。
 * 该模块不建立 WebSocket-DAP 会话，也不依赖 wsDap.ts。
 */
import type {AddressExpr} from '../utils/elfTypes'

/** 固件接口路径。 */
export const CURVE_LOCAL_START_PATH = '/api/curve/start'
export const CURVE_LOCAL_STOP_PATH = '/api/curve/stop'
export const CURVE_LOCAL_STATUS_PATH = '/api/curve/status'
export const CURVE_LOCAL_FRAMES_PATH = '/api/curve/frames'

/** 固件契约限制（docs §1.1）。 */
export const CURVE_LOCAL_MIN_SERIES = 1
export const CURVE_LOCAL_MAX_SERIES = 16
export const CURVE_LOCAL_MAX_SERIES_SIZE = 8
export const CURVE_LOCAL_MAX_SAMPLE_BYTES = 256
export const CURVE_LOCAL_MIN_INTERVAL_US = 5000
export const CURVE_LOCAL_MAX_INTERVAL_US = 200000
export const CURVE_LOCAL_MAX_EXPR_DEPTH = 8
export const CURVE_LOCAL_FRAMES_MAX = 64
export const CURVE_LOCAL_FRAMES_DEFAULT = 32

/** CVF1 头长度与帧头长度（little-endian）。 */
const CVF1_HEADER_BYTES = 16
const CVF1_FRAME_HEADER_BYTES = 12
const CVF1_MAGIC = 0x31465643 // 'CVF1'（字节 43 56 46 31）
const CVF1_VERSION = 1
const CVF1_FLAG_GAP = 0x01

export class CurveLocalError extends Error {
    /** HTTP 状态码；0 = 网络/解析错误。 */
    httpStatus: number
    /** 固件 JSON 错误码（若提供）。 */
    code?: number

    constructor(message: string, httpStatus = 0, code?: number) {
        super(message)
        this.name = 'CurveLocalError'
        this.httpStatus = httpStatus
        this.code = code
    }
}

/** 下发给固件的曲线描述：id 对应 frames payload 内的顺序槽位。 */
export interface CurveLocalSeriesSpec {
    id: number
    size: number
    expr: AddressExpr
}

export interface StartLocalCurveOptions {
    intervalUs: number
    /** 固件每拍 halt 读取（默认 true，Phase 1 只支持 halt 闭环）。 */
    halt?: boolean
    series: readonly CurveLocalSeriesSpec[]
}

export interface StartLocalCurveResult {
    seriesCount: number
    sampleBytes: number
    intervalUs: number
}

export interface LocalCurveStatus {
    ok: boolean
    running: boolean
    intervalUs: number
    frames: number
    overwritten: number
    invalid: number
    error: number
    sampleBytes: number
}

export interface CurveLocalFrame {
    seq: number
    /** ESP32 esp_timer_get_time() 微秒时间戳。 */
    timestampUs: number
    /** sum(series.size) 字节，按 start 时 series 顺序拼接。 */
    payload: Uint8Array
}

export interface CurveLocalFramesPage {
    version: number
    flags: number
    /** bit0=gap（since 已被 ring 覆盖，存在丢帧）。 */
    gap: boolean
    count: number
    firstSeq: number
    frameBytes: number
    frames: CurveLocalFrame[]
}

export function emptyCurveFramesPage(): CurveLocalFramesPage {
    return {version: 0, flags: 0, gap: false, count: 0, firstSeq: 0, frameBytes: 0, frames: []}
}

function envValue(name: 'VITE_APP_MODE' | 'VITE_DEVICE_HOST_NAME'): string | undefined {
    // 生产构建由 Vite 静态替换；Node 单测里 import.meta.env 不存在，故用可选链。
    const env = (import.meta as unknown as {env?: Record<string, string | undefined>}).env
    return env?.[name]
}

/** 设备 HTTP 基地址：生产环境同源；dev 模式指向真实设备。 */
export function deviceApiUrl(path: string): string {
    if (envValue('VITE_APP_MODE') === 'dev') {
        const host = envValue('VITE_DEVICE_HOST_NAME')
        if (host) {
            const withoutScheme = host.replace(/^https?:\/\//, '').replace(/\/+$/, '')
            return `http://${withoutScheme}${path}`
        }
    }
    return path
}

/** 计算 AddressExpr 深度（根为 1）；用于契约的深度 <= 8 校验。 */
export function addressExprDepth(expr: AddressExpr): number {
    switch (expr.kind) {
        case 'add':
        case 'deref':
            return 1 + addressExprDepth(expr.inner)
        default:
            return 1
    }
}

/** 下发前做本地校验，尽早给出可读错误（避免固件返回模糊失败）。 */
export function validateStartLocalCurve(options: StartLocalCurveOptions): void {
    const {intervalUs, series} = options
    if (!Number.isInteger(intervalUs) || intervalUs < CURVE_LOCAL_MIN_INTERVAL_US || intervalUs > CURVE_LOCAL_MAX_INTERVAL_US) {
        throw new CurveLocalError(`interval_us must be ${CURVE_LOCAL_MIN_INTERVAL_US}..${CURVE_LOCAL_MAX_INTERVAL_US}`)
    }
    if (series.length < CURVE_LOCAL_MIN_SERIES || series.length > CURVE_LOCAL_MAX_SERIES) {
        throw new CurveLocalError(`series count must be ${CURVE_LOCAL_MIN_SERIES}..${CURVE_LOCAL_MAX_SERIES}`)
    }
    let sampleBytes = 0
    for (const item of series) {
        if (!Number.isInteger(item.size) || item.size < 1 || item.size > CURVE_LOCAL_MAX_SERIES_SIZE) {
            throw new CurveLocalError(`series[${item.id}] size must be 1..${CURVE_LOCAL_MAX_SERIES_SIZE}`)
        }
        if (addressExprDepth(item.expr) > CURVE_LOCAL_MAX_EXPR_DEPTH) {
            throw new CurveLocalError(`series[${item.id}] expr depth > ${CURVE_LOCAL_MAX_EXPR_DEPTH}`)
        }
        sampleBytes += item.size
    }
    if (sampleBytes > CURVE_LOCAL_MAX_SAMPLE_BYTES) {
        throw new CurveLocalError(`sum(series.size) must be <= ${CURVE_LOCAL_MAX_SAMPLE_BYTES}`)
    }
}

function parseJsonText(text: string): Record<string, unknown> | null {
    if (!text) return null
    try {
        const parsed = JSON.parse(text)
        return typeof parsed === 'object' && parsed !== null ? parsed as Record<string, unknown> : null
    } catch {
        return null
    }
}

function errorMessage(body: Record<string, unknown> | null, status: number, text?: string): string {
    if (typeof body?.err === 'string' && body.err) return body.err
    if (text && text.trim()) return text.trim().slice(0, 200)
    return status > 0 ? `HTTP ${status}` : 'network error'
}

function errorCode(body: Record<string, unknown> | null): number | undefined {
    return typeof body?.code === 'number' && Number.isFinite(body.code) ? body.code : undefined
}

async function requestJson(path: string, init: RequestInit): Promise<Record<string, unknown>> {
    let response: Response
    try {
        response = await fetch(deviceApiUrl(path), {cache: 'no-store', ...init})
    } catch (error) {
        throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
    }
    let text = ''
    try {
        text = await response.text()
    } catch {
        text = ''
    }
    const body = parseJsonText(text)
    if (!response.ok || body?.ok === false) {
        throw new CurveLocalError(errorMessage(body, response.status, text), response.status, errorCode(body))
    }
    return body ?? {}
}

function finiteNumber(value: unknown, fallback: number): number {
    return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

/** 启动设备本地采样。 */
export async function startLocalCurve(options: StartLocalCurveOptions): Promise<StartLocalCurveResult> {
    validateStartLocalCurve(options)
    const body = await requestJson(CURVE_LOCAL_START_PATH, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            interval_us: options.intervalUs,
            halt: options.halt !== false,
            series: options.series.map((item) => ({id: item.id, size: item.size, expr: item.expr})),
        }),
    })
    const seriesCount = finiteNumber(body.series_count, options.series.length)
    const sampleBytes = finiteNumber(body.sample_bytes, options.series.reduce((sum, item) => sum + item.size, 0))
    return {
        seriesCount,
        sampleBytes,
        intervalUs: finiteNumber(body.interval_us, options.intervalUs),
    }
}

/** 停止设备本地采样（可重复调用；unmount 时用 keepalive 发送）。 */
export async function stopLocalCurve(): Promise<void> {
    const response = await fetch(deviceApiUrl(CURVE_LOCAL_STOP_PATH), {
        method: 'POST',
        cache: 'no-store',
        keepalive: true,
    }).catch((error: unknown) => {
        throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
    })
    // stop 的响应体可能为空；失败时尽量带 JSON err。
    let text = ''
    try {
        text = await response.text()
    } catch {
        text = ''
    }
    const body = parseJsonText(text)
    if (!response.ok || body?.ok === false) {
        throw new CurveLocalError(errorMessage(body, response.status, text), response.status, errorCode(body))
    }
}

/** 读取设备本地采样状态。 */
export async function getLocalCurveStatus(): Promise<LocalCurveStatus> {
    const body = await requestJson(CURVE_LOCAL_STATUS_PATH, {method: 'GET'})
    return {
        ok: body.ok !== false,
        running: body.running === true,
        intervalUs: finiteNumber(body.interval_us, 0),
        frames: finiteNumber(body.frames, 0),
        overwritten: finiteNumber(body.overwritten, 0),
        invalid: finiteNumber(body.invalid, 0),
        error: finiteNumber(body.error, 0),
        sampleBytes: finiteNumber(body.sample_bytes, 0),
    }
}

function readU32(bytes: Uint8Array, offset: number): number {
    return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0
}

function readU16(bytes: Uint8Array, offset: number): number {
    return bytes[offset] | (bytes[offset + 1] << 8)
}

function readU64Number(bytes: Uint8Array, offset: number): number {
    // esp_timer_get_time() 微秒数远小于 2^53，Number 足够精确。
    const low = readU32(bytes, offset)
    const high = readU32(bytes, offset + 4)
    return high * 0x100000000 + low
}

/**
 * 解码 CVF1 二进制帧页。
 * 空 body（204 或固件返回 0 字节）返回空页；magic/version/截断错误抛 CurveLocalError。
 */
export function decodeCurveFrames(input: ArrayBuffer | Uint8Array): CurveLocalFramesPage {
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input)
    if (bytes.length === 0) return emptyCurveFramesPage()
    if (bytes.length < CVF1_HEADER_BYTES) {
        throw new CurveLocalError(`CVF1 header too short (${bytes.length}B)`)
    }
    const magic = readU32(bytes, 0)
    if (magic !== CVF1_MAGIC) {
        throw new CurveLocalError(`CVF1 magic mismatch: 0x${magic.toString(16)}`)
    }
    const version = bytes[4]
    if (version !== CVF1_VERSION) {
        throw new CurveLocalError(`CVF1 unsupported version ${version}`)
    }
    const flags = bytes[5]
    const count = readU16(bytes, 6)
    const firstSeq = readU32(bytes, 8)
    const frameBytes = readU16(bytes, 12)
    const frameStride = CVF1_FRAME_HEADER_BYTES + frameBytes
    const expected = CVF1_HEADER_BYTES + count * frameStride
    if (bytes.length < expected) {
        throw new CurveLocalError(`CVF1 truncated: need ${expected}B, got ${bytes.length}B`)
    }
    const frames: CurveLocalFrame[] = []
    let offset = CVF1_HEADER_BYTES
    for (let index = 0; index < count; index++) {
        const seq = readU32(bytes, offset)
        const timestampUs = readU64Number(bytes, offset + 4)
        const payloadStart = offset + CVF1_FRAME_HEADER_BYTES
        frames.push({
            seq,
            timestampUs,
            payload: bytes.slice(payloadStart, payloadStart + frameBytes),
        })
        offset += frameStride
    }
    return {version, flags, gap: (flags & CVF1_FLAG_GAP) !== 0, count, firstSeq, frameBytes, frames}
}

/** 轮询设备本地采样帧；since=0 从 ring 最旧帧开始。 */
export async function getLocalCurveFrames(since: number, max = CURVE_LOCAL_FRAMES_DEFAULT): Promise<CurveLocalFramesPage> {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    const safeMax = Math.max(1, Math.min(CURVE_LOCAL_FRAMES_MAX, Math.floor(Number.isFinite(max) ? max : CURVE_LOCAL_FRAMES_DEFAULT)))
    const query = `?since=${safeSince}&max=${safeMax}`
    let response: Response
    try {
        response = await fetch(deviceApiUrl(CURVE_LOCAL_FRAMES_PATH + query), {
            method: 'GET',
            cache: 'no-store',
        })
    } catch (error) {
        throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
    }
    if (response.status === 204) return emptyCurveFramesPage()
    if (!response.ok) {
        let text = ''
        try {
            text = await response.text()
        } catch {
            text = ''
        }
        const body = parseJsonText(text)
        throw new CurveLocalError(errorMessage(body, response.status, text), response.status, errorCode(body))
    }
    const buffer = await response.arrayBuffer()
    return decodeCurveFrames(buffer)
}
