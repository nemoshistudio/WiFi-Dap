/**
 * OffscreenCanvas 图表渲染 Worker（task-47 → task-62 P1-15 → task-67 WebGL 后备）。
 *
 * 与旧实现（主线程每帧把窗口内所有 series 的 Float32Array 快照 transfer 进来）相比：
 *  - Worker 自己维护与主线程同构的 ring（ChartRingStore），主线程只按 pushSample
 *    频率发小消息（time + ids + values，A256 时 ~2 KB/次），不再每帧复制整窗数据；
 *  - Y 范围计算、log/normalized 变换、平滑、像素列 min/max 降采样、canvas 光栅
 *    全部在 Worker 内完成；主线程只 drawImage(bitmap) + hover overlay（主线程 ring）；
 *  - 大系列数（A 模式 256 路）自适应降低内部光栅分辨率并按颜色合批 stroke；
 *  - 派生曲线（add/sub）仍在主线程重算，存在派生曲线时 DebugCurve 回退主线程绘制；
 *  - **task-67**：feature flag 打开且大系列（默认 >=64 路）时，曲线层改用 WebGL
 *    （GPU 常驻 ring + 增量上传，见 curveChartWebgl.ts），背景/网格/刻度仍由 2D
 *    canvas 绘制后合成；WebGL 不支持/context lost/全量上传异常/平滑开启时自动
 *    回退本文件的 2D 路径，主线程/导出/悬停逻辑完全不变。
 *
 * 失败/不支持 OffscreenCanvas 时主线程回退 drawChartLocal（数据/导出/光标不变）。
 */
import {
    ChartRingStore,
    buildColumnEnvelope,
    computeChartRange,
    decideChartWebgl,
    formatAxisValue,
    niceTimeStep,
    type ChartDrawMessage,
    type ChartGeometry,
    type ChartRingStore as ChartRingStoreType,
    type ChartWebglMode,
    type ChartWorkerInput,
    type ColumnEnvelope,
} from './curveChartCore'
import { createChartWebglRenderer, type ChartWebglRenderer } from './curveChartWebgl'

const scope = self as unknown as {
    onmessage: ((event: MessageEvent<ChartWorkerInput>) => void) | null
    postMessage: (message: unknown, transfer?: Transferable[]) => void
}

const store: ChartRingStoreType = new ChartRingStore()

let cachedCanvas: OffscreenCanvas | null = null
let cachedPixelWidth = 0
let cachedPixelHeight = 0
/** 每 series 包络数组复用（避免每帧分配）。 */
const envelopeCache = new Map<number, ColumnEnvelope>()

// ---- task-67 WebGL 后备 ------------------------------------------------------
let webglMode: ChartWebglMode = 'off'
let webglRenderer: ChartWebglRenderer | null = null
let webglUnavailable = false
let webglFallbackReason: string | null = null
/** 合成 canvas（2D）：叠背景/网格/刻度文字 + WebGL 透明曲线层。 */
let compositeCanvas: OffscreenCanvas | null = null
let compositeContext: OffscreenCanvasRenderingContext2D | null = null

/**
 * 每帧统计：2D 路径带 decimated/segments/columnsDrawn/startOrder，WebGL 路径带
 * uploadBytes/uploadCalls/drawCalls/vertices/rangeMs/groups；统一成可转发主线程的字典。
 */
interface FrameStats {
    renderer: 'webgl2' | 'webgl1' | 'canvas2d'
    series: number
    drawMs: number
    [key: string]: number | string | null
}

interface FrameResult {
    bitmap: ImageBitmap
    geometry: ChartGeometry | null
    stats: FrameStats
}

/** 坐标轴/网格/背景的公共布局（2D 与 WebGL 合成路径共用，样式保持一致）。 */
interface AxesLayout {
    padding: ChartDrawMessage['padding']
    plotWidth: number
    plotHeight: number
    xMin: number
    xMax: number
    minValue: number
    maxValue: number
    yMode: ChartDrawMessage['yMode']
    width: number
    height: number
}

/**
 * 背景 + Y/X 网格 + 刻度文字（task-67 从 drawScene 抽出来，WebGL 合成路径复用，
 * 保证两条渲染路径的坐标轴样式完全一致）。
 */
function drawAxesLayer(
    context: OffscreenCanvasRenderingContext2D,
    scene: ChartDrawMessage,
    layout: AxesLayout,
): void {
    const {padding, plotWidth, plotHeight, xMin, xMax, minValue, maxValue, width, height} = layout
    context.fillStyle = scene.palette.bg
    context.fillRect(0, 0, width, height)
    if (plotWidth < 20 || plotHeight < 20) return
    const span = xMax - xMin
    const toX = (time: number) => padding.left + ((time - xMin) / span) * plotWidth
    const toY = (value: number) => padding.top + (1 - (value - minValue) / (maxValue - minValue)) * plotHeight
    const formatY = (value: number) => {
        if (scene.yMode === 'normalized') return `${Math.round(value)}%`
        if (scene.yMode === 'log') return formatAxisValue(Math.pow(10, value))
        return formatAxisValue(value)
    }

    // 网格 + Y 轴
    context.font = '11px system-ui, sans-serif'
    context.textBaseline = 'middle'
    context.textAlign = 'right'
    context.lineWidth = 1
    for (let step = 0; step <= 5; step++) {
        const value = minValue + ((maxValue - minValue) * step) / 5
        const y = toY(value)
        context.strokeStyle = step === 0 || step === 5 ? scene.palette.axis : scene.palette.grid
        context.beginPath()
        context.moveTo(padding.left, y)
        context.lineTo(padding.left + plotWidth, y)
        context.stroke()
        context.fillStyle = scene.palette.text
        context.fillText(formatY(value), padding.left - 6, y)
    }
    // X 轴
    context.textAlign = 'center'
    context.textBaseline = 'top'
    const timeStep = niceTimeStep(span)
    const firstTick = Math.ceil(xMin / timeStep) * timeStep
    for (let time = firstTick; time <= xMax + 1e-9; time += timeStep) {
        const x = toX(time)
        context.strokeStyle = scene.palette.grid
        context.beginPath()
        context.moveTo(x, padding.top)
        context.lineTo(x, padding.top + plotHeight)
        context.stroke()
        context.fillStyle = scene.palette.text
        context.fillText(`${time.toFixed(0)}s`, x, padding.top + plotHeight + 6)
    }
}

function buildAxesLayout(scene: ChartDrawMessage, geometry: ChartGeometry, width: number, height: number): AxesLayout {
    return {
        padding: geometry.padding,
        plotWidth: geometry.plotWidth,
        plotHeight: geometry.plotHeight,
        xMin: geometry.xMin,
        xMax: geometry.xMax,
        minValue: geometry.minValue,
        maxValue: geometry.maxValue,
        yMode: geometry.yMode,
        width,
        height,
    }
}

function drawScene(scene: ChartDrawMessage): FrameResult {
    const startedAt = performance.now()
    const width = Math.max(1, Math.floor(scene.width))
    const height = Math.max(1, Math.floor(scene.height))
    const dpr = Math.min(2, Math.max(1, scene.dpr))
    // A 模式 256 路时降低内部光栅分辨率（主线程 drawImage 拉伸显示），换取帧率。
    const renderScale = scene.series.length > 192 ? 0.65 : scene.series.length > 96 ? 0.8 : 1
    const pixelWidth = Math.max(1, Math.round(width * dpr * renderScale))
    const pixelHeight = Math.max(1, Math.round(height * dpr * renderScale))
    if (!cachedCanvas || cachedPixelWidth !== pixelWidth || cachedPixelHeight !== pixelHeight) {
        cachedCanvas = new OffscreenCanvas(pixelWidth, pixelHeight)
        cachedPixelWidth = pixelWidth
        cachedPixelHeight = pixelHeight
    }
    const canvas = cachedCanvas
    const context = canvas.getContext('2d')
    if (!context) throw new Error('OffscreenCanvas 2d unavailable')
    context.setTransform(dpr * renderScale, 0, 0, dpr * renderScale, 0, 0)
    context.fillStyle = scene.palette.bg
    context.fillRect(0, 0, width, height)

    const padding = scene.padding
    const plotWidth = width - padding.left - padding.right
    const plotHeight = height - padding.top - padding.bottom
    if (plotWidth < 20 || plotHeight < 20 || store.count === 0 || scene.series.length === 0) {
        return {
            bitmap: canvas.transferToImageBitmap(),
            geometry: null,
            stats: {
                renderer: 'canvas2d', series: scene.series.length, decimated: 0, segments: 0,
                columnsDrawn: 0, startOrder: 0, drawMs: performance.now() - startedAt,
            },
        }
    }
    const startOrder = store.lowerBoundTime(scene.xMin)
    if (startOrder >= store.count) {
        return {
            bitmap: canvas.transferToImageBitmap(),
            geometry: null,
            stats: {
                renderer: 'canvas2d', series: scene.series.length, decimated: 0, segments: 0,
                columnsDrawn: 0, startOrder: 0, drawMs: performance.now() - startedAt,
            },
        }
    }
    const range = computeChartRange(store, scene.series, startOrder, scene.yMode)
    if (!range) {
        return {
            bitmap: canvas.transferToImageBitmap(),
            geometry: null,
            stats: {
                renderer: 'canvas2d', series: scene.series.length, decimated: 0, segments: 0,
                columnsDrawn: 0, startOrder: 0, drawMs: performance.now() - startedAt,
            },
        }
    }
    const {minValue, maxValue} = range
    const span = scene.xMax - scene.xMin
    const toX = (time: number) => padding.left + ((time - scene.xMin) / span) * plotWidth
    const toY = (value: number) => padding.top + (1 - (value - minValue) / (maxValue - minValue)) * plotHeight
    // 背景/网格/刻度与 WebGL 合成路径共用同一实现（task-67）。
    drawAxesLayer(context, scene, {
        padding, plotWidth, plotHeight, xMin: scene.xMin, xMax: scene.xMax,
        minValue, maxValue, yMode: scene.yMode, width, height,
    })

    const orderCount = store.count - startOrder
    const useDecimation = orderCount > plotWidth * 1.5
    // 大系列数：更粗的像素列 + 点抽取（范围仍精确扫描，见 core.computeChartRange）。
    const columnScale = scene.series.length > 192 ? 20 : scene.series.length > 96 ? 4 : 1
    const orderStep = scene.series.length > 192 ? 4 : 1
    const byColor = new Map<string, ChartDrawSeriesSpecLike[]>()
    for (const series of scene.series) {
        const bucket = byColor.get(series.color)
        if (bucket) bucket.push(series)
        else byColor.set(series.color, [series])
    }
    context.lineWidth = 1.5
    let columnsDrawn = 0
    let segments = 0
    if (useDecimation) {
        const options = {
            left: padding.left,
            plotWidth,
            columnScale,
            orderStep,
            xMin: scene.xMin,
            xMax: scene.xMax,
            yMode: scene.yMode,
            smoothingWindow: scene.smoothingWindow,
            ranges: range.ranges,
        }
        for (const [color, items] of byColor) {
            context.strokeStyle = color
            context.beginPath()
            for (const series of items) {
                const envelope = buildColumnEnvelope(store, series, startOrder, options, envelopeCache.get(series.id) ?? null)
                if (!envelope) continue
                envelopeCache.set(series.id, envelope)
                columnsDrawn += envelope.columns
                const xs = envelope.xs
                const min = envelope.min
                const max = envelope.max
                let previousColumn = -2
                for (let column = 0; column < envelope.columns; column++) {
                    if (!Number.isFinite(min[column])) continue
                    const x = xs[column]
                    const yTop = toY(max[column])
                    const yBottom = toY(min[column])
                    if (column !== previousColumn + 1) context.moveTo(x, yTop)
                    else context.lineTo(x, yTop)
                    context.lineTo(x, yBottom)
                    previousColumn = column
                    segments += 1
                }
            }
            context.stroke()
        }
    } else {
        for (const [color, items] of byColor) {
            context.strokeStyle = color
            context.beginPath()
            for (const series of items) {
                const values = store.seriesValues(series.id)
                const seriesRange = range.ranges.get(series.id)
                let started = false
                const smoothQueue: number[] = []
                let smoothSum = 0
                const smoothing = Math.max(1, scene.smoothingWindow)
                for (let order = startOrder; order < store.count; order++) {
                    const slot = store.slotOf(order)
                    if (store.slotSeq[slot] < series.startSeq) continue
                    const raw = values[slot]
                    let value: number | null
                    if (!Number.isFinite(raw)) value = null
                    else if (scene.yMode === 'log') value = raw > 0 ? Math.log10(raw) : null
                    else if (scene.yMode === 'normalized') {
                        if (!seriesRange) value = null
                        else {
                            const rangeSpan = seriesRange.max - seriesRange.min
                            value = rangeSpan < 1e-12 ? 50 : ((raw - seriesRange.min) / rangeSpan) * 100
                        }
                    } else value = raw
                    if (value === null) {
                        started = false
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
                    const x = toX(store.timeAt(order))
                    const y = toY(drawValue)
                    if (!started) {
                        context.moveTo(x, y)
                        started = true
                    } else {
                        context.lineTo(x, y)
                        segments += 1
                    }
                }
            }
            context.stroke()
        }
    }

    const ranges: Record<string, [number, number]> = {}
    for (const [id, seriesRange] of range.ranges) ranges[String(id)] = [seriesRange.min, seriesRange.max]
    return {
        bitmap: canvas.transferToImageBitmap(),
        geometry: {
            padding,
            plotWidth,
            plotHeight,
            xMin: scene.xMin,
            xMax: scene.xMax,
            minValue,
            maxValue,
            yMode: scene.yMode,
            ranges,
        },
        stats: {
            renderer: 'canvas2d',
            series: scene.series.length,
            startOrder,
            orderCount,
            decimated: useDecimation ? 1 : 0,
            columnsDrawn,
            segments,
            drawMs: performance.now() - startedAt,
        },
    }
}

/** 与 ChartDrawSeriesSpec 形状一致（避免重复 import type 噪音）。 */
interface ChartDrawSeriesSpecLike {
    id: number
    color: string
    startSeq: number
}

function ensureWebglRenderer(): ChartWebglRenderer {
    if (webglRenderer) return webglRenderer
    if (webglUnavailable) throw new Error(webglFallbackReason ?? 'webgl unavailable')
    if (typeof OffscreenCanvas === 'undefined') {
        webglUnavailable = true
        webglFallbackReason = 'OffscreenCanvas unavailable'
        throw new Error(webglFallbackReason)
    }
    const canvas = new OffscreenCanvas(1, 1)
    const renderer = createChartWebglRenderer(canvas as unknown as Parameters<typeof createChartWebglRenderer>[0])
    if (!renderer) {
        webglUnavailable = true
        webglFallbackReason = 'webgl2/webgl1 context unavailable'
        throw new Error(webglFallbackReason)
    }
    renderer.resize(store.capacity)
    webglRenderer = renderer
    return renderer
}

function ensureCompositeContext(pixelWidth: number, pixelHeight: number): OffscreenCanvasRenderingContext2D {
    if (!compositeCanvas || compositeCanvas.width !== pixelWidth || compositeCanvas.height !== pixelHeight) {
        compositeCanvas = new OffscreenCanvas(pixelWidth, pixelHeight)
        compositeContext = compositeCanvas.getContext('2d')
    }
    if (!compositeContext) throw new Error('composite OffscreenCanvas 2d unavailable')
    return compositeContext
}

/** WebGL 曲线层 + 2D 背景/坐标轴合成；失败抛错由调用方回退 2D。 */
function drawWebglScene(scene: ChartDrawMessage): FrameResult {
    const startedAt = performance.now()
    const width = Math.max(1, Math.floor(scene.width))
    const height = Math.max(1, Math.floor(scene.height))
    const dpr = Math.min(2, Math.max(1, scene.dpr))
    const pixelWidth = Math.max(1, Math.round(width * dpr))
    const pixelHeight = Math.max(1, Math.round(height * dpr))
    const renderer = ensureWebglRenderer()
    const frame = renderer.draw(scene, store)
    const bitmap = renderer.transferBitmap()
    try {
        const context = ensureCompositeContext(pixelWidth, pixelHeight)
        context.setTransform(dpr, 0, 0, dpr, 0, 0)
        const layout: AxesLayout = frame.geometry
            ? buildAxesLayout(scene, frame.geometry, width, height)
            : {
                padding: scene.padding,
                plotWidth: width - scene.padding.left - scene.padding.right,
                plotHeight: height - scene.padding.top - scene.padding.bottom,
                xMin: scene.xMin,
                xMax: scene.xMax,
                minValue: 0,
                maxValue: 1,
                yMode: scene.yMode,
                width,
                height,
            }
        drawAxesLayer(context, scene, layout)
        if (frame.geometry) context.drawImage(bitmap, 0, 0, width, height)
        const output = compositeCanvas!.transferToImageBitmap()
        const stats: FrameStats = {...frame.stats, drawMs: performance.now() - startedAt}
        return {bitmap: output, geometry: frame.geometry, stats}
    } finally {
        bitmap.close()
    }
}

/** 渲染入口：WebGL（可选、大系列）→ 2D OffscreenCanvas 回退。 */
function drawMessage(scene: ChartDrawMessage): FrameResult {
    const decision = decideChartWebgl(webglMode, scene.series.length, scene.smoothingWindow)
    // webglUnavailable 后本会话不再逐帧尝试（避免每帧抛错）；reset 时可重新探测。
    if (decision.use && !webglUnavailable && store.count >= 2) {
        try {
            return drawWebglScene(scene)
        } catch (error) {
            // 一旦失败（context lost / transfer 异常）本会话永久回退 2D，避免每帧抖动。
            if (!webglUnavailable) {
                webglUnavailable = true
                webglFallbackReason = error instanceof Error ? error.message : String(error)
                try {
                    webglRenderer?.dispose()
                } catch { /* ignore */ }
                webglRenderer = null
            }
        }
    }
    const result = drawScene(scene)
    const stats = result.stats as FrameStats
    stats.webglMode = webglMode
    stats.webglFallback = webglUnavailable ? (webglFallbackReason ?? 'unavailable') : null
    if (decision.reason) stats.webglSkipped = decision.reason
    return result
}

scope.onmessage = (event: MessageEvent<ChartWorkerInput>) => {
    const message = event.data
    try {
        if (message.type === 'reset') {
            store.resize(message.maxPoints)
            store.reset()
            envelopeCache.clear()
            webglMode = message.webgl ?? 'off'
            if (webglRenderer) {
                if (webglMode === 'off') {
                    webglRenderer.dispose()
                    webglRenderer = null
                } else {
                    webglRenderer.resize(store.capacity)
                    webglRenderer.reset()
                }
            }
            // 渲染后端开关变化后允许重新探测（上一次的失败原因仍保留给诊断）。
            if (webglMode !== 'off') webglUnavailable = false
            return
        }
        if (message.type === 'samples') {
            store.push(message.time, message.ids, message.values)
            // task-67：WebGL renderer 只记脏 slot（不做 GL 调用），draw 时批量增量上传。
            if (webglRenderer) webglRenderer.onSample(store)
            return
        }
        const result = drawMessage(message)
        scope.postMessage(
            {
                type: 'bitmap',
                requestId: message.requestId,
                bitmap: result.bitmap,
                geometry: result.geometry,
                stats: result.stats,
                drawMs: result.stats.drawMs,
                renderer: result.stats.renderer,
                webglFallback: result.stats.webglFallback ?? null,
                webglMode: result.stats.webglMode ?? webglMode,
            },
            [result.bitmap],
        )
    } catch (error) {
        scope.postMessage({
            type: 'error',
            requestId: message.type === 'draw' ? message.requestId : 0,
            error: error instanceof Error ? error.message : String(error),
        })
    }
}
