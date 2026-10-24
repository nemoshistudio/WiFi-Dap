/**
 * 图表渲染核心（task-62 / P1-15）：Worker 内 ring 存储 + Y 范围 + 像素列降采样。
 *
 * 设计边界：
 *  - 主线程不再每帧把整个窗口的曲线复制/transform 给 Worker；采样在 pushSample 时
 *    以小消息（时间 + 各 series 数值）增量送给 Worker，Worker 自己维护与主线程同构
 *    的 ring（容量 MAX_POINTS、覆盖语义一致）。这样 A 模式 256 路 × 6000 点的
 *    每帧快照复制（~6 MB/帧 → ~180 MB/s）被消除，只有绘制在 Worker 里发生。
 *  - 本模块是纯 TS（无 DOM），可被 Worker 与 Node 单测/基准同时引用；实际 canvas
 *    光栅仍在 curveChartWorker.ts 中完成。
 *  - 派生曲线（add/sub）由主线程按环重新计算，不在 Worker 重算：存在派生曲线时
 *    DebugCurve 回退主线程渲染（见 drawChart 的边界说明）。
 */

/** 与 DebugCurve MAX_POINTS 保持一致（默认容量；实际以 init/reset 消息为准）。 */
export const CHART_DEFAULT_MAX_POINTS = 6000

export type ChartYMode = 'auto' | 'normalized' | 'log'

/** 一帧采样消息：time=相对秒；ids/values 对应 pushSample 的 series 快照。 */
export interface ChartSampleMessage {
    type: 'samples'
    time: number
    ids: number[]
    values: number[]
}

/** 清空 ring（clearData/切换会话时）。 */
export interface ChartResetMessage {
    type: 'reset'
    maxPoints: number
}

export interface ChartDrawSeriesSpec {
    id: number
    color: string
    /** 该曲线第一个有效 sampleSeq（ring order 小于它视为无数据）。 */
    startSeq: number
}

export interface ChartCanvasPalette {
    bg: string
    grid: string
    text: string
    axis: string
}

export interface ChartDrawMessage {
    type: 'draw'
    requestId: number
    width: number
    height: number
    dpr: number
    padding: {left: number; right: number; top: number; bottom: number}
    xMin: number
    xMax: number
    yMode: ChartYMode
    smoothingWindow: number
    palette: ChartCanvasPalette
    series: ChartDrawSeriesSpec[]
}

export type ChartWorkerInput = ChartSampleMessage | ChartResetMessage | ChartDrawMessage

export interface ChartRangeResult {
    /** 全局 Y 轴范围（已按 yMode 变换；normalized 固定 0..100）。 */
    minValue: number
    maxValue: number
    /** normalized 模式各 series 的原始值范围（hover/主线程 transform 用）。 */
    ranges: Map<number, {min: number; max: number}>
}

export interface ChartGeometry {
    padding: {left: number; right: number; top: number; bottom: number}
    plotWidth: number
    plotHeight: number
    xMin: number
    xMax: number
    minValue: number
    maxValue: number
    yMode: ChartYMode
    ranges: Record<string, [number, number]>
}

/** ring 采样存储（与主线程 ring 同构：环形覆盖、共享时间轴、每 series 独立值数组）。 */
export class ChartRingStore {
    capacity: number
    head = 0
    count = 0
    /** 已写入的 sample 数（每个 pushSample 消息 +1，与主线程 sampleSeq 对齐）。 */
    seq = 0
    times: Float64Array
    slotSeq: Float64Array
    private readonly series = new Map<number, Float64Array>()

    constructor(capacity = CHART_DEFAULT_MAX_POINTS) {
        this.capacity = Math.max(1, Math.floor(capacity))
        this.times = new Float64Array(this.capacity)
        this.slotSeq = new Float64Array(this.capacity)
    }

    /** 调整容量会清空数据（主线程容量固定，仅在 init 时调用一次）。 */
    resize(capacity: number): void {
        const next = Math.max(1, Math.floor(capacity))
        if (next === this.capacity) return
        this.capacity = next
        this.times = new Float64Array(next)
        this.slotSeq = new Float64Array(next)
        this.series.clear()
        this.head = 0
        this.count = 0
        this.seq = 0
    }

    reset(): void {
        this.head = 0
        this.count = 0
        this.seq = 0
    }

    /** 取（必要时创建）一个 series 的值数组。 */
    seriesValues(id: number): Float64Array {
        let values = this.series.get(id)
        if (!values) {
            values = new Float64Array(this.capacity)
            this.series.set(id, values)
        }
        return values
    }

    /** 写入一次采样（对应主线程一次 pushSample）。 */
    push(time: number, ids: readonly number[], values: ArrayLike<number>): void {
        const slot = this.head
        this.times[slot] = time
        this.slotSeq[slot] = this.seq
        for (let index = 0; index < ids.length; index++) {
            const id = ids[index]
            if (!Number.isFinite(id)) continue
            this.seriesValues(id)[slot] = values[index] ?? Number.NaN
        }
        this.head = (this.head + 1) % this.capacity
        if (this.count < this.capacity) this.count += 1
        this.seq += 1
    }

    slotOf(order: number): number {
        return (this.head - this.count + order + this.capacity) % this.capacity
    }

    timeAt(order: number): number {
        return this.times[this.slotOf(order)]
    }

    seqAt(order: number): number {
        return this.slotSeq[this.slotOf(order)]
    }

    /** 第一个 time >= target 的 order；全部更早返回 count。 */
    lowerBoundTime(target: number): number {
        let low = 0
        let high = this.count
        while (low < high) {
            const mid = (low + high) >>> 1
            if (this.times[this.slotOf(mid)] < target) low = mid + 1
            else high = mid
        }
        return low
    }
}

export function niceTimeStep(span: number): number {
    const target = span / 5
    const magnitude = Math.pow(10, Math.floor(Math.log10(Math.max(target, 1e-6))))
    for (const candidate of [1, 2, 5, 10]) {
        if (candidate * magnitude >= target) return candidate * magnitude
    }
    return 10 * magnitude
}

export function formatAxisValue(value: number): string {
    if (!Number.isFinite(value)) return ''
    const abs = Math.abs(value)
    if (abs >= 1e6 || (abs > 0 && abs < 1e-3)) return value.toExponential(2)
    if (Number.isInteger(value)) return String(value)
    return value.toFixed(2)
}

function transformValue(raw: number, yMode: ChartYMode): number | null {
    if (!Number.isFinite(raw)) return null
    if (yMode === 'log') return raw > 0 ? Math.log10(raw) : null
    return raw
}

/**
 * 计算 Y 轴范围（与主线程 computeYRange 语义一致）：
 *  - auto：所有 drawn 曲线数值全局 min/max（8% padding，退化时 ±1）；
 *  - log：log10(value)，<=0 视为无点；
 *  - normalized：固定 0..100，并返回各 series 原始 min/max 供 transform。
 * 只统计 startOrder 起、slotSeq >= startSeq 的点（等价主线程 ring 语义）。
 */
export function computeChartRange(
    store: ChartRingStore,
    series: readonly ChartDrawSeriesSpec[],
    startOrder: number,
    yMode: ChartYMode,
): ChartRangeResult | null {
    const ranges = new Map<number, {min: number; max: number}>()
    let minValue = Number.POSITIVE_INFINITY
    let maxValue = Number.NEGATIVE_INFINITY
    const count = store.count
    const capacity = store.capacity
    const slotSeq = store.slotSeq
    const from = Math.max(0, Math.min(count, Math.floor(startOrder)))
    if (from >= count || series.length === 0) return null
    // 热循环：用游标顺序遍历 ring（每点 from*series 次，避免 slotOf 函数调用与取模）。
    let startSlot = store.slotOf(from)
    for (const spec of series) {
        const values = store.seriesValues(spec.id)
        const startSeq = spec.startSeq
        let cursor = startSlot
        if (yMode === 'normalized') {
            let min = Number.POSITIVE_INFINITY
            let max = Number.NEGATIVE_INFINITY
            for (let order = from; order < count; order++) {
                if (slotSeq[cursor] >= startSeq) {
                    const raw = values[cursor]
                    if (Number.isFinite(raw)) {
                        if (raw < min) min = raw
                        if (raw > max) max = raw
                    }
                }
                cursor += 1
                if (cursor === capacity) cursor = 0
            }
            if (!Number.isFinite(min) || !Number.isFinite(max)) continue
            if (max - min < 1e-12) max = min + 1
            ranges.set(spec.id, {min, max})
        } else {
            for (let order = from; order < count; order++) {
                if (slotSeq[cursor] >= startSeq) {
                    const raw = values[cursor]
                    if (Number.isFinite(raw)) {
                        const value = yMode === 'log' ? (raw > 0 ? Math.log10(raw) : null) : raw
                        if (value !== null) {
                            if (value < minValue) minValue = value
                            if (value > maxValue) maxValue = value
                        }
                    }
                }
                cursor += 1
                if (cursor === capacity) cursor = 0
            }
        }
    }
    if (yMode === 'normalized') {
        if (ranges.size === 0) return null
        return {minValue: 0, maxValue: 100, ranges}
    }
    if (!Number.isFinite(minValue) || !Number.isFinite(maxValue)) return null
    if (maxValue - minValue < 1e-9) {
        minValue -= 1
        maxValue += 1
    } else {
        const span = maxValue - minValue
        minValue -= span * 0.08
        maxValue += span * 0.08
    }
    return {minValue, maxValue, ranges}
}

export interface ColumnEnvelope {
    /** 列数（像素列，含 padding.left 之后的绘制宽度）。 */
    columns: number
    /** 每列映射到的绘制 x（CSS 像素，绝对坐标）。 */
    xs: Float32Array
    min: Float32Array
    max: Float32Array
}

export interface ColumnEnvelopeOptions {
    /** 绘制区左边界（CSS 像素）。 */
    left: number
    /** 绘制区宽度（CSS 像素）。 */
    plotWidth: number
    /** 每列覆盖的 CSS 像素数（≥1；大系列数时增大）。 */
    columnScale: number
    /** 每 series 的点抽取步长（≥1；大系列数时增大）。 */
    orderStep: number
    xMin: number
    xMax: number
    yMode: ChartYMode
    smoothingWindow: number
    /** normalized 模式各 series 原始值范围。 */
    ranges?: Map<number, {min: number; max: number}>
}

/**
 * 构建一条曲线的像素列 min/max 包络（降采样核心）：
 *  - 每列只保留该列内点的 min/max，绘制时每列画一条竖线（或相邻列连线），
 *    视觉上等价“每像素列 min/max”；
 *  - 跳过 slotSeq < startSeq 的点（曲线后加入）与 NaN/无点；
 *  - orderStep>1 时只抽取部分点（大系列数提速），极端孤立尖峰可能被跳过，
 *    因此 Y 轴范围仍由 computeChartRange 精确扫描得到（不依赖包络）。
 */
export function buildColumnEnvelope(
    store: ChartRingStore,
    spec: ChartDrawSeriesSpec,
    startOrder: number,
    options: ColumnEnvelopeOptions,
    reuse?: ColumnEnvelope | null,
): ColumnEnvelope | null {
    const scale = Math.max(1, Math.floor(options.columnScale))
    const plotWidth = Math.max(1, options.plotWidth)
    const columns = Math.max(1, Math.ceil(plotWidth / scale))
    // 复用上一次的 typed array（Worker 每帧每 series 构建包络，避免 30fps 下的 GC 抖动）。
    const xs = reuse && reuse.xs.length >= columns ? reuse.xs : new Float32Array(columns)
    const effectiveScale = plotWidth / columns
    for (let column = 0; column < columns; column++) {
        xs[column] = options.left + column * effectiveScale + effectiveScale * 0.5
    }
    const min = reuse && reuse.min.length >= columns ? reuse.min : new Float32Array(columns)
    const max = reuse && reuse.max.length >= columns ? reuse.max : new Float32Array(columns)
    min.fill(Number.POSITIVE_INFINITY, 0, columns)
    max.fill(Number.NEGATIVE_INFINITY, 0, columns)
    const values = store.seriesValues(spec.id)
    const step = Math.max(1, Math.floor(options.orderStep))
    const from = Math.max(0, Math.min(store.count, Math.floor(startOrder)))
    if (from >= store.count) return null
    const smoothing = Math.max(1, Math.floor(options.smoothingWindow))
    const smoothQueue: number[] = []
    let smoothSum = 0
    let any = false
    const yMode = options.yMode
    const range = options.ranges?.get(spec.id)
    const span = options.xMax - options.xMin
    if (!(span > 0)) return null
    const bucket = Math.max(1, options.columnScale)
    const capacity = store.capacity
    const slotSeq = store.slotSeq
    let cursor = store.slotOf(from)
    for (let order = from; order < store.count; order += step) {
        const slot = cursor
        if (slotSeq[slot] < spec.startSeq) {
            cursor += step
            if (cursor >= capacity) cursor %= capacity
            continue
        }
        const raw = values[slot]
        let value: number | null
        if (!Number.isFinite(raw)) value = null
        else if (yMode === 'log') value = raw > 0 ? Math.log10(raw) : null
        else if (yMode === 'normalized') {
            if (!range) value = null
            else {
                const rangeSpan = range.max - range.min
                value = rangeSpan < 1e-12 ? 50 : ((raw - range.min) / rangeSpan) * 100
            }
        } else value = raw
        if (value === null) {
            smoothQueue.length = 0
            smoothSum = 0
            continue
        }
        let drawValue = value
        if (smoothing > 1) {
            smoothQueue.push(value)
            smoothSum += value
            if (smoothQueue.length > smoothing) smoothSum -= smoothQueue.shift() as number
            drawValue = smoothSum / smoothQueue.length
        }
        const x = options.left + ((store.times[slot] - options.xMin) / span) * plotWidth
        if (Number.isFinite(x)) {
            let column = Math.floor((x - options.left) / bucket)
            if (column >= columns) column = columns - 1
            if (column >= 0) {
                if (drawValue < min[column]) min[column] = drawValue
                if (drawValue > max[column]) max[column] = drawValue
                any = true
            }
        }
        cursor += step
        if (cursor >= capacity) cursor %= capacity
    }
    if (!any) return null
    return {columns, xs, min, max}
}

/**
 * 从包络直接推导“列内 min/max 的极值”（列 min/max 覆盖列内点；用于基准/自检）。
 * 返回 null = 无有限点。
 */
export function envelopeExtent(envelope: ColumnEnvelope): {min: number; max: number} | null {
    let min = Number.POSITIVE_INFINITY
    let max = Number.NEGATIVE_INFINITY
    for (let column = 0; column < envelope.columns; column++) {
        const columnMin = envelope.min[column]
        const columnMax = envelope.max[column]
        if (Number.isFinite(columnMin) && columnMin < min) min = columnMin
        if (Number.isFinite(columnMax) && columnMax > max) max = columnMax
    }
    return Number.isFinite(min) && Number.isFinite(max) ? {min, max} : null
}
