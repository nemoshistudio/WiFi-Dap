<template>
  <div class="curve-layout">
    <h1 class="page-title">{{ translate('page.debugCurve') }}</h1>

    <el-alert
        class="mb-3"
        type="warning"
        :title="translate('curve.takeoverHint')"
        :closable="false"
        show-icon
    />

    <el-alert
        v-if="kickWarning"
        class="mb-3"
        type="warning"
        :title="kickWarning"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="haltedSuspected"
        class="mb-3"
        type="warning"
        :title="translate('curve.targetHalted')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="notice"
        class="mb-3"
        :type="notice.type"
        :title="notice.title"
        :closable="false"
        show-icon
    />

    <!-- 连接 / 采样控制 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center gap-2">
        <el-tag :type="statusTagType" effect="dark">{{ statusText }}</el-tag>
        <span class="font-mono text-xs opacity-70">{{ endpointText }}</span>
        <span v-if="targetInfo" class="font-mono text-xs opacity-70">
          IDCODE {{ formatHex32(targetInfo.idcode) }} · AP IDR {{ formatHex32(targetInfo.apIdr) }}
        </span>
        <span class="flex-1"></span>

        <el-button v-if="!connected" type="primary" :loading="connecting" @click="onConnect">
          {{ translate('curve.connect') }}
        </el-button>
        <template v-else>
          <el-button v-if="!sampling" type="primary" :disabled="seriesList.length === 0" @click="startSampling">
            {{ translate('curve.start') }}
          </el-button>
          <el-button v-else type="warning" @click="stopSampling">
            {{ translate('curve.stop') }}
          </el-button>
          <el-button :loading="resetting" @click="onResetTarget">{{ translate('curve.resetTarget') }}</el-button>
          <el-button @click="onDisconnect">{{ translate('curve.disconnect') }}</el-button>
        </template>
        <el-button v-if="haltedSuspected" type="warning" :loading="resuming" @click="onResumeTarget">
          {{ translate('curve.resumeTarget') }}
        </el-button>
        <el-button :disabled="sampleCount === 0" @click="clearData">{{ translate('curve.clear') }}</el-button>
      </div>

      <div class="mt-3 flex flex-wrap items-center gap-x-6 gap-y-2 text-sm">
        <label class="flex items-center gap-2">
          <span class="whitespace-nowrap">{{ translate('curve.sampleRate') }}</span>
          <el-select v-model="sampleRate" size="small" style="width: 8rem" :disabled="sampling">
            <el-option v-for="rate in SAMPLE_RATES" :key="rate" :value="rate" :label="rate + ' Hz'"/>
          </el-select>
        </label>
        <span>{{ translate('curve.effectiveRate') }}: <b>{{ effectiveRate.toFixed(1) }} Hz</b></span>
        <span>{{ translate('curve.points') }}: <b>{{ sampleCount }}</b></span>
        <span>{{ translate('curve.failedCount') }}: <b>{{ failedCount }}</b></span>
        <span>{{ translate('curve.droppedCount') }}: <b>{{ droppedCount }}</b></span>
        <label class="flex items-center gap-2">
          <span class="whitespace-nowrap">{{ translate('curve.format') }}</span>
          <el-select v-model="globalFormat" size="small" style="width: 6rem" @change="applyGlobalFormat">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
        </label>
      </div>
    </section>

    <!-- ELF 符号选择 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-lg font-bold">{{ translate('curve.elfUpload') }}</h2>
        <el-button :loading="elfParsing" @click="openElfDialog">{{ translate('curve.elfUpload') }}</el-button>
        <input ref="elfInputRef" type="file" accept=".elf,.out" class="hidden" @change="onElfSelected"/>
      </div>
      <p class="mt-1 text-xs opacity-70">{{ translate('curve.elfHint') }}</p>

      <p v-if="elfInfo" class="mt-2 text-sm">
        {{ translate('curve.elfParsed', {count: elfInfo.symbols.length}) }}
        <span class="opacity-70">(RAM {{ elfInfo.ramSymbols }} · Flash {{ elfInfo.flashSymbols }})</span>
      </p>
      <el-alert v-if="elfError" class="mt-2" type="error" :title="elfError" :closable="false" show-icon/>

      <div v-if="elfInfo && elfInfo.symbols.length > 0" class="mt-3 grid gap-4 md:grid-cols-2">
        <div>
          <el-input v-model="symbolSearch" size="small" clearable :placeholder="translate('curve.searchPlaceholder')"/>
          <div class="mt-1 text-xs opacity-70">{{ translate('curve.variables') }}: {{ filteredSymbols.length }}</div>
          <div class="symbol-list">
            <label v-for="symbol in filteredSymbols" :key="symbol.name + '@' + formatHex32(symbol.address)" class="symbol-row">
              <el-checkbox :model-value="isSelected(symbol)" @change="toggleSymbol(symbol)"/>
              <span class="flex-1 truncate" :title="symbol.name">{{ symbol.name }}</span>
              <span class="font-mono text-xs opacity-70">{{ formatHex32(symbol.address) }}</span>
              <span class="w-10 text-right text-xs opacity-60">{{ symbol.size }}</span>
            </label>
          </div>
        </div>
        <div>
          <div class="text-xs opacity-70">{{ translate('curve.selected') }}: {{ seriesList.length }} / {{ MAX_CURVE_SERIES }}</div>
          <div class="symbol-list">
            <div v-for="item in seriesList" :key="item.id" class="symbol-row">
              <span class="series-dot" :style="{background: item.color}"></span>
              <span class="flex-1 truncate" :title="item.name">{{ item.name }}</span>
              <span class="font-mono text-xs opacity-70">{{ formatHex32(item.address) }}</span>
              <el-select v-model="item.format" size="small" style="width: 5.5rem">
                <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
              </el-select>
              <el-button type="danger" link size="small" @click="removeSeries(item.id)">×</el-button>
            </div>
          </div>
        </div>
      </div>

      <div class="mt-3 flex flex-wrap items-center gap-2">
        <span class="text-sm font-bold">{{ translate('curve.manualAddress') }}</span>
        <el-input
            v-model="manualInput"
            size="small"
            class="manual-input"
            :placeholder="translate('curve.manualAddressHint')"
            @keyup.enter="addManualAddress"
        />
        <el-button size="small" @click="addManualAddress">{{ translate('curve.addAddress') }}</el-button>
      </div>
      <el-alert v-if="addressError" class="mt-2" type="warning" :title="addressError" :closable="false" show-icon/>
    </section>

    <!-- 实时曲线 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <span class="text-sm opacity-70">{{ translate('curve.points') }}: {{ sampleCount }} · 20 s</span>
        <div class="flex gap-2">
          <el-button size="small" :disabled="sampleCount === 0" @click="exportCsv">
            {{ translate('curve.exportCsv') }}
          </el-button>
          <el-button size="small" :disabled="sampleCount === 0" @click="exportPng">
            {{ translate('curve.exportPng') }}
          </el-button>
        </div>
      </div>

      <div ref="chartWrapRef" class="chart-wrap">
        <canvas ref="canvasRef" @mousemove="onCanvasMove" @mouseleave="onCanvasLeave"></canvas>
        <div v-if="sampleCount === 0" class="chart-empty">{{ translate('curve.noData') }}</div>
        <div v-if="hover" class="chart-tooltip" :style="hoverStyle">
          <div class="font-mono">{{ hover.time.toFixed(3) }} s</div>
          <div v-for="entry in hover.entries" :key="entry.name" class="flex items-center gap-1">
            <span class="series-dot" :style="{background: entry.color}"></span>
            <span class="truncate">{{ entry.name }}</span>
            <span class="ml-auto font-mono">{{ entry.text }}</span>
          </div>
        </div>
      </div>

      <div v-if="seriesList.length > 0" class="legend">
        <button
            v-for="item in seriesList"
            :key="item.id"
            type="button"
            class="legend-item"
            :class="{off: !item.visible}"
            :title="item.name"
            @click="toggleVisible(item)"
        >
          <span class="series-dot" :style="{background: item.color}"></span>
          <span class="truncate">{{ item.name }}</span>
          <span class="font-mono text-xs opacity-60">{{ latestText(item) }}</span>
        </button>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import {computed, onBeforeUnmount, onMounted, ref, shallowRef} from 'vue'
import {ElMessageBox} from 'element-plus'
import {translate} from '@/locales'
import {globalNotify} from '@/composables/notification'
import {CmsisDapClient, MAX_CURVE_SERIES, WsDapTransport, type DapTargetInfo} from '@/api/wsDap'
import {kickDapSession} from '@/api/apiDap'
import {
  formatHex32,
  parseElf32Symbols,
  type ElfParseResult,
  type ElfSymbol,
} from '@/utils/elfSymbols'

type DisplayFormat = 'u32' | 'i32' | 'f32'

const FORMATS: DisplayFormat[] = ['u32', 'i32', 'f32']
const SAMPLE_RATES = [10, 20, 50, 100]
/** 环形缓冲总点数（约 10 分钟 @100Hz）。 */
const MAX_POINTS = 60000
/** 曲线窗口（秒）：X 轴显示最近 20 秒，自动滚动。 */
const WINDOW_SECONDS = 20
const COLORS = [
  '#2563eb', '#dc2626', '#16a34a', '#d97706', '#7c3aed', '#0891b2',
  '#db2777', '#65a30d', '#4f46e5', '#ea580c', '#0d9488', '#9333ea',
  '#b91c1c', '#047857', '#a16207', '#1d4ed8', '#c026d3', '#475569',
  '#15803d', '#be123c', '#4338ca', '#0f766e', '#a21caf', '#92400e',
]

interface CurveSeries {
  id: number
  name: string
  address: number
  format: DisplayFormat
  color: string
  visible: boolean
  /** 该曲线开始采样的 sampleSeq：中途新增的曲线不回填旧点。 */
  startSeq: number
}

interface CurveNotice {
  type: 'success' | 'info' | 'warning' | 'error'
  title: string
}

const SAMPLE_RATES_DEFAULT = 100

// ---- 页面状态 -------------------------------------------------------------
const sampleRate = ref(SAMPLE_RATES_DEFAULT)
const globalFormat = ref<DisplayFormat>('u32')
const seriesList = ref<CurveSeries[]>([])
const symbolSearch = ref('')
const manualInput = ref('')
const addressError = ref('')
const elfParsing = ref(false)
const elfInfo = shallowRef<ElfParseResult | null>(null)
const elfError = ref('')
const elfInputRef = ref<HTMLInputElement | null>(null)
const connecting = ref(false)
const connected = ref(false)
const sampling = ref(false)
const everSampled = ref(false)
const statusError = ref(false)
const haltedSuspected = ref(false)
const resuming = ref(false)
const resetting = ref(false)
const targetInfo = shallowRef<DapTargetInfo | null>(null)
const notice = ref<CurveNotice | null>(null)
const kickWarning = ref('')
const sampleCount = ref(0)
const failedCount = ref(0)
const droppedCount = ref(0)
const effectiveRate = ref(0)

const canvasRef = ref<HTMLCanvasElement | null>(null)
const chartWrapRef = ref<HTMLDivElement | null>(null)
const hover = ref<{ index: number; x: number; y: number; time: number; entries: Array<{ name: string; color: string; text: string }> } | null>(null)

let client: CmsisDapClient | null = null
let transport: WsDapTransport | null = null
let nextSeriesId = 1
let sampleTimer = 0
let sampleSeq = 0
let sampleEpochMs = 0
let nextSampleAt = 0
let consecutiveFailures = 0
let recentCompletions: number[] = []
let constantReference: number[] | null = null
let constantSamples = 0
let resizeObserver: ResizeObserver | null = null
let drawPending = false

// 环形缓冲：timestamps/slotSeq 按写入顺序共享，原始值每个曲线一份 Uint32Array。
const timestamps = new Float64Array(MAX_POINTS)
const slotSeq = new Float64Array(MAX_POINTS)
const seriesBuffers = new Map<number, Uint32Array>()
let ringHead = 0
let ringCount = 0

const f32Scratch = new Float32Array(1)
const u32Scratch = new Uint32Array(f32Scratch.buffer)

// ---- 连接状态 -------------------------------------------------------------
const statusTagType = computed<'primary' | 'success' | 'info' | 'warning' | 'danger'>(() => {
  if (connecting.value) return 'warning'
  if (!connected.value) return 'info'
  if (sampling.value) return 'success'
  if (statusError.value) return 'danger'
  if (everSampled.value) return 'warning'
  return 'primary'
})

const statusText = computed(() => {
  if (connecting.value) return translate('curve.statusConnecting')
  if (!connected.value) return translate('curve.statusIdle')
  if (sampling.value) return translate('curve.statusSampling')
  if (statusError.value) return translate('curve.statusError')
  if (everSampled.value) return translate('curve.statusStopped')
  return translate('curve.statusConnected')
})

function dapHost(): string {
  if (import.meta.env.VITE_APP_MODE === 'dev') {
    const override = import.meta.env.VITE_DEVICE_HOST_NAME
    if (override) return override.replace(/^https?:\/\//, '').split('/')[0].split(':')[0]
  }
  return window.location.hostname || 'dap.local'
}

const endpointText = computed(() => WsDapTransport.urlForHost(dapHost()))

function handleTransportClosed(reason: string): void {
  stopSampling()
  connected.value = false
  client = null
  transport = null
  targetInfo.value = null
  notice.value = {type: 'error', title: translate('curve.reconnectHint')}
  if (import.meta.env.VITE_APP_MODE === 'dev') {
    console.warn('[curve] transport closed:', reason)
  }
  scheduleDraw()
}

/** 接管后 WS 握手重试：USBIP 会话断开后设备需要一点时间重新接受 3240 连接。 */
async function openDapTransportWithRetry(): Promise<WsDapTransport> {
  const deadline = performance.now() + 2200
  let lastError: unknown = null
  while (performance.now() < deadline) {
    const candidate = new WsDapTransport(WsDapTransport.urlForHost(dapHost()))
    try {
      await candidate.open(800)
      return candidate
    } catch (error) {
      lastError = error
      candidate.close('connect retry')
      await new Promise<void>((resolve) => setTimeout(resolve, 200))
    }
  }
  const finalTransport = new WsDapTransport(WsDapTransport.urlForHost(dapHost()))
  try {
    await finalTransport.open(1200)
    return finalTransport
  } catch (error) {
    finalTransport.close('connect failed')
    throw lastError ?? error
  }
}

async function onConnect(): Promise<void> {
  if (connecting.value || connected.value) return
  const confirmed = await ElMessageBox.confirm(
      translate('curve.takeoverHint'),
      translate('curve.takeoverConfirmTitle'),
      {
        confirmButtonText: translate('common.confirm'),
        cancelButtonText: translate('common.cancel'),
        type: 'warning',
      },
  ).then(() => true).catch(() => false)
  if (!confirmed) return

  connecting.value = true
  notice.value = null
  kickWarning.value = ''
  statusError.value = false
  let pendingTransport: WsDapTransport | null = null
  try {
    // 接管：先 POST /api/dap/kick 让固件断开占用中的 USBIP（或 elaphureLink）会话，
    // 等 ~300ms 后再连 WS；USBIP 仍占用时首次 WS 握手会超时，故最多重试 ~2s。
    // 端点缺失或失败时仍继续尝试 WS-DAP：用户也可能已手动断开 USBIP。
    const kick = await kickDapSession()
    if (!kick.ok) {
      kickWarning.value = translate('curve.kickFailed')
    } else {
      await new Promise<void>((resolve) => setTimeout(resolve, 300))
    }

    pendingTransport = await openDapTransportWithRetry()
    pendingTransport.onClose = (reason) => handleTransportClosed(reason)
    const nextClient = new CmsisDapClient(pendingTransport)
    const info = await nextClient.connect()
    transport = pendingTransport
    pendingTransport = null
    client = nextClient
    targetInfo.value = info
    connected.value = true
    kickWarning.value = ''
    resetConstantTracking()
    // 目标可能在接管时仍处于 halt（DHCSR.S_HALT）：提示用户可手动恢复运行。
    haltedSuspected.value = !info.resumed
    globalNotify(translate('curve.statusConnected'), 'success')
  } catch (error) {
    const message = error instanceof Error ? error.message : 'unknown error'
    if (pendingTransport) {
      pendingTransport.close('connect failed')
      pendingTransport = null
    }
    const failedTransport = transport
    transport = null
    client = null
    connected.value = false
    if (failedTransport) {
      failedTransport.close('connect failed')
    }
    notice.value = {type: 'error', title: translate('curve.connectFailed', {err: message})}
  } finally {
    connecting.value = false
  }
}

async function onDisconnect(): Promise<void> {
  stopSampling()
  const current = client
  const currentTransport = transport
  client = null
  transport = null
  connected.value = false
  targetInfo.value = null
  statusError.value = false
  resetConstantTracking()
  if (current) {
    await current.disconnect()
  }
  if (currentTransport) {
    currentTransport.close('user disconnect')
  }
  notice.value = {type: 'info', title: translate('curve.usbipReattachHint')}
  scheduleDraw()
}

// ---- 采样循环 -------------------------------------------------------------
function startSampling(): void {
  if (!connected.value || !client || seriesList.value.length === 0) return
  if (sampling.value) return
  statusError.value = false
  notice.value = null
  failedCount.value = 0
  consecutiveFailures = 0
  recentCompletions = []
  effectiveRate.value = 0
  resetConstantTracking()
  const lastT = sampleCount.value > 0 ? timestamps[(ringHead - 1 + MAX_POINTS) % MAX_POINTS] : 0
  sampleEpochMs = performance.now() - lastT * 1000
  nextSampleAt = performance.now()
  sampling.value = true
  everSampled.value = true
  scheduleNextSample(0)
}

function stopSampling(): void {
  sampling.value = false
  if (sampleTimer) {
    window.clearTimeout(sampleTimer)
    sampleTimer = 0
  }
}

function scheduleNextSample(delay: number): void {
  if (!sampling.value) return
  if (sampleTimer) window.clearTimeout(sampleTimer)
  sampleTimer = window.setTimeout(runSample, Math.max(0, delay))
}

async function runSample(): Promise<void> {
  sampleTimer = 0
  if (!sampling.value || !client) return
  const begin = performance.now()
  // 固定本次采样的曲线快照：读取期间用户可能增删曲线/改地址，不能按新数组取值。
  const snapshot = seriesList.value.slice()
  const addresses = snapshot.map((item) => item.address)
  try {
    const values = await client.sampleWords(addresses)
    const date = (begin - sampleEpochMs) / 1000
    pushSample(date, values, snapshot)
    trackConstantValues(values)
    consecutiveFailures = 0
  } catch (error) {
    consecutiveFailures += 1
    failedCount.value += 1
    if (!sampling.value) return
    if (consecutiveFailures >= 3) {
      stopSampling()
      statusError.value = true
      const message = error instanceof Error ? error.message : 'unknown error'
      if (transport === null || transport.isClosed) {
        notice.value = {type: 'error', title: translate('curve.reconnectHint')}
      } else {
        notice.value = {type: 'error', title: translate('curve.connectFailed', {err: message})}
      }
      scheduleDraw()
      return
    }
    // 读失败：不额外计 dropped，等一个节拍后重试（连续失败 3 次会停止采样）。
    nextSampleAt = performance.now() + 1000 / sampleRate.value
    scheduleNextSample(1000 / sampleRate.value)
    return
  }
  const completedAt = performance.now()
  recentCompletions.push(completedAt)
  const cutoff = completedAt - 1000
  while (recentCompletions.length > 0 && recentCompletions[0] < cutoff) recentCompletions.shift()
  effectiveRate.value = recentCompletions.length
  advanceSchedule()
  scheduleDraw()
}

function resetConstantTracking(): void {
  constantReference = null
  constantSamples = 0
  haltedSuspected.value = false
}

/**
 * 目标 halt 时所有变量值恒定（真机接管后常见 DHCSR.S_HALT）。
 * 连续 25 个采样点完全不变时提示用户（可手动点“恢复目标运行”）。
 */
function trackConstantValues(values: number[]): void {
  if (constantReference === null || constantReference.length !== values.length) {
    constantReference = values.map((value) => value >>> 0)
    constantSamples = 0
    return
  }
  let same = true
  for (let i = 0; i < values.length; i++) {
    if ((values[i] >>> 0) !== constantReference[i]) {
      same = false
      break
    }
  }
  if (same) {
    constantSamples += 1
    if (constantSamples >= 25) haltedSuspected.value = true
  } else {
    constantReference = values.map((value) => value >>> 0)
    constantSamples = 0
    haltedSuspected.value = false
  }
}

async function onResetTarget(): Promise<void> {
    const current = client
    if (!current || resetting.value) return
    resetting.value = true
    try {
        await current.resetAndRun()
        haltedSuspected.value = false
    } catch (err) {
        globalNotify(translate('curve.connectFailed', {err: String(err)}), 'error')
    } finally {
        resetting.value = false
    }
}

async function onResumeTarget(): Promise<void> {
  const current = client
  if (!current || resuming.value) return
  resuming.value = true
  try {
    await current.resumeTarget()
    resetConstantTracking()
    globalNotify(translate('common.success'), 'success')
  } catch (error) {
    const message = error instanceof Error ? error.message : 'unknown error'
    notice.value = {type: 'error', title: translate('curve.connectFailed', {err: message})}
  } finally {
    resuming.value = false
  }
}

/** 维持固定采样节拍；落后超过一个节拍时计入 dropped 并重新对齐（不补发）。 */
function advanceSchedule(): void {
  if (!sampling.value) return
  const period = 1000 / sampleRate.value
  const now = performance.now()
  nextSampleAt += period
  if (nextSampleAt < now) {
    const missed = Math.max(1, Math.floor((now - nextSampleAt) / period))
    droppedCount.value += missed
    nextSampleAt = now + period
  }
  scheduleNextSample(nextSampleAt - performance.now())
}

function pushSample(time: number, values: number[], snapshot: readonly CurveSeries[]): void {
  const slot = ringHead
  timestamps[slot] = time
  slotSeq[slot] = sampleSeq
  for (let i = 0; i < snapshot.length; i++) {
    const buffer = seriesBuffers.get(snapshot[i].id)
    if (buffer) buffer[slot] = (values[i] ?? 0) >>> 0
  }
  ringHead = (ringHead + 1) % MAX_POINTS
  if (ringCount < MAX_POINTS) ringCount += 1
  sampleSeq += 1
  sampleCount.value = ringCount
}

function clearData(): void {
  ringHead = 0
  ringCount = 0
  sampleSeq = 0
  sampleCount.value = 0
  failedCount.value = 0
  droppedCount.value = 0
  effectiveRate.value = 0
  everSampled.value = sampling.value
  statusError.value = false
  recentCompletions = []
  hover.value = null
  resetConstantTracking()
  for (const item of seriesList.value) item.startSeq = 0
  scheduleDraw()
}

// ---- 曲线管理 -------------------------------------------------------------
function newSeries(name: string, address: number): CurveSeries | null {
  if (seriesList.value.length >= MAX_CURVE_SERIES) {
    globalNotify(translate('curve.maxSeries', {max: MAX_CURVE_SERIES}), 'warning')
    return null
  }
  const series: CurveSeries = {
    id: nextSeriesId++,
    name,
    address: address >>> 0,
    format: globalFormat.value,
    color: COLORS[seriesList.value.length % COLORS.length],
    visible: true,
    startSeq: sampleSeq + (sampling.value ? 1 : 0),
  }
  seriesBuffers.set(series.id, new Uint32Array(MAX_POINTS))
  seriesList.value = [...seriesList.value, series]
  scheduleDraw()
  return series
}

function removeSeries(id: number): void {
  seriesBuffers.delete(id)
  seriesList.value = seriesList.value.filter((item) => item.id !== id)
  hover.value = null
  scheduleDraw()
}

function toggleVisible(item: CurveSeries): void {
  item.visible = !item.visible
  scheduleDraw()
}

function applyGlobalFormat(): void {
  for (const item of seriesList.value) item.format = globalFormat.value
  scheduleDraw()
}

function symbolKey(symbol: ElfSymbol): string {
  return `${symbol.name}@${symbol.address}`
}

function isSelected(symbol: ElfSymbol): boolean {
  return seriesList.value.some((item) => symbolKey(symbol) === `${item.name}@${item.address}`)
}

function toggleSymbol(symbol: ElfSymbol): void {
  if (isSelected(symbol)) {
    const remaining = seriesList.value.filter((item) => `${item.name}@${item.address}` !== symbolKey(symbol))
    for (const item of seriesList.value) {
      if (!remaining.includes(item)) seriesBuffers.delete(item.id)
    }
    seriesList.value = remaining
    scheduleDraw()
    return
  }
  newSeries(symbol.name, symbol.address)
}

const filteredSymbols = computed<ElfSymbol[]>(() => {
  const info = elfInfo.value
  if (!info) return []
  const query = symbolSearch.value.trim().toLowerCase()
  if (!query) return info.symbols
  return info.symbols.filter((symbol) => {
    if (symbol.name.toLowerCase().includes(query)) return true
    return formatHex32(symbol.address).toLowerCase().includes(query)
  })
})

function openElfDialog(): void {
  elfInputRef.value?.click()
}

async function onElfSelected(event: Event): Promise<void> {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  elfParsing.value = true
  elfError.value = ''
  try {
    const buffer = await file.arrayBuffer()
    const result = parseElf32Symbols(buffer)
    elfInfo.value = result
    if (result.symbols.length === 0) {
      elfError.value = translate('curve.noSymbols')
    }
  } catch (error) {
    elfInfo.value = null
    const message = error instanceof Error ? error.message : 'unknown error'
    elfError.value = translate('curve.elfInvalid', {err: message})
  } finally {
    elfParsing.value = false
  }
}

function parseAddressToken(token: string): number | null {
  let text = token.trim()
  if (!text) return null
  if (/^0x/i.test(text)) text = text.slice(2)
  if (!/^[0-9a-fA-F]{1,8}$/.test(text)) return null
  const value = Number.parseInt(text, 16) >>> 0
  if (value === 0 || value % 4 !== 0) return null
  return value
}

function addManualAddress(): void {
  addressError.value = ''
  const tokens = manualInput.value.split(/[\s,;]+/).filter((token) => token.length > 0)
  if (tokens.length === 0) {
    addressError.value = translate('curve.invalidAddress')
    return
  }
  const added: string[] = []
  for (const token of tokens) {
    const address = parseAddressToken(token)
    if (address === null) {
      addressError.value = translate('curve.invalidAddress')
      continue
    }
    if (seriesList.value.length >= MAX_CURVE_SERIES) {
      globalNotify(translate('curve.maxSeries', {max: MAX_CURVE_SERIES}), 'warning')
      break
    }
    if (seriesList.value.some((item) => item.address === address)) continue
    if (newSeries(formatHex32(address), address)) added.push(formatHex32(address))
  }
  if (added.length > 0) manualInput.value = ''
}

// ---- 数值格式 -------------------------------------------------------------
function rawToNumber(raw: number, format: DisplayFormat): number {
  if (format === 'i32') return raw | 0
  if (format === 'f32') {
    u32Scratch[0] = raw >>> 0
    return f32Scratch[0]
  }
  return raw >>> 0
}

function formatValue(raw: number, format: DisplayFormat): string {
  if (format === 'f32') {
    const value = rawToNumber(raw, format)
    if (!Number.isFinite(value)) return String(value)
    return String(Number.parseFloat(value.toPrecision(6)))
  }
  return String(rawToNumber(raw, format))
}

function formatAxisValue(value: number): string {
  if (!Number.isFinite(value)) return ''
  const abs = Math.abs(value)
  if (abs >= 1e6 || (abs > 0 && abs < 1e-3)) return value.toExponential(2)
  if (Number.isInteger(value)) return String(value)
  return value.toFixed(2)
}

function latestText(item: CurveSeries): string {
  if (ringCount === 0) return '-'
  for (let order = ringCount - 1; order >= 0; order--) {
    const slot = ringSlot(order)
    if (slotSeq[slot] < item.startSeq) continue
    const buffer = seriesBuffers.get(item.id)
    if (!buffer) return '-'
    return formatValue(buffer[slot], item.format)
  }
  return '-'
}

function ringSlot(orderIndex: number): number {
  return (ringHead - ringCount + orderIndex + MAX_POINTS) % MAX_POINTS
}

/** 用二分查找找到第一个时间戳 >= time 的 order 下标。 */
function lowerBoundTime(time: number): number {
  let low = 0
  let high = ringCount
  while (low < high) {
    const mid = (low + high) >> 1
    if (timestamps[ringSlot(mid)] < time) low = mid + 1
    else high = mid
  }
  return low
}

// ---- Canvas 绘图 ----------------------------------------------------------
function scheduleDraw(): void {
  if (drawPending) return
  drawPending = true
  window.requestAnimationFrame(() => {
    drawPending = false
    drawChart()
  })
}

function drawChart(): void {
  const canvas = canvasRef.value
  const wrap = chartWrapRef.value
  if (!canvas || !wrap) return
  const width = Math.max(1, Math.floor(wrap.clientWidth))
  const height = Math.max(1, Math.floor(wrap.clientHeight))
  const dpr = window.devicePixelRatio || 1
  const pixelWidth = Math.max(1, Math.round(width * dpr))
  const pixelHeight = Math.max(1, Math.round(height * dpr))
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth
    canvas.height = pixelHeight
  }
  const context = canvas.getContext('2d')
  if (!context) return
  context.setTransform(dpr, 0, 0, dpr, 0, 0)
  context.clearRect(0, 0, width, height)
  context.fillStyle = '#ffffff'
  context.fillRect(0, 0, width, height)

  const padding = {left: 58, right: 14, top: 12, bottom: 26}
  const plotWidth = width - padding.left - padding.right
  const plotHeight = height - padding.top - padding.bottom
  if (plotWidth < 20 || plotHeight < 20) return

  const visible = seriesList.value.filter((item) => item.visible)
  if (ringCount === 0 || visible.length === 0) {
    context.fillStyle = '#9ca3af'
    context.font = '12px system-ui, sans-serif'
    context.textAlign = 'center'
    context.textBaseline = 'middle'
    if (ringCount === 0) return
    context.fillText(translate('curve.noData'), width / 2, height / 2)
    return
  }

  const lastTime = timestamps[ringSlot(ringCount - 1)]
  const xMax = Math.max(lastTime, 5)
  const xMin = Math.max(0, xMax - WINDOW_SECONDS)
  const startOrder = lowerBoundTime(xMin)
  if (startOrder >= ringCount) return

  // Y 自动缩放：只统计窗口内、且在该曲线创建之后的点。
  let minValue = Number.POSITIVE_INFINITY
  let maxValue = Number.NEGATIVE_INFINITY
  for (let order = startOrder; order < ringCount; order++) {
    const slot = ringSlot(order)
    for (const item of visible) {
      if (slotSeq[slot] < item.startSeq) continue
      const buffer = seriesBuffers.get(item.id)
      if (!buffer) continue
      const value = rawToNumber(buffer[slot], item.format)
      if (!Number.isFinite(value)) continue
      if (value < minValue) minValue = value
      if (value > maxValue) maxValue = value
    }
  }
  if (!Number.isFinite(minValue) || !Number.isFinite(maxValue)) return
  if (maxValue - minValue < 1e-9) {
    minValue -= 1
    maxValue += 1
  } else {
    const span = maxValue - minValue
    minValue -= span * 0.08
    maxValue += span * 0.08
  }

  const toX = (time: number) => padding.left + ((time - xMin) / (xMax - xMin)) * plotWidth
  const toY = (value: number) => padding.top + (1 - (value - minValue) / (maxValue - minValue)) * plotHeight

  // 网格 + Y 轴刻度
  context.font = '11px system-ui, sans-serif'
  context.textBaseline = 'middle'
  context.textAlign = 'right'
  context.lineWidth = 1
  const ySteps = 5
  for (let step = 0; step <= ySteps; step++) {
    const value = minValue + ((maxValue - minValue) * step) / ySteps
    const y = toY(value)
    context.strokeStyle = step === 0 || step === ySteps ? '#d1d5db' : '#eef0f3'
    context.beginPath()
    context.moveTo(padding.left, y)
    context.lineTo(padding.left + plotWidth, y)
    context.stroke()
    context.fillStyle = '#6b7280'
    context.fillText(formatAxisValue(value), padding.left - 6, y)
  }

  // X 轴刻度（秒）
  context.textAlign = 'center'
  context.textBaseline = 'top'
  const timeStep = niceTimeStep(xMax - xMin)
  const firstTick = Math.ceil(xMin / timeStep) * timeStep
  for (let time = firstTick; time <= xMax + 1e-9; time += timeStep) {
    const x = toX(time)
    context.strokeStyle = '#eef0f3'
    context.beginPath()
    context.moveTo(x, padding.top)
    context.lineTo(x, padding.top + plotHeight)
    context.stroke()
    context.fillStyle = '#6b7280'
    context.fillText(`${time.toFixed(0)}s`, x, padding.top + plotHeight + 6)
  }

  // 数据曲线（窗口内最多约 20s * 100Hz = 2000 点，无需降采样）
  for (const item of visible) {
    const buffer = seriesBuffers.get(item.id)
    if (!buffer) continue
    context.strokeStyle = item.color
    context.lineWidth = 1.5
    context.beginPath()
    let started = false
    for (let order = startOrder; order < ringCount; order++) {
      const slot = ringSlot(order)
      if (slotSeq[slot] < item.startSeq) {
        started = false
        continue
      }
      const value = rawToNumber(buffer[slot], item.format)
      if (!Number.isFinite(value)) {
        started = false
        continue
      }
      const x = toX(timestamps[slot])
      const y = toY(value)
      if (!started) {
        context.moveTo(x, y)
        started = true
      } else {
        context.lineTo(x, y)
      }
    }
    context.stroke()
  }

  // 悬停十字线 + 各曲线当前点
  const hoverState = hover.value
  if (hoverState) {
    const order = hoverState.index
    if (order >= 0 && order < ringCount) {
      const slot = ringSlot(order)
      const x = toX(timestamps[slot])
      context.strokeStyle = '#94a3b8'
      context.lineWidth = 1
      context.setLineDash([4, 3])
      context.beginPath()
      context.moveTo(x, padding.top)
      context.lineTo(x, padding.top + plotHeight)
      context.stroke()
      context.setLineDash([])
      for (const item of visible) {
        const buffer = seriesBuffers.get(item.id)
        if (!buffer || slotSeq[slot] < item.startSeq) continue
        const value = rawToNumber(buffer[slot], item.format)
        if (!Number.isFinite(value)) continue
        context.fillStyle = item.color
        context.beginPath()
        context.arc(x, toY(value), 3, 0, Math.PI * 2)
        context.fill()
      }
    }
  }
}

function niceTimeStep(span: number): number {
  const target = span / 5
  const magnitude = Math.pow(10, Math.floor(Math.log10(Math.max(target, 1e-6))))
  const candidates = [1, 2, 5, 10]
  for (const candidate of candidates) {
    if (candidate * magnitude >= target) return candidate * magnitude
  }
  return 10 * magnitude
}

function onCanvasMove(event: MouseEvent): void {
  const canvas = canvasRef.value
  if (!canvas || ringCount === 0 || seriesList.value.length === 0) return
  const rect = canvas.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top
  const padding = {left: 58, right: 14, top: 12, bottom: 26}
  const plotWidth = rect.width - padding.left - padding.right
  if (plotWidth <= 0 || x < padding.left || x > rect.width - padding.right) {
    hover.value = null
    scheduleDraw()
    return
  }
  const lastTime = timestamps[ringSlot(ringCount - 1)]
  const xMax = Math.max(lastTime, 5)
  const xMin = Math.max(0, xMax - WINDOW_SECONDS)
  const time = xMin + ((x - padding.left) / plotWidth) * (xMax - xMin)
  const startOrder = lowerBoundTime(xMin)
  const order = Math.min(Math.max(lowerBoundTime(time), startOrder), ringCount - 1)
  const slot = ringSlot(order)
  const entries: Array<{name: string; color: string; text: string}> = []
  for (const item of seriesList.value) {
    if (!item.visible || slotSeq[slot] < item.startSeq) continue
    const buffer = seriesBuffers.get(item.id)
    if (!buffer) continue
    entries.push({name: item.name, color: item.color, text: formatValue(buffer[slot], item.format)})
  }
  hover.value = {index: order, x, y, time: timestamps[slot], entries}
  scheduleDraw()
}

function onCanvasLeave(): void {
  if (!hover.value) return
  hover.value = null
  scheduleDraw()
}

const hoverStyle = computed(() => {
  const state = hover.value
  const wrap = chartWrapRef.value
  if (!state || !wrap) return {}
  const width = wrap.clientWidth
  const left = Math.min(state.x + 14, Math.max(0, width - 210))
  const top = Math.max(8, Math.min(state.y + 14, wrap.clientHeight - 90))
  return {left: `${left}px`, top: `${top}px`}
})

// ---- 导出 -----------------------------------------------------------------
function downloadBlob(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  link.click()
  window.setTimeout(() => URL.revokeObjectURL(url), 2000)
}

function fileStamp(): string {
  return new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19)
}

function csvEscape(text: string): string {
  if (/[",\n\r]/.test(text)) return `"${text.replace(/"/g, '""')}"`
  return text
}

function exportCsv(): void {
  if (ringCount === 0) return
  const header = ['time_s', ...seriesList.value.map((item) => csvEscape(item.name))]
  const lines: string[] = [header.join(',')]
  for (let order = 0; order < ringCount; order++) {
    const slot = ringSlot(order)
    const cells: string[] = [(timestamps[slot] as number).toFixed(4)]
    for (const item of seriesList.value) {
      const buffer = seriesBuffers.get(item.id)
      if (!buffer || slotSeq[slot] < item.startSeq) {
        cells.push('')
      } else {
        cells.push(formatValue(buffer[slot], item.format))
      }
    }
    lines.push(cells.join(','))
  }
  const blob = new Blob([lines.join('\n')], {type: 'text/csv;charset=utf-8'})
  downloadBlob(blob, `data-curve-${fileStamp()}.csv`)
}

function exportPng(): void {
  const canvas = canvasRef.value
  if (!canvas || ringCount === 0) return
  const previousHover = hover.value
  hover.value = null
  drawChart()
  const filename = `data-curve-${fileStamp()}.png`
  const restore = () => {
    hover.value = previousHover
    scheduleDraw()
  }
  if (typeof canvas.toBlob === 'function') {
    canvas.toBlob((blob) => {
      if (blob) downloadBlob(blob, filename)
      restore()
    }, 'image/png')
  } else {
    const link = document.createElement('a')
    link.href = canvas.toDataURL('image/png')
    link.download = filename
    link.click()
    restore()
  }
}

// ---- 生命周期 -------------------------------------------------------------
onMounted(() => {
  resizeObserver = new ResizeObserver(() => scheduleDraw())
  if (chartWrapRef.value) resizeObserver.observe(chartWrapRef.value)
  scheduleDraw()
})

onBeforeUnmount(() => {
  stopSampling()
  if (transport) transport.close('page unmount')
  transport = null
  client = null
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
})
</script>

<style scoped>
.curve-layout {
  @apply mx-auto w-full max-w-6xl px-2 pb-8;
}

.curve-card {
  @apply my-3 rounded-lg border border-gray-200 p-3;
}

.symbol-list {
  @apply mt-1 overflow-auto rounded border border-gray-200;
  max-height: 15rem;
}

.symbol-row {
  @apply flex cursor-pointer items-center gap-2 px-2 py-0.5 text-sm;
}

.symbol-row:hover {
  background-color: var(--el-color-primary-light-9);
}

.series-dot {
  @apply inline-block h-2.5 w-2.5 shrink-0 rounded-full;
}

.manual-input {
  @apply min-w-64 flex-1;
}

.chart-wrap {
  position: relative;
  @apply mt-2 h-80 w-full overflow-hidden rounded border border-gray-200;
}

.chart-wrap canvas {
  @apply block h-full w-full;
}

.chart-empty {
  @apply pointer-events-none absolute inset-0 flex items-center justify-center text-gray-400;
}

.chart-tooltip {
  @apply pointer-events-none absolute max-w-52 rounded border border-gray-200 bg-white px-2 py-1 text-xs shadow;
}

.legend {
  @apply mt-2 flex flex-wrap gap-2;
}

.legend-item {
  @apply flex max-w-64 items-center gap-1 rounded border border-gray-200 px-2 py-1 text-xs;
}

.legend-item:hover {
  background-color: var(--el-color-primary-light-9);
}

.legend-item.off {
  @apply opacity-40;
}
</style>
