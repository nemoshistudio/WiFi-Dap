<template>
  <div class="curve-layout" :class="{'mobile-advanced': mobileAdvanced}">
    <h1 class="page-title">{{ translate('page.debugCurve') }}</h1>

    <el-alert
        v-if="!isLocalSource"
        class="mb-3"
        type="warning"
        :title="translate('curve.takeoverHint')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else
        class="mb-3"
        type="info"
        :title="translate('curve.localTakeoverNote')"
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

    <!-- SourceBrowser/左侧 tab 扩展位：源码 tab 由独立任务接入，本任务保留布局挂点。 -->
    <div class="curve-workbench">
      <div class="curve-workbench-main">
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
        <el-button class="mobile-only" size="small" @click="mobileAdvanced = !mobileAdvanced">
          {{ mobileAdvanced ? translate('curve.mobileLess') : translate('curve.mobileMore') }}
        </el-button>
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
              <el-option value="watchpoint" :label="translate('curve.localTriggerWatchpoint')"/>
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
          <template v-if="localTrigger === 'watchpoint'">
            <label class="flex items-center gap-2">
              <span class="whitespace-nowrap">{{ translate('curve.watchpointAddress') }}</span>
              <el-input v-model="watchAddressInput" size="small" style="width: 9.5rem" :disabled="sampling" placeholder="0x24001234"/>
            </label>
            <label class="flex items-center gap-2">
              <span>{{ translate('curve.watchpointSize') }}</span>
              <el-select v-model="watchSize" size="small" style="width: 4.5rem" :disabled="sampling">
                <el-option :value="1" label="1"/>
                <el-option :value="2" label="2"/>
                <el-option :value="4" label="4"/>
              </el-select>
            </label>
            <label class="flex items-center gap-2">
              <span>{{ translate('curve.watchpointAccess') }}</span>
              <el-select v-model="watchAccess" size="small" style="width: 6.5rem" :disabled="sampling">
                <el-option value="write" :label="translate('curve.watchpointWrite')"/>
                <el-option value="readwrite" :label="translate('curve.watchpointReadWrite')"/>
              </el-select>
            </label>
            <span v-if="watchpointError" class="text-xs" style="color: #dc2626">{{ watchpointError }}</span>
          </template>
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
          <span v-else-if="localTrigger === 'watchpoint'">
            {{ translate('curve.watchpointArmed') }}: <b>{{ watchpointArmed === null ? '-' : (watchpointArmed ? 'ON' : 'OFF') }}</b>
            · {{ translate('curve.watchpointHits') }}: <b>{{ watchpointHits ?? '-' }}</b>
            · {{ translate('curve.watchpointTriggerSeq') }}: <b>{{ watchTriggerSeq ?? '-' }}</b>
          </span>
          <span v-else>{{ translate('curve.localHits') }}: <b>{{ localStatusInfo?.hits ?? localHits ?? '-' }}</b></span>
          <span v-if="activePc !== null">PC: <b class="font-mono">0x{{ activePc.toString(16).toUpperCase() }}</b>
            <span v-if="activePcLocation" class="opacity-70">
              {{ typeof activePcLocation.file === 'string' ? activePcLocation.file : (activePcLocation.file as any)?.relativePath ?? (activePcLocation.file as any)?.path ?? '' }}:{{ activePcLocation.line }}
            </span>
            <span v-if="pcLineStatusMessage" class="opacity-60"> · {{ pcLineStatusMessage }}</span>
          </span>
          <span v-if="localTrigger === 'breakpoint'">{{ translate('curve.localSampled') }}: <b>{{ localStatusInfo?.sampled ?? localSampled ?? '-' }}</b></span>
          <span>{{ translate('curve.localFrames') }}: <b>{{ localFramesReceived }}</b></span>
          <span>{{ translate('curve.localInvalid') }}: <b>{{ localInvalidCount }}</b></span>
          <span>{{ translate('curve.localOutOfScope') }}: <b>{{ localOutOfScopeCount }}</b></span>
          <span>{{ translate('curve.localOverwritten') }}: <b>{{ localOverwrittenCount }}</b></span>
          <span>{{ translate('curve.localSeries') }}: <b>{{ seriesList.length }}/{{ seriesLimit }}</b></span>
          <span>{{ translate('curve.localGroups') }}: <b>{{ localActiveGroupCount }}</b></span>
          <span>{{ translate('curve.localCurrentGroup') }}: <b>{{ sampling ? ((localCurrentGroup ?? 0) + 1) + '/' + localActiveGroupCount : '-' }}</b></span>
          <span>{{ translate('curve.localGroupRate') }}: <b>{{ localGroupRefreshRate.toFixed(1) }} Hz</b></span>
          <span>{{ translate('curve.wsTransport') }}: <b>{{ wsTransport }}</b></span>
          <span>{{ translate('curve.wsFrames') }}: <b>{{ wsFrames }}</b></span>
          <span>{{ translate('curve.wsDropped') }}: <b>{{ wsDropped }}</b></span>
          <span>{{ translate('curve.wsLatency') }}: <b>{{ wsLatencyMs.toFixed(1) }} ms</b></span>
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
      <div v-if="isLocalSource && localTrigger === 'breakpoint' && localBreakpointError" class="mt-1 text-xs curve-field-error">
        {{ localBreakpointError }}
      </div>
      <div v-if="isLocalSource && localTrigger === 'watchpoint'" class="mt-1 flex flex-wrap items-center gap-2 text-xs">
        <span>{{ translate('curve.watchpointJump') }}</span>
        <el-input-number v-model="jumpFrames" :min="1" :max="500" :controls="false" size="small" style="width: 4.5rem"/>
        <el-button size="small" :disabled="watchTriggerSeq === null" @click="jumpToWatchTrigger(true)">{{ translate('curve.watchpointJumpPre') }}</el-button>
        <el-button size="small" :disabled="watchTriggerSeq === null" @click="jumpToWatchTrigger(false)">{{ translate('curve.watchpointJumpPost') }}</el-button>
        <span v-if="watchFramesTriggered" class="render-warning">{{ translate('curve.watchpointTriggered') }}</span>
      </div>
      <div v-if="isLocalSource && localOutOfScopeCount > 0" class="mt-1 text-xs opacity-70">
        {{ translate('curve.localOutOfScopeHint') }}
      </div>
      <div v-if="isLocalSource" class="mt-1 text-xs opacity-70">
        {{ translate('curve.localGroupingHint') }}
      </div>
      <div v-if="isLocalSource" class="mt-2 rounded border border-gray-200 p-2 text-xs">
        <div class="flex flex-wrap items-center gap-2">
          <el-checkbox v-model="conditionEnabled" :disabled="sampling">{{ translate('curve.conditionEnable') }}</el-checkbox>
          <template v-if="conditionEnabled">
            <label class="flex items-center gap-1">
              <span>{{ translate('curve.conditionAddress') }}</span>
              <el-input v-model="conditionAddressInput" size="small" style="width: 8.5rem" :disabled="sampling" placeholder="0x24001234"/>
            </label>
            <label class="flex items-center gap-1">
              <span>{{ translate('curve.conditionSize') }}</span>
              <el-select v-model="conditionSize" size="small" style="width: 4rem" :disabled="sampling">
                <el-option :value="1" label="1"/><el-option :value="2" label="2"/><el-option :value="4" label="4"/>
              </el-select>
            </label>
            <label class="flex items-center gap-1">
              <span>{{ translate('curve.conditionOp') }}</span>
              <el-select v-model="conditionOp" size="small" style="width: 4.5rem" :disabled="sampling">
                <el-option v-for="op in ['==','!=','>','>=','<','<=']" :key="op" :value="op" :label="op"/>
              </el-select>
            </label>
            <label class="flex items-center gap-1">
              <span>{{ translate('curve.conditionValue') }}</span>
              <el-input-number v-model="conditionValue" :controls="false" size="small" style="width: 6rem" :disabled="sampling"/>
            </label>
            <el-checkbox v-model="conditionSigned" :disabled="sampling">{{ translate('curve.conditionSigned') }}</el-checkbox>
            <label class="flex items-center gap-1">
              <span>{{ translate('curve.conditionMode') }}</span>
              <el-select v-model="conditionMode" size="small" style="width: 6rem" :disabled="sampling">
                <el-option value="once" :label="translate('curve.conditionOnce')"/>
                <el-option value="continuous" :label="translate('curve.conditionContinuous')"/>
              </el-select>
            </label>
            <el-checkbox v-model="conditionStopAfter" :disabled="sampling">{{ translate('curve.conditionStopAfter') }}</el-checkbox>
            <span v-if="conditionError" style="color: #dc2626">{{ conditionError }}</span>
          </template>
          <template v-if="conditionEnabled && (conditionArmed !== null || conditionHits !== null)">
            <span>{{ translate('curve.conditionArmed') }}: <b>{{ conditionArmed === null ? '-' : (conditionArmed ? 'ON' : 'OFF') }}</b></span>
            <span>{{ translate('curve.conditionMet') }}: <b>{{ conditionMet === null ? '-' : (conditionMet ? 'YES' : 'NO') }}</b></span>
            <span>{{ translate('curve.conditionHits') }}: <b>{{ conditionHits ?? '-' }}</b></span>
            <span>{{ translate('curve.conditionTriggerSeq') }}: <b>{{ conditionTriggerSeq ?? '-' }}</b></span>
          </template>
          <template v-if="conditionTriggerSeq !== null">
            <span>{{ translate('curve.conditionJump') }}</span>
            <el-button size="small" @click="jumpToConditionTrigger(true)">{{ translate('curve.watchpointJumpPre') }}</el-button>
            <el-button size="small" @click="jumpToConditionTrigger(false)">{{ translate('curve.watchpointJumpPost') }}</el-button>
            <span v-if="conditionFramesTriggered" class="render-warning">{{ translate('curve.watchpointTriggered') }}</span>
          </template>
        </div>
      </div>
      <div v-if="isLocalSource" class="group-weights mt-1 flex flex-wrap items-center gap-2 text-xs">
        <span>{{ translate('curve.groupWeight') }}</span>
        <label v-for="(weight, index) in groupWeights.slice(0, localActiveGroupCount)" :key="index" class="flex items-center gap-1">
          <span>G{{ index + 1 }}</span>
          <el-input-number
              :model-value="weight"
              :min="1"
              :max="8"
              :step="1"
              size="small"
              :controls="false"
              :disabled="sampling"
              style="width: 3.2rem"
              @change="(value: number) => setGroupWeight(index, value)"
          />
        </label>
        <span class="opacity-70">Σ{{ localGroupWeightSum }} (≤64)</span>
        <span v-if="localGroupWeightedRates.length" class="font-mono opacity-70">
          {{ localGroupWeightedRates.map((rate) => rate.toFixed(1)).join(' / ') }} Hz
        </span>
      </div>
    </section>

    <!-- ELF / DWARF 变量浏览器 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-lg font-bold">{{ translate('curve.elfUpload') }}</h2>
        <el-segmented v-model="elfView" :options="elfViewOptions" size="small" class="elf-view-switch"/>
        <el-button :loading="elfParsing" :disabled="isLocalSource && sampling" @click="openElfDialog">{{ translate('curve.elfUpload') }}</el-button>
        <input ref="elfInputRef" type="file" accept=".elf,.out" class="hidden" @change="onElfSelected"/>
      </div>
      <p class="mt-1 text-xs opacity-70">{{ translate('curve.elfHint') }}</p>
      <p v-if="!elfInfo" class="mt-1 text-xs opacity-70">{{ translate('curve.emptyElfState') }}</p>

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

      <!-- Variables：原有变量树/搜索/批量选择/手动地址（保持全部现有逻辑） -->
      <div v-show="elfView === 'variables'">
      <template v-if="elfInfo && elfInfo.variables.length > 0">
        <div class="mt-2 flex flex-wrap items-center gap-2">
          <el-input
              v-model="variableSearchInput"
              size="small"
              clearable
              class="variable-search"
              :placeholder="translate('curve.searchPlaceholder')"
              @input="onVariableSearchInput"
          />
          <el-checkbox v-model="onlySampleable" size="small">{{ translate('curve.onlySampleable') }}</el-checkbox>
          <el-checkbox v-model="activeOnly" size="small">{{ translate('curve.activeOnly') }}</el-checkbox>
          <el-checkbox v-model="currentFunctionOnly" size="small">{{ translate('curve.currentFunctionOnly') }}</el-checkbox>
          <label class="flex items-center gap-1 text-xs">
            <span class="whitespace-nowrap">{{ translate('curve.groupBy') }}</span>
            <el-select v-model="groupBy" size="small" style="width: 9rem">
              <el-option v-for="option in GROUP_OPTIONS" :key="option" :value="option" :label="groupLabel(option)"/>
            </el-select>
          </label>
          <el-button size="small" @click="expandAll">{{ translate('curve.expandAll') }}</el-button>
          <el-button size="small" @click="collapseAll">{{ translate('curve.collapseAll') }}</el-button>
          <span class="ml-auto text-xs opacity-70">{{ translate('curve.variables') }}: {{ visibleVariableCount }}</span>
          <span class="w-full"></span>
          <el-button size="small" :disabled="bulkSelectDisabled" @click="selectAllVisible">{{ translate('curve.selectAllVisible') }}</el-button>
          <el-button size="small" :disabled="bulkSelectDisabled" @click="selectAllSampleable">{{ translate('curve.selectAllSampleable') }}</el-button>
          <el-button size="small" :disabled="(isLocalSource && sampling) || seriesList.length === 0" @click="clearSelection">{{ translate('curve.clearSelection') }}</el-button>
          <el-button size="small" :disabled="bulkSelectDisabled || !lastClickedFile" @click="selectCurrentFile">{{ translate('curve.selectCurrentFile') }}</el-button>
          <el-button size="small" :disabled="bulkSelectDisabled || !lastClickedGroup" @click="selectCurrentGroup">{{ translate('curve.selectCurrentGroup') }}</el-button>
          <span class="text-xs opacity-70">{{ translate('curve.selected') }}: {{ seriesList.length }}/{{ selectionLimitForSource() }} · {{ translate('curve.shiftSelectHint') }}</span>
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
            <div
                v-for="row in shownVariableRows"
                :key="row.key"
                v-memo="[row.key, row.kind, row.expanded, row.depth, selectedKeySet, variableListEpoch,
                  onlySampleable, groupBy, variableSearch, samplingSource, row.groupLabel, row.groupCount]"
                :class="row.kind === 'group'
                  ? 'var-group-row'
                  : ['var-row', {selected: isRowSelected(row), unsampleable: !row.node.sampleable,
                    selectable: nodeSelectable(row.node), 'pc-stale': activePc !== null && !nodeValidAtPc(row.node)}]"
                @click="row.kind === 'variable' ? onVariableRowClick(row, $event) : undefined"
            >
              <template v-if="row.kind === 'group'">
                <span class="truncate font-semibold" :title="row.groupLabel">{{ row.groupLabel }}</span>
                <span class="opacity-60">({{ row.groupCount }})</span>
              </template>
              <template v-else>
                <span class="var-check" @click.stop>
                  <span
                      v-if="activePc !== null && !nodeValidAtPc(row.node)"
                      class="stale-dot"
                      :title="translate('curve.activeStaleHint')"
                  >●</span>
                  <el-checkbox
                      v-if="nodeSelectable(row.node)"
                      :model-value="isRowSelected(row)"
                      :disabled="isLocalSource && sampling"
                      @change="toggleRow(row)"
                  />
                  <button
                      type="button"
                      class="watch-star"
                      :class="{active: isWatched(row.key)}"
                      :title="isWatched(row.key) ? translate('curve.watchRemove') : translate('curve.watchAdd')"
                      @click.stop="toggleWatch(row.key, row.node.name)"
                  >{{ isWatched(row.key) ? '★' : '☆' }}</button>
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
                  <span
                      v-if="row.node.loclistMulti"
                      class="mode-badge loclist-badge"
                      :title="translate('curve.loclistMultiHint')"
                  >{{ translate('curve.loclistMultiShort') }}</span>
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
              </template>
            </div>
            <div v-if="visibleVariableCount === 0" class="px-2 py-2 text-sm opacity-70">
              {{ translate('curve.noSymbols') }}
            </div>
          </div>
          <div v-if="remainingVariableRows > 0" class="show-more-row">
            <el-button size="small" @click="showMoreVariables">
              {{ translate('curve.showMoreVariables', {count: remainingVariableRows}) }}
            </el-button>
            <span class="text-xs opacity-70">{{ shownVariableRows.length }} / {{ variableRows.length }}</span>
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
      <p v-if="isLocalSource && seriesList.length === 0" class="mt-2 text-xs opacity-70">{{ translate('curve.localSelectHint') }}</p>
      </div>

      <!-- Source：本地项目源码浏览 + 点击变量直接加入曲线（SourceBrowser 独立组件，labels 由页面 i18n 注入） -->
      <div v-if="sourceViewEverOpened" v-show="elfView === 'source'" class="source-view">
        <SourceBrowser
            ref="sourceBrowserRef"
            :project="sourceProject"
            :variables="elfInfo?.variables ?? []"
            :selected-keys="sourceSelectedKeys"
            :labels="sourceBrowserLabels"
            :live-values="sourceLiveValues"
            :active-key="activeSourceKey"
            :active-file-line="sourceActiveFileLine"
            :line-addresses="sourceLineAddresses"
            @project-loaded="onSourceProjectLoaded"
            @select-variable="onSourceVariableSelected"
            @elf-file="onSourceElfFile"
            @line-click="onSourceLineClick"
        />
      </div>
    </section>

    <!-- task-42：目标检测 / ELF 匹配 -->
    <section class="curve-card target-panel advanced-control">
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="text-sm font-bold">{{ translate('curve.targetTitle') }}</span>
        <el-button size="small" :loading="targetLoading" @click="refreshTargetInfo">{{ translate('curve.targetRefresh') }}</el-button>
        <span v-if="targetError" style="color: #dc2626">{{ targetError }}</span>
        <span v-else-if="targetRaw" class="opacity-70">
          <template v-if="detectedTarget.detected">
            {{ translate('curve.targetDetected') }}: {{ detectedTarget.baseGroup || detectedTarget.series || '-' }}
            · DEV_ID {{ detectedTarget.devId || '-' }}
            · Flash {{ detectedTarget.flashKib || '-' }} KiB
            · {{ translate('curve.targetConfidence') }} {{ detectedTarget.confidence }}
          </template>
          <template v-else>{{ translate('curve.targetNotDetected') }}</template>
        </span>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.targetProfile') }}</span>
          <el-select v-model="selectedTargetProfileId" size="small" clearable style="width: 12rem" :placeholder="translate('curve.targetProfileNone')">
            <el-option v-for="profile in knownTargetProfiles" :key="profile.id" :value="profile.id" :label="`${profile.id} (${profile.flashKib} KiB)`"/>
          </el-select>
        </label>
        <span v-if="selectedTargetProfile" class="opacity-70">
          {{ translate('curve.targetMatch') }}: {{ targetMatch.level }}
        </span>
        <span v-if="elfFlashRange" class="font-mono opacity-70">
          ELF: 0x{{ elfFlashRange[0].toString(16).toUpperCase() }}–0x{{ elfFlashRange[1].toString(16).toUpperCase() }}
        </span>
        <el-alert v-if="targetFlashWarning" type="warning" :title="targetFlashWarning" :closable="false" show-icon class="w-full"/>
        <el-button size="small" :loading="verifyState === 'running'" @click="verifyElfAgainstTarget">{{ translate('curve.verifyButton') }}</el-button>
        <span v-if="verifyState !== 'idle'" class="font-mono" :style="{color: verifyState === 'match' ? '#16a34a' : verifyState === 'mismatch' ? '#dc2626' : '#6b7280'}">
          {{ verifyState }} {{ verifyDetail }}
        </span>
      </div>
    </section>

    <!-- 已选曲线 -->
    <section v-if="seriesList.length > 0" class="curve-card">
      <div class="flex flex-wrap items-center gap-2 text-xs opacity-70">
        <span>{{ translate('curve.selected') }}: {{ seriesList.length }} / {{ seriesLimit }}</span>
        <el-button size="small" link @click="selectedCollapsed = !selectedCollapsed">
          {{ selectedCollapsed ? translate('curve.selectedExpand') : translate('curve.selectedCollapse') }}
        </el-button>
        <el-button size="small" type="danger" link :disabled="isLocalSource && sampling" @click="clearSelection">{{ translate('curve.clearSelection') }}</el-button>
        <el-button size="small" link :disabled="sampling" @click="addDerivedCurve('add')">{{ translate('curve.derivedAdd') }}</el-button>
        <el-button size="small" link :disabled="sampling" @click="addDerivedCurve('sub')">{{ translate('curve.derivedSub') }}</el-button>
        <el-input v-model="presetName" size="small" style="width: 9rem" :placeholder="translate('curve.presetName')"/>
        <el-button size="small" @click="savePreset">{{ translate('curve.presetSave') }}</el-button>
        <el-select v-model="selectedPresetId" size="small" style="width: 10rem" :placeholder="translate('curve.presetSelect')">
          <el-option v-for="preset in presets" :key="preset.id" :value="preset.id" :label="preset.name"/>
        </el-select>
        <el-button size="small" :disabled="!selectedPresetId" @click="applyPreset">{{ translate('curve.presetLoad') }}</el-button>
        <el-button size="small" :disabled="!selectedPresetId" @click="deletePreset">{{ translate('curve.presetDelete') }}</el-button>
      </div>
      <div v-if="!selectedCollapsed" class="selected-list mt-1">
        <div
            v-for="item in shownSelectedSeries"
            :key="item.id"
            class="selected-row"
        >
          <el-checkbox :model-value="item.visible" size="small" @change="toggleVisible(item)"/>
          <span class="series-dot" :style="{background: seriesDisplayColor(item.color)}"></span>
          <span class="truncate jump-source" :title="translate('curve.sourceViewSource')" @click.stop="jumpToSource(item.key)">{{ item.name }}</span>
          <span class="truncate text-xs opacity-70" :title="item.typeName">{{ item.typeName || translate('curve.unknownType') }}</span>
          <span class="font-mono text-xs opacity-70" :title="item.location ? addressExprText(item.location) : ''">{{ item.location ? addressExprText(item.location) : formatHex32(item.address) }}</span>
          <el-select v-model="item.format" size="small" style="width: 5.5rem" @change="onSeriesFormatChange(item)">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
          <el-button link size="small" :disabled="seriesList[0]?.id === item.id" @click="moveSeries(item, -1)">↑</el-button>
          <el-button link size="small" :disabled="seriesList[seriesList.length - 1]?.id === item.id" @click="moveSeries(item, 1)">↓</el-button>
          <el-button type="danger" link size="small" @click="removeSeries(item.id)">×</el-button>
        </div>
        <div v-if="remainingSelected > 0" class="show-more-row">
          <el-button size="small" @click="selectedShowAll = true">
            {{ translate('curve.legendShowMore', {count: remainingSelected}) }}
          </el-button>
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

      <div class="render-bar advanced-control mt-2 flex flex-wrap items-center gap-x-3 gap-y-1 text-xs">
        <span>{{ translate('curve.rendering') }}: <b>{{ renderingLabel }}</b></span>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.smoothing') }}</span>
          <el-select v-model="smoothingWindow" size="small" style="width: 4.5rem">
            <el-option v-for="option in SMOOTHING_WINDOWS" :key="option" :value="option" :label="option === 1 ? translate('curve.smoothingOff') : String(option)"/>
          </el-select>
        </label>
        <el-button size="small" @click="runFft">{{ translate('curve.fftButton') }}</el-button>
        <el-checkbox v-model="autoPcSource" size="small">{{ translate('curve.pcSourceFollow') }}</el-checkbox>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.yAxis') }}</span>
          <el-select v-model="yAxisMode" size="small" style="width: 7.5rem">
            <el-option value="auto" :label="translate('curve.yAxisAuto')"/>
            <el-option value="normalized" :label="translate('curve.yAxisNormalized')"/>
            <el-option value="log" :label="translate('curve.yAxisLog')"/>
          </el-select>
        </label>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.timeAxis') }}</span>
          <el-select v-model="timeWindowSeconds" size="small" style="width: 4.5rem">
            <el-option v-for="window in TIME_WINDOW_OPTIONS" :key="window" :value="window" :label="window + 's'"/>
          </el-select>
        </label>
        <label class="flex items-center gap-1">
          <el-switch v-model="followLatest" size="small"/>
          <span>{{ translate('curve.followLatest') }}</span>
        </label>
        <span class="font-mono">{{ timeAxisStatus }}</span>
        <el-button size="small" @click="toggleFreeze">{{ chartFrozen ? translate('curve.unfreeze') : translate('curve.freeze') }}</el-button>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.valueScale') }}</span>
          <el-input-number v-model="valueMultiplier" size="small" :step="0.1" :controls="false" style="width: 4.5rem"/>
          <span>× +</span>
          <el-input-number v-model="valueOffset" size="small" :step="1" :controls="false" style="width: 4.5rem"/>
        </label>
        <label class="flex items-center gap-1">
          <el-switch v-model="renderAll" size="small"/>
          <span>{{ translate('curve.renderModeA') }}</span>
        </label>
        <label v-if="!renderAll" class="flex items-center gap-1">
          <span>{{ translate('curve.drawCap') }}</span>
          <el-select v-model="drawCap" size="small" style="width: 4.5rem">
            <el-option v-for="cap in DRAW_CAP_OPTIONS" :key="cap" :value="cap" :label="String(cap)"/>
          </el-select>
        </label>
        <span v-if="hiddenByCapCount > 0" class="render-warning">{{ translate('curve.hiddenByCap', {count: hiddenByCapCount}) }}</span>
        <span v-if="autoCapReduced" class="render-warning">{{ translate('curve.autoCapReduced') }}</span>
        <span v-if="renderAll && visibleSeriesList.length > 128" class="render-warning">{{ translate('curve.aModeOverload', {count: visibleSeriesList.length}) }}</span>
        <span v-if="renderAll" class="render-warning">{{ translate('curve.renderAllHint') }}</span>
      </div>

      <div ref="chartWrapRef" class="chart-wrap">
        <canvas
            ref="canvasRef"
            role="img"
            tabindex="0"
            :aria-label="translate('curve.ariaChart')"
            @click="onCanvasCursorClick"
            @pointermove="onCanvasPointerMove"
            @pointerdown="onCanvasPointerDown"
            @pointerup="onCanvasPointerUp"
            @pointercancel="onCanvasPointerUp"
            @pointerleave="onCanvasLeave"
            @wheel.prevent="onCanvasWheel"
            @dblclick="resetTimeView"
        ></canvas>
        <canvas ref="hoverCanvasRef" class="chart-overlay"></canvas>
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
            v-for="item in legendItems"
            :key="item.id"
            type="button"
            class="legend-item"
            :class="{off: !item.visible}"
            :title="legendLabel(item)"
            @click="toggleVisible(item)"
        >
          <span class="series-dot" :style="{background: seriesDisplayColor(item.color)}"></span>
          <span class="truncate">{{ item.name }}</span>
          <span v-if="item.typeName" class="truncate text-xs opacity-60">[{{ item.typeName }}]</span>
          <span class="font-mono text-xs opacity-60">{{ latestText(item) }}</span>
        </button>
        <button
            v-if="legendMoreCount > 0"
            type="button"
            class="legend-more"
            @click="legendExpanded = true"
        >{{ translate('curve.legendShowMore', {count: legendMoreCount}) }}</button>
        <button
            v-if="legendExpanded && seriesList.length > 24"
            type="button"
            class="legend-more"
            @click="legendExpanded = false"
        >{{ translate('curve.legendCollapse') }}</button>
      </div>
    </section>

    <!-- FFT 频谱（简单 DFT） -->
    <section v-if="fftOpen && fftData" class="curve-card advanced-control fft-panel">
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="text-sm font-bold">{{ translate('curve.fftTitle') }}</span>
        <span class="truncate opacity-70">{{ fftData.name }}</span>
        <span class="font-mono opacity-70">fs={{ fftData.sampleRate.toFixed(2) }} Hz</span>
        <el-button size="small" link @click="fftOpen = false">×</el-button>
      </div>
      <table class="delta-table mt-1">
        <thead><tr><th>Hz</th><th>{{ translate('curve.fftMagnitude') }}</th></tr></thead>
        <tbody>
        <tr v-for="peak in fftData.peaks" :key="peak.hz">
          <td class="font-mono">{{ peak.hz.toFixed(3) }}</td>
          <td class="font-mono">{{ peak.magnitude.toFixed(4) }}</td>
        </tr>
        </tbody>
      </table>
    </section>

    <!-- 双光标 Δ / 统计 -->
    <section v-if="cursorDelta" class="curve-card delta-panel advanced-control" aria-live="polite">
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="font-semibold">{{ translate('curve.cursorDeltaTitle') }}</span>
        <span>Δt: <b class="font-mono">{{ cursorDelta.dt.toFixed(4) }} s</b></span>
        <span class="opacity-70">{{ translate('curve.cursorHint') }}</span>
        <el-button size="small" link @click="clearCursors">{{ translate('curve.cursorClear') }}</el-button>
      </div>
      <div class="delta-table-wrap mt-1">
        <table class="delta-table">
          <thead>
          <tr>
            <th>{{ translate('curve.colName') }}</th>
            <th>A</th>
            <th>B</th>
            <th>Δ</th>
            <th>min</th>
            <th>max</th>
            <th>avg</th>
          </tr>
          </thead>
          <tbody>
          <tr v-for="row in cursorDelta.rows" :key="row.name">
            <td><span class="series-dot" :style="{background: row.color}"></span> {{ row.name }}</td>
            <td class="font-mono">{{ row.aText }}</td>
            <td class="font-mono">{{ row.bText }}</td>
            <td class="font-mono">{{ row.delta }}</td>
            <td class="font-mono">{{ row.min }}</td>
            <td class="font-mono">{{ row.max }}</td>
            <td class="font-mono">{{ row.avg }}</td>
          </tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- Watch 列表（独立于曲线选择） -->
    <section v-if="watchList.length > 0" class="curve-card watch-panel advanced-control">
      <div class="text-xs font-semibold">{{ translate('curve.watchTitle') }}</div>
      <div class="watch-list mt-1">
        <div v-for="watch in watchList" :key="watch.key" class="watch-row">
          <span class="truncate">{{ watch.name }}</span>
          <span class="font-mono">{{ watchStats[watch.key]?.current ?? '-' }}</span>
          <span class="font-mono opacity-70">min {{ watchStats[watch.key]?.min ?? '-' }}</span>
          <span class="font-mono opacity-70">max {{ watchStats[watch.key]?.max ?? '-' }}</span>
          <span class="font-mono opacity-70">Δn {{ watchStats[watch.key]?.changes ?? 0 }}</span>
          <el-button link size="small" @click="toggleWatch(watch.key, watch.name)">×</el-button>
        </div>
      </div>
    </section>

    <!-- 录制 / 回放 -->
    <section class="curve-card record-panel advanced-control">
      <div class="flex flex-wrap items-center gap-2 text-xs">
        <span class="font-semibold">{{ translate('curve.recordTitle') }}</span>
        <el-button size="small" :disabled="!sampling || playbackMode" @click="startRecording">{{ translate('curve.recordStart') }}</el-button>
        <el-button size="small" :disabled="!recordingActive" @click="stopRecordingCapture">{{ translate('curve.recordStop') }}</el-button>
        <el-input v-model="recordingName" size="small" style="width: 10rem" :placeholder="translate('curve.recordName')"/>
        <el-button size="small" :disabled="recordRowCount === 0" @click="saveCurrentRecording">{{ translate('curve.recordSave') }}</el-button>
        <span class="opacity-70">{{ translate('curve.recordRows') }}: {{ recordRowCount }}</span>
        <el-select v-model="selectedRecordingId" size="small" style="width: 12rem" :placeholder="translate('curve.recordSelect')">
          <el-option v-for="recording in recordings" :key="recording.id" :value="recording.id" :label="`${recording.name} (${recording.rowCount})`"/>
        </el-select>
        <el-button size="small" :disabled="!selectedRecordingId" @click="loadRecordingById(selectedRecordingId)">{{ translate('curve.recordLoad') }}</el-button>
        <el-button size="small" :disabled="!selectedRecordingId" @click="deleteSelectedRecording">{{ translate('curve.recordDelete') }}</el-button>
        <el-button size="small" @click="refreshRecordings">{{ translate('curve.recordRefresh') }}</el-button>
        <el-button size="small" :disabled="!selectedRecordingId" @click="exportSelectedRecordingJson">{{ translate('curve.recordExportJson') }}</el-button>
        <el-button size="small" :disabled="!selectedRecordingId" @click="exportSelectedRecordingCsv">{{ translate('curve.recordExportCsv') }}</el-button>
        <el-button v-if="playbackMode" size="small" @click="exitPlayback">{{ translate('curve.playbackExit') }}</el-button>
      </div>
      <div v-if="playbackMode" class="mt-2 flex flex-wrap items-center gap-2 text-xs">
        <el-button size="small" @click="togglePlayback">{{ playbackPlaying ? translate('curve.playbackPause') : translate('curve.playbackPlay') }}</el-button>
        <label class="flex items-center gap-1">
          <span>{{ translate('curve.playbackSpeed') }}</span>
          <el-select v-model="playbackSpeed" size="small" style="width: 4.5rem">
            <el-option :value="0.5" label="0.5×"/>
            <el-option :value="1" label="1×"/>
            <el-option :value="2" label="2×"/>
            <el-option :value="4" label="4×"/>
          </el-select>
        </label>
        <input
            class="playback-range"
            type="range"
            min="0"
            :max="Math.max(0, playbackRows.length - 1)"
            :value="playbackIndex"
            @input="(event: Event) => applyPlaybackIndex(Number((event.target as HTMLInputElement).value))"
        />
        <span class="font-mono">{{ playbackIndex }}/{{ Math.max(0, playbackRows.length - 1) }}</span>
        <span class="opacity-70">{{ playbackRows[playbackIndex]?.time.toFixed(3) ?? '0.000' }} s</span>
      </div>
    </section>

    <!-- 诊断面板 -->
    <details class="curve-card diag-panel" :open="diagnosticsOpen" @toggle="(event: Event) => diagnosticsOpen = (event.target as HTMLDetailsElement).open">
      <summary class="cursor-pointer text-sm font-semibold">{{ translate('curve.diagTitle') }}</summary>
      <div class="mt-2 flex flex-wrap items-center gap-2 text-xs">
        <el-button size="small" @click="copyDiagnostics">{{ diagnosticsCopied ? translate('curve.diagCopied') : translate('curve.diagCopy') }}</el-button>
        <span class="opacity-70">{{ translate('curve.diagHint') }}</span>
      </div>
      <pre class="diag-json mt-2">{{ diagnosticsJson }}</pre>
    </details>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import {computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch} from 'vue'
import CurveFramesWorker from '../utils/curveFramesWorker?worker&inline'
import CurveChartWorker from '../utils/curveChartWorker?worker&inline'
import {ElMessageBox} from 'element-plus'
import {translate, translateRaw, type TranslationKeys} from '@/locales'
import {globalNotify} from '@/composables/notification'
import SourceBrowser from '@/components/source/SourceBrowser.vue'
import type {SourceBrowserLabels} from '@/components/source/labels'
import type {SourceLineAddressMap, SourceLineClickPayload, SourceLiveValueMap} from '@/utils/sourceProject'
import type {SourceActiveFileLine} from '@/components/source/lineTarget'
import {
  deleteRecording,
  listRecordings,
  loadRecording,
  newRecordingId,
  saveRecording,
  type CurveRecordingData,
  type RecordingMeta,
  type RecordingRow,
  type RecordingSeriesData,
} from '@/utils/curveRecording'
import type {SourceProject} from '@/components/source/projectLoader'
import type {SourceVariableSelectionPayload} from '@/utils/sourceProject'
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
  fileLineToAddress,
  findElfLineFiles,
  parseElfLineIndex,
  pcToLine,
  type ElfLineIndex,
  type ElfLineLocation,
} from '@/utils/elfLineIndex'
import {getTargetInfo, type TargetInfo} from '@/api/apiTarget'
import {
  allTargetProfiles,
  compactTargetProfileApi,
  compareDetectedToProfile,
  detectionLimitations,
  findKnownTargetProfile,
  parseTargetDetection,
  type CompactTargetProfile,
} from '@/utils/targetProfile'
import {parseElfImage} from '@/utils/firmwareImage'
import {
  buildFunctionSymbols,
  chooseBreakpointFunction,
  formatBreakpointAddress,
  formatFunctionOption,
  resolveBreakpointWithOffset,
  type FunctionSymbol,
} from '@/utils/elfFunctionSymbols'
import {
  CURVE_LOCAL_FLASH_END,
  CURVE_LOCAL_FLASH_START,
  CURVE_LOCAL_FRAMES_DEFAULT,
  CURVE_LOCAL_FRAMES_PATH,
  deviceApiUrl,
  decodeCurveFrames,
  CURVE_LOCAL_MAX_GROUPS,
  CURVE_LOCAL_MAX_SERIES_PER_GROUP,
  CURVE_LOCAL_MAX_TOTAL_SERIES,
  CurveLocalError,
  splitSeriesIntoGroups,
  type CurveLocalLocationVariant,
  getLocalCurveFrames,
  getLocalCurveStatus,
  parseBreakpointAddressInput,
  startLocalCurve,
  stopLocalCurve,
  type CurveConditionSpec,
  type CurveLocalFrame,
  type CurveLocalFramesPage,
  type CurveLocalWatchpointSpec,
  type CurveLocalSeriesSpec,
  type CurveLocalTrigger,
  type LocalCurveStatus,
} from '@/api/curveLocal'

type CurveFormat = DisplayFormat | 'u8' | 'i8' | 'u16' | 'i16' | 'hex' | 'bool' | 'enum'
const FORMATS: CurveFormat[] = ['auto', 'u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'f32', 'hex', 'bool', 'enum']
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
/** ELF 卡片视图：Variables（原有变量树/手动地址）/ Source（SourceBrowser 源码浏览）。 */
type ElfView = 'variables' | 'source'
/** 设备本地采样间隔（ms）；固件契约 5..200 ms。 */
const LOCAL_INTERVALS = [5, 10, 20, 50]
/** 新用户默认 20 ms（50 Hz）本地采样；保留 10 ms 选项。 */
const DEFAULT_LOCAL_INTERVAL_MS = 20
/** B 模式默认绘制上限；可在 32/64/128 间配置（A 模式忽略此上限）。 */
const DEFAULT_DRAW_CAP = 64
const DRAW_CAP_OPTIONS = [32, 64, 128]
/** Canvas 绘制节流：约 30fps；数据仍按采样率进入 ring。 */
const DRAW_INTERVAL_MS = 30
/** Y 自动缩放扫描预算：每帧/缓存刷新最多抽样 20k 个值（大数组时降采样）。 */
const Y_SCAN_BUDGET = 20000
const Y_RANGE_TTL_MS = 250
/** 图例默认展示上限，超过后折叠为 +N more。 */
const LEGEND_LIMIT = 24
/** 变量树首屏渲染上限（Show more 增量），避免一次性创建数千 DOM 行。 */
const VARIABLE_PAGE_SIZE = 300
/** 搜索时首屏更小（100），让过滤结果更快上屏（debounce 150ms + 渲染 <200ms）。 */
const SEARCH_PAGE_SIZE = 100
/** 已选面板默认只渲染前 64 行（批量选择 256 时避免挂载数百个 Element 组件），可 Show all。 */
const SELECTED_RENDER_LIMIT = 64
/** 采样统计 UI 刷新间隔（数据仍按采样率入 ring）；降低 Vue 全页 rerender 频率。 */
const STATS_FLUSH_MS = 1000
/** 本地采样计数器（frames/current group 等）UI 刷新间隔。 */
const LOCAL_UI_FLUSH_MS = 1000
/** 本地帧轮询周期（契约要求 20–50 ms）。 */
const LOCAL_POLL_MS = 20
const LOCAL_POLL_MAX_FRAMES = 64
/** 采样管线深度：允许同时在途的 WS-DAP 采样请求数（固件按序回包）。 */
const PIPELINE_DEPTH = 8
const AUTO_MIN_RATE = 10
const AUTO_MAX_RATE = 100
/** Auto 自适应窗口（毫秒）。 */
const AUTO_RATE_WINDOW_MS = 2500
/**
 * 环形缓冲总点数：曲线窗口仅 20s；多路本地 v2（≤256 系列）下每系列 lo/hi/values
 * 三个数组的内存 = points × 16B。6000 点 @100Hz ≈ 60s（本地 50Hz ≈ 120s），
 * 既覆盖图表窗口/CSV 导出，也把 256 路内存从 ~245MB 降到 ~24MB，减少 GC 卡顿。
 */
const MAX_POINTS = 6000
/** 曲线窗口（秒）：X 轴显示最近 20 秒，自动滚动。 */
const WINDOW_SECONDS = 20
const COLORS = [
  '#2563eb', '#dc2626', '#16a34a', '#d97706', '#7c3aed', '#0891b2',
  '#db2777', '#65a30d', '#4f46e5', '#ea580c', '#0d9488', '#9333ea',
  '#b91c1c', '#047857', '#a16207', '#1d4ed8', '#c026d3', '#475569',
  '#15803d', '#be123c', '#4338ca', '#0f766e', '#a21caf', '#92400e',
]

/**
 * 深色主题曲线显示色：把浅色调色板与白色混合提亮，保证曲线在近黑 canvas
 * 上可读。仅用于"显示"（canvas/图例/悬浮点），不改 series.color 状态与导出。
 */
const DARK_COLORS = COLORS.map((hex) => mixHexWithWhite(hex, 0.34))
const DARK_COLOR_BY_BASE = new Map(COLORS.map((color, index) => [color, DARK_COLORS[index]]))

function mixHexWithWhite(hex: string, ratio: number): string {
  const value = hex.replace('#', '')
  if (value.length !== 6) return hex
  const channel = (offset: number) => {
    const base = Number.parseInt(value.slice(offset, offset + 2), 16)
    const mixed = Math.round(base + (255 - base) * ratio)
    return Math.min(255, Math.max(0, mixed)).toString(16).padStart(2, '0')
  }
  return `#${channel(0)}${channel(2)}${channel(4)}`
}

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
  format: CurveFormat
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
  /** 所属函数的 DWARF PC 范围（本地采样下发 pc_low/pc_high，用于固件 PC 门控） */
  pcLow?: number
  pcHigh?: number
  /** loclist 多段位置：当前仅第一段（UI 标注限制） */
  loclistMulti?: boolean
  /** loclist 多段变体（协议 v2：随系列下发，固件按 PC 选择） */
  pcVariants?: CurveLocalLocationVariant[]
  /** 简单派生曲线：A+B / A-B（不参与采集，只用于显示/导出）。 */
  derived?: {a: number; b: number; op: 'add' | 'sub'}
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
const samplingSource = ref<SamplingSource>('local')
const isLocalSource = computed(() => samplingSource.value === 'local')
const localIntervalMs = ref(DEFAULT_LOCAL_INTERVAL_MS)
/** 当前来源允许的最大曲线数：设备本地随固件提升到 32，Web-DAP 仍为 24。 */
const seriesLimit = computed(() => (isLocalSource.value ? CURVE_LOCAL_MAX_TOTAL_SERIES : MAX_CURVE_SERIES))
/** 本地 v2：按每组 32 路自动分组（运行前按选择计划，运行后以固件 group_count 为准）。 */
const localPlannedGroups = computed(() => Math.max(1, Math.ceil(seriesList.value.length / CURVE_LOCAL_MAX_SERIES_PER_GROUP)))
const localActiveGroupCount = computed(() => (sampling.value && localGroupCount.value > 0 ? localGroupCount.value : localPlannedGroups.value))
/** 每组刷新率 = 1/(组数 × interval)；总帧率仍约 1/interval（每组轮转）。 */
const localGroupRefreshRate = computed(() => {
  const groups = localActiveGroupCount.value
  if (groups <= 0 || localIntervalMs.value <= 0) return 0
  return 1000 / (groups * localIntervalMs.value)
})
/** 可见曲线（勾选 legend/selected 的 visible）与 B 模式实际绘制列表。 */
const visibleSeriesList = computed(() => seriesList.value.filter((item) => item.visible))
/** 自适应 B cap：>128 条可见时自动降到 32（仍保留手动 32/64/128），提示用户切 A。 */
const effectiveDrawCap = computed(() => {
  if (renderAll.value) return Number.MAX_SAFE_INTEGER
  const visible = visibleSeriesList.value.length
  return visible > 128 ? Math.min(drawCap.value, 32) : drawCap.value
})
const autoCapReduced = computed(() => !renderAll.value && effectiveDrawCap.value < drawCap.value)
const drawnSeriesList = computed(() => (renderAll.value
  ? visibleSeriesList.value
  : visibleSeriesList.value.slice(0, effectiveDrawCap.value)))
const hiddenByCapCount = computed(() => Math.max(0, visibleSeriesList.value.length - drawnSeriesList.value.length))
const renderingLabel = computed(() => `${drawnSeriesList.value.length}/${visibleSeriesList.value.length}`)
/** 图例：默认 24 条，展开后全部。 */
const legendItems = computed(() => (legendExpanded.value ? seriesList.value : seriesList.value.slice(0, LEGEND_LIMIT)))
const legendMoreCount = computed(() => Math.max(0, seriesList.value.length - legendItems.value.length))
const shownSelectedSeries = computed(() => (selectedShowAll.value
  ? seriesList.value
  : seriesList.value.slice(0, SELECTED_RENDER_LIMIT)))
const remainingSelected = computed(() => Math.max(0, seriesList.value.length - shownSelectedSeries.value.length))
/** 时间轴位置文本：跟随时显示窗口；否则显示当前视图起止秒。 */
const timeAxisStatus = computed(() => {
  if (followLatest.value) return `${timeWindowSeconds.value}s · ${translate('curve.followLatest')}`
  const end = viewEndTime.value > 0 ? viewEndTime.value : lastDataTime.value
  const start = Math.max(0, end - timeWindowSeconds.value)
  return `${timeWindowSeconds.value}s · ${start.toFixed(1)}–${end.toFixed(1)}s`
})
/** 变量树首屏行数（Show more 增量），避免数千行 DOM 一次性渲染。 */
const shownVariableRows = computed(() => variableRows.value.slice(0, variableDisplayLimit.value))
const remainingVariableRows = computed(() => Math.max(0, variableRows.value.length - shownVariableRows.value.length))

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
  if (localTrigger.value === 'breakpoint') return breakpointResolution.value?.ok !== true
  if (localTrigger.value === 'watchpoint') return watchpointSpec.value === null
  if (conditionEnabled.value && conditionSpec.value === null) return true
  return false
})
/** 本地采样器是否已 start（用于保证 stop/卸载时 POST /api/curve/stop）。 */
const localActive = ref(false)
const localStatusInfo = shallowRef<LocalCurveStatus | null>(null)
const localFramesReceived = ref(0)
const localInvalidCount = ref(0)
const localOverwrittenCount = ref(0)
/** PC 门控：当前函数不在栈上、固件填 0 的累计次数（status.out_of_scope）。 */
const localOutOfScopeCount = ref(0)
const localGap = ref(false)
/** 协议 v2：固件 group 数 / 当前轮转 group（status/start 回读）。 */
const localGroupCount = ref(0)
const localCurrentGroup = ref<number | null>(null)
/** 本地选中的系列上限（v2 总 256；每组 32，自动切分）。 */
const localSeriesLimit = CURVE_LOCAL_MAX_TOTAL_SERIES
/** Auto 模式下当前实际使用的目标采样率（自动调整，UI 显示）。 */
const autoRate = ref(AUTO_MAX_RATE)
const globalFormat = ref<CurveFormat>('auto')
/** B/A 渲染模式：B（默认）只绘制前 drawCap 条可见曲线；A 绘制全部可见（降采样）。 */
const renderAll = ref(false)
const drawCap = ref(DEFAULT_DRAW_CAP)
/** task-44：滑动平均窗口（1=off，3/5/9）与 FFT 面板。 */
const smoothingWindow = ref(1)
const SMOOTHING_WINDOWS = [1, 3, 5, 9]
const fftOpen = ref(false)
const fftData = ref<{name: string; sampleRate: number; peaks: Array<{hz: number; magnitude: number}>} | null>(null)
const mobileAdvanced = ref(false)
const legendExpanded = ref(false)
const selectedCollapsed = ref(false)
const selectedShowAll = ref(false)
/** Y 轴：共享 auto（默认）/每变量归一化 0-100% / 对数。 */
type YAxisMode = 'auto' | 'normalized' | 'log'
const Y_AXIS_MODES: YAxisMode[] = ['auto', 'normalized', 'log']
const yAxisMode = ref<YAxisMode>('auto')
/** X 时间轴：窗口秒数、是否跟随最新、非跟随时视图右端。 */
const TIME_WINDOW_OPTIONS = [1, 2, 5, 10, 20, 60]
const timeWindowSeconds = ref(20)
const followLatest = ref(true)
const viewEndTime = ref(0)
/** 冻结显示：采样继续，图表暂停重绘（freeze/export 简化）。 */
const chartFrozen = ref(false)
/** 最近数据时间（用于时间轴状态显示，随 stats flush 更新）。 */
const lastDataTime = ref(0)
/** 数值缩放（格式设置，localStorage 持久化；不属于变量选择记忆）。 */
const valueMultiplier = ref(1)
const valueOffset = ref(0)
const VALUE_SCALE_STORAGE_KEY = 'curve.valueScale'

// ---- task-37：group weight / watch / 双光标 / 预设 / 诊断 / 录制回放 ----
/** 每组采样权重 1..8（默认1，sum(weight)≤64，契约 §9.7）。 */
const groupWeights = ref<number[]>([1])
/** task-35：当前帧 PC（active frame）——用于标记 pcLow/pcHigh 不含 PC 的变量为 stale。 */
const activePc = ref<number | null>(null)
let latestFramePc: number | undefined
/** task-42：ELF 行表索引（shallowRef 避免大对象深响应式）。 */
const lineIndex = shallowRef<ElfLineIndex | null>(null)
const lineIndexError = ref('')
/** PC 当前源码行/当前函数过滤。 */
const activeOnly = ref(false)
const currentFunctionOnly = ref(false)
/** 目标检测 / profile（显式选择，不自动推断）。 */
const targetRaw = shallowRef<TargetInfo | null>(null)
const targetLoading = ref(false)
const targetError = ref('')
const selectedTargetProfileId = ref('')
const elfFlashRange = shallowRef<[number, number] | null>(null)
const watchAddressInput = ref('')
const watchSize = ref<1 | 2 | 4>(4)
const watchAccess = ref<'write' | 'readwrite'>('write')
/** watchpoint 触发状态（task-38）。 */
const watchTriggerSeq = ref<number | null>(null)
const watchpointArmed = ref<boolean | null>(null)
const watchpointHits = ref<number | null>(null)
const watchFramesTriggered = ref(false)
const jumpFrames = ref(10)
/** task-45 条件触发 UI/状态（可与 periodic/breakpoint/watchpoint 组合）。 */
const conditionEnabled = ref(false)
const conditionAddressInput = ref('')
const conditionSize = ref<1 | 2 | 4>(4)
const conditionOp = ref<'==' | '!=' | '>' | '>=' | '<' | '<='>('<')
const conditionValue = ref(0)
const conditionSigned = ref(false)
const conditionMode = ref<'once' | 'continuous'>('once')
const conditionStopAfter = ref(true)
const conditionArmed = ref<boolean | null>(null)
const conditionMet = ref<boolean | null>(null)
const conditionHits = ref<number | null>(null)
const conditionTriggerSeq = ref<number | null>(null)
const conditionFramesTriggered = ref(false)
/** task-42：显式 ELF/目标固件一致性校验（本地 no-halt 读 Flash 首段首字）。 */
const elfVerifySegment = shallowRef<{address: number; bytes: Uint8Array} | null>(null)
const verifyState = ref<'idle' | 'running' | 'match' | 'mismatch' | 'error'>('idle')
const verifyDetail = ref('')
/** task-39：/ws/curve 实时推送（WS 优先，失败自动回退 HTTP）。 */
const CURVE_WS_PATH = '/ws/curve'
const wsTransport = ref<'http' | 'ws' | 'fallback'>('http')
const wsFrames = ref(0)
const wsDropped = ref(0)
const wsLatencyMs = ref(0)
let curveWs: WebSocket | null = null
let wsFallbackTimer = 0
let wsFirstMessageTimer = 0
let wsLastMessageAt = 0
let wsWindowStart = 0
let wsWindowFrames = 0
const PRESET_STORAGE_KEY = 'curve.presets'
interface PresetRecord {id: string; name: string; createdAt: number; series: RecordingSeriesData[]}
const presets = ref<PresetRecord[]>([])
const presetName = ref('')
const selectedPresetId = ref('')
/** Watch 列表独立于曲线选择：只记录 key，有对应已选曲线时展示统计。 */
const watchKeys = ref<string[]>([])
const watchStats = ref<Record<string, {current: string; min: string; max: string; changes: number}>>({})
/** 双光标（order 为 ring order 下标）+ Δ/统计面板。 */
const cursorA = ref<{index: number; time: number} | null>(null)
const cursorB = ref<{index: number; time: number} | null>(null)
const cursorPulse = ref(0)
/** 诊断面板。 */
const diagnosticsOpen = ref(false)
const diagnosticsCopied = ref(false)
const lastElfName = ref('')
const lastElfBytes = ref(0)
/** IndexedDB 录制/回放。 */
const recordings = ref<RecordingMeta[]>([])
const selectedRecordingId = ref('')
const recordingName = ref('')
const recordingActive = ref(false)
const recordRowCount = ref(0)
let recordRows: RecordingRow[] = []
let recordSnapshot: CurveSeries[] = []
let recordStartTimestamp = 0
const MAX_RECORD_ROWS = 6000
const playbackMode = ref(false)
const playbackRows = ref<RecordingRow[]>([])
const playbackSeries = ref<CurveSeries[]>([])
const playbackIndex = ref(0)
const playbackPlaying = ref(false)
const playbackSpeed = ref(1)
let playbackTimer = 0
let playbackWallStart = 0
let playbackRecordStart = 0
const seriesList = ref<CurveSeries[]>([])
const variableSearch = ref('')
/** 搜索框输入值；经 150ms debounce 后才驱动变量树过滤。 */
const variableSearchInput = ref('')
/** 变量树首屏/Show more 行数上限（过滤条件变化时重置为 300）。 */
const variableDisplayLimit = ref(VARIABLE_PAGE_SIZE)
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
/** ELF 卡片视图：Variables（原有变量树）或 Source（本地项目源码浏览）。 */
const elfView = ref<ElfView>('variables')
const elfViewOptions = computed(() => [
  {label: translate('curve.sourceViewVariables'), value: 'variables'},
  {label: translate('curve.sourceViewSource'), value: 'source'},
])
/** 源码项目：SourceBrowser 首次选目录后由 @project-loaded 回传；切换视图复用，不重复选目录。 */
const sourceProject = shallowRef<SourceProject | null>(null)
/** 实时值推送节拍：stats flush 时 +1，驱动 SourceBrowser liveValues 更新（低频）。 */
const sourceLivePulse = ref(0)
/** SourceBrowser 当前高亮变量 key（点击源码变量/曲线时更新）。 */
const activeSourceKey = ref<string | null>(null)
/**
 * SourceBrowser labels：组件本身不 import i18n（避免契约冲突），页面把全部字段按当前语言注入。
 * 注意：必须保持与 `SourceBrowserLabels`（components/source/labels.ts）字段一字不差；
 * 这里使用显式 key（契约 §2.2 禁止动态拼接/变量 key），由 vue-tsc 的 Record 类型兜底。
 */
const sourceBrowserLabels = computed<SourceBrowserLabels>(() => ({
  openProject: translateRaw('curve.sourceOpenProject'),
  changeFolder: translateRaw('curve.sourceChangeFolder'),
  chooseElf: translateRaw('curve.sourceChooseElf'),
  secureContextHint: translateRaw('curve.sourceSecureContextHint'),
  privacyNote: translateRaw('curve.sourcePrivacyNote'),
  projectRoot: translateRaw('curve.sourceProjectRoot'),
  filesCount: translateRaw('curve.sourceFilesCount'),
  matchedSummary: translateRaw('curve.sourceMatchedSummary'),
  unmatchedSummary: translateRaw('curve.sourceUnmatchedSummary'),
  ambiguousSummary: translateRaw('curve.sourceAmbiguousSummary'),
  scanProgress: translateRaw('curve.sourceScanProgress'),
  limitsHint: translateRaw('curve.sourceLimitsHint'),
  limitsHintDefault: translateRaw('curve.sourceLimitsHintDefault'),
  elfDetected: translateRaw('curve.sourceElfDetected'),
  elfTime: translateRaw('curve.sourceElfTime'),
  useElf: translateRaw('curve.sourceUseElf'),
  elfNone: translateRaw('curve.sourceElfNone'),
  elfReadFailed: translateRaw('curve.sourceElfReadFailed'),
  elfMissing: translateRaw('curve.sourceElfMissing'),
  projectError: translateRaw('curve.sourceProjectError'),
  noProjectTitle: translateRaw('curve.sourceNoProjectTitle'),
  noProjectBody: translateRaw('curve.sourceNoProjectBody'),
  noSourceFiles: translateRaw('curve.sourceNoSourceFiles'),
  noSourceFilesHint: translateRaw('curve.sourceNoSourceFilesHint'),
  searchPlaceholder: translateRaw('curve.sourceSearchPlaceholder'),
  noSearchResults: translateRaw('curve.sourceNoSearchResults'),
  searchResultsCount: translateRaw('curve.sourceSearchResultsCount'),
  unmatchedTitle: translateRaw('curve.sourceUnmatchedTitle'),
  unmatchedHint: translateRaw('curve.sourceUnmatchedHint'),
  showMore: translateRaw('curve.sourceShowMore'),
  showLess: translateRaw('curve.sourceShowLess'),
  fileVariables: translateRaw('curve.sourceFileVariables'),
  selectFileTitle: translateRaw('curve.sourceSelectFileTitle'),
  selectFileHint: translateRaw('curve.sourceSelectFileHint'),
  readingFile: translateRaw('curve.sourceReadingFile'),
  noMatchInFile: translateRaw('curve.sourceNoMatchInFile'),
  readFailed: translateRaw('curve.sourceReadFailed'),
  retryRead: translateRaw('curve.sourceRetryRead'),
  jumpToVariable: translateRaw('curve.sourceJumpToVariable'),
  prevVariable: translateRaw('curve.sourcePrevVariable'),
  nextVariable: translateRaw('curve.sourceNextVariable'),
  candidateTitle: translateRaw('curve.sourceCandidateTitle'),
  candidateLine: translateRaw('curve.sourceCandidateLine'),
  candidateFile: translateRaw('curve.sourceCandidateFile'),
  candidateProject: translateRaw('curve.sourceCandidateProject'),
  sampleable: translateRaw('curve.sourceSampleable'),
  unsampleable: translateRaw('curve.sourceUnsampleable'),
  typeLabel: translateRaw('curve.sourceTypeLabel'),
  fileLine: translateRaw('curve.sourceFileLine'),
  hoverMore: translateRaw('curve.sourceHoverMore'),
  noVariables: translateRaw('curve.sourceNoVariables'),
  noVariablesHint: translateRaw('curve.sourceNoVariablesHint'),
  loading: translateRaw('curve.sourceLoading'),
  openProjectFallback: translateRaw('curve.sourceOpenProjectFallback'),
  lineLabel: translateRaw('curve.sourceLineLabel'),
  onlyFilesWithVariables: translateRaw('curve.sourceOnlyFilesWithVariables'),
  showAllProjectFiles: translateRaw('curve.sourceShowAllProjectFiles'),
  noFilesWithVariables: translateRaw('curve.sourceNoFilesWithVariables'),
  liveValueStale: translateRaw('curve.sourceLiveValueStale'),
  breakpointLineHint: translateRaw('curve.sourceBreakpointLineHint'),
  liveValues: translateRaw('curve.sourceLiveValues'),
  activePcLine: translateRaw('curve.sourceActivePcLine'),
  lineNoAddressHint: translateRaw('curve.sourceLineNoAddressHint'),
  close: translateRaw('curve.sourceClose'),
}))


/** 源码视图中高亮已选变量：key 与 CurveSeries.key 同语义（path@address）。 */
const sourceSelectedKeys = computed<string[]>(() => seriesList.value.map((item) => item.key))
/**
 * Source 视图是否曾打开：首次切换到 Source 时才挂载 SourceBrowser（减少无关 DOM/文件输入，
 * 也避免影响只做变量选择的既有流程）；挂载后保持（v-show），切换视图不丢浏览状态。
 */
const sourceViewEverOpened = ref(false)
const sourceBrowserRef = ref<{
  scrollToVariableByKey(key: string): Promise<boolean>
  openFile?(file: string): Promise<void>
  scrollToLine?(line: number, highlight?: boolean): void
} | null>(null)
/** task-44：PC 当前行自动联动 Source（可开关，避免抢用户滚动）。 */
const autoPcSource = ref(true)
watch(elfView, (view) => {
  if (view === 'source') sourceViewEverOpened.value = true
}, {immediate: true})
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

/**
 * 深色主题状态（纯视觉）：主题切换只改 <html class/data-theme>，由 MutationObserver
 * 同步本 ref 并触发 canvas 调色板失效 + 重绘；同时让图例/工具底色点响应式更新。
 */
const darkTheme = ref(document.documentElement.classList.contains('dark'))
/** Canvas 调色板缓存：只在主题切换时失效，避免每帧 getComputedStyle。 */
let chartPaletteCache: { bg: string; grid: string; text: string; axis: string } | null = null
let themeObserver: MutationObserver | null = null

let client: CmsisDapClient | null = null
let transport: WsDapTransport | null = null
let nextSeriesId = 1
let sampleTimer = 0
let debugTimer = 0
let debugRunId = 0
let debugInFlight = false
// 设备本地采样状态
let localRunId = 0
/** 固定节拍轮询定时器 + 独立状态刷新/绘制定时器（task-33）。 */
let localPollTimer = 0
let localStatusTimer = 0
let localDrawLoopTimer = 0
/** 同时在途的 frames 请求数（允许 2–3 个，提升有效取帧率）。 */
const LOCAL_POLL_MAX_IN_FLIGHT = 3
let localPollsInFlightCount = 0
let localPollError = false
/** 乱序/重复帧缓冲：seq -> frame（按 seq 连续出队，gap 时排序重同步）。 */
let localFrameBuffer = new Map<number, {frame: CurveLocalFrame; version: number}>()
let localPollCounter = 0
let localSince = 0
let localBaseTimestampUs = 0
let localLastPollAt = 0
let localRateTimestamps: number[] = []
/** 本地运行时的系列顺序（carry values 对齐图表 series）。 */
let localSeriesOrder: CurveSeries[] = []
/** 协议 v2 group 映射：group id -> 该组内 series 在 localSeriesOrder 中的下标与字节数。 */
let localGroupEntries: Array<Array<{index: number; item: CurveSeries; size: number}>> = []
/** 最近一次各 series 的原始字节（多组轮转时未采样组沿用上一次值，避免假 0）。 */
let localLatestValues = new Map<number, Uint8Array>()
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
let lastDrawAt = 0
let chartDirty = true
let searchTimer = 0
let hoverDrawPending = false
/** 可选 debug 计数：window.__curveDebug()，供 E2E 性能断言。 */
const perfCounter = {draws: 0, lastDrawMs: 0, drawMsTotal: 0, hiddenPauses: 0}
/** task-33 取帧吞吐统计：完成响应数/滑动窗口。 */
const pollCounterState = {completions: 0, windowStart: 0, windowCompletions: 0, pollsPerSecond: 0}
/** Y 范围缓存 + dirty flag（避免每帧全量扫描）。 */
let yRangeCache = {min: 0, max: 1, updatedAt: 0, seriesCount: -1, dirty: true}
const normalizedRangeCache = new Map<number, {min: number; max: number; updatedAt: number; count: number}>()
/** 最近一次绘制几何，供 hover overlay 使用（不重建 canvas/不重绘主图）。 */
let lastGeometry: {
  padding: {left: number; right: number; top: number; bottom: number}
  plotWidth: number; plotHeight: number
  xMin: number; xMax: number
  minValue: number; maxValue: number
  yAxisMode: YAxisMode
  normalizedRanges?: Map<number, {min: number; max: number}>
  width: number; height: number
} | null = null
let hoverCanvasRef = ref<HTMLCanvasElement | null>(null)
/** 采样统计的 UI 刷新节流：采样本身在 100Hz 完成，但 Vue 响应式/DOM 更新不必
 *  每个点都触发（否则主线程频繁重渲染会造成采样节拍抖动和管线积压）。 */
let statsTimer = 0
let lastStatsAt = 0
let droppedPending = 0
// 本地采样 UI 计数器节流：poll 里先累积，每 200ms 一次性写入 reactive refs，
// 避免 30~50Hz 的 frames/currentGroup 更新触发整个页面（300 变量行+100 已选行）rerender。
let localUiTimer = 0
let localPendingFrames = 0
let localPendingInvalid = 0
let localPendingOverwritten = 0
let localPendingGroup: number | null = null

// 环形缓冲：timestamps/slotSeq 按写入顺序共享；每个曲线存 64-bit 原始值的
// 低/高 32-bit（支持 DWARF 的 1/2/4/8 字节 + 位域）。
const timestamps = new Float64Array(MAX_POINTS)
const slotSeq = new Float64Array(MAX_POINTS)
/** 每点所属本地 group（CSV 导出组/时间戳；CVF1/Web 恒 0）。 */
const slotGroup = new Uint8Array(MAX_POINTS)
/** task-42：每个 ring 点对应的设备帧 seq（watchpoint 跳转 trigger_seq 用）。 */
const slotDeviceSeq = new Float64Array(MAX_POINTS)
const seriesLo = new Map<number, Uint32Array>()
const seriesHi = new Map<number, Uint32Array>()
/** 每系列解码后的数值缓存（Float64Array），绘制/缩放直接读，避免每帧重复 decode。 */
const seriesValues = new Map<number, Float64Array>()
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
  const remain = Math.max(0, STATS_FLUSH_MS - (now - lastStatsAt))
  statsTimer = window.setTimeout(() => {
    statsTimer = 0
    flushStats()
  }, remain)
}

function scheduleLocalUiFlush(): void {
  if (localUiTimer) return
  localUiTimer = window.setTimeout(() => {
    localUiTimer = 0
    if (localPendingFrames > 0) {
      const next = localFramesReceived.value + localPendingFrames
      if (next !== localFramesReceived.value) localFramesReceived.value = next
    }
    if (localPendingInvalid > 0) {
      const next = localInvalidCount.value + localPendingInvalid
      if (next !== localInvalidCount.value) localInvalidCount.value = next
    }
    if (localPendingOverwritten > 0) {
      const next = localOverwrittenCount.value + localPendingOverwritten
      if (next !== localOverwrittenCount.value) localOverwrittenCount.value = next
    }
    if (localPendingGroup !== null && localCurrentGroup.value !== localPendingGroup) localCurrentGroup.value = localPendingGroup
    localPendingFrames = 0
    localPendingInvalid = 0
    localPendingOverwritten = 0
    localPendingGroup = null
  }, LOCAL_UI_FLUSH_MS)
}

function flushStats(): void {
  lastStatsAt = performance.now()
  if (sampleCount.value !== ringCount) sampleCount.value = ringCount
  const dataTime = lastDataTimestamp()
  if (lastDataTime.value !== dataTime) lastDataTime.value = dataTime
  sourceLivePulse.value += 1
  if (latestFramePc !== undefined && activePc.value !== latestFramePc) {
    activePc.value = latestFramePc
  }
  if (watchKeys.value.length > 0) updateWatchStats()
  if (cursorA.value || cursorB.value) cursorPulse.value += 1
  if (seriesList.value.some((item) => item.derived)) updateDerivedValues()
  if (isLocalSource.value) {
    const rate = localRateTimestamps.length
    if (effectiveRate.value !== rate) effectiveRate.value = rate
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

/** 按 sizes 拆一帧 payload；长度不符返回 null（跳过该帧）。 */
function splitPayloadBytes(payload: Uint8Array, sizes: readonly number[]): Uint8Array[] | null {
  const expected = sizes.reduce((sum, size) => sum + size, 0)
  if (payload.length < expected) return null
  const values: Uint8Array[] = []
  let offset = 0
  for (const size of sizes) {
    values.push(payload.slice(offset, offset + size))
    offset += size
  }
  return values
}

/** 构建本地 series spec：loclist 多段发 variants（v2），否则 expr +（仅寄存器表达式）PC 门控。 */
function buildLocalSeriesSpec(item: CurveSeries, id: number, index: number): {spec: CurveLocalSeriesSpec; entry: {index: number; item: CurveSeries; size: number; needsHalt: boolean}} | null {
  const size = Math.max(1, Math.min(8, item.sample.size))
  const variants = item.pcVariants?.filter((variant) => validPcRange(variant.pcLow, variant.pcHigh)) ?? []
  const expr = item.location
    ?? (item.sample.address ? {kind: 'addr' as const, address: item.sample.address >>> 0} : null)
  if (variants.length === 0 && !expr) return null
  const exprNeedsRegisters = variants.length > 0
    ? variants.some((variant) => addressExprRegisters(variant.expr).size > 0)
    : addressExprRegisters(expr!).size > 0
  const spec: CurveLocalSeriesSpec = {id, size}
  if (variants.length > 0) {
    // 契约 §9.4：多段 loclist 发送全部 variants，由固件按当前 PC 选择。
    spec.variants = variants
  } else {
    spec.expr = expr!
    // Lead 修复：仅依赖寄存器的栈帧表达式做 PC 门控；函数内 static 固定地址不门控。
    const pcRange = exprNeedsRegisters ? validPcRange(item.pcLow, item.pcHigh) : null
    if (pcRange) {
      spec.pcLow = pcRange.pcLow
      spec.pcHigh = pcRange.pcHigh
    }
  }
  return {spec, entry: {index, item, size, needsHalt: exprNeedsRegisters}}
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
  localOutOfScopeCount.value = status.outOfScope ?? 0
  if (status.groupCount !== undefined && status.groupCount > 0) localGroupCount.value = status.groupCount
  if (status.currentGroup !== undefined) localCurrentGroup.value = status.currentGroup
  watchpointArmed.value = status.watchpointArmed ?? null
  watchpointHits.value = status.watchpointHits ?? null
  if (status.triggerSeq !== undefined) watchTriggerSeq.value = status.triggerSeq
  conditionArmed.value = status.conditionArmed ?? null
  conditionMet.value = status.conditionMet ?? null
  conditionHits.value = status.conditionHits ?? null
  if (status.conditionTriggerSeq !== undefined) conditionTriggerSeq.value = status.conditionTriggerSeq
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
  latestFramePc = undefined
  clearLocalPollTimers()
  stopCurveWs()
  localPollsInFlightCount = 0
  localFrameBuffer.clear()
  if (wasActive) void postLocalStop()
  flushStats()
  scheduleDraw()
}

async function startLocalSampling(): Promise<void> {
  if (sampling.value || seriesList.value.length === 0) return
  if (connected.value || connecting.value) {
    await onDisconnect()
  }
  const snapshot = seriesList.value.filter((item) => !item.derived)
  if (snapshot.length === 0) {
    notice.value = {type: 'error', title: translate('curve.derivedNeedTwo')}
    return
  }
  // v2：自动按每组 32 路切分（最多 8 组/256 路）；超出明确提示，不伪造。
  const split = splitSeriesIntoGroups(snapshot, CURVE_LOCAL_MAX_SERIES_PER_GROUP, CURVE_LOCAL_MAX_GROUPS)
  if (split.error === 'empty') return
  if (split.error === 'too_many_series') {
    notice.value = {type: 'error', title: translate('curve.localGroupLimit', {count: CURVE_LOCAL_MAX_TOTAL_SERIES})}
    return
  }
  const groupSpecs: Array<{id: number; weight: number; series: CurveLocalSeriesSpec[]}> = []
  ensureGroupWeights(split.groupCount)
  const groupEntries: Array<Array<{index: number; item: CurveSeries; size: number}>> = []
  let flatIndex = 0
  let needsHalt = false
  let hasVariants = false
  for (const group of split.groups) {
    const series: CurveLocalSeriesSpec[] = []
    const entries: Array<{index: number; item: CurveSeries; size: number}> = []
    for (const item of group) {
      const built = buildLocalSeriesSpec(item, series.length, flatIndex)
      if (!built) {
        notice.value = {type: 'error', title: translate('curve.localRegisterUnsupported')}
        return
      }
      series.push(built.spec)
      entries.push({index: built.entry.index, item: built.entry.item, size: built.entry.size})
      if (built.entry.needsHalt) needsHalt = true
      if ((built.spec.variants?.length ?? 0) > 0) hasVariants = true
      flatIndex += 1
    }
    groupSpecs.push({id: groupSpecs.length, weight: groupWeights.value[groupSpecs.length] ?? 1, series})
    groupEntries.push(entries)
  }
  statusError.value = false
  notice.value = null
  const trigger = localTrigger.value
  let breakpointAddress: number | undefined
  let watchpoint: CurveLocalWatchpointSpec | undefined
  if (trigger === 'breakpoint') {
    const resolution = breakpointResolution.value
    if (!resolution || !resolution.ok) {
      notice.value = {type: 'error', title: translate('curve.localBreakpointInvalid')}
      return
    }
    breakpointAddress = resolution.address
  } else if (trigger === 'watchpoint') {
    if (!watchpointSpec.value) {
      notice.value = {type: 'error', title: translate('curve.watchpointInvalid')}
      return
    }
    watchpoint = watchpointSpec.value
  }
  if (conditionEnabled.value && !conditionSpec.value) {
    notice.value = {type: 'error', title: translate('curve.conditionInvalid')}
    return
  }
  if (needsHalt && localIntervalMs.value < 5) {
    notice.value = {type: 'error', title: translate('curve.localHaltIntervalTooFast')}
    return
  }
  const requestBase = {
    intervalUs: localIntervalMs.value * 1000,
    trigger,
    breakpointAddress,
    watchpoint,
    triggerCondition: conditionEnabled.value ? (conditionSpec.value ?? undefined) : undefined,
    // variants 必须 halt（契约 §9.4）；固定地址仅在有寄存器表达式时 halt。breakpoint 忽略 halt。
    halt: trigger === 'periodic' ? (hasVariants ? true : needsHalt) : undefined,
  }
  let started
  const startOnce = () => (groupSpecs.length === 1 && !hasVariants
    // 单组且无 variants：旧 root-series 请求（兼容旧固件）；多组/variants：v2 groups 请求。
    ? startLocalCurve({...requestBase, series: groupSpecs[0].series})
    : startLocalCurve({...requestBase, groups: groupSpecs}))
  try {
    started = await startOnce()
  } catch (error) {
    // ESP32 在刚 OTA/WebSocket 缓冲紧张时偶发 cJSON 解析失败（invalid JSON object）；
    // 短暂让出后重试一次，避免用户看到无意义的 400。
    const transient = /invalid JSON object|HTTP 5\d\d/i.test(localErrorText(error))
    if (transient) {
      await new Promise((resolve) => window.setTimeout(resolve, 350))
      try {
        started = await startOnce()
      } catch (retryError) {
        statusError.value = true
        notice.value = {type: 'error', title: translate('curve.localStartFailed', {err: localErrorText(retryError)})}
        return
      }
    } else {
      statusError.value = true
      notice.value = {type: 'error', title: translate('curve.localStartFailed', {err: localErrorText(error)})}
      return
    }
  }
  // 设备时间戳与既有 ring 时间轴不同源：清空旧数据，按帧时间戳重新累积。
  clearData()
  failedCount.value = 0
  localSeriesOrder = snapshot
  localGroupEntries = groupEntries
  localLatestValues = new Map()
  localGroupCount.value = started.groupCount > 0 ? started.groupCount : groupEntries.length
  localCurrentGroup.value = null
  localSince = 0
  localBaseTimestampUs = 0
  localPollCounter = 0
  localPollsInFlightCount = 0
  localFrameBuffer.clear()
  localPollError = false
  latestFramePc = undefined
  activePc.value = null
  watchTriggerSeq.value = null
  watchpointArmed.value = null
  watchpointHits.value = null
  watchFramesTriggered.value = false
  conditionArmed.value = null
  conditionMet.value = null
  conditionHits.value = null
  conditionTriggerSeq.value = null
  conditionFramesTriggered.value = false
  wsTransport.value = 'http'
  wsFrames.value = 0
  wsDropped.value = 0
  wsLatencyMs.value = 0
  wsLastMessageAt = 0
  wsWindowStart = 0
  wsWindowFrames = 0
  localRateTimestamps = []
  localFramesReceived.value = 0
  localInvalidCount.value = 0
  localOverwrittenCount.value = 0
  localPendingFrames = 0
  localPendingInvalid = 0
  localPendingOverwritten = 0
  localPendingGroup = null
  if (localUiTimer) { window.clearTimeout(localUiTimer); localUiTimer = 0 }
  localHits.value = null
  localSampled.value = null
  localOutOfScopeCount.value = 0
  localGap.value = false
  localStatusInfo.value = null
  localActive.value = true
  sampling.value = true
  everSampled.value = true
  localLastPollAt = performance.now()
  const runId = ++localRunId
  startLocalPolling(runId)
  startSamplingDrawLoop()
  scheduleDraw()
}

function clearLocalPollTimers(): void {
  if (localPollTimer) { window.clearInterval(localPollTimer); localPollTimer = 0 }
  if (localStatusTimer) { window.clearInterval(localStatusTimer); localStatusTimer = 0 }
  if (localDrawLoopTimer) { window.clearInterval(localDrawLoopTimer); localDrawLoopTimer = 0 }
}

/** 固定节拍轮询（20ms）+ 最多 3 个请求在途：取帧率不再被单次 60ms+ 串行延迟限制。 */
function startLocalPolling(runId: number): void {
  clearLocalPollTimers()
  localPollError = false
  localStatusTimer = window.setInterval(() => {
    if (!sampling.value || runId !== localRunId || localPollError) return
    void refreshLocalStatus().catch(() => { /* 状态刷新失败不终止采样 */ })
  }, 1000)
  startSamplingDrawLoop()
  startHttpPollingOnly(runId)
  connectCurveWs(runId)
}

/** 采样期间固定 30fps 视觉刷新（数据未到也重绘脏视图，hover/时间轴保持流畅）。 */
function startSamplingDrawLoop(): void {
  if (localDrawLoopTimer) return
  localDrawLoopTimer = window.setInterval(() => {
    if (sampling.value && !chartFrozen.value) scheduleDraw()
  }, 33)
}

function issueLocalPoll(runId: number): void {
  if (!sampling.value || runId !== localRunId || !localActive.value || localPollError) return
  if (localPollsInFlightCount >= LOCAL_POLL_MAX_IN_FLIGHT) return
  const since = localSince
  localPollsInFlightCount += 1
  void requestLocalFrames(since)
    .then((page) => {
      if (!sampling.value || runId !== localRunId) return
      handleLocalFramesPage(page)
    })
    .catch((error: unknown) => {
      if (!sampling.value || runId !== localRunId || localPollError) return
      localPollError = true
      notice.value = {type: 'error', title: translate('curve.localPollFailed', {err: localErrorText(error)})}
      statusError.value = true
      stopLocalSampling()
    })
    .finally(() => {
      localPollsInFlightCount = Math.max(0, localPollsInFlightCount - 1)
    })
}

/** 单帧映射：group payload -> series ring（CVF1 单组/CVF2 按 frame.group）。 */
function applyLocalFrame(frame: CurveLocalFrame, version: number): boolean {
  if (frame.pc !== undefined) latestFramePc = frame.pc
  if (localBaseTimestampUs === 0) localBaseTimestampUs = frame.timestampUs
  const groupIndex = version >= 2 ? frame.group : 0
  const groupEntries = localGroupEntries[groupIndex]
  if (!groupEntries) {
    localPendingInvalid += 1
    return false
  }
  const parts = splitPayloadBytes(frame.payload, groupEntries.map((entry) => entry.size))
  if (!parts) {
    localPendingInvalid += 1
    return false
  }
  groupEntries.forEach((entry, slot) => {
    localLatestValues.set(entry.item.id, parts[slot] ?? new Uint8Array(entry.size))
  })
  // 多组轮转：本轮未采样的组沿用最近值，保持图表连续（不写假 0）。
  const rawValues = localSeriesOrder.map((item) => {
    const latest = localLatestValues.get(item.id)
    return latest ?? new Uint8Array(Math.max(1, Math.min(8, item.sample.size)))
  })
  const seconds = (frame.timestampUs - localBaseTimestampUs) / 1e6
  pushSample(seconds, rawValues, localSeriesOrder, groupIndex, frame.seq)
  localRateTimestamps.push(seconds)
  trimLocalRateWindow(seconds)
  localPendingGroup = groupIndex
  return true
}

/**
 * 处理单次 frames 响应：按 seq 去重/排序；正常连续出队，检测到 gap 时排序重同步。
 * 乱序响应只入 buffer，按 localSince+1 连续应用，避免前跳/重复。
 */
function handleLocalFramesPage(page: CurveLocalFramesPage): void {
  localPollCounter += 1
  if (page.triggered) {
    watchFramesTriggered.value = true
    void refreshLocalStatus()
  }
  if (page.conditionTriggered) {
    conditionFramesTriggered.value = true
    void refreshLocalStatus()
  }
  const now = performance.now()
  if (pollCounterState.windowStart === 0) pollCounterState.windowStart = now
  pollCounterState.completions += 1
  pollCounterState.windowCompletions += 1
  if (now - pollCounterState.windowStart >= 1000) {
    pollCounterState.pollsPerSecond = pollCounterState.windowCompletions * 1000 / (now - pollCounterState.windowStart)
    pollCounterState.windowStart = now
    pollCounterState.windowCompletions = 0
  }
  if (page.frames.length === 0) return
  if (page.gap || (localFrameBuffer.size === 0 && localSince > 0 && page.frames[0].seq > localSince + 1)) {
    localGap.value = true
  }
  for (const frame of page.frames) {
    if (frame.seq <= localSince) continue
    localFrameBuffer.set(frame.seq, {frame, version: page.version})
  }
  const buffered = [...localFrameBuffer.values()].sort((left, right) => left.frame.seq - right.frame.seq)
  const firstSeq = buffered[0]?.frame.seq
  if (localGap.value || (firstSeq !== undefined && firstSeq > localSince + 1)) {
    // ring 已覆盖旧帧：按序应用现有帧并重同步 seq（允许缺口）。
    localGap.value = true
    let applied = 0
    for (const entry of buffered) {
      if (entry.frame.seq <= localSince) continue
      if (applyLocalFrame(entry.frame, entry.version)) applied += 1
      localSince = entry.frame.seq
    }
    localFrameBuffer.clear()
    if (applied > 0) {
      localPendingFrames += applied
      scheduleLocalUiFlush()
      scheduleDraw()
    }
    return
  }
  let applied = 0
  while (localFrameBuffer.has(localSince + 1)) {
    const entry = localFrameBuffer.get(localSince + 1)
    localFrameBuffer.delete(localSince + 1)
    if (!entry) break
    if (applyLocalFrame(entry.frame, entry.version)) applied += 1
    localSince += 1
  }
  if (applied > 0) {
    localPendingFrames += applied
    scheduleLocalUiFlush()
    scheduleDraw()
  }
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
    localOutOfScopeCount.value = 0
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

function pushSample(time: number, rawValues: Uint8Array[], snapshot: readonly CurveSeries[], group = 0, deviceSeq = 0): void {
  const slot = ringHead
  timestamps[slot] = time
  slotSeq[slot] = sampleSeq
  slotGroup[slot] = group & 0xff
  slotDeviceSeq[slot] = deviceSeq
  for (let i = 0; i < snapshot.length; i++) {
    const item = snapshot[i]
    const loBuffer = seriesLo.get(item.id)
    const hiBuffer = seriesHi.get(item.id)
    if (!loBuffer || !hiBuffer) continue
    const bits = extractSampleBits(rawValues[i] ?? new Uint8Array(item.sample.size), item.sample)
    loBuffer[slot] = bits.lo
    hiBuffer[slot] = bits.hi
    // 数据写入时解码一次缓存到 Float64Array；绘制/缩放不再每帧 decode。
    const values = seriesValues.get(item.id)
    if (values) values[slot] = decodeNumber(item, bits.lo, bits.hi)
  }
  ringHead = (ringHead + 1) % MAX_POINTS
  if (ringCount < MAX_POINTS) ringCount += 1
  sampleSeq += 1
  if (recordingActive.value && !playbackMode.value) {
    if (recordRowCount.value >= MAX_RECORD_ROWS) {
      recordingActive.value = false
      globalNotify(translate('curve.recordLimit', {count: MAX_RECORD_ROWS}), 'warning')
    } else {
      if (recordSnapshot.length === 0) recordSnapshot = snapshot.slice()
      recordRows.push({time, group, payloads: rawValues.map((value) => new Uint8Array(value)), seq: deviceSeq, pc: latestFramePc})
      recordRowCount.value += 1
    }
  }
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
  invalidateYRange()
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

/** 每个变量节点的一次性小写搜索索引（避免每次搜索对 10 个字段重复 toLowerCase）。 */
const nodeSearchIndex = new WeakMap<VariableNode, string>()

function nodeSearchText(node: VariableNode): string {
  let text = nodeSearchIndex.get(node)
  if (text !== undefined) return text
  const fields = [
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
  text = fields.filter((value) => !!value).join('\n').toLowerCase()
  nodeSearchIndex.set(node, text)
  return text
}

function prewarmSearchNode(node: VariableNode): void {
  nodeSearchText(node)
  if (node.children) for (const child of node.children) prewarmSearchNode(child)
}

/** ELF 解析完成后空闲预热搜索索引，避免第一次输入时对 2 万+节点现建缓存。 */
function prewarmSearchIndex(info: ElfVariablesResult): void {
  window.setTimeout(() => {
    if (elfInfo.value !== info) return
    for (const node of info.variables) prewarmSearchNode(node)
  }, 0)
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

function nodeMatchesDeep(node: VariableNode, needle: string): boolean {
  if (nodeSearchText(node).includes(needle)) return true
  if (!node.children) return false
  return node.children.some((child) => nodeMatchesDeep(child, needle))
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

/** 变量所属函数的 PC 范围是否有效且落在目标 Flash（用于下发 PC 门控）。 */
function validPcRange(low?: number, high?: number): {pcLow: number; pcHigh: number} | null {
  if (typeof low !== 'number' || typeof high !== 'number') return null
  if (!Number.isFinite(low) || !Number.isFinite(high)) return null
  const pcLow = low >>> 0
  const pcHigh = high >>> 0
  if (pcLow < CURVE_LOCAL_FLASH_START || pcHigh > CURVE_LOCAL_FLASH_END || pcHigh <= pcLow) return null
  return {pcLow, pcHigh}
}

/** 寄存器值变量（DW_OP_reg）暂不支持设备本地采样。 */
function isLocalRegisterValue(node: VariableNode): boolean {
  return node.sample?.register !== undefined
}

const variableRows = computed<VariableRow[]>(() => {
  const info = elfInfo.value
  if (!info) return []
  const query = variableSearch.value.trim().toLowerCase()
  const rows: VariableRow[] = []
  const visit = (node: VariableNode, depth: number, parentPath: string, bucket: VariableRow[], ancestorMatched: boolean) => {
    const path = depth === 0 ? node.name : parentPath + node.name
    const selfMatch = query === '' || nodeSearchText(node).includes(query)
    if (query && !selfMatch && !ancestorMatched && !nodeMatchesDeep(node, query)) return
    const key = nodeKey(path, node)
    const hasChildren = !!node.children && node.children.length > 0
    const expanded = hasChildren && (query !== '' || expandedKeys.value.has(key))
    bucket.push({kind: 'variable', key, node, depth, path, hasChildren, expanded})
    if (hasChildren && expanded) {
      // 名称自身命中时展示完整子树（保持旧路径搜索行为）；仅后代命中时继续过滤。
      for (const child of node.children ?? []) visit(child, depth + 1, path, bucket, ancestorMatched || selfMatch)
    }
  }

  // 默认只列当前模式下可采样的变量；关闭过滤后把“有名字但无位置”的变量也列出（标记不可采样）。
  const base = info.variables.filter((node) => {
    if (activeOnly.value && activePc.value !== null && !nodeValidAtPc(node)) return false
    if (currentFunctionOnly.value && activePc.value !== null && !nodeInActiveFunction(node)) return false
    if (!onlySampleable.value) return true
    if (isLocalSource.value) return !!localExprForNode(node)
    return samplingMode.value === 'debug' ? node.sampleable : node.liveSamplable
  })
  if (groupBy.value === 'none') {
    for (const node of base) visit(node, 0, '', rows, false)
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
    for (const node of bucket) visit(node, 0, '', rows, false)
  }
  return rows
})

const visibleVariableCount = computed(() => variableRows.value.filter((row) => row.kind === 'variable').length)
const unsampleableCount = computed(() => {
  const info = elfInfo.value
  if (!info) return 0
  return info.variables.filter((node) => !node.sampleable).length
})

/** O(1) 选中集合：替代每行线性扫 seriesList（300 行 × 256 选中）。 */
const selectedKeySet = computed<Set<string>>(() => new Set(seriesList.value.map((item) => item.key)))

function isRowSelected(row: VariableRow): boolean {
  if (row.kind !== 'variable') return false
  return selectedKeySet.value.has(row.key)
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

/** 构建曲线对象（不修改 seriesList；批量选择时先构建后一次性赋值）。 */
function makeSeries(name: string, key: string, typeName: string, node: VariableNode): CurveSeries | null {
  if (!node.sample && !node.location) return null
  const location = node.sample?.location ?? node.location
  const register = node.sample?.register
  if (isLocalSource.value && !localExprForNode(node)) return null
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
    loclistMulti: node.loclistMulti === true,
    ...(node.locationVariants && node.locationVariants.length > 0
      ? {pcVariants: node.locationVariants.map((variant) => ({pcLow: variant.pcLow, pcHigh: variant.pcHigh, expr: variant.expr}))}
      : (validPcRange(node.pcLow, node.pcHigh) ?? {})),
  }
  return series
}

/** 注册曲线到 ring 缓冲 + seriesList（单个/批量共用；批量只触发一次响应式更新）。 */
function registerSeries(series: CurveSeries): void {
  seriesLo.set(series.id, new Uint32Array(MAX_POINTS))
  seriesHi.set(series.id, new Uint32Array(MAX_POINTS))
  seriesValues.set(series.id, new Float64Array(MAX_POINTS))
}

function newSeries(name: string, key: string, typeName: string, node: VariableNode): CurveSeries | null {
  if (seriesList.value.some((item) => item.key === key)) return null
  if (isLocalSource.value && seriesList.value.length >= CURVE_LOCAL_MAX_TOTAL_SERIES) {
    globalNotify(translate('curve.localSeriesLimit', {count: CURVE_LOCAL_MAX_TOTAL_SERIES}), 'warning')
    return null
  }
  if (!isLocalSource.value && seriesList.value.length >= MAX_CURVE_SERIES) {
    globalNotify(translate('curve.maxSeries', {max: MAX_CURVE_SERIES}), 'warning')
    return null
  }
  const series = makeSeries(name, key, typeName, node)
  if (!series) return null
  registerSeries(series)
  seriesList.value = [...seriesList.value, series]
  invalidateYRange()
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
  seriesValues.delete(id)
  seriesList.value = seriesList.value.filter((item) => item.id !== id)
  hover.value = null
  invalidateYRange()
  scheduleDraw()
}

function removeSeriesByKey(key: string): void {
  const item = seriesList.value.find((candidate) => candidate.key === key)
  if (item) removeSeries(item.id)
}

function toggleVisible(item: CurveSeries): void {
  item.visible = !item.visible
  invalidateYRange()
  scheduleDraw()
}

/** 搜索框 150ms debounce：输入期间不重算变量树/重建 DOM。 */
function onVariableSearchInput(): void {
  if (searchTimer) window.clearTimeout(searchTimer)
  searchTimer = window.setTimeout(() => {
    searchTimer = 0
    variableSearch.value = variableSearchInput.value
    variableDisplayLimit.value = variableSearch.value ? SEARCH_PAGE_SIZE : VARIABLE_PAGE_SIZE
  }, 150)
}

function showMoreVariables(): void {
  const increment = variableSearch.value ? SEARCH_PAGE_SIZE : VARIABLE_PAGE_SIZE
  variableDisplayLimit.value = Math.min(variableRows.value.length, variableDisplayLimit.value + increment)
}

function selectionLimitForSource(): number {
  return isLocalSource.value ? localSeriesLimit : MAX_CURVE_SERIES
}

/** 批量选择（一次操作只提示一次上限）：仍复用 newSeries 的采样表达式/PC/variants 构建。 */
function addSelectedRowsBulk(rows: Array<{name: string; key: string; node: VariableNode}>): void {
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  const limit = selectionLimitForSource()
  const existing = new Set(seriesList.value.map((item) => item.key))
  const created: CurveSeries[] = []
  for (const row of rows) {
    if (seriesList.value.length + created.length >= limit) break
    if (existing.has(row.key)) continue
    const series = makeSeries(row.name, row.key, row.node.typeName, row.node)
    if (!series) continue
    registerSeries(series)
    existing.add(row.key)
    created.push(series)
  }
  if (created.length > 0) {
    // 一次性替换数组：避免批量选择时 256 次 reactive 更新/重绘。
    seriesList.value = [...seriesList.value, ...created]
    // 大批量选择时先折叠已选面板，避免首次挂载数百个 Element 组件阻塞交互。
    if (seriesList.value.length > SELECTED_RENDER_LIMIT) selectedCollapsed.value = true
    invalidateYRange()
    scheduleDraw()
  }
}

/** 选择当前变量树中可见/可选的变量（受搜索与分组过滤）。 */
function selectAllVisible(): void {
  const rows = variableRows.value
    .filter((row) => row.kind === 'variable' && nodeSelectable(row.node) && !isRowSelected(row))
    .map((row) => ({name: row.path, key: row.key, node: row.node}))
  if (rows.length === 0) return
  const remaining = selectionLimitForSource() - seriesList.value.length
  const picked = rows.slice(0, Math.max(0, remaining))
  addSelectedRowsBulk(picked)
  if (rows.length > picked.length) {
    globalNotify(translate('curve.selectionLimitReached', {max: selectionLimitForSource()}), 'warning')
  }
}

/** 选择当前 ELF 中全部可采样根变量（受采样来源上限约束）。 */
function selectAllSampleable(): void {
  const info = elfInfo.value
  if (!info) return
  const rows = info.variables
    .filter((node) => nodeSelectable(node))
    .map((node) => ({name: node.name, key: nodeKey(node.name, node), node}))
  const remaining = selectionLimitForSource() - seriesList.value.length
  const picked = rows.slice(0, Math.max(0, remaining))
  addSelectedRowsBulk(picked)
  if (rows.length > picked.length) {
    globalNotify(translate('curve.selectionLimitReached', {max: selectionLimitForSource()}), 'warning')
  }
}

function clearSelection(): void {
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  for (const item of seriesList.value) {
    seriesLo.delete(item.id)
    seriesHi.delete(item.id)
    seriesValues.delete(item.id)
  }
  seriesList.value = []
  hover.value = null
  invalidateYRange()
  scheduleDraw()
}

const bulkSelectDisabled = computed(() => (isLocalSource.value && sampling.value)
  || !elfInfo.value || (elfInfo.value?.variables.length ?? 0) === 0)
/** Shift 连选锚点 + Select current file/group 的上下文。 */
let lastClickedRowKey = ''
let lastClickedNode: VariableNode | null = null
const lastClickedFile = computed(() => lastClickedNode ? (lastClickedNode.relativeFile ?? lastClickedNode.file ?? '') : '')
const lastClickedGroup = computed(() => {
  if (!lastClickedNode || groupBy.value === 'none') return ''
  return groupKeyOf(lastClickedNode, groupBy.value)
})

function rowsBetweenKeys(fromKey: string, toKey: string): Array<{name: string; key: string; node: VariableNode}> {
  const rows = variableRows.value
  const from = rows.findIndex((row) => row.key === fromKey)
  const to = rows.findIndex((row) => row.key === toKey)
  if (from < 0 || to < 0) return []
  const [low, high] = from <= to ? [from, to] : [to, from]
  return rows.slice(low, high + 1)
    .filter((row) => row.kind === 'variable' && nodeSelectable(row.node))
    .map((row) => ({name: row.path, key: row.key, node: row.node}))
}

function onVariableRowClick(row: VariableRow, event: MouseEvent): void {
  if (row.kind !== 'variable' || !nodeSelectable(row.node)) return
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  if (event.shiftKey && lastClickedRowKey) {
    const entries = rowsBetweenKeys(lastClickedRowKey, row.key)
    if (entries.length > 0) {
      const selecting = !isRowSelected(row)
      if (selecting) {
        addSelectedRowsBulk(entries)
      } else {
        for (const entry of entries) removeSeriesByKey(entry.key)
      }
    }
    lastClickedRowKey = row.key
    lastClickedNode = row.node
    return
  }
  lastClickedRowKey = row.key
  lastClickedNode = row.node
  toggleRow(row)
}

function selectCurrentFile(): void {
  const file = lastClickedFile.value
  const info = elfInfo.value
  if (!file || !info) return
  const rows = info.variables
    .filter((node) => (node.relativeFile ?? node.file) === file && nodeSelectable(node))
    .map((node) => ({name: node.name, key: nodeKey(node.name, node), node}))
  addSelectedRowsBulk(rows)
}

function selectCurrentGroup(): void {
  const info = elfInfo.value
  if (!lastClickedNode || !info || groupBy.value === 'none') return
  const group = groupKeyOf(lastClickedNode, groupBy.value)
  const rows = info.variables
    .filter((node) => groupKeyOf(node, groupBy.value) === group && nodeSelectable(node))
    .map((node) => ({name: node.name, key: nodeKey(node.name, node), node}))
  addSelectedRowsBulk(rows)
}

function moveSeries(item: CurveSeries, delta: number): void {
  const list = [...seriesList.value]
  const index = list.findIndex((candidate) => candidate.id === item.id)
  const next = index + delta
  if (index < 0 || next < 0 || next >= list.length) return
  const [moved] = list.splice(index, 1)
  list.splice(next, 0, moved)
  seriesList.value = list
  scheduleDraw()
}

// 过滤/来源变化时重置“Show more”分页，避免搜索结果被旧 limit 截断。
watch([variableSearch, onlySampleable, groupBy, elfInfo, samplingSource], () => {
  variableDisplayLimit.value = variableSearch.value ? SEARCH_PAGE_SIZE : VARIABLE_PAGE_SIZE
  invalidateYRange()
})
watch([renderAll, drawCap], () => {
  invalidateYRange()
  scheduleDraw()
})
watch([yAxisMode], () => {
  yRangeCache.dirty = true
  normalizedRangeCache.clear()
  scheduleDraw()
})
watch([timeWindowSeconds, followLatest], () => {
  scheduleDraw()
})

function applyGlobalFormat(): void {
  for (const item of seriesList.value) {
    item.format = globalFormat.value
    recomputeSeriesValues(item)
  }
  invalidateYRange()
  scheduleDraw()
}

/** 单条曲线切换显示格式：重算缓存值（历史 ring 也需要按新格式解释）。 */
function onSeriesFormatChange(item: CurveSeries): void {
  recomputeSeriesValues(item)
  invalidateYRange()
  scheduleDraw()
}

function recomputeSeriesValues(item: CurveSeries): void {
  const values = seriesValues.get(item.id)
  const loBuffer = seriesLo.get(item.id)
  const hiBuffer = seriesHi.get(item.id)
  if (!values || !loBuffer || !hiBuffer) return
  for (let order = 0; order < ringCount; order++) {
    const slot = ringSlot(order)
    values[slot] = decodeNumber(item, loBuffer[slot], hiBuffer[slot])
  }
}

function invalidateYRange(): void {
  yRangeCache.dirty = true
}

/** 数值缩放：multiplier/offset 仅作用于图表数值（hex/bool/enum 文本保持原义）。 */
function applyValueScale(value: number): number {
  const multiplier = valueMultiplier.value
  const offset = valueOffset.value
  if (!Number.isFinite(value)) return value
  if (multiplier === 1 && offset === 0) return value
  if (!Number.isFinite(multiplier) || !Number.isFinite(offset)) return value
  return value * multiplier + offset
}

function loadValueScale(): void {
  try {
    const raw = window.localStorage.getItem(VALUE_SCALE_STORAGE_KEY)
    if (!raw) return
    const parsed = JSON.parse(raw) as {multiplier?: number; offset?: number}
    if (typeof parsed.multiplier === 'number' && Number.isFinite(parsed.multiplier)) valueMultiplier.value = parsed.multiplier
    if (typeof parsed.offset === 'number' && Number.isFinite(parsed.offset)) valueOffset.value = parsed.offset
  } catch { /* 忽略损坏的本地设置 */ }
}

function saveValueScale(): void {
  try {
    window.localStorage.setItem(VALUE_SCALE_STORAGE_KEY, JSON.stringify({multiplier: valueMultiplier.value, offset: valueOffset.value}))
  } catch { /* localStorage 不可用时忽略 */ }
}

watch([valueMultiplier, valueOffset], () => {
  for (const item of seriesList.value) recomputeSeriesValues(item)
  invalidateYRange()
  saveValueScale()
  scheduleDraw()
})

const wideHint = computed(() => seriesList.value.some((item) => item.sample.size > 4 || item.sample.bitSize > 32))

function sourceLabel(node: VariableNode): string {
  const path = node.relativeFile ?? node.file
  if (!path) return '-'
  return node.line ? `${path}:${node.line}` : path
}

function legendLabel(item: CurveSeries): string {
  return item.typeName ? `${item.name} [${item.typeName}]` : item.name
}

// ---- task-42：精确行表 / PC 当前行 / 目标匹配 / watchpoint ---------------
/** 读 ELF32 PT_LOAD 的 Flash 地址范围（仅用于与 target flash_size 比对）。 */
function elfPtLoadRange(buffer: ArrayBuffer): [number, number] | null {
  const bytes = new Uint8Array(buffer)
  if (bytes.length < 52) return null
  const view = new DataView(buffer)
  if (view.getUint8(0) !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46) return null
  const phoff = view.getUint32(28, true)
  const phentsize = view.getUint16(42, true)
  const phnum = view.getUint16(44, true)
  if (phoff <= 0 || phentsize < 32 || phnum <= 0 || phoff + phentsize * phnum > bytes.length) return null
  let min = Number.POSITIVE_INFINITY
  let max = 0
  for (let index = 0; index < phnum; index++) {
    const offset = phoff + index * phentsize
    if (view.getUint32(offset, true) !== 1) continue // PT_LOAD
    const fileSize = view.getUint32(offset + 16, true)
    if (fileSize === 0) continue
    const lma = view.getUint32(offset + 12, true) || view.getUint32(offset + 8, true)
    const end = lma + fileSize
    if (lma < 0x08000000 || lma >= 0x09000000) continue
    if (lma < min) min = lma
    if (end > max) max = end
  }
  return Number.isFinite(min) ? [min >>> 0, max >>> 0] : null
}

/** 取第一个 PT_LOAD Flash 段地址与前 4 字节（用于真实固件一致性校验）。 */
function elfFirstLoadableSegment(buffer: ArrayBuffer): {address: number; bytes: Uint8Array} | null {
  const bytes = new Uint8Array(buffer)
  if (bytes.length < 52) return null
  const view = new DataView(buffer)
  const phoff = view.getUint32(28, true)
  const phentsize = view.getUint16(42, true)
  const phnum = view.getUint16(44, true)
  if (phoff <= 0 || phentsize < 32 || phnum <= 0 || phoff + phentsize * phnum > bytes.length) return null
  for (let index = 0; index < phnum; index++) {
    const offset = phoff + index * phentsize
    if (view.getUint32(offset, true) !== 1) continue
    const fileOffset = view.getUint32(offset + 4, true)
    const lma = view.getUint32(offset + 12, true) || view.getUint32(offset + 8, true)
    const fileSize = view.getUint32(offset + 16, true)
    if (fileSize < 4 || lma < 0x08000000 || lma >= 0x09000000) continue
    if (fileOffset + 4 > bytes.length) continue
    return {address: lma >>> 0, bytes: bytes.slice(fileOffset, fileOffset + 4)}
  }
  return null
}

const detectedTarget = computed(() => parseTargetDetection(targetRaw.value))
const knownTargetProfiles = allTargetProfiles()
const selectedTargetProfile = computed(() => findKnownTargetProfile(selectedTargetProfileId.value))
const compactTarget = computed<CompactTargetProfile>(() => compactTargetProfileApi(detectedTarget.value, selectedTargetProfile.value))
const targetMatch = computed(() => compareDetectedToProfile(detectedTarget.value, selectedTargetProfile.value))
const targetLimitations = computed(() => detectionLimitations(detectedTarget.value, selectedTargetProfile.value))
const targetFlashWarning = computed(() => {
  const flash = compactTarget.value.flashRange
  const elf = elfFlashRange.value
  if (!flash || !elf) return ''
  const [flashStart, flashEnd] = flash
  const [elfStart, elfEnd] = elf
  if (elfStart < flashStart || elfEnd > flashEnd) return translate('curve.targetFlashMismatch')
  if (detectedTarget.value.flashKib > 0 && elfEnd - flashStart > detectedTarget.value.flashKib * 1024) return translate('curve.targetFlashBeyond')
  return ''
})
const activePcLocation = computed<ElfLineLocation | null>(() => {
  if (activePc.value === null || !lineIndex.value) return null
  return pcToLine(lineIndex.value, activePc.value, {allowNonFlash: true})
})

/** task-43/44：声明式 PC 行（关闭 Follow 时传 null；组件负责高亮+滚动）。 */
const sourceActiveFileLine = computed<SourceActiveFileLine | null>(() => {
  if (!autoPcSource.value || activePc.value === null || !lineIndex.value) return null
  const location = activePcLocation.value
  if (!location) return null
  const file = typeof location.file === 'string'
    ? location.file
    : (location.file as {relativePath?: string; path?: string} | null)?.relativePath
      ?? (location.file as {path?: string} | null)?.path
      ?? ''
  if (!file) return null
  return {path: file, line: location.line, address: location.address}
})

/** 无行表/PC 无行时的降级提示（不阻塞 Variables/图表）。 */
const pcLineStatusMessage = computed(() => {
  if (!autoPcSource.value || activePc.value === null) return ''
  if (!lineIndex.value) return translate('curve.pcLineNoLineIndex')
  if (!activePcLocation.value) return translate('curve.pcLineNoLine')
  return ''
})




async function refreshTargetInfo(): Promise<void> {
  targetLoading.value = true
  targetError.value = ''
  try {
    targetRaw.value = await getTargetInfo()
    if (targetRaw.value.httpStatus && targetRaw.value.detected === false) {
      targetError.value = targetRaw.value.err ?? `HTTP ${targetRaw.value.httpStatus}`
    }
  } catch (error) {
    targetError.value = error instanceof Error ? error.message : String(error)
  } finally {
    targetLoading.value = false
  }
}

function nodeInActiveFunction(node: VariableNode): boolean {
  if (activePc.value === null) return true
  const location = activePcLocation.value
  const file = node.relativeFile ?? node.file
  const locationFile = typeof location?.file === 'string'
    ? location.file
    : (location?.file as {relativePath?: string; path?: string} | null)?.relativePath
      ?? (location?.file as {path?: string} | null)?.path
      ?? ''
  if (locationFile && file) {
    const current = locationFile.replace(/\\/g, '/')
    const candidate = file.replace(/\\/g, '/')
    if (!candidate.endsWith(current) && !current.endsWith(candidate)) return false
  }
  return nodeValidAtPc(node)
}

function parseWatchAddressInput(text: string): number | null {
  let value = String(text ?? '').trim()
  if (!value) return null
  if (/^0x/i.test(value)) value = value.slice(2)
  if (!/^[0-9a-fA-F]{1,8}$/.test(value)) return null
  const address = Number.parseInt(value, 16) >>> 0
  return address === 0 ? null : address
}

const watchpointSpec = computed<CurveLocalWatchpointSpec | null>(() => {
  const address = parseWatchAddressInput(watchAddressInput.value)
  if (address === null) return null
  if (address % watchSize.value !== 0) return null
  return {address, size: watchSize.value, access: watchAccess.value}
})
const conditionSpec = computed<CurveConditionSpec | null>(() => {
  if (!conditionEnabled.value) return null
  const address = parseWatchAddressInput(conditionAddressInput.value)
  if (address === null || address % conditionSize.value !== 0) return null
  return {
    address,
    size: conditionSize.value,
    op: conditionOp.value,
    value: conditionValue.value,
    signed: conditionSigned.value,
    mode: conditionMode.value,
    stopAfter: conditionStopAfter.value,
  }
})
const conditionError = computed(() => {
  if (!conditionEnabled.value) return ''
  if (!conditionAddressInput.value.trim()) return translate('curve.conditionAddressRequired')
  if (conditionSpec.value === null) return translate('curve.conditionInvalid')
  return ''
})
const activeTriggerSeq = computed(() => conditionTriggerSeq.value ?? watchTriggerSeq.value)

const watchpointError = computed(() => {
  if (!isLocalSource.value || localTrigger.value !== 'watchpoint') return ''
  if (!watchAddressInput.value.trim()) return translate('curve.watchpointAddressRequired')
  if (watchpointSpec.value === null) return translate('curve.watchpointInvalid')
  return ''
})

function jumpToTriggerSeq(targetSeq: number | null, preTrigger: boolean): void {
  if (targetSeq === null || ringCount === 0) {
    globalNotify(translate('curve.watchpointNoTrigger'), 'warning')
    return
  }
  let foundSlot = -1
  for (let order = ringCount - 1; order >= 0; order--) {
    const slot = ringSlot(order)
    if (slotDeviceSeq[slot] === targetSeq) { foundSlot = slot; break }
  }
  if (foundSlot < 0) {
    globalNotify(translate('curve.watchpointNoTrigger'), 'warning')
    return
  }
  const triggerTime = timestamps[foundSlot]
  const firstTime = timestamps[ringSlot(0)]
  const dt = ringCount > 1 ? Math.max(1e-4, (lastDataTimestamp() - firstTime) / (ringCount - 1)) : 0.02
  const desired = Math.max(1, jumpFrames.value * dt * 2)
  const window = TIME_WINDOW_OPTIONS.find((option) => option >= desired) ?? 60
  timeWindowSeconds.value = window
  followLatest.value = false
  const shift = preTrigger ? -jumpFrames.value * dt : jumpFrames.value * dt
  viewEndTime.value = clampViewEnd(triggerTime + shift + window / 2)
  cursorA.value = {index: ringSlotToOrder(foundSlot), time: triggerTime}
  cursorPulse.value += 1
  scheduleDraw()
}

async function verifyElfAgainstTarget(): Promise<void> {
  const segment = elfVerifySegment.value
  if (!segment) {
    verifyState.value = 'error'
    verifyDetail.value = translate('curve.verifyNeedElf')
    return
  }
  if (sampling.value) {
    verifyState.value = 'error'
    verifyDetail.value = translate('curve.verifyStopFirst')
    return
  }
  verifyState.value = 'running'
  verifyDetail.value = translate('curve.verifyRunning')
  try {
    // 显式 no-halt 单变量采样读取 Flash 首段 4 字节；结束立即 stop 恢复目标运行。
    await startLocalCurve({
      intervalUs: 20000,
      trigger: 'periodic',
      halt: false,
      series: [{id: 0, size: 4, expr: {kind: 'addr', address: segment.address}}],
    })
    let read: Uint8Array | null = null
    const deadline = performance.now() + 2500
    while (performance.now() < deadline) {
      await new Promise((resolve) => window.setTimeout(resolve, 120))
      try {
        const page = await getLocalCurveFrames(0, 16)
        const frame = page.frames.find((item) => item.payload.length >= 4)
        if (frame) { read = frame.payload.slice(0, 4); break }
      } catch { /* keep polling */ }
    }
    if (!read) {
      verifyState.value = 'error'
      verifyDetail.value = translate('curve.verifyNoFrames')
      return
    }
    const expected = Array.from(segment.bytes)
    const actual = Array.from(read)
    const same = expected.every((value, index) => value === actual[index])
    verifyState.value = same ? 'match' : 'mismatch'
    verifyDetail.value = `${formatHex32(segment.address)} ELF=${expected.map((v) => v.toString(16).padStart(2, '0')).join('')} TARGET=${actual.map((v) => v.toString(16).padStart(2, '0')).join('')}`
    if (!same) globalNotify(translate('curve.verifyMismatch'), 'warning')
  } catch (error) {
    verifyState.value = 'error'
    verifyDetail.value = localErrorText(error)
  } finally {
    try { await stopLocalCurve() } catch { /* best effort */ }
  }
}

function jumpToWatchTrigger(preTrigger: boolean): void {
  jumpToTriggerSeq(watchTriggerSeq.value, preTrigger)
}

function jumpToConditionTrigger(preTrigger: boolean): void {
  jumpToTriggerSeq(conditionTriggerSeq.value, preTrigger)
}

/** task-44：对当前窗口第一条绘制曲线做简单 DFT（N≤256，按需执行，不阻塞主流程）。 */
function runFft(): void {
  const item = drawnSeriesList.value[0]
  const geometry = lastGeometry
  if (!item || !geometry || ringCount < 16) {
    globalNotify(translate('curve.fftNoSeries'), 'warning')
    return
  }
  const values = seriesValues.get(item.id)
  if (!values) return
  const startOrder = Math.max(0, lowerBoundTime(geometry.xMin))
  const endOrder = Math.min(ringCount, Math.max(startOrder + 2, lowerBoundTime(geometry.xMax)))
  const available = endOrder - startOrder
  const n = Math.min(256, available)
  if (n < 16) {
    globalNotify(translate('curve.fftNoSeries'), 'warning')
    return
  }
  const samples = new Float64Array(n)
  for (let index = 0; index < n; index++) {
    const order = startOrder + Math.floor(index * available / n)
    const slot = ringSlot(Math.min(ringCount - 1, order))
    samples[index] = Number.isFinite(values[slot]) ? values[slot] : 0
  }
  const mean = samples.reduce((sum, value) => sum + value, 0) / n
  for (let index = 0; index < n; index++) samples[index] -= mean
  const dt = Math.max(1e-6, (geometry.xMax - geometry.xMin) / n)
  const sampleRate = 1 / dt
  const peaks: Array<{hz: number; magnitude: number}> = []
  for (let k = 1; k <= n / 2; k++) {
    let real = 0
    let imag = 0
    for (let t = 0; t < n; t++) {
      const angle = (2 * Math.PI * k * t) / n
      real += samples[t] * Math.cos(angle)
      imag -= samples[t] * Math.sin(angle)
    }
    peaks.push({hz: (k * sampleRate) / n, magnitude: Math.hypot(real, imag) / n})
  }
  peaks.sort((left, right) => right.magnitude - left.magnitude)
  fftData.value = {name: item.name, sampleRate, peaks: peaks.slice(0, 8)}
  fftOpen.value = true
}

async function exportSelectedRecordingJson(): Promise<void> {
  if (!selectedRecordingId.value) return
  try {
    const data = await loadRecording(selectedRecordingId.value)
    if (!data) return
    const exported = {
      ...data,
      rows: data.rows.map((row) => ({
        ...row,
        payloads: row.payloads.map((payload) => Array.from(payload).map((byte) => byte.toString(16).padStart(2, '0')).join('')),
      })),
    }
    downloadBlob(new Blob([JSON.stringify(exported, null, 2)], {type: 'application/json'}), `recording-${data.name.replace(/[^a-zA-Z0-9_-]+/g, '-')}.json`)
  } catch (error) {
    globalNotify(translate('curve.recordLoadFailed', {err: localErrorText(error)}), 'error')
  }
}

async function exportSelectedRecordingCsv(): Promise<void> {
  if (!selectedRecordingId.value) return
  try {
    const data = await loadRecording(selectedRecordingId.value)
    if (!data) return
    const header = ['time_s', 'group', 'seq', 'pc', ...data.series.map((series) => csvEscape(`${series.name} [${series.typeName}]`))]
    const lines = [header.join(',')]
    for (const row of data.rows) {
      const cells: string[] = [
        row.time.toFixed(6),
        String(row.group ?? 0),
        row.seq !== undefined ? String(row.seq) : '',
        row.pc !== undefined ? `0x${row.pc.toString(16).toUpperCase()}` : '',
      ]
      for (const payload of row.payloads) {
        cells.push(Array.from(payload).map((byte) => byte.toString(16).padStart(2, '0')).join(''))
      }
      lines.push(cells.join(','))
    }
    downloadBlob(new Blob([lines.join('\n')], {type: 'text/csv;charset=utf-8'}), `recording-${data.name.replace(/[^a-zA-Z0-9_-]+/g, '-')}.csv`)
  } catch (error) {
    globalNotify(translate('curve.recordLoadFailed', {err: localErrorText(error)}), 'error')
  }
}

function ringSlotToOrder(slot: number): number {
  return (slot - ringHead + ringCount + MAX_POINTS) % MAX_POINTS
}

// ---- task-37：group weight / 预设 / watch / 光标 / 派生 / 诊断 -----------
const localGroupWeightSum = computed(() => groupWeights.value.slice(0, localActiveGroupCount.value).reduce((sum, weight) => sum + weight, 0))
/** 每组实际帧刷新率 = interval 总节拍按 weight 分配。 */
const localGroupWeightedRates = computed(() => {
  const count = localActiveGroupCount.value
  const total = localGroupWeightSum.value
  if (total <= 0 || localIntervalMs.value <= 0) return [] as number[]
  return Array.from({length: count}, (_, index) => (1000 / localIntervalMs.value) * (groupWeights.value[index] ?? 1) / total)
})

function ensureGroupWeights(count: number): void {
  const next = groupWeights.value.slice(0, Math.max(1, count))
  while (next.length < Math.max(1, count)) next.push(1)
  groupWeights.value = next
}

watch(localActiveGroupCount, (count) => ensureGroupWeights(count), {immediate: true})

function setGroupWeight(index: number, value: number): void {
  if (index < 0 || index >= groupWeights.value.length) return
  const next = [...groupWeights.value]
  next[index] = Math.min(8, Math.max(1, Math.round(Number(value) || 1)))
  groupWeights.value = next
}

/** PC 是否落入变量/变体的半开区间（无 PC 信息时不判定为 stale）。 */
function pcRangeContains(low: number | undefined, high: number | undefined, pc: number): boolean {
  return typeof low === 'number' && typeof high === 'number' && pc >= low && pc < high
}

function nodeHasPcRange(node: VariableNode): boolean {
  if (node.locationVariants && node.locationVariants.length > 0) return true
  return typeof node.pcLow === 'number' && typeof node.pcHigh === 'number'
}

function nodeValidAtPc(node: VariableNode): boolean {
  const pc = activePc.value
  if (pc === null || !nodeHasPcRange(node)) return true
  if (node.locationVariants && node.locationVariants.length > 0) {
    return node.locationVariants.some((variant) => pcRangeContains(variant.pcLow, variant.pcHigh, pc))
  }
  return pcRangeContains(node.pcLow, node.pcHigh, pc)
}

function seriesActiveAtPc(item: CurveSeries, pc: number | null): boolean {
  if (pc === null) return true
  if (item.pcVariants && item.pcVariants.length > 0) {
    return item.pcVariants.some((variant) => pcRangeContains(variant.pcLow, variant.pcHigh, pc))
  }
  if (typeof item.pcLow === 'number' && typeof item.pcHigh === 'number') {
    return pcRangeContains(item.pcLow, item.pcHigh, pc)
  }
  return true
}

const staleSeriesKeys = computed<Set<string>>(() => new Set(seriesList.value
  .filter((item) => !seriesActiveAtPc(item, activePc.value))
  .map((item) => item.key)))

// ---- 显式预设（localStorage，用户点击保存；不自动保存选择） ----
function serializeSeriesData(item: CurveSeries): RecordingSeriesData {
  // Vue reactive proxy 不能被 IndexedDB structured clone；JSON 往返得到纯对象。
  const plain = JSON.parse(JSON.stringify(item)) as CurveSeries
  return {
    name: plain.name,
    key: plain.key,
    typeName: plain.typeName,
    address: plain.address,
    size: plain.size,
    sample: plain.sample,
    format: plain.format,
    color: plain.color,
    visible: plain.visible,
    location: plain.location,
    register: plain.register,
    pcLow: plain.pcLow,
    pcHigh: plain.pcHigh,
    loclistMulti: plain.loclistMulti,
    variants: plain.pcVariants,
  }
}

function loadPresets(): void {
  try {
    const raw = window.localStorage.getItem(PRESET_STORAGE_KEY)
    if (!raw) return
    const parsed = JSON.parse(raw) as PresetRecord[]
    if (Array.isArray(parsed)) presets.value = parsed.filter((item) => item && typeof item.id === 'string' && Array.isArray(item.series))
  } catch { /* ignore corrupted presets */ }
}

function persistPresets(): void {
  try { window.localStorage.setItem(PRESET_STORAGE_KEY, JSON.stringify(presets.value)) } catch { /* ignore */ }
}

function restoreSeriesFromData(data: RecordingSeriesData): CurveSeries {
  const series: CurveSeries = {
    id: nextSeriesId++,
    name: data.name,
    key: data.key,
    typeName: data.typeName,
    address: data.address,
    size: data.size,
    sample: data.sample,
    format: data.format as CurveFormat,
    color: data.color,
    visible: data.visible !== false,
    startSeq: 0,
    mode: 'live',
    location: data.location,
    register: data.register,
    pcLow: data.pcLow,
    pcHigh: data.pcHigh,
    loclistMulti: data.loclistMulti,
    pcVariants: data.variants,
  }
  registerSeries(series)
  return series
}

function savePreset(): void {
  if (seriesList.value.length === 0) {
    globalNotify(translate('curve.presetEmpty'), 'warning')
    return
  }
  const name = presetName.value.trim() || `${translate('curve.presetDefault')} ${presets.value.length + 1}`
  const preset: PresetRecord = {
    id: `preset-${Date.now().toString(36)}`,
    name,
    createdAt: Date.now(),
    series: seriesList.value.filter((item) => !item.derived).map(serializeSeriesData),
  }
  presets.value = [preset, ...presets.value]
  selectedPresetId.value = preset.id
  presetName.value = ''
  persistPresets()
  globalNotify(translate('curve.presetSaved'), 'success')
}

function applyPreset(): void {
  const preset = presets.value.find((item) => item.id === selectedPresetId.value)
  if (!preset) return
  if (sampling.value) void stopSampling()
  for (const item of seriesList.value) {
    seriesLo.delete(item.id)
    seriesHi.delete(item.id)
    seriesValues.delete(item.id)
  }
  hover.value = null
  cursorA.value = null
  cursorB.value = null
  seriesList.value = preset.series.map(restoreSeriesFromData)
  clearData()
  selectedCollapsed.value = false
  invalidateYRange()
  scheduleDraw()
  globalNotify(translate('curve.presetApplied'), 'success')
}

function deletePreset(): void {
  presets.value = presets.value.filter((item) => item.id !== selectedPresetId.value)
  selectedPresetId.value = presets.value[0]?.id ?? ''
  persistPresets()
}

// ---- Watch（独立于曲线选择；有对应已选曲线时展示统计） ----
const watchNames = ref<Record<string, string>>({})
const watchList = computed(() => watchKeys.value.map((key) => ({key, name: watchNames.value[key] ?? key})))

function isWatched(key: string): boolean {
  return watchKeys.value.includes(key)
}

function toggleWatch(key: string, name: string): void {
  if (isWatched(key)) {
    watchKeys.value = watchKeys.value.filter((item) => item !== key)
  } else {
    watchKeys.value = [...watchKeys.value, key]
    watchNames.value[key] = name
  }
  updateWatchStats()
}

function updateWatchStats(): void {
  if (watchKeys.value.length === 0) return
  const next: Record<string, {current: string; min: string; max: string; changes: number}> = {}
  for (const key of watchKeys.value) {
    const item = seriesList.value.find((candidate) => candidate.key === key)
    if (!item) continue
    const values = seriesValues.get(item.id)
    if (!values || ringCount === 0) continue
    let min = Number.POSITIVE_INFINITY
    let max = Number.NEGATIVE_INFINITY
    let last = Number.NaN
    let changes = 0
    const stride = Math.max(1, Math.ceil(ringCount / 4000))
    for (let order = 0; order < ringCount; order += stride) {
      const slot = ringSlot(order)
      if (slotSeq[slot] < item.startSeq) continue
      const value = values[slot]
      if (!Number.isFinite(value)) continue
      if (value < min) min = value
      if (value > max) max = value
      if (Number.isFinite(last) && value !== last) changes += 1
      last = value
    }
    if (!Number.isFinite(min)) continue
    next[key] = {
      current: latestText(item),
      min: formatFloatText(min),
      max: formatFloatText(max),
      changes,
    }
  }
  watchStats.value = next
}

// ---- 双光标 A/B + Δ/统计 ----
function formatNumeric(value: number | undefined): string {
  return value === undefined || !Number.isFinite(value) ? '-' : formatFloatText(value)
}

function seriesValueAt(item: CurveSeries, order: number): number | undefined {
  const values = seriesValues.get(item.id)
  if (!values || order < 0 || order >= ringCount) return undefined
  const slot = ringSlot(order)
  if (slotSeq[slot] < item.startSeq) return undefined
  const value = values[slot]
  return Number.isFinite(value) ? value : undefined
}

function onCanvasCursorClick(event: MouseEvent): void {
  if (panMoved) {
    panMoved = false
    return
  }
  const canvas = canvasRef.value
  const geometry = lastGeometry
  if (!canvas || !geometry || ringCount === 0) return
  const rect = canvas.getBoundingClientRect()
  const x = event.clientX - rect.left
  if (x < geometry.padding.left || x > geometry.padding.left + geometry.plotWidth) return
  const time = geometry.xMin + ((x - geometry.padding.left) / geometry.plotWidth) * (geometry.xMax - geometry.xMin)
  const order = Math.min(ringCount - 1, Math.max(0, lowerBoundTime(time)))
  const point = {index: order, time: timestamps[ringSlot(order)]}
  if (event.shiftKey) cursorB.value = point
  else cursorA.value = point
  cursorPulse.value += 1
  scheduleHoverDraw()
}

function clearCursors(): void {
  if (!cursorA.value && !cursorB.value) return
  cursorA.value = null
  cursorB.value = null
  cursorPulse.value += 1
  scheduleHoverDraw()
}

const cursorDelta = computed(() => {
  void cursorPulse.value
  const a = cursorA.value
  if (!a) return null
  const b = cursorB.value ?? a
  const rows: Array<{name: string; color: string; aText: string; bText: string; delta: string; min: string; max: string; avg: string}> = []
  for (const item of drawnSeriesList.value.slice(0, 24)) {
    const aValue = seriesValueAt(item, a.index)
    const bValue = seriesValueAt(item, b.index)
    let min = Number.POSITIVE_INFINITY
    let max = Number.NEGATIVE_INFINITY
    let sum = 0
    let count = 0
    const low = Math.min(a.index, b.index)
    const high = Math.max(a.index, b.index)
    const stride = Math.max(1, Math.ceil((high - low + 1) / 2000))
    for (let order = low; order <= high; order += stride) {
      const value = seriesValueAt(item, order)
      if (value === undefined) continue
      if (value < min) min = value
      if (value > max) max = value
      sum += value
      count += 1
    }
    rows.push({
      name: item.name,
      color: seriesDisplayColor(item.color),
      aText: formatNumeric(aValue),
      bText: formatNumeric(bValue),
      delta: aValue !== undefined && bValue !== undefined ? formatNumeric(aValue - bValue) : '-',
      min: count > 0 ? formatNumeric(min) : '-',
      max: count > 0 ? formatNumeric(max) : '-',
      avg: count > 0 ? formatNumeric(sum / count) : '-',
    })
  }
  return {dt: b.time - a.time, rows}
})

// ---- A±B 派生曲线 ----
function addDerivedCurve(op: 'add' | 'sub'): void {
  if (sampling.value) {
    globalNotify(translate('curve.derivedWhileSampling'), 'warning')
    return
  }
  const base = seriesList.value.filter((item) => !item.derived)
  if (base.length < 2) {
    globalNotify(translate('curve.derivedNeedTwo'), 'warning')
    return
  }
  const a = base[0]
  const b = base[1]
  const derived: CurveSeries = {
    id: nextSeriesId++,
    name: `${a.name} ${op === 'add' ? '+' : '−'} ${b.name}`,
    key: `derived:${Date.now().toString(36)}:${a.id}:${op}:${b.id}`,
    typeName: translate('curve.derivedType'),
    address: 0,
    size: Math.max(a.size, b.size),
    sample: a.sample,
    format: 'auto',
    color: COLORS[seriesList.value.length % COLORS.length],
    visible: true,
    startSeq: 0,
    mode: 'live',
    derived: {a: a.id, b: b.id, op},
  }
  registerSeries(derived)
  seriesList.value = [...seriesList.value, derived]
  updateDerivedValues()
  invalidateYRange()
  scheduleDraw()
}

function updateDerivedValues(): void {
  for (const item of seriesList.value) {
    if (!item.derived) continue
    const target = seriesValues.get(item.id)
    const left = seriesValues.get(item.derived.a)
    const right = seriesValues.get(item.derived.b)
    if (!target || !left || !right) continue
    for (let order = 0; order < ringCount; order++) {
      const slot = ringSlot(order)
      const leftValue = left[slot]
      const rightValue = right[slot]
      target[slot] = Number.isFinite(leftValue) && Number.isFinite(rightValue)
        ? (item.derived.op === 'add' ? leftValue + rightValue : leftValue - rightValue)
        : Number.NaN
    }
  }
}

// ---- 诊断 ----
const diagnosticsData = computed(() => {
  const info = elfInfo.value
  const variables = info?.variables ?? []
  const sourceMatched = variables.filter((node) => !!(node.relativeFile ?? node.file)).length
  const status = localStatusInfo.value
  return {
    time: new Date().toISOString(),
    source: samplingSource.value,
    sampling: sampling.value,
    playback: playbackMode.value,
    elf: {
      name: lastElfName.value,
      bytes: lastElfBytes.value,
      variables: variables.length,
      sourceMatched,
      unsampleable: unsampleableCount.value,
    },
    device: status ? {
      running: status.running,
      halt: status.halt,
      frames: status.frames,
      overwritten: status.overwritten,
      invalid: status.invalid,
      outOfScope: status.outOfScope,
      groupCount: status.groupCount,
      currentGroup: status.currentGroup,
      intervalUs: status.intervalUs,
      sampleBytes: status.sampleBytes,
      error: status.error,
    } : null,
    ring: {points: ringCount, selected: seriesList.value.length, maxSeries: seriesLimit.value},
    render: {
      yAxisMode: yAxisMode.value,
      renderAll: renderAll.value,
      drawn: drawnSeriesList.value.length,
      pollsPerSecond: Math.round(pollCounterState.pollsPerSecond * 10) / 10,
      avgDrawMs: perfCounter.draws > 0 ? Math.round((perfCounter.drawMsTotal / perfCounter.draws) * 100) / 100 : 0,
      draws: perfCounter.draws,
    },
    recording: {active: recordingActive.value, rows: recordRowCount.value, saved: recordings.value.length},
    activeFrame: {
      pc: activePc.value !== null ? `0x${activePc.value.toString(16).toUpperCase()}` : null,
      staleSeries: staleSeriesKeys.value.size,
      line: activePcLocation.value ? `${typeof activePcLocation.value.file === 'string' ? activePcLocation.value.file : (activePcLocation.value.file as any)?.relativePath ?? ''}:${activePcLocation.value.line}` : null,
    },
    target: {
      detected: detectedTarget.value.detected,
      group: detectedTarget.value.baseGroup,
      devId: detectedTarget.value.devId,
      flashKib: detectedTarget.value.flashKib,
      confidence: detectedTarget.value.confidence,
      selectedProfile: selectedTargetProfile.value?.id ?? null,
      match: targetMatch.value.level,
      limitations: targetLimitations.value,
      elfFlashRange: elfFlashRange.value,
      warning: targetFlashWarning.value,
    },
    lineIndex: lineIndex.value ? {files: lineIndex.value.files.length, rows: lineIndex.value.rows.length} : null,
    watchpoint: {armed: watchpointArmed.value, hits: watchpointHits.value, triggerSeq: watchTriggerSeq.value},
    playbackState: {active: playbackMode.value, index: playbackIndex.value},
    cursor: {a: cursorA.value?.time ?? null, b: cursorB.value?.time ?? null},
    watch: watchKeys.value.length,
    presets: presets.value.length,
    userAgent: typeof navigator !== 'undefined' ? navigator.userAgent : '',
  }
})

const diagnosticsJson = computed(() => JSON.stringify(diagnosticsData.value, null, 2))

async function copyDiagnostics(): Promise<void> {
  try {
    await navigator.clipboard.writeText(diagnosticsJson.value)
    diagnosticsCopied.value = true
    window.setTimeout(() => { diagnosticsCopied.value = false }, 1500)
  } catch {
    globalNotify(translate('curve.diagCopyFailed'), 'warning')
  }
}

// ---- IndexedDB 录制 / 回放 ----
async function refreshRecordings(): Promise<void> {
  try { recordings.value = await listRecordings() } catch { recordings.value = [] }
}

function startRecording(): void {
  if (!sampling.value) {
    globalNotify(translate('curve.recordNeedSampling'), 'warning')
    return
  }
  if (recordingActive.value) return
  recordRows = []
  recordSnapshot = []
  recordStartTimestamp = 0
  recordRowCount.value = 0
  recordingActive.value = true
  globalNotify(translate('curve.recordStarted'), 'success')
}

function stopRecordingCapture(): void {
  if (!recordingActive.value) return
  recordingActive.value = false
}

async function saveCurrentRecording(): Promise<void> {
  stopRecordingCapture()
  if (recordRows.length === 0 || recordSnapshot.length === 0) {
    globalNotify(translate('curve.recordEmpty'), 'warning')
    return
  }
  const name = recordingName.value.trim() || `${translate('curve.recordDefault')} ${new Date().toLocaleTimeString()}`
  const data: CurveRecordingData = {
    id: newRecordingId(),
    name,
    createdAt: Date.now(),
    series: recordSnapshot.map(serializeSeriesData),
    rows: recordRows,
    trigger: localTrigger.value,
    triggerSeq: watchTriggerSeq.value,
    watchpoint: watchpointSpec.value ? {address: watchpointSpec.value.address, size: watchpointSpec.value.size, access: watchpointSpec.value.access} : null,
  }
  try {
    await saveRecording(data)
    recordingName.value = ''
    recordRows = []
    recordSnapshot = []
    recordRowCount.value = 0
    await refreshRecordings()
    globalNotify(translate('curve.recordSaved'), 'success')
  } catch (error) {
    globalNotify(translate('curve.recordSaveFailed', {err: localErrorText(error)}), 'error')
  }
}

function stopPlayback(): void {
  playbackPlaying.value = false
  if (playbackTimer) { window.clearInterval(playbackTimer); playbackTimer = 0 }
}

function applyPlaybackIndex(index: number): void {
  if (!playbackMode.value || playbackRows.value.length === 0) return
  stopPlayback()
  const target = Math.max(0, Math.min(playbackRows.value.length - 1, Math.floor(index)))
  clearData()
  for (let rowIndex = 0; rowIndex <= target; rowIndex++) {
    const row = playbackRows.value[rowIndex]
    if (!row) continue
    pushSample(row.time, row.payloads, playbackSeries.value, row.group)
  }
  playbackIndex.value = target
  followLatest.value = true
  flushStats()
  scheduleDraw()
}

function togglePlayback(): void {
  if (!playbackMode.value || playbackRows.value.length === 0) return
  if (playbackPlaying.value) {
    stopPlayback()
    return
  }
  if (playbackIndex.value >= playbackRows.value.length - 1) applyPlaybackIndex(0)
  playbackPlaying.value = true
  playbackWallStart = performance.now()
  playbackRecordStart = playbackRows.value[playbackIndex.value]?.time ?? 0
  playbackTimer = window.setInterval(() => {
    if (!playbackPlaying.value) return
    const targetTime = playbackRecordStart + ((performance.now() - playbackWallStart) / 1000) * playbackSpeed.value
    let advanced = false
    while (playbackIndex.value + 1 < playbackRows.value.length
      && (playbackRows.value[playbackIndex.value + 1]?.time ?? Number.POSITIVE_INFINITY) <= targetTime) {
      playbackIndex.value += 1
      const row = playbackRows.value[playbackIndex.value]
      if (row) {
        pushSample(row.time, row.payloads, playbackSeries.value, row.group)
        advanced = true
      }
    }
    if (advanced) {
      flushStats()
      scheduleDraw()
    }
    if (playbackIndex.value >= playbackRows.value.length - 1) stopPlayback()
  }, 33)
}

async function loadRecordingById(id: string): Promise<void> {
  if (!id) return
  try {
    const data = await loadRecording(id)
    if (!data) return
    if (sampling.value) void stopSampling()
    stopPlayback()
    recordingActive.value = false
    playbackMode.value = true
    for (const item of seriesList.value) {
      seriesLo.delete(item.id)
      seriesHi.delete(item.id)
      seriesValues.delete(item.id)
    }
    playbackSeries.value = data.series.map(restoreSeriesFromData)
    seriesList.value = playbackSeries.value
    playbackRows.value = data.rows
    playbackIndex.value = 0
    selectedRecordingId.value = id
    clearData()
    applyPlaybackIndex(0)
    globalNotify(translate('curve.recordLoaded'), 'success')
  } catch (error) {
    globalNotify(translate('curve.recordLoadFailed', {err: localErrorText(error)}), 'error')
  }
}

async function deleteSelectedRecording(): Promise<void> {
  if (!selectedRecordingId.value) return
  try {
    await deleteRecording(selectedRecordingId.value)
    if (playbackMode.value) exitPlayback()
    selectedRecordingId.value = ''
    await refreshRecordings()
  } catch { /* ignore */ }
}

function exitPlayback(): void {
  stopPlayback()
  playbackMode.value = false
  playbackRows.value = []
  playbackSeries.value = []
  playbackIndex.value = 0
  clearSelection()
  clearData()
}

// ---- /ws/curve 推送（task-39；WS 优先，失败回退 HTTP） -------------------
function stopCurveWs(): void {
  if (wsFallbackTimer) { window.clearTimeout(wsFallbackTimer); wsFallbackTimer = 0 }
  if (wsFirstMessageTimer) { window.clearTimeout(wsFirstMessageTimer); wsFirstMessageTimer = 0 }
  if (curveWs) {
    try { curveWs.close() } catch { /* ignore */ }
    curveWs = null
  }
}

function startHttpPollingOnly(runId: number): void {
  if (localPollTimer) { window.clearInterval(localPollTimer); localPollTimer = 0 }
  if (!sampling.value || runId !== localRunId) return
  localPollTimer = window.setInterval(() => issueLocalPoll(runId), LOCAL_POLL_MS)
  issueLocalPoll(runId)
}

function stopHttpPolling(): void {
  if (localPollTimer) { window.clearInterval(localPollTimer); localPollTimer = 0 }
}

function fallbackToHttpPolling(runId: number): void {
  if (runId !== localRunId || !sampling.value) return
  stopCurveWs()
  stopHttpPolling()
  wsTransport.value = 'fallback'
  startHttpPollingOnly(runId)
}

function connectCurveWs(runId: number): void {
  stopCurveWs()
  if (typeof WebSocket === 'undefined') { wsTransport.value = 'http'; return }
  wsTransport.value = 'ws'
  let socket: WebSocket
  try {
    socket = new WebSocket(deviceApiUrl(CURVE_WS_PATH).replace(/^http(s?):/, 'ws$1:'))
    curveWs = socket
  } catch {
    fallbackToHttpPolling(runId)
    return
  }
  wsFallbackTimer = window.setTimeout(() => {
    if (curveWs === socket && wsFrames.value === 0) fallbackToHttpPolling(runId)
  }, 2000)
  socket.onopen = () => {
    if (runId !== localRunId) return
    stopHttpPolling()
    try { socket.send(JSON.stringify({cmd: 'subscribe'})) } catch { /* ignore */ }
    wsFirstMessageTimer = window.setTimeout(() => {
      if (curveWs === socket && wsFrames.value === 0) fallbackToHttpPolling(runId)
    }, 1500)
  }
  socket.onmessage = (event: MessageEvent) => {
    if (runId !== localRunId) return
    if (typeof event.data === 'string') return
    void (async () => {
      try {
        const buffer = event.data instanceof Blob
          ? await event.data.arrayBuffer()
          : event.data instanceof ArrayBuffer ? event.data : null
        if (!buffer) return
        const page = decodeCurveFrames(new Uint8Array(buffer))
        if (wsFirstMessageTimer) { window.clearTimeout(wsFirstMessageTimer); wsFirstMessageTimer = 0 }
        if (wsFallbackTimer) { window.clearTimeout(wsFallbackTimer); wsFallbackTimer = 0 }
        const now = performance.now()
        if (wsLastMessageAt > 0) wsLatencyMs.value = Math.round((wsLatencyMs.value * 0.8 + (now - wsLastMessageAt) * 0.2) * 10) / 10
        wsLastMessageAt = now
        if (wsWindowStart === 0) wsWindowStart = now
        wsWindowFrames += 1
        if (now - wsWindowStart >= 1000) {
          wsLatencyMs.value = Math.round((now - wsWindowStart) / wsWindowFrames * 10) / 10
          wsWindowStart = now
          wsWindowFrames = 0
        }
        wsFrames.value += 1
        if (page.triggered) { watchFramesTriggered.value = true; void refreshLocalStatus() }
        handleLocalFramesPage(page)
      } catch {
        wsDropped.value += 1
        if (wsDropped.value >= 5) fallbackToHttpPolling(runId)
      }
    })()
  }
  socket.onerror = () => fallbackToHttpPolling(runId)
  socket.onclose = () => {
    if (curveWs === socket) curveWs = null
    if (sampling.value && runId === localRunId) fallbackToHttpPolling(runId)
  }
}

// ---- frames Worker（CVF2 解码移出主线程；失败 fallback 主线程） ----
let framesWorker: Worker | null = null
let framesWorkerSeq = 0
const framesWorkerPending = new Map<number, {resolve: (page: CurveLocalFramesPage) => void; reject: (error: Error) => void; timeout: number}>()

function initFramesWorker(): void {
  if (typeof Worker === 'undefined') return
  try {
    framesWorker = new CurveFramesWorker()
    framesWorker.onmessage = (event: MessageEvent<{id: number; page?: CurveLocalFramesPage; error?: string}>) => {
      const pending = framesWorkerPending.get(event.data.id)
      if (!pending) return
      window.clearTimeout(pending.timeout)
      framesWorkerPending.delete(event.data.id)
      if (event.data.error || !event.data.page) pending.reject(new Error(event.data.error ?? 'worker error'))
      else pending.resolve(event.data.page)
    }
    framesWorker.onerror = () => {
      framesWorker = null
      for (const pending of framesWorkerPending.values()) {
        window.clearTimeout(pending.timeout)
        pending.reject(new Error('frames worker failed'))
      }
      framesWorkerPending.clear()
    }
  } catch {
    framesWorker = null
  }
}

function requestWorkerFrames(since: number): Promise<CurveLocalFramesPage> {
  const worker = framesWorker
  if (!worker) return Promise.reject(new Error('frames worker unavailable'))
  return new Promise((resolve, reject) => {
    const id = ++framesWorkerSeq
    const timeout = window.setTimeout(() => {
      framesWorkerPending.delete(id)
      reject(new Error('frames worker timeout'))
    }, 10000)
    framesWorkerPending.set(id, {resolve, reject, timeout})
    const query = `?since=${Math.max(0, Math.floor(since))}&max=${LOCAL_POLL_MAX_FRAMES}`
    worker.postMessage({id, url: deviceApiUrl(CURVE_LOCAL_FRAMES_PATH + query)})
  })
}

async function requestLocalFrames(since: number): Promise<CurveLocalFramesPage> {
  if (framesWorker) {
    try {
      return await requestWorkerFrames(since)
    } catch {
      framesWorker = null
    }
  }
  return getLocalCurveFrames(since, LOCAL_POLL_MAX_FRAMES)
}

// ---- 键盘快捷键 ----
function onGlobalKeydown(event: KeyboardEvent): void {
  const target = event.target as HTMLElement | null
  if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.tagName === 'SELECT' || target.isContentEditable)) return
  if (event.key === ' ' && !sampling.value) {
    event.preventDefault()
    void startSampling()
  } else if (event.key === ' ' && sampling.value) {
    event.preventDefault()
    void stopSampling()
  } else if (event.key === 'f' || event.key === 'F') {
    toggleFreeze()
  } else if (event.key === 'd' || event.key === 'D') {
    diagnosticsOpen.value = !diagnosticsOpen.value
  } else if (event.key === '+' || event.key === '=') {
    const index = TIME_WINDOW_OPTIONS.indexOf(timeWindowSeconds.value)
    if (index > 0) timeWindowSeconds.value = TIME_WINDOW_OPTIONS[index - 1]
  } else if (event.key === '-' || event.key === '_') {
    const index = TIME_WINDOW_OPTIONS.indexOf(timeWindowSeconds.value)
    if (index < TIME_WINDOW_OPTIONS.length - 1) timeWindowSeconds.value = TIME_WINDOW_OPTIONS[index + 1]
  } else if (event.key === 'Escape') {
    clearCursors()
  } else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
    if (ringCount === 0) return
    event.preventDefault()
    followLatest.value = false
    const direction = event.key === 'ArrowLeft' ? -1 : 1
    viewEndTime.value = clampViewEnd((viewEndTime.value || lastDataTime.value) + direction * timeWindowSeconds.value * 0.1)
    scheduleDraw()
  }
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

/**
 * 解析 ELF：手工上传、SourceBrowser 项目内自动发现/手工选择共用同一流程。
 * 成功后刷新变量树、函数符号、搜索索引与选中状态。
 */
async function parseElfFile(file: File): Promise<void> {
  elfParsing.value = true
  elfError.value = ''
  try {
    const buffer = await file.arrayBuffer()
    lastElfName.value = file.name
    lastElfBytes.value = buffer.byteLength
    const result = parseElfVariables(buffer)
    elfInfo.value = result
    elfFlashRange.value = elfPtLoadRange(buffer)
    elfVerifySegment.value = elfFirstLoadableSegment(buffer)
    verifyState.value = 'idle'
    verifyDetail.value = ''
    try {
      lineIndex.value = parseElfLineIndex(buffer)
      lineIndexError.value = ''
    } catch (error) {
      lineIndex.value = null
      lineIndexError.value = error instanceof Error ? error.message : String(error)
    }
    void refreshTargetInfo()
    prewarmSearchIndex(result)
    try {
      elfFunctionSymbols.value = buildFunctionSymbols(parseElf32Symbols(buffer).symbols)
    } catch {
      elfFunctionSymbols.value = []
    }
    selectedFunctionAddress.value = null
    variableListEpoch.value += 1
    variableSearch.value = ''
    variableSearchInput.value = ''
    if (searchTimer) { window.clearTimeout(searchTimer); searchTimer = 0 }
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

async function onElfSelected(event: Event): Promise<void> {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  await parseElfFile(file)
}

// ---- Source 视图（SourceBrowser）与 Variables 视图联动 ---------------------
/** SourceBrowser 选好项目目录后保存：切换 Variables/Source 视图时不丢项目（无需重新选目录）。 */
function onSourceProjectLoaded(project: SourceProject): void {
  sourceProject.value = project
}

/** SourceBrowser 自动发现/手工选择的 ELF：与 ELF 卡片上传走同一解析流程。 */
async function onSourceElfFile(file: File): Promise<void> {
  await parseElfFile(file)
}

/**
 * 源码中点击变量：语义与变量树勾选一致。
 *  - 本地采样运行中锁定（现有曲线不允许中途改）；
 *  - 当前采样模式不可采样 -> 提示；
 *  - 已选（key 相同）-> 去重提示，不重复添加；
 *  - 其余复用 newSeries（系列上限/重复/通知均由现有逻辑处理）。
 */
function onSourceVariableSelected(payload: SourceVariableSelectionPayload<VariableNode>): void {
  if (isLocalSource.value && sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  const node = payload.variable
  if (!nodeSelectable(node)) {
    globalNotify(translate('curve.sourceSelectUnsamplable'), 'warning')
    return
  }
  if (seriesList.value.some((item) => item.key === payload.key)) {
    globalNotify(translate('curve.sourceAlreadySelected'), 'info')
    return
  }
  if (newSeries(payload.path, payload.key, node.typeName, node)) activeSourceKey.value = payload.key
}

/** SourceBrowser 实时值：key 与曲线 key 一致，低频（stats flush）刷新。 */
const sourceLiveValues = computed<SourceLiveValueMap>(() => {
  void sourceLivePulse.value
  const map: Record<string, {text: string; color?: string; stale?: boolean}> = {}
  for (const item of seriesList.value) {
    const text = latestText(item)
    map[item.key] = {
      text,
      color: seriesDisplayColor(item.color),
      stale: !sampling.value || text === '-' || !seriesActiveAtPc(item, activePc.value),
    }
  }
  return map
})

/** task-42：精确行表 file:line -> 可执行地址（不再用函数 low_pc/变量地址近似）。 */
const sourceLineAddresses = computed<SourceLineAddressMap>(() => {
  const index = lineIndex.value
  if (!index) return {}
  const map: Record<string, Record<number, number>> = {}
  for (const row of index.rows) {
    if (!row.flash) continue
    const file = index.files[row.fileIndex]
    if (!file) continue
    const path = (file.relativePath || file.path || '').replace(/\\/g, '/')
    if (!path) continue
    if (!map[path]) map[path] = {}
    const previous = map[path][row.line]
    if (previous === undefined || row.isStmt) map[path][row.line] = row.address
  }
  return map
})

function jumpToSource(key: string): void {
  elfView.value = 'source'
  sourceViewEverOpened.value = true
  void nextTick(() => {
    void sourceBrowserRef.value?.scrollToVariableByKey(key)
  })
}

/** task-29 接口：点击源码行号 gutter -> 本地断点模式 + 地址（需本地采样支持）。 */
function onSourceLineClick(payload: SourceLineClickPayload): void {
  if (sampling.value) {
    globalNotify(translate('curve.localSeriesLocked'), 'warning')
    return
  }
  let address = payload.address
  if (address === undefined && lineIndex.value) {
    const files = findElfLineFiles(lineIndex.value, payload.path)
    const best = files.length > 0 ? fileLineToAddress(lineIndex.value, files[0], payload.line) : null
    address = best?.address
  }
  if (address === undefined) {
    globalNotify(translate('curve.sourceLineNoLineTable'), 'warning')
    return
  }
  samplingSource.value = 'local'
  localTrigger.value = 'breakpoint'
  localBreakpointInput.value = formatHex32(address >>> 0)
  selectedFunctionAddress.value = null
  localBreakpointOffsetInput.value = ''
  globalNotify(`${translate('curve.sourceBreakpointSet')} ${formatHex32(address >>> 0)}`, 'success')
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
    const addLimit = isLocalSource.value ? localSeriesLimit : MAX_CURVE_SERIES
    if (seriesList.value.length >= addLimit) {
      const message = isLocalSource.value
        ? translate('curve.localSeriesLimit', {count: addLimit})
        : translate('curve.maxSeries', {max: addLimit})
      globalNotify(message, 'warning')
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
  let value: number
  if (item.format === 'u8') value = lo & 0xff
  else if (item.format === 'i8') value = (lo << 24) >> 24
  else if (item.format === 'u16') value = lo & 0xffff
  else if (item.format === 'i16') value = (lo << 16) >> 16
  else if (item.format === 'u32') value = lo >>> 0
  else if (item.format === 'i32') value = lo | 0
  else if (item.format === 'f32') value = f32FromU32(lo)
  else if (item.format === 'hex') value = lo >>> 0
  else if (item.format === 'bool') value = (lo | hi) !== 0 ? 1 : 0
  else if (item.format === 'enum') value = Number(bitsValue(lo, hi, sampleBitWidth(item), false))
  else {
    const bits = sampleBitWidth(item)
    switch (item.sample.kind) {
      case 'float32':
        value = f32FromU32(lo)
        break
      case 'float64':
        value = f64FromLoHi(lo, hi)
        break
      case 'bool':
        value = bitsValue(lo, hi, bits, false) !== 0n ? 1 : 0
        break
      case 'int':
        value = Number(bitsValue(lo, hi, bits, true))
        break
      case 'enum':
        value = Number(bitsValue(lo, hi, bits, false))
        break
      case 'raw':
        value = lo >>> 0
        break
      default:
        value = Number(bitsValue(lo, hi, bits, false))
    }
  }
  return applyValueScale(value)
}

function formatFloatText(value: number): string {
  if (!Number.isFinite(value)) return String(value)
  return String(Number.parseFloat(value.toPrecision(7)))
}

function formatAxisValue(value: number): string {
  if (!Number.isFinite(value)) return ''
  const abs = Math.abs(value)
  if (abs >= 1e6 || (abs > 0 && abs < 1e-3)) return value.toExponential(2)
  if (Number.isInteger(value)) return String(value)
  return value.toFixed(2)
}

function decodeText(item: CurveSeries, lo: number, hi: number): string {
  if (item.format === 'u8') return String(applyValueScale(lo & 0xff))
  if (item.format === 'i8') return String(applyValueScale((lo << 24) >> 24))
  if (item.format === 'u16') return String(applyValueScale(lo & 0xffff))
  if (item.format === 'i16') return String(applyValueScale((lo << 16) >> 16))
  if (item.format === 'u32') return String(applyValueScale(lo >>> 0))
  if (item.format === 'i32') return String(applyValueScale(lo | 0))
  if (item.format === 'f32') return formatFloatText(applyValueScale(f32FromU32(lo)))
  if (item.format === 'hex') {
    const bits = sampleBitWidth(item)
    if (bits <= 32) return `0x${(lo >>> 0).toString(16).toUpperCase()}`
    return `0x${toBigInt(lo, hi).toString(16).toUpperCase()}`
  }
  if (item.format === 'bool') return (lo | hi) !== 0 ? 'true' : 'false'
  if (item.format === 'enum') {
    const value = Number(bitsValue(lo, hi, sampleBitWidth(item), false))
    const enumerator = item.sample.enumValues?.find((entry) => entry[0] === value)
    return enumerator ? `${enumerator[1]} (${value})` : String(value)
  }
  const bits = sampleBitWidth(item)
  switch (item.sample.kind) {
    case 'float32':
      return formatFloatText(applyValueScale(f32FromU32(lo)))
    case 'float64':
      return formatFloatText(applyValueScale(f64FromLoHi(lo, hi)))
    case 'bool':
      return bitsValue(lo, hi, bits, false) !== 0n ? 'true' : 'false'
    case 'enum': {
      const value = Number(bitsValue(lo, hi, bits, false))
      const enumerator = item.sample.enumValues?.find((entry) => entry[0] === value)
      return enumerator ? `${enumerator[1]} (${value})` : String(value)
    }
    case 'int':
      return String(applyValueScale(Number(bitsValue(lo, hi, bits, true))))
    case 'raw': {
      if (bits <= 32) return `0x${(lo >>> 0).toString(16).toUpperCase()}`
      return `0x${toBigInt(lo, hi).toString(16).toUpperCase()}`
    }
    default:
      return String(applyValueScale(Number(bitsValue(lo, hi, bits, false))))
  }
}

function latestText(item: CurveSeries): string {
  if (ringCount === 0) return '-'
  if (item.derived) {
    const values = seriesValues.get(item.id)
    if (!values) return '-'
    for (let order = ringCount - 1; order >= 0; order--) {
      const slot = ringSlot(order)
      if (slotSeq[slot] < item.startSeq) continue
      return formatFloatText(values[slot])
    }
    return '-'
  }
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
/** 列缓冲复用：min/max 降采样时避免每帧分配大数组。 */
let columnMin = new Float32Array(0)
let columnMax = new Float32Array(0)
/** 每帧预计算一次 X 像素坐标（所有 series 共享时间轴），避免每系列重复 toX。 */
let xCache = new Float32Array(0)
let hoverMovePending = false
let pendingMove: MouseEvent | null = null

function scheduleDraw(): void {
  chartDirty = true
  if (chartFrozen.value) return
  // document.hidden 时暂停绘制；visibilitychange 恢复后补一次。
  if (document.hidden) {
    perfCounter.hiddenPauses += 1
    return
  }
  if (drawPending) return
  drawPending = true
  window.requestAnimationFrame(drawFrame)
}

function toggleFreeze(): void {
  chartFrozen.value = !chartFrozen.value
  if (!chartFrozen.value) {
    invalidateYRange()
    scheduleDraw()
  }
}

/** 自适应 FPS：100+ 曲线（尤其 A 模式）降低重绘频率，防止主线程过载。 */
function adaptiveDrawIntervalMs(): number {
  // task-33：视觉刷新目标 ≥30fps；绘制成本由 dirty/cache/降采样控制，不再按曲线数降帧。
  return DRAW_INTERVAL_MS
}

/** 约 30fps 节流：数据进入 ring 的频率可以更高，但主线程绘制降到 ~30fps。 */
function drawFrame(now: number): void {
  drawPending = false
  if (document.hidden) return
  if (!chartDirty) return
  if (now - lastDrawAt < adaptiveDrawIntervalMs()) {
    drawPending = true
    window.requestAnimationFrame(drawFrame)
    return
  }
  lastDrawAt = now
  chartDirty = false
  drawChart(now)
}

function prepareCanvas(canvas: HTMLCanvasElement, width: number, height: number, dpr: number): CanvasRenderingContext2D | null {
  const pixelWidth = Math.max(1, Math.round(width * dpr))
  const pixelHeight = Math.max(1, Math.round(height * dpr))
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth
    canvas.height = pixelHeight
  }
  const context = canvas.getContext('2d')
  if (!context) return null
  context.setTransform(dpr, 0, 0, dpr, 0, 0)
  context.clearRect(0, 0, width, height)
  return context
}

/** Y 自动缩放：TTL 缓存 + 抽样预算（20k 值），曲线很多时不逐点全扫。 */
function updateYRange(drawn: readonly CurveSeries[], startOrder: number, now: number): {min: number; max: number} | null {
  if (!yRangeCache.dirty && now - yRangeCache.updatedAt < Y_RANGE_TTL_MS
    && yRangeCache.seriesCount === drawn.length) {
    return {min: yRangeCache.min, max: yRangeCache.max}
  }
  const total = Math.max(1, (ringCount - startOrder) * Math.max(1, drawn.length))
  const stride = Math.max(1, Math.ceil(total / Y_SCAN_BUDGET))
  let minValue = Number.POSITIVE_INFINITY
  let maxValue = Number.NEGATIVE_INFINITY
  for (const item of drawn) {
    const values = seriesValues.get(item.id)
    if (!values) continue
    for (let order = startOrder; order < ringCount; order += stride) {
      const slot = ringSlot(order)
      if (slotSeq[slot] < item.startSeq) continue
      const value = values[slot]
      if (!Number.isFinite(value)) continue
      if (value < minValue) minValue = value
      if (value > maxValue) maxValue = value
    }
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
  yRangeCache = {min: minValue, max: maxValue, updatedAt: now, seriesCount: drawn.length, dirty: false}
  return {min: minValue, max: maxValue}
}

/** Canvas 颜色统一从 design-tokens 读取（--bb-chart-*），主题切换后缓存失效。 */
function chartPalette(): { bg: string; grid: string; text: string; axis: string } {
  if (chartPaletteCache) return chartPaletteCache
  const style = getComputedStyle(document.documentElement)
  const dark = darkTheme.value
  const read = (name: string, fallback: string) => style.getPropertyValue(name).trim() || fallback
  chartPaletteCache = {
    bg: read('--bb-chart-bg', dark ? '#0f1216' : '#ffffff'),
    grid: read('--bb-chart-grid', dark ? 'rgba(255, 255, 255, 0.07)' : 'rgba(16, 24, 40, 0.08)'),
    text: read('--bb-chart-text', dark ? '#7d8795' : '#69727f'),
    axis: read('--bb-chart-axis', dark ? '#3a4350' : '#c3c9d3'),
  }
  return chartPaletteCache
}

/** 曲线显示色：浅色主题用原调色板；深色主题用提亮版本（不改 series.color 状态/导出）。 */
function seriesDisplayColor(color: string): string {
  return darkTheme.value ? (DARK_COLOR_BY_BASE.get(color) ?? color) : color
}

/** 当前数据最后时间戳（ring 非响应式，绘制/交互时直接读取）。 */
function lastDataTimestamp(): number {
  return ringCount > 0 ? timestamps[ringSlot(ringCount - 1)] : 0
}

/** 当前 X 视图范围：跟随最新或固定右端；窗口受 ring 内数据范围约束。 */
function currentXRange(lastTime = lastDataTimestamp()): {xMin: number; xMax: number} {
  const win = timeWindowSeconds.value
  const first = ringCount > 0 ? timestamps[ringSlot(0)] : 0
  const minSpan = Math.max(first + win, win)
  let xMax: number
  if (followLatest.value) {
    xMax = Math.max(lastTime, Math.min(win, 5))
  } else {
    const desired = viewEndTime.value > 0 ? viewEndTime.value : lastTime
    xMax = Math.min(Math.max(desired, minSpan), Math.max(lastTime, minSpan))
  }
  xMax = Math.max(xMax, minSpan)
  const xMin = Math.max(first, xMax - win)
  return {xMin, xMax}
}

/** 归一化模式下每个变量在窗口内的 min/max（250ms 缓存 + 每变量抽样预算）。 */
function normalizedRangeFor(item: CurveSeries, startOrder: number, now: number): {min: number; max: number} | null {
  const cached = normalizedRangeCache.get(item.id)
  if (cached && now - cached.updatedAt < Y_RANGE_TTL_MS && cached.count === ringCount - startOrder) {
    return {min: cached.min, max: cached.max}
  }
  const values = seriesValues.get(item.id)
  if (!values) return null
  const total = Math.max(1, ringCount - startOrder)
  const stride = Math.max(1, Math.ceil(total / 4000))
  let min = Number.POSITIVE_INFINITY
  let max = Number.NEGATIVE_INFINITY
  for (let order = startOrder; order < ringCount; order += stride) {
    const slot = ringSlot(order)
    if (slotSeq[slot] < item.startSeq) continue
    const value = values[slot]
    if (!Number.isFinite(value)) continue
    if (value < min) min = value
    if (value > max) max = value
  }
  if (!Number.isFinite(min) || !Number.isFinite(max)) return null
  if (max - min < 1e-12) max = min + 1
  normalizedRangeCache.set(item.id, {min, max, updatedAt: now, count: ringCount - startOrder})
  return {min, max}
}

/** Y 轴范围：normalized 固定 0..100；log 在 log10 域；auto 为原始共享值。 */
function computeYRange(
  drawn: readonly CurveSeries[],
  startOrder: number,
  now: number,
  normalizedRanges: Map<number, {min: number; max: number}>,
): {min: number; max: number} | null {
  if (yAxisMode.value === 'normalized') return {min: 0, max: 100}
  const total = Math.max(1, (ringCount - startOrder) * Math.max(1, drawn.length))
  const stride = Math.max(1, Math.ceil(total / Y_SCAN_BUDGET))
  let minValue = Number.POSITIVE_INFINITY
  let maxValue = Number.NEGATIVE_INFINITY
  for (const item of drawn) {
    const values = seriesValues.get(item.id)
    if (!values) continue
    for (let order = startOrder; order < ringCount; order += stride) {
      const slot = ringSlot(order)
      if (slotSeq[slot] < item.startSeq) continue
      const raw = values[slot]
      if (!Number.isFinite(raw)) continue
      const value = yAxisMode.value === 'log'
        ? (raw > 0 ? Math.log10(raw) : Number.NaN)
        : raw
      if (!Number.isFinite(value)) continue
      if (value < minValue) minValue = value
      if (value > maxValue) maxValue = value
    }
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
  return {min: minValue, max: maxValue}
}

/** 原始数值 -> Y 轴数值（normalized/log/auto）。 */
function transformYValue(
  item: CurveSeries,
  raw: number,
  normalizedRanges?: Map<number, {min: number; max: number}>,
): number | null {
  if (!Number.isFinite(raw)) return null
  if (yAxisMode.value === 'auto') return raw
  if (yAxisMode.value === 'log') return raw > 0 ? Math.log10(raw) : null
  const range = normalizedRanges?.get(item.id)
  if (!range) return null
  const span = range.max - range.min
  if (span < 1e-12) return 50
  return ((raw - range.min) / span) * 100
}

function formatYTick(value: number): string {
  if (yAxisMode.value === 'normalized') return `${Math.round(value)}%`
  if (yAxisMode.value === 'log') return formatAxisValue(Math.pow(10, value))
  return formatAxisValue(value)
}

// ---- OffscreenCanvas 图表 Worker（task-47；失败自动回退主线程 drawChartLocal） ----
interface ChartWorkerSeries {id: string; color: string; values: Float32Array}
interface ChartWorkerScene {
  requestId: number
  width: number
  height: number
  dpr: number
  padding: {left: number; right: number; top: number; bottom: number}
  xMin: number
  xMax: number
  times: Float32Array
  series: ChartWorkerSeries[]
  yMode: YAxisMode
  smoothingWindow: number
  palette: {bg: string; grid: string; text: string; axis: string}
}
interface ChartWorkerGeometry {
  padding: {left: number; right: number; top: number; bottom: number}
  plotWidth: number; plotHeight: number
  xMin: number; xMax: number
  minValue: number; maxValue: number
  yMode: YAxisMode
  ranges: Record<string, [number, number]>
}
let chartWorker: Worker | null = null
let chartWorkerReady = false
let chartWorkerFailed = false
let chartWorkerInFlight = false
let chartWorkerSuspended = false
let chartRequestId = 0
const chartRenderMode = ref<'offscreen' | 'main'>('main')

function initChartWorker(): void {
  if (typeof Worker === 'undefined' || typeof OffscreenCanvas === 'undefined') {
    chartRenderMode.value = 'main'
    return
  }
  try {
    const worker = new CurveChartWorker()
    chartWorker = worker
    worker.onmessage = (event: MessageEvent<{
      type: 'bitmap' | 'error' | 'geometry'
      requestId: number
      bitmap?: ImageBitmap
      drawMs?: number
      error?: string
      geometry?: ChartWorkerGeometry
    }>) => {
      const message = event.data
      if (message.type === 'error') {
        chartWorkerFailed = true
        chartWorkerReady = false
        chartRenderMode.value = 'main'
        chartWorkerInFlight = false
        chartDirty = true
        scheduleDraw()
        return
      }
      if (message.type !== 'bitmap') return
      const geometry = message.geometry
      if (!geometry) {
        // Worker 无法产出几何（空窗口/全部 NaN）：结束 in-flight 并回退主线程。
        chartWorkerInFlight = false
        if (message.bitmap) message.bitmap.close()
        chartDirty = true
        drawChartLocal()
        return
      }
      const canvas = canvasRef.value
      const wrap = chartWrapRef.value
      if (canvas && wrap) {
        const width = Math.max(1, Math.floor(wrap.clientWidth))
        const height = Math.max(1, Math.floor(wrap.clientHeight))
        const dpr = Math.min(2, window.devicePixelRatio || 1)
        const pixelWidth = Math.max(1, Math.round(width * dpr))
        const pixelHeight = Math.max(1, Math.round(height * dpr))
        if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
          canvas.width = pixelWidth
          canvas.height = pixelHeight
        }
        const context = canvas.getContext('2d')
        if (context && message.bitmap) {
          context.setTransform(1, 0, 0, 1, 0, 0)
          context.clearRect(0, 0, canvas.width, canvas.height)
          context.drawImage(message.bitmap, 0, 0, canvas.width, canvas.height)
          message.bitmap.close()
        }
        const normalizedRanges = new Map<number, {min: number; max: number}>()
        for (const [id, range] of Object.entries(geometry.ranges)) {
          normalizedRanges.set(Number(id), {min: range[0], max: range[1]})
        }
        lastGeometry = {
          padding: geometry.padding,
          plotWidth: geometry.plotWidth,
          plotHeight: geometry.plotHeight,
          xMin: geometry.xMin,
          xMax: geometry.xMax,
          minValue: geometry.minValue,
          maxValue: geometry.maxValue,
          yAxisMode: geometry.yMode,
          normalizedRanges,
          width,
          height,
        }
      }
      perfCounter.draws += 1
      perfCounter.lastDrawMs = message.drawMs ?? 0
      perfCounter.drawMsTotal += message.drawMs ?? 0
      chartWorkerInFlight = false
      drawHoverOverlay()
      if (chartDirty) scheduleDraw()
    }
    worker.onerror = () => {
      chartWorkerFailed = true
      chartWorkerReady = false
      chartRenderMode.value = 'main'
      chartWorkerInFlight = false
      chartWorker = null
      chartDirty = true
      scheduleDraw()
    }
    chartWorkerReady = true
    chartRenderMode.value = 'offscreen'
  } catch {
    chartWorkerFailed = true
    chartWorkerReady = false
    chartRenderMode.value = 'main'
  }
}

function sendChartToWorker(): boolean {
  const worker = chartWorker
  const canvas = canvasRef.value
  const wrap = chartWrapRef.value
  if (!worker || !chartWorkerReady || chartWorkerFailed || chartWorkerSuspended || !canvas || !wrap) return false
  const visible = visibleSeriesList.value
  const drawn = drawnSeriesList.value
  if (ringCount === 0 || visible.length === 0 || drawn.length === 0) return false
  // 大系列数时 OffscreenCanvas 场景传输/软件光栅在部分环境会退化；
  // 阈值内走 worker，超过后使用已优化的主线程 drawChartLocal。
  if (drawn.length > 48) return false
  const width = Math.max(1, Math.floor(wrap.clientWidth))
  const height = Math.max(1, Math.floor(wrap.clientHeight))
  if (width < 40 || height < 40) return false
  const lastTime = lastDataTimestamp()
  const {xMin, xMax} = currentXRange(lastTime)
  const startOrder = lowerBoundTime(xMin)
  if (startOrder >= ringCount) return false
  const count = ringCount - startOrder
  const times = new Float32Array(count)
  for (let index = 0; index < count; index++) times[index] = timestamps[ringSlot(startOrder + index)]
  const series: ChartWorkerSeries[] = []
  const transfers: Transferable[] = [times.buffer]
  for (const item of drawn) {
    const values = seriesValues.get(item.id)
    if (!values) continue
    const snapshot = new Float32Array(count)
    for (let index = 0; index < count; index++) {
      const slot = ringSlot(startOrder + index)
      snapshot[index] = slotSeq[slot] >= item.startSeq && Number.isFinite(values[slot]) ? values[slot] : Number.NaN
    }
    series.push({id: String(item.id), color: seriesDisplayColor(item.color), values: snapshot})
    transfers.push(snapshot.buffer)
  }
  if (series.length === 0) return false
  const palette = chartPalette()
  const scene: ChartWorkerScene = {
    requestId: ++chartRequestId,
    width,
    height,
    dpr: Math.min(2, window.devicePixelRatio || 1),
    padding: {left: 58, right: 14, top: 12, bottom: 26},
    xMin,
    xMax,
    times,
    series,
    yMode: yAxisMode.value,
    smoothingWindow: smoothingWindow.value,
    palette,
  }
  if (chartWorkerInFlight) {
    chartDirty = true
    return true
  }
  chartWorkerInFlight = true
  worker.postMessage(scene, transfers)
  return true
}

/** 主入口：优先 OffscreenCanvas worker；不支持/失败/冻结/导出时回退主线程。 */
function drawChart(now = performance.now()): void {
  if (!chartWorkerSuspended && !chartFrozen.value && chartWorkerReady && !chartWorkerFailed) {
    if (sendChartToWorker()) return
  }
  drawChartLocal(now)
}

function drawChartLocal(now = performance.now()): void {
  const drawStart = performance.now()
  const canvas = canvasRef.value
  const wrap = chartWrapRef.value
  if (!canvas || !wrap) return
  const width = Math.max(1, Math.floor(wrap.clientWidth))
  const height = Math.max(1, Math.floor(wrap.clientHeight))
  // DPR 上限 2：高 DPI 屏幕上避免 3x/4x 像素带来的 2~4 倍填充成本。
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  const context = prepareCanvas(canvas, width, height, dpr)
  if (!context) return
  const palette = chartPalette()
  context.fillStyle = palette.bg
  context.fillRect(0, 0, width, height)

  const padding = {left: 58, right: 14, top: 12, bottom: 26}
  const plotWidth = width - padding.left - padding.right
  const plotHeight = height - padding.top - padding.bottom
  if (plotWidth < 20 || plotHeight < 20) {
    lastGeometry = null
    drawHoverOverlay()
    return
  }

  const visible = visibleSeriesList.value
  const drawn = drawnSeriesList.value
  if (ringCount === 0 || visible.length === 0) {
    lastGeometry = null
    if (ringCount === 0) {
      // 空状态由模板 chart-empty 提示；这里只清背景。
    } else {
      context.fillStyle = palette.text
      context.font = '12px system-ui, sans-serif'
      context.textAlign = 'center'
      context.textBaseline = 'middle'
      context.fillText(translate('curve.noData'), width / 2, height / 2)
    }
    drawHoverOverlay()
    perfCounter.lastDrawMs = performance.now() - drawStart
    return
  }

  const lastTime = timestamps[ringSlot(ringCount - 1)]
  const {xMin, xMax} = currentXRange(lastTime)
  const startOrder = lowerBoundTime(xMin)
  if (startOrder >= ringCount) {
    lastGeometry = null
    drawHoverOverlay()
    return
  }
  const normalizedRanges = new Map<number, {min: number; max: number}>()
  if (yAxisMode.value === 'normalized') {
    for (const item of drawn) {
      const range = normalizedRangeFor(item, startOrder, now)
      if (range) normalizedRanges.set(item.id, range)
    }
  }
  const range = computeYRange(drawn, startOrder, now, normalizedRanges)
  if (!range) {
    lastGeometry = null
    drawHoverOverlay()
    return
  }
  const {min: minValue, max: maxValue} = range

  const toX = (time: number) => padding.left + ((time - xMin) / (xMax - xMin)) * plotWidth
  const toY = (value: number) => padding.top + (1 - (value - minValue) / (maxValue - minValue)) * plotHeight
  if (seriesList.value.some((item) => item.derived)) updateDerivedValues()
  const orderCount = ringCount - startOrder
  if (xCache.length < orderCount) xCache = new Float32Array(orderCount)
  for (let offset = 0; offset < orderCount; offset++) {
    xCache[offset] = toX(timestamps[ringSlot(startOrder + offset)])
  }

  // 网格 + Y 轴刻度
  context.font = '11px system-ui, sans-serif'
  context.textBaseline = 'middle'
  context.textAlign = 'right'
  context.lineWidth = 1
  const ySteps = 5
  for (let step = 0; step <= ySteps; step++) {
    const value = minValue + ((maxValue - minValue) * step) / ySteps
    const y = toY(value)
    context.strokeStyle = step === 0 || step === ySteps ? palette.axis : palette.grid
    context.beginPath()
    context.moveTo(padding.left, y)
    context.lineTo(padding.left + plotWidth, y)
    context.stroke()
    context.fillStyle = palette.text
    context.fillText(formatYTick(value), padding.left - 6, y)
  }

  // X 轴刻度（秒）
  context.textAlign = 'center'
  context.textBaseline = 'top'
  const timeStep = niceTimeStep(xMax - xMin)
  const firstTick = Math.ceil(xMin / timeStep) * timeStep
  for (let time = firstTick; time <= xMax + 1e-9; time += timeStep) {
    const x = toX(time)
    context.strokeStyle = palette.grid
    context.beginPath()
    context.moveTo(x, padding.top)
    context.lineTo(x, padding.top + plotHeight)
    context.stroke()
    context.fillStyle = palette.text
    context.fillText(`${time.toFixed(0)}s`, x, padding.top + plotHeight + 6)
  }

  // 数据曲线：共享窗口若点数超过列数则按列 min/max 降采样；A 模式 256 路时
  // 合并到 1/2~1/3 像素列（视觉仍连续），把路径段数从 ~28 万降到 ~9 万/帧。
  const useDecimation = orderCount > plotWidth * 1.5
  const columnScale = drawn.length > 192 ? 12 : drawn.length > 96 ? 3 : 1
  // A 模式 256 路：隔点采样（min/max 包络近似），把每帧变换/路径成本再减半。
  const orderStep = drawn.length > 192 ? 3 : 1
  const plotColumns = Math.max(1, Math.ceil(plotWidth / columnScale))
  if (columnMin.length < plotColumns) {
    columnMin = new Float32Array(plotColumns)
    columnMax = new Float32Array(plotColumns)
  }
  context.lineWidth = 1.5
  const yMode = yAxisMode.value
  // 同色曲线合批：最多 24 种颜色 -> 每帧 ~24 次 stroke，而不是 256 次。
  const byColor = new Map<string, CurveSeries[]>()
  for (const item of drawn) {
    if (!seriesValues.get(item.id)) continue
    const color = seriesDisplayColor(item.color)
    const bucket = byColor.get(color)
    if (bucket) bucket.push(item)
    else byColor.set(color, [item])
  }
  for (const [color, items] of byColor) {
    context.strokeStyle = color
    context.beginPath()
    for (const item of items) {
      const values = seriesValues.get(item.id)
      if (!values) continue
      if (!useDecimation) {
        let started = false
        const smoothQueue: number[] = []
        for (let order = startOrder; order < ringCount; order++) {
          const slot = ringSlot(order)
          if (slotSeq[slot] < item.startSeq) {
            started = false
            continue
          }
          const raw = values[slot]
          const value = yMode === 'auto' ? (Number.isFinite(raw) ? raw : null) : transformYValue(item, raw, normalizedRanges)
          if (value === null) {
            started = false
            continue
          }
          let drawValue = value
          if (smoothingWindow.value > 1) {
            smoothQueue.push(value)
            if (smoothQueue.length > smoothingWindow.value) smoothQueue.shift()
            drawValue = smoothQueue.reduce((sum, itemValue) => sum + itemValue, 0) / smoothQueue.length
          }
          const x = xCache[order - startOrder]
          const y = toY(drawValue)
          if (!started) {
            context.moveTo(x, y)
            started = true
          } else {
            context.lineTo(x, y)
          }
        }
      } else {
        const minBuf = columnMin
        const maxBuf = columnMax
        const smoothQueue: number[] = []
        for (let column = 0; column < plotColumns; column++) {
          minBuf[column] = Number.POSITIVE_INFINITY
          maxBuf[column] = Number.NEGATIVE_INFINITY
        }
        for (let order = startOrder; order < ringCount; order += orderStep) {
          const slot = ringSlot(order)
          if (slotSeq[slot] < item.startSeq) continue
          const raw = values[slot]
          const value = yMode === 'auto' ? (Number.isFinite(raw) ? raw : null) : transformYValue(item, raw, normalizedRanges)
          if (value === null) continue
          let drawValue = value
          if (smoothingWindow.value > 1) {
            smoothQueue.push(value)
            if (smoothQueue.length > smoothingWindow.value) smoothQueue.shift()
            drawValue = smoothQueue.reduce((sum, itemValue) => sum + itemValue, 0) / smoothQueue.length
          }
          const column = Math.min(plotColumns - 1, Math.max(0, Math.floor((xCache[order - startOrder] - padding.left) / columnScale)))
          if (drawValue < minBuf[column]) minBuf[column] = drawValue
          if (drawValue > maxBuf[column]) maxBuf[column] = drawValue
        }
        let previousColumn = -2
        for (let column = 0; column < plotColumns; column++) {
          if (!Number.isFinite(minBuf[column])) continue
          const x = padding.left + column * columnScale + columnScale * 0.5
          const yTop = toY(maxBuf[column])
          const yBottom = toY(minBuf[column])
          if (column !== previousColumn + 1) context.moveTo(x, yTop)
          else context.lineTo(x, yTop)
          context.lineTo(x, yBottom)
          previousColumn = column
        }
      }
    }
    context.stroke()
  }

  lastGeometry = {padding, plotWidth, plotHeight, xMin, xMax, minValue, maxValue, yAxisMode: yAxisMode.value,
    normalizedRanges, width, height}
  drawHoverOverlay()
  perfCounter.draws += 1
  perfCounter.lastDrawMs = performance.now() - drawStart
  perfCounter.drawMsTotal += perfCounter.lastDrawMs
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

/** hover 独立 overlay：鼠标移动只重绘 overlay，不重绘主曲线/不重建 canvas。 */
function drawHoverOverlay(): void {
  const overlay = hoverCanvasRef.value
  const wrap = chartWrapRef.value
  const geometry = lastGeometry
  if (!overlay || !wrap) return
  const width = Math.max(1, Math.floor(wrap.clientWidth))
  const height = Math.max(1, Math.floor(wrap.clientHeight))
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  const context = prepareCanvas(overlay, width, height, dpr)
  if (!context) return
  const palette = chartPalette()
  const hoverState = hover.value
  if (!hoverState || !geometry || ringCount === 0) return
  const x = geometry.padding.left + ((hoverState.time - geometry.xMin) / (geometry.xMax - geometry.xMin)) * geometry.plotWidth
  context.strokeStyle = palette.text
  context.lineWidth = 1
  context.setLineDash([4, 3])
  context.beginPath()
  context.moveTo(x, geometry.padding.top)
  context.lineTo(x, geometry.padding.top + geometry.plotHeight)
  context.stroke()
  context.setLineDash([])
  const slot = ringSlot(Math.min(Math.max(hoverState.index, 0), ringCount - 1))
  for (const item of drawnSeriesList.value) {
    const values = seriesValues.get(item.id)
    if (!values || slotSeq[slot] < item.startSeq) continue
    const value = transformYValue(item, values[slot], geometry.normalizedRanges)
    if (value === null) continue
    const y = geometry.padding.top + (1 - (value - geometry.minValue) / (geometry.maxValue - geometry.minValue)) * geometry.plotHeight
    context.fillStyle = seriesDisplayColor(item.color)
    context.beginPath()
    context.arc(x, y, 3, 0, Math.PI * 2)
    context.fill()
  }
  // task-37：双光标 A/B 竖线（overlay）。
  for (const cursor of [
    {point: cursorA.value, color: '#0ea5e9', label: 'A'},
    {point: cursorB.value, color: '#f59e0b', label: 'B'},
  ]) {
    if (!cursor.point) continue
    const cursorX = geometry.padding.left + ((cursor.point.time - geometry.xMin) / (geometry.xMax - geometry.xMin)) * geometry.plotWidth
    if (cursorX < geometry.padding.left || cursorX > geometry.padding.left + geometry.plotWidth) continue
    context.strokeStyle = cursor.color
    context.lineWidth = 1
    context.setLineDash([2, 3])
    context.beginPath()
    context.moveTo(cursorX, geometry.padding.top)
    context.lineTo(cursorX, geometry.padding.top + geometry.plotHeight)
    context.stroke()
    context.setLineDash([])
    context.fillStyle = cursor.color
    context.font = '10px system-ui, sans-serif'
    context.textAlign = 'left'
    context.fillText(cursor.label, cursorX + 2, geometry.padding.top + 10)
  }
}

function scheduleHoverDraw(): void {
  if (hoverDrawPending) return
  hoverDrawPending = true
  window.requestAnimationFrame(() => {
    hoverDrawPending = false
    drawHoverOverlay()
  })
}

function processCanvasMove(event: MouseEvent): void {
  const canvas = canvasRef.value
  if (!canvas || ringCount === 0 || seriesList.value.length === 0) return
  const rect = canvas.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top
  const padding = {left: 58, right: 14, top: 12, bottom: 26}
  const plotWidth = rect.width - padding.left - padding.right
  if (plotWidth <= 0 || x < padding.left || x > rect.width - padding.right) {
    if (hover.value) {
      hover.value = null
      scheduleHoverDraw()
    }
    return
  }
  const lastTime = lastDataTimestamp()
  const geometry = lastGeometry
  const fallback = currentXRange(lastTime)
  const xMin = geometry?.xMin ?? fallback.xMin
  const xMax = geometry?.xMax ?? fallback.xMax
  const time = xMin + ((x - padding.left) / plotWidth) * (xMax - xMin)
  const startOrder = lowerBoundTime(xMin)
  const order = Math.min(Math.max(lowerBoundTime(time), startOrder), ringCount - 1)
  const slot = ringSlot(order)
  const entries: Array<{name: string; color: string; text: string}> = []
  const drawn = drawnSeriesList.value
  for (const item of drawn) {
    if (!item.visible || slotSeq[slot] < item.startSeq) continue
    const loBuffer = seriesLo.get(item.id)
    const hiBuffer = seriesHi.get(item.id)
    if (!loBuffer || !hiBuffer) continue
    entries.push({name: item.name, color: seriesDisplayColor(item.color), text: decodeText(item, loBuffer[slot], hiBuffer[slot])})
    if (entries.length >= 16) break
  }
  hover.value = {index: order, x, y, time: timestamps[slot], entries}
  scheduleHoverDraw()
}

function onCanvasMove(event: MouseEvent): void {
  pendingMove = event
  if (hoverMovePending) return
  hoverMovePending = true
  window.requestAnimationFrame(() => {
    hoverMovePending = false
    if (pendingMove) {
      const move = pendingMove
      pendingMove = null
      processCanvasMove(move)
    }
  })
}

function onCanvasLeave(): void {
  pendingMove = null
  if (!hover.value) return
  hover.value = null
  scheduleHoverDraw()
}

// ---- X 时间轴交互：滚轮缩放 / 拖拽平移 / 双击复位 / 跟随最新 ---------------
let panning = false
let panMoved = false
let panStartX = 0
let panStartEnd = 0
let activePointerId: number | null = null

function clampViewEnd(value: number): number {
  const win = timeWindowSeconds.value
  const first = ringCount > 0 ? timestamps[ringSlot(0)] : 0
  const last = lastDataTimestamp()
  const minEnd = Math.max(first + win, win)
  const maxEnd = Math.max(last, minEnd)
  return Math.min(Math.max(value, minEnd), maxEnd)
}

function onCanvasWheel(event: WheelEvent): void {
  if (ringCount === 0) return
  const canvas = canvasRef.value
  if (!canvas) return
  const currentIndex = TIME_WINDOW_OPTIONS.indexOf(timeWindowSeconds.value)
  const direction = event.deltaY > 0 ? 1 : -1
  const nextIndex = Math.min(TIME_WINDOW_OPTIONS.length - 1, Math.max(0, currentIndex + direction))
  if (nextIndex === currentIndex) return
  const nextWindow = TIME_WINDOW_OPTIONS[nextIndex]
  const rect = canvas.getBoundingClientRect()
  const paddingLeft = 58
  const paddingRight = 14
  const plotWidth = Math.max(1, rect.width - paddingLeft - paddingRight)
  const fraction = Math.min(1, Math.max(0, (event.clientX - rect.left - paddingLeft) / plotWidth))
  const before = currentXRange(lastDataTimestamp())
  const centerTime = before.xMin + fraction * (before.xMax - before.xMin)
  if (!followLatest.value) {
    // 以鼠标位置为中心缩放；若在跟随时则保持跟随最新。
    followLatest.value = false
    viewEndTime.value = clampViewEnd(centerTime + nextWindow * (1 - fraction))
  }
  timeWindowSeconds.value = nextWindow
  scheduleDraw()
}

function resetTimeView(): void {
  followLatest.value = true
  timeWindowSeconds.value = 20
  viewEndTime.value = 0
  invalidateYRange()
  scheduleDraw()
}

function onCanvasPointerDown(event: PointerEvent): void {
  if (event.button !== 0 || ringCount === 0) return
  const canvas = canvasRef.value
  if (!canvas) return
  panning = true
  panStartX = event.clientX
  panStartEnd = currentXRange(lastDataTimestamp()).xMax
  activePointerId = event.pointerId
  try { canvas.setPointerCapture(event.pointerId) } catch { /* ignore */ }
  event.preventDefault()
}

function onCanvasPointerMove(event: PointerEvent): void {
  if (panning) {
    const canvas = canvasRef.value
    if (!canvas) return
    const dx = event.clientX - panStartX
    if (Math.abs(dx) > 2) {
      panMoved = true
      const rect = canvas.getBoundingClientRect()
      const plotWidth = Math.max(1, rect.width - 58 - 14)
      const secondsPerPixel = timeWindowSeconds.value / plotWidth
      followLatest.value = false
      viewEndTime.value = clampViewEnd(panStartEnd - dx * secondsPerPixel)
      if (hover.value) {
        hover.value = null
        scheduleHoverDraw()
      }
      scheduleDraw()
    }
    return
  }
  onCanvasMove(event)
}

function onCanvasPointerUp(event: PointerEvent): void {
  if (!panning) return
  panning = false
  const canvas = canvasRef.value
  if (canvas && activePointerId !== null) {
    try { canvas.releasePointerCapture(activePointerId) } catch { /* ignore */ }
  }
  activePointerId = null
  event.preventDefault()
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
  // 导出当前 X 视图范围（冻结/缩放/平移后所见即所得）；始终带 group 列。
  const lastTime = lastDataTimestamp()
  const view = lastGeometry
    ? {xMin: lastGeometry.xMin, xMax: lastGeometry.xMax}
    : currentXRange(lastTime)
  const startOrder = Math.max(0, lowerBoundTime(view.xMin))
  const endOrder = Math.max(startOrder, lowerBoundTime(view.xMax + 1e-9))
  const header = ['time_s', 'group', ...seriesList.value.map((item) => csvEscape(legendLabel(item)))]
  const lines: string[] = [header.join(',')]
  for (let order = startOrder; order < endOrder; order++) {
    const slot = ringSlot(order)
    const cells: string[] = [(timestamps[slot] as number).toFixed(4), String(slotGroup[slot])]
    for (const item of seriesList.value) {
      const loBuffer = seriesLo.get(item.id)
      const hiBuffer = seriesHi.get(item.id)
      if (item.derived) {
        const values = seriesValues.get(item.id)
        cells.push(values && slotSeq[slot] >= item.startSeq ? formatFloatText(values[slot]) : '')
      } else if (!loBuffer || !hiBuffer || slotSeq[slot] < item.startSeq) {
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
  chartWorkerSuspended = true
  drawChartLocal()
  const filename = `data-curve-${fileStamp()}.png`
  const restore = () => {
    hover.value = previousHover
    chartWorkerSuspended = false
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
function onVisibilityChange(): void {
  if (document.hidden) {
    perfCounter.hiddenPauses += 1
    return
  }
  // 页面回到前台：数据可能已积压，强制补一次绘制。
  invalidateYRange()
  scheduleDraw()
}

/** 可选 debug 快照，供 fake-DAP/Playwright 性能断言（不影响生产逻辑）。 */
function installDebugSnapshot(): void {
  ;(window as unknown as {__curveDebug?: () => Record<string, unknown>}).__curveDebug = () => ({
    draws: perfCounter.draws,
    pollsPerSecond: Math.round(pollCounterState.pollsPerSecond * 10) / 10,
    pollsInFlight: localPollsInFlightCount,
    drawIntervalMs: adaptiveDrawIntervalMs(),
    renderMode: chartRenderMode.value,
    lastDrawMs: Math.round(perfCounter.lastDrawMs * 100) / 100,
    avgDrawMs: perfCounter.draws > 0 ? Math.round((perfCounter.drawMsTotal / perfCounter.draws) * 100) / 100 : 0,
    drawn: drawnSeriesList.value.length,
    visible: visibleSeriesList.value.length,
    selected: seriesList.value.length,
    renderAll: renderAll.value,
    drawCap: drawCap.value,
    ringCount,
    hiddenPauses: perfCounter.hiddenPauses,
    variableRows: variableRows.value.length,
    renderedVariableRows: shownVariableRows.value.length,
    search: variableSearch.value,
    legendItems: legendItems.value.length,
  })
}

onMounted(() => {
  loadValueScale()
  loadPresets()
  void refreshRecordings()
  initFramesWorker()
  initChartWorker()
  void refreshTargetInfo()
  window.addEventListener('keydown', onGlobalKeydown)
  resizeObserver = new ResizeObserver(() => scheduleDraw())
  if (chartWrapRef.value) resizeObserver.observe(chartWrapRef.value)
  document.addEventListener('visibilitychange', onVisibilityChange)
  installDebugSnapshot()
  // 主题切换（html class/data-theme）后失效 canvas 调色板并重绘（纯视觉）。
  darkTheme.value = document.documentElement.classList.contains('dark')
  themeObserver = new MutationObserver(() => {
    const next = document.documentElement.classList.contains('dark')
    chartPaletteCache = null
    if (darkTheme.value !== next) darkTheme.value = next
    scheduleDraw()
  })
  themeObserver.observe(document.documentElement, {attributes: true, attributeFilter: ['class', 'data-theme']})
  scheduleDraw()
})

onBeforeUnmount(() => {
  stopSampling()
  document.removeEventListener('visibilitychange', onVisibilityChange)
  window.removeEventListener('keydown', onGlobalKeydown)
  stopPlayback()
  if (framesWorker) { framesWorker.terminate(); framesWorker = null }
  if (chartWorker) { chartWorker.terminate(); chartWorker = null }
  themeObserver?.disconnect()
  themeObserver = null
  chartPaletteCache = null
  delete (window as unknown as {__curveDebug?: () => Record<string, unknown>}).__curveDebug
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
.jump-source { cursor: pointer; }
.jump-source:hover { color: var(--bb-accent, #2563eb); text-decoration: underline; }

/* ==========================================================================
 * 曲线页视觉：全部颜色走 design-tokens.css（深色优先，浅色自动切回）。
 * 布局/尺寸/交互逻辑保持原样（本文件只做视觉迁移）。
 * ======================================================================== */
.curve-layout {
  @apply mx-auto w-full max-w-7xl px-2 pb-8;
  color: var(--bb-text, #151a21);
}

/* 预留 SourceBrowser 左侧 tab/selected rail：后续任务把 aside 放进 grid 第一列即可。 */
.curve-workbench {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 0.75rem;
}

.curve-workbench-main {
  min-width: 0;
}

/* 面板 / 卡片 ------------------------------------------------------------- */
.curve-card {
  @apply my-3 rounded-lg border p-3;
  background: var(--bb-panel-bg, #ffffff);
  border-color: var(--bb-panel-border, #dde1e8);
  border-radius: var(--bb-panel-radius, 10px);
  box-shadow: var(--bb-shadow-sm, 0 1px 2px rgba(16, 24, 40, 0.08));
  color: var(--bb-text, #151a21);
}

.variable-search {
  @apply min-w-56 flex-1;
}

/* ELF 卡片内 Variables / Source 视图切换（SourceBrowser 源码浏览） */
.elf-view-switch {
  @apply mx-2;
}

.source-view {
  display: flex;
  height: 560px;
  min-height: 360px;
  margin-top: 0.5rem;
}

.source-view > * {
  flex: 1 1 auto;
  min-width: 0;
}

/* 表单校验错误（断点地址等） */
.curve-field-error {
  color: var(--bb-danger, #b91c1c);
}

/* 变量表 ----------------------------------------------------------------- */
.var-table {
  @apply overflow-hidden rounded border;
  background: var(--bb-surface, #ffffff);
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text, #151a21);
}

.var-head,
.var-row {
  display: grid;
  grid-template-columns: 2.6rem minmax(0, 1.7fr) minmax(0, 1.2fr) 6.5rem 3.5rem minmax(0, 1fr);
  align-items: center;
  gap: 0.25rem;
  padding: 0 0.375rem;
}

.var-head {
  @apply sticky top-0 z-10 border-b text-xs font-semibold;
  height: 1.75rem;
  background: var(--bb-surface-inset, #eef0f4);
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text-secondary, #3d4552);
}

.var-list {
  max-height: 22rem;
  overflow: auto;
}

.var-row {
  height: 1.75rem;
  border-bottom: 1px solid var(--bb-border-subtle, #e9ecf1);
  font-size: 0.75rem;
  color: var(--bb-text, #151a21);
}

.var-row:hover {
  background-color: var(--bb-nav-link-hover-bg, rgba(16, 24, 40, 0.05));
}

.var-row.selected {
  background-color: var(--bb-accent-soft, rgba(31, 111, 235, 0.1));
  box-shadow: inset 2px 0 0 var(--bb-accent, #1f6feb);
}

.var-row.unsampleable {
  opacity: 0.62;
}

.var-row.selectable {
  cursor: pointer;
}

.unsampleable-text {
  font-size: 0.7rem;
  color: var(--bb-text-muted, #69727f);
}

/* .var-addr 的基色优先，但不可采样占位文案用弱化色 */
.var-addr.unsampleable-text {
  color: var(--bb-text-muted, #69727f);
}

.breakpoint-input.is-invalid :deep(.el-input__wrapper) {
  box-shadow: 0 0 0 1px var(--bb-danger, #b91c1c) inset;
}

.loclist-badge {
  background-color: var(--bb-warning-soft, rgba(180, 83, 9, 0.12));
  color: var(--bb-warning, #b45309);
}

.mode-badge {
  @apply inline-flex items-center rounded px-1 text-[0.6rem] font-semibold;
  background-color: var(--bb-info-soft, rgba(29, 78, 216, 0.1));
  color: var(--bb-info, #1d4ed8);
  white-space: nowrap;
}

.var-group-row {
  @apply flex items-center gap-2 border-b px-2 text-xs font-semibold;
  height: 1.5rem;
  background: var(--bb-surface-inset, #eef0f4);
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text-secondary, #3d4552);
}

.var-check {
  @apply flex items-center justify-center;
}

.var-name {
  @apply flex min-w-0 items-center gap-1;
}

.var-type,
.var-src {
  @apply min-w-0 overflow-hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--bb-text-muted, #69727f);
}

.var-addr {
  color: var(--bb-text-secondary, #3d4552);
}

.var-size {
  color: inherit;
}

.tree-arrow {
  @apply flex h-4 w-4 shrink-0 items-center justify-center text-xs;
  color: var(--bb-text-muted, #69727f);
}

.tree-arrow:hover {
  color: var(--bb-text, #151a21);
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
  grid-template-columns: 1.5rem 0.75rem minmax(0, 1.5fr) minmax(0, 1fr) 6.5rem 5.5rem 1.5rem;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.75rem;
  padding: 0.125rem 0;
  color: var(--bb-text, #151a21);
}

.series-dot {
  @apply inline-block h-2.5 w-2.5 shrink-0 rounded-full;
}

.manual-input {
  @apply min-w-64 flex-1;
}

/* 图表 ------------------------------------------------------------------- */
.chart-wrap {
  position: relative;
  @apply mt-2 h-80 w-full overflow-hidden rounded border;
  border-color: var(--bb-border, #dde1e8);
  background: var(--bb-chart-bg, #ffffff);
}

.chart-wrap canvas {
  @apply block h-full w-full;
}

.chart-overlay {
  position: absolute;
  inset: 0;
  pointer-events: none;
}

.chart-wrap canvas:first-child {
  touch-action: none;
  cursor: crosshair;
}
.group-weights {
  color: #4b5563;
}

.watch-star {
  margin-left: 0.15rem;
  border: none;
  background: transparent;
  color: #9ca3af;
  cursor: pointer;
  font-size: 0.8rem;
  line-height: 1;
}

.watch-star.active { color: #d97706; }

.stale-dot {
  color: #dc2626;
  font-size: 0.55rem;
  line-height: 1;
}

.var-row.pc-stale { opacity: 0.55; }

.stale-badge {
  background-color: #fee2e2;
  color: #b91c1c;
}

.watch-list { max-height: 10rem; overflow: auto; }

.watch-row {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(0, 1.2fr) repeat(3, minmax(0, 1fr)) 1.5rem;
  gap: 0.35rem;
  font-size: 0.72rem;
  align-items: center;
  padding: 0.1rem 0;
}

.delta-table-wrap { max-height: 14rem; overflow: auto; }

.delta-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.72rem;
}

.delta-table th,
.delta-table td {
  border-bottom: 1px solid #eef0f3;
  padding: 0.15rem 0.3rem;
  text-align: right;
  white-space: nowrap;
}

.delta-table th:first-child,
.delta-table td:first-child { text-align: left; }

.diag-json {
  max-height: 18rem;
  overflow: auto;
  border-radius: 0.375rem;
  background: #f8fafc;
  padding: 0.5rem;
  font-size: 0.7rem;
  line-height: 1.35;
}

.playback-range { flex: 1; min-width: 8rem; }

.mobile-only { display: none; }

@media (max-width: 760px) {
  .mobile-only { display: inline-flex; }
  .advanced-control { display: none !important; }
  .curve-layout.mobile-advanced .advanced-control { display: flex !important; }
}

@media (max-width: 760px) {
  .curve-layout { padding-left: 0.4rem; padding-right: 0.4rem; }
  .render-bar { flex-direction: column; align-items: flex-start; }
  .chart-wrap { height: 62vw; min-height: 220px; }
  .var-head, .var-row {
    grid-template-columns: 2.4rem minmax(0, 2fr) minmax(0, 1.2fr) 4.5rem 2.5rem minmax(0, 1fr);
    font-size: 0.7rem;
  }
  .var-list { max-height: 16rem; }
  .selected-list { max-height: 30vh; }
  .legend-item { max-width: 100%; }
  .delta-table-wrap { overflow-x: auto; }
  .page-title { font-size: 1.15rem; }
}


.render-bar {
  color: var(--bb-text-muted, #69727f);
}

.render-warning {
  color: var(--bb-warning, #b45309);
}

.show-more-row {
  @apply flex items-center justify-center gap-2 border-t py-1;
  border-color: var(--bb-border, #dde1e8);
}

.legend-more {
  @apply rounded border border-dashed px-2 py-1 text-xs;
  border-color: var(--bb-border-strong, #c3c9d3);
  color: var(--bb-text-secondary, #3d4552);
}

.legend-more:hover {
  background-color: var(--bb-nav-link-hover-bg, rgba(16, 24, 40, 0.05));
}

.chart-empty {
  @apply pointer-events-none absolute inset-0 flex items-center justify-center;
  color: var(--bb-text-muted, #69727f);
}

.chart-tooltip {
  @apply pointer-events-none absolute max-w-72 rounded border px-2 py-1 text-xs;
  background: var(--bb-surface-raised, #ffffff);
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text, #151a21);
  box-shadow: var(--bb-shadow-md, 0 10px 28px rgba(16, 24, 40, 0.1));
}

.legend {
  @apply mt-2 flex flex-wrap gap-2;
}

.legend-item {
  @apply flex max-w-96 items-center gap-1 rounded border px-2 py-1 text-xs;
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text-secondary, #3d4552);
}

.legend-item:hover {
  background-color: var(--bb-nav-link-hover-bg, rgba(16, 24, 40, 0.05));
}

.legend-item.off {
  @apply opacity-40;
}
</style>
