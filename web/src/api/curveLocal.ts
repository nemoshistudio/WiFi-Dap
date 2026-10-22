/**
 * 设备本地采样（ESP32 SWD 闭环）HTTP API、CVF1/CVF2 帧解码与多组切分。
 *
 * 契约：docs/curve-local-sampler.md §1.1 / §2 / §5 / §9（协议 v2）。
 * - POST /api/curve/start  {trigger,breakpoint_address,interval_us,halt,series|groups}
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
/** 单组系列上限（兼容旧接口名；v2 多组见 MAX_TOTAL_SERIES）。 */
export const CURVE_LOCAL_MAX_SERIES = 32
export const CURVE_LOCAL_MAX_GROUPS = 8
export const CURVE_LOCAL_MAX_SERIES_PER_GROUP = 32
export const CURVE_LOCAL_MAX_TOTAL_SERIES = CURVE_LOCAL_MAX_GROUPS * CURVE_LOCAL_MAX_SERIES_PER_GROUP
export const CURVE_LOCAL_MAX_VARIANTS = 16
export const CURVE_LOCAL_MAX_SERIES_SIZE = 8
export const CURVE_LOCAL_MAX_SAMPLE_BYTES = 256
export const CURVE_LOCAL_MIN_INTERVAL_US = 1000
export const CURVE_LOCAL_MAX_INTERVAL_US = 200000
/** breakpoint 触发时 interval_us 是节流下限，契约允许 5 ms..1 s。 */
export const CURVE_LOCAL_MAX_INTERVAL_US_BREAKPOINT = 1000000
/** Cortex-M FPB 断点地址范围：目标 Flash。 */
export const CURVE_LOCAL_FLASH_START = 0x08000000
export const CURVE_LOCAL_FLASH_END = 0x08ffffff

export type CurveLocalTrigger = 'periodic' | 'breakpoint' | 'watchpoint'
export const CURVE_LOCAL_MAX_EXPR_DEPTH = 8
export const CURVE_LOCAL_FRAMES_MAX = 64
export const CURVE_LOCAL_FRAMES_DEFAULT = 32

/** CVF1/CVF2 头长度（little-endian，两类头布局相同）。 */
const CURVE_FRAME_HEADER_BYTES = 16
/** CVF1 单组帧头：seq u32 + timestamp u64。 */
const CVF1_FRAME_HEADER_BYTES = 12
const CVF1_MAGIC = 0x31465643 // 'CVF1'（字节 43 56 46 31）
const CVF1_VERSION = 1
/** CVF2 多组帧头：seq u32 + timestamp u64 + group u8 + payload_len u16。 */
const CVF2_FRAME_HEADER_BYTES = 15
const CVF2_MAGIC = 0x32465643 // 'CVF2'（字节 43 56 46 32）
const CVF2_VERSION = 2
const CVF_FLAG_GAP = 0x01
/** task-35：flags bit1 = 帧中带 PC（19B 帧头），否则 15B。 */
const CVF_FLAG_PC = 0x02
/** task-38：flags bit2 = 本批包含 watchpoint 触发帧。 */
const CVF_FLAG_TRIGGER = 0x04
/** task-45：flags bit3 = 本批包含条件触发帧。 */
const CVF_FLAG_CONDITION = 0x08

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

/** loclist 多段变体（协议 v2：固件按 PC 选择）。 */
export interface CurveLocalLocationVariant {
    pcLow: number
    pcHigh: number
    expr: AddressExpr
}

/** 下发给固件的曲线描述：id 对应 frames payload 内的顺序槽位。 */
export interface CurveLocalSeriesSpec {
    id: number
    size: number
    /** 单一位置表达式；有 variants 时可省略（v2 契约示例为 variants-only）。 */
    expr?: AddressExpr
    /** DWARF subprogram PC 范围（可选；仅周期/断点模式都下发，用于固件 PC 门控）。 */
    pcLow?: number
    pcHigh?: number
    /** loclist 多段变体（1..16；有 variants 时固件强制 halt 并按 PC 选择）。 */
    variants?: readonly CurveLocalLocationVariant[]
}

/** 协议 v2 分组（每组 ≤32 路，总和 ≤256）。 */
/** 固件 watchpoint 配置（地址按 size 对齐；access write/readwrite）。 */
export interface CurveLocalWatchpointSpec {
    address: number
    size: 1 | 2 | 4
    access: 'write' | 'readwrite'
}

/** task-45 条件触发适配层（Web 先行预留；固件支持后由 start 直接发送）。 */
export type CurveConditionOp = '==' | '!=' | '>' | '>=' | '<' | '<='
export interface CurveConditionSpec {
    address: number
    size: 1 | 2 | 4
    op: CurveConditionOp
    value: number
    /** 有符号比较（默认 false）。 */
    signed?: boolean
    /** 命中模式：once（默认，命中后停）/ continuous。 */
    mode?: 'once' | 'continuous'
    /** 命中后停止采样（默认 true，与 mode=once 一致）。 */
    stopAfter?: boolean
    /** 兼容 task-44 adapter 旧字段。 */
    once?: boolean
    stop?: boolean
}

export interface CurveLocalGroupSpec {
    id: number
    series: readonly CurveLocalSeriesSpec[]
    /** 采样调度权重 1..8（默认 1；sum(weight) ≤64，契约 §9.7）。 */
    weight?: number
}

export interface StartLocalCurveOptions {
    intervalUs: number
    /** periodic（默认）或 breakpoint（FPB 命中触发）。 */
    trigger?: CurveLocalTrigger
    /** breakpoint 触发地址；Thumb bit0 忽略，范围 Flash 且半字对齐。 */
    breakpointAddress?: number
    /** watchpoint 触发配置（task-38；trigger='watchpoint' 时必填）。 */
    watchpoint?: CurveLocalWatchpointSpec
    /** task-45 条件触发（与 periodic/breakpoint/watchpoint 组合）。 */
    triggerCondition?: CurveConditionSpec
    /** task-44 adapter 旧字段（兼容映射到 trigger_condition）。 */
    condition?: CurveConditionSpec
    /** periodic 是否每拍 halt；breakpoint 时忽略且不下发（命中时核心已暂停）。 */
    halt?: boolean
    /** 是否请求固件在帧中带 PC（task-35，默认 true；halt/breakpoint 会话才有效）。 */
    wantPc?: boolean
    /** 旧单组路径（与 groups 二选一；缺省 groups 时作为单 group）。 */
    series?: readonly CurveLocalSeriesSpec[]
    /** 协议 v2 多组（1..8 组；每组 1..32 路）。 */
    groups?: readonly CurveLocalGroupSpec[]
}

export interface StartLocalCurveResult {
    groupCount: number
    seriesCount: number
    /** 各 group payload 最大值。 */
    sampleBytes: number
    intervalUs: number
}

export interface LocalCurveStatus {
    ok: boolean
    running: boolean
    /** 固件当前是否每拍 halt（false = 纯固定地址/指针的免暂停模式）。 */
    halt?: boolean
    /** 当前触发方式（Phase 2）。 */
    trigger?: CurveLocalTrigger
    /** breakpoint 触发地址（固件回读）。 */
    breakpointAddress?: number
    /** FPB 命中次数（breakpoint 触发）。 */
    hits?: number
    /** 实际写入 ring 的采样次数（breakpoint 触发）。 */
    sampled?: number
    /** 因节流丢弃的次数（breakpoint 触发）。 */
    throttled?: number
    /** PC 不在变量所属函数范围而被固件填 0 的累计次数（PC 门控）。 */
    outOfScope?: number
    /** 协议 v2：group 数 / 当前轮转到的 group。 */
    groupCount?: number
    currentGroup?: number
    /** watchpoint 状态（task-38）。 */
    watchpointArmed?: boolean
    watchpointAddress?: number
    watchpointSize?: number
    watchpointAccess?: 'write' | 'readwrite'
    watchpointHits?: number
    triggerSeq?: number
    /** task-45 条件触发状态。 */
    conditionArmed?: boolean
    conditionMet?: boolean
    conditionHits?: number
    conditionTriggerSeq?: number
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
    /** CVF2 帧所属 group；CVF1 恒为 0。 */
    group: number
    /** task-35：flags bit1=PC_PRESENT 时帧头携带的当前 PC。 */
    pc?: number
    /** task-38：flags bit2=TRIGGER，表示本批含 watchpoint 触发帧。 */
    trigger?: boolean
    /** 该 group 的 series.size 之和字节，按该 group series 顺序拼接。 */
    payload: Uint8Array
}

export interface CurveLocalFramesPage {
    version: number
    flags: number
    /** task-38：flags bit2（watchpoint 触发批次）。 */
    triggered?: boolean
    /** task-45：flags bit3（条件触发批次）。 */
    conditionTriggered?: boolean
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

/** 规范化 FPB 断点地址：Thumb 地址 bit0 忽略（契约 §5.1）。 */
export function normalizeBreakpointAddress(address: number): number {
    return (Math.floor(address) & 0xfffffffe) >>> 0
}

/** 断点地址是否落在目标 Flash 且半字对齐（规范化后）。 */
export function isValidBreakpointAddress(address: number): boolean {
    if (!Number.isFinite(address)) return false
    const normalized = normalizeBreakpointAddress(address)
    return normalized >= CURVE_LOCAL_FLASH_START && normalized <= CURVE_LOCAL_FLASH_END
}

/**
 * 解析 Web UI 的十六进制断点地址输入：允许可选的 0x/0X 前缀与下划线，
 * 自动忽略 Thumb bit0；范围/格式错误返回 null。
 */
export function parseBreakpointAddressInput(text: string): number | null {
    let value = String(text ?? '').trim()
    if (/^0[xX]/.test(value)) value = value.slice(2)
    value = value.replace(/_/g, '')
    if (!/^[0-9a-fA-F]{1,8}$/.test(value)) return null
    const address = Number.parseInt(value, 16) >>> 0
    if (!isValidBreakpointAddress(address)) return null
    return normalizeBreakpointAddress(address)
}

/** 协议 v2 分组切分结果（纯函数，便于单测 0/32/33/64/256/257）。 */
export interface SeriesGroupSplit<T> {
    groups: T[][]
    groupCount: number
    /** 首个错误：empty / too_many_series；null = 可下发。 */
    error: 'empty' | 'too_many_series' | null
}

/** 按每组 perGroup 路切分（默认 32；最多 maxGroups 组 = 总 256 路）。 */
export function splitSeriesIntoGroups<T>(
    series: readonly T[],
    perGroup = CURVE_LOCAL_MAX_SERIES_PER_GROUP,
    maxGroups = CURVE_LOCAL_MAX_GROUPS,
): SeriesGroupSplit<T> {
    const totalLimit = perGroup * maxGroups
    const groups: T[][] = []
    for (let index = 0; index < series.length; index += perGroup) {
        groups.push(series.slice(index, index + perGroup))
    }
    if (series.length === 0) return {groups: [], groupCount: 0, error: 'empty'}
    if (series.length > totalLimit || groups.length > maxGroups) {
        return {groups: groups.slice(0, maxGroups), groupCount: groups.length, error: 'too_many_series'}
    }
    return {groups, groupCount: groups.length, error: null}
}

/** 解析 StartLocalCurveOptions 的分组形式（groups 缺省时 root series 当单组）。 */
export function normalizeLocalGroups(options: StartLocalCurveOptions): CurveLocalGroupSpec[] {
    if (options.groups && options.groups.length > 0) return [...options.groups]
    if (options.series && options.series.length > 0) return [{id: 0, series: options.series}]
    return []
}

/** 计算 series 的字节数（variants 与 expr 互斥；size 由调用方校验）。 */
function seriesSize(item: CurveLocalSeriesSpec): number {
    return Number.isInteger(item.size) ? Math.max(1, Math.min(8, item.size)) : 1
}

/**
 * 把 AddressExpr 编码为固件 v3 compact 字符串（固定宽度十六进制前缀）：
 *   A<addr8> R<reg2><off8> C<off8> +<off8><inner> D<off8><inner>
 *
 * 它替代嵌套 JSON expr，把 40 路深指针请求从 ~7 KiB/数百个 cJSON 节点
 * 降到 ~1–2 KiB/几十个节点，避免 ESP32 堆碎片导致 start 解析失败。
 */
export function encodeAddressExprCompact(expr: AddressExpr): string {
    const off = (value: number) => (value >>> 0).toString(16).padStart(8, '0')
    switch (expr.kind) {
        case 'addr':
            return 'A' + (expr.address >>> 0).toString(16).padStart(8, '0')
        case 'reg':
            return 'R' + (expr.reg & 0xff).toString(16).padStart(2, '0') + off(expr.offset)
        case 'cfa':
            return 'C' + off(expr.offset)
        case 'add':
            return '+' + off(expr.offset) + encodeAddressExprCompact(expr.inner)
        case 'deref':
            return 'D' + off(expr.offset) + encodeAddressExprCompact(expr.inner)
        default:
            throw new CurveLocalError('unsupported address expression kind')
    }
}

/** 本地校验 series 的 expr/variants 结构（loclist 多段 variants 需 1..16 且 PC 区间有效）。 */
function validateSeriesLocation(series: CurveLocalSeriesSpec, label: string): void {
    const variants = series.variants ?? []
    if (variants.length > 0) {
        if (variants.length > CURVE_LOCAL_MAX_VARIANTS) {
            throw new CurveLocalError(`${label} variants count must be 1..${CURVE_LOCAL_MAX_VARIANTS}`)
        }
        for (const variant of variants) {
            if (!Number.isInteger(variant.pcLow) || !Number.isInteger(variant.pcHigh)
                || variant.pcLow < 0 || variant.pcHigh > 0xffffffff || variant.pcLow >= variant.pcHigh) {
                throw new CurveLocalError(`${label} variant pc_low/pc_high must be valid ranges`)
            }
            if (!isValidAddressExprShape(variant.expr)) {
                throw new CurveLocalError(`${label} variant expr is invalid`)
            }
            if (addressExprDepth(variant.expr) > CURVE_LOCAL_MAX_EXPR_DEPTH) {
                throw new CurveLocalError(`${label} variant expr depth > ${CURVE_LOCAL_MAX_EXPR_DEPTH}`)
            }
        }
        // variants 与顶层 expr 可并存（旧路径兼容），但必须有可用变体。
        return
    }
    if (!series.expr) throw new CurveLocalError(`${label} expr is required when variants are absent`)
    if (!isValidAddressExprShape(series.expr)) throw new CurveLocalError(`${label} expr is invalid`)
    if (addressExprDepth(series.expr) > CURVE_LOCAL_MAX_EXPR_DEPTH) {
        throw new CurveLocalError(`${label} expr depth > ${CURVE_LOCAL_MAX_EXPR_DEPTH}`)
    }
}

/** AddressExpr 基础形状校验（kind + 必填字段），防止把坏数据发给固件。 */
export function isValidAddressExprShape(expr: AddressExpr): boolean {
    if (!expr || typeof expr !== 'object') return false
    switch (expr.kind) {
        case 'addr':
            return typeof expr.address === 'number' && Number.isFinite(expr.address)
        case 'reg':
            return Number.isInteger(expr.reg) && expr.reg >= 0 && Number.isInteger(expr.offset)
        case 'cfa':
            return Number.isInteger(expr.offset)
        case 'add':
        case 'deref':
            return typeof expr.offset === 'number' && Number.isInteger(expr.offset)
                && isValidAddressExprShape(expr.inner)
        default:
            return false
    }
}

/** 下发前做本地校验，尽早给出可读错误（避免固件返回模糊失败）。 */
export function validateStartLocalCurve(options: StartLocalCurveOptions): void {
    const {intervalUs} = options
    const trigger = options.trigger ?? 'periodic'
    const maxInterval = trigger === 'breakpoint' || trigger === 'watchpoint' ? CURVE_LOCAL_MAX_INTERVAL_US_BREAKPOINT : CURVE_LOCAL_MAX_INTERVAL_US
    const minInterval = trigger === 'breakpoint' || trigger === 'watchpoint' ? 5000 : CURVE_LOCAL_MIN_INTERVAL_US
    if (!Number.isInteger(intervalUs) || intervalUs < minInterval || intervalUs > maxInterval) {
        throw new CurveLocalError(`interval_us must be ${minInterval}..${maxInterval} for trigger=${trigger}`)
    }
    if (trigger === 'breakpoint') {
        if (typeof options.breakpointAddress !== 'number' || !Number.isInteger(options.breakpointAddress)) {
            throw new CurveLocalError('breakpoint_address required for breakpoint trigger')
        }
        if (!isValidBreakpointAddress(options.breakpointAddress)) {
            throw new CurveLocalError(
                `breakpoint address must be in Flash 0x${CURVE_LOCAL_FLASH_START.toString(16)}..0x${CURVE_LOCAL_FLASH_END.toString(16)} and halfword aligned`,
            )
        }
    }
    const customCondition = options.triggerCondition ?? options.condition
    if (customCondition) {
        const condition = customCondition
        const address = condition.address >>> 0
        if (!Number.isInteger(address) || address === 0) throw new CurveLocalError('condition address must be non-zero u32')
        if (![1, 2, 4].includes(condition.size)) throw new CurveLocalError('condition size must be 1, 2 or 4')
        if (address % condition.size !== 0) throw new CurveLocalError('condition address must be aligned to size')
        if (!['==', '!=', '>', '>=', '<', '<='].includes(condition.op)) throw new CurveLocalError('invalid condition op')
        if (!Number.isFinite(condition.value)) throw new CurveLocalError('condition value must be a number')
        const mode = condition.mode ?? 'once'
        if (mode !== 'once' && mode !== 'continuous') throw new CurveLocalError("condition mode must be 'once' or 'continuous'")
    }
    if (trigger === 'watchpoint') {
        const watchpoint = options.watchpoint
        if (!watchpoint) throw new CurveLocalError('watchpoint config required for watchpoint trigger')
        const address = watchpoint.address >>> 0
        if (!Number.isInteger(address) || address === 0) throw new CurveLocalError('watchpoint address must be non-zero u32')
        if (![1, 2, 4].includes(watchpoint.size)) throw new CurveLocalError('watchpoint size must be 1, 2 or 4')
        if (address % watchpoint.size !== 0) throw new CurveLocalError('watchpoint address must be aligned to size')
        if (watchpoint.access !== 'write' && watchpoint.access !== 'readwrite') {
            throw new CurveLocalError("watchpoint access must be 'write' or 'readwrite'")
        }
    }
    const groups = normalizeLocalGroups(options)
    if (groups.length < 1 || groups.length > CURVE_LOCAL_MAX_GROUPS) {
        throw new CurveLocalError(`groups count must be 1..${CURVE_LOCAL_MAX_GROUPS}`)
    }
    let totalSeries = 0
    let totalWeight = 0
    for (const group of groups) {
        const weight = group.weight ?? 1
        if (!Number.isInteger(weight) || weight < 1 || weight > 8) {
            throw new CurveLocalError(`group[${group.id}] weight must be 1..8`)
        }
        totalWeight += weight
        if (!Array.isArray(group.series) || group.series.length < CURVE_LOCAL_MIN_SERIES
            || group.series.length > CURVE_LOCAL_MAX_SERIES_PER_GROUP) {
            throw new CurveLocalError(`group[${group.id}] series count must be ${CURVE_LOCAL_MIN_SERIES}..${CURVE_LOCAL_MAX_SERIES_PER_GROUP}`)
        }
        let groupBytes = 0
        for (const item of group.series) {
            totalSeries += 1
            if (!Number.isInteger(item.size) || item.size < 1 || item.size > CURVE_LOCAL_MAX_SERIES_SIZE) {
                throw new CurveLocalError(`series[${item.id}] size must be 1..${CURVE_LOCAL_MAX_SERIES_SIZE}`)
            }
            validateSeriesLocation(item, `group[${group.id}] series[${item.id}]`)
            if (item.pcLow !== undefined || item.pcHigh !== undefined) {
                const pcLow = item.pcLow
                const pcHigh = item.pcHigh
                if (typeof pcLow !== 'number' || typeof pcHigh !== 'number'
                    || !Number.isInteger(pcLow) || !Number.isInteger(pcHigh)
                    || pcLow < CURVE_LOCAL_FLASH_START || pcHigh > CURVE_LOCAL_FLASH_END
                    || pcLow >= pcHigh) {
                    throw new CurveLocalError(
                        `series[${item.id}] pc_low/pc_high must be a valid Flash range (0x${CURVE_LOCAL_FLASH_START.toString(16)}..0x${CURVE_LOCAL_FLASH_END.toString(16)}, low<high)`,
                    )
                }
            }
            groupBytes += item.size
        }
        if (groupBytes > CURVE_LOCAL_MAX_SAMPLE_BYTES) {
            throw new CurveLocalError(`group[${group.id}] sum(series.size) must be <= ${CURVE_LOCAL_MAX_SAMPLE_BYTES}`)
        }
    }
    if (totalSeries > CURVE_LOCAL_MAX_TOTAL_SERIES) {
        throw new CurveLocalError(`total series count must be <= ${CURVE_LOCAL_MAX_TOTAL_SERIES}`)
    }
    if (totalWeight > 64) {
        throw new CurveLocalError('sum(group.weight) must be <= 64')
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

function optionalFiniteNumber(value: unknown): number | undefined {
    return typeof value === 'number' && Number.isFinite(value) ? value : undefined
}

/** 启动设备本地采样。 */
/** 序列化一个 series（v2：variants-only 或 expr+pc_low/pc_high）。 */
function serializeSeriesSpec(item: CurveLocalSeriesSpec): Record<string, unknown> {
    const spec: Record<string, unknown> = {id: item.id, size: item.size}
    const variants = item.variants ?? []
    if (variants.length > 0) {
        spec.variants = variants.map((variant) => ({
            pc_low: variant.pcLow >>> 0,
            pc_high: variant.pcHigh >>> 0,
            expr_s: encodeAddressExprCompact(variant.expr),
        }))
    } else {
        if (!item.expr) throw new CurveLocalError('expr is required when variants are absent')
        spec.expr_s = encodeAddressExprCompact(item.expr)
        if (item.pcLow !== undefined && item.pcHigh !== undefined) {
            spec.pc_low = item.pcLow >>> 0
            spec.pc_high = item.pcHigh >>> 0
        }
    }
    return spec
}

/** 启动设备本地采样（协议 v2：groups；单组无 variants 时保持 v1 root series 兼容）。 */
export async function startLocalCurve(options: StartLocalCurveOptions): Promise<StartLocalCurveResult> {
    validateStartLocalCurve(options)
    const trigger = options.trigger ?? 'periodic'
    const groups = normalizeLocalGroups(options)
    const hasVariants = groups.some((group) => group.series.some((item) => (item.variants?.length ?? 0) > 0))
    const explicitGroups = (options.groups?.length ?? 0) > 0
    const useGroups = explicitGroups || groups.length > 1 || hasVariants
    const payload: Record<string, unknown> = {
        trigger,
        interval_us: options.intervalUs,
        // task-35：默认请求 PC（固件在 halt/breakpoint 会话才带；no-halt 会话忽略）。
        want_pc: options.wantPc !== false,
    }
    if (useGroups) {
        payload.groups = groups.map((group, index) => ({
            id: Number.isInteger(group.id) ? group.id : index,
            weight: group.weight ?? 1,
            series: group.series.map(serializeSeriesSpec),
        }))
    } else {
        payload.series = groups[0].series.map(serializeSeriesSpec)
    }
    if (trigger === 'breakpoint') {
        // 断点命中时核心已暂停，halt 字段无意义且契约要求忽略；不下发避免冲突。
        payload.breakpoint_address = normalizeBreakpointAddress(options.breakpointAddress ?? 0)
    } else if (trigger === 'watchpoint' && options.watchpoint) {
        payload.watch_address = options.watchpoint.address >>> 0
        payload.watch_size = options.watchpoint.size
        payload.watch_access = options.watchpoint.access
    } else {
        // loclist variants 要求固件按 PC 选择，必须 halt（契约 §9.4）。
        payload.halt = hasVariants ? true : options.halt !== false
    }
    const customCondition = options.triggerCondition ?? options.condition
    if (customCondition) {
        // task-45：可与 periodic/breakpoint/watchpoint 组合；字段使用 trigger_condition。
        payload.trigger_condition = {
            address: customCondition.address >>> 0,
            size: customCondition.size,
            op: customCondition.op,
            value: customCondition.value,
            signed: customCondition.signed === true,
            mode: customCondition.mode ?? (customCondition.once === true ? 'once' : 'continuous'),
            stop_after: customCondition.stopAfter ?? customCondition.stop ?? true,
        }
    }
    const body = await requestJson(CURVE_LOCAL_START_PATH, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload),
    })
    const totalSeries = groups.reduce((sum, group) => sum + group.series.length, 0)
    const maxGroupBytes = groups.reduce((max, group) => Math.max(max, group.series.reduce((sum, item) => sum + item.size, 0)), 0)
    return {
        groupCount: finiteNumber(body.group_count, groups.length),
        seriesCount: finiteNumber(body.series_count, totalSeries),
        sampleBytes: finiteNumber(body.sample_bytes, maxGroupBytes),
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
        halt: body.halt === true ? true : body.halt === false ? false : undefined,
        trigger: body.trigger === 'breakpoint' ? 'breakpoint' : body.trigger === 'watchpoint' ? 'watchpoint' : body.trigger === 'periodic' ? 'periodic' : undefined,
        breakpointAddress: optionalFiniteNumber(body.breakpoint_address),
        hits: optionalFiniteNumber(body.hits),
        sampled: optionalFiniteNumber(body.sampled),
        throttled: optionalFiniteNumber(body.throttled),
        outOfScope: optionalFiniteNumber(body.out_of_scope),
        groupCount: optionalFiniteNumber(body.group_count),
        currentGroup: optionalFiniteNumber(body.current_group),
        watchpointArmed: body.watchpoint_armed === true ? true : body.watchpoint_armed === false ? false : undefined,
        watchpointAddress: optionalFiniteNumber(body.watchpoint_address),
        watchpointSize: optionalFiniteNumber(body.watchpoint_size),
        watchpointAccess: body.watchpoint_access === 'readwrite' ? 'readwrite' : body.watchpoint_access === 'write' ? 'write' : undefined,
        watchpointHits: optionalFiniteNumber(body.watchpoint_hits),
        triggerSeq: optionalFiniteNumber(body.trigger_seq),
        conditionArmed: body.condition_armed === true ? true : body.condition_armed === false ? false : undefined,
        conditionMet: body.condition_met === true ? true : body.condition_met === false ? false : undefined,
        conditionHits: optionalFiniteNumber(body.condition_hits),
        conditionTriggerSeq: optionalFiniteNumber(body.condition_trigger_seq),
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
 * 解码 CVF1（单组）或 CVF2（多组）二进制帧页。
 * 空 body（204/0 字节）返回空页；magic/version/截断/坏 payload_len 抛 CurveLocalError。
 */
export function decodeCurveFrames(input: ArrayBuffer | Uint8Array): CurveLocalFramesPage {
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input)
    if (bytes.length === 0) return emptyCurveFramesPage()
    if (bytes.length < CURVE_FRAME_HEADER_BYTES) {
        throw new CurveLocalError(`CVF header too short (${bytes.length}B)`)
    }
    const magic = readU32(bytes, 0)
    if (magic === CVF1_MAGIC) return decodeCvf1(bytes)
    if (magic === CVF2_MAGIC) return decodeCvf2(bytes)
    throw new CurveLocalError(`CVF magic mismatch: 0x${magic.toString(16)}`)
}

function decodeCvf1(bytes: Uint8Array): CurveLocalFramesPage {
    const version = bytes[4]
    if (version !== CVF1_VERSION) throw new CurveLocalError(`CVF1 unsupported version ${version}`)
    const flags = bytes[5]
    const count = readU16(bytes, 6)
    const firstSeq = readU32(bytes, 8)
    const frameBytes = readU16(bytes, 12)
    const stride = CVF1_FRAME_HEADER_BYTES + frameBytes
    const expected = CURVE_FRAME_HEADER_BYTES + count * stride
    if (bytes.length < expected) {
        throw new CurveLocalError(`CVF1 truncated: need ${expected}B, got ${bytes.length}B`)
    }
    const frames: CurveLocalFrame[] = []
    let offset = CURVE_FRAME_HEADER_BYTES
    for (let index = 0; index < count; index++) {
        const seq = readU32(bytes, offset)
        const timestampUs = readU64Number(bytes, offset + 4)
        const payloadStart = offset + CVF1_FRAME_HEADER_BYTES
        frames.push({seq, timestampUs, group: 0, payload: bytes.slice(payloadStart, payloadStart + frameBytes)})
        offset += stride
    }
    return {version: 1, flags, triggered: (flags & CVF_FLAG_TRIGGER) !== 0,
      conditionTriggered: (flags & CVF_FLAG_CONDITION) !== 0, gap: (flags & CVF_FLAG_GAP) !== 0, count, firstSeq, frameBytes, frames}
}

function decodeCvf2(bytes: Uint8Array): CurveLocalFramesPage {
    const version = bytes[4]
    if (version !== CVF2_VERSION) throw new CurveLocalError(`CVF2 unsupported version ${version}`)
    const flags = bytes[5]
    const count = readU16(bytes, 6)
    const firstSeq = readU32(bytes, 8)
    const frameBytes = readU16(bytes, 12)
    // task-35：必须先按 flags 判断 PC_PRESENT；有 PC 的帧头是 19B（含 pc u32），
    // 无 PC 15B。否则 halt 会话的 payload 会错位。
    const pcPresent = (flags & CVF_FLAG_PC) !== 0
    const frameHeaderBytes = CVF2_FRAME_HEADER_BYTES + (pcPresent ? 4 : 0)
    const frames: CurveLocalFrame[] = []
    let offset = CURVE_FRAME_HEADER_BYTES
    for (let index = 0; index < count; index++) {
        if (offset + frameHeaderBytes > bytes.length) {
            throw new CurveLocalError(`CVF2 truncated frame header at index ${index}`)
        }
        const seq = readU32(bytes, offset)
        const timestampUs = readU64Number(bytes, offset + 4)
        const group = bytes[offset + 12]
        const payloadLen = readU16(bytes, offset + 13)
        if (payloadLen > frameBytes) {
            throw new CurveLocalError(`CVF2 invalid payload_len ${payloadLen} (frame_bytes ${frameBytes})`)
        }
        const pc = pcPresent ? readU32(bytes, offset + 15) : undefined
        const payloadStart = offset + frameHeaderBytes
        if (payloadStart + payloadLen > bytes.length) {
            throw new CurveLocalError(`CVF2 truncated payload at index ${index}`)
        }
        const frame: CurveLocalFrame = {seq, timestampUs, group, payload: bytes.slice(payloadStart, payloadStart + payloadLen)}
        if (pc !== undefined) frame.pc = pc
        frames.push(frame)
        offset = payloadStart + payloadLen
    }
    return {version: 2, flags, triggered: (flags & CVF_FLAG_TRIGGER) !== 0,
      conditionTriggered: (flags & CVF_FLAG_CONDITION) !== 0, gap: (flags & CVF_FLAG_GAP) !== 0, count, firstSeq, frameBytes, frames}
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
