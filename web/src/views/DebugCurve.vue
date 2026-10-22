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
        v-if="haltedSuspected && !isLocalSource"
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
        <template v-if="!isLocalSource">
          <span class="font-mono text-xs opacity-70">{{ endpointText }}</span>
          <span v-if="targetInfo" class="font-mono text-xs opacity-70">
            IDCODE {{ formatHex32(targetInfo.idcode) }} · AP IDR {{ formatHex32(targetInfo.apIdr) }}
          </span>
        </template>
        <span class="flex-1"></span>

        <template v-if="isLocalSource">
          <el-button v-if="!sampling" type="primary" :disabled="localStartDisabled" @click="startSampling">
            {{ translate('curve.start') }}
          </el-button>
          <el-button v-else type="warning" @click="stopSampling">
            {{ translate('curve.stop') }}
          </el-button>
        </template>
        <template v-else>
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
        </template>
        <el-button :disabled="sampleCount === 0" @click="clearData">{{ translate('curve.clear') }}</el-button>
      </div>

      <div class="mt-3 flex flex-wrap items-center gap-x-6 gap-y-2 text-sm">
        <label class="flex items-center gap-2">
          <span class="whitespace-nowrap">{{ translate('curve.samplingSource') }}</span>
          <el-select v-model="samplingSource" size="small" style="width: 13rem" :disabled="sampling" @change="onSamplingSourceChange">
            <el-option
                v-for="source in SAMPLING_SOURCES"
                :key="source"
                :value="source"
                :label="source === 'local' ? translate('curve.samplingSourceLocal') : translate('curve.samplingSourceWeb')"
            />
          </el-select>
        </label>
        <template v-if="isLocalSource">
          <label class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.localTrigger') }}</span>
            <el-select v-model="localTrigger" size="small" style="width: 8rem" :disabled="sampling" @change="onLocalTriggerChange">
              <el-option value="periodic" :label="translate('curve.localTriggerPeriodic')"/>
              <el-option value="breakpoint" :label="translate('curve.localTriggerBreakpoint')"/>
            </el-select>
          </label>
          <label v-if="localTrigger === 'breakpoint'" class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.localBreakpoint') }}</span>
            <el-input
                v-model="localBreakpointInput"
                size="small"
                style="width: 9.5rem"
                :disabled="sampling"
                placeholder="0x08001234"
                class="breakpoint-input"
                :class="{'is-invalid': !!localBreakpointError}"
                @input="onBreakpointAddressInput"
            />
          </label>
          <label v-if="localTrigger === 'breakpoint'" class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.localFunction') }}</span>
            <el-select
                v-model="selectedFunctionAddress"
                size="small"
                filterable
                clearable
                style="width: 18rem"
                :disabled="sampling || elfFunctionSymbols.length === 0"
                :placeholder="elfFunctionSymbols.length === 0 ? translate('curve.localFunctionEmpty') : translate('curve.localFunctionPlaceholder')"
                @change="onFunctionSelected"
            >
              <el-option
                  v-for="symbol in elfFunctionSymbols"
                  :key="symbol.address + ':' + symbol.name"
                  :value="symbol.address"
                  :label="formatFunctionOption(symbol)"
              >
                <span class="font-mono">{{ formatHex32(symbol.address) }}</span>
                <span class="ml-2">{{ symbol.displayName }}</span>
              </el-option>
            </el-select>
            <span class="text-xs opacity-70">{{ translate('curve.localFunctionHint', {count: elfFunctionSymbols.length}) }}</span>
          </label>
          <label v-if="localTrigger === 'breakpoint'" class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.localOffset') }}</span>
            <el-input
                v-model="localBreakpointOffsetInput"
                size="small"
                style="width: 5.5rem"
                :disabled="sampling"
                placeholder="0x10 / 16"
            />
            <span v-if="breakpointResolution?.ok" class="font-mono text-xs opacity-70">{{ formatHex32(breakpointResolution.address) }}</span>
          </label>
          <label class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ localTrigger === 'breakpoint' ? translate('curve.localMinInterval') : translate('curve.localInterval') }}</span>
            <el-select v-model="localIntervalMs" size="small" style="width: 6rem" :disabled="sampling">
              <el-option v-for="interval in LOCAL_INTERVALS" :key="interval" :value="interval" :label="interval + ' ms'"/>
            </el-select>
          </label>
          <span>{{ translate('curve.effectiveRate') }}: <b>{{ effectiveRate.toFixed(1) }} Hz</b></span>
          <span>{{ translate('curve.points') }}: <b>{{ sampleCount }}</b></span>
          <span>{{ translate('curve.localStatus') }}: <b>{{ (sampling && localStatusInfo?.running !== false) ? translate('curve.localRunning') : translate('curve.localStopped') }}</b></span>
          <span v-if="localTrigger === 'periodic'">Halt: <b>{{ localStatusInfo?.halt === false ? 'OFF' : (localStatusInfo?.halt === true ? 'ON' : '-') }}</b></span>
          <span v-else>{{ translate('curve.localHits') }}: <b>{{ localStatusInfo?.hits ?? localHits ?? '-' }}</b></span>
          <span v-if="localTrigger === 'breakpoint'">{{ translate('curve.localSampled') }}: <b>{{ localStatusInfo?.sampled ?? localSampled ?? '-' }}</b></span>
          <span>{{ translate('curve.localFrames') }}: <b>{{ localFramesReceived }}</b></span>
          <span>{{ translate('curve.localInvalid') }}: <b>{{ localInvalidCount }}</b></span>
          <span>{{ translate('curve.localOverwritten') }}: <b>{{ localOverwrittenCount }}</b></span>
        </template>
        <template v-else>
          <label class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.samplingMode') }}</span>
            <el-select v-model="samplingMode" size="small" style="width: 12rem" :disabled="sampling" @change="onSamplingModeChange">
              <el-option value="live" :label="translate('curve.samplingModeLive')"/>
              <el-option value="debug" :label="translate('curve.samplingModeDebug')"/>
            </el-select>
          </label>
          <label v-if="!isDebugMode" class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.sampleRate') }}</span>
            <el-select v-model="sampleRate" size="small" style="width: 8rem" :disabled="sampling">
              <el-option
                  v-for="rate in SAMPLE_RATES"
                  :key="rate"
                  :value="rate"
                  :label="rate === 0 ? translate('curve.sampleRateAuto') : rate + ' Hz'"
              />
            </el-select>
          </label>
          <label v-else class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ translate('curve.debugInterval') }}</span>
            <el-select v-model="debugIntervalMs" size="small" style="width: 7rem" :disabled="sampling">
              <el-option v-for="interval in DEBUG_INTERVALS" :key="interval" :value="interval" :label="interval + ' ms'"/>
            </el-select>
          </label>
          <span>{{ translate('curve.effectiveRate') }}: <b>{{ effectiveRate.toFixed(1) }} Hz</b></span>
          <span v-if="!isDebugMode && sampleRate === 0">{{ translate('curve.autoRate') }}: <b>{{ autoRateDisplay }} Hz</b></span>
          <span>{{ translate('curve.points') }}: <b>{{ sampleCount }}</b></span>
          <span>{{ translate('curve.failedCount') }}: <b>{{ failedCount }}</b></span>
          <span>{{ translate('curve.droppedCount') }}: <b>{{ droppedCount }}</b></span>
        </template>
        <label class="flex items-center gap-2">
          <span class="whitespace-nowrap">{{ translate('curve.format') }}</span>
          <el-select v-model="globalFormat" size="small" style="width: 6rem" @change="applyGlobalFormat">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
        </label>
      </div>
      <div class="mt-1 text-xs opacity-70">
        {{ isLocalSource
          ? (localTrigger === 'breakpoint' ? translate('curve.localBreakpointHint') : translate('curve.localModeHint'))
          : (isDebugMode ? translate('curve.debugModeHint') : translate('curve.samplingHint')) }}
      </div>
      <div v-if="isLocalSource && localTrigger === 'breakpoint' && localBreakpointError" class="mt-1 text-xs" style="color: #dc2626">
        {{ localBreakpointError }}
      </div>
    </section>

    <!-- ELF / DWARF 变量浏览器 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-lg font-bold">{{ translate('curve.elfUpload') }}</h2>
        <el-button :loading="elfParsing" :disabled="isLocalSource && sampling" @click="openElfDialog">{{ translate('curve.elfUpload') }}</el-button>
        <input ref="elfInputRef" type="file" accept=".elf,.out" class="hidden" @change="onElfSelected"/>
      </div>
      <p class="mt-1 text-xs opacity-70">{{ translate('curve.elfHint') }}</p>

      <div v-if="elfInfo" class="mt-2">
        <p v-if="elfInfo.source === 'dwarf'" class="text-sm">
          {{ translate('curve.dwarfParsed', {count: elfInfo.variables.length, fixed: elfInfo.dwarfWithAddress}) }}
          <span v-if="unsampleableCount > 0" class="block text-xs opacity-70">
            {{ translate('curve.unsampleableHint', {count: unsampleableCount}) }}
          </span>
          <span v-if="elfInfo.dwarfHaltSamplable > 0" class="block text-xs opacity-70">
            {{ translate('curve.haltSamplableHint', {count: elfInfo.dwarfHaltSamplable}) }}
          </span>
          <span v-if="elfInfo.dwarfPointerExpanded > 0" class="block text-xs opacity-70">
            {{ translate('curve.pointerExpandedHint', {count: elfInfo.dwarfPointerExpanded}) }}
          </span>
          <span class="block text-xs opacity-70">{{ translate('curve.debugInfoHint') }}</span>
        </p>
        <template v-else>
          <el-alert
              class="mt-1"
              type="info"
              :title="translate('curve.symtabNotice')"
              :closable="false"
              show-icon
          />
          <p v-if="dwarfFallbackReason" class="mt-1 text-xs opacity-70">
            {{ translate('curve.dwarfFallback', {err: dwarfFallbackReason}) }}
          </p>
          <p class="mt-1 text-sm">{{ translate('curve.elfParsed', {count: elfInfo.variables.length}) }}</p>
        </template>
        <p v-if="wideHint" class="mt-1 text-xs opacity-70">{{ translate('curve.wideHint') }}</p>
      </div>
      <el-alert v-if="elfError" class="mt-2" type="error" :title="elfError" :closable="false" show-icon/>
      <el-alert v-if="addressError" class="mt-2" type="warning" :title="addressError" :closable="false" show-icon/>

      <template v-if="elfInfo && elfInfo.variables.length > 0">
        <div class="mt-2 flex flex-wrap items-center gap-2">
          <el-input
              v-model="variableSearch"
              size="small"
              clearable
              class="variable-search"
              :placeholder="translate('curve.searchPlaceholder')"
          />
          <el-checkbox v-model="onlySampleable" size="small">{{ translate('curve.onlySampleable') }}</el-checkbox>
          <label class="flex items-center gap-1 text-xs">
            <span class="whitespace-nowrap">{{ translate('curve.groupBy') }}</span>
            <el-select v-model="groupBy" size="small" style="width: 9rem">
              <el-option v-for="option in GROUP_OPTIONS" :key="option" :value="option" :label="groupLabel(option)"/>
            </el-select>
          </label>
          <el-button size="small" @click="expandAll">{{ translate('curve.expandAll') }}</el-button>
          <el-button size="small" @click="collapseAll">{{ translate('curve.collapseAll') }}</el-button>
          <span class="ml-auto text-xs opacity-70">{{ translate('curve.variables') }}: {{ visibleVariableCount }}</span>
        </div>

        <div class="var-table mt-2" :key="variableListEpoch">
          <div class="var-head">
            <span></span>
            <span>{{ translate('curve.colName') }}</span>
            <span>{{ translate('curve.colType') }}</span>
            <span>{{ translate('curve.colAddress') }}</span>
            <span class="text-right">{{ translate('curve.colSize') }}</span>
            <span>{{ translate('curve.colSource') }}</span>
          </div>
          <div class="var-list">
            <template v-for="row in variableRows" :key="row.key">
              <div v-if="row.kind === 'group'" class="var-group-row">
                <span class="truncate font-semibold" :title="row.groupLabel">{{ row.groupLabel }}</span>
                <span class="opacity-60">({{ row.groupCount }})</span>
              </div>
              <div
                  v-else
                  class="var-row"
                  :class="{selected: isRowSelected(row), unsampleable: !row.node.sampleable}"
              >
                <span class="var-check">
                  <el-checkbox
                      v-if="nodeSelectable(row.node)"
                      :model-value="isRowSelected(row)"
                      :disabled="isLocalSource && sampling"
                      @change="toggleRow(row)"
                  />
                </span>
                <span
                    class="var-name"
                    :style="{paddingLeft: (row.depth * 12) + 'px'}"
                    :title="[row.path, nodeLocationText(row.node), row.node.mangledName].filter(Boolean).join('\n')"
                >
                  <button
                      v-if="row.hasChildren"
                      type="button"
                      class="tree-arrow"
                      @click.stop="toggleExpand(row)"
                  >{{ row.expanded ? '▾' : '▸' }}</button>
                  <span v-else class="tree-arrow-spacer"></span>
                  <span class="truncate">{{ row.node.name }}</span>
                </span>
                <span class="var-type truncate" :title="row.node.typeName">{{ row.node.typeName || translate('curve.unknownType') }}</span>
                <span v-if="isLocalSource && isLocalRegisterValue(row.node)" class="var-addr unsampleable-text" :title="translate('curve.localRegisterUnsupported')">
                  {{ translate('curve.localRegisterShort') }}
                </span>
                <span v-else-if="row.node.sampleable" class="var-addr font-mono">
                  <template v-if="row.node.sample && row.node.sample.address">{{ formatHex32(row.node.sample.address) }}</template>
                  <span v-else class="mode-badge" :title="nodeLocationText(row.node)">
                    {{ row.node.liveSamplable ? translate('curve.pointerSamplable') : translate('curve.debugSamplable') }}
                  </span>
                </span>
                <span v-else class="var-addr unsampleable-text" :title="translate('curve.unsampleable')">{{ translate('curve.unsampleable') }}</span>
                <span class="var-size text-right">{{ row.node.size }}</span>
                <span class="var-src truncate" :title="row.node.file || ''">{{ sourceLabel(row.node) }}</span>
              </div>
            </template>
            <div v-if="visibleVariableCount === 0" class="px-2 py-2 text-sm opacity-70">
              {{ translate('curve.noSymbols') }}
            </div>
          </div>
        </div>
      </template>

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
    </section>

    <!-- 已选曲线 -->
    <section v-if="seriesList.length > 0" class="curve-card">
      <div class="text-xs opacity-70">{{ translate('curve.selected') }}: {{ seriesList.length }} / {{ MAX_CURVE_SERIES }}</div>
      <div class="selected-list mt-1">
        <div v-for="item in seriesList" :key="item.id" class="selected-row">
          <span class="series-dot" :style="{background: item.color}"></span>
          <span class="truncate" :title="item.name">{{ item.name }}</span>
          <span class="truncate text-xs opacity-70" :title="item.typeName">{{ item.typeName || translate('curve.unknownType') }}</span>
          <span class="font-mono text-xs opacity-70" :title="item.location ? addressExprText(item.location) : ''">{{ item.location ? addressExprText(item.location) : formatHex32(item.address) }}</span>
          <el-select v-model="item.format" size="small" style="width: 5.5rem">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
          <el-button type="danger" link size="small" @click="removeSeries(item.id)">×</el-button>
        </div>
      </div>
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
            :title="legendLabel(item)"
            @click="toggleVisible(item)"
        >
          <span class="series-dot" :style="{background: item.color}"></span>
          <span class="truncate">{{ item.name }}</span>
          <span v-if="item.typeName" class="truncate text-xs opacity-60">[{{ item.typeName }}]</span>
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
import {CmsisDapClient, DapError, MAX_CURVE_SERIES, WsDapTransport, type DapTargetInfo} from '@/api/wsDap'
import {kickDapSession} from '@/api/apiDap'
import {
  formatHex32,
  parseElf32Symbols,
  parseElfVariables,
  type AddressExpr,
  type DisplayFormat,
  type ElfVariablesResult,
  type SampleLeaf,
  isPlausibleTargetAddress,
  type VariableNode,
} from '@/utils/elfSymbols'
import {addressExprIsLive, addressExprRegisters, addressExprText} from '@/utils/dwarfLocation'
import {
  buildFunctionSymbols,
  chooseBreakpointFunction,
  formatBreakpointAddress,
  formatFunctionOption,
  resolveBreakpointWithOffset,
  type FunctionSymbol,
} from '@/utils/elfFunctionSymbols'
import {
  CURVE_LOCAL_MAX_SERIES,
  CurveLocalError,
  getLocalCurveFrames,
  getLocalCurveStatus,
  parseBreakpointAddressInput,
  startLocalCurve,
  stopLocalCurve,
  type CurveLocalTrigger,
  type LocalCurveStatus,
} from '@/api/curveLocal'

const FORMATS: DisplayFormat[] = ['auto', 'u32', 'i32', 'f32']
/**
 * 采样率选项；0 = Auto（自适应目标速率）。
 * 默认 100 Hz；WS-DAP 管线化后实际速率受网络往返限制，Auto 会自动降低目标
 * 采样率以减少“丢弃点数”。
 */
const SAMPLE_RATES = [0, 10, 20, 50, 100]
/** 0 = Auto 模式标识。 */
const SAMPLE_RATE_AUTO = 0
/** 调试采样模式：暂停目标读寄存器/栈帧/指针，采样间隔可选（低采样率）。 */
const DEBUG_INTERVALS = [20, 50, 100, 200]
const DEFAULT_DEBUG_INTERVAL_MS = 50
/** 采样来源：Web DAP（WiFi 往返）或设备本地（ESP32 SWD 闭环，Phase 1）。 */
type SamplingSource = 'web' | 'local'
const SAMPLING_SOURCES: SamplingSource[] = ['web', 'local']
/** 设备本地采样间隔（ms）；固件契约 5..200 ms。 */
const LOCAL_INTERVALS = [5, 10, 20, 50]
const DEFAULT_LOCAL_INTERVAL_MS = 10
/** 本地帧轮询周期（契约要求 20–50 ms）。 */
const LOCAL_POLL_MS = 30
const LOCAL_POLL_MAX_FRAMES = 32
/** 采样管线深度：允许同时在途的 WS-DAP 采样请求数（固件按序回包）。 */
const PIPELINE_DEPTH = 8
const AUTO_MIN_RATE = 10
const AUTO_MAX_RATE = 100
/** Auto 自适应窗口（毫秒）。 */
const AUTO_RATE_WINDOW_MS = 2500
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
  /** 变量显示路径（如 sn_tools.data.as_file / sn_buff[3]） */
  name: string
  /** 去重键：path@address */
  key: string
  typeName: string
  address: number
  size: number
  sample: SampleLeaf
  format: DisplayFormat
  color: string
  visible: boolean
  /** 该曲线开始采样的 sampleSeq：中途新增的曲线不回填旧点。 */
  startSeq: number
  /** 采样方式：live=实时；halt=调试模式（暂停目标后读取） */
  mode: 'live' | 'halt'
  /** 指针链/调试位置表达式（实时两阶段或调试采样时求值） */
  location?: AddressExpr
  /** 值直接在核心寄存器里（DW_OP_reg） */
  register?: number
}

interface CurveNotice {
  type: 'success' | 'info' | 'warning' | 'error'
  title: string
}

type GroupMode = 'none' | 'file' | 'namespace'
const GROUP_OPTIONS: GroupMode[] = ['none', 'file', 'namespace']

interface VariableRow {
  kind: 'variable' | 'group'
  key: string
  /** 变量行使用真实节点；分组行用占位节点（模板按 kind 区分）。 */
  node: VariableNode
  depth: number
  path: string
  hasChildren: boolean
  expanded: boolean
  groupLabel?: string
  groupCount?: number
}

const SAMPLE_RATES_DEFAULT = 100

// ---- 页面状态 -------------------------------------------------------------
const sampleRate = ref(SAMPLE_RATES_DEFAULT)
/** 采样模式：实时（固定地址，高采样率）/ 调试（暂停读取帧/寄存器/指针，低采样率）。 */
const samplingMode = ref<'live' | 'debug'>('live')
const debugIntervalMs = ref(DEFAULT_DEBUG_INTERVAL_MS)
const isDebugMode = computed(() => samplingMode.value === 'debug')
/** 采样来源：web=现有 WebSocket-DAP 路径；local=ESP32 本地 halt→读→resume 闭环。 */
const samplingSource = ref<SamplingSource>('web')
const isLocalSource = computed(() => samplingSource.value === 'local')
const localIntervalMs = ref(DEFAULT_LOCAL_INTERVAL_MS)
/** Phase 2 触发方式：periodic（周期）或 breakpoint（FPB 命中触发）。 */
const localTrigger = ref<CurveLocalTrigger>('periodic')
/** 断点地址输入（十六进制，可带/不带 0x；Thumb bit0 忽略）。 */
const localBreakpointInput = ref('')
/** 从 ELF 解析出的 Flash 函数符号（局部断点选择器数据）。 */
const elfFunctionSymbols = shallowRef<FunctionSymbol[]>([])
/** 函数选择器的当前选中项（地址）；手工改地址时清空。 */
const selectedFunctionAddress = ref<number | null>(null)
/** 可选 Offset（hex/dec），用于选“函数入口之后”的指令地址。 */
const localBreakpointOffsetInput = ref('')
const localHits = ref<number | null>(null)
const localSampled = ref<number | null>(null)
const parsedBreakpointAddress = computed(() => (localTrigger.value === 'breakpoint'
  ? parseBreakpointAddressInput(localBreakpointInput.value)
  : null))
/** 断点地址 + 可选 Offset 的最终 FPB 地址校验结果。 */
const breakpointResolution = computed(() => (isLocalSource.value && localTrigger.value === 'breakpoint'
  ? resolveBreakpointWithOffset(localBreakpointInput.value, localBreakpointOffsetInput.value)
  : null))
const localBreakpointError = computed(() => {
  if (!isLocalSource.value || localTrigger.value !== 'breakpoint') return ''
  if (!localBreakpointInput.value.trim()) return translate('curve.localBreakpointRequired')
  const result = breakpointResolution.value
  if (!result || result.ok) return ''
  if (result.reason === 'offset') return translate('curve.localOffsetInvalid')
  if (result.reason === 'range') return translate('curve.localBreakpointInvalid')
  return translate('curve.localBreakpointInvalid')
})
const localStartDisabled = computed(() => {
  if (seriesList.value.length === 0) return true
  if (!isLocalSource.value) return false
  if (localTrigger.value !== 'breakpoint') return false
  return breakpointResolution.value?.ok !== true
})
/** 本地采样器是否已 start（用于保证 stop/卸载时 POST /api/curve/stop）。 */
const localActive = ref(false)
const localStatusInfo = shallowRef<LocalCurveStatus | null>(null)
const localFramesReceived = ref(0)
const localInvalidCount = ref(0)
const localOverwrittenCount = ref(0)
const localGap = ref(false)
/** Auto 模式下当前实际使用的目标采样率（自动调整，UI 显示）。 */
const autoRate = ref(AUTO_MAX_RATE)
const globalFormat = ref<DisplayFormat>('auto')
const seriesList = ref<CurveSeries[]>([])
const variableSearch = ref('')
/** 默认只看可采样变量；关闭后列出寄存器/栈/被优化掉的命名变量（标记不可采样）。 */
const onlySampleable = ref(true)
const groupBy = ref<GroupMode>('none')
const expandedKeys = ref<Set<string>>(new Set())
/** 变量列表整体重渲染代次：替换 ELF 时强制重建 DOM，避免旧过滤列表的残留行。 */
const variableListEpoch = ref(0)
const manualInput = ref('')
const addressError = ref('')
const elfParsing = ref(false)
const elfInfo = shallowRef<ElfVariablesResult | null>(null)
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
const autoRateDisplay = computed(() => Math.round(autoRate.value))

const canvasRef = ref<HTMLCanvasElement | null>(null)
const chartWrapRef = ref<HTMLDivElement | null>(null)
const hover = ref<{ index: number; x: number; y: number; time: number; entries: Array<{ name: string; color: string; text: string }> } | null>(null)

let client: CmsisDapClient | null = null
let transport: WsDapTransport | null = null
let nextSeriesId = 1
let sampleTimer = 0
let debugTimer = 0
let debugRunId = 0
let debugInFlight = false
// 设备本地采样状态
let localTimer = 0
let localRunId = 0
let localPollInFlight = false
let localPollCounter = 0
let localSince = 0
let localBaseTimestampUs = 0
let localLastPollAt = 0
let localRateTimestamps: number[] = []
let localSnapshot: Array<{item: CurveSeries; expr: AddressExpr; size: number}> = []
let sampleSeq = 0
let sampleEpochMs = 0
let nextSampleAt = 0
let consecutiveFailures = 0
let recentCompletions: number[] = []
/** 采样运行代次：停止/重启后忽略迟到的完成回调。 */
let samplingRunId = 0
/** 已发出、尚未完成的采样请求数。 */
let inflightSamples = 0
// Auto 速率控制窗口统计
let autoWindowStart = 0
let autoWindowBeats = 0
let autoWindowSkipped = 0
let autoWindowCompletions = 0
let autoWindowInflightSum = 0
let autoWindowInflightCount = 0
/** Auto 连续“差/好”窗口计数：避免偶发抖动导致速率被棘轮式压低。 */
let autoBadWindows = 0
let autoGoodWindows = 0
let constantReference: Uint8Array[] | null = null
let constantSamples = 0
let resizeObserver: ResizeObserver | null = null
let drawPending = false
/** 采样统计的 UI 刷新节流：采样本身在 100Hz 完成，但 Vue 响应式/DOM 更新不必
 *  每个点都触发（否则主线程频繁重渲染会造成采样节拍抖动和管线积压）。 */
let statsTimer = 0
let lastStatsAt = 0
let droppedPending = 0

// 环形缓冲：timestamps/slotSeq 按写入顺序共享；每个曲线存 64-bit 原始值的
// 低/高 32-bit（支持 DWARF 的 1/2/4/8 字节 + 位域）。
const timestamps = new Float64Array(MAX_POINTS)
const slotSeq = new Float64Array(MAX_POINTS)
const seriesLo = new Map<number, Uint32Array>()
const seriesHi = new Map<number, Uint32Array>()
let ringHead = 0
let ringCount = 0

const f32Buffer = new Float32Array(1)
const f32U32 = new Uint32Array(f32Buffer.buffer)
const f64Buffer = new Float64Array(1)
const f64U32 = new Uint32Array(f64Buffer.buffer)

// ---- 连接状态 -------------------------------------------------------------
const statusTagType = computed<'primary' | 'success' | 'info' | 'warning' | 'danger'>(() => {
  if (isLocalSource.value) {
    if (sampling.value) return 'success'
    if (statusError.value) return 'danger'
    if (localActive.value) return 'warning'
    return 'info'
  }
  if (connecting.value) return 'warning'
  if (!connected.value) return 'info'
  if (sampling.value) return 'success'
  if (statusError.value) return 'danger'
  if (everSampled.value) return 'warning'
  return 'primary'
})

const statusText = computed(() => {
  if (isLocalSource.value) {
    if (sampling.value) return translate('curve.statusSampling')
    if (statusError.value) return translate('curve.statusError')
    return translate('curve.localStopped')
  }
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
    // bring-up 全部串行完成后再开启采样管线（connect/info/config 保持串行语义）。
    nextClient.setPipelineDepth(PIPELINE_DEPTH)
    transport = pendingTransport
    pendingTransport = null
    client = nextClient
    targetInfo.value = info
    connected.value = true
    kickWarning.value = ''
    resetConstantTracking()
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

async function onResetTarget(): Promise<void> {
  const current = client
  if (!current || resetting.value) return
  resetting.value = true
  try {
    await current.resetAndRun()
    haltedSuspected.value = false
  } catch (error) {
    const message = error instanceof Error ? error.message : 'unknown error'
    globalNotify(translate('curve.connectFailed', {err: message}), 'error')
  } finally {
    resetting.value = false
  }
}

// ---- 采样循环（节拍调度与完成事件解耦，WS-DAP 管线化） -------------------
function currentTargetRate(): number {
  if (sampleRate.value === SAMPLE_RATE_AUTO) return autoRate.value
  return sampleRate.value
}

function resetAutoWindow(now: number): void {
  autoWindowStart = now
  autoWindowBeats = 0
  autoWindowSkipped = 0
  autoWindowCompletions = 0
  autoWindowInflightSum = 0
  autoWindowInflightCount = 0
}

/**
 * Auto 自适应：每个窗口（约 1.5s）评估一次。
 *  - 窗口内出现被跳过的发送节拍 → 目标速率 *0.85（减少丢弃）；
 *  - 否则完成数 ≥ 0.9*目标速率且平均在途数 < depth/2 → *1.10（尝试提高）。
 */
function maybeAdjustAutoRate(now: number): void {
  if (sampleRate.value !== SAMPLE_RATE_AUTO) return
  if (now - autoWindowStart < AUTO_RATE_WINDOW_MS) return
  let next = autoRate.value
  const skipRatio = autoWindowBeats > 0 ? autoWindowSkipped / autoWindowBeats : 0
  const averageInflight = autoWindowInflightCount > 0
      ? autoWindowInflightSum / autoWindowInflightCount
      : 0
  if (skipRatio > 0.03) {
    autoBadWindows += 1
    autoGoodWindows = 0
    // 连续两个窗口管线都吃紧才降速，避免单个抖动窗口造成“棘轮降速”。
    if (autoBadWindows >= 2) {
      next = Math.max(AUTO_MIN_RATE, autoRate.value * 0.90)
      autoBadWindows = 0
    }
  } else {
    autoGoodWindows += 1
    autoBadWindows = 0
    const canRise = skipRatio <= 0.005 &&
        autoWindowCompletions >= 0.9 * autoRate.value &&
        averageInflight < PIPELINE_DEPTH * 0.5
    if (canRise && autoGoodWindows >= 3) {
      // 连续干净窗口：小幅升速（优先把丢弃点压到接近 0）。
      next = Math.min(AUTO_MAX_RATE, autoRate.value * 1.05)
    }
  }
  resetAutoWindow(now)
  if (Math.abs(next - autoRate.value) > 0.01) {
    autoRate.value = next
    // 速率变化后按新周期重新对齐下一个节拍。
    if (sampleTimer) {
      window.clearTimeout(sampleTimer)
      sampleTimer = 0
    }
    nextSampleAt = performance.now()
    scheduleBeat(samplingRunId, 1000 / Math.max(currentTargetRate(), 1))
  }
}

/** 以约 8 Hz 刷新采样统计显示，避免每个样本都触发响应式重渲染。 */
function scheduleStatsFlush(): void {
  if (statsTimer) return
  const now = performance.now()
  const remain = Math.max(0, 120 - (now - lastStatsAt))
  statsTimer = window.setTimeout(() => {
    statsTimer = 0
    flushStats()
  }, remain)
}

function flushStats(): void {
  lastStatsAt = performance.now()
  sampleCount.value = ringCount
  if (isLocalSource.value) {
    effectiveRate.value = localRateTimestamps.length
    return
  }
  effectiveRate.value = recentCompletions.length
  if (droppedPending > 0) {
    droppedCount.value += droppedPending
    droppedPending = 0
  }
}

/** 地址表达式求值时的批量指针读取器：同一微任务里的 readWord 合并为一次 sampleRaw。 */
interface WordWaiter {
  resolve: (value: number) => void
  reject: (error: unknown) => void
}

class WordPrefetcher {
  /**
   * 同一地址可能被多个待求值表达式同时请求（例如多个成员都基于同一个指针
   * `*[r13+4]`）。必须为每个地址保存一个 waiter 列表；旧实现用 Map<地址, waiter>
   * 后写覆盖先写，会让前一个 Promise 永远不 resolve，调试采样首拍即永久挂起
   * （表现为 Samples 0 / Failures 0 / 0 Hz）。
   */
  private pending = new Map<number, WordWaiter[]>()
  private scheduled = false

  constructor(private readonly target: CmsisDapClient) {}

  read(address: number): Promise<number> {
    return new Promise<number>((resolve, reject) => {
      const key = address >>> 0
      // 失效的栈帧/指针值可能落在 0/PPB 等不可访问地址；直接返回 0，
      // 不要向 DAP 发非法读（否则 ACK=FAULT，整个调试采样链会被打断）。
      if (!isPlausibleTargetAddress(key)) {
        resolve(0)
        return
      }
      const waiters = this.pending.get(key)
      if (waiters) waiters.push({resolve, reject})
      else this.pending.set(key, [{resolve, reject}])
      if (!this.scheduled) {
        this.scheduled = true
        queueMicrotask(() => { void this.flush() })
      }
    })
  }

  private async flush(): Promise<void> {
    this.scheduled = false
    const batch = [...this.pending.entries()]
    this.pending = new Map()
    if (batch.length === 0) return
    try {
      const raws = await this.target.sampleRaw(batch.map(([address]) => ({address, size: 4})))
      batch.forEach(([, waiters], index) => {
        const raw = raws[index] ?? new Uint8Array(4)
        let value = 0
        for (let byte = 0; byte < 4; byte++) value |= (raw[byte] ?? 0) << (byte * 8)
        const word = value >>> 0
        for (const waiter of waiters) waiter.resolve(word)
      })
    } catch (error) {
      for (const [, waiters] of batch) for (const waiter of waiters) waiter.reject(error)
    }
    if (this.pending.size > 0) {
      this.scheduled = true
      queueMicrotask(() => { void this.flush() })
    }
  }
}

/** 异步求值地址表达式（指针解引用通过 WordPrefetcher 批量读取）。 */
async function resolveAddressAsync(
    expr: AddressExpr,
    registers: ReadonlyMap<number, number>,
    prefetcher: WordPrefetcher,
): Promise<number> {
  switch (expr.kind) {
    case 'addr':
      return expr.address >>> 0
    case 'reg': {
      const value = registers.get(expr.reg)
      if (value === undefined) throw new Error(`missing core register r${expr.reg}`)
      return (value + expr.offset) >>> 0
    }
    case 'cfa': {
      const value = registers.get(13)
      if (value === undefined) throw new Error('missing core register SP (r13) for CFA')
      return (value + expr.offset) >>> 0
    }
    case 'add':
      return ((await resolveAddressAsync(expr.inner, registers, prefetcher)) + expr.offset) >>> 0
    case 'deref': {
      const pointerAddress = await resolveAddressAsync(expr.inner, registers, prefetcher)
      return ((await prefetcher.read(pointerAddress)) + expr.offset) >>> 0
    }
    default:
      throw new Error('unsupported DWARF address expression')
  }
}

/**
 * 实时采样读取：普通叶子直接读固定地址；指针链成员需要先读指针（两阶段），
 * 仍不需要暂停目标，因此实时模式也可采样固定地址指针的成员。
 */
async function fetchLiveRawValues(snapshot: readonly CurveSeries[]): Promise<Uint8Array[]> {
  const current = client
  if (!current) throw new Error('not connected')
  if (!snapshot.some((item) => item.location)) {
    return current.sampleRaw(snapshot.map((item) => ({address: item.sample.address, size: item.sample.size})))
  }
  const prefetcher = new WordPrefetcher(current)
  const emptyRegisters = new Map<number, number>()
  const addresses = await Promise.all(snapshot.map((item) => (
    item.location
      ? resolveAddressAsync(item.location, emptyRegisters, prefetcher)
      : Promise.resolve(item.sample.address >>> 0)
  )))
  const results = snapshot.map((item, index) => ({
    value: new Uint8Array(Math.max(1, Math.min(8, item.sample.size))),
    read: {address: addresses[index], size: item.sample.size},
    valid: isPlausibleTargetAddress(addresses[index]),
  }))
  const reads = results.filter((item) => item.valid).map((item) => item.read)
  if (reads.length > 0) {
    const raws = await current.sampleRaw(reads)
    let slot = 0
    for (const item of results) {
      if (item.valid) item.value = raws[slot++] ?? item.value
    }
  }
  return results.map((item) => item.value)
}

/**
 * 调试采样：haltTarget → 读需要的一次性核心寄存器 → 批量指针/内存读取 → resume。
 * 每次采样独立 halt/resume，约 5–20 Hz；出错时立即停止采样并在 finally 恢复目标。
 */
function isTransientDebugError(error: unknown): boolean {
  if (error instanceof DapError) {
    // ack/target/protocol 通常来自“读到了失效的栈帧/指针地址”或目标暂时
    // 未就绪；不应立刻停止采样。ws-* 表示连接已经坏了，必须停止/重连。
    return error.code === 'ack' || error.code === 'target' || error.code === 'protocol'
  }
  return false
}

function noteDebugSampleSuccess(): void {
  consecutiveFailures = 0
  const completedAt = performance.now()
  recentCompletions.push(completedAt)
  const cutoff = completedAt - 1000
  while (recentCompletions.length > 0 && recentCompletions[0] < cutoff) recentCompletions.shift()
  scheduleStatsFlush()
  scheduleDraw()
}

/**
 * 调试采样：haltTarget → 读需要的一次性核心寄存器 → 批量指针/内存读取 → resume。
 * 纯实时变量（固定地址/固定指针链）在调试模式下不 halt，直接走实时路径，
 * 避免为了全局变量也付出暂停目标的代价。
 *
 * 失效地址（栈帧未命中/指针垃圾）返回 0 并跳过，不再让单个坏读取终止整场采样；
 * 偶发 DAP ACK=FAULT 计入 Failures 后继续下一拍，连续 5 次才停止。
 */
async function debugSampleTick(runId: number): Promise<void> {
  debugTimer = 0
  if (!sampling.value || runId !== debugRunId || !client) return
  const current = client
  const begin = performance.now()
  let halted = false
  let snapshot: CurveSeries[] = []
  debugInFlight = true
  try {
    snapshot = seriesList.value.slice()
    if (snapshot.length === 0) {
      stopSampling()
      return
    }
    const needsHalt = snapshot.some((item) => item.register !== undefined || (item.location && !addressExprIsLive(item.location)))
    if (!needsHalt) {
      const rawValues = await fetchLiveRawValues(snapshot)
      if (!sampling.value || runId !== debugRunId) return
      pushSample((begin - sampleEpochMs) / 1000, rawValues, snapshot)
      noteDebugSampleSuccess()
    } else {
      halted = await current.haltTarget(false)
      if (!halted) throw new DapError('target', 'target halt failed')
      const registerIds = new Set<number>()
      for (const item of snapshot) {
        if (item.register !== undefined) registerIds.add(item.register)
        if (item.location) {
          for (const reg of addressExprRegisters(item.location)) registerIds.add(reg)
        }
      }
      const orderedRegs = [...registerIds].sort((left, right) => left - right)
      const registers = new Map<number, number>()
      if (orderedRegs.length > 0) {
        const values = await current.readCoreRegs(orderedRegs)
        orderedRegs.forEach((reg, index) => registers.set(reg, values[index] ?? 0))
      }
      const prefetcher = new WordPrefetcher(current)
      const addresses = await Promise.all(snapshot.map((item) => (
        item.location
          ? resolveAddressAsync(item.location, registers, prefetcher)
          : Promise.resolve(item.sample.address >>> 0)
      )))
      const values: Uint8Array[] = new Array(snapshot.length)
      const memoryReads: Array<{address: number; size: number}> = []
      const memoryIndices: number[] = []
      for (let index = 0; index < snapshot.length; index++) {
        const item = snapshot[index]
        if (item.register !== undefined) {
          const size = Math.max(1, Math.min(8, item.sample.size))
          const bytes = new Uint8Array(size)
          let value = registers.get(item.register) ?? 0
          for (let byte = 0; byte < size; byte++) {
            bytes[byte] = value & 0xff
            value = Math.floor(value / 256)
          }
          values[index] = bytes
          continue
        }
        const address = addresses[index]
        if (!isPlausibleTargetAddress(address)) {
          values[index] = new Uint8Array(Math.max(1, Math.min(8, item.sample.size)))
          continue
        }
        memoryReads.push({address, size: item.sample.size})
        memoryIndices.push(index)
      }
      if (memoryReads.length > 0) {
        const raws = await current.sampleRaw(memoryReads)
        memoryIndices.forEach((seriesIndex, slot) => {
          values[seriesIndex] = raws[slot] ?? new Uint8Array(1)
        })
      }
      if (!sampling.value || runId !== debugRunId) return
      pushSample((begin - sampleEpochMs) / 1000, values, snapshot)
      noteDebugSampleSuccess()
    }
  } catch (error) {
    if (!sampling.value || runId !== debugRunId) return
    failedCount.value += 1
    consecutiveFailures += 1
    const message = error instanceof Error ? error.message : 'unknown error'
    // 失效地址会让 AHB-AP 进入 sticky FAULT；不清除的话后续 halt/读寄存器
    // 全部失败，只能刷新页面。这里先 ABORT+SELECT+CSW 恢复再继续下一拍。
    if (error instanceof DapError && error.code === 'ack') {
      try {
        await current.recoverFromFault()
      } catch {
        /* 连接已断时由 transport 关闭流程处理 */
      }
    }
    if (consecutiveFailures >= 5 || !isTransientDebugError(error) || transport === null || transport.isClosed) {
      stopSampling()
      statusError.value = true
      if (transport === null || transport.isClosed) {
        notice.value = {type: 'error', title: translate('curve.reconnectHint')}
      } else {
        notice.value = {type: 'error', title: translate('curve.debugSampleFailed', {err: message})}
      }
    }
    scheduleDraw()
  } finally {
    debugInFlight = false
    if (halted) {
      try {
        await current.resumeTarget(false)
      } catch {
        try {
          await current.resumeTarget(false)
        } catch {
          // 恢复失败时连接层会报错；下一拍重试。
        }
      }
    }
  }
  if (sampling.value && runId === debugRunId) {
    const elapsed = performance.now() - begin
    if (elapsed > debugIntervalMs.value * 1.5) droppedPending += 1
    // 按“目标周期”而不是“tick 结束后再等一个周期”调度；否则实际周期是
    // tick 耗时 + interval（WiFi 下 tick 可达 40–100 ms，6 Hz 的主要来源）。
    const delay = Math.max(0, debugIntervalMs.value - elapsed)
    debugTimer = window.setTimeout(() => { void debugSampleTick(runId) }, delay)
    scheduleStatsFlush()
  }
}

// ---- 设备本地采样（ESP32 halt→read→resume 闭环，Web 只轮询批量帧） ----
function localErrorText(error: unknown): string {
  if (error instanceof CurveLocalError) return error.httpStatus > 0 ? `${error.message} (HTTP ${error.httpStatus})` : error.message
  return error instanceof Error ? error.message : 'unknown error'
}

/** 按 series 顺序把一帧 payload 拆成每路原始字节；长度不符时返回 null（跳过该帧）。 */
function splitLocalPayload(payload: Uint8Array): Uint8Array[] | null {
  const expected = localSnapshot.reduce((sum, entry) => sum + entry.size, 0)
  if (payload.length < expected) return null
  const values: Uint8Array[] = []
  let offset = 0
  for (const entry of localSnapshot) {
    values.push(payload.slice(offset, offset + entry.size))
    offset += entry.size
  }
  return values
}

function trimLocalRateWindow(latestSeconds: number): void {
  const cutoff = latestSeconds - 1
  while (localRateTimestamps.length > 0 && localRateTimestamps[0] < cutoff) localRateTimestamps.shift()
}

async function refreshLocalStatus(): Promise<void> {
  const status = await getLocalCurveStatus()
  if (!sampling.value) return
  localStatusInfo.value = status
  localInvalidCount.value = status.invalid
  localOverwrittenCount.value = status.overwritten
  localHits.value = status.hits ?? null
  localSampled.value = status.sampled ?? null
  if (status.running !== true && status.error > 0) {
    localGap.value = true
  }
}

async function postLocalStop(): Promise<void> {
  try {
    await stopLocalCurve()
    // stop 成功后刷新一次状态，页面显示设备已恢复/采样器已停止。
    try {
      localStatusInfo.value = await getLocalCurveStatus()
    } catch {
      /* 状态刷新失败不影响停止结果 */
    }
  } catch (error) {
    notice.value = {type: 'error', title: translate('curve.localStopFailed', {err: localErrorText(error)})}
  }
}

/** 停止设备本地采样：本地先置 stopped，再 best-effort POST /api/curve/stop。 */
function stopLocalSampling(): void {
  const wasActive = localActive.value
  sampling.value = false
  localRunId += 1
  localActive.value = false
  if (localTimer) {
    window.clearTimeout(localTimer)
    localTimer = 0
  }
  if (wasActive) void postLocalStop()
  flushStats()
  scheduleDraw()
}

async function startLocalSampling(): Promise<void> {
  if (sampling.value || seriesList.value.length === 0) return
  // 设备本地与 WebSocket-DAP 互斥：已连接时先停止/断开，避免 DAP 会话冲突。
  if (connected.value || connecting.value) {
    await onDisconnect()
  }
  const snapshot = seriesList.value.slice()
  const specs: Array<{id: number; size: number; expr: AddressExpr}> = []
  const entries: Array<{item: CurveSeries; expr: AddressExpr; size: number}> = []
  for (let index = 0; index < snapshot.length; index++) {
    const item = snapshot[index]
    const expr = item.location
      ?? (item.sample.address ? {kind: 'addr' as const, address: item.sample.address >>> 0} : null)
    if (!expr) {
      notice.value = {type: 'error', title: translate('curve.localRegisterUnsupported')}
      return
    }
    entries.push({item, expr, size: Math.max(1, Math.min(8, item.sample.size))})
    specs.push({id: index, size: Math.max(1, Math.min(8, item.sample.size)), expr})
  }
  statusError.value = false
  notice.value = null
  // 纯固定地址/固定指针表达式不需要暂停核心；只有 reg/cfa（栈帧）才必须 halt。
  // 这样全局变量/指针成员可走 5 ms 本地轮询（~200 Hz）且不打断目标运行。
  const trigger = localTrigger.value
  const needsHalt = entries.some((entry) => addressExprRegisters(entry.expr).size > 0)
  let breakpointAddress: number | undefined
  if (trigger === 'breakpoint') {
    const resolution = breakpointResolution.value
    if (!resolution || !resolution.ok) {
      notice.value = {type: 'error', title: translate('curve.localBreakpointInvalid')}
      return
    }
    breakpointAddress = resolution.address
  }
  if (needsHalt && localIntervalMs.value < 5) {
    notice.value = {type: 'error', title: translate('curve.localHaltIntervalTooFast')}
    return
  }
  try {
    await startLocalCurve({
      intervalUs: localIntervalMs.value * 1000,
      trigger,
      breakpointAddress,
      // breakpoint 命中时核心已暂停，halt 字段无意义；只在周期模式下发。
      halt: trigger === 'periodic' ? needsHalt : undefined,
      series: specs,
    })
  } catch (error) {
    statusError.value = true
    notice.value = {type: 'error', title: translate('curve.localStartFailed', {err: localErrorText(error)})}
    return
  }
  // 设备时间戳与既有 ring 时间轴不同源：清空旧数据，按帧时间戳重新累积。
  clearData()
  failedCount.value = 0
  localSnapshot = entries
  localSince = 0
  localBaseTimestampUs = 0
  localPollCounter = 0
  localPollInFlight = false
  localRateTimestamps = []
  localFramesReceived.value = 0
  localInvalidCount.value = 0
  localOverwrittenCount.value = 0
  localHits.value = null
  localSampled.value = null
  localGap.value = false
  localStatusInfo.value = null
  localActive.value = true
  sampling.value = true
  everSampled.value = true
  localLastPollAt = performance.now()
  const runId = ++localRunId
  scheduleLocalPoll(runId, 0)
  scheduleDraw()
}

function scheduleLocalPoll(runId: number, delay = LOCAL_POLL_MS): void {
  if (!localActive.value || runId !== localRunId) return
  localTimer = window.setTimeout(() => { void localPollTick(runId) }, Math.max(0, delay))
}

/** 轮询 /api/curve/frames（20–50ms），把 ESP32 微秒时间戳写入现有 ring。 */
async function localPollTick(runId: number): Promise<void> {
  localTimer = 0
  if (!sampling.value || runId !== localRunId || !localActive.value) return
  if (localPollInFlight) {
    scheduleLocalPoll(runId)
    return
  }
  localPollInFlight = true
  try {
    const page = await getLocalCurveFrames(localSince, LOCAL_POLL_MAX_FRAMES)
    if (!sampling.value || runId !== localRunId) return
    if (page.frames.length > 0) {
      if (page.gap || (localSince > 0 && page.frames[0].seq > localSince + 1)) localGap.value = true
      const seriesSnapshot = localSnapshot.map((entry) => entry.item)
      let applied = 0
      for (const frame of page.frames) {
        if (frame.seq <= localSince) continue
        if (localBaseTimestampUs === 0) localBaseTimestampUs = frame.timestampUs
        const rawValues = splitLocalPayload(frame.payload)
        if (!rawValues) {
          localInvalidCount.value += 1
          continue
        }
        const seconds = (frame.timestampUs - localBaseTimestampUs) / 1e6
        pushSample(seconds, rawValues, seriesSnapshot)
        localRateTimestamps.push(seconds)
        trimLocalRateWindow(seconds)
        localSince = frame.seq
        applied += 1
      }
      if (applied > 0) {
        localFramesReceived.value += applied
        effectiveRate.value = localRateTimestamps.length
        scheduleDraw()
      }
    }
    localPollCounter += 1
    if (localPollCounter % 10 === 0 || localStatusInfo.value === null) {
      await refreshLocalStatus()
    }
    localLastPollAt = performance.now()
  } catch (error) {
    if (!sampling.value || runId !== localRunId) return
    notice.value = {type: 'error', title: translate('curve.localPollFailed', {err: localErrorText(error)})}
    stopLocalSampling()
    statusError.value = true
    return
  } finally {
    localPollInFlight = false
  }
  if (sampling.value && runId === localRunId) scheduleLocalPoll(runId)
}

/** 切换采样来源：本地方案不建立 WS-DAP；从 Web 切到本地时先断开 DAP。 */
async function onSamplingSourceChange(): Promise<void> {
  if (sampling.value) return
  statusError.value = false
  everSampled.value = false
  notice.value = null
  if (isLocalSource.value) {
    if (connected.value || connecting.value) {
      await onDisconnect()
    }
    const removable = seriesList.value.filter((item) => item.register !== undefined
      || (!item.location && !item.sample.address))
    for (const item of removable) removeSeries(item.id)
    if (removable.length > 0) {
      globalNotify(translate('curve.localSeriesRemoved', {count: removable.length}), 'info')
    }
    localStatusInfo.value = null
    localFramesReceived.value = 0
    localInvalidCount.value = 0
    localOverwrittenCount.value = 0
    localHits.value = null
    localSampled.value = null
    localGap.value = false
  }
  scheduleDraw()
}

/** 函数选择器：选中后自动填 0x%08X（bit0 清 0），地址仍可手工改。 */
function onFunctionSelected(address: number | null | undefined): void {
  if (address === null || address === undefined) return
  const symbol = chooseBreakpointFunction(elfFunctionSymbols.value, address)
  const normalized = symbol ? symbol.address : (Number(address) & 0xfffffffe) >>> 0
  localBreakpointInput.value = formatBreakpointAddress(normalized)
}

/** 手工修改地址时取消函数下拉的选中态（避免显示与实际地址不一致）。 */
function onBreakpointAddressInput(): void {
  if (selectedFunctionAddress.value === null) return
  const parsed = parseBreakpointAddressInput(localBreakpointInput.value)
  if (parsed === null || parsed !== selectedFunctionAddress.value) selectedFunctionAddress.value = null
}

/** 切换 Trigger 时清空上一次断点统计/状态显示（周期模式完全兼容）。 */
function onLocalTriggerChange(): void {
  localHits.value = null
  localSampled.value = null
  localStatusInfo.value = null
  scheduleDraw()
}

function startSampling(): void {
  if (isLocalSource.value) {
    void startLocalSampling()
    return
  }
  if (!connected.value || !client || seriesList.value.length === 0) return
  if (sampling.value) return
  statusError.value = false
  notice.value = null
  failedCount.value = 0
  consecutiveFailures = 0
  recentCompletions = []
  effectiveRate.value = 0
  inflightSamples = 0
  if (sampleRate.value === SAMPLE_RATE_AUTO) {
    autoRate.value = AUTO_MAX_RATE
  }
  resetConstantTracking()
  const lastT = ringCount > 0 ? timestamps[(ringHead - 1 + MAX_POINTS) % MAX_POINTS] : 0
  sampleEpochMs = performance.now() - lastT * 1000
  nextSampleAt = performance.now()
  autoBadWindows = 0
  autoGoodWindows = 0
  resetAutoWindow(performance.now())
  sampling.value = true
  everSampled.value = true
  if (samplingMode.value === 'debug') {
    debugRunId += 1
    samplingRunId = debugRunId
    debugTimer = 0
    void debugSampleTick(debugRunId)
    return
  }
  debugRunId += 1
  const runId = ++samplingRunId
  scheduleBeat(runId, 0)
}

function stopWebSampling(): void {
  sampling.value = false
  samplingRunId += 1
  debugRunId += 1
  if (sampleTimer) {
    window.clearTimeout(sampleTimer)
    sampleTimer = 0
  }
  if (debugTimer) {
    window.clearTimeout(debugTimer)
    debugTimer = 0
  }
  // 调试模式停止时确保目标恢复运行（正在进行的 tick 会在 finally 里恢复）。
  const current = client
  if (isDebugMode.value && !debugInFlight && current && connected.value) {
    void current.resumeTarget().catch(() => { /* ignore */ })
  }
  inflightSamples = 0
  flushStats()
}

/** 统一停止入口：本地采样 POST /api/curve/stop，Web-DAP 保持原有停止逻辑。 */
function stopSampling(): void {
  if (isLocalSource.value || localActive.value) {
    stopLocalSampling()
    return
  }
  stopWebSampling()
}

/** 切换采样模式：实时模式不能采样依赖寄存器的调试帧变量，切回时移除这些曲线。 */
function onSamplingModeChange(): void {
  if (sampling.value) return
  if (samplingMode.value === 'live') {
    const removable = seriesList.value.filter((item) => !!item.location && !addressExprIsLive(item.location))
    for (const item of removable) removeSeries(item.id)
    if (removable.length > 0) {
      globalNotify(translate('curve.haltSeriesRemoved', {count: removable.length}), 'info')
    }
  }
  scheduleDraw()
}

function scheduleBeat(runId: number, delay: number): void {
  if (!sampling.value || runId !== samplingRunId) return
  sampleTimer = window.setTimeout(() => samplingBeat(runId), Math.max(0, delay))
}

/** 固定节拍：先排下一拍，再视管线容量决定“发起采样”还是“跳过并计入 dropped”。 */
function samplingBeat(runId: number): void {
  sampleTimer = 0
  if (!sampling.value || runId !== samplingRunId || !client) return
  const period = 1000 / Math.max(currentTargetRate(), 1)
  const now = performance.now()
  nextSampleAt += period
  if (nextSampleAt < now) {
    // 浏览器定时器/主线程抖动会让节拍迟到；迟到只是采样时刻轻微漂移，本拍仍会
    // 发一个采样请求，没有“丢样本”。因此这里只重新对齐相位、不计 dropped。
    // dropped 只统计“管线已满、主动跳过的采样节拍”（真正的采样点缺失）。
    nextSampleAt = now + period
  }
  scheduleBeat(runId, nextSampleAt - now)
  autoWindowBeats += 1

  if (inflightSamples >= PIPELINE_DEPTH) {
    // 管线已满：跳过这一拍（没有数据丢失，只是少发一个采样请求）。
    droppedPending += 1
    autoWindowSkipped += 1
    scheduleStatsFlush()
    maybeAdjustAutoRate(now)
    return
  }

  const snapshot = seriesList.value.slice()
  const begin = now
  inflightSamples += 1
  autoWindowInflightSum += inflightSamples
  autoWindowInflightCount += 1
  fetchLiveRawValues(snapshot).then((rawValues) => {
    if (!sampling.value || runId !== samplingRunId) return
    inflightSamples = Math.max(0, inflightSamples - 1)
    // 时间戳使用“发起请求”的时刻（管线化后完成时刻会晚于采样时刻）。
    const date = (begin - sampleEpochMs) / 1000
    pushSample(date, rawValues, snapshot)
    trackConstantValues(rawValues)
    consecutiveFailures = 0
    const completedAt = performance.now()
    recentCompletions.push(completedAt)
    const cutoff = completedAt - 1000
    while (recentCompletions.length > 0 && recentCompletions[0] < cutoff) recentCompletions.shift()
    scheduleStatsFlush()
    autoWindowCompletions += 1
    maybeAdjustAutoRate(completedAt)
    scheduleDraw()
  }).catch((error) => {
    if (!sampling.value || runId !== samplingRunId) return
    inflightSamples = Math.max(0, inflightSamples - 1)
    handleSampleError(error)
  })
}

function handleSampleError(error: unknown): void {
  failedCount.value += 1
  consecutiveFailures += 1
  if (consecutiveFailures < 3) return
  stopSampling()
  statusError.value = true
  const message = error instanceof Error ? error.message : 'unknown error'
  if (transport === null || transport.isClosed) {
    notice.value = {type: 'error', title: translate('curve.reconnectHint')}
  } else {
    notice.value = {type: 'error', title: translate('curve.connectFailed', {err: message})}
  }
  scheduleDraw()
}

/** 把 DWARF 叶子读到的字节转换成 64-bit 原始值（含位域抽取）。 */
function extractSampleBits(bytes: Uint8Array, leaf: SampleLeaf): {lo: number; hi: number} {
  if (leaf.bitSize > 0) {
    let value = 0n
    for (let i = bytes.length - 1; i >= 0; i--) {
      value = (value << 8n) | BigInt(bytes[i] ?? 0)
    }
    const width = Math.min(leaf.bitSize, 64)
    value = (value >> BigInt(leaf.bitOffset)) & ((1n << BigInt(width)) - 1n)
    return {
      lo: Number(value & 0xffffffffn) >>> 0,
      hi: Number((value >> 32n) & 0xffffffffn) >>> 0,
    }
  }
  let lo = 0
  let hi = 0
  for (let i = 0; i < bytes.length && i < 8; i++) {
    if (i < 4) lo |= (bytes[i] ?? 0) << (i * 8)
    else hi |= (bytes[i] ?? 0) << ((i - 4) * 8)
  }
  return {lo: lo >>> 0, hi: hi >>> 0}
}

function pushSample(time: number, rawValues: Uint8Array[], snapshot: readonly CurveSeries[]): void {
  const slot = ringHead
  timestamps[slot] = time
  slotSeq[slot] = sampleSeq
  for (let i = 0; i < snapshot.length; i++) {
    const item = snapshot[i]
    const loBuffer = seriesLo.get(item.id)
    const hiBuffer = seriesHi.get(item.id)
    if (!loBuffer || !hiBuffer) continue
    const bits = extractSampleBits(rawValues[i] ?? new Uint8Array(item.sample.size), item.sample)
    loBuffer[slot] = bits.lo
    hiBuffer[slot] = bits.hi
  }
  ringHead = (ringHead + 1) % MAX_POINTS
  if (ringCount < MAX_POINTS) ringCount += 1
  sampleSeq += 1
  scheduleStatsFlush()
}

function clearData(): void {
  ringHead = 0
  ringCount = 0
  sampleSeq = 0
  droppedPending = 0
  if (statsTimer) { window.clearTimeout(statsTimer); statsTimer = 0 }
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

function resetConstantTracking(): void {
  constantReference = null
  constantSamples = 0
  haltedSuspected.value = false
}

/** 目标 halt 时所有变量值恒定（真机接管后常见 DHCSR.S_HALT）。 */
function trackConstantValues(rawValues: Uint8Array[]): void {
  if (constantReference === null || constantReference.length !== rawValues.length) {
    constantReference = rawValues.map((bytes) => bytes.slice())
    constantSamples = 0
    return
  }
  let same = true
  for (let i = 0; i < rawValues.length; i++) {
    const left = rawValues[i]
    const right = constantReference[i]
    if (!right || left.length !== right.length) {
      same = false
      break
    }
    for (let j = 0; j < left.length; j++) {
      if (left[j] !== right[j]) {
        same = false
        break
      }
    }
    if (!same) break
  }
  if (same) {
    constantSamples += 1
    if (constantSamples >= 25) haltedSuspected.value = true
  } else {
    constantReference = rawValues.map((bytes) => bytes.slice())
    constantSamples = 0
    haltedSuspected.value = false
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

// ---- 变量树 / 曲线管理 ----------------------------------------------------
function nodeKey(path: string, node: VariableNode): string {
  return `${path}@${node.address.toString(16)}`
}

function pathOf(depth: number, parentPath: string, node: VariableNode): string {
  return depth === 0 ? node.name : parentPath + node.name
}

function nodeMatches(node: VariableNode, path: string, query: string): boolean {
  const needle = query.toLowerCase()
  const fields = [
    path,
    node.name,
    node.shortName,
    node.qualifiedName,
    node.mangledName,
    node.typeName,
    node.relativeFile,
    node.file,
    node.sample && node.sample.address ? formatHex32(node.sample.address) : '',
    node.sample?.register !== undefined ? `r${node.sample.register}` : '',
    node.location ? addressExprText(node.location) : '',
  ]
  return fields.some((value) => !!value && String(value).toLowerCase().includes(needle))
}

function groupLabel(mode: GroupMode): string {
  switch (mode) {
    case 'file':
      return translate('curve.groupFile')
    case 'namespace':
      return translate('curve.groupNamespace')
    default:
      return translate('curve.groupNone')
  }
}

function groupKeyOf(node: VariableNode, mode: GroupMode): string {
  if (mode === 'file') {
    return node.relativeFile ?? node.file ?? '-'
  }
  if (mode === 'namespace') {
    const qualified = node.qualifiedName ?? node.name
    const index = qualified.lastIndexOf('::')
    return index > 0 ? qualified.slice(0, index) : '-'
  }
  return ''
}

function nodeMatchesDeep(node: VariableNode, path: string, query: string): boolean {
  if (nodeMatches(node, path, query)) return true
  if (!node.children) return false
  return node.children.some((child) => nodeMatchesDeep(child, path + child.name, query))
}

/** 当前采样模式下该节点是否可选（实时：固定地址/无寄存器指针；调试：含帧/寄存器位置）。 */
function nodeSelectable(node: VariableNode): boolean {
  if (isLocalSource.value) {
    return !!localExprForNode(node)
  }
  if (samplingMode.value === 'debug') {
    return !!node.sampleable && (!!node.sample || !!node.location)
  }
  return !!node.sample && (node.sample.address !== 0 || !!node.liveSamplable)
}

/** 调试定位表达式/寄存器的人读文本（地址列 tooltip、搜索）。 */
function nodeLocationText(node: VariableNode): string {
  const expr = node.location ?? node.sample?.location
  if (expr) return addressExprText(expr)
  if (node.sample?.register !== undefined) return `r${node.sample.register}`
  return ''
}

/** 设备本地采样使用的 AddressExpr：优先调试/指针位置，其次固定地址。 */
function localExprForNode(node: VariableNode): AddressExpr | null {
  const location = node.sample?.location ?? node.location
  if (location) return location
  if (node.sample?.register !== undefined) return null
  const address = node.sample?.address ?? 0
  if (node.sample && address !== 0) return {kind: 'addr', address: address >>> 0}
  return null
}

/** 寄存器值变量（DW_OP_reg）暂不支持设备本地采样。 */
function isLocalRegisterValue(node: VariableNode): boolean {
  return node.sample?.register !== undefined
}

const variableRows = computed<VariableRow[]>(() => {
  const info = elfInfo.value
  if (!info) return []
  const query = variableSearch.value.trim()
  const rows: VariableRow[] = []
  const visit = (node: VariableNode, depth: number, parentPath: string, bucket: VariableRow[]) => {
    const path = depth === 0 ? node.name : parentPath + node.name
    if (query && !nodeMatchesDeep(node, path, query)) return
    const key = nodeKey(path, node)
    const hasChildren = !!node.children && node.children.length > 0
    const expanded = hasChildren && (query !== '' || expandedKeys.value.has(key))
    bucket.push({kind: 'variable', key, node, depth, path, hasChildren, expanded})
    if (hasChildren && expanded) {
      for (const child of node.children ?? []) visit(child, depth + 1, path, bucket)
    }
  }

  // 默认只列当前模式下可采样的变量；关闭过滤后把“有名字但无位置”的变量也列出（标记不可采样）。
  const base = info.variables.filter((node) => {
    if (!onlySampleable.value) return true
    if (isLocalSource.value) return !!localExprForNode(node)
    return samplingMode.value === 'debug' ? node.sampleable : node.liveSamplable
  })
  if (groupBy.value === 'none') {
    for (const node of base) visit(node, 0, '', rows)
    return rows
  }
  const groups = new Map<string, VariableNode[]>()
  for (const node of base) {
    const label = groupKeyOf(node, groupBy.value)
    const bucket = groups.get(label)
    if (bucket) bucket.push(node)
    else groups.set(label, [node])
  }
  const labels = [...groups.keys()].sort((left, right) => left.localeCompare(right))
  for (const label of labels) {
    const bucket = groups.get(label) ?? []
    rows.push({
      kind: 'group',
      key: `group:${label}`,
      node: {name: label, typeName: '', address: 0, size: 0},
      depth: 0,
      path: label,
      hasChildren: false,
      expanded: false,
      groupLabel: label,
      groupCount: bucket.length,
    })
    for (const node of bucket) visit(node, 0, '', rows)
  }
  return rows
})

const visibleVariableCount = computed(() => variableRows.value.filter((row) => row.kind === 'variable').length)
const unsampleableCount = computed(() => {
  const info = elfInfo.value
  if (!info) return 0
  return info.variables.filter((node) => !node.sampleable).length
})

function isRowSelected(row: VariableRow): boolean {
  if (row.kind !== 'variable') return false
  return seriesList.value.some((item) => item.key === row.key)
}

function toggleExpand(row: VariableRow): void {
  if (row.kind !== 'variable') return
  const next = new Set(expandedKeys.value)
  if (next.has(row.key)) next.delete(row.key)
  else next.add(row.key)
  expandedKeys.value = next
}

function collectExpandable(node: VariableNode, path: string, out: string[]): void {
  if (!node.children || node.children.length === 0) return
  out.push(nodeKey(path, node))
  for (const child of node.children) collectExpandable(child, path + child.name, out)
}

function expandAll(): void {
  const info = elfInfo.value
  if (!info) return
  const keys: string[] = []
  for (const node of info.variables) collectExpandable(node, node.name, keys)
  expandedKeys.value = new Set(keys)
}

function collapseAll(): void {
  expandedKeys.value = new Set()
}

function newSeries(name: string, key: string, typeName: string, node: VariableNode): CurveSeries | null {
  if (!node.sample && !node.location) return null
  const location = node.sample?.location ?? node.location
  const register = node.sample?.register
  if (isLocalSource.value && !localExprForNode(node)) return null
  if (seriesList.value.some((item) => item.key === key)) return null
  if (isLocalSource.value && seriesList.value.length >= CURVE_LOCAL_MAX_SERIES) {
    globalNotify(translate('curve.localSeriesLimit', {count: CURVE_LOCAL_MAX_SERIES}), 'warning')
    return null
  }
  if (!isLocalSource.value && seriesList.value.length >= MAX_CURVE_SERIES) {
    globalNotify(translate('curve.maxSeries', {max: MAX_CURVE_SERIES}), 'warning')
    return null
  }
  const series: CurveSeries = {
    id: nextSeriesId++,
    name,
    key,
    typeName,
    address: node.address >>> 0,
    size: node.size,
    sample: node.sample as SampleLeaf,
    format: globalFormat.value,
    color: COLORS[seriesList.value.length % COLORS.length],
    visible: true,
    startSeq: sampleSeq + (sampling.value ? 1 : 0),
    mode: samplingMode.value === 'debug' ? 'halt' : 'live',
    location,
    register,
  }
  seriesLo.set(series.id, new Uint32Array(MAX_POINTS))
  seriesHi.set(series.id, new Uint32Array(MAX_POINTS))
  seriesList.value = [...seriesList.value, series]
  scheduleDraw()
  return series
}

function toggleRow(row: VariableRow): void {
  if (row.kind !== 'variable' || !nodeSelectable(row.node)) return
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  if (isRowSelected(row)) {
    removeSeriesByKey(row.key)
    return
  }
  newSeries(row.path, row.key, row.node.typeName, row.node)
}

function removeSeries(id: number): void {
  seriesLo.delete(id)
  seriesHi.delete(id)
  seriesList.value = seriesList.value.filter((item) => item.id !== id)
  hover.value = null
  scheduleDraw()
}

function removeSeriesByKey(key: string): void {
  const item = seriesList.value.find((candidate) => candidate.key === key)
  if (item) removeSeries(item.id)
}

function toggleVisible(item: CurveSeries): void {
  item.visible = !item.visible
  scheduleDraw()
}

function applyGlobalFormat(): void {
  for (const item of seriesList.value) item.format = globalFormat.value
  scheduleDraw()
}

const wideHint = computed(() => seriesList.value.some((item) => item.sample.size > 4 || item.sample.bitSize > 32))

function sourceLabel(node: VariableNode): string {
  const path = node.relativeFile ?? node.file
  if (!path) return '-'
  return node.line ? `${path}:${node.line}` : path
}

function legendLabel(item: CurveSeries): string {
  return item.typeName ? `${item.name} [${item.typeName}]` : item.name
}

// ---- ELF 加载 -------------------------------------------------------------
const dwarfFallbackReason = computed(() => {
  const info = elfInfo.value
  if (!info) return ''
  const warning = info.warnings.find((item) => item.startsWith('DWARF parse failed:'))
  return warning ? warning.replace('DWARF parse failed:', '').trim() : ''
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
    const result = parseElfVariables(buffer)
    elfInfo.value = result
    try {
      elfFunctionSymbols.value = buildFunctionSymbols(parseElf32Symbols(buffer).symbols)
    } catch {
      elfFunctionSymbols.value = []
    }
    selectedFunctionAddress.value = null
    variableListEpoch.value += 1
    variableSearch.value = ''
    expandedKeys.value = new Set()
    if (result.variables.length === 0) {
      elfError.value = translate('curve.noSymbols')
    }
  } catch (error) {
    elfInfo.value = null
    elfFunctionSymbols.value = []
    selectedFunctionAddress.value = null
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
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
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
    const name = formatHex32(address)
    if (seriesList.value.some((item) => item.address === address && item.typeName === '')) continue
    const sample: SampleLeaf = {address, size: 4, bitOffset: 0, bitSize: 0, kind: 'uint', signed: false}
    const node: VariableNode = {name, typeName: '', address, size: 4, sample}
    if (newSeries(name, `manual@${address.toString(16)}`, '', node)) added.push(name)
  }
  if (added.length > 0) manualInput.value = ''
}

// ---- 数值解码（DWARF 类型感知） -------------------------------------------
function f32FromU32(value: number): number {
  f32U32[0] = value >>> 0
  return f32Buffer[0]
}

function f64FromLoHi(lo: number, hi: number): number {
  f64U32[0] = lo >>> 0
  f64U32[1] = hi >>> 0
  return f64Buffer[0]
}

function toBigInt(lo: number, hi: number): bigint {
  return (BigInt(hi >>> 0) << 32n) | BigInt(lo >>> 0)
}

function bitsValue(lo: number, hi: number, bits: number, signed: boolean): bigint {
  let value = toBigInt(lo, hi)
  if (bits > 0 && bits < 64) value &= (1n << BigInt(bits)) - 1n
  if (signed && bits > 0) {
    const signBit = 1n << BigInt(bits - 1)
    if ((value & signBit) !== 0n) value -= 1n << BigInt(bits)
  }
  return value
}

function sampleBitWidth(item: CurveSeries): number {
  return item.sample.bitSize > 0 ? Math.min(item.sample.bitSize, 64) : Math.min(item.sample.size * 8, 64)
}

/** 图表 Y 值：按 series.format（auto 时按 DWARF 类型）解码成 number。 */
function decodeNumber(item: CurveSeries, lo: number, hi: number): number {
  if (item.format === 'u32') return lo >>> 0
  if (item.format === 'i32') return lo | 0
  if (item.format === 'f32') return f32FromU32(lo)
  const bits = sampleBitWidth(item)
  switch (item.sample.kind) {
    case 'float32':
      return f32FromU32(lo)
    case 'float64':
      return f64FromLoHi(lo, hi)
    case 'bool':
      return bitsValue(lo, hi, bits, false) !== 0n ? 1 : 0
    case 'int':
      return Number(bitsValue(lo, hi, bits, true))
    case 'enum':
      return Number(bitsValue(lo, hi, bits, false))
    case 'raw':
      return lo >>> 0
    default:
      return Number(bitsValue(lo, hi, bits, false))
  }
}

function formatFloatText(value: number): string {
  if (!Number.isFinite(value)) return String(value)
  return String(Number.parseFloat(value.toPrecision(7)))
}

/** 读数/图例/悬浮提示文本：auto 时按 DWARF 类型（64-bit 精确字符串、枚举名）。 */
function decodeText(item: CurveSeries, lo: number, hi: number): string {
  if (item.format === 'u32') return String(lo >>> 0)
  if (item.format === 'i32') return String(lo | 0)
  if (item.format === 'f32') return formatFloatText(f32FromU32(lo))
  const bits = sampleBitWidth(item)
  switch (item.sample.kind) {
    case 'float32':
      return formatFloatText(f32FromU32(lo))
    case 'float64':
      return formatFloatText(f64FromLoHi(lo, hi))
    case 'bool':
      return bitsValue(lo, hi, bits, false) !== 0n ? 'true' : 'false'
    case 'enum': {
      const value = Number(bitsValue(lo, hi, bits, false))
      const enumerator = item.sample.enumValues?.find((entry) => entry[0] === value)
      return enumerator ? `${enumerator[1]} (${value})` : String(value)
    }
    case 'int':
      return String(bitsValue(lo, hi, bits, true))
    case 'raw': {
      if (bits <= 32) return `0x${(lo >>> 0).toString(16).toUpperCase()}`
      return `0x${toBigInt(lo, hi).toString(16).toUpperCase()}`
    }
    default:
      return String(bitsValue(lo, hi, bits, false))
  }
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
  const loBuffer = seriesLo.get(item.id)
  const hiBuffer = seriesHi.get(item.id)
  if (!loBuffer || !hiBuffer) return '-'
  for (let order = ringCount - 1; order >= 0; order--) {
    const slot = ringSlot(order)
    if (slotSeq[slot] < item.startSeq) continue
    return decodeText(item, loBuffer[slot], hiBuffer[slot])
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
      const loBuffer = seriesLo.get(item.id)
      const hiBuffer = seriesHi.get(item.id)
      if (!loBuffer || !hiBuffer) continue
      const value = decodeNumber(item, loBuffer[slot], hiBuffer[slot])
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
    const loBuffer = seriesLo.get(item.id)
    const hiBuffer = seriesHi.get(item.id)
    if (!loBuffer || !hiBuffer) continue
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
      const value = decodeNumber(item, loBuffer[slot], hiBuffer[slot])
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
        const loBuffer = seriesLo.get(item.id)
        const hiBuffer = seriesHi.get(item.id)
        if (!loBuffer || !hiBuffer || slotSeq[slot] < item.startSeq) continue
        const value = decodeNumber(item, loBuffer[slot], hiBuffer[slot])
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
    const loBuffer = seriesLo.get(item.id)
    const hiBuffer = seriesHi.get(item.id)
    if (!loBuffer || !hiBuffer) continue
    entries.push({name: item.name, color: item.color, text: decodeText(item, loBuffer[slot], hiBuffer[slot])})
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
  const left = Math.min(state.x + 14, Math.max(0, width - 230))
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
  const header = ['time_s', ...seriesList.value.map((item) => csvEscape(legendLabel(item)))]
  const lines: string[] = [header.join(',')]
  for (let order = 0; order < ringCount; order++) {
    const slot = ringSlot(order)
    const cells: string[] = [(timestamps[slot] as number).toFixed(4)]
    for (const item of seriesList.value) {
      const loBuffer = seriesLo.get(item.id)
      const hiBuffer = seriesHi.get(item.id)
      if (!loBuffer || !hiBuffer || slotSeq[slot] < item.startSeq) {
        cells.push('')
      } else {
        cells.push(decodeText(item, loBuffer[slot], hiBuffer[slot]))
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

.variable-search {
  @apply min-w-56 flex-1;
}

.var-table {
  @apply overflow-hidden rounded border border-gray-200;
}

.var-head,
.var-row {
  display: grid;
  grid-template-columns: 1.5rem minmax(0, 1.7fr) minmax(0, 1.2fr) 6.5rem 3.5rem minmax(0, 1fr);
  align-items: center;
  gap: 0.25rem;
  padding: 0 0.375rem;
}

.var-head {
  @apply sticky top-0 z-10 border-b border-gray-200 bg-gray-50 text-xs font-semibold text-gray-600;
  height: 1.75rem;
}

.var-list {
  max-height: 22rem;
  overflow: auto;
}

.var-row {
  height: 1.75rem;
  border-bottom: 1px solid #f3f4f6;
  font-size: 0.75rem;
}

.var-row:hover {
  background-color: var(--el-color-primary-light-9);
}

.var-row.selected {
  background-color: var(--el-color-primary-light-8);
}

.var-row.unsampleable {
  opacity: 0.62;
}

.unsampleable-text {
  font-size: 0.7rem;
  color: #9ca3af;
}

.breakpoint-input.is-invalid :deep(.el-input__wrapper) {
  box-shadow: 0 0 0 1px #dc2626 inset;
}

.mode-badge {
  @apply inline-flex items-center rounded px-1 text-[0.6rem] font-semibold;
  background-color: #e0f2fe;
  color: #0369a1;
  white-space: nowrap;
}

.var-group-row {
  @apply flex items-center gap-2 border-b border-gray-200 bg-gray-50 px-2 text-xs font-semibold text-gray-700;
  height: 1.5rem;
}

.var-check {
  @apply flex items-center justify-center;
}

.var-name {
  @apply flex min-w-0 items-center gap-1;
}

.var-type,
.var-src {
  @apply min-w-0 overflow-hidden text-gray-600;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.var-addr {
  @apply text-gray-700;
}

.tree-arrow {
  @apply flex h-4 w-4 shrink-0 items-center justify-center text-xs text-gray-500;
}

.tree-arrow-spacer {
  @apply inline-block h-4 w-4 shrink-0;
}

.selected-list {
  max-height: 12rem;
  overflow: auto;
}

.selected-row {
  display: grid;
  grid-template-columns: 0.75rem minmax(0, 1.5fr) minmax(0, 1fr) 6.5rem 5.5rem 1.5rem;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.75rem;
  padding: 0.125rem 0;
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
  @apply pointer-events-none absolute max-w-72 rounded border border-gray-200 bg-white px-2 py-1 text-xs shadow;
}

.legend {
  @apply mt-2 flex flex-wrap gap-2;
}

.legend-item {
  @apply flex max-w-96 items-center gap-1 rounded border border-gray-200 px-2 py-1 text-xs;
}

.legend-item:hover {
  background-color: var(--el-color-primary-light-9);
}

.legend-item.off {
  @apply opacity-40;
}
</style>
