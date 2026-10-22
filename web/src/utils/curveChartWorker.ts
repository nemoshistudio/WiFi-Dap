/**
 * OffscreenCanvas 图表渲染 Worker（task-47）。
 *
 * 主线程把当前 X 视图窗口的原始值（Float32Array，NaN=无点）与颜色/尺寸/Y 模式
 * transfer 进来；本 Worker 在 OffscreenCanvas 上完成 Y 范围计算、网格/坐标轴、
 * 像素列 min/max 降采样/平滑，然后 transferToImageBitmap 回传。主线程只做
 * drawImage(bitmap) 与 hover overlay，便于失败回退主线程渲染。
 *
 * 不负责 ring/采样协议；ring/数值转换仍在主线程（明确边界）。
 */
interface SceneSeries {
    id: string
    color: string
    values: Float32Array
}

interface ChartScene {
    requestId: number
    width: number
    height: number
    dpr: number
    padding: {left: number; right: number; top: number; bottom: number}
    xMin: number
    xMax: number
    times: Float32Array
    series: SceneSeries[]
    yMode: 'auto' | 'normalized' | 'log'
    smoothingWindow: number
    palette: {bg: string; grid: string; text: string; axis: string}
}

const scope = self as unknown as {
    onmessage: ((event: MessageEvent<ChartScene>) => void) | null
    postMessage: (message: unknown, transfer?: Transferable[]) => void
}

function niceTimeStep(span: number): number {
    const target = span / 5
    const magnitude = Math.pow(10, Math.floor(Math.log10(Math.max(target, 1e-6))))
    for (const candidate of [1, 2, 5, 10]) {
        if (candidate * magnitude >= target) return candidate * magnitude
    }
    return 10 * magnitude
}

function formatAxisValue(value: number): string {
    if (!Number.isFinite(value)) return ''
    const abs = Math.abs(value)
    if (abs >= 1e6 || (abs > 0 && abs < 1e-3)) return value.toExponential(2)
    if (Number.isInteger(value)) return String(value)
    return value.toFixed(2)
}

let cachedCanvas: OffscreenCanvas | null = null
let cachedPixelWidth = 0
let cachedPixelHeight = 0
let cachedMin: Float32Array | null = null
let cachedMax: Float32Array | null = null

function drawScene(scene: ChartScene): {bitmap: ImageBitmap; geometry: unknown} {
    const width = Math.max(1, Math.floor(scene.width))
    const height = Math.max(1, Math.floor(scene.height))
    const dpr = Math.min(2, Math.max(1, scene.dpr))
    // A 256 路时降低内部光栅分辨率（主线程 drawImage 拉伸显示），换取帧率。
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
    if (plotWidth < 20 || plotHeight < 20) return {bitmap: canvas.transferToImageBitmap(), geometry: null}
    const count = scene.times.length
    if (count === 0 || scene.series.length === 0) return {bitmap: canvas.transferToImageBitmap(), geometry: null}

    const seriesRanges = new Map<string, {min: number; max: number}>()
    let minValue = Number.POSITIVE_INFINITY
    let maxValue = Number.NEGATIVE_INFINITY
    const rawForY = (value: number): number | null => {
        if (!Number.isFinite(value)) return null
        if (scene.yMode === 'log') return value > 0 ? Math.log10(value) : null
        return value
    }
    for (const series of scene.series) {
        if (scene.yMode === 'normalized') {
            let min = Number.POSITIVE_INFINITY
            let max = Number.NEGATIVE_INFINITY
            for (let index = 0; index < series.values.length; index++) {
                const value = series.values[index]
                if (!Number.isFinite(value)) continue
                if (value < min) min = value
                if (value > max) max = value
            }
            if (!Number.isFinite(min) || !Number.isFinite(max)) continue
            if (max - min < 1e-12) max = min + 1
            seriesRanges.set(series.id, {min, max})
        } else {
            for (let index = 0; index < series.values.length; index++) {
                const value = rawForY(series.values[index])
                if (value === null) continue
                if (value < minValue) minValue = value
                if (value > maxValue) maxValue = value
            }
        }
    }
    if (scene.yMode === 'normalized') {
        minValue = 0
        maxValue = 100
    } else {
        if (!Number.isFinite(minValue) || !Number.isFinite(maxValue)) return {bitmap: canvas.transferToImageBitmap(), geometry: null}
        if (maxValue - minValue < 1e-9) {
            minValue -= 1
            maxValue += 1
        } else {
            const span = maxValue - minValue
            minValue -= span * 0.08
            maxValue += span * 0.08
        }
    }

    const toX = (time: number) => padding.left + ((time - scene.xMin) / (scene.xMax - scene.xMin)) * plotWidth
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
    const timeStep = niceTimeStep(scene.xMax - scene.xMin)
    const firstTick = Math.ceil(scene.xMin / timeStep) * timeStep
    for (let time = firstTick; time <= scene.xMax + 1e-9; time += timeStep) {
        const x = toX(time)
        context.strokeStyle = scene.palette.grid
        context.beginPath()
        context.moveTo(x, padding.top)
        context.lineTo(x, padding.top + plotHeight)
        context.stroke()
        context.fillStyle = scene.palette.text
        context.fillText(`${time.toFixed(0)}s`, x, padding.top + plotHeight + 6)
    }

    const orderCount = count
    const useDecimation = orderCount > plotWidth * 1.5
    const columnScale = scene.series.length > 192 ? 20 : scene.series.length > 96 ? 4 : 1
    const orderStep = scene.series.length > 192 ? 4 : 1
    const plotColumns = Math.max(1, Math.ceil(plotWidth / columnScale))
    if (!cachedMin || cachedMin.length < plotColumns) {
        cachedMin = new Float32Array(plotColumns)
        cachedMax = new Float32Array(plotColumns)
    }
    const columnMin = cachedMin
    const columnMax = cachedMax!
    context.lineWidth = 1.5
    for (const series of scene.series) {
        const smoothQueue: number[] = []
        const valueForY = (value: number): number | null => {
            const raw = rawForY(value)
            if (raw === null) return null
            if (scene.yMode === 'normalized') {
                const range = seriesRanges.get(series.id)
                if (!range) return null
                const span = range.max - range.min
                if (span < 1e-12) return 50
                return ((value - range.min) / span) * 100
            }
            return raw
        }
        const smooth = (value: number): number => {
            if (scene.smoothingWindow <= 1) return value
            smoothQueue.push(value)
            if (smoothQueue.length > scene.smoothingWindow) smoothQueue.shift()
            return smoothQueue.reduce((sum, item) => sum + item, 0) / smoothQueue.length
        }
        context.strokeStyle = series.color
        context.beginPath()
        if (!useDecimation) {
            let started = false
            for (let index = 0; index < orderCount; index++) {
                const value = valueForY(series.values[index])
                if (value === null) {
                    started = false
                    smoothQueue.length = 0
                    continue
                }
                const x = toX(scene.times[index])
                const y = toY(smooth(value))
                if (!started) {
                    context.moveTo(x, y)
                    started = true
                } else {
                    context.lineTo(x, y)
                }
            }
        } else {
            columnMin.fill(Number.POSITIVE_INFINITY)
            columnMax.fill(Number.NEGATIVE_INFINITY)
            for (let index = 0; index < orderCount; index += orderStep) {
                const value = valueForY(series.values[index])
                if (value === null) continue
                const drawValue = smooth(value)
                const column = Math.min(plotColumns - 1, Math.max(0, Math.floor((toX(scene.times[index]) - padding.left) / columnScale)))
                if (drawValue < columnMin[column]) columnMin[column] = drawValue
                if (drawValue > columnMax[column]) columnMax[column] = drawValue
            }
            let previous = -2
            for (let column = 0; column < plotColumns; column++) {
                if (!Number.isFinite(columnMin[column])) continue
                const x = padding.left + column * columnScale + columnScale * 0.5
                const yTop = toY(columnMax[column])
                const yBottom = toY(columnMin[column])
                if (column !== previous + 1) context.moveTo(x, yTop)
                else context.lineTo(x, yTop)
                context.lineTo(x, yBottom)
                previous = column
            }
        }
        context.stroke()
    }

    const ranges: Record<string, [number, number]> = {}
    for (const [id, range] of seriesRanges) ranges[id] = [range.min, range.max]
    return {
        bitmap: canvas.transferToImageBitmap(),
        geometry: {padding, plotWidth, plotHeight, xMin: scene.xMin, xMax: scene.xMax, minValue, maxValue, yMode: scene.yMode, ranges},
    }
}

scope.onmessage = (event: MessageEvent<ChartScene>) => {
    const scene = event.data
    try {
        const startedAt = performance.now()
        const result = drawScene(scene)
        scope.postMessage({type: 'bitmap', requestId: scene.requestId, bitmap: result.bitmap,
          geometry: result.geometry, drawMs: performance.now() - startedAt}, [result.bitmap])
    } catch (error) {
        scope.postMessage({type: 'error', requestId: scene.requestId, error: error instanceof Error ? error.message : String(error)})
    }
}
