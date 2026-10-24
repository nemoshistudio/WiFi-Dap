/**
 * 设备本地采样（ESP32 SWD 闭环）HTTP API、CVF1/CVF2 帧解码与多组切分。
 *
 * 契约：docs/curve-local-sampler.md §1.1 / §2 / §5 / §9（协议 v2；固件 task-60
 * 增补 per-series `sample_every`、CVF2 per-series valid 位图 footer、最多 4 路 DWT
 * watchpoint 与 trace/lines 行接口——均向后兼容旧固件/旧 CVF2 解码）。
 * - POST /api/curve/start  {trigger,breakpoint_address,interval_us,halt,series|groups,
 *                           watch/watch2/watch3/watch4|watchpoints, sample_every}
 * - POST /api/curve/stop
 * - GET  /api/curve/status
 * - GET  /api/curve/frames?since=&max=  -> CVF1/CVF2 二进制（204/空 = 无新帧）
 * - GET  /api/curve/trace/lines?since=&max= -> JSON 行（task-60；404 = 旧固件）
 *
 * 生产环境使用同源相对路径；Vite dev 模式用 VITE_DEVICE_HOST_NAME 指向设备。
 * 该模块不建立 WebSocket-DAP 会话，也不依赖 wsDap.ts。
 */
import type {AddressExpr} from '../utils/elfTypes'

/** 固件接口路径。 */
export const CURVE_LOCAL_START_PATH = '/api/curve/start'
/** 固件 task-60（P0-1）：CBS1 流式二进制 start 请求端点（Content-Type 任意，按 octet-stream 发）。 */
export const CURVE_LOCAL_START_BIN_PATH = '/api/curve/start-bin'
/** CBS1 body 上限（固件 CURVE_BIN_BODY_MAX=16384；流式解析，不用 16 KiB 连续堆）。 */
export const CURVE_LOCAL_MAX_BIN_BODY = 16384
/** 旧 JSON start body 上限（固件 CURVE_JSON_BODY_MAX 恢复为 16384）。 */
export const CURVE_LOCAL_MAX_JSON_BODY = 16384
export const CURVE_LOCAL_STOP_PATH = '/api/curve/stop'
export const CURVE_LOCAL_STATUS_PATH = '/api/curve/status'
export const CURVE_LOCAL_FRAMES_PATH = '/api/curve/frames'
/** 固件 task-57 调试能力：目标 UART trace（原型）。 */
export const CURVE_LOCAL_TRACE_START_PATH = '/api/curve/trace/start'
export const CURVE_LOCAL_TRACE_STOP_PATH = '/api/curve/trace/stop'
export const CURVE_LOCAL_TRACE_PATH = '/api/curve/trace'
/**
 * 固件 task-60 trace v2：带固件侧行缓冲/时间戳/触发标记的 JSON 行接口。
 * 旧 `/api/curve/trace`（raw bytes + X-Curve-Trace-* 头）保持不变；本接口 404 时
 * Web 回退旧 raw 视图（不伪造行/时间）。
 */
export const CURVE_LOCAL_TRACE_LINES_PATH = '/api/curve/trace/lines'
/**
 * 单次 trace/lines 请求的最大行数。固件 task-60 FINAL：`max` 允许 1..8，
 * 固件行 ring 只保留最近 32 行；Web 每轮按 max=8 拉取，连续轮询叠加显示。
 */
export const CURVE_LOCAL_TRACE_LINES_MAX = 8
/** trace/lines 默认请求行数（固件上限 8）。 */
export const CURVE_LOCAL_TRACE_LINES_DEFAULT = CURVE_LOCAL_TRACE_LINES_MAX
/** UI 显示窗口最多保留的行数（行本身来自固件，不拼接/伪造）。 */
export const CURVE_LOCAL_TRACE_DISPLAY_LINES = 500
/** 固件 task-57：帧指针 backtrace（最近一次 trigger 命中）。 */
export const CURVE_LOCAL_BACKTRACE_PATH = '/api/curve/backtrace'
/**
 * 固件 trace ring 容量（`CURVE_TRACE_RING_BYTES`）。只用于 UI 边界说明：
 * 115200 波特下约 0.17 s 目标输出，不是全量日志。
 */
export const CURVE_LOCAL_TRACE_RING_BYTES = 2048
/** 固件帧指针 backtrace 最大帧数（`CURVE_BACKTRACE_MAX_FRAMES`）。 */
export const CURVE_LOCAL_MAX_BACKTRACE_FRAMES = 8

/** 固件契约限制（docs §1.1）。 */
export const CURVE_LOCAL_MIN_SERIES = 1
/** 单组系列上限（兼容旧接口名；v2 多组见 MAX_TOTAL_SERIES）。 */
export const CURVE_LOCAL_MAX_SERIES = 64
export const CURVE_LOCAL_MAX_GROUPS = 8
/** 固件 task-51：每组上限 64 路，总上限 512 路。 */
export const CURVE_LOCAL_MAX_SERIES_PER_GROUP = 64
export const CURVE_LOCAL_MAX_TOTAL_SERIES = 256
export const CURVE_LOCAL_MAX_VARIANTS = 16
export const CURVE_LOCAL_MAX_SERIES_SIZE = 8
export const CURVE_LOCAL_MAX_SAMPLE_BYTES = 256
export const CURVE_LOCAL_MIN_INTERVAL_US = 1000
export const CURVE_LOCAL_MAX_INTERVAL_US = 200000
/**
 * 固件 task-60：series.sample_every（别名 divisor）范围 1..64。
 * 语义 = 该 series 所在 group 每 N 个 tick 采一次；有效刷新率 = group tick 率 / N。
 */
export const CURVE_LOCAL_MAX_SAMPLE_EVERY = 64
/** 固件 task-60：DWT comparator 上限（watch/watch2/watch3/watch4 或 watchpoints 数组）。 */
export const CURVE_LOCAL_MAX_WATCHPOINTS = 4
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
/**
 * 固件 task-60：flags bit4 = 本批存在 stale（未采样）series。
 * 置位时在**所有帧之后**追加 count × u64 LE footer，按帧顺序：
 * bit i（0..63）= 该帧所属 group 内第 i 个 series 本次 tick 是否 valid。
 * 帧布局不变 → 旧解码器忽略尾部仍可读；无 bit4 时 Web 视为全 valid。
 */
const CVF_FLAG_SERIES_MASK = 0x10
/** 每帧 valid 位图字节数（u64 LE）。 */
const CVF_SERIES_MASK_BYTES = 8

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
    /**
     * 固件 task-60：每 N 个 group tick 采样一次该 series（1..64，默认 1）。
     * 序列化为 `sample_every`（固件同时接受别名 `divisor`，sample_every 优先）。
     * 未采样 tick 的 payload 字节为 0，并在 CVF2 per-series valid mask 中标 stale。
     */
    sampleEvery?: number
}

/** 协议 v2 分组（每组 ≤32 路，总和 ≤256）。 */
/**
 * DWT watchpoint 访问类型（固件 task-57）：
 *   write = DWT_FUNCTION 6（store）、read = 5（load）、readwrite = 7（load/store）。
 */
export type CurveLocalWatchAccess = 'write' | 'read' | 'readwrite'
export interface CurveLocalWatchpointSpec {
    address: number
    size: 1 | 2 | 4
    access: CurveLocalWatchAccess
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
    /**
     * 函数快照（固件 task-57，仅 breakpoint）：首次 FPB 命中后记录 trigger frame，
     * 再采集 N 帧后自动停止并保留 ring（0..32）。字段缺省 = 旧的连续断点语义；
     * 旧固件会忽略该字段，Web 侧仍按“首帧冻结”降级。
     */
    snapshotAfter?: number
    /** watchpoint 触发配置（task-38；trigger='watchpoint' 时必填）。 */
    watchpoint?: CurveLocalWatchpointSpec
    /**
     * 第二路 DWT comparator（固件 task-57，可选；仅 trigger='watchpoint'）。
     * 目标需至少 2 个空闲 comparator，否则 start 返回 409/500 且 err 含 DWT_CTRL
     * 实际值（UI 原样展示固件错误，不猜测资源是否够用）。
     */
    watch2?: CurveLocalWatchpointSpec
    /** 固件 task-60：第三/四路 DWT comparator 便捷别名（同 watch2 格式）。 */
    watch3?: CurveLocalWatchpointSpec
    watch4?: CurveLocalWatchpointSpec
    /**
     * 固件 task-60 canonical：最多 4 路 watchpoint 数组（仅 trigger='watchpoint'）。
     * 显式提供时优先于 watch/watch2/watch3/watch4 便捷字段（固件侧数组优先、
     * 旧字段补位）。超过 2 路时必须用本字段（旧固件会忽略未知 root 字段）。
     */
    watchpoints?: readonly CurveLocalWatchpointSpec[]
    /**
     * 帧指针 backtrace（固件 task-57，原型；仅 breakpoint/watchpoint 触发）。
     * 命中后固件读 r7 沿 AAPCS 帧指针链展开（≤8 PC），GET /api/curve/backtrace 取回。
     */
    backtrace?: boolean
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
    /** 固件 task-60：本会话 series.sample_every 生效最大值（旧固件缺省）。 */
    sampleEveryMax?: number
    /** 固件 start 响应实际配置的 watchpoint 数 0..4（旧固件可能缺省）。 */
    watchCount?: number
    /** 实际使用的请求编码：CBS1 二进制（新固件）或旧 JSON start（回退）。 */
    transport?: 'bin' | 'json'
    /** CBS1 请求体字节数（transport='bin' 时）。 */
    requestBytes?: number
    /** 从 CBS1 回退到 JSON 的原因（旧固件 404/400/415 等；未回退则缺省）。 */
    binFallbackReason?: string
}

/** 单路 DWT watchpoint 状态（canonical `status.watchpoints` 数组元素，宽松解析）。 */
export interface LocalCurveWatchpointStatus {
    /** 1 起的 comparator 序号（固件未给时按数组下标 +1）。 */
    index: number
    armed?: boolean
    address?: number
    size?: number
    access?: CurveLocalWatchAccess
    hits?: number
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
    watchpointAccess?: CurveLocalWatchAccess
    watchpointHits?: number
    /** task-57：配置的 watchpoint 数（1/2）与第二路 comparator 状态。 */
    watchCount?: number
    watchpoint2Armed?: boolean
    watchpoint2Address?: number
    watchpoint2Hits?: number
    /** 最近一次命中的 watchpoint 序号（1/2；0 = 无命中）。 */
    lastWatchIndex?: number
    /** 固件 task-60：第三/四路 comparator 状态（watch_count 0..4，last_watch_index 1..4）。 */
    watchpoint3Armed?: boolean
    watchpoint3Address?: number
    watchpoint3Hits?: number
    watchpoint4Armed?: boolean
    watchpoint4Address?: number
    watchpoint4Hits?: number
    /** 固件 task-60（若提供）：canonical watchpoints 数组状态的宽松解析。 */
    watchpoints?: LocalCurveWatchpointStatus[]
    triggerSeq?: number
    /** 固件 task-60：本会话 series.sample_every 生效最大值（旧固件缺省）。 */
    sampleEveryMax?: number
    /** 固件 task-60：本会话累计 stale 系列槽数（未采样 tick）。 */
    seriesStale?: number
    /** 固件 task-60：设备堆余量（旧固件缺省）。 */
    heapFree?: number
    heapLargest?: number
    /** task-57：目标 UART trace 捕获状态（trace/start 订阅 UART RX）。 */
    traceActive?: boolean
    traceBytes?: number
    traceDropped?: number
    /** 固件 task-60：trace v2 行接口（lines/t_us/触发）状态；旧固件缺省。 */
    traceFormat?: number
    traceLines?: number
    traceDroppedLines?: number
    traceTriggered?: boolean
    traceTriggerSeq?: number
    /** task-57：最近一次 trigger 命中的帧指针 backtrace 摘要。 */
    backtraceValid?: boolean
    backtraceCount?: number
    /** task-35：固件最近一次 PC（pc_available=true 时有效）。 */
    pcAvailable?: boolean
    lastPc?: number
    /** task-45 条件触发状态。 */
    conditionArmed?: boolean
    conditionMet?: boolean
    conditionHits?: number
    conditionTriggerSeq?: number
    /** 函数快照（固件 task-57）：off/armed/capturing/complete/aborted；旧固件缺省。 */
    snapshotState?: 'off' | 'armed' | 'capturing' | 'complete' | 'aborted'
    /** 请求的后置帧数；旧固件缺省。 */
    snapshotAfter?: number
    /** 快照触发帧 device seq。 */
    snapshotTriggerSeq?: number
    /** 命中瞬间 ring 保留的预触发帧数。 */
    snapshotPreFrames?: number
    /** 已采集的后置帧数（capturing/complete）。 */
    snapshotPostFrames?: number
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
    /**
     * 固件 task-60：本帧 per-series valid 位图（group 内 series 序号，bit i=1 valid）。
     * 仅当批次 flags bit4=SERIES_MASK 时存在；缺省 = 全部有效（旧 CVF2）。
     * lo 为 bit0..31，hi 为 bit32..63（JS 位运算 u32，避免 BigInt）。
     */
    validMask?: {lo: number; hi: number}
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
    /** 固件 task-60：flags bit4（本批带 per-series valid/stale 位图）。 */
    seriesMask?: boolean
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

/** 读取一帧中某 series 槽位是否 valid（无 validMask = 旧格式，一律 valid）。 */
export function isFrameSeriesValid(frame: CurveLocalFrame, slot: number): boolean {
    const mask = frame.validMask
    if (!mask) return true
    if (!Number.isInteger(slot) || slot < 0 || slot > 63) return false
    return slot < 32
        ? ((mask.lo >>> slot) & 1) !== 0
        : ((mask.hi >>> (slot - 32)) & 1) !== 0
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
    error: 'empty' | 'too_many_series' | 'too_many_bytes' | null
}

/**
 * 按每组 perGroup 路切分；可选按字节上限再切（固件每组 sum(size)≤256 B）。
 * 默认 64/组、最多 8 组 = 总 512 路。
 */
export function splitSeriesIntoGroups<T>(
    series: readonly T[],
    perGroup = CURVE_LOCAL_MAX_SERIES_PER_GROUP,
    maxGroups = CURVE_LOCAL_MAX_GROUPS,
    options: {sizeOf?: (item: T) => number; maxBytes?: number; maxTotal?: number} = {},
): SeriesGroupSplit<T> {
    const totalLimit = Math.min(perGroup * maxGroups, options.maxTotal ?? CURVE_LOCAL_MAX_TOTAL_SERIES)
    const maxBytes = options.maxBytes ?? CURVE_LOCAL_MAX_SAMPLE_BYTES
    const groups: T[][] = []
    let current: T[] = []
    let currentBytes = 0
    for (const item of series) {
        const itemBytes = options.sizeOf ? Math.max(1, options.sizeOf(item)) : 0
        if (current.length > 0 && (current.length >= perGroup || (options.sizeOf && currentBytes + itemBytes > maxBytes))) {
            groups.push(current)
            current = []
            currentBytes = 0
        }
        current.push(item)
        currentBytes += itemBytes
    }
    if (current.length > 0) groups.push(current)
    if (series.length === 0) return {groups: [], groupCount: 0, error: 'empty'}
    if (options.sizeOf && series.some((item) => Math.max(1, options.sizeOf!(item)) > maxBytes)) {
        return {groups: groups.slice(0, maxGroups), groupCount: groups.length, error: 'too_many_bytes'}
    }
    if (series.length > totalLimit || groups.length > maxGroups) {
        return {groups: groups.slice(0, maxGroups), groupCount: groups.length, error: 'too_many_series'}
    }
    return {groups, groupCount: groups.length, error: null}
}

// ---------------------------------------------------------------------------
// 每变量目标刷新率（divisor 分桶 + group weight）
// ---------------------------------------------------------------------------
/**
 * 每变量目标刷新率分频（相对全局 interval 的“采样节拍数”）：1=全速（1/interval），
 * 2/4/8 表示每 2/4/8 个全局节拍采样一次。固件没有独立的 divisor 字段，Web 通过
 * “按 divisor 分桶 → 每组一个 group → 组权重 ∝ 1/divisor”实现（契约 §9.7）。
 */
export const CURVE_LOCAL_RATE_DIVISORS = [1, 2, 4, 8] as const
export type CurveLocalRateDivisor = (typeof CURVE_LOCAL_RATE_DIVISORS)[number]

/** 固件 group weight 上限（契约 §9.7，1..8；sum(weight)≤64）。 */
export const CURVE_LOCAL_MAX_GROUP_WEIGHT = 8
/** 函数快照一次性采集（固件 task-57）后置帧上限。 */
export const CURVE_LOCAL_MAX_SNAPSHOT_AFTER = 32

/** 把任意输入规范化为受支持的 divisor（非法值回退 1=全速）。 */
export function normalizeRateDivisor(value: unknown): CurveLocalRateDivisor {
    const numeric = typeof value === 'number' && Number.isFinite(value) ? Math.round(value) : 1
    return (CURVE_LOCAL_RATE_DIVISORS as readonly number[]).includes(numeric)
        ? numeric as CurveLocalRateDivisor
        : 1
}

/**
 * divisor → group weight（相对权重）。divisor 1/2/4/8 对应 8/4/2/1，
 * 组帧率比 = weight 比 = 1/divisor 比；固件按 weight 展开调度表轮转。
 */
export function rateDivisorWeight(divisor: unknown): number {
    return Math.max(1, Math.min(CURVE_LOCAL_MAX_GROUP_WEIGHT, Math.round(8 / normalizeRateDivisor(divisor))))
}

// ---------------------------------------------------------------------------
// 固件 task-60：per-series sample_every（精确目标 Hz）
// ---------------------------------------------------------------------------
/** 把任意输入规范化为 1..64 的 sample_every（非法值回退 1=每 tick）。 */
export function normalizeSampleEvery(value: unknown): number {
    const numeric = typeof value === 'number' && Number.isFinite(value) ? Math.round(value) : 1
    return Math.max(1, Math.min(CURVE_LOCAL_MAX_SAMPLE_EVERY, numeric))
}

/** sample_every → 该 series 的实际刷新率（Hz）；groupTickHz 为该 series 所在 group 的 tick 率。 */
export function sampleEveryRateHz(groupTickHz: number, sampleEvery: unknown): number {
    if (!Number.isFinite(groupTickHz) || groupTickHz <= 0) return 0
    return groupTickHz / normalizeSampleEvery(sampleEvery)
}

export interface SampleEveryPlan {
    /** 下发固件的 sample_every（1..64 整数）。 */
    sampleEvery: number
    /** 按该 sample_every 得到的实际刷新率（Hz）。 */
    actualHz: number
    /** 目标 Hz（原样返回，便于 UI 对照）。 */
    targetHz: number
    /** 目标高于 group tick 率时无法更快：sample_every=1，planCapped=true。 */
    capped: boolean
    /** 输入无效（非正数/非有限）时 sampleEvery=1、actualHz=0；调用方应清除目标。 */
    invalid: boolean
}

/**
 * 目标 Hz → sample_every 计划（纯函数，整数取整）。
 *
 *   sample_every = clamp(round(groupTickHz / targetHz), 1..64)
 *   actualHz     = groupTickHz / sample_every
 *
 * 固件一个 group 每 tick 采样一次、sample_every=N 时每 N tick 采该 series，
 * 所以目标 Hz 只能取 groupTickHz / N 的离散档位；UI 必须同时显示 actualHz
 * （不把 target 当作实际值展示）。目标高于 tick 率时 capped=true。
 */
export function planSampleEvery(targetHz: number, groupTickHz: number, maxSampleEvery = CURVE_LOCAL_MAX_SAMPLE_EVERY): SampleEveryPlan {
    const max = Math.max(1, Math.min(CURVE_LOCAL_MAX_SAMPLE_EVERY, Math.round(maxSampleEvery) || 1))
    const safeTarget = Number.isFinite(targetHz) ? targetHz : 0
    const safeTick = Number.isFinite(groupTickHz) && groupTickHz > 0 ? groupTickHz : 0
    if (safeTarget <= 0 || safeTick <= 0) {
        return {sampleEvery: 1, actualHz: 0, targetHz: safeTarget, capped: false, invalid: true}
    }
    const raw = safeTick / safeTarget
    const sampleEvery = Math.max(1, Math.min(max, Math.round(raw)))
    return {
        sampleEvery,
        actualHz: safeTick / sampleEvery,
        targetHz: safeTarget,
        capped: safeTarget > safeTick,
        invalid: false,
    }
}

/** 一组 series 的 sample_every 最大值（固件 start 响应的 sample_every_max 语义）。 */
export function maxSampleEvery(specs: readonly {sampleEvery?: number}[]): number {
    let max = 1
    for (const spec of specs) {
        if (spec.sampleEvery === undefined) continue
        max = Math.max(max, normalizeSampleEvery(spec.sampleEvery))
    }
    return max
}

/** 每变量刷新率计划错误（超能力时明确拒绝，不伪造数据）。 */
export type CurveLocalRatePlanError = 'empty' | 'too_many_series' | 'too_many_groups' | 'too_many_bytes'

/** 一个按目标刷新率分桶后的固件 group。 */
export interface LocalRateGroup<T> {
    /** 固件 group id（按 divisor 升序、桶内原始选择顺序）。 */
    id: number
    divisor: CurveLocalRateDivisor
    /** 采样调度权重（1..8，∝ 1/divisor）。 */
    weight: number
    items: T[]
}

/** 分桶结果：groups + 每组帧份额（weight/sumWeight）+ 首个错误。 */
export interface LocalRateGroupPlan<T> {
    groups: LocalRateGroup<T>[]
    groupCount: number
    seriesCount: number
    totalWeight: number
    /** 每组帧份额（0..1，Σ=1）；实际组帧率 ≈ 总帧率 × share。 */
    shares: number[]
    /** 首个错误；null = 可下发（容量允许）。 */
    error: CurveLocalRatePlanError | null
}

export interface PlanLocalRateGroupsOptions<T> {
    /** 每个 series 的 payload 字节数（用于每组 sum(size)≤maxBytes）。 */
    sizeOf?: (item: T) => number
    /** 每个 series 的目标 divisor（1/2/4/8；缺省 1=全局）。 */
    divisorOf?: (item: T) => number | undefined
    perGroup?: number
    maxGroups?: number
    maxBytes?: number
    maxTotal?: number
}

/**
 * 按目标刷新率分桶并切分为固件 groups。
 *
 * 算法（确定性、纯函数）：
 *  1. 按 normalizeRateDivisor(divisorOf(item)) 分桶，桶按 divisor 升序（快的在前），
 *     桶内保持调用方顺序；未设置 divisor 的曲线一律进 divisor=1 桶（默认行为不变）。
 *  2. 每个桶内部按 perGroup（≤64）与 sum(size)（≤maxBytes=256B）继续切分；同一桶的
 *     多个 subgroup 使用同一 weight（即同一个目标 rate）。
 *  3. weight = 8/divisor（1→8, 2→4, 4→2, 8→1）；组帧率 share = weight / Σweight。
 *     例：每个 divisor 一个组 → 权重 8/4/2/1，share 8/15、4/15、2/15、1/15；
 *     只有 divisor=1 的默认选择 → 每组 weight 8、share=1/G，与旧的等权轮转一致。
 *  4. 超能力（>maxGroups / >maxTotal / 无法切分的字节超限）返回 error 与完整
 *     groups（截断由调用方决定），绝不静默丢系列或伪造速率。
 *
 * 注意：固件一个 tick 只采样一个 group，所以所有组 share 之和恒为 1；当仅有一个
 * divisor>1 的桶（单组）时 share=1，实际仍是全局节拍速率——调用方必须把
 * “目标速率 ≠ 实际速率”如实展示（可增大全局 interval 来降低绝对速率）。
 */
export function planLocalRateGroups<T>(
    series: readonly T[],
    options: PlanLocalRateGroupsOptions<T> = {},
): LocalRateGroupPlan<T> {
    const perGroup = Math.max(1, options.perGroup ?? CURVE_LOCAL_MAX_SERIES_PER_GROUP)
    const maxGroups = Math.max(1, options.maxGroups ?? CURVE_LOCAL_MAX_GROUPS)
    const maxBytes = Math.max(1, options.maxBytes ?? CURVE_LOCAL_MAX_SAMPLE_BYTES)
    const maxTotal = Math.max(1, options.maxTotal ?? CURVE_LOCAL_MAX_TOTAL_SERIES)
    const sizeOf = options.sizeOf ?? (() => 1)
    const divisorOf = options.divisorOf ?? (() => 1)

    const buckets = new Map<CurveLocalRateDivisor, T[]>()
    for (const item of series) {
        const divisor = normalizeRateDivisor(divisorOf(item))
        const bucket = buckets.get(divisor)
        if (bucket) bucket.push(item)
        else buckets.set(divisor, [item])
    }

    const groups: LocalRateGroup<T>[] = []
    let error: CurveLocalRatePlanError | null = null
    const divisors = [...buckets.keys()].sort((left, right) => left - right)
    for (const divisor of divisors) {
        const weight = rateDivisorWeight(divisor)
        let current: T[] = []
        let currentBytes = 0
        for (const item of buckets.get(divisor) ?? []) {
            const size = Math.max(1, Math.floor(sizeOf(item)))
            if (size > maxBytes) error = error ?? 'too_many_bytes'
            if (current.length > 0 && (current.length >= perGroup || currentBytes + size > maxBytes)) {
                groups.push({id: groups.length, divisor, weight, items: current})
                current = []
                currentBytes = 0
            }
            current.push(item)
            currentBytes += size
        }
        if (current.length > 0) groups.push({id: groups.length, divisor, weight, items: current})
    }

    const totalWeight = groups.reduce((sum, group) => sum + group.weight, 0)
    const shares = groups.map((group) => (totalWeight > 0 ? group.weight / totalWeight : 0))
    if (series.length === 0) error = error ?? 'empty'
    else if (series.length > maxTotal) error = error ?? 'too_many_series'
    if (groups.length > maxGroups) error = error ?? 'too_many_groups'
    return {
        groups,
        groupCount: groups.length,
        seriesCount: series.length,
        totalWeight,
        shares,
        error,
    }
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

/** DWT watchpoint 访问类型枚举（固件 curve_watch_access_t 的字符串形式）。 */
export function isCurveLocalWatchAccess(value: unknown): value is CurveLocalWatchAccess {
    return value === 'write' || value === 'read' || value === 'readwrite'
}

/** 校验一路 watchpoint 配置（地址非 0、size 1/2/4、按 size 对齐、access 合法）。 */
function validateWatchpointSlot(watchpoint: CurveLocalWatchpointSpec | undefined, label: string): void {
    if (!watchpoint) throw new CurveLocalError(`${label} config required for watchpoint trigger`)
    const address = watchpoint.address >>> 0
    if (!Number.isInteger(address) || address === 0) throw new CurveLocalError(`${label} address must be non-zero u32`)
    if (![1, 2, 4].includes(watchpoint.size)) throw new CurveLocalError(`${label} size must be 1, 2 or 4`)
    if (address % watchpoint.size !== 0) throw new CurveLocalError(`${label} address must be aligned to size`)
    if (!isCurveLocalWatchAccess(watchpoint.access)) {
        throw new CurveLocalError(`${label} access must be 'write', 'read' or 'readwrite'`)
    }
}

/** 两路 watchpoint 完全相同（地址+size+access）时固件返回 400；Web 提前拒绝。 */
export function isDuplicateWatchpoint(
    first: CurveLocalWatchpointSpec | undefined,
    second: CurveLocalWatchpointSpec | undefined,
): boolean {
    if (!first || !second) return false
    return (first.address >>> 0) === (second.address >>> 0)
        && first.size === second.size
        && first.access === second.access
}

/**
 * 归一化 watchpoint 配置（固件 task-60，最多 4 路）：
 *   显式 `watchpoints` 数组优先；否则按 watchpoint/watch2/watch3/watch4 便捷字段
 *   依序补位（旧请求 watch/watch2 行为不变）。
 * 返回数组只含已提供的槽位（可能为空）；不在这里做合法性判断。
 */
export function normalizeWatchpoints(options: StartLocalCurveOptions): CurveLocalWatchpointSpec[] {
    if (options.watchpoints && options.watchpoints.length > 0) return [...options.watchpoints]
    return [options.watchpoint, options.watch2, options.watch3, options.watch4]
        .filter((item): item is CurveLocalWatchpointSpec => item !== undefined && item !== null)
}

/** 固件能力探测结果（基于 status 回读字段；旧固件缺省 → false，走兼容路径）。 */
export interface LocalCurveCapabilities {
    /** series.sample_every（精确每变量速率）可用。 */
    sampleEvery: boolean
    /** watchpoints 数组 / 最多 4 路 DWT comparator 可用。 */
    watchpoints4: boolean
    /** trace v2 行接口（/api/curve/trace/lines）可能可用。 */
    traceLines: boolean
}

/**
 * 从 status 字段探测固件新能力。旧固件不返回 `sample_every_max` 等字段：
 * Web 据此选择“旧 groups+weight divisor 分桶”fallback，而不是假设字段被接受。
 */
export function detectLocalCurveCapabilities(status: LocalCurveStatus | null | undefined): LocalCurveCapabilities {
    const sampleEveryMax = status?.sampleEveryMax
    const watchCount = status?.watchCount
    const traceFormat = status?.traceFormat
    return {
        sampleEvery: typeof sampleEveryMax === 'number' && Number.isFinite(sampleEveryMax) && sampleEveryMax >= 1,
        watchpoints4: (typeof watchCount === 'number' && watchCount > 2)
            || status?.watchpoint3Armed !== undefined
            || status?.watchpoint4Armed !== undefined,
        traceLines: (typeof traceFormat === 'number' && traceFormat >= 2)
            || (typeof status?.traceLines === 'number' && status.traceLines >= 0),
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
        if (options.snapshotAfter !== undefined) {
            if (!Number.isInteger(options.snapshotAfter)
                || options.snapshotAfter < 0 || options.snapshotAfter > CURVE_LOCAL_MAX_SNAPSHOT_AFTER) {
                throw new CurveLocalError(`snapshot_after must be 0..${CURVE_LOCAL_MAX_SNAPSHOT_AFTER} for breakpoint trigger`)
            }
        }
    } else if (options.snapshotAfter !== undefined) {
        throw new CurveLocalError('snapshot_after requires breakpoint trigger')
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
        // 固件 task-60：watchpoints 数组（canonical，最多 4 路）或 watch/watch2/watch3/watch4
        // 便捷字段；本地只校验格式/重复，DWT comparator 资源是否足够由固件原文报错。
        const watchpoints = normalizeWatchpoints(options)
        if (watchpoints.length === 0) {
            throw new CurveLocalError('watchpoint config required for watchpoint trigger')
        }
        if (watchpoints.length > CURVE_LOCAL_MAX_WATCHPOINTS) {
            throw new CurveLocalError(`watchpoints count must be 1..${CURVE_LOCAL_MAX_WATCHPOINTS}`)
        }
        watchpoints.forEach((watchpoint, index) => {
            validateWatchpointSlot(watchpoint, index === 0 ? 'watchpoint' : `watch${index + 1}`)
        })
        for (let left = 0; left < watchpoints.length; left++) {
            for (let right = left + 1; right < watchpoints.length; right++) {
                if (isDuplicateWatchpoint(watchpoints[left], watchpoints[right])) {
                    throw new CurveLocalError(
                        left === 0 && right === 1
                            ? 'watch2 duplicates the first watchpoint'
                            : `watch${right + 1} duplicates watch${left + 1}`,
                    )
                }
            }
        }
    } else {
        if (options.watch2 !== undefined) throw new CurveLocalError('watch2 requires trigger="watchpoint"')
        if (options.watch3 !== undefined || options.watch4 !== undefined || (options.watchpoints?.length ?? 0) > 0) {
            throw new CurveLocalError('watch3/watch4/watchpoints require trigger="watchpoint"')
        }
    }
    if (options.backtrace !== undefined) {
        if (typeof options.backtrace !== 'boolean') throw new CurveLocalError('backtrace must be a boolean')
        if (options.backtrace && trigger !== 'breakpoint' && trigger !== 'watchpoint') {
            throw new CurveLocalError('backtrace requires trigger="breakpoint" or "watchpoint"')
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
            if (item.sampleEvery !== undefined
                && (!Number.isInteger(item.sampleEvery) || item.sampleEvery < 1 || item.sampleEvery > CURVE_LOCAL_MAX_SAMPLE_EVERY)) {
                // 固件 task-60；旧固件忽略该字段（Web 侧用能力探测走 fallback）。
                throw new CurveLocalError(`series[${item.id}] sample_every must be 1..${CURVE_LOCAL_MAX_SAMPLE_EVERY}`)
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

function optionalBoolean(value: unknown): boolean | undefined {
    return value === true ? true : value === false ? false : undefined
}

function parseSnapshotState(value: unknown): LocalCurveStatus['snapshotState'] {
    return value === 'armed' || value === 'capturing' || value === 'complete' || value === 'aborted' || value === 'off'
        ? value
        : undefined
}

function parseWatchAccess(value: unknown): CurveLocalWatchAccess | undefined {
    return value === 'readwrite' ? 'readwrite' : value === 'write' ? 'write' : value === 'read' ? 'read' : undefined
}

/** 宽松解析 canonical `status.watchpoints` 数组；非数组/坏项直接忽略（不猜测）。 */
function parseWatchpointStatusArray(value: unknown): LocalCurveWatchpointStatus[] | undefined {
    if (!Array.isArray(value)) return undefined
    const entries: LocalCurveWatchpointStatus[] = []
    value.forEach((item, index) => {
        if (typeof item !== 'object' || item === null) return
        const raw = item as Record<string, unknown>
        const entry: LocalCurveWatchpointStatus = {
            index: optionalFiniteNumber(raw.index) ?? index + 1,
            armed: optionalBoolean(raw.armed),
            address: optionalFiniteNumber(raw.address),
            size: optionalFiniteNumber(raw.size),
            access: parseWatchAccess(raw.access),
            hits: optionalFiniteNumber(raw.hits),
        }
        entries.push(entry)
    })
    return entries.length > 0 ? entries : undefined
}

/** 启动设备本地采样。 */
/** 序列化一个 series（v2：variants-only 或 expr+pc_low/pc_high；task-60 可带 sample_every）。 */
function serializeSeriesSpec(item: CurveLocalSeriesSpec): Record<string, unknown> {
    const spec: Record<string, unknown> = {size: item.size}
    if (item.sampleEvery !== undefined) {
        // 固件 task-60；=1 时省略保持旧请求字节数不变（旧固件忽略未知字段）。
        const sampleEvery = normalizeSampleEvery(item.sampleEvery)
        if (sampleEvery > 1) spec.sample_every = sampleEvery
    }
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
        if (options.snapshotAfter !== undefined) {
            // 固件 task-57：有该字段 = 一次性函数快照（命中→N 后置帧→自动停止）；
            // 旧固件忽略未知字段，Web 端按“首帧冻结”降级，不伪造固件快照。
            payload.snapshot_after = options.snapshotAfter
        }
    } else if (trigger === 'watchpoint') {
        // 固件 task-60 两种编码互斥（curve_json.c parse_watchpoints）：
        //  - canonical: `watchpoints` 数组（≤4，含 slot0），不能再带 watch2/watch3/watch4；
        //  - legacy: root watch_address/watch_size/watch_access（slot0）+ watch2/watch3/
        //    watch4 对象（slot1..3，要求 slot 连续）。
        // 显式传 watchpoints 数组 => canonical only；否则 legacy（旧固件忽略未知的
        // watch3/watch4，仍按 1-2 路工作；新固件支持全部 4 路别名）。
        const watchpoints = normalizeWatchpoints(options)
        if (options.watchpoints && options.watchpoints.length > 0) {
            payload.watchpoints = watchpoints.map((watchpoint) => ({
                address: watchpoint.address >>> 0,
                size: watchpoint.size,
                access: watchpoint.access,
            }))
        } else {
            watchpoints.forEach((watchpoint, index) => {
                const serialized = {
                    address: watchpoint.address >>> 0,
                    size: watchpoint.size,
                    access: watchpoint.access,
                }
                if (index === 0) {
                    payload.watch_address = serialized.address
                    payload.watch_size = serialized.size
                    payload.watch_access = serialized.access
                } else if (index === 1) {
                    payload.watch2 = serialized
                } else if (index === 2) {
                    payload.watch3 = serialized
                } else if (index === 3) {
                    payload.watch4 = serialized
                }
            })
        }
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
    if (options.backtrace === true) {
        // task-57：仅在显式开启时下发；旧固件忽略未知字段（命中时无 backtrace 数据）。
        payload.backtrace = true
    }
    const body = await requestJson(CURVE_LOCAL_START_PATH, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload),
    })
    return {...mapStartResponse(body, groups, options), transport: 'json'}
}

/** 把 `/api/curve/start` 与 `/api/curve/start-bin` 相同的成功响应映射为结果。 */
function mapStartResponse(
    body: Record<string, unknown>,
    groups: readonly CurveLocalGroupSpec[],
    options: StartLocalCurveOptions,
): StartLocalCurveResult {
    const totalSeries = groups.reduce((sum, group) => sum + group.series.length, 0)
    const maxGroupBytes = groups.reduce((max, group) => Math.max(max, group.series.reduce((sum, item) => sum + item.size, 0)), 0)
    return {
        groupCount: finiteNumber(body.group_count, groups.length),
        seriesCount: finiteNumber(body.series_count, totalSeries),
        sampleBytes: finiteNumber(body.sample_bytes, maxGroupBytes),
        intervalUs: finiteNumber(body.interval_us, options.intervalUs),
        // 固件 task-60：确认 sample_every 是否真正生效（旧固件无该字段 → undefined）。
        sampleEveryMax: optionalFiniteNumber(body.sample_every_max),
        // 固件 start 响应的 watch_count（用于识别旧固件忽略 W3/W4 的降级）。
        watchCount: optionalFiniteNumber(body.watch_count),
    }
}

// ---------------------------------------------------------------------------
// 固件 task-60（P0-1）：CBS1 流式二进制 start 请求编码
// ---------------------------------------------------------------------------
const CBS1_VERSION = 1
const CBS1_FLAG_HALT = 0x01
const CBS1_FLAG_WANT_PC = 0x02
const CBS1_FLAG_BACKTRACE = 0x04
const CBS1_FLAG_CONDITION = 0x08
const CBS1_FLAG_SNAPSHOT = 0x10
const CBS1_COND_FLAG_SIGNED = 0x01
const CBS1_COND_FLAG_STOP = 0x02
const CBS1_SERIES_FLAG_PC_GATE = 0x01
const CBS1_TRIGGER_CODES: Record<CurveLocalTrigger, number> = {periodic: 0, breakpoint: 1, watchpoint: 2}
/** 固件 curve_watch_access_t：WRITE=0, RW=1, READ=2（注意不是枚举顺序）。 */
const CBS1_WATCH_ACCESS_CODES: Record<CurveLocalWatchAccess, number> = {write: 0, readwrite: 1, read: 2}
/** 固件 curve_cond_op_t：EQ=0, NE=1, LT=2, LE=3, GT=4, GE=5。 */
const CBS1_COND_OP_CODES: Record<CurveConditionOp, number> = {'==': 0, '!=': 1, '<': 2, '<=': 3, '>': 4, '>=': 5}

/** 小端字节写入器（CBS1 全部 little-endian；body ≤16 KiB，number[] 足够）。 */
class Cbs1Writer {
    private bytes: number[] = []

    u8(value: number): this {
        this.bytes.push(value & 0xff)
        return this
    }

    u16(value: number): this {
        this.bytes.push(value & 0xff, (value >>> 8) & 0xff)
        return this
    }

    u32(value: number): this {
        const numeric = value >>> 0
        this.bytes.push(numeric & 0xff, (numeric >>> 8) & 0xff, (numeric >>> 16) & 0xff, (numeric >>> 24) & 0xff)
        return this
    }

    i32(value: number): this {
        return this.u32(value | 0)
    }

    raw(values: Uint8Array | readonly number[]): this {
        for (const value of values) this.bytes.push(value & 0xff)
        return this
    }

    /** u16 长度前缀 + ASCII expr_s（与 JSON 的 `expr_s` 语法完全一致）。 */
    expr(text: string): this {
        const encoded = new TextEncoder().encode(text)
        if (encoded.length > 0xffff) throw new CurveLocalError('CBS1 expression too long')
        this.u16(encoded.length)
        return this.raw(encoded)
    }

    get size(): number {
        return this.bytes.length
    }

    toUint8Array(): Uint8Array {
        return Uint8Array.from(this.bytes)
    }
}

/**
 * 编码固件 task-60 的 CBS1 二进制 start 请求（P0-1）。
 *
 * - 与 JSON `/api/curve/start` 语义完全等价（复用 `validateStartLocalCurve`
 *   与同一套 expr_s/variants/sample_every/watch/condition/snapshot 字段）；
 * - 4×64 series（256 路、expr `A<addr8>`）约 5936 B（与固件 host 测试一致），
 *   走流式解析、不占 16 KiB 连续堆；
 * - 单组无 variants 的旧请求在二进制里同样编码为 1 个 group（weight=1）。
 *
 * 布局（docs/curve-local-sampler.md §15.1 / curve_priv.h CURVE_BIN_MAGIC）：
 *   "CBS1"|version u8|flags u8|trigger u8|watch_count u8|interval_us u32|
 *   breakpoint_address u32|cond_address u32|cond_value i32|cond_size u8|
 *   cond_op u8|cond_mode u8|cond_flags u8|snapshot_after u8|group_count u8|
 *   reserved u16 | watch_count×{address u32,size u8,access u8} |
 *   group_count×{weight u8,series_count u8,reserved u16 | series_count×{
 *     size u8|flags u8(sample pc gate)|sample_every u8|variant_count u8|
 *     pc_low u32|pc_high u32|u16 expr_len|expr|variant_count×{pc_low u32,
 *     pc_high u32,u16 expr_len,expr}}}
 */
export function encodeStartBinCbs1(options: StartLocalCurveOptions): Uint8Array {
    validateStartLocalCurve(options)
    const trigger = options.trigger ?? 'periodic'
    const explicitGroups = (options.groups?.length ?? 0) > 0
    const groups: readonly CurveLocalGroupSpec[] = explicitGroups
        ? [...(options.groups ?? [])]
        : [{id: 0, weight: 1, series: options.series ?? []}]
    const hasVariants = groups.some((group) => group.series.some((item) => (item.variants?.length ?? 0) > 0))
    // 与 JSON 序列化一致：periodic 才显式控制 halt（variants 强制 halt）；breakpoint/
    // watchpoint 的 JSON 请求不带 halt（固件默认 true），这里同样编码 halt=1。
    const halt = trigger === 'periodic' ? (hasVariants ? true : options.halt !== false) : true
    let flags = 0
    if (halt) flags |= CBS1_FLAG_HALT
    if (options.wantPc !== false) flags |= CBS1_FLAG_WANT_PC
    if (options.backtrace === true) flags |= CBS1_FLAG_BACKTRACE
    const condition = options.triggerCondition ?? options.condition
    if (condition) flags |= CBS1_FLAG_CONDITION
    const snapshotEnabled = trigger === 'breakpoint' && options.snapshotAfter !== undefined
    if (snapshotEnabled) flags |= CBS1_FLAG_SNAPSHOT
    const watchpoints = trigger === 'watchpoint' ? normalizeWatchpoints(options) : []
    const writer = new Cbs1Writer()
    writer.raw([0x43, 0x42, 0x53, 0x31]) // "CBS1"
    writer.u8(CBS1_VERSION)
    writer.u8(flags)
    writer.u8(CBS1_TRIGGER_CODES[trigger])
    writer.u8(watchpoints.length)
    writer.u32(options.intervalUs)
    writer.u32(trigger === 'breakpoint' ? normalizeBreakpointAddress(options.breakpointAddress ?? 0) : 0)
    writer.u32(condition ? condition.address >>> 0 : 0)
    writer.i32(condition ? condition.value : 0)
    writer.u8(condition ? condition.size : 0)
    writer.u8(condition ? CBS1_COND_OP_CODES[condition.op] : 0)
    writer.u8(condition ? ((condition.mode ?? (condition.once === true ? 'once' : 'continuous')) === 'continuous' ? 1 : 0) : 0)
    let condFlags = 0
    if (condition?.signed === true) condFlags |= CBS1_COND_FLAG_SIGNED
    if (condition && (condition.stopAfter ?? condition.stop ?? true)) condFlags |= CBS1_COND_FLAG_STOP
    writer.u8(condFlags)
    writer.u8(snapshotEnabled ? Math.min(CURVE_LOCAL_MAX_SNAPSHOT_AFTER, Math.max(0, Math.round(options.snapshotAfter ?? 0))) : 0)
    writer.u8(groups.length)
    writer.u16(0)
    for (const watchpoint of watchpoints) {
        writer.u32(watchpoint.address >>> 0)
        writer.u8(watchpoint.size)
        writer.u8(CBS1_WATCH_ACCESS_CODES[watchpoint.access])
    }
    for (const group of groups) {
        writer.u8(group.weight ?? 1)
        writer.u8(group.series.length)
        writer.u16(0)
        for (const item of group.series) {
            const variants = item.variants ?? []
            const pcGate = variants.length === 0 && item.pcLow !== undefined && item.pcHigh !== undefined
            writer.u8(item.size)
            writer.u8(pcGate ? CBS1_SERIES_FLAG_PC_GATE : 0)
            writer.u8(normalizeSampleEvery(item.sampleEvery ?? 1))
            writer.u8(variants.length)
            writer.u32(pcGate ? item.pcLow! >>> 0 : 0)
            writer.u32(pcGate ? item.pcHigh! >>> 0 : 0)
            if (variants.length === 0) {
                if (!item.expr) throw new CurveLocalError('expr is required when variants are absent')
                writer.expr(encodeAddressExprCompact(item.expr))
            } else {
                // 与 JSON serializer 一致：variants-only series 不带 series 级 expr/pc gate。
                writer.u16(0)
                for (const variant of variants) {
                    writer.u32(variant.pcLow >>> 0)
                    writer.u32(variant.pcHigh >>> 0)
                    writer.expr(encodeAddressExprCompact(variant.expr))
                }
            }
        }
    }
    const bytes = writer.toUint8Array()
    if (bytes.length > CURVE_LOCAL_MAX_BIN_BODY) {
        throw new CurveLocalError(`CBS1 request is ${bytes.length} B (> firmware limit ${CURVE_LOCAL_MAX_BIN_BODY} B)`)
    }
    return bytes
}

/** 旧固件/不支持 start-bin 时回退 JSON 的 HTTP 状态集合（其余错误原样抛出）。 */
const CBS1_FALLBACK_STATUSES = new Set([400, 404, 405, 415, 501])

/**
 * 启动设备本地采样（P0-1）：优先 `POST /api/curve/start-bin`（CBS1 流式二进制，
 * 256 series ≈5.9 KiB），旧固件 404/400/415（以及 405/501）时自动回退现有 JSON
 * `/api/curve/start`（body ≤16384）。两者成功响应相同，结果带 `transport`。
 */
export async function startLocalCurveAuto(options: StartLocalCurveOptions): Promise<StartLocalCurveResult> {
    const groups = normalizeLocalGroups(options)
    const encoded = encodeStartBinCbs1(options)
    try {
        const body = await requestJson(CURVE_LOCAL_START_BIN_PATH, {
            method: 'POST',
            headers: {'Content-Type': 'application/octet-stream'},
            body: encoded,
        })
        return {
            ...mapStartResponse(body, groups, options),
            transport: 'bin',
            requestBytes: encoded.length,
        }
    } catch (error) {
        const status = error instanceof CurveLocalError ? error.httpStatus : 0
        if (!CBS1_FALLBACK_STATUSES.has(status)) throw error
        const reason = error instanceof Error ? error.message : 'start-bin unavailable'
        const result = await startLocalCurve(options)
        return {
            ...result,
            transport: 'json',
            requestBytes: JSON.stringify(options).length,
            binFallbackReason: `start-bin HTTP ${status}: ${reason}`,
        }
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
        watchpointAccess: body.watchpoint_access === 'readwrite' ? 'readwrite' : body.watchpoint_access === 'write' ? 'write' : body.watchpoint_access === 'read' ? 'read' : undefined,
        watchpointHits: optionalFiniteNumber(body.watchpoint_hits),
        // task-57：多 DWT comparator 状态（watch_count / watchpoint2_* / last_watch_index）。
        watchCount: optionalFiniteNumber(body.watch_count),
        watchpoint2Armed: optionalBoolean(body.watchpoint2_armed),
        watchpoint2Address: optionalFiniteNumber(body.watchpoint2_address),
        watchpoint2Hits: optionalFiniteNumber(body.watchpoint2_hits),
        // 固件 task-60：第三/四路 comparator 状态 + canonical watchpoints 数组（宽松解析）。
        watchpoint3Armed: optionalBoolean(body.watchpoint3_armed),
        watchpoint3Address: optionalFiniteNumber(body.watchpoint3_address),
        watchpoint3Hits: optionalFiniteNumber(body.watchpoint3_hits),
        watchpoint4Armed: optionalBoolean(body.watchpoint4_armed),
        watchpoint4Address: optionalFiniteNumber(body.watchpoint4_address),
        watchpoint4Hits: optionalFiniteNumber(body.watchpoint4_hits),
        watchpoints: parseWatchpointStatusArray(body.watchpoints),
        lastWatchIndex: optionalFiniteNumber(body.last_watch_index),
        triggerSeq: optionalFiniteNumber(body.trigger_seq),
        // 固件 task-60：per-series sample_every 生效值与累计 stale 槽数。
        sampleEveryMax: optionalFiniteNumber(body.sample_every_max),
        seriesStale: optionalFiniteNumber(body.series_stale),
        heapFree: optionalFiniteNumber(body.heap_free),
        heapLargest: optionalFiniteNumber(body.heap_largest),
        // task-57：目标 UART trace 捕获状态（status 摘要；字节流本身走 GET /api/curve/trace）。
        traceActive: optionalBoolean(body.trace_active),
        traceBytes: optionalFiniteNumber(body.trace_bytes),
        traceDropped: optionalFiniteNumber(body.trace_dropped),
        // 固件 task-60：trace v2 行接口状态（旧固件缺省）。
        traceFormat: optionalFiniteNumber(body.trace_format),
        traceLines: optionalFiniteNumber(body.trace_lines),
        traceDroppedLines: optionalFiniteNumber(body.trace_dropped_lines),
        traceTriggered: optionalBoolean(body.trace_triggered),
        traceTriggerSeq: optionalFiniteNumber(body.trace_trigger_seq),
        // task-57：最近一次 trigger 命中的帧指针 backtrace 摘要（PC 列表走 /api/curve/backtrace）。
        backtraceValid: optionalBoolean(body.backtrace_valid),
        backtraceCount: optionalFiniteNumber(body.backtrace_count),
        pcAvailable: optionalBoolean(body.pc_available),
        lastPc: optionalFiniteNumber(body.last_pc),
        conditionArmed: body.condition_armed === true ? true : body.condition_armed === false ? false : undefined,
        conditionMet: body.condition_met === true ? true : body.condition_met === false ? false : undefined,
        conditionHits: optionalFiniteNumber(body.condition_hits),
        conditionTriggerSeq: optionalFiniteNumber(body.condition_trigger_seq),
        snapshotState: parseSnapshotState(body.snapshot_state),
        snapshotAfter: optionalFiniteNumber(body.snapshot_after),
        snapshotTriggerSeq: optionalFiniteNumber(body.snapshot_trigger_seq),
        snapshotPreFrames: optionalFiniteNumber(body.snapshot_pre_frames),
        snapshotPostFrames: optionalFiniteNumber(body.snapshot_post_frames),
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
    if ((flags & CVF_FLAG_SERIES_MASK) !== 0) applySeriesMaskFooter(bytes, offset, frames, 'CVF1')
    return {version: 1, flags, triggered: (flags & CVF_FLAG_TRIGGER) !== 0,
      conditionTriggered: (flags & CVF_FLAG_CONDITION) !== 0,
      seriesMask: (flags & CVF_FLAG_SERIES_MASK) !== 0,
      gap: (flags & CVF_FLAG_GAP) !== 0, count, firstSeq, frameBytes, frames}
}

/**
 * 固件 task-60：flags bit4 时解析所有帧之后的 count × u64 LE valid 位图 footer。
 * 布局与帧 payload 无关（帧头/帧体不变）；截断时抛错（不把缺失位图当全 valid）。
 */
function applySeriesMaskFooter(bytes: Uint8Array, offset: number, frames: CurveLocalFrame[], label: string): void {
    if (frames.length === 0) return
    const needed = frames.length * CVF_SERIES_MASK_BYTES
    if (offset + needed > bytes.length) {
        throw new CurveLocalError(`${label} series mask footer truncated: need ${needed}B at ${offset}, got ${bytes.length}B body`)
    }
    frames.forEach((frame, index) => {
        const base = offset + index * CVF_SERIES_MASK_BYTES
        frame.validMask = {lo: readU32(bytes, base), hi: readU32(bytes, base + 4)}
    })
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
    // 固件 task-60：per-series valid/stale 位图在最后一帧之后；旧格式无 bit4 → 全 valid。
    if ((flags & CVF_FLAG_SERIES_MASK) !== 0) applySeriesMaskFooter(bytes, offset, frames, 'CVF2')
    return {version: 2, flags, triggered: (flags & CVF_FLAG_TRIGGER) !== 0,
      conditionTriggered: (flags & CVF_FLAG_CONDITION) !== 0,
      seriesMask: (flags & CVF_FLAG_SERIES_MASK) !== 0,
      gap: (flags & CVF_FLAG_GAP) !== 0, count, firstSeq, frameBytes, frames}
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

// ---------------------------------------------------------------------------
// task-57 调试能力（固件原型）：目标 UART trace + 帧指针 backtrace
// ---------------------------------------------------------------------------
/**
 * 目标 UART trace 一次 GET 响应。
 *
 * 固件契约（docs/curve-local-sampler.md §12.1）：
 *   GET /api/curve/trace?since=<byte seq> -> application/octet-stream
 *   响应头 X-Curve-Trace-Seq（下一次 since）/ -Active / -Gap / -Total / -Dropped；
 *   无新字节 -> 204（仍带响应头）。
 * 本函数只搬运字节与响应头，**不生成任何目标时间戳/行边界**（只有字节序号）。
 */
export interface CurveLocalTraceChunk {
    /** 原始目标 UART 字节（本次新增）。 */
    bytes: Uint8Array
    /** 下一次请求的 since（字节序号，不是时间戳）。 */
    nextSeq: number
    /** 捕获是否 active；响应头不可读时为 null（不猜测）。 */
    active: boolean | null
    /** since 已被 ring 覆盖（存在 gap）；响应头不可读时为 null。 */
    gap: boolean | null
    /** 自 trace/start 起累计字节数；响应头不可读时为 null。 */
    total: number | null
    /** ring 已覆盖字节数；响应头不可读时为 null。 */
    dropped: number | null
    /** 固件 X-Curve-Trace-* 响应头是否可读（dev 跨域未 expose 时为 false）。 */
    headersAvailable: boolean
}

export interface CurveLocalTraceStartResult {
    active: boolean
    /** 固件 ring 容量（字节）；固件未回传时 null。 */
    ringBytes: number | null
    /** 固件 task-60：trace 格式版本（1=旧 raw；2=raw+lines）；未回传时 null。 */
    format: number | null
}

/** trace/start 可选参数（固件 task-60：行触发；旧固件忽略未知 body 字段）。 */
export interface LocalCurveTraceStartOptions {
    /** 行包含该 PATTERN 时置 triggered/trigger_seq（固件 task-60；空/缺省=不触发）。 */
    trigger?: string
    /** 触发后自动停止捕获（需 trigger；缺省 false=继续捕获）。 */
    stopAfter?: boolean
}

/** 响应头 getter（Response.headers 兼容）。 */
export interface HeaderGetter {
    get(name: string): string | null
}

function headerU32(getHeader: HeaderGetter, name: string): number | null {
    const text = getHeader.get(name)
    if (text === null || text === undefined) return null
    const trimmed = String(text).trim()
    if (!/^\d{1,10}$/.test(trimmed)) return null
    const value = Number.parseInt(trimmed, 10)
    return Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff ? value : null
}

function headerFlag(getHeader: HeaderGetter, name: string): boolean | null {
    const text = getHeader.get(name)
    if (text === null || text === undefined) return null
    const trimmed = String(text).trim()
    if (trimmed === '1') return true
    if (trimmed === '0') return false
    return null
}

/**
 * 解析 trace 响应头（纯函数，便于单测）。
 * 头不可读时 `nextSeq = since + 本次字节数`（字节序号连续），并显式标记
 * `headersAvailable=false`（gap/active/dropped 不伪报，返回 null）。
 */
export function parseTraceResponseMeta(
    getHeader: HeaderGetter,
    since: number,
    byteLength: number,
): Omit<CurveLocalTraceChunk, 'bytes'> {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    const safeLength = Math.max(0, Math.floor(Number.isFinite(byteLength) ? byteLength : 0))
    const seq = headerU32(getHeader, 'X-Curve-Trace-Seq')
    return {
        nextSeq: seq ?? (safeSince + safeLength),
        active: headerFlag(getHeader, 'X-Curve-Trace-Active'),
        gap: headerFlag(getHeader, 'X-Curve-Trace-Gap'),
        total: headerU32(getHeader, 'X-Curve-Trace-Total'),
        dropped: headerU32(getHeader, 'X-Curve-Trace-Dropped'),
        headersAvailable: seq !== null,
    }
}

/**
 * 开始目标 UART trace 捕获（POST /api/curve/trace/start；会初始化 UART 服务）。
 * 固件 task-60：可选 body `{trigger, stop_after}` 启用行触发；无参数时保持旧行为
 * （旧固件忽略未知字段）。
 */
export async function startLocalCurveTrace(options: LocalCurveTraceStartOptions = {}): Promise<CurveLocalTraceStartResult> {
    const payload: Record<string, unknown> = {}
    const trigger = typeof options.trigger === 'string' ? options.trigger : ''
    if (trigger) {
        payload.trigger = trigger
        payload.stop_after = options.stopAfter === true
    }
    const hasBody = Object.keys(payload).length > 0
    const body = await requestJson(CURVE_LOCAL_TRACE_START_PATH, {
        method: 'POST',
        ...(hasBody ? {headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)} : {}),
    })
    return {
        active: body.active !== false,
        ringBytes: optionalFiniteNumber(body.ring_bytes) ?? null,
        format: optionalFiniteNumber(body.format) ?? null,
    }
}

/** 停止目标 UART trace 捕获（POST /api/curve/trace/stop；未启动时固件幂等返回 ok）。 */
export async function stopLocalCurveTrace(): Promise<void> {
    await requestJson(CURVE_LOCAL_TRACE_STOP_PATH, {method: 'POST'})
}

/** 拉取 since 之后的原始目标 UART 字节（204 = 无新数据，不伪造空文本/时间戳）。 */
export async function getLocalCurveTrace(since = 0): Promise<CurveLocalTraceChunk> {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    let response: Response
    try {
        response = await fetch(deviceApiUrl(`${CURVE_LOCAL_TRACE_PATH}?since=${safeSince}`), {
            method: 'GET',
            cache: 'no-store',
        })
    } catch (error) {
        throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
    }
    if (response.status !== 204 && !response.ok) {
        let text = ''
        try {
            text = await response.text()
        } catch {
            text = ''
        }
        const body = parseJsonText(text)
        throw new CurveLocalError(errorMessage(body, response.status, text), response.status, errorCode(body))
    }
    let bytes = new Uint8Array(0)
    if (response.status !== 204) {
        try {
            bytes = new Uint8Array(await response.arrayBuffer())
        } catch (error) {
            throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
        }
    }
    return {
        bytes,
        ...parseTraceResponseMeta(response.headers, safeSince, bytes.length),
    }
}

// ---------------------------------------------------------------------------
// 固件 task-60：UART trace v2（行缓冲 + 固件时间戳 + 触发标记）
// ---------------------------------------------------------------------------
/**
 * 一行目标 UART 输出（固件按 '\n' 切分，超长行由固件截断）。
 * `tUs` 是固件 ESP32 侧 esp_timer 单调时间（行结束换行时刻，微秒），
 * **不是目标 MCU 时间**，UI 必须如实标注来源。
 */
export interface CurveLocalTraceLine {
    /** 行序号（since 游标；从 trace/start 起累计行数，非字节序号）。 */
    seq: number
    /** 固件 esp_timer 微秒时间戳（行结束时刻）。 */
    tUs: number
    /** 行文本（固件已把非可打印字节替换为 '.'；不包含换行符）。 */
    text: string
    /** 固件是否截断了超长行（CURVE_TRACE_LINE_FLAG_TRUNCATED）。 */
    truncated?: boolean
}

/** GET /api/curve/trace/lines 一次响应（JSON）。 */
export interface CurveLocalTraceLinesChunk {
    active: boolean | null
    /** since 已被 ring 覆盖（旧行丢弃）；null=字段缺失（不猜）。 */
    gap: boolean | null
    /** 下一次请求的 since（行游标）。 */
    nextSeq: number
    /** 自 start 起的累计行数；null=固件未提供。 */
    totalLines: number | null
    /** 被 ring 覆盖丢弃的行数；null=固件未提供。 */
    droppedLines: number | null
    /** 是否发生过行触发命中；null=固件未提供。 */
    triggered: boolean | null
    /** 触发命中的行序号；null=无/未提供。 */
    triggerSeq: number | null
    lines: CurveLocalTraceLine[]
}

export function emptyCurveTraceLinesChunk(since = 0): CurveLocalTraceLinesChunk {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    return {
        active: null,
        gap: false,
        nextSeq: safeSince,
        totalLines: null,
        droppedLines: null,
        triggered: null,
        triggerSeq: null,
        lines: [],
    }
}

/**
 * 解析 trace/lines JSON（纯函数，便于单测）。
 * 行/时间戳只取固件字段：缺字段/类型错误抛 CurveLocalError（不补行、不造时间）。
 */
export function parseTraceLinesResponse(
    body: Record<string, unknown>,
    since = 0,
    maxLines = CURVE_LOCAL_TRACE_LINES_DEFAULT,
): CurveLocalTraceLinesChunk {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    const safeMax = Math.max(1, Math.min(CURVE_LOCAL_TRACE_LINES_MAX, Math.floor(Number.isFinite(maxLines) ? maxLines : CURVE_LOCAL_TRACE_LINES_DEFAULT)))
    if (!body || typeof body !== 'object') throw new CurveLocalError('trace lines response must be a JSON object')
    const rawLines = body.lines
    if (!Array.isArray(rawLines)) throw new CurveLocalError('trace lines response missing lines array')
    const lines: CurveLocalTraceLine[] = []
    let previousSeq: number | null = null
    for (const item of rawLines) {
        if (typeof item !== 'object' || item === null) throw new CurveLocalError('trace line entry must be an object')
        const raw = item as Record<string, unknown>
        const seq = raw.seq
        const tUs = raw.t_us
        const text = raw.text
        if (typeof seq !== 'number' || !Number.isInteger(seq) || seq < 0 || seq > 0xffffffff) {
            throw new CurveLocalError(`trace line has invalid seq (${String(seq)})`)
        }
        if (typeof tUs !== 'number' || !Number.isFinite(tUs) || tUs < 0) {
            throw new CurveLocalError(`trace line seq=${seq} has invalid t_us (${String(tUs)})`)
        }
        if (typeof text !== 'string') throw new CurveLocalError(`trace line seq=${seq} has invalid text`)
        if (previousSeq !== null && seq <= previousSeq) {
            throw new CurveLocalError(`trace lines seq not increasing (${previousSeq} -> ${seq})`)
        }
        previousSeq = seq
        const truncated = raw.truncated === true ? true : raw.truncated === false ? false : undefined
        lines.push({seq, tUs, text, ...(truncated === undefined ? {} : {truncated})})
    }
    const nextSeq = optionalFiniteNumber(body.next_seq)
        ?? (lines.length > 0 ? lines[lines.length - 1].seq + 1 : safeSince)
    return {
        active: optionalBoolean(body.active) ?? null,
        gap: optionalBoolean(body.gap) ?? null,
        nextSeq,
        totalLines: optionalFiniteNumber(body.total_lines) ?? null,
        droppedLines: optionalFiniteNumber(body.dropped_lines) ?? null,
        triggered: optionalBoolean(body.triggered) ?? null,
        triggerSeq: optionalFiniteNumber(body.trigger_seq) ?? null,
        lines: lines.slice(0, safeMax),
    }
}

/**
 * 拉取 since 之后的行（固件 task-60）。204 → 空 chunk；404 抛 CurveLocalError
 * （httpStatus=404），调用方据此回退旧 raw 字节接口。
 */
export async function getLocalCurveTraceLines(
    since = 0,
    max = CURVE_LOCAL_TRACE_LINES_DEFAULT,
): Promise<CurveLocalTraceLinesChunk> {
    const safeSince = Math.max(0, Math.floor(Number.isFinite(since) ? since : 0))
    const safeMax = Math.max(1, Math.min(CURVE_LOCAL_TRACE_LINES_MAX, Math.floor(Number.isFinite(max) ? max : CURVE_LOCAL_TRACE_LINES_DEFAULT)))
    let response: Response
    try {
        response = await fetch(deviceApiUrl(`${CURVE_LOCAL_TRACE_LINES_PATH}?since=${safeSince}&max=${safeMax}`), {
            method: 'GET',
            cache: 'no-store',
        })
    } catch (error) {
        throw new CurveLocalError(error instanceof Error ? error.message : 'network error')
    }
    if (response.status === 204) return emptyCurveTraceLinesChunk(safeSince)
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
    if (!body) throw new CurveLocalError('trace lines response is not valid JSON', response.status)
    return parseTraceLinesResponse(body, safeSince, safeMax)
}

/** 固件帧指针 backtrace（GET /api/curve/backtrace）解析结果。 */
export interface CurveLocalBacktrace {
    /** 最近一次 trigger 命中是否产生了合法帧指针链。 */
    valid: boolean
    /** 有效 PC 数（0..CURVE_LOCAL_MAX_BACKTRACE_FRAMES）。 */
    count: number
    /** 是否在 8 帧上限处截断（链可能更长）。 */
    truncated: boolean
    /** PC 列表：pcs[0] = 命中时刻 PC，其后为返回地址（Thumb bit0 已清 0）。 */
    pcs: number[]
}

/** 单个 PC 值 -> u32（接受固件 "0x%08X" 字符串或数字；Thumb bit0 清 0）。 */
function parseBacktracePc(value: unknown): number {
    if (typeof value === 'number') {
        if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
            throw new CurveLocalError(`invalid backtrace pc ${String(value)}`)
        }
        return (value & 0xfffffffe) >>> 0
    }
    if (typeof value === 'string') {
        const text = value.trim()
        const match = /^(?:0[xX])?([0-9a-fA-F]{1,8})$/.exec(text)
        if (!match) throw new CurveLocalError(`invalid backtrace pc "${value}"`)
        return ((Number.parseInt(match[1], 16) & 0xfffffffe) >>> 0)
    }
    throw new CurveLocalError('invalid backtrace pc entry')
}

/**
 * 解析 backtrace JSON（纯函数）。
 * 契约：`{ok:true,valid,count,truncated,pcs:["0x08029A44", ...]}`；
 * 数据以 `pcs` 为准（count 只做交叉校验），超过 8 帧按固件上限截断并置 truncated。
 * 非法/损坏响应抛 CurveLocalError（显示原始错误，不编造帧）。
 */
export function parseBacktraceResponse(body: Record<string, unknown>): CurveLocalBacktrace {
    if (!body || typeof body !== 'object') throw new CurveLocalError('backtrace response must be a JSON object')
    const rawPcs = body.pcs
    if (rawPcs === undefined || rawPcs === null) {
        if (body.valid === true) throw new CurveLocalError('backtrace response valid=true without pcs')
        return {valid: false, count: 0, truncated: false, pcs: []}
    }
    if (!Array.isArray(rawPcs)) throw new CurveLocalError('backtrace pcs must be an array')
    const parsed = rawPcs.map(parseBacktracePc)
    const truncated = body.truncated === true || parsed.length > CURVE_LOCAL_MAX_BACKTRACE_FRAMES
    const pcs = parsed.slice(0, CURVE_LOCAL_MAX_BACKTRACE_FRAMES)
    const valid = body.valid === true && pcs.length > 0
    if (!valid) return {valid: false, count: 0, truncated: false, pcs: []}
    return {valid: true, count: pcs.length, truncated, pcs}
}

/** 读取最近一次 trigger 命中的帧指针 backtrace（每次 start 后最多保留最后一次命中）。 */
export async function getLocalCurveBacktrace(): Promise<CurveLocalBacktrace> {
    const body = await requestJson(CURVE_LOCAL_BACKTRACE_PATH, {method: 'GET'})
    return parseBacktraceResponse(body)
}
