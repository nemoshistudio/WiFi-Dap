/**
 * 可选 WebGL 曲线渲染后端（task-67 / P1-15）。
 *
 * 定位：只在 feature flag 打开、且是大系列（默认 A 模式 >= 64 路）时由
 * `curveChartWorker` 选择；小系列 / 平滑 / 不支持 / context lost / 导出等场景
 * 回退现有 OffscreenCanvas 2D（曲线）与主线程 Canvas（最终显示/PNG）。
 *
 * 与 2D 路径的关键差异（数据接口，避免每帧全量上传）：
 *  - Worker 自持 ring（ChartRingStore）是权威数据源；本模块只把 **增量** 写进 GPU
 *    缓冲：time/seq 两个共享 VBO（capacity×float）+ series 交错 value VBO（按
 *    ≤63 列一组，规避 WebGL1 的 vertex attrib stride ≤255 限制；256 路 = 5 组）。
 *    首次 / series 集合变化 / ring 重写时一次性全量上传；之后每次 draw 只
 *    `bufferSubData` 上次 draw 以来新写入的 ring slot（连续区间，ring 回绕最多 2 段）。
 *  - 顶点着色器直接读 GPU 里的 ring：每 series 一次 `LINE_STRIP`（物理写序即时间序，
 *    满 ring 回绕拆成 2 段），窗口外点由裁剪自然裁掉；`startSeq`/NaN 由顶点标记 +
 *    片元丢弃处理。
 *  - 每帧 CPU 只做：抽样 Y 范围（预算 2 万值，等价主线程 Y_SCAN_BUDGET）、
 *    NDC 缩放常量、每 series 的 uniform/draw 调用；不再构建像素列包络，也不做
 *    CPU 光栅。曲线由 GPU 绘制到一个透明 OffscreenCanvas，Worker 再用 2D canvas
 *    叠背景/网格/刻度文字（样式与 2D 路径一致），最后把合成位图 transfer 回主线程。
 *
 * 明确边界（回退而非伪实现）：
 *  - 平滑（smoothingWindow > 1）：GPU 顶点着色器无法等价实现滑动平均 → 2D；
 *  - 派生曲线（+/- 等）本来就不进 Worker → 主线程 2D；
 *  - hover/双光标/图例/冻结/录制/FFT 主线程 overlay 与数据逻辑不变，WebGL 只影响
 *    主曲线位图；
 *  - 极窄窗口/无数据 → 空几何，交给调用方画背景；
 *  - 与 2D 像素列包络的视觉差异：WebGL 画全分辨率折线，不会把一列内的 min/max
 *    都画成竖线；大系列 A 模式下两者都是近似可视化，WebGL 可能漏掉“单点尖峰”的
 *    极值像素。Y 轴范围用抽样扫描而非精确扫描（与主线程 Canvas fallback 的
 *    Y_SCAN_BUDGET 行为一致）。
 *
 * 本模块是纯 TS（只依赖 WebGL 接口，不 import DOM/Worker 全局），Node 单测用
 * fake GL 验证 buffer 复用/增量上传/回退语义。
 */
import {
    CHART_DEFAULT_MAX_POINTS,
    ChartRingStore,
    computeChartRange,
    type ChartDrawMessage,
    type ChartGeometry,
    type ChartYMode,
} from './curveChartCore'

export type ChartWebglKind = 'webgl2' | 'webgl1'

type ChartGl = WebGLRenderingContext | WebGL2RenderingContext

/** OffscreenCanvas（或测试 fake）的最小结构；worker 里传真实 OffscreenCanvas。 */
export interface ChartWebglCanvasLike {
    width: number
    height: number
    getContext(kind: string, attributes?: Record<string, unknown>): unknown
    transferToImageBitmap?: () => ImageBitmap
}

export interface ChartWebglFrameStats {
    renderer: ChartWebglKind
    series: number
    groups: number
    vertices: number
    drawCalls: number
    uploadCalls: number
    uploadBytes: number
    uploadMode: 'full' | 'delta' | 'none'
    rangeMs: number
    glMs: number
    drawMs: number
    points: number
    /** 每帧上传字节 / 全量字节，用于诊断（1 = 全量上传）。 */
    uploadRatio: number
}

export interface ChartWebglFrameResult {
    geometry: ChartGeometry | null
    stats: ChartWebglFrameStats
}

/** WebGL 路径 Y 范围扫描预算（与主线程 Y_SCAN_BUDGET 一致，约 2 万值/帧）。 */
export const CHART_WEBGL_Y_SCAN_BUDGET = 20000

/**
 * 每个交错 VBO 的 series 列数上限。WebGL1 的 `vertexAttribPointer` stride 必须
 * ≤ 255（63×4=252），256 路 A 模式因此拆成 5 个 value VBO（每帧增量上传仍只
 * 覆盖新 slot 的连续块）。
 */
export const CHART_WEBGL_SERIES_PER_GROUP = 63

const VERTEX_SHADER = `
attribute float a_time;
attribute float a_seq;
attribute float a_value;
uniform float u_scaleX;
uniform float u_offsetX;
uniform float u_yBottom;
uniform float u_yHeight;
uniform float u_rangeMin;
uniform float u_rangeMax;
uniform float u_startSeq;
uniform int u_yMode;
varying float v_valid;
void main() {
    float value = a_value;
    float valid = 1.0;
    if (a_seq < u_startSeq) {
        valid = 0.0;
        value = 0.0;
    } else if (u_yMode == 1) {
        if (value > 0.0) {
            value = log2(value) * 0.301029995663981;
        } else {
            valid = 0.0;
            value = 0.0;
        }
    }
    // NaN 检测（GLSL ES 1.00 无 isnan；NaN != NaN）。
    if (value != value) {
        valid = 0.0;
        value = 0.0;
    }
    float span = u_rangeMax - u_rangeMin;
    float weight = span > 1e-12 ? (value - u_rangeMin) / span : 0.5;
    vec2 position = vec2(a_time * u_scaleX + u_offsetX, u_yBottom + u_yHeight * weight);
    if (valid < 0.5) position = vec2(3.0, 3.0);
    gl_Position = vec4(position, 0.0, 1.0);
    v_valid = valid;
}
`

const FRAGMENT_SHADER = `
precision mediump float;
uniform vec4 u_color;
varying float v_valid;
void main() {
    if (v_valid < 0.999) discard;
    gl_FragColor = u_color;
}
`

interface ProgramBundle {
    program: WebGLProgram
    aTime: number
    aSeq: number
    aValue: number
    uScaleX: WebGLUniformLocation | null
    uOffsetX: WebGLUniformLocation | null
    uYBottom: WebGLUniformLocation | null
    uYHeight: WebGLUniformLocation | null
    uRangeMin: WebGLUniformLocation | null
    uRangeMax: WebGLUniformLocation | null
    uStartSeq: WebGLUniformLocation | null
    uYMode: WebGLUniformLocation | null
    uColor: WebGLUniformLocation | null
}

/** 一组 series 交错的 GPU value 缓冲（capacity × count）。 */
interface ValueGroup {
    buffer: WebGLBuffer
    staging: Float32Array
    count: number
    /** 该组在 columnIds 中的起始列。 */
    startColumn: number
}

function compileShader(gl: ChartGl, type: number, source: string): WebGLShader {
    const shader = gl.createShader(type)
    if (!shader) throw new Error('createShader failed')
    gl.shaderSource(shader, source)
    gl.compileShader(shader)
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        const info = gl.getShaderInfoLog(shader) ?? ''
        gl.deleteShader(shader)
        throw new Error(`shader compile failed: ${info.slice(0, 200)}`)
    }
    return shader
}

function linkProgram(gl: ChartGl): ProgramBundle {
    const vertex = compileShader(gl, gl.VERTEX_SHADER, VERTEX_SHADER)
    const fragment = compileShader(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER)
    const program = gl.createProgram()
    if (!program) {
        gl.deleteShader(vertex)
        gl.deleteShader(fragment)
        throw new Error('createProgram failed')
    }
    gl.attachShader(program, vertex)
    gl.attachShader(program, fragment)
    gl.linkProgram(program)
    gl.deleteShader(vertex)
    gl.deleteShader(fragment)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        const info = gl.getProgramInfoLog(program) ?? ''
        gl.deleteProgram(program)
        throw new Error(`program link failed: ${info.slice(0, 200)}`)
    }
    return {
        program,
        aTime: gl.getAttribLocation(program, 'a_time'),
        aSeq: gl.getAttribLocation(program, 'a_seq'),
        aValue: gl.getAttribLocation(program, 'a_value'),
        uScaleX: gl.getUniformLocation(program, 'u_scaleX'),
        uOffsetX: gl.getUniformLocation(program, 'u_offsetX'),
        uYBottom: gl.getUniformLocation(program, 'u_yBottom'),
        uYHeight: gl.getUniformLocation(program, 'u_yHeight'),
        uRangeMin: gl.getUniformLocation(program, 'u_rangeMin'),
        uRangeMax: gl.getUniformLocation(program, 'u_rangeMax'),
        uStartSeq: gl.getUniformLocation(program, 'u_startSeq'),
        uYMode: gl.getUniformLocation(program, 'u_yMode'),
        uColor: gl.getUniformLocation(program, 'u_color'),
    }
}

/** 复用同一 OffscreenCanvas：先试 WebGL2，再试 WebGL1；都失败返回 null。 */
export function createChartWebglRenderer(canvas: ChartWebglCanvasLike): ChartWebglRenderer | null {
    const attributes = {
        alpha: true,
        antialias: true,
        depth: false,
        stencil: false,
        premultipliedAlpha: true,
        preserveDrawingBuffer: false,
        powerPreference: 'high-performance',
    }
    for (const [kind, contextId] of [['webgl2', 'webgl2'], ['webgl1', 'webgl']] as const) {
        const gl = canvas.getContext(contextId, attributes) as ChartGl | null
        if (!gl) continue
        try {
            return new ChartWebglRenderer(canvas, gl, kind)
        } catch {
            continue
        }
    }
    return null
}

/**
 * WebGL ring 渲染器。
 *
 * 生命周期：worker 首次判定可用时创建；`reset()` 跟随主线程 clearData；
 * 每收到一次 `samples` 调 `onSample()`（只记脏 slot，不做 GL 调用）；
 * 每次 draw 调 `draw(scene, store)`（增量上传 + 每 series LINE_STRIP + 返回几何）。
 */
export class ChartWebglRenderer {
    readonly canvas: ChartWebglCanvasLike
    readonly gl: ChartGl
    readonly kind: ChartWebglKind

    private capacity = CHART_DEFAULT_MAX_POINTS
    private columnIds: number[] = []
    private columnIndex = new Map<number, number>()
    private columnGroup: number[] = []
    private columnGroupOffset: number[] = []
    private groups: ValueGroup[] = []
    private colorCache = new Map<string, [number, number, number, number] | null>()

    private program: ProgramBundle | null = null
    private timeBuffer: WebGLBuffer | null = null
    private seqBuffer: WebGLBuffer | null = null
    private timeStaging = new Float32Array(0)
    private seqStaging = new Float32Array(0)
    private inited = false
    private lost = false
    private pixelWidth = 0
    private pixelHeight = 0

    /** 上次 draw 之后新写入的 ring slot 数（最多 capacity；达到 capacity 即全量）。 */
    private pendingCount = 0
    private pendingEndSlot = -1
    private fullSyncPending = true
    /** 诊断：最近一次 draw 的上传字节/调用数。 */
    private lastUploadBytes = 0
    private lastUploadCalls = 0

    constructor(canvas: ChartWebglCanvasLike, gl: ChartGl, kind: ChartWebglKind) {
        this.canvas = canvas
        this.gl = gl
        this.kind = kind
        try {
            const target = canvas as unknown as EventTarget
            if (typeof target.addEventListener === 'function') {
                target.addEventListener('webglcontextlost', (event: Event) => {
                    event.preventDefault?.()
                    this.lost = true
                })
                target.addEventListener('webglcontextrestored', () => {
                    // 上下文恢复后 GL 资源全部失效：标全量重建，下一次 draw 重新初始化。
                    this.lost = false
                    this.inited = false
                    this.program = null
                    this.timeBuffer = null
                    this.seqBuffer = null
                    this.groups = []
                    this.fullSyncPending = true
                })
            }
        } catch {
            /* 某些实现不是 EventTarget：靠 gl.isContextLost() 兜底 */
        }
    }

    get isLost(): boolean {
        return this.lost || this.gl.isContextLost()
    }

    /** 当前 GPU 列映射（诊断/测试用）。 */
    get mappedSeries(): readonly number[] {
        return this.columnIds
    }

    get capacityPoints(): number {
        return this.capacity
    }

    /** 最近一次 draw 的增量上传字节数（诊断用）。 */
    get uploadedBytes(): number {
        return this.lastUploadBytes
    }

    /** 与主线程 clearData 同步：ring 清空、下一次全量同步；VBO 复用不重建。 */
    reset(): void {
        this.pendingCount = 0
        this.pendingEndSlot = -1
        this.fullSyncPending = true
        this.lastUploadBytes = 0
        this.lastUploadCalls = 0
    }

    /** 容量变化（init 消息）：重建 time/seq staging + 各 value 组（数据清零，需全量）。 */
    resize(capacity: number): void {
        const next = Math.max(1, Math.floor(capacity))
        if (next === this.capacity && this.timeStaging.length === next) {
            this.reset()
            return
        }
        const gl = this.gl
        for (const group of this.groups) gl.deleteBuffer(group.buffer)
        this.groups = []
        this.capacity = next
        this.pendingCount = 0
        this.pendingEndSlot = -1
        this.fullSyncPending = true
        this.timeStaging = new Float32Array(next)
        this.seqStaging = new Float32Array(next)
        this.seqStaging.fill(-1)
        this.timeBuffer = this.replaceBuffer(this.timeBuffer, next * 4)
        this.seqBuffer = this.replaceBuffer(this.seqBuffer, next * 4)
        this.columnIds = []
        this.columnIndex.clear()
        this.columnGroup = []
        this.columnGroupOffset = []
    }

    /**
     * Worker 每写入一次 ring 后调用（`store.push` 之后）：只记录“有新的 ring slot”，
     * 不复制数据、不做 GL 调用；真实上传在 draw 时按连续区间批量做。
     */
    onSample(store: ChartRingStore): void {
        if (this.lost) return
        if (store.capacity !== this.capacity) this.resize(store.capacity)
        const slot = (store.head - 1 + store.capacity) % store.capacity
        if (this.pendingCount >= this.capacity) return
        // pendingEndSlot 始终指向最近一次写入的物理 slot，pendingCount 回溯起点。
        this.pendingEndSlot = slot
        this.pendingCount += 1
        if (this.pendingCount >= this.capacity) {
            // 一个 draw 周期内写满整个 ring：直接全量，避免逐段计算。
            this.pendingCount = 0
            this.pendingEndSlot = -1
            this.fullSyncPending = true
        }
    }

    /**
     * 绘制一帧：增量上传 → GPU 画全部 series → 返回几何（Worker 负责叠背景/坐标轴）。
     * 抛错表示 WebGL 不可用/上下文丢失，调用方应永久回退 2D。
     */
    draw(scene: ChartDrawMessage, store: ChartRingStore): ChartWebglFrameResult {
        const startedAt = performance.now()
        if (this.isLost) throw new Error('webgl context lost')
        if (store.capacity !== this.capacity) this.resize(store.capacity)
        this.ensureInited()
        const gl = this.gl
        const program = this.program as ProgramBundle
        this.syncSize(scene)

        const width = Math.max(1, Math.floor(scene.width))
        const height = Math.max(1, Math.floor(scene.height))
        const padding = scene.padding
        const plotWidth = width - padding.left - padding.right
        const plotHeight = height - padding.top - padding.bottom

        this.syncColumns(scene)
        const startedRange = performance.now()
        const startOrder = store.count > 0 ? store.lowerBoundTime(scene.xMin) : 0
        const range = store.count >= 2 && scene.series.length > 0 && plotWidth >= 2 && plotHeight >= 2
            ? computeChartRange(store, scene.series, startOrder, scene.yMode, CHART_WEBGL_Y_SCAN_BUDGET)
            : null
        const rangeMs = performance.now() - startedRange

        // 先清成透明：几何为空时也要返回干净的位图（Worker 会叠背景）。
        gl.viewport(0, 0, Math.max(1, this.pixelWidth), Math.max(1, this.pixelHeight))
        gl.clearColor(0, 0, 0, 0)
        gl.clear(gl.COLOR_BUFFER_BIT)

        const uploadMode = this.upload(store)
        const uploadBytes = this.lastUploadBytes
        const glStarted = performance.now()
        let drawCalls = 0
        let vertices = 0
        let geometry: ChartGeometry | null = null

        if (range && startOrder < store.count) {
            gl.useProgram(program.program)
            gl.enable(gl.BLEND)
            gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
            gl.lineWidth(1)
            const {minValue, maxValue} = range
            const span = scene.xMax - scene.xMin
            const scaleX = (2 * plotWidth) / (span * width)
            const offsetX = (2 * padding.left) / width - 1 - scaleX * scene.xMin
            const yBottom = 1 - (2 * (padding.top + plotHeight)) / height
            const yHeight = (2 * plotHeight) / height
            this.set1f(program.uScaleX, scaleX)
            this.set1f(program.uOffsetX, offsetX)
            this.set1f(program.uYBottom, yBottom)
            this.set1f(program.uYHeight, yHeight)
            this.setYModeUniform(program, scene.yMode)
            this.bindTimeSeqAttributes(program)
            const count = store.count
            for (const spec of scene.series) {
                const column = this.columnIndex.get(spec.id)
                if (column === undefined) continue
                let seriesMin = minValue
                let seriesMax = maxValue
                let visibleRange: {min: number; max: number} | undefined
                if (scene.yMode === 'normalized') {
                    visibleRange = range.ranges.get(spec.id)
                    if (!visibleRange) continue
                    seriesMin = visibleRange.min
                    seriesMax = visibleRange.max
                }
                const color = this.parseColor(spec.color)
                if (!color) continue
                this.set4f(program.uColor, color[0], color[1], color[2], color[3])
                this.set1f(program.uRangeMin, seriesMin)
                this.set1f(program.uRangeMax, seriesMax)
                this.set1f(program.uStartSeq, spec.startSeq)
                // value 缓冲按 ≤63 列一组交错；stride ≤ 255 兼容 WebGL1。
                const group = this.groups[this.columnGroup[column]]
                if (!group) continue
                gl.bindBuffer(gl.ARRAY_BUFFER, group.buffer)
                gl.vertexAttribPointer(program.aValue, 1, gl.FLOAT, false, group.count * 4, this.columnGroupOffset[column] * 4)
                const firstOrder = lowerBoundSeq(store, spec.startSeq, startOrder)
                if (firstOrder >= count) continue
                let remaining = count - firstOrder
                let physical = store.slotOf(firstOrder)
                let seriesVertices = 0
                while (remaining > 0) {
                    const take = Math.min(remaining, store.capacity - physical)
                    if (take >= 2) {
                        gl.drawArrays(gl.LINE_STRIP, physical, take)
                        drawCalls += 1
                        seriesVertices += take
                    }
                    remaining -= take
                    physical = 0
                }
                vertices += seriesVertices
            }
            const normalizedRanges: Record<string, [number, number]> = {}
            for (const [id, seriesRange] of range.ranges) normalizedRanges[String(id)] = [seriesRange.min, seriesRange.max]
            geometry = {
                padding,
                plotWidth,
                plotHeight,
                xMin: scene.xMin,
                xMax: scene.xMax,
                minValue,
                maxValue,
                yMode: scene.yMode,
                ranges: normalizedRanges,
            }
        }
        const glMs = performance.now() - glStarted
        const fullBytes = this.fullUploadBytes()
        return {
            geometry,
            stats: {
                renderer: this.kind,
                series: this.columnIds.length,
                groups: this.groups.length,
                vertices,
                drawCalls,
                uploadCalls: this.lastUploadCalls,
                uploadBytes,
                uploadMode,
                rangeMs,
                glMs,
                drawMs: performance.now() - startedAt,
                points: store.count,
                uploadRatio: fullBytes > 0 ? Math.min(1, uploadBytes / fullBytes) : 0,
            },
        }
    }

    /** 把 WebGL 透明曲线层转成 ImageBitmap（Worker 合成用）。失败抛错 → 回退 2D。 */
    transferBitmap(): ImageBitmap {
        if (typeof this.canvas.transferToImageBitmap !== 'function') {
            throw new Error('OffscreenCanvas.transferToImageBitmap unavailable for webgl')
        }
        return this.canvas.transferToImageBitmap()
    }

    dispose(): void {
        const gl = this.gl
        for (const buffer of [this.timeBuffer, this.seqBuffer]) {
            if (buffer) gl.deleteBuffer(buffer)
        }
        for (const group of this.groups) gl.deleteBuffer(group.buffer)
        this.groups = []
        this.timeBuffer = null
        this.seqBuffer = null
        if (this.program) gl.deleteProgram(this.program.program)
        this.program = null
        this.inited = false
    }

    // ---- 内部实现 -----------------------------------------------------------
    private ensureInited(): void {
        if (this.inited) return
        const gl = this.gl
        this.program = linkProgram(gl)
        if (this.timeStaging.length !== this.capacity) this.timeStaging = new Float32Array(this.capacity)
        if (this.seqStaging.length !== this.capacity) this.seqStaging = new Float32Array(this.capacity)
        this.seqStaging.fill(-1)
        this.timeBuffer = this.timeBuffer ?? gl.createBuffer()
        this.seqBuffer = this.seqBuffer ?? gl.createBuffer()
        if (!this.timeBuffer || !this.seqBuffer) throw new Error('createBuffer failed')
        gl.bindBuffer(gl.ARRAY_BUFFER, this.timeBuffer)
        gl.bufferData(gl.ARRAY_BUFFER, this.capacity * 4, gl.DYNAMIC_DRAW)
        gl.bindBuffer(gl.ARRAY_BUFFER, this.seqBuffer)
        gl.bufferData(gl.ARRAY_BUFFER, this.capacity * 4, gl.DYNAMIC_DRAW)
        this.columnIds = []
        this.columnIndex.clear()
        this.columnGroup = []
        this.columnGroupOffset = []
        this.groups = []
        this.fullSyncPending = true
        this.inited = true
    }

    private replaceBuffer(buffer: WebGLBuffer | null, bytes: number): WebGLBuffer {
        const gl = this.gl
        const next = buffer ?? gl.createBuffer()
        if (!next) throw new Error('createBuffer failed')
        gl.bindBuffer(gl.ARRAY_BUFFER, next)
        gl.bufferData(gl.ARRAY_BUFFER, Math.max(4, bytes), gl.DYNAMIC_DRAW)
        return next
    }

    private syncSize(scene: ChartDrawMessage): void {
        const dpr = Math.min(2, Math.max(1, scene.dpr))
        const pixelWidth = Math.max(1, Math.round(Math.max(1, Math.floor(scene.width)) * dpr))
        const pixelHeight = Math.max(1, Math.round(Math.max(1, Math.floor(scene.height)) * dpr))
        if (this.canvas.width !== pixelWidth || this.canvas.height !== pixelHeight) {
            this.canvas.width = pixelWidth
            this.canvas.height = pixelHeight
        }
        this.pixelWidth = pixelWidth
        this.pixelHeight = pixelHeight
    }

    /** 列映射（series id -> value 组/列）；变化时重建 value VBO 并全量同步。 */
    private syncColumns(scene: ChartDrawMessage): boolean {
        const ids = scene.series.map((spec) => spec.id)
        let changed = ids.length !== this.columnIds.length
        if (!changed) {
            for (let index = 0; index < ids.length; index++) {
                if (ids[index] !== this.columnIds[index]) { changed = true; break }
            }
        }
        if (!changed) return false
        const gl = this.gl
        for (const group of this.groups) gl.deleteBuffer(group.buffer)
        this.columnIds = ids.slice()
        this.columnIndex.clear()
        this.columnGroup = []
        this.columnGroupOffset = []
        this.groups = []
        for (let start = 0; start < this.columnIds.length; start += CHART_WEBGL_SERIES_PER_GROUP) {
            const count = Math.min(CHART_WEBGL_SERIES_PER_GROUP, this.columnIds.length - start)
            const staging = new Float32Array(this.capacity * count)
            const buffer = gl.createBuffer()
            if (!buffer) throw new Error('createBuffer failed')
            gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
            gl.bufferData(gl.ARRAY_BUFFER, Math.max(4, staging.byteLength), gl.DYNAMIC_DRAW)
            const groupIndex = this.groups.length
            this.groups.push({buffer, staging, count, startColumn: start})
            for (let offset = 0; offset < count; offset++) {
                this.columnIndex.set(this.columnIds[start + offset], start + offset)
                this.columnGroup[start + offset] = groupIndex
                this.columnGroupOffset[start + offset] = offset
            }
        }
        this.fullSyncPending = true
        return true
    }

    /** 从 marker 给出的环形区间读出物理 slot 范围（最多 2 段）。 */
    private pendingRanges(): Array<[number, number]> {
        if (this.pendingCount <= 0 || this.pendingEndSlot < 0) return []
        const count = Math.min(this.pendingCount, this.capacity)
        const end = this.pendingEndSlot
        const first = (end - count + 1 + this.capacity) % this.capacity
        if (first <= end) return [[first, end]]
        return [[first, this.capacity - 1], [0, end]]
    }

    /**
     * 增量上传：把上次 draw 以来新写入的 ring slot 写进 GPU（time/seq 共享 VBO +
     * 分组 value VBO）。全量仅发生在：首次、reset 后、series 集合变化、一个 draw
     * 周期写满整个 ring。
     */
    private upload(store: ChartRingStore): 'full' | 'delta' | 'none' {
        const gl = this.gl
        this.lastUploadCalls = 0
        this.lastUploadBytes = 0
        if (this.groups.length === 0 || this.columnIds.length === 0 || store.count === 0) {
            this.pendingCount = 0
            return 'none'
        }
        if (this.fullSyncPending) {
            this.fullSync(store)
            return 'full'
        }
        const ranges = this.pendingRanges()
        this.pendingCount = 0
        this.pendingEndSlot = -1
        if (ranges.length === 0) return 'none'
        for (const [first, last] of ranges) this.uploadRange(first, last, store)
        return 'delta'
    }

    private fullSync(store: ChartRingStore): void {
        const gl = this.gl
        const capacity = this.capacity
        this.timeStaging.set(store.times.subarray(0, capacity))
        const wrapped = store.head !== store.count
        for (let slot = 0; slot < capacity; slot++) {
            this.seqStaging[slot] = !wrapped && slot >= store.count ? -1 : store.slotSeq[slot]
        }
        gl.bindBuffer(gl.ARRAY_BUFFER, this.timeBuffer)
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, this.timeStaging)
        gl.bindBuffer(gl.ARRAY_BUFFER, this.seqBuffer)
        gl.bufferSubData(gl.ARRAY_BUFFER, 0, this.seqStaging)
        this.lastUploadCalls += 2
        this.lastUploadBytes += capacity * 4 * 2
        for (const group of this.groups) {
            group.staging.fill(Number.NaN)
            for (let column = 0; column < group.count; column++) {
                const values = store.seriesValues(this.columnIds[group.startColumn + column])
                for (let order = 0; order < store.count; order++) {
                    const slot = store.slotOf(order)
                    group.staging[slot * group.count + column] = values[slot]
                }
            }
            gl.bindBuffer(gl.ARRAY_BUFFER, group.buffer)
            gl.bufferSubData(gl.ARRAY_BUFFER, 0, group.staging)
            this.lastUploadCalls += 1
            this.lastUploadBytes += group.staging.byteLength
        }
        this.fullSyncPending = false
        this.pendingCount = 0
        this.pendingEndSlot = -1
    }

    private uploadRange(first: number, last: number, store: ChartRingStore): void {
        const gl = this.gl
        const length = last - first + 1
        if (length <= 0) return
        for (let slot = first; slot <= last; slot++) {
            this.timeStaging[slot] = store.times[slot]
            this.seqStaging[slot] = store.slotSeq[slot]
        }
        gl.bindBuffer(gl.ARRAY_BUFFER, this.timeBuffer)
        gl.bufferSubData(gl.ARRAY_BUFFER, first * 4, this.timeStaging.subarray(first, last + 1))
        gl.bindBuffer(gl.ARRAY_BUFFER, this.seqBuffer)
        gl.bufferSubData(gl.ARRAY_BUFFER, first * 4, this.seqStaging.subarray(first, last + 1))
        this.lastUploadCalls += 2
        this.lastUploadBytes += length * 4 * 2
        for (const group of this.groups) {
            const columns = group.count
            for (let column = 0; column < columns; column++) {
                const values = store.seriesValues(this.columnIds[group.startColumn + column])
                for (let slot = first; slot <= last; slot++) {
                    group.staging[slot * columns + column] = values[slot]
                }
            }
            gl.bindBuffer(gl.ARRAY_BUFFER, group.buffer)
            gl.bufferSubData(gl.ARRAY_BUFFER, first * columns * 4, group.staging.subarray(first * columns, (last + 1) * columns))
            this.lastUploadCalls += 1
            this.lastUploadBytes += length * columns * 4
        }
    }

    private fullUploadBytes(): number {
        return this.capacity * 4 * 2 + this.capacity * Math.max(1, this.columnIds.length) * 4
    }

    /** a_time / a_seq 是整 ring 共享属性，每帧绑定一次。 */
    private bindTimeSeqAttributes(program: ProgramBundle): void {
        const gl = this.gl
        gl.bindBuffer(gl.ARRAY_BUFFER, this.timeBuffer)
        gl.enableVertexAttribArray(program.aTime)
        gl.vertexAttribPointer(program.aTime, 1, gl.FLOAT, false, 4, 0)
        gl.bindBuffer(gl.ARRAY_BUFFER, this.seqBuffer)
        gl.enableVertexAttribArray(program.aSeq)
        gl.vertexAttribPointer(program.aSeq, 1, gl.FLOAT, false, 4, 0)
        gl.enableVertexAttribArray(program.aValue)
    }

    private set1f(location: WebGLUniformLocation | null, value: number): void {
        if (location) this.gl.uniform1f(location, value)
    }

    private set4f(location: WebGLUniformLocation | null, r: number, g: number, b: number, a: number): void {
        if (location) this.gl.uniform4f(location, r, g, b, a)
    }

    private setYModeUniform(program: ProgramBundle, mode: ChartYMode): void {
        if (!program.uYMode) return
        this.gl.uniform1i(program.uYMode, mode === 'log' ? 1 : mode === 'normalized' ? 2 : 0)
    }

    private parseColor(color: string): [number, number, number, number] | null {
        if (this.colorCache.has(color)) return this.colorCache.get(color) ?? null
        const parsed = parseCssColor(color)
        this.colorCache.set(color, parsed)
        return parsed
    }
}

/** logical order -> 第一个 slotSeq >= startSeq 的 order（ring 内 seq 单调）。 */
export function lowerBoundSeq(store: ChartRingStore, startSeq: number, from: number): number {
    let low = Math.max(0, Math.floor(from))
    let high = store.count
    while (low < high) {
        const mid = (low + high) >>> 1
        if (store.slotSeq[store.slotOf(mid)] < startSeq) low = mid + 1
        else high = mid
    }
    return low
}

/** 解析 #rgb/#rrggbb/#rrggbbaa 与 rgb()/rgba()（worker 里 palette/series 色都是 hex）。 */
export function parseCssColor(color: string): [number, number, number, number] | null {
    const value = color.trim()
    if (value.startsWith('#')) {
        const hex = value.slice(1)
        if (hex.length === 3) {
            const r = Number.parseInt(hex[0] + hex[0], 16)
            const g = Number.parseInt(hex[1] + hex[1], 16)
            const b = Number.parseInt(hex[2] + hex[2], 16)
            return Number.isFinite(r) && Number.isFinite(g) && Number.isFinite(b) ? [r / 255, g / 255, b / 255, 1] : null
        }
        if (hex.length === 6 || hex.length === 8) {
            const r = Number.parseInt(hex.slice(0, 2), 16)
            const g = Number.parseInt(hex.slice(2, 4), 16)
            const b = Number.parseInt(hex.slice(4, 6), 16)
            const a = hex.length === 8 ? Number.parseInt(hex.slice(6, 8), 16) / 255 : 1
            return Number.isFinite(r) && Number.isFinite(g) && Number.isFinite(b) && Number.isFinite(a)
                ? [r / 255, g / 255, b / 255, a]
                : null
        }
        return null
    }
    const match = value.match(/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)\s*)?\)$/i)
    if (!match) return null
    const r = Number(match[1]) / 255
    const g = Number(match[2]) / 255
    const b = Number(match[3]) / 255
    const a = match[4] === undefined ? 1 : Number(match[4])
    if (![r, g, b, a].every((channel) => Number.isFinite(channel))) return null
    return [r, g, b, a]
}
