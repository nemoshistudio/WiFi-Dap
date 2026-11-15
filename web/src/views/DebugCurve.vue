<template>
  <div class="curve-layout">
    <h1 class="page-title">{{ translate('page.debugCurve') }}</h1>

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
        <span class="flex-1"></span>

        <el-button v-if="!sampling" type="primary" :disabled="localStartDisabled" @click="startSampling">
          {{ translate('curve.start') }}
        </el-button>
        <el-button v-else type="warning" @click="stopSampling">
          {{ translate('curve.stop') }}
        </el-button>
        <el-button :disabled="sampleCount === 0" @click="clearData">{{ translate('curve.clear') }}</el-button>
      </div>

      <div class="mt-3 flex flex-wrap items-center gap-x-6 gap-y-2 text-sm">
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
          <!-- task-55：函数快照（选函数/地址 → 断点 → 首个命中帧冻结/标记） -->
          <div v-if="localTrigger === 'breakpoint'" class="function-snapshot flex flex-wrap items-center gap-2 rounded border px-2 py-1 text-xs">
            <span class="font-semibold">Function snapshot</span>
            
            <el-checkbox v-model="snapshotFreezeOnHit" :disabled="sampling">Freeze on hit</el-checkbox>
            <el-checkbox v-model="snapshotFirmwareOneShot" :disabled="sampling" >FW one-shot</el-checkbox>
            <label v-if="snapshotFirmwareOneShot" class="flex items-center gap-1">
              <span>post frames</span>
              <el-input-number v-model="snapshotAfterFrames" :min="0" :max="CURVE_LOCAL_MAX_SNAPSHOT_AFTER" :controls="false" size="small" style="width: 4rem" :disabled="sampling"/>
            </label>
            <el-button size="small" :disabled="sampling || localStartDisabled" @click="armFunctionSnapshot">Arm snapshot</el-button>
            <el-button size="small" :disabled="sampling || (!snapshotHit && !snapshotArmed)" @click="clearSnapshotMark">Clear mark</el-button>
            <span v-if="snapshotArmed" class="snapshot-armed">ARMED · {{ snapshotFunctionName }} @ <span class="font-mono">{{ formatHex32(snapshotBoundaryAddr) }}</span></span>
            <span v-else-if="snapshotHit" class="snapshot-hit">
              HIT ≈{{ snapshotHitTime === null ? '-' : snapshotHitTime.toFixed(3) }}s
              <template v-if="snapshotHitPc !== null"> · PC <span class="font-mono">{{ formatHex32(snapshotHitPc) }}</span></template>
              <template v-if="snapshotSeq !== null"> · seq {{ snapshotSeq }}</template>
            </span>
            <span v-if="snapshotFirmwareState" class="opacity-70">
              FW: {{ snapshotFirmwareState }}<template v-if="snapshotFirmwarePreFrames !== null"> · pre {{ snapshotFirmwarePreFrames }}</template><template v-if="snapshotFirmwarePostFrames !== null"> · post {{ snapshotFirmwarePostFrames }}</template>
            </span>
            <span v-else-if="snapshotFirmwareOneShot && sampling" class="opacity-70">
              FW: no snapshot_* status yet (old firmware?) — first-hit freeze fallback
            </span>
          </div>
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
              <el-select v-model="watchAccess" size="small" style="width: 7.5rem" :disabled="sampling">
                <el-option value="write" :label="translate('curve.watchpointWrite')"/>
                <el-option value="read" :label="translate('curve.watchpointRead')"/>
                <el-option value="readwrite" :label="translate('curve.watchpointReadWrite')"/>
              </el-select>
            </label>
            <!-- task-58：第二路 DWT comparator（固件 task-57；资源不足时原样显示固件错误） -->
            <label class="watch-second-toggle inline-flex items-center gap-1">
              <el-checkbox v-model="watchSecondEnabled" :disabled="sampling">{{ translate('curve.watchpointSecond') }}</el-checkbox>
              
            </label>
            <template v-if="watchSecondEnabled">
              <span class="font-semibold text-xs">W2</span>
              <label class="flex items-center gap-2">
                <span class="whitespace-nowrap">{{ translate('curve.watchpointAddress') }}</span>
                <el-input v-model="watch2AddressInput" size="small" style="width: 9.5rem" :disabled="sampling" placeholder="0x24001238"/>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointSize') }}</span>
                <el-select v-model="watch2Size" size="small" style="width: 4.5rem" :disabled="sampling">
                  <el-option :value="1" label="1"/>
                  <el-option :value="2" label="2"/>
                  <el-option :value="4" label="4"/>
                </el-select>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointAccess') }}</span>
                <el-select v-model="watch2Access" size="small" style="width: 7.5rem" :disabled="sampling">
                  <el-option value="write" :label="translate('curve.watchpointWrite')"/>
                  <el-option value="read" :label="translate('curve.watchpointRead')"/>
                  <el-option value="readwrite" :label="translate('curve.watchpointReadWrite')"/>
                </el-select>
              </label>
            </template>
            <!-- task-62：第 3/4 路 DWT comparator（固件 task-60；最多 4 路）。资源不足时
                 固件返回 err 原文（含 DWT_CTRL/空闲 comparator 信息），原样显示在下方。 -->
            <span class="watch-extra-toggle inline-flex items-center gap-1">
              <el-checkbox v-model="watchThird.enabled" :disabled="sampling">W3</el-checkbox>
              <el-checkbox v-model="watchFourth.enabled" :disabled="sampling">W4</el-checkbox>
              
            </span>
            <template v-if="watchThird.enabled">
              <span class="font-semibold text-xs">W3</span>
              <label class="flex items-center gap-2">
                <span class="whitespace-nowrap">{{ translate('curve.watchpointAddress') }}</span>
                <el-input v-model="watchThird.addressInput" size="small" style="width: 9.5rem" :disabled="sampling" placeholder="0x2400123C"/>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointSize') }}</span>
                <el-select v-model="watchThird.size" size="small" style="width: 4.5rem" :disabled="sampling">
                  <el-option :value="1" label="1"/>
                  <el-option :value="2" label="2"/>
                  <el-option :value="4" label="4"/>
                </el-select>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointAccess') }}</span>
                <el-select v-model="watchThird.access" size="small" style="width: 7.5rem" :disabled="sampling">
                  <el-option value="write" :label="translate('curve.watchpointWrite')"/>
                  <el-option value="read" :label="translate('curve.watchpointRead')"/>
                  <el-option value="readwrite" :label="translate('curve.watchpointReadWrite')"/>
                </el-select>
              </label>
            </template>
            <template v-if="watchFourth.enabled">
              <span class="font-semibold text-xs">W4</span>
              <label class="flex items-center gap-2">
                <span class="whitespace-nowrap">{{ translate('curve.watchpointAddress') }}</span>
                <el-input v-model="watchFourth.addressInput" size="small" style="width: 9.5rem" :disabled="sampling" placeholder="0x24001240"/>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointSize') }}</span>
                <el-select v-model="watchFourth.size" size="small" style="width: 4.5rem" :disabled="sampling">
                  <el-option :value="1" label="1"/>
                  <el-option :value="2" label="2"/>
                  <el-option :value="4" label="4"/>
                </el-select>
              </label>
              <label class="flex items-center gap-2">
                <span>{{ translate('curve.watchpointAccess') }}</span>
                <el-select v-model="watchFourth.access" size="small" style="width: 7.5rem" :disabled="sampling">
                  <el-option value="write" :label="translate('curve.watchpointWrite')"/>
                  <el-option value="read" :label="translate('curve.watchpointRead')"/>
                  <el-option value="readwrite" :label="translate('curve.watchpointReadWrite')"/>
                </el-select>
              </label>
            </template>
            <span v-if="watchpointError" class="text-xs" style="color: #dc2626">{{ watchpointError }}</span>
          </template>
          <label class="flex items-center gap-2">
            <span class="whitespace-nowrap">{{ localTrigger === 'breakpoint' ? translate('curve.localMinInterval') : translate('curve.localInterval') }}</span>
            <el-select v-model="localIntervalMs" size="small" style="width: 6rem" :disabled="sampling">
              <el-option v-for="interval in LOCAL_INTERVALS" :key="interval" :value="interval" :label="interval + ' ms'"/>
            </el-select>
          </label>
          <span>{{ translate('curve.points') }}: <b>{{ sampleCount }}</b></span>
          <span>{{ translate('curve.localStatus') }}: <b>{{ (sampling && localStatusInfo?.running !== false) ? translate('curve.localRunning') : translate('curve.localStopped') }}</b></span>
          <span v-if="localTrigger === 'periodic'">
            Halt: <b>{{ localStatusInfo?.halt === false ? 'OFF' : (localStatusInfo?.halt === true ? 'ON' : '-') }}</b>
          </span>
          <span v-else-if="localTrigger === 'watchpoint'">
            {{ translate('curve.watchpointArmed') }}: <b>{{ watchpointArmed === null ? '-' : (watchpointArmed ? 'ON' : 'OFF') }}</b>
            · {{ translate('curve.watchpointHits') }}: <b>{{ watchpointHits ?? '-' }}</b>
            · {{ translate('curve.watchpointTriggerSeq') }}: <b>{{ watchTriggerSeq ?? '-' }}</b>
            · {{ translate('curve.watchpointCount') }}: <b>{{ watchCount ?? '-' }}</b>
            <template v-if="watchCount !== null && watchCount > 1">
              · {{ translate('curve.watchpoint2Armed') }}: <b>{{ watchpoint2Armed === null ? '-' : (watchpoint2Armed ? 'ON' : 'OFF') }}</b>
              · {{ translate('curve.watchpoint2Hits') }}: <b>{{ watchpoint2Hits ?? '-' }}</b>
              <template v-if="watchpoint2Address !== null"> · W2 <span class="font-mono">{{ formatHex32(watchpoint2Address) }}</span></template>
            </template>
            <template v-for="row in extraWatchStatusRows" :key="row.index">
              · {{ row.label }}: <b>{{ row.armed === null ? '-' : (row.armed ? 'ON' : 'OFF') }}</b>
              · hits <b>{{ row.hits ?? '-' }}</b>
              <template v-if="row.address !== null"> · <span class="font-mono">{{ formatHex32(row.address) }}</span></template>
            </template>
            · {{ translate('curve.watchpointLastIndex') }}: <b>{{ lastWatchIndex === null ? '-' : (lastWatchIndex === 0 ? translate('curve.watchpointLastNone') : lastWatchIndex) }}</b>
          </span>
          <span v-else>{{ translate('curve.localHits') }}: <b>{{ localStatusInfo?.hits ?? localHits ?? '-' }}</b></span>
          <span v-if="activePc !== null">PC: <b class="font-mono">0x{{ activePc.toString(16).toUpperCase() }}</b>
            <span v-if="activePcLocation" class="opacity-70">
              {{ typeof activePcLocation.file === 'string' ? activePcLocation.file : (activePcLocation.file as any)?.relativePath ?? (activePcLocation.file as any)?.path ?? '' }}:{{ activePcLocation.line }}
            </span>
            <span v-if="pcLineStatusMessage" class="opacity-60"> · {{ pcLineStatusMessage }}</span>
          </span>
          <span>{{ translate('curve.localSeries') }}: <b>{{ seriesList.length }}/{{ seriesLimit }}</b></span>
          <span v-if="sampling || totalStaleCount > 0" :title="'Per-series valid/stale tick slots from CVF2 per-series mask (firmware task-60). valid=' + totalValidCount + ', stale=' + totalStaleCount + (localSeriesStale === null ? '' : ', firmware series_stale=' + localSeriesStale)">
            valid: <b>{{ totalValidCount }}</b> · stale: <b>{{ totalStaleCount }}</b>
          </span>
          <span v-if="localSampleEveryMax !== null" class="opacity-70" title="Firmware-confirmed maximum per-series sample_every for this session">sample_every max: <b>{{ localSampleEveryMax }}</b></span>
          <span v-if="localStartTransport" class="opacity-70" :title="localStartTransport === 'bin'
            ? 'Request transport: CBS1 streaming binary (POST /api/curve/start-bin, no 16 KiB contiguous heap)'
            : ('Request transport: legacy JSON start (16384 B limit). ' + (localStartFallbackReason || ''))">
            req: <b>{{ localStartTransport }}</b><template v-if="localStartRequestBytes !== null"> · {{ localStartRequestBytes }} B</template>
            <template v-if="localStartTransport === 'json' && localStartFallbackReason"> · fallback</template>
          </span>
          <span class="inline-flex items-center gap-1">{{ translate('curve.localGroups') }}: <b>{{ localActiveGroupCount }}</b>
            
          </span>
          <span>{{ translate('curve.localCurrentGroup') }}: <b>{{ sampling ? ((localCurrentGroup ?? 0) + 1) + '/' + localActiveGroupCount : '-' }}</b></span>
          <span>{{ translate('curve.wsTransport') }}: <b>{{ wsTransport }}</b></span>
        </template>
        <label class="flex items-center gap-2">
          <span class="whitespace-nowrap">{{ translate('curve.format') }}</span>
          <el-select v-model="globalFormat" size="small" style="width: 6rem" @change="applyGlobalFormat">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
        </label>
      </div>
      <div v-if="isLocalSource" class="mt-1 flex flex-wrap gap-x-4 gap-y-1 text-xs opacity-80">
        <span v-if="localTrigger === 'breakpoint'">{{ translate('curve.localSampled') }}: <b>{{ localStatusInfo?.sampled ?? localSampled ?? '-' }}</b></span>
        <span>{{ translate('curve.localHits') }}: <b>{{ localStatusInfo?.hits ?? localHits ?? '-' }}</b></span>
        <span>{{ translate('curve.localFrames') }}: <b>{{ localFramesReceived }}</b></span>
        <span>{{ translate('curve.localInvalid') }}: <b>{{ localInvalidCount }}</b></span>
        <span class="inline-flex items-center gap-1">{{ translate('curve.localOutOfScope') }}: <b>{{ localOutOfScopeCount }}</b>
          
        </span>
        <span>{{ translate('curve.localOverwritten') }}: <b>{{ localOverwrittenCount }}</b></span>
        <span title="Average group rate = measured total frame rate / group count; per-group and per-variable ≈ rates are shown in the Selected rows and group-weight bar.">{{ translate('curve.localGroupRate') }}: <b>≈{{ localGroupRefreshRate.toFixed(1) }} Hz</b></span>
        <span>{{ translate('curve.wsFrames') }}: <b>{{ wsFrames }}</b></span>
        <span>{{ translate('curve.wsDropped') }}: <b>{{ wsDropped }}</b></span>
        <span>{{ translate('curve.wsLatency') }}: <b>{{ wsLatencyMs.toFixed(1) }} ms</b></span>
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
      <!-- task-58：资源不足/固件拒绝时**原样**显示固件 error（含 DWT_CTRL 实际值），不改写 -->
      <div v-if="isLocalSource && localTrigger === 'watchpoint' && watchStartError" class="mt-1 text-xs curve-field-error">
        {{ translate('curve.watchpointFirmwareError') }}: <code class="font-mono">{{ watchStartError }}</code>
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
      <div v-if="isLocalSource && (localActiveGroupCount > 1 || hasAnyRatePlan || hasRateTargets)" class="group-weights mt-1 flex flex-wrap items-center gap-2 text-xs">
        <span>{{ translate('curve.groupWeight') }}</span>
        <label
            v-for="(weight, index) in effectiveGroupWeights.slice(0, Math.min(localActiveGroupCount, CURVE_LOCAL_MAX_GROUPS))"
            :key="index"
            class="flex items-center gap-1"
        >
          <span>G{{ index + 1 }}<template v-if="hasAnyRatePlan"> ÷{{ normalizeRateDivisor(plannedGroups[index]?.divisor ?? 1) }}</template></span>
          <el-input-number
              v-if="!hasAnyRatePlan && !usePreciseSampleEvery"
              :model-value="weight"
              :min="1"
              :max="CURVE_LOCAL_MAX_GROUP_WEIGHT"
              :step="1"
              size="small"
              :controls="false"
              :disabled="sampling"
              style="width: 3.2rem"
              @change="(value: number) => setGroupWeight(index, value)"
          />
          <span v-else class="font-mono">{{ weight }}</span>
        </label>
        <span class="opacity-70">Σ{{ localGroupWeightSum }} (≤64)</span>
        <span
            v-if="localGroupActualRates.length"
            class="font-mono opacity-70"
            title="≈ actual group refresh rate = measured total frame rate × group weight share (firmware samples one group per tick); PC gating/out-of-scope can reduce valid samples."
        >≈ {{ localGroupActualRates.map((rate) => formatRateHz(rate)).join(' / ') }} Hz</span>
        <span v-if="hasAnyRatePlan" class="opacity-70">
          target rates are group-weight shares of the global tick (not exact Hz)
        </span>
        <span v-if="usePreciseSampleEvery" class="opacity-70">
          per-curve target Hz → firmware sample_every (integer division of group tick; actual/valid Hz shown per curve)
        </span>
      </div>
      <div v-if="isLocalSource && ratePlanErrorText" class="mt-1 text-xs" style="color: #dc2626">{{ ratePlanErrorText }}</div>
      <div v-if="isLocalSource && rateMismatchWarning" class="mt-1 text-xs render-warning">{{ rateMismatchWarning }}</div>
      <div v-if="isLocalSource && preciseRateWarning" class="mt-1 text-xs render-warning">{{ preciseRateWarning }}</div>
    </section>

    <!-- ELF / DWARF 变量浏览器 -->
    <section class="curve-card">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="inline-flex items-center gap-1 text-lg font-bold">{{ translate('curve.elfUpload') }}
          
        </h2>
        <el-segmented v-model="elfView" :options="elfViewOptions" size="small" class="elf-view-switch"/>
        <el-button :loading="elfParsing" :disabled="isLocalSource && sampling" @click="openElfDialog">{{ translate('curve.elfUpload') }}</el-button>
        <input ref="elfInputRef" type="file" accept=".elf,.out" class="hidden" @change="onElfSelected"/>
      </div>
      <p v-if="!elfInfo" class="mt-1 text-xs opacity-70">{{ translate('curve.emptyElfState') }}</p>

      <div v-if="elfInfo" class="mt-2">
        <p v-if="elfInfo.source === 'dwarf'" class="inline-flex items-center gap-1 text-sm">
          {{ translate('curve.dwarfParsed', {count: elfInfo.variables.length, fixed: elfInfo.dwarfWithAddress}) }}
          
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
        <span v-if="wideHint" class="ml-1 inline-flex align-middle">
          
        </span>
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
                  onlySampleable, groupBy, variableSearch, samplingSource, row.groupLabel, row.groupCount,
                  focusedVariableKey]"
                :data-key="row.key"
                :class="row.kind === 'group'
                  ? 'var-group-row'
                  : ['var-row', {selected: isRowSelected(row), unsampleable: !row.node.sampleable,
                    selectable: nodeSelectable(row.node), 'pc-stale': activePc !== null && !nodeValidAtPc(row.node),
                    'focus-target': focusedVariableKey === row.key}]"
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
      </div>

      <!-- Objects：Ozone 式对象/字段浏览（ObjectBrowser 独立组件，task-52 API） -->
      <div v-if="objectsViewEverOpened" v-show="elfView === 'objects'" class="object-view">
        <ObjectBrowser
            ref="objectBrowserRef"
            :variables="elfInfo?.variables ?? []"
            :roots="objectRoots"
            :root-key="objectRootKey"
            :selected-keys="objectSelectedKeys"
            :watch-keys="watchKeys"
            :live-values="objectLiveValues"
            :presets="objectPresets"
            @root-change="onObjectRootChange"
            @select-variable="onObjectSelected"
            @add-curves="onObjectAddCurves"
            @watch-nodes="onObjectWatchNodes"
            @toggle-watch="onObjectToggleWatch"
            @jump-source="onObjectJumpSource"
            @save-preset="onObjectSavePreset"
            @load-preset="onObjectLoadPreset"
            @delete-preset="onObjectDeletePreset"
        />
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
            {{ translate('curve.targetDetected') }}:
            <template v-if="detectedTarget.series">{{ detectedTarget.series }} · </template>
            {{ detectedTarget.baseGroup || '-' }}
            · DEV_ID {{ detectedTarget.devId || '-' }}
            · Flash {{ detectedTarget.flashKib || '-' }} KiB
            <template v-if="autoTargetModel">
              · {{ translate('curve.targetAutoModel') }}: {{ autoTargetModel.label }}
            </template>
          </template>
          <template v-else>{{ translate('curve.targetNotDetected') }}</template>
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
        <!-- task-55：批量设置目标刷新率（divisor 分桶 → 固件 group weight） -->
        <template v-if="showRateControls">
          <span class="inline-flex items-center gap-1">
            <span class="text-xs" title="Set the target refresh rate for every selected curve (divisor of the global local tick; legacy firmware fallback)">Rate</span>
            <el-select v-model="batchRateDivisor" size="small" style="width: 6.5rem" :disabled="sampling">
              <el-option v-for="divisor in CURVE_LOCAL_RATE_DIVISORS" :key="divisor" :value="divisor" :label="rateDivisorLabel(divisor)" :title="rateDivisorTitle(divisor)"/>
            </el-select>
            <el-button size="small" :disabled="sampling || seriesList.length === 0" @click="applyBatchRate">{{ translate('common.apply') }}</el-button>
          </span>
          <!-- task-62：按目标 Hz 精确设置（固件 task-60 sample_every；未确认时 fallback） -->
          <span class="inline-flex items-center gap-1" :title="localCapabilities.sampleEvery
            ? 'Target refresh rate per curve in Hz: firmware per-series sample_every (actual = group tick Hz / round(tick/target), shown per curve)'
            : 'Target refresh rate in Hz: firmware sample_every not confirmed yet; falls back to the nearest divisor group/weight mapping'">
            <span class="text-xs">Hz</span>
            <el-input-number v-model="batchTargetHz" size="small" :min="0.01" :max="100000" :step="1" :controls="false" style="width: 4.6rem" :disabled="sampling" placeholder="Hz"/>
            <el-button size="small" :disabled="sampling || seriesList.length === 0" @click="applyBatchTargetHz">{{ translate('common.apply') }}</el-button>
            <span class="text-xs" :class="localCapabilities.sampleEvery ? 'opacity-70' : 'render-warning'">
              {{ localCapabilities.sampleEvery ? 'sample_every OK' : 'fallback' }}
            </span>
          </span>
        </template>
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
            :class="{'with-rate': showRateControls}"
        >
          <el-checkbox :model-value="item.visible" size="small" @change="toggleVisible(item)"/>
          <span class="series-dot" :style="{background: seriesDisplayColor(item.color)}"></span>
          <span class="truncate jump-source" :title="translate('curve.sourceViewSource')" @click.stop="jumpToSource(item.key)">{{ item.name }}</span>
          <span class="truncate text-xs opacity-70" :title="item.typeName">{{ item.typeName || translate('curve.unknownType') }}</span>
          <span class="font-mono text-xs opacity-70" :title="item.location ? addressExprText(item.location) : ''">{{ item.location ? addressExprText(item.location) : formatHex32(item.address) }}</span>
          <!-- task-55/62：每变量目标 Hz / divisor + 计划/实测刷新率（仅设备本地采样） -->
          <span v-if="showRateControls" class="rate-cell">
            <el-input-number
                :model-value="item.rateTargetHz ?? undefined"
                size="small"
                class="rate-hz"
                :controls="false"
                :min="0.01"
                :max="100000"
                :disabled="sampling"
                placeholder="Hz"
                :title="seriesRateTitle(item)"
                @change="(value: number | undefined) => setSeriesTargetHz(item, value)"
            />
            <el-select
                :model-value="normalizeRateDivisor(item.rateDivisor)"
                size="small"
                class="rate-select"
                :disabled="sampling"
                :title="rateDivisorTitle(item.rateDivisor)"
                @change="(value: number) => setSeriesRateDivisor(item, value)"
            >
              <el-option v-for="divisor in CURVE_LOCAL_RATE_DIVISORS" :key="divisor" :value="divisor" :label="rateDivisorLabel(divisor)"/>
            </el-select>
            <span class="rate-actual" :title="seriesRateTitle(item)">{{ seriesRateLabel(item) }}</span>
          </span>
          <el-select v-model="item.format" size="small" style="width: 5.5rem" @change="onSeriesFormatChange(item)">
            <el-option v-for="format in FORMATS" :key="format" :value="format" :label="format"/>
          </el-select>
          <span class="selected-actions">
            <el-button link size="small" :disabled="seriesList[0]?.id === item.id" @click="moveSeries(item, -1)">↑</el-button>
            <el-button link size="small" :disabled="seriesList[seriesList.length - 1]?.id === item.id" @click="moveSeries(item, 1)">↓</el-button>
            <el-button type="danger" link size="small" @click="removeSeries(item.id)">×</el-button>
          </span>
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
import {useRoute} from 'vue-router'
import CurveFramesWorker from '../utils/curveFramesWorker?worker&inline'
import CurveChartWorker from '../utils/curveChartWorker?worker&inline'
import {ElMessageBox} from 'element-plus'
import {translate, translateRaw, type TranslationKeys} from '@/locales'
import {globalNotify} from '@/composables/notification'
import SourceBrowser from '@/components/source/SourceBrowser.vue'
import ObjectBrowser from '@/components/object/ObjectBrowser.vue'
import {
  DEFAULT_OBJECT_ROOT_NAMES,
  defaultObjectRoot,
  discoverObjectRoots,
  findObjectNodes,
  findObjectRoot,
  objectNodeRef,
  type ObjectBatchPayload,
  type ObjectNodeRef,
  type ObjectPreset,
  type ObjectPresetSavePayload,
  type ObjectRoot,
  type ObjectSourceJumpPayload,
  type ObjectToggleWatchPayload,
} from '@/components/object'
import type {SourceBrowserLabels} from '@/components/source/labels'
import type {SourceLineAddressMap, SourceLineClickPayload, SourceLiveValueMap} from '@/utils/sourceProject'
import type {SourceActiveFileLine} from '@/components/source/lineTarget'
import {
  deleteObjectWatchPreset,
  listObjectWatchPresets,
  saveObjectWatchPreset,
} from '@/utils/objectWatchPresets'
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
  parseTargetDetection,
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
  resolveChartWebglMode,
  type ChartDrawMessage,
  type ChartGeometry,
  type ChartWebglMode,
} from '@/utils/curveChartCore'
import {
  DAP_COMMAND_INDEX_EVENT,
  DAP_COMMAND_INDEX_VERSION_KEY,
  DAP_COMMAND_INDEX_WINDOW_KEY,
  DAP_FOCUS_VARIABLE_EVENT,
  buildDapCommandIndex,
  emptyDapCommandIndex,
  findCommandIndexVariable,
  type DapCommandIndex,
  type DapCommandIndexVariable,
} from '@/utils/elfCommandIndex'
import {
  CURVE_LOCAL_FLASH_END,
  CURVE_LOCAL_FLASH_START,
  CURVE_LOCAL_FRAMES_DEFAULT,
  CURVE_LOCAL_FRAMES_PATH,
  CURVE_LOCAL_MAX_GROUP_WEIGHT,
  CURVE_LOCAL_MAX_SNAPSHOT_AFTER,
  CURVE_LOCAL_RATE_DIVISORS,
  CURVE_LOCAL_MAX_SAMPLE_EVERY,
  CURVE_LOCAL_MAX_WATCHPOINTS,
  deviceApiUrl,
  detectLocalCurveCapabilities,
  isFrameSeriesValid,
  planSampleEvery,
  decodeCurveFrames,
  CURVE_LOCAL_MAX_GROUPS,
  CURVE_LOCAL_MAX_SERIES_PER_GROUP,
  CURVE_LOCAL_MAX_SAMPLE_BYTES,
  CURVE_LOCAL_MAX_TOTAL_SERIES,
  CurveLocalError,
  isDuplicateWatchpoint,
  normalizeRateDivisor,
  planLocalRateGroups,
  splitSeriesIntoGroups,
  type CurveLocalLocationVariant,
  type CurveLocalRateDivisor,
  type LocalCurveCapabilities,
  type CurveLocalWatchAccess,
  type LocalRateGroupPlan,
  getLocalCurveFrames,
  getLocalCurveStatus,
  parseBreakpointAddressInput,
  startLocalCurve,
  startLocalCurveAuto,
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
type ElfView = 'variables' | 'source' | 'objects'
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
/** 已选面板默认只渲染前 64 行（批量选择接近 64 上限时避免一次挂载过多 Element 组件），可 Show all。 */
const SELECTED_RENDER_LIMIT = 64
/** 采样统计 UI 刷新间隔（数据仍按采样率入 ring）；降低 Vue 全页 rerender 频率。 */
const STATS_FLUSH_MS = 1000
/** 本地采样计数器（frames/current group 等）UI 刷新间隔。 */
const LOCAL_UI_FLUSH_MS = 1000
/** 本地帧轮询周期（契约要求 20–50 ms）。 */
const LOCAL_POLL_MS = 20
const LOCAL_POLL_MAX_FRAMES = 64
/** task-58：目标 UART trace 轮询间隔（ms；固件只保存 2 KiB ring，属于轮询快照语义）。 */
const TRACE_POLL_OPTIONS = [100, 250, 500, 1000]
/** task-58：trace 显示窗口上限（只影响 UI 文本，不改变固件 ring / 字节序号）。 */
const TRACE_DISPLAY_MAX_BYTES = 4096
/** 采样管线深度：允许同时在途的 WS-DAP 采样请求数（固件按序回包）。 */
const PIPELINE_DEPTH = 8
const AUTO_MIN_RATE = 10
const AUTO_MAX_RATE = 100
/** Auto 自适应窗口（毫秒）。 */
const AUTO_RATE_WINDOW_MS = 2500
/**
 * 环形缓冲总点数：曲线窗口仅 20s；多路本地 v2（精简版 ≤64 系列）下每系列
 * lo/hi/values 三个数组的内存 = points × 16B。6000 点 @100Hz ≈ 60s（本地 50Hz
 * ≈ 120s），既覆盖图表窗口/CSV 导出，也把 64 路内存从 ~61MB 降到 ~6MB，减少
 * GC 卡顿。
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
  /**
   * 每变量目标刷新率分频（task-55）：缺省/1 = 跟随全局 interval（默认 20ms/50Hz）；
   * 2/4/8 = 通过 groups+weights 相对降速。仅设备本地采样使用。
   */
  rateDivisor?: CurveLocalRateDivisor
  /**
   * 每变量目标刷新率（Hz，task-62）：设置后新固件（confirmed sample_every）按
   * `sample_every = round(group tick Hz / target Hz)` 精确下发；旧固件 fallback 到
   * 最近的 divisor 分桶 + 组权重。与 rateDivisor 互斥（设置一个会清另一个）。
   */
  rateTargetHz?: number
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
/** 采样模式：实时（固定地址，高采样率）/ 调试（暂停读取帧/寄存器/指针，低采样率）。 */
/** 采样来源：web=现有 WebSocket-DAP 路径；local=ESP32 本地 halt→读→resume 闭环。 */
const samplingSource = ref<SamplingSource>('local')
const isLocalSource = computed(() => samplingSource.value === 'local')
const localIntervalMs = ref(DEFAULT_LOCAL_INTERVAL_MS)
/** 当前来源允许的最大曲线数：设备本地为精简版 64，Web-DAP 仍为 24。 */
const seriesLimit = computed(() => (isLocalSource.value ? CURVE_LOCAL_MAX_TOTAL_SERIES : CURVE_LOCAL_MAX_TOTAL_SERIES))
/** 本地可采集系列（派生曲线不参与采集）。 */
const localRateSeries = computed(() => seriesList.value.filter((item) => !item.derived))
/** 有任意曲线设置了非全速目标（divisor≠1）→ 启用“目标率分桶 + 组权重”路径。 */
const hasRateOverrides = computed(() => localRateSeries.value.some((item) => normalizeRateDivisor(item.rateDivisor) !== 1))
/**
 * 固件能力（task-60）：从 status 回读探测。`sampleEvery` = 固件支持 series.sample_every
 * （精确每变量速率）；未知/旧固件 → 继续用 groups+weight divisor 分桶 fallback。
 */
const localCapabilities = ref<LocalCurveCapabilities>({sampleEvery: false, watchpoints4: false})
const localCapabilitiesKnown = ref(false)
async function refreshLocalCapabilities(): Promise<void> {
  try {
    const status = await getLocalCurveStatus()
    localCapabilities.value = detectLocalCurveCapabilities(status)
    localCapabilitiesKnown.value = true
  } catch {
    // 设备离线/旧固件接口异常：保持保守 fallback（不假设新字段被接受）。
  }
}
/** 用户设置了目标 Hz（>0）的曲线。 */
const hasRateTargets = computed(() => localRateSeries.value.some((item) => (item.rateTargetHz ?? 0) > 0))
/** 精确 sample_every 模式：固件确认支持 + 至少一条曲线设了目标 Hz。 */
const usePreciseSampleEvery = computed(() => localCapabilities.value.sampleEvery && hasRateTargets.value)
/** 名义 tick 率（1000/interval）与计划组数：sample_every 是相对所属 group tick 的抽取。 */
const nominalTickHz = computed(() => (localIntervalMs.value > 0 ? 1000 / localIntervalMs.value : 0))
/** 兼容 fallback：目标 Hz → 最近的 divisor（1/2/4/8），用于旧固件 group weight 路径。 */
function legacyDivisorForTarget(targetHz: number): CurveLocalRateDivisor {
  const tick = nominalTickHz.value
  if (!(targetHz > 0) || !(tick > 0)) return 1
  let best: CurveLocalRateDivisor = 1
  let bestError = Number.POSITIVE_INFINITY
  for (const divisor of CURVE_LOCAL_RATE_DIVISORS) {
    const actual = tick / divisor
    const error = Math.abs(actual - targetHz)
    if (error < bestError) {
      bestError = error
      best = divisor
    }
  }
  return best
}
/** 实际用于分组/下发 divisor 的取值（优先目标 Hz 的 fallback，其次手选 divisor）。 */
function effectiveRateDivisor(item: CurveSeries): CurveLocalRateDivisor {
  const target = item.rateTargetHz ?? 0
  if (target > 0 && !usePreciseSampleEvery.value) return legacyDivisorForTarget(target)
  return normalizeRateDivisor(item.rateDivisor)
}
/** 目标 Hz → sample_every 计划（仅在精确模式有意义；旧固件 fallback 见 effectiveRateDivisor）。 */
interface SeriesSampleEveryPlanEntry { sampleEvery: number; actualHz: number; targetHz: number; capped: boolean }
const sampleEveryPlanBySeriesId = computed<Map<number, SeriesSampleEveryPlanEntry>>(() => {
  const map = new Map<number, SeriesSampleEveryPlanEntry>()
  if (!usePreciseSampleEvery.value) return map
  const groups = plannedPreciseGroups.value
  const tickPerGroup = groups.length > 0 ? nominalTickHz.value / groups.length : nominalTickHz.value
  for (const group of groups) {
    for (const item of group.items) {
      const target = item.rateTargetHz ?? 0
      if (!(target > 0)) continue
      const plan = planSampleEvery(target, tickPerGroup, CURVE_LOCAL_MAX_SAMPLE_EVERY)
      map.set(item.id, {sampleEvery: plan.sampleEvery, actualHz: plan.actualHz, targetHz: target, capped: plan.capped})
    }
  }
  return map
})
/** 精确模式下的分组：按容量顺序切分（64 路/组/256B/8 组），权重恒 1。 */
const plannedPreciseGroups = computed<Array<{items: CurveSeries[]}>>(() => {
  const split = splitSeriesIntoGroups(localRateSeries.value, CURVE_LOCAL_MAX_SERIES_PER_GROUP, CURVE_LOCAL_MAX_GROUPS, {
    sizeOf: (item) => Math.max(1, Math.min(8, item.sample.size)),
    maxBytes: CURVE_LOCAL_MAX_SAMPLE_BYTES,
  })
  return split.groups.map((items) => ({items}))
})
/** 精确模式容量错误（>8 组 / sum(size) 超限）：启动前明确拒绝，不静默丢系列。 */
const preciseGroupsError = computed(() => {
  if (!usePreciseSampleEvery.value) return ''
  const split = splitSeriesIntoGroups(localRateSeries.value, CURVE_LOCAL_MAX_SERIES_PER_GROUP, CURVE_LOCAL_MAX_GROUPS, {
    sizeOf: (item) => Math.max(1, Math.min(8, item.sample.size)),
    maxBytes: CURVE_LOCAL_MAX_SAMPLE_BYTES,
  })
  if (split.error === 'too_many_series') {
    return `Precise rate plan needs ${split.groupCount} groups (> firmware max ${CURVE_LOCAL_MAX_GROUPS}); reduce variables.`
  }
  if (split.error === 'too_many_bytes') {
    return `A single series payload exceeds ${CURVE_LOCAL_MAX_SAMPLE_BYTES} bytes (firmware group limit); cannot send.`
  }
  return ''
})
/**
 * 每变量目标刷新率分桶计划（task-55，纯函数）：按 divisor 分桶、桶内按 64 路/256B
 * 再切分、权重 8/4/2/1（∝1/divisor）。容量不足时 error 明确拒绝（不丢系列/不伪造）。
 */
const ratePlan = computed<LocalRateGroupPlan<CurveSeries>>(() => planLocalRateGroups(localRateSeries.value, {
  sizeOf: (item) => Math.max(1, Math.min(8, item.sample.size)),
  divisorOf: (item) => effectiveRateDivisor(item),
  perGroup: CURVE_LOCAL_MAX_SERIES_PER_GROUP,
  maxGroups: CURVE_LOCAL_MAX_GROUPS,
  maxBytes: CURVE_LOCAL_MAX_SAMPLE_BYTES,
  maxTotal: CURVE_LOCAL_MAX_TOTAL_SERIES,
}))
/** 是否需要速率计划（手选 divisor 或目标 Hz）；精确 sample_every 模式不走分桶。 */
const hasAnyRatePlan = computed(() => (hasRateOverrides.value || hasRateTargets.value) && !usePreciseSampleEvery.value)
/**
 * 实际下发的计划分组：
 *  - 精确模式（固件支持 sample_every 且设置了目标 Hz）：按容量顺序等权分组，
 *    每 series 单独下发 sample_every（组 tick 率 / sample_every = 目标档位）；
 *  - 旧固件/手选 divisor：沿用 task-55 的 divisor 分桶 + 组 weight（fallback）；
 *  - 未设置目标：旧的等权 64 路自动分组（默认行为/负载不变）。
 */
const plannedGroups = computed<Array<{divisor: CurveLocalRateDivisor; weight: number; items: CurveSeries[]}>>(() => {
  if (usePreciseSampleEvery.value) {
    return plannedPreciseGroups.value.map((group) => ({divisor: 1 as CurveLocalRateDivisor, weight: 1, items: group.items}))
  }
  if (hasAnyRatePlan.value) {
    return ratePlan.value.groups.map((group) => ({divisor: group.divisor, weight: group.weight, items: group.items}))
  }
  const split = splitSeriesIntoGroups(localRateSeries.value, CURVE_LOCAL_MAX_SERIES_PER_GROUP, CURVE_LOCAL_MAX_GROUPS, {
    sizeOf: (item) => Math.max(1, Math.min(8, item.sample.size)),
    maxBytes: CURVE_LOCAL_MAX_SAMPLE_BYTES,
  })
  return split.groups.map((items, index) => ({divisor: 1 as CurveLocalRateDivisor, weight: groupWeights.value[index] ?? 1, items}))
})
/** 本地计划组数（最多 8 组）；采样后以固件回读 group_count 为准。 */
const localPlannedGroups = computed(() => Math.max(1, plannedGroups.value.length))
const localActiveGroupCount = computed(() => (sampling.value && localGroupCount.value > 0 ? localGroupCount.value : localPlannedGroups.value))
/** 组平均刷新率 = 总帧率/组数（总帧率优先用实测帧率）；逐组速率见 localGroupActualRates。 */
const localGroupRefreshRate = computed(() => {
  const groups = localActiveGroupCount.value
  if (groups <= 0) return 0
  return localTotalFrameRateHz.value / groups
})

const localModeHintText = computed(() => (localTrigger.value === 'breakpoint'
  ? translate('curve.localBreakpointHint')
  : translate('curve.localModeHint')))
const dwarfDetailsText = computed(() => {
  if (!elfInfo.value) return ''
  const lines: string[] = []
  if (unsampleableCount.value > 0) lines.push(translate('curve.unsampleableHint', {count: unsampleableCount.value}))
  if (elfInfo.value.dwarfHaltSamplable > 0) lines.push(translate('curve.haltSamplableHint', {count: elfInfo.value.dwarfHaltSamplable}))
  if (elfInfo.value.dwarfPointerExpanded > 0) lines.push(translate('curve.pointerExpandedHint', {count: elfInfo.value.dwarfPointerExpanded}))
  lines.push(translate('curve.debugInfoHint'))
  return lines.join('\n')
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
// ---- task-55：每变量目标刷新率 / 函数快照 ---------------------------------
/** 批量设置目标刷新率时使用的 divisor（1=全速/跟随全局）。 */
const batchRateDivisor = ref<CurveLocalRateDivisor>(1)
/**
 * 函数快照状态（task-55）：arm → 断点采样 → 首个命中帧/固件 snapshot 完成后冻结显示
 * 并在图表上标记。只使用现有 breakpoint 采样 + （新固件的可选 snapshot_after），
 * 不伪造 SWO/backtrace/RTOS 数据。
 */
const snapshotArmed = ref(false)
const snapshotHit = ref(false)
const snapshotHitTime = ref<number | null>(null)
const snapshotHitPc = ref<number | null>(null)
const snapshotSeq = ref<number | null>(null)
const snapshotFunctionName = ref('')
const snapshotBoundaryAddr = ref(0)
const snapshotFreezeOnHit = ref(true)
/** 固件一次性快照：首命中后采集 N 帧并自动停止（缺省关闭以兼容旧固件）。 */
const snapshotFirmwareOneShot = ref(false)
const snapshotAfterFrames = ref(8)
const snapshotFirmwareState = ref<LocalCurveStatus['snapshotState']>(undefined)
const snapshotFirmwarePreFrames = ref<number | null>(null)
const snapshotFirmwarePostFrames = ref<number | null>(null)
let snapshotFinalizeTimer = 0
/** 本次会话是否已向固件下发 snapshot_after（用于识别 firmware 自动停止，避免无限轮询）。 */
let snapshotFirmwareRun = false
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
  // task-55：目标刷新率分桶超出固件容量（>8 组 / >64 路）时禁止 Start（面板有明确原因）。
  if (hasRateOverrides.value && ratePlan.value.error) return true
  if (localTrigger.value === 'breakpoint') return breakpointResolution.value?.ok !== true
  if (localTrigger.value === 'watchpoint') {
    if (watchpointSpec.value === null) return true
    if (conditionEnabled.value && conditionSpec.value === null) return true
    if (!watchSecondEnabled.value) return false
    if (watchpoint2Spec.value === null) return true
    return isDuplicateWatchpoint(watchpointSpec.value, watchpoint2Spec.value)
  }
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
/** 本地选中的系列上限（精简版 v2 总 64；每组 ≤64，自动切分）。 */
const localSeriesLimit = CURVE_LOCAL_MAX_TOTAL_SERIES
/** Auto 模式下当前实际使用的目标采样率（自动调整，UI 显示）。 */
const globalFormat = ref<CurveFormat>('auto')
/** B/A 渲染模式：B（默认）只绘制前 drawCap 条可见曲线；A 绘制全部可见（降采样）。 */
const renderAll = ref(false)
const drawCap = ref(DEFAULT_DRAW_CAP)
/** task-44：滑动平均窗口（1=off，3/5/9）与 FFT 面板。 */
const smoothingWindow = ref(1)
const SMOOTHING_WINDOWS = [1, 3, 5, 9]
const fftOpen = ref(false)
const fftData = ref<{name: string; sampleRate: number; peaks: Array<{hz: number; magnitude: number}>} | null>(null)

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
const elfFlashRange = shallowRef<[number, number] | null>(null)
const watchAddressInput = ref('')
const watchSize = ref<1 | 2 | 4>(4)
const watchAccess = ref<CurveLocalWatchAccess>('write')
/** watchpoint 触发状态（task-38）。 */
const watchTriggerSeq = ref<number | null>(null)
const watchpointArmed = ref<boolean | null>(null)
const watchpointHits = ref<number | null>(null)
const watchFramesTriggered = ref(false)
const jumpFrames = ref(10)
/** task-58：第二路 DWT comparator（固件 task-57；目标需 ≥2 个空闲 comparator）。 */
const watchSecondEnabled = ref(false)
const watch2AddressInput = ref('')
const watch2Size = ref<1 | 2 | 4>(4)
const watch2Access = ref<CurveLocalWatchAccess>('write')
/** 多 comparator / watchpoint2_* 状态（status 回读）。 */
const watchCount = ref<number | null>(null)
const watchpoint2Armed = ref<boolean | null>(null)
const watchpoint2Address = ref<number | null>(null)
const watchpoint2Hits = ref<number | null>(null)
const lastWatchIndex = ref<number | null>(null)
/**
 * task-62 固件 task-60：第 3/4 路 DWT comparator（通用槽位；最多 4 路）。
 * 只保存配置，实际资源是否足够由固件回读/错误原文决定（不猜测）。
 */
interface ExtraWatchSlot {
  enabled: boolean
  addressInput: string
  size: 1 | 2 | 4
  access: CurveLocalWatchAccess
}
function newExtraWatchSlot(): ExtraWatchSlot {
  return {enabled: false, addressInput: '', size: 4, access: 'readwrite'}
}
const watchThird = ref<ExtraWatchSlot>(newExtraWatchSlot())
const watchFourth = ref<ExtraWatchSlot>(newExtraWatchSlot())
/** status 回读的 canonical watchpoints 数组（若有）；否则用 legacy watchpointN_* 字段。 */
const watchpoint3Armed = ref<boolean | null>(null)
const watchpoint3Address = ref<number | null>(null)
const watchpoint3Hits = ref<number | null>(null)
const watchpoint4Armed = ref<boolean | null>(null)
const watchpoint4Address = ref<number | null>(null)
const watchpoint4Hits = ref<number | null>(null)
/** 最近一次 watchpoint start 失败时固件返回的原文（资源不足，不改写/不复述为猜测）。 */
const watchStartError = ref('')
/** task-58：帧指针 backtrace（固件 task-57；仅 breakpoint/watchpoint 命中后可用）。 */
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
// ---- task-62 P1-17：命令面板数据源（ELF → window.__dapCommandIndex + 事件） ----
/** 当前 ELF 的命令索引（空索引也发布，命令面板据此隐藏变量/函数段）。 */
const commandIndex = shallowRef<DapCommandIndex>(emptyDapCommandIndex())
let commandIndexVersion = 0
/** 由 ?focusVar= 或 dap:focus-variable 事件请求聚焦的变量 path（ELF 未解析时暂存）。 */
let pendingFocusVar = ''
const focusedVariableKey = ref('')
let focusScrollTimer = 0
const route = useRoute()
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
  {label: translate('curve.sourceViewObjects'), value: 'objects'},
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
/** task-53：Objects/Watch 视图（懒挂载，切 tab 保持状态）。 */
const objectsViewEverOpened = ref(false)
const objectRoots = computed<ObjectRoot[]>(() => (elfInfo.value
  ? discoverObjectRoots(elfInfo.value.variables, {preferredNames: DEFAULT_OBJECT_ROOT_NAMES})
  : []))
const objectRootKey = ref<string | null>(null)
const activeObjectRoot = computed(() => findObjectRoot(objectRoots.value, objectRootKey.value)
  ?? defaultObjectRoot(objectRoots.value))
const objectBrowserRef = ref<{ selectRootByKey?(key: string): void } | null>(null)
const objectPresetVersion = ref(0)
const objectSelectedKeys = computed<string[]>(() => seriesList.value.map((item) => item.key))
const objectPresets = computed(() => {
  void objectPresetVersion.value
  const root = activeObjectRoot.value
  if (!root) return []
  return listObjectWatchPresets(root.qualifiedName).map((preset) => ({
    id: preset.id,
    name: preset.name,
    rootPath: preset.rootPath,
    fields: preset.fields,
  }))
})
const objectLiveValues = computed(() => sourceLiveValues.value)
const sourceBrowserRef = ref<{
  scrollToVariableByKey(key: string): Promise<boolean>
  openFile?(file: string): Promise<void>
  scrollToLine?(line: number, highlight?: boolean): void
} | null>(null)
/** task-44：PC 当前行自动联动 Source（可开关，避免抢用户滚动）。 */
const autoPcSource = ref(true)
watch(elfView, (view) => {
  if (view === 'source') sourceViewEverOpened.value = true
  if (view === 'objects') objectsViewEverOpened.value = true
}, {immediate: true})
const sampling = ref(false)
const everSampled = ref(false)
const statusError = ref(false)
const notice = ref<CurveNotice | null>(null)
const sampleCount = ref(0)
const failedCount = ref(0)
const droppedCount = ref(0)

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

let nextSeriesId = 1
let sampleTimer = 0
// 设备本地采样状态
let localRunId = 0
/** 固定节拍轮询定时器 + 独立状态刷新/绘制定时器（task-33）。 */
let localPollTimer = 0
let localStatusTimer = 0
let localDrawLoopTimer = 0
/** task-58：UART trace 轮询/backtrace 自动抓取定时器（事件驱动 + 去抖）。 */
let traceTimer = 0
let tracePollInFlight = false
/** 连续 trace 轮询失败次数（3 次后停止轮询，避免错误刷屏）。 */
let traceErrorStreak = 0
/** 已处理的 backtrace status 指纹（valid:count），避免 1 Hz status 轮询重复抓取。 */
let lastBacktraceStatusKey = ''
let backtraceTimer = 0
let lastBacktraceFetchAt = 0
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
/** task-62：per-series 有效/stale 采样计数（非响应式；1 Hz 用 validCountVersion 触发 UI 刷新）。 */
interface SeriesValidCounter {valid: number; stale: number}
const seriesValidCounters = new Map<number, SeriesValidCounter>()
const validCountVersion = ref(0)
/** 本次会话已应用的最新帧时间（秒，自会话起点）；有效刷新率窗口。 */
const validCountWindowSeconds = ref(0)
/** 固件 status.series_stale（本会话累计 stale 槽数；旧固件缺省）。 */
const localSeriesStale = ref<number | null>(null)
/** 本会话是否真正按 sample_every 精确采样（start 响应 sample_every_max 确认）。 */
const localPreciseActive = ref(false)
/** 本会话固件回读的 sample_every_max（undefined=旧固件未确认）。 */
const localSampleEveryMax = ref<number | null>(null)
/** P0-1：本次启动实际使用的请求编码（bin=CBS1，json=旧固件回退）与原因/字节数。 */
const localStartTransport = ref<'bin' | 'json' | null>(null)
const localStartRequestBytes = ref<number | null>(null)
const localStartFallbackReason = ref('')
/** 目标 Hz 的批量输入框值。 */
const batchTargetHz = ref<number | null>(null)
/** 本地运行时的系列顺序（carry values 对齐图表 series）。 */
let localSeriesOrder: CurveSeries[] = []
/** 协议 v2 group 映射：group id -> 该组内 series 在 localSeriesOrder 中的下标与字节数。 */
let localGroupEntries: Array<Array<{index: number; item: CurveSeries; size: number}>> = []
/** 最近一次各 series 的原始字节（多组轮转时未采样组沿用上一次值，避免假 0）。 */
let localLatestValues = new Map<number, Uint8Array>()
let sampleSeq = 0
let sampleEpochMs = 0
let consecutiveFailures = 0
let recentCompletions: number[] = []
/** 采样运行代次：停止/重启后忽略迟到的完成回调。 */
/** 已发出、尚未完成的采样请求数。 */
let inflightSamples = 0
// Auto 速率控制窗口统计
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

// ---- 采样状态 -------------------------------------------------------------
const statusTagType = computed<'primary' | 'success' | 'info' | 'warning' | 'danger'>(() => {
  if (sampling.value) return 'success'
  if (statusError.value) return 'danger'
  if (localActive.value) return 'warning'
  return 'info'
})
const statusText = computed(() => {
  if (sampling.value) return translate('curve.statusSampling')
  if (statusError.value) return translate('curve.statusError')
  return translate('curve.localStopped')
})

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
    validCountVersion.value += 1
    return
  }
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
  // task-58：多 DWT comparator 状态 + UART trace 摘要 + backtrace 摘要（旧固件字段缺省）。
  watchCount.value = status.watchCount ?? null
  watchpoint2Armed.value = status.watchpoint2Armed ?? null
  watchpoint2Address.value = status.watchpoint2Address ?? null
  watchpoint2Hits.value = status.watchpoint2Hits ?? null
  // task-62：固件 task-60 的 3/4 路 comparator 状态（旧固件缺省 → null，不伪报）。
  watchpoint3Armed.value = status.watchpoint3Armed ?? null
  watchpoint3Address.value = status.watchpoint3Address ?? null
  watchpoint3Hits.value = status.watchpoint3Hits ?? null
  watchpoint4Armed.value = status.watchpoint4Armed ?? null
  watchpoint4Address.value = status.watchpoint4Address ?? null
  watchpoint4Hits.value = status.watchpoint4Hits ?? null
  lastWatchIndex.value = status.lastWatchIndex ?? null
  conditionArmed.value = status.conditionArmed ?? null
  conditionMet.value = status.conditionMet ?? null
  conditionHits.value = status.conditionHits ?? null
  if (status.conditionTriggerSeq !== undefined) conditionTriggerSeq.value = status.conditionTriggerSeq
  // task-55：固件一次性函数快照（task-57 固件；旧固件无这些字段 → undefined，
  // 仍走“首个命中帧冻结”的降级路径，界面明确标注固件是否支持）。
  if (status.snapshotState !== undefined) snapshotFirmwareState.value = status.snapshotState
  if (status.snapshotAfter !== undefined) snapshotAfterFrames.value = status.snapshotAfter
  if (status.snapshotPreFrames !== undefined) snapshotFirmwarePreFrames.value = status.snapshotPreFrames
  if (status.snapshotPostFrames !== undefined) snapshotFirmwarePostFrames.value = status.snapshotPostFrames
  if (snapshotFirmwareRun && (status.snapshotState === 'complete' || status.snapshotState === 'aborted')) {
    // 固件已命中并自动停止（或中止）：用固件 trigger seq 定位标记帧；若帧尚未取回，
    // 用当前最新数据时间标记并保留“≈”。随后等待在途帧落地，再仅关闭前端轮询。
    snapshotFirmwareRun = false
    if (!snapshotHit.value) {
      const order = ringOrderForDeviceSeq(status.snapshotTriggerSeq ?? null)
      const time = order >= 0 ? timestamps[ringSlot(order)] : lastDataTimestamp()
      markSnapshotHit(time, latestFramePc, status.snapshotTriggerSeq ?? undefined)
    }
    snapshotArmed.value = false
    if (status.snapshotState === 'complete') {
      notice.value = {type: 'success', title: snapshotFirmwareCompleteText()}
    } else {
      notice.value = {type: 'warning', title: 'Function snapshot aborted by firmware (session stopped).'}
    }
    if (!snapshotFinalizeTimer) {
      const finalizeRunId = localRunId
      snapshotFinalizeTimer = window.setTimeout(() => {
        snapshotFinalizeTimer = 0
        // 只在同一采样会话内收尾，避免迟到的定时器影响后续重新 Start 的会话。
        if (sampling.value && localRunId === finalizeRunId) stopLocalSampling(false)
      }, 500)
    }
  }
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

/** 停止设备本地采样：本地先置 stopped，再 best-effort POST /api/curve/stop（postStop=false 用于固件已自动停止的快照）。 */
function stopLocalSampling(postStop = true): void {
  const wasActive = localActive.value
  sampling.value = false
  localRunId += 1
  localActive.value = false
  latestFramePc = undefined
  clearLocalPollTimers()
  stopCurveWs()
  localPollsInFlightCount = 0
  localFrameBuffer.clear()
  if (snapshotFinalizeTimer) { window.clearTimeout(snapshotFinalizeTimer); snapshotFinalizeTimer = 0 }
  snapshotFirmwareRun = false
  // 未命中的 armed 状态在停止后取消；命中的标记保留供查看/下次 Start 时重置。
  if (snapshotArmed.value && !snapshotHit.value) snapshotArmed.value = false
  if (wasActive && postStop) void postLocalStop()
  flushStats()
  scheduleDraw()
}

// ---- task-58/62：目标 UART trace（旧 raw 字节流 + 固件 task-60 lines 接口） ------
/**
 * 调度下一次 trace 轮询（只在 active 时自循环）。
 * 轮询快照语义；raw 模式只有字节序号，lines 模式的行/时间戳全部来自固件字段。
 */
async function startLocalSampling(): Promise<void> {
  if (sampling.value || seriesList.value.length === 0) return
  const snapshot = seriesList.value.filter((item) => !item.derived)
  if (snapshot.length === 0) {
    notice.value = {type: 'error', title: translate('curve.derivedNeedTwo')}
    return
  }
  // task-62：启动前确认固件能力（sample_every / watchpoints4）。未知时保持保守 fallback，
  // 不用“假设固件接受新字段”的方式伪造精确速率。
  if (!localCapabilitiesKnown.value) await refreshLocalCapabilities()
  // task-55：设置了每变量目标刷新率时走“divisor 分桶 + 组权重”；task-62 新固件
  // 支持 sample_every 时按目标 Hz 精确下发。三条路径共用 64/组、256B/组、8 组、64 路限制。
  if (usePreciseSampleEvery.value && preciseGroupsError.value) {
    notice.value = {type: 'error', title: preciseGroupsError.value}
    return
  }
  if (hasAnyRatePlan.value && ratePlan.value.error) {
    notice.value = {type: 'error', title: ratePlanErrorText.value || 'Rate plan exceeds firmware group capacity'}
    return
  }
  const groupPlan = plannedGroups.value
  if (groupPlan.length === 0) return
  if (hasAnyRatePlan.value) {
    // 权重由 divisor 决定；写回 groupWeights 让下方 weight payload 行/旧 UI 保持一致。
    groupWeights.value = groupPlan.map((group) => group.weight).slice(0, CURVE_LOCAL_MAX_GROUPS)
  }
  ensureGroupWeights(groupPlan.length)
  const groupSpecs: Array<{id: number; weight: number; series: CurveLocalSeriesSpec[]}> = []
  const groupEntries: Array<Array<{index: number; item: CurveSeries; size: number}>> = []
  const snapshotIndexById = new Map(snapshot.map((item, index) => [item.id, index]))
  let needsHalt = false
  let hasVariants = false
  for (const group of groupPlan) {
    const series: CurveLocalSeriesSpec[] = []
    const entries: Array<{index: number; item: CurveSeries; size: number}> = []
    for (const item of group.items) {
      const built = buildLocalSeriesSpec(item, series.length, snapshotIndexById.get(item.id) ?? 0)
      if (!built) {
        notice.value = {type: 'error', title: translate('curve.localRegisterUnsupported')}
        return
      }
      // task-62：精确模式按目标 Hz 下发 per-series sample_every（= group tick Hz / target）。
      if (usePreciseSampleEvery.value) {
        const ratePlanEntry = sampleEveryPlanBySeriesId.value.get(item.id)
        if (ratePlanEntry && ratePlanEntry.sampleEvery > 1) built.spec.sampleEvery = ratePlanEntry.sampleEvery
      }
      series.push(built.spec)
      entries.push({index: built.entry.index, item: built.entry.item, size: built.entry.size})
      if (built.entry.needsHalt) needsHalt = true
      if ((built.spec.variants?.length ?? 0) > 0) hasVariants = true
    }
    groupSpecs.push({id: groupSpecs.length, weight: groupWeights.value[groupSpecs.length] ?? 1, series})
    groupEntries.push(entries)
  }
  statusError.value = false
  notice.value = null
  const trigger = localTrigger.value
  let breakpointAddress: number | undefined
  let watchpoint: CurveLocalWatchpointSpec | undefined
  let watch2: CurveLocalWatchpointSpec | undefined
  let watch3: CurveLocalWatchpointSpec | undefined
  let watch4: CurveLocalWatchpointSpec | undefined
  let watchpointsArray: CurveLocalWatchpointSpec[] | undefined
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
    // task-58/62：第 2/3/4 路 comparator 可选；启用但配置非法时本地拒绝（不静默少发）。
    if (watchSecondEnabled.value) {
      if (!watchpoint2Spec.value) {
        notice.value = {type: 'error', title: translate('curve.watchpointInvalid')}
        return
      }
      if (isDuplicateWatchpoint(watchpoint, watchpoint2Spec.value)) {
        notice.value = {type: 'error', title: translate('curve.watchpointDuplicatesFirst')}
        return
      }
      watch2 = watchpoint2Spec.value
    }
    if (watchThird.value.enabled) {
      if (!watchpoint3Spec.value) {
        notice.value = {type: 'error', title: translate('curve.watchpointInvalid')}
        return
      }
      watch3 = watchpoint3Spec.value
    }
    if (watchFourth.value.enabled) {
      if (!watchpoint4Spec.value) {
        notice.value = {type: 'error', title: translate('curve.watchpointInvalid')}
        return
      }
      watch4 = watchpoint4Spec.value
    }
    if (watchpointError.value) {
      notice.value = {type: 'error', title: watchpointError.value}
      return
    }
    // 固件 task-60 两种编码互斥：确认新固件（4 路能力）时用 canonical watchpoints
    // 数组；未知/旧固件用 legacy root+watch2/watch3/watch4（旧固件忽略 W3/W4，优雅
    // 降级为 1-2 路，start 响应 watch_count 会如实反映）。
    const requested = requestedWatchpoints.value
    if (localCapabilities.value.watchpoints4) watchpointsArray = requested
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
    // task-58/62：第 2-4 路 DWT comparator（固件 task-57/60）；旧固件忽略未知字段。
    watch2,
    watch3,
    watch4,
    watchpoints: watchpointsArray,
    triggerCondition: conditionEnabled.value ? (conditionSpec.value ?? undefined) : undefined,
    // 函数快照（task-55）：仅在 breakpoint + armed + 固件一次性快照开关时下发
    // snapshot_after；旧固件忽略未知字段 → 仍按“首帧冻结”降级。
    snapshotAfter: trigger === 'breakpoint' && snapshotArmed.value && snapshotFirmwareOneShot.value
      ? Math.min(CURVE_LOCAL_MAX_SNAPSHOT_AFTER, Math.max(0, Math.round(Number(snapshotAfterFrames.value) || 0)))
      : undefined,
    // variants 必须 halt（契约 §9.4）；固定地址仅在有寄存器表达式时 halt。breakpoint 忽略 halt。
    halt: trigger === 'periodic' ? (hasVariants ? true : needsHalt) : undefined,
  }
  // task-58：清空上一轮固件错误原文/backtrace 摘要，避免旧命中误导。
  watchStartError.value = ''
  // P0-1：本次启动编码由 startOnce 结果回填；先清空避免显示上一轮。
  localStartTransport.value = null
  localStartRequestBytes.value = null
  localStartFallbackReason.value = ''
  let started
  // P0-1：优先 CBS1 start-bin（精简版 ≤64 series 流式、不占 16 KiB 连续堆）；旧固件
  // 404/400/415/405/501 时函数内部自动回退旧 JSON start（16384 上限），结果带 transport。
  const startOnce = () => (groupSpecs.length === 1 && !hasVariants
    // 单组且无 variants：旧 root-series 语义（兼容旧固件）；多组/variants：groups 语义。
    ? startLocalCurveAuto({...requestBase, series: groupSpecs[0].series})
    : startLocalCurveAuto({...requestBase, groups: groupSpecs}))
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
        if (trigger === 'watchpoint') watchStartError.value = localErrorText(retryError)
        notice.value = {type: 'error', title: translate('curve.localStartFailed', {err: localErrorText(retryError)})}
        return
      }
    } else {
      statusError.value = true
      // task-58：DWT 资源不足（comparator 忙/不足）时固件 error 原文（含 DWT_CTRL 值）
      // 原样保留在 watchpoint 面板，不做二次改写。
      if (trigger === 'watchpoint') watchStartError.value = localErrorText(error)
      notice.value = {type: 'error', title: translate('curve.localStartFailed', {err: localErrorText(error)})}
      return
    }
  }
  // P0-1：记录本次启动实际编码（bin=CBS1 / json=旧固件回退）供状态与诊断显示。
  localStartTransport.value = started.transport ?? null
  localStartRequestBytes.value = started.requestBytes ?? null
  localStartFallbackReason.value = started.binFallbackReason ?? ''
  // task-62：核对固件是否真正按 sample_every 采样（start 响应 sample_every_max）。
  localSampleEveryMax.value = started.sampleEveryMax ?? null
  localPreciseActive.value = usePreciseSampleEvery.value && started.sampleEveryMax !== undefined
  if (usePreciseSampleEvery.value) {
    const requestedMax = Math.max(1, ...[...sampleEveryPlanBySeriesId.value.values()].map((plan) => plan.sampleEvery))
    if (started.sampleEveryMax === undefined) {
      // 固件忽略未知字段（旧固件）：不伪报精确速率，改为显示真实组速率。
      localCapabilities.value = {...localCapabilities.value, sampleEvery: false}
      notice.value = {type: 'warning', title: 'Firmware did not confirm series.sample_every (fixed target Hz may be ignored). Falling back to groups+weights divisor mapping; per-curve "valid" rate shows the real received rate.'}
    } else if (started.sampleEveryMax < requestedMax) {
      notice.value = {type: 'warning', title: `Firmware reports sample_every_max=${started.sampleEveryMax} (requested ${requestedMax}); actual per-curve rates are shown as plan ≈ / valid`}
    }
  }
  // task-62：固件实际配置的 comparator 数少于请求（旧固件忽略 W3/W4）时明确提示。
  if (trigger === 'watchpoint' && started.watchCount !== undefined) {
    const requestedCount = requestedWatchpoints.value.length
    if (started.watchCount < requestedCount) {
      notice.value = {type: 'warning', title: `Firmware configured ${started.watchCount} of ${requestedCount} DWT watchpoints (old firmware ignores W3/W4 aliases). W1/W2 keep working; update firmware for 4 comparators.`}
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
  watchCount.value = null
  watchpoint2Armed.value = null
  watchpoint2Address.value = null
  watchpoint2Hits.value = null
  lastWatchIndex.value = null
  watchpoint3Armed.value = null
  watchpoint3Address.value = null
  watchpoint3Hits.value = null
  watchpoint4Armed.value = null
  watchpoint4Address.value = null
  watchpoint4Hits.value = null
  watchFramesTriggered.value = false
  conditionArmed.value = null
  conditionMet.value = null
  conditionHits.value = null
  conditionTriggerSeq.value = null
  conditionFramesTriggered.value = false
  // task-55：函数快照标记按本次运行重置（armed 由 Arm 按钮设置，Start 时保留）。
  snapshotHit.value = false
  snapshotHitTime.value = null
  snapshotHitPc.value = null
  snapshotSeq.value = null
  // 固件是否真正支持 snapshot_after 以 status.snapshot_state 回读为准（旧固件无该字段）。
  snapshotFirmwareState.value = undefined
  snapshotFirmwareRun = requestBase.snapshotAfter !== undefined
  snapshotFirmwarePreFrames.value = null
  snapshotFirmwarePostFrames.value = null
  if (snapshotFinalizeTimer) { window.clearTimeout(snapshotFinalizeTimer); snapshotFinalizeTimer = 0 }
  wsTransport.value = 'http'
  wsFrames.value = 0
  wsDropped.value = 0
  wsLatencyMs.value = 0
  wsLastMessageAt = 0
  wsWindowStart = 0
  wsWindowFrames = 0
  localRateTimestamps = []
  resetValidCounters()
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
function seriesValidCounter(id: number): SeriesValidCounter {
  let counter = seriesValidCounters.get(id)
  if (!counter) {
    counter = {valid: 0, stale: 0}
    seriesValidCounters.set(id, counter)
  }
  return counter
}
function resetValidCounters(): void {
  seriesValidCounters.clear()
  validCountWindowSeconds.value = 0
  validCountVersion.value += 1
  localSeriesStale.value = null
}
/** 该 series 在当前帧是否 valid（无 per-series mask = 旧 CVF2，一律 valid）。 */
function frameSlotValid(frame: CurveLocalFrame, slot: number): boolean {
  return isFrameSeriesValid(frame, slot)
}

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
  // task-60：per-series valid/stale。stale tick 的 payload 字节为 0（固件约定），
  // 这里保持上一次有效值（sample-and-hold）并把该 tick 记入 stale 计数——绝不把
  // 0 当成真实采样点画进曲线；有效刷新率按 valid 计数单独显示。
  groupEntries.forEach((entry, slot) => {
    const counter = seriesValidCounter(entry.item.id)
    if (frameSlotValid(frame, slot)) {
      counter.valid += 1
      localLatestValues.set(entry.item.id, parts[slot] ?? new Uint8Array(entry.size))
    } else {
      counter.stale += 1
    }
  })
  // 多组轮转：本轮未采样的组沿用最近值，保持图表连续（不写假 0）。
  const rawValues = localSeriesOrder.map((item) => {
    const latest = localLatestValues.get(item.id)
    return latest ?? new Uint8Array(Math.max(1, Math.min(8, item.sample.size)))
  })
  const seconds = (frame.timestampUs - localBaseTimestampUs) / 1e6
  if (seconds > validCountWindowSeconds.value) validCountWindowSeconds.value = seconds
  pushSample(seconds, rawValues, localSeriesOrder, groupIndex, frame.seq)
  localRateTimestamps.push(seconds)
  trimLocalRateWindow(seconds)
  localPendingGroup = groupIndex
  // task-55：断点触发下，首个被应用/绘制的采样帧就是一次 FPB 命中 → 冻结/标记。
  // 旧固件没有 snapshot_* 字段时这是唯一的“命中后冻结”来源；新固件一次性快照
  // 仍会在 status 中回报 complete（见 refreshLocalStatus）。
  if (snapshotArmed.value && localTrigger.value === 'breakpoint' && !snapshotHit.value) {
    markSnapshotHit(seconds, frame.pc ?? latestFramePc, frame.seq)
  }
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
    // task-58：breakpoint/watchpoint 命中的同一时刻固件已保存帧指针 backtrace。
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
  // 函数快照只在 breakpoint 触发下有意义；切走后清空 arm/命中标记避免误导。
  if (localTrigger.value !== 'breakpoint') resetSnapshotState()
  watchStartError.value = ''
  scheduleDraw()
}

// ---- task-55：函数快照（breakpoint → 首个命中帧冻结/标记） -----------------
/**
 * 清空函数快照状态（arm + 命中标记）。图表冻结是独立开关，由用户 Unfreeze；
 * 不在这里改 chartFrozen，避免用户手动冻结被意外解除。
 */
function resetSnapshotState(): void {
  snapshotArmed.value = false
  snapshotHit.value = false
  snapshotHitTime.value = null
  snapshotHitPc.value = null
  snapshotSeq.value = null
  snapshotFirmwareState.value = undefined
  snapshotFirmwarePreFrames.value = null
  snapshotFirmwarePostFrames.value = null
  snapshotFirmwareRun = false
  if (snapshotFinalizeTimer) { window.clearTimeout(snapshotFinalizeTimer); snapshotFinalizeTimer = 0 }
}

/** ring 中 device seq → order 下标（-1=未找到；用于定位固件快照 trigger frame）。 */
function ringOrderForDeviceSeq(seq: number | null): number {
  if (seq === null || ringCount === 0) return -1
  for (let order = ringCount - 1; order >= 0; order--) {
    if (slotDeviceSeq[ringSlot(order)] === seq) return order
  }
  return -1
}

/** 快照命中的简短说明：明确只有“命中时刻的已采样变量 + 可选 PC”，无 SWO/backtrace/RTOS。 */
function snapshotHitNoticeText(): string {
  const timeText = snapshotHitTime.value === null ? '-' : `${snapshotHitTime.value.toFixed(3)}s`
  const pcText = snapshotHitPc.value === null ? '' : `, PC 0x${(snapshotHitPc.value >>> 0).toString(16).toUpperCase()}`
  const freezeText = snapshotFreezeOnHit.value ? 'display frozen' : 'marked on chart (freeze off)'
  return `Function snapshot hit at ≈${timeText}${pcText} — ${freezeText}. Captured only the selected variables at the FPB hit (throttled by Min interval); no SWO trace, call-stack backtrace or RTOS task data; with multiple groups one hit samples only one group (others keep their last values).`
}

function snapshotFirmwareCompleteText(): string {
  const post = snapshotFirmwarePostFrames.value
  const pre = snapshotFirmwarePreFrames.value
  return `Function snapshot complete (firmware one-shot${post === null ? '' : `, ${post} post frames`}${pre === null ? '' : `, ${pre} pre-hit frames retained`}). Device stopped sampling; chart is frozen and the trigger sample is marked.`
}

/**
 * 标记函数快照命中：冻结图表（可选）并把现有双光标 A 放到命中样本上。
 * 数据来源 = 首个命中断点帧（或固件 snapshot_trigger_seq 已知时定位该帧）；
 * 不生成额外寄存器/栈/RTOS 数据，也不宣称精确的命中 PC（帧里没有 PC 时不显示）。
 */
function markSnapshotHit(timeSeconds: number, pc?: number, seq?: number): void {
  if (snapshotHit.value) return
  snapshotHit.value = true
  snapshotHitTime.value = Number.isFinite(timeSeconds) ? timeSeconds : lastDataTimestamp()
  snapshotHitPc.value = typeof pc === 'number' && Number.isFinite(pc) ? (pc >>> 0) : null
  snapshotSeq.value = typeof seq === 'number' && Number.isFinite(seq) ? seq : null
  snapshotArmed.value = false
  if (snapshotFreezeOnHit.value) {
    // 先排队一次重绘（scheduleDraw 在冻结后直接返回），再冻结显示；采样继续。
    invalidateYRange()
    scheduleDraw()
    chartFrozen.value = true
  }
  if (ringCount > 0) {
    const order = Math.min(ringCount - 1, Math.max(0, lowerBoundTime(snapshotHitTime.value)))
    cursorA.value = {index: order, time: timestamps[ringSlot(order)]}
    cursorB.value = null
    cursorPulse.value += 1
    scheduleHoverDraw()
  }
  notice.value = {type: 'info', title: snapshotHitNoticeText()}
}

/**
 * Arm 函数快照并立即开始断点采样：选函数/地址 → 设断点 → 采样 → 命中后冻结/标记。
 * 需要 breakpoint trigger；若关闭 Freeze on hit 则只标记不冻结。
 */
function armFunctionSnapshot(): void {
  if (!isLocalSource.value || sampling.value) return
  if (localTrigger.value !== 'breakpoint') {
    localTrigger.value = 'breakpoint'
    onLocalTriggerChange()
  }
  const resolution = breakpointResolution.value
  if (!resolution || !resolution.ok) {
    notice.value = {type: 'error', title: translate('curve.localBreakpointInvalid')}
    return
  }
  if (seriesList.value.length === 0) {
    notice.value = {type: 'error', title: translate('curve.localSelectHint')}
    return
  }
  resetSnapshotState()
  const symbol = selectedFunctionAddress.value === null
    ? null
    : chooseBreakpointFunction(elfFunctionSymbols.value, selectedFunctionAddress.value)
  snapshotFunctionName.value = symbol ? symbol.displayName : formatHex32(resolution.address)
  snapshotBoundaryAddr.value = resolution.address
  snapshotArmed.value = true
  void startLocalSampling()
}

function clearSnapshotMark(): void {
  resetSnapshotState()
  if (chartFrozen.value) toggleFreeze()
  clearCursors()
}

function startSampling(): void {
  if (sampling.value) return
  void startLocalSampling()
}

function stopSampling(): void {
  if (!sampling.value) return
  stopLocalSampling(false)
  sampling.value = false
  localActive.value = false
}

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
  // task-62 P1-15：把本次采样增量同步给图表 Worker 自己的 ring（小消息；不复制整窗）。
  postChartSampleToWorker(time, slot, snapshot)
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
  // 主线程 ring 与 Worker ring 同步复位（否则 order/seq 映射错位）。
  resetChartWorkerRing()
  droppedPending = 0
  if (statsTimer) { window.clearTimeout(statsTimer); statsTimer = 0 }
  sampleCount.value = 0
  failedCount.value = 0
  droppedCount.value = 0
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
  } else {
    constantReference = rawValues.map((bytes) => bytes.slice())
    constantSamples = 0
    }
}

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
  if (false) {
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
    return false ? node.sampleable : node.liveSamplable
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
/** W3/W4 状态行（W1/W2 保留原有 translate 文案行，避免重复）。 */
const extraWatchStatusRows = computed(() => watchStatusRows.value.filter((row) => row.index >= 3))

const visibleVariableCount = computed(() => variableRows.value.filter((row) => row.kind === 'variable').length)
const unsampleableCount = computed(() => {
  const info = elfInfo.value
  if (!info) return 0
  return info.variables.filter((node) => !node.sampleable).length
})

/** O(1) 选中集合：替代每行线性扫 seriesList（300 行 × 最多 64 选中）。 */
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
    mode: false ? 'halt' : 'live',
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
  if (!isLocalSource.value && seriesList.value.length >= CURVE_LOCAL_MAX_TOTAL_SERIES) {
    globalNotify(translate('curve.maxSeries', {max: CURVE_LOCAL_MAX_TOTAL_SERIES}), 'warning')
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
  return isLocalSource.value ? localSeriesLimit : CURVE_LOCAL_MAX_TOTAL_SERIES
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
    // 一次性替换数组：避免批量选择时逐项 reactive 更新/重绘。
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

/**
 * task-49：目标检测区简化后的内置自动匹配 adapter。
 * task-48 的 target-eng API 尚未落库；先在本页用 KNOWN_TARGET_PROFILES 按
 * DEV_ID -> Flash 做只读匹配（不暴露 profile 选择/不写 selected profile）。
 * task-48 API 就绪后只需替换此函数实现，UI/诊断字段不变。
 */
interface AutoTargetModel {
  label: string
  basis: 'dev-id' | 'flash' | 'detected-group'
}
const autoTargetModel = computed<AutoTargetModel | null>(() => {
  const detected = detectedTarget.value
  if (!detected.detected) return null
  const seriesKey = (value: string | undefined) => (value ?? '').toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 6)
  const detectedSeries = seriesKey(detected.series)
  const seriesCompatible = (series: string | undefined) => {
    if (!detectedSeries || !series) return true
    return seriesKey(series) === detectedSeries
  }
  if (detected.devId) {
    const byDevId = knownTargetProfiles.find((profile) => profile.devId === detected.devId
      && seriesCompatible(profile.series))
    if (byDevId) return {label: byDevId.mcu || byDevId.id, basis: 'dev-id'}
  }
  if (detected.flashKib) {
    // 只有系列兼容时才用 Flash 兜底，避免把 H7 的 1 MiB 误配到 F4 型号。
    const byFlash = knownTargetProfiles.find((profile) => profile.flashKib === detected.flashKib
      && seriesCompatible(profile.series))
    if (byFlash) return {label: byFlash.mcu || byFlash.id, basis: 'flash'}
  }
  return {label: detected.baseGroup || detected.group || detected.series || '-', basis: 'detected-group'}
})
/** 内置自动匹配仅用于显示/诊断；Flash 超界仍以 detected flash 为准。 */
const compactTarget = computed(() => compactTargetProfileApi(detectedTarget.value, null))
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
/** task-58：第二路 comparator 配置（未勾选时不下发）。 */
const watchpoint2Spec = computed<CurveLocalWatchpointSpec | null>(() => {
  if (!watchSecondEnabled.value) return null
  const address = parseWatchAddressInput(watch2AddressInput.value)
  if (address === null) return null
  if (address % watch2Size.value !== 0) return null
  return {address, size: watch2Size.value, access: watch2Access.value}
})
/** task-62：第 3/4 路 comparator 配置（未勾选/地址非法时 null）。 */
function extraWatchSpec(slot: ExtraWatchSlot): CurveLocalWatchpointSpec | null {
  if (!slot.enabled) return null
  const address = parseWatchAddressInput(slot.addressInput)
  if (address === null) return null
  if (address % slot.size !== 0) return null
  return {address, size: slot.size, access: slot.access}
}
const watchpoint3Spec = computed<CurveLocalWatchpointSpec | null>(() => extraWatchSpec(watchThird.value))
const watchpoint4Spec = computed<CurveLocalWatchpointSpec | null>(() => extraWatchSpec(watchFourth.value))
/** 本地下发的全部 watchpoint（按 1..4 顺序；最多 4 路，固件 task-60）。 */
const requestedWatchpoints = computed<CurveLocalWatchpointSpec[]>(() => {
  const list: CurveLocalWatchpointSpec[] = []
  if (watchpointSpec.value) list.push(watchpointSpec.value)
  if (watchpoint2Spec.value) list.push(watchpoint2Spec.value)
  if (watchpoint3Spec.value) list.push(watchpoint3Spec.value)
  if (watchpoint4Spec.value) list.push(watchpoint4Spec.value)
  return list.slice(0, CURVE_LOCAL_MAX_WATCHPOINTS)
})
/** 已启用的 comparator 配置数（含非法地址的槽位也计入，用于报错）。 */
const enabledWatchpointSlotCount = computed(() => {
  let count = 1
  if (watchSecondEnabled.value) count += 1
  if (watchThird.value.enabled) count += 1
  if (watchFourth.value.enabled) count += 1
  return count
})
/** 状态显示：优先 canonical status.watchpoints 数组，否则用 legacy watchpointN_* 字段。 */
interface WatchStatusRow {index: number; label: string; armed: boolean | null; address: number | null; hits: number | null}
const watchStatusRows = computed<WatchStatusRow[]>(() => {
  void validCountVersion.value
  const status = localStatusInfo.value
  const rows: WatchStatusRow[] = []
  const canonical = status?.watchpoints
  const legacy: Array<{armed: boolean | null; address: number | null; hits: number | null; fallbackAddress: number | null}> = [
    {armed: watchpointArmed.value, address: status?.watchpointAddress ?? null, hits: watchpointHits.value, fallbackAddress: watchpointSpec.value?.address ?? null},
    {armed: watchpoint2Armed.value, address: watchpoint2Address.value, hits: watchpoint2Hits.value, fallbackAddress: watchpoint2Spec.value?.address ?? null},
    {armed: watchpoint3Armed.value, address: watchpoint3Address.value, hits: watchpoint3Hits.value, fallbackAddress: watchpoint3Spec.value?.address ?? null},
    {armed: watchpoint4Armed.value, address: watchpoint4Address.value, hits: watchpoint4Hits.value, fallbackAddress: watchpoint4Spec.value?.address ?? null},
  ]
  const count = Math.max(1, Math.min(CURVE_LOCAL_MAX_WATCHPOINTS, watchCount.value ?? enabledWatchpointSlotCount.value))
  for (let index = 0; index < count; index++) {
    const entry = canonical?.find((item) => item.index === index + 1) ?? canonical?.[index]
    const row: WatchStatusRow = {
      index: index + 1,
      label: `W${index + 1}`,
      armed: entry?.armed ?? legacy[index]?.armed ?? null,
      address: entry?.address ?? legacy[index]?.address ?? legacy[index]?.fallbackAddress ?? null,
      hits: entry?.hits ?? legacy[index]?.hits ?? null,
    }
    rows.push(row)
  }
  return rows
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
  if (watchSecondEnabled.value) {
    if (!watch2AddressInput.value.trim()) return translate('curve.watchpointAddressRequired')
    if (watchpoint2Spec.value === null) return translate('curve.watchpointInvalid')
    if (isDuplicateWatchpoint(watchpointSpec.value, watchpoint2Spec.value)) {
      return translate('curve.watchpointDuplicatesFirst')
    }
  }
  // task-62：legacy 编码要求 comparator slot 连续（firmware: "requires the previous
  // watchpoint slot to be configured"）；canonical 数组也按连续槽位下发，故 UI 统一要求。
  if (watchThird.value.enabled && !watchSecondEnabled.value) {
    return 'W3 requires W2 to be enabled (DWT comparator slots must be contiguous)'
  }
  if (watchFourth.value.enabled && !watchThird.value.enabled) {
    return 'W4 requires W3 to be enabled (DWT comparator slots must be contiguous)'
  }
  // task-62：第 3/4 路（固件 task-60，最多 4 路）。资源是否足够由固件原文报错。
  for (const [slotIndex, slot] of [[3, watchThird.value], [4, watchFourth.value]] as const) {
    if (!slot.enabled) continue
    if (!slot.addressInput.trim()) return translate('curve.watchpointAddressRequired')
    const spec = extraWatchSpec(slot)
    if (spec === null) return translate('curve.watchpointInvalid')
    for (const [offset, other] of requestedWatchpoints.value.entries()) {
      if (offset >= slotIndex - 1) break
      if (isDuplicateWatchpoint(other, spec)) return `W${slotIndex} duplicates W${offset + 1}`
    }
  }
  if (enabledWatchpointSlotCount.value > CURVE_LOCAL_MAX_WATCHPOINTS) {
    return `Firmware supports at most ${CURVE_LOCAL_MAX_WATCHPOINTS} DWT watchpoints`
  }
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

// ---- task-37 / task-55：group weight / 每变量刷新率 / 预设 / watch / 光标 / 派生 / 诊断 ----
/** 总帧率（Hz）：采样中优先用实测帧率（≈），否则用名义值 1000/interval。 */
const localTotalFrameRateHz = computed(() => {
  return localIntervalMs.value > 0 ? 1000 / localIntervalMs.value : 0
})
/**
 * 每组生效权重：目标率分桶路径由 divisor 决定（不可手改）；默认路径沿用现有
 * 手改的 groupWeights。下标与下发 groups 顺序一致。
 */
const effectiveGroupWeights = computed<number[]>(() => {
  const count = localPlannedGroups.value
  const source = plannedGroups.value.map((group) => group.weight ?? 1)
  return Array.from({length: count}, (_, index) => Math.min(CURVE_LOCAL_MAX_GROUP_WEIGHT, Math.max(1, source[index] ?? 1)))
})
/** Σ weight（固件限制 ≤64）。 */
const localGroupWeightSum = computed(() => effectiveGroupWeights.value.slice(0, localActiveGroupCount.value).reduce((sum, weight) => sum + weight, 0))
/** 每组实际帧刷新率 ≈ 总帧率 × weight/Σweight（实测总帧率优先，标注为 ≈ 不伪造精确值）。 */
const localGroupActualRates = computed(() => {
  const count = localActiveGroupCount.value
  const total = localGroupWeightSum.value
  if (total <= 0 || localTotalFrameRateHz.value <= 0) return [] as number[]
  return effectiveGroupWeights.value.slice(0, count).map((weight) => localTotalFrameRateHz.value * weight / total)
})
/** series.id → 所属计划 group 下标（用于 Selected 行每变量实际刷新率）。 */
const rateGroupIndexBySeriesId = computed(() => {
  const map = new Map<number, number>()
  plannedGroups.value.forEach((group, index) => {
    for (const item of group.items) map.set(item.id, index)
  })
  return map
})
/** 名义目标刷新率（未设置=全局 1000/interval；divisor=2/4/8 依次降低）。 */
function targetRateForDivisor(divisor: unknown): number {
  const base = localIntervalMs.value > 0 ? 1000 / localIntervalMs.value : 0
  return base / normalizeRateDivisor(divisor)
}
/** 人类可读速率（整数省略小数；≤1 位小数足够，12.5/6.25 保留）。 */
function formatRateHz(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return '0'
  const rounded = Math.round(value * 100) / 100
  return String(rounded)
}
/** target 下拉标签：纯数字/单位（不新增 locale key）。 */
function rateDivisorLabel(divisor: number): string {
  return `${formatRateHz(targetRateForDivisor(divisor))} Hz (÷${normalizeRateDivisor(divisor)})`
}
function rateDivisorTitle(divisor: unknown): string {
  return `Target refresh rate: global tick ${formatRateHz(1000 / Math.max(1, localIntervalMs.value))} Hz ÷ ${normalizeRateDivisor(divisor)}. Implemented as firmware group weight (no per-variable firmware divisor field).`
}
/** 每条曲线的目标刷新率：优先用户 Hz 目标；否则由 divisor 推导（缺省=全局 tick 率）。 */
function seriesTargetHz(item: CurveSeries): number {
  const target = item.rateTargetHz ?? 0
  if (target > 0) return target
  return targetRateForDivisor(effectiveRateDivisor(item))
}
/** 每条曲线的计划有效刷新率估计（≈ 组帧率 / sample_every；PC 门控会进一步降低）。 */
function seriesActualRateHz(item: CurveSeries): number | null {
  const index = rateGroupIndexBySeriesId.value.get(item.id)
  if (index === undefined) return null
  const weights = effectiveGroupWeights.value
  const total = weights.reduce((sum, weight) => sum + weight, 0)
  if (!(total > 0)) return null
  const groupRate = localTotalFrameRateHz.value * (weights[index] ?? 1) / total
  const precise = sampleEveryPlanBySeriesId.value.get(item.id)
  if (localPreciseActive.value && precise) return groupRate / Math.max(1, precise.sampleEvery)
  return groupRate
}
/** 实测有效刷新率（按收到帧的 valid 计数/时间窗口；未知返回 null，不猜）。 */
function seriesValidRateHz(item: CurveSeries): number | null {
  const counter = seriesValidCounters.get(item.id)
  if (!counter || counter.valid <= 0) return null
  const elapsed = validCountWindowSeconds.value
  if (!(elapsed > 0)) return null
  return counter.valid / elapsed
}
/** 本次会话累计有效/ stale 采样槽（每 series 每个 tick-slot 记一次）。 */
const totalValidCount = computed(() => {
  void validCountVersion.value
  let sum = 0
  for (const counter of seriesValidCounters.values()) sum += counter.valid
  return sum
})
const totalStaleCount = computed(() => {
  void validCountVersion.value
  let sum = 0
  for (const counter of seriesValidCounters.values()) sum += counter.stale
  return sum
})
function seriesRateLabel(item: CurveSeries): string {
  const hz = seriesActualRateHz(item)
  const valid = seriesValidRateHz(item)
  const precise = sampleEveryPlanBySeriesId.value.get(item.id)
  const preciseTag = localPreciseActive.value && precise ? `÷${precise.sampleEvery} ` : ''
  const validText = valid === null ? '' : ` · valid ${formatRateHz(valid)} Hz`
  return hz === null ? (validText ? validText.replace(/^ · /, '') : '-') : `${preciseTag}≈${formatRateHz(hz)} Hz${validText}`
}
function seriesRateTitle(item: CurveSeries): string {
  const groupIndex = rateGroupIndexBySeriesId.value.get(item.id)
  const target = seriesTargetHz(item)
  const precise = sampleEveryPlanBySeriesId.value.get(item.id)
  const weight = groupIndex === undefined ? '-' : `${effectiveGroupWeights.value[groupIndex] ?? '-'} / Σ${localGroupWeightSum.value || '-'}`
  const actual = seriesActualRateHz(item)
  const valid = seriesValidRateHz(item)
  const counter = seriesValidCounters.get(item.id)
  const mode = localPreciseActive.value
    ? `firmware per-series sample_every=${precise?.sampleEvery ?? 1} (exact integer division of group tick; reported max ${localSampleEveryMax.value ?? '-'})`
    : 'legacy groups+weight fallback (firmware did not confirm sample_every)'
  return `Target ${formatRateHz(target)} Hz · ${mode} · group weight ${weight} · plan ≈${actual === null ? '-' : formatRateHz(actual)} Hz`
    + ` · measured valid ${valid === null ? '-' : formatRateHz(valid)} Hz (${counter?.valid ?? 0} valid / ${counter?.stale ?? 0} stale tick-slots).`
    + ' sample_every samples this series once per N group ticks; the other ticks are stale and hold the last valid value on the chart.'
}
/** 设置单条曲线目标 Hz（≤0/NaN = 清除，跟随全局 tick 率）。 */
function setSeriesTargetHz(item: CurveSeries, value: unknown): void {
  const numeric = typeof value === 'number' && Number.isFinite(value) ? value : Number(value)
  item.rateTargetHz = Number.isFinite(numeric) && numeric > 0 ? Math.min(100000, numeric) : undefined
}
/** 设置单条曲线目标 divisor（1=清除/跟随全局）。 */
function setSeriesRateDivisor(item: CurveSeries, value: unknown): void {
  const divisor = normalizeRateDivisor(value)
  item.rateDivisor = divisor === 1 ? undefined : divisor
  if (divisor !== 1) item.rateTargetHz = undefined
}
/** 批量设置当前已选曲线的目标 Hz（精确模式）；旧固件自动映射到最近 divisor fallback。 */
function applyBatchRate(): void {
  const divisor = normalizeRateDivisor(batchRateDivisor.value)
  for (const item of seriesList.value) {
    if (item.derived) continue
    item.rateDivisor = divisor === 1 ? undefined : divisor
    if (divisor !== 1) item.rateTargetHz = undefined
  }
}
/** 批量设置目标 Hz。 */
function applyBatchTargetHz(): void {
  const target = Number(batchTargetHz.value)
  for (const item of seriesList.value) {
    if (item.derived) continue
    item.rateTargetHz = Number.isFinite(target) && target > 0 ? target : undefined
  }
}

function ensureGroupWeights(count: number): void {
  const next = groupWeights.value.slice(0, Math.max(1, count))
  while (next.length < Math.max(1, count)) next.push(1)
  groupWeights.value = next
}

watch(localActiveGroupCount, (count) => ensureGroupWeights(count), {immediate: true})

function setGroupWeight(index: number, value: number): void {
  if (index < 0 || index >= groupWeights.value.length) return
  const next = [...groupWeights.value]
  next[index] = Math.min(CURVE_LOCAL_MAX_GROUP_WEIGHT, Math.max(1, Math.round(Number(value) || 1)))
  groupWeights.value = next
}

/** 容量错误（>8 组 / >64 路等）：明确拒绝，不静默丢系列。 */
const ratePlanErrorText = computed(() => {
  if (!hasAnyRatePlan.value) return ''
  const plan = ratePlan.value
  switch (plan.error) {
    case 'too_many_groups':
      return `Rate plan needs ${plan.groupCount} groups (> firmware max ${CURVE_LOCAL_MAX_GROUPS}); reduce variables or use fewer rate buckets. Series: ${plan.seriesCount}/${CURVE_LOCAL_MAX_TOTAL_SERIES}.`
    case 'too_many_series':
      return `Rate plan has ${plan.seriesCount} series (> firmware max ${CURVE_LOCAL_MAX_TOTAL_SERIES}); reduce variables.`
    case 'too_many_bytes':
      return `A single series payload exceeds ${CURVE_LOCAL_MAX_SAMPLE_BYTES} bytes (firmware group limit); cannot send rate plan.`
    default:
      return ''
  }
})
/**
 * 目标速率 ≠ 可实现速率提示：固件一个 tick 只采样一个 group，组帧率只能按
 * weight 份额归一化，所以绝对 Hz 不保证（尤其只有一个 divisor>1 桶时）。
 */
const rateMismatchWarning = computed(() => {
  if (!hasAnyRatePlan.value || ratePlan.value.error) return ''
  const mismatched: string[] = []
  for (const [index, group] of plannedGroups.value.entries()) {
    if (group.divisor === 1) continue
    const target = targetRateForDivisor(group.divisor)
    if (target <= 0) continue
    const nominalActual = localIntervalMs.value > 0
      ? (1000 / localIntervalMs.value) * ((effectiveGroupWeights.value[index] ?? 1) / Math.max(1, localGroupWeightSum.value))
      : 0
    if (Math.abs(nominalActual - target) > Math.max(0.01 * target, 0.02)) {
      mismatched.push(`÷${group.divisor} target ${formatRateHz(target)} Hz → ≈${formatRateHz(nominalActual)} Hz`)
    }
  }
  if (mismatched.length === 0) return ''
  return `Group weights are relative shares of the global tick, so absolute target rates cannot be guaranteed (${mismatched.join('; ')}). Keep a ÷1 bucket for full rate, and increase Local interval if you need lower absolute rates.`
})
/** task-62：精确模式下目标 Hz 高于 group tick 率时无法更快（sample_every=1 封顶）。 */
const preciseRateWarning = computed(() => {
  if (!usePreciseSampleEvery.value) return ''
  const capped = [...sampleEveryPlanBySeriesId.value.values()].filter((plan) => plan.capped)
  if (capped.length === 0) return ''
  const tickPerGroup = nominalTickHz.value / Math.max(1, plannedPreciseGroups.value.length)
  return `${capped.length} curve(s) target above the group tick rate (${formatRateHz(tickPerGroup)} Hz); firmware cannot sample faster than sample_every=1. Increase the global interval or lower the target.`
})
/** 每变量刷新率界面仅在设备本地采样模式显示（Web-DAP 路径没有 groups/weights）。 */
const showRateControls = computed(() => isLocalSource.value)


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
      // task-62：固件 task-60 per-series 速率/stale 摘要（旧固件缺省）。
      sampleEveryMax: status.sampleEveryMax ?? null,
      seriesStale: status.seriesStale ?? null,
      heapFree: status.heapFree ?? null,
      heapLargest: status.heapLargest ?? null,
    } : null,
    ring: {points: ringCount, selected: seriesList.value.length, maxSeries: seriesLimit.value},
    // P0-1：启动请求编码（CBS1 流式二进制 / 旧 JSON 回退）与字节数。
    startRequest: {
      transport: localStartTransport.value,
      bytes: localStartRequestBytes.value,
      fallbackReason: localStartFallbackReason.value || null,
    },
    render: {
      yAxisMode: yAxisMode.value,
      renderAll: renderAll.value,
      drawn: drawnSeriesList.value.length,
      pollsPerSecond: Math.round(pollCounterState.pollsPerSecond * 10) / 10,
      avgDrawMs: perfCounter.draws > 0 ? Math.round((perfCounter.drawMsTotal / perfCounter.draws) * 100) / 100 : 0,
      draws: perfCounter.draws,
    },
    // task-62 P1-15：Worker 内 ring + 绘制统计（主线程不再每帧复制整窗曲线）。
    chartWorker: {
      mode: chartRenderMode.value,
      samplePosts: chartSamplePosts,
      last: chartWorkerStats,
      derivedFallback: drawnSeriesList.value.some((item) => item.derived),
      // task-67：WebGL 后备（feature flag 默认 off；renderer 为 Worker 实际后端）。
      webglMode: chartWebglMode,
      renderer: chartWebglRenderer,
      webglFallback: chartWebglFallback,
    },
    recording: {active: recordingActive.value, rows: recordRowCount.value, saved: recordings.value.length},
    activeFrame: {
      pc: activePc.value !== null ? `0x${activePc.value.toString(16).toUpperCase()}` : null,
      staleSeries: staleSeriesKeys.value.size,
      line: activePcLocation.value ? `${typeof activePcLocation.value.file === 'string' ? activePcLocation.value.file : (activePcLocation.value.file as any)?.relativePath ?? ''}:${activePcLocation.value.line}` : null,
    },
    target: {
      detected: detectedTarget.value.detected,
      series: detectedTarget.value.series,
      group: detectedTarget.value.baseGroup,
      devId: detectedTarget.value.devId,
      flashKib: detectedTarget.value.flashKib,
      autoModel: autoTargetModel.value,
      elfFlashRange: elfFlashRange.value,
      warning: targetFlashWarning.value,
    },
    lineIndex: lineIndex.value ? {files: lineIndex.value.files.length, rows: lineIndex.value.rows.length} : null,
    watchpoint: {
      armed: watchpointArmed.value,
      hits: watchpointHits.value,
      triggerSeq: watchTriggerSeq.value,
      count: watchCount.value,
      second: {
        enabled: watchSecondEnabled.value,
        armed: watchpoint2Armed.value,
        address: watchpoint2Address.value !== null ? `0x${watchpoint2Address.value.toString(16).toUpperCase()}` : null,
        hits: watchpoint2Hits.value,
      },
      lastWatchIndex: lastWatchIndex.value,
      // task-62：W3/W4（固件 task-60）状态行 + 本地下发的 comparator 数。
      rows: watchStatusRows.value,
      requested: requestedWatchpoints.value.length,
      // 固件 start 失败原文（DWT 资源不足等）；未失败为 null。
      startError: watchStartError.value || null,
    },
    playbackState: {active: playbackMode.value, index: playbackIndex.value},
    cursor: {a: cursorA.value?.time ?? null, b: cursorB.value?.time ?? null},
    // task-55：每变量刷新率计划（divisor 分桶 + 权重份额）与函数快照状态。
    rateGroups: {
      overrides: hasRateOverrides.value,
      baseRateHz: localIntervalMs.value > 0 ? 1000 / localIntervalMs.value : 0,
      groups: plannedGroups.value.map((group) => ({divisor: group.divisor, weight: group.weight, series: group.items.length})),
      totalWeight: localGroupWeightSum.value,
      actualRates: localGroupActualRates.value.map((rate) => Math.round(rate * 100) / 100),
      error: ratePlan.value.error,
      mismatchWarning: rateMismatchWarning.value || null,
      // task-62：固件 task-60 sample_every 精确模式 + CVF valid/stale 计数。
      precise: usePreciseSampleEvery.value,
      preciseActive: localPreciseActive.value,
      sampleEveryMax: localSampleEveryMax.value,
      sampleEveryPlan: [...sampleEveryPlanBySeriesId.value.values()].length,
      cappedTargets: [...sampleEveryPlanBySeriesId.value.values()].filter((plan) => plan.capped).length,
      preciseWarning: preciseRateWarning.value || null,
      validSlots: totalValidCount.value,
      staleSlots: totalStaleCount.value,
    },
    commandIndex: {
      variables: commandIndex.value.variables.length,
      functions: commandIndex.value.functions.length,
      version: commandIndexVersion,
      focusedPath: focusedVariableKey.value || null,
    },
    functionSnapshot: {
      armed: snapshotArmed.value,
      hit: snapshotHit.value,
      hitTime: snapshotHitTime.value,
      pc: snapshotHitPc.value,
      deviceSeq: snapshotSeq.value,
      freezeOnHit: snapshotFreezeOnHit.value,
      firmwareOneShot: snapshotFirmwareOneShot.value,
      firmwareState: snapshotFirmwareState.value ?? null,
      preFrames: snapshotFirmwarePreFrames.value,
      postFrames: snapshotFirmwarePostFrames.value,
    },
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

// ---- task-62 P1-17：命令索引发布 + ?focusVar= 聚焦 ---------------------------
/** 构建并发布命令索引：window.__dapCommandIndex + `dap:command-index` 事件。 */
function publishCommandIndex(): void {
  const index = elfInfo.value
    ? buildDapCommandIndex(elfInfo.value.variables, elfFunctionSymbols.value)
    : emptyDapCommandIndex()
  commandIndex.value = index
  commandIndexVersion += 1
  const scope = window as unknown as Record<string, unknown>
  scope[DAP_COMMAND_INDEX_WINDOW_KEY] = index
  scope[DAP_COMMAND_INDEX_VERSION_KEY] = commandIndexVersion
  try {
    window.dispatchEvent(new CustomEvent(DAP_COMMAND_INDEX_EVENT, {detail: index}))
  } catch {
    // 极旧环境无 CustomEvent：window 属性仍已更新（命令面板读当前值即可）。
  }
  // ELF 刚解析完：如果有暂存的 focusVar，现在尝试聚焦。
  tryFocusPendingVariable()
}

/** 聚焦一个命令索引变量：加入曲线（若未选）并高亮/滚动变量行。 */
function focusVariableEntry(entry: DapCommandIndexVariable): void {
  const info = elfInfo.value
  if (!info) return
  const byPath = info.variables.find((candidate) => {
    const qualified = (candidate.qualifiedName || '').trim() || candidate.name
    return qualified === entry.path && (candidate.address >>> 0) === (entry.address >>> 0)
  })
  const node = byPath ?? info.variables.find((candidate) => candidate.name === entry.name
    && (candidate.address >>> 0) === (entry.address >>> 0))
  if (!node) {
    globalNotify(`Variable "${entry.path}" not found in the current ELF`, 'warning')
    return
  }
  // 变量树顶层 path 就是 node.name；series key = `name@address`（与变量树去重键一致）。
  const key = nodeKey(node.name, node)
  focusedVariableKey.value = key
  const existing = seriesList.value.find((item) => item.key === key)
  if (existing) {
    existing.visible = true
    invalidateYRange()
    scheduleDraw()
    globalNotify(`Focused "${entry.path}" (already selected)`, 'info')
  } else if (isLocalSource.value && sampling.value) {
    globalNotify(`Focused "${entry.path}" — stop sampling to add it as a curve`, 'warning')
  } else {
    const series = newSeries(node.shortName || node.name, key, node.typeName || '', node)
    if (series) globalNotify(`Focused "${entry.path}" — added to curves`, 'success')
    else globalNotify(`Variable "${entry.path}" cannot be sampled in the current source/mode`, 'warning')
  }
  // 让变量行出现在当前过滤结果里并滚动到可见（不改变 ELF/采样状态）。
  void nextTick(() => {
    variableSearch.value = node.name
    variableSearchInput.value = node.name
    variableDisplayLimit.value = Math.max(variableDisplayLimit.value, VARIABLE_PAGE_SIZE)
    if (focusScrollTimer) { window.clearTimeout(focusScrollTimer); focusScrollTimer = 0 }
    focusScrollTimer = window.setTimeout(() => {
      focusScrollTimer = 0
      const escaped = typeof CSS !== 'undefined' && typeof CSS.escape === 'function'
        ? CSS.escape(key)
        : key.replace(/"/g, '\\"')
      document.querySelector(`[data-key="${escaped}"]`)?.scrollIntoView({block: 'center', behavior: 'smooth'})
    }, 80)
  })
}

/** 解析 ?focusVar= / dap:focus-variable 事件：ELF 未就绪时暂存，解析后自动应用。 */
function applyFocusVariable(raw: unknown): void {
  const text = typeof raw === 'string' ? raw : ''
  if (!text.trim()) return
  let decoded = text.trim()
  try {
    decoded = decodeURIComponent(decoded)
  } catch {
    // 非法编码：按原字符串匹配（不伪造）。
  }
  pendingFocusVar = decoded
  tryFocusPendingVariable()
}

function tryFocusPendingVariable(): void {
  if (!pendingFocusVar || !elfInfo.value) return
  const path = pendingFocusVar
  pendingFocusVar = ''
  const match = findCommandIndexVariable(commandIndex.value, path)
  if (!match) {
    globalNotify(`Variable "${path}" not found in current ELF command index`, 'warning')
    return
  }
  focusVariableEntry(match)
}

function onDapFocusVariable(event: Event): void {
  const detail = (event as CustomEvent<{path?: string}>).detail
  applyFocusVariable(detail?.path)
}

/** ?focusVar= 变化（命令面板跳转）→ 等 ELF 就绪后聚焦。 */
watch(() => route.query.focusVar, (value) => applyFocusVariable(value), {immediate: true})

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
    objectRootKey.value = defaultObjectRoot(discoverObjectRoots(result.variables), {preferredNames: DEFAULT_OBJECT_ROOT_NAMES})?.key ?? null
    objectPresetVersion.value += 1
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
    // task-62 P1-17：发布命令面板索引（变量/函数）并应用暂存的 focusVar。
    publishCommandIndex()
  } catch (error) {
    elfInfo.value = null
    elfFunctionSymbols.value = []
    selectedFunctionAddress.value = null
    const message = error instanceof Error ? error.message : 'unknown error'
    elfError.value = translate('curve.elfInvalid', {err: message})
    publishCommandIndex()
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

// ---- task-53：Objects/Watch 集成 -----------------------------------------
function applyObjectRootChange(key: string, root: ObjectRoot): void {
  objectRootKey.value = key || root.key
  objectPresetVersion.value += 1
}

function applyObjectSelected(payload: ObjectNodeRef): void {
  if (newSeries(payload.path, payload.curveKey, payload.typeName, payload.node)) {
    activeSourceKey.value = payload.curveKey
  }
}

function applyObjectAddCurves(payload: ObjectBatchPayload): void {
  if (payload.truncated) {
    globalNotify(translate('curve.objectBatchTruncated', {count: payload.nodes.length, total: payload.total}), 'warning')
  }
  if (payload.nodes.length === 0) return
  addSelectedRowsBulk(payload.nodes.map((node) => ({name: node.path, key: node.curveKey, node: node.node})))
}

function ensureObjectSeries(node: ObjectNodeRef): void {
  if (!isWatched(node.curveKey)) toggleWatch(node.curveKey, node.path)
  if (seriesList.value.some((item) => item.key === node.curveKey)) return
  newSeries(node.path, node.curveKey, node.typeName, node.node)
}

function applyObjectToggleWatch(payload: ObjectToggleWatchPayload): void {
  toggleWatch(payload.curveKey, payload.path)
  if (payload.watched) ensureObjectSeries(payload)
}

function applyObjectWatchNodes(payload: ObjectBatchPayload): void {
  if (payload.truncated) {
    globalNotify(translate('curve.objectBatchTruncated', {count: payload.nodes.length, total: payload.total}), 'warning')
  }
  for (const node of payload.nodes) ensureObjectSeries(node)
}

function applyObjectJumpSource(payload: ObjectSourceJumpPayload): void {
  elfView.value = 'source'
  sourceViewEverOpened.value = true
  void nextTick(async () => {
    const file = payload.relativeFile || payload.path
    if (file) await sourceBrowserRef.value?.openFile?.(file)
    if (payload.line > 0) sourceBrowserRef.value?.scrollToLine?.(payload.line)
  })
}

function applyObjectSavePreset(payload: ObjectPresetSavePayload): void {
  saveObjectWatchPreset({
    name: payload.name,
    rootPath: payload.rootPath || activeObjectRoot.value?.qualifiedName || '',
    fields: payload.keys,
  })
  objectPresetVersion.value += 1
}

function applyObjectLoadPreset(preset: ObjectPreset): void {
  const root = objectRoots.value.find((candidate) => candidate.qualifiedName === preset.rootPath)
    ?? activeObjectRoot.value
  if (!root) return
  if (objectRootKey.value !== root.key) objectRootKey.value = root.key
  const keys = preset.keys?.length ? preset.keys : []
  if (keys.length === 0) {
    objectPresetVersion.value += 1
    return
  }
  const matches = findObjectNodes(root, keys, {maxNodes: 512})
  addSelectedRowsBulk(matches.map((match) => {
    const ref = objectNodeRef({key: root.key, name: root.name}, match.node.node, match.node.path, match.node.depth)
    return {name: ref.path, key: ref.curveKey, node: ref.node}
  }))
  objectPresetVersion.value += 1
}

function applyObjectDeletePreset(id: string): void {
  deleteObjectWatchPreset(id)
  objectPresetVersion.value += 1
}

// ObjectBrowser emits 在 SFC 模板推断中为宽松函数类型；统一 rest-arg adapter。
const onObjectRootChange = (...args: unknown[]): void => applyObjectRootChange(args[0] as string, args[1] as ObjectRoot)
const onObjectSelected = (...args: unknown[]): void => applyObjectSelected(args[0] as ObjectNodeRef)
const onObjectAddCurves = (...args: unknown[]): void => applyObjectAddCurves(args[0] as ObjectBatchPayload)
const onObjectWatchNodes = (...args: unknown[]): void => applyObjectWatchNodes(args[0] as ObjectBatchPayload)
const onObjectToggleWatch = (...args: unknown[]): void => applyObjectToggleWatch(args[0] as ObjectToggleWatchPayload)
const onObjectJumpSource = (...args: unknown[]): void => applyObjectJumpSource(args[0] as ObjectSourceJumpPayload)
const onObjectSavePreset = (...args: unknown[]): void => applyObjectSavePreset(args[0] as ObjectPresetSavePayload)
const onObjectLoadPreset = (...args: unknown[]): void => applyObjectLoadPreset(args[0] as ObjectPreset)
const onObjectDeletePreset = (...args: unknown[]): void => applyObjectDeletePreset(args[0] as string)

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
    const addLimit = isLocalSource.value ? localSeriesLimit : CURVE_LOCAL_MAX_TOTAL_SERIES
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

// ---- OffscreenCanvas 图表 Worker（task-47 → task-62 P1-15） ------------------
// Worker 内自己维护 ring（ChartRingStore）：主线程在 pushSample 时只发增量小消息
// （time + ids + values），不再每帧把整窗曲线复制/transfer 给 Worker；Y 范围、
// 变换、平滑、像素列 min/max 降采样与光栅都在 Worker 内完成（见 utils/curveChartWorker.ts）。
// 存在派生曲线时 Worker 无派生值可画 → 回退主线程 drawChartLocal（数据功能不受影响）。
let chartWorker: Worker | null = null
let chartWorkerReady = false
let chartWorkerFailed = false
let chartWorkerInFlight = false
let chartWorkerSuspended = false
let chartRequestId = 0
const chartRenderMode = ref<'offscreen' | 'main'>('main')
/** 采样消息计数 + 最近一次 Worker 绘制统计（只用于 __curveDebug 测量，不影响逻辑）。 */
let chartSamplePosts = 0
let chartWorkerEmptyStreak = 0
let chartWorkerStats: {
  renderer: string
  series: number
  decimated: boolean
  segments: number
  columnsDrawn: number
  startOrder: number
  drawMs: number
  uploadBytes?: number
  uploadCalls?: number
  drawCalls?: number
  vertices?: number
  rangeMs?: number
  uploadMode?: string
} | null = null

// ---- task-67：可选 WebGL 曲线渲染（feature flag，默认关闭） -------------------
// 打开方式（显式 opt-in，不做自动开启）：
//   URL `?chartWebgl=1`（本次会话）或 localStorage `wifi-dap.chart.webgl=1|auto|force`；
// 打开后 Worker 只在“大系列（>= CHART_WEBGL_MIN_SERIES）+ 无平滑”时走 WebGL，
// 小系列/不支持/context lost/导出/冻结仍走现有 Canvas/Worker 路径。
// 默认关闭的原因：本机无目标机 GPU，Node/headless 只能证明 CPU 段收益，不能证明
// 真机端到端帧率；详见 docs/web-variable-curve.md §24。
const CHART_WEBGL_STORAGE_KEY = 'wifi-dap.chart.webgl'
let chartWebglMode: ChartWebglMode = 'off'
/** Worker 上报的最近一帧实际渲染后端（webgl2/webgl1/canvas2d）。 */
let chartWebglRenderer: string | null = null
let chartWebglFallback: string | null = null

function readChartWebglMode(): ChartWebglMode {
  let stored: string | null = null
  try {
    stored = window.localStorage.getItem(CHART_WEBGL_STORAGE_KEY)
  } catch { /* localStorage 不可用（隐私模式）→ 仅看 URL */ }
  return resolveChartWebglMode(window.location.search, stored)
}

function initChartWorker(): void {
  chartWebglMode = readChartWebglMode()
  if (typeof Worker === 'undefined' || typeof OffscreenCanvas === 'undefined') {
    chartRenderMode.value = 'main'
    return
  }
  try {
    const worker = new CurveChartWorker()
    chartWorker = worker
    worker.onmessage = (event: MessageEvent<{
      type: 'bitmap' | 'error'
      requestId: number
      bitmap?: ImageBitmap
      drawMs?: number
      error?: string
      geometry?: ChartGeometry
      renderer?: string
      webglFallback?: string | null
      stats?: {
        series?: number; decimated?: number; segments?: number; columnsDrawn?: number
        startOrder?: number; renderer?: string; uploadBytes?: number; uploadCalls?: number
        drawCalls?: number; vertices?: number; rangeMs?: number; uploadMode?: string
      }
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
      // task-67：记录 Worker 实际使用的渲染后端 / WebGL 回退原因（诊断用）。
      chartWebglRenderer = message.renderer ?? message.stats?.renderer ?? 'canvas2d'
      chartWebglFallback = message.webglFallback ?? null
      const geometry = message.geometry
      if (message.stats) {
        chartWorkerStats = {
          renderer: message.stats.renderer ?? chartWebglRenderer,
          series: message.stats.series ?? 0,
          decimated: (message.stats.decimated ?? 0) !== 0,
          segments: message.stats.segments ?? 0,
          columnsDrawn: message.stats.columnsDrawn ?? 0,
          startOrder: message.stats.startOrder ?? 0,
          drawMs: message.drawMs ?? 0,
          uploadBytes: message.stats.uploadBytes,
          uploadCalls: message.stats.uploadCalls,
          drawCalls: message.stats.drawCalls,
          vertices: message.stats.vertices,
          rangeMs: message.stats.rangeMs,
          uploadMode: message.stats.uploadMode,
        }
      }
      if (!geometry) {
        // Worker 无法产出几何（窗口内全部 NaN，或 Worker ring 与主线程不同步）。
        chartWorkerInFlight = false
        if (message.bitmap) message.bitmap.close()
        chartWorkerEmptyStreak += 1
        if (chartWorkerEmptyStreak >= 3) {
          // 连续空几何：判定 Worker ring 失步/退化，永久回退主线程渲染（数据/功能不变）。
          chartWorkerFailed = true
          chartWorkerReady = false
          chartRenderMode.value = 'main'
        }
        // 主线程 ring 是权威数据源：直接主线程画一次（不额外标记 dirty，避免循环）。
        drawChartLocal()
        return
      }
      chartWorkerEmptyStreak = 0
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
    // 主线程 ring 容量固定 MAX_POINTS：Worker 用同一容量保持 order/seq 语义一致。
    worker.postMessage({type: 'reset', maxPoints: MAX_POINTS, webgl: chartWebglMode})
    chartWorkerReady = true
    chartRenderMode.value = 'offscreen'
  } catch {
    chartWorkerFailed = true
    chartWorkerReady = false
    chartRenderMode.value = 'main'
  }
}

/**
 * 把一次 pushSample 的数值增量发给 Worker（不阻塞、失败静默回退主线程）。
 * Worker ring 与主线程 ring 的写入次数/顺序严格一致；payload 为空时也发（seq 对齐）。
 */
function postChartSampleToWorker(time: number, slot: number, snapshot: readonly CurveSeries[]): void {
  const worker = chartWorker
  if (!worker || !chartWorkerReady || chartWorkerFailed) return
  const ids: number[] = []
  const values: number[] = []
  for (const item of snapshot) {
    const seriesBuffer = seriesValues.get(item.id)
    if (!seriesBuffer) continue
    ids.push(item.id)
    values.push(seriesBuffer[slot])
  }
  chartSamplePosts += 1
  worker.postMessage({type: 'samples', time, ids, values})
}

/** Worker ring 复位（clearData 时；与主线程 ring 同一步清空）。 */
function resetChartWorkerRing(): void {
  if (!chartWorker || !chartWorkerReady || chartWorkerFailed) return
  chartWorker.postMessage({type: 'reset', maxPoints: MAX_POINTS, webgl: chartWebglMode})
  chartWorkerStats = null
}

function sendChartToWorker(): boolean {
  const worker = chartWorker
  const canvas = canvasRef.value
  const wrap = chartWrapRef.value
  if (!worker || !chartWorkerReady || chartWorkerFailed || chartWorkerSuspended || !canvas || !wrap) return false
  const visible = visibleSeriesList.value
  const drawn = drawnSeriesList.value
  if (ringCount === 0 || visible.length === 0 || drawn.length === 0) return false
  // 派生曲线的值由主线程推算，Worker ring 里没有 → 含派生曲线时用主线程渲染。
  if (drawn.some((item) => item.derived)) return false
  const width = Math.max(1, Math.floor(wrap.clientWidth))
  const height = Math.max(1, Math.floor(wrap.clientHeight))
  if (width < 40 || height < 40) return false
  const lastTime = lastDataTimestamp()
  const {xMin, xMax} = currentXRange(lastTime)
  if (lowerBoundTime(xMin) >= ringCount) return false
  const series = drawn.map((item) => ({
    id: item.id,
    color: seriesDisplayColor(item.color),
    startSeq: item.startSeq,
  }))
  const palette = chartPalette()
  const scene: ChartDrawMessage = {
    type: 'draw',
    requestId: ++chartRequestId,
    width,
    height,
    dpr: Math.min(2, window.devicePixelRatio || 1),
    padding: {left: 58, right: 14, top: 12, bottom: 26},
    xMin,
    xMax,
    yMode: yAxisMode.value,
    smoothingWindow: smoothingWindow.value,
    palette,
    series,
  }
  if (chartWorkerInFlight) {
    chartDirty = true
    return true
  }
  chartWorkerInFlight = true
  worker.postMessage(scene)
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

  // 数据曲线：共享窗口若点数超过列数则按列 min/max 降采样；A 模式大系列数时
  // 合并到 1/2~1/3 像素列（视觉仍连续），把路径段数从 ~28 万降到 ~9 万/帧。
  const useDecimation = orderCount > plotWidth * 1.5
  const columnScale = drawn.length > 192 ? 12 : drawn.length > 96 ? 3 : 1
  // A 模式大系列数：隔点采样（min/max 包络近似），把每帧变换/路径成本再减半。
  const orderStep = drawn.length > 192 ? 3 : 1
  const plotColumns = Math.max(1, Math.ceil(plotWidth / columnScale))
  if (columnMin.length < plotColumns) {
    columnMin = new Float32Array(plotColumns)
    columnMax = new Float32Array(plotColumns)
  }
  context.lineWidth = 1.5
  const yMode = yAxisMode.value
  // 同色曲线合批：最多 24 种颜色 -> 每帧 ~24 次 stroke，而不是逐曲线 stroke。
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
    chartSamplePosts,
    chartWorkerStats,
    chartWebglMode,
    chartWebglRenderer,
    chartWebglFallback,
    localCapabilities: localCapabilities.value,
    preciseRates: localPreciseActive.value,
    sampleEveryMax: localSampleEveryMax.value,
    startTransport: localStartTransport.value,
    startRequestBytes: localStartRequestBytes.value,
    validSamples: totalValidCount.value,
    staleSamples: totalStaleCount.value,
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
  // task-62 P1-17：先发布空命令索引（本页 ELF 状态是组件内状态；无 ELF 时命令面板不显示变量段）。
  publishCommandIndex()
  window.addEventListener('keydown', onGlobalKeydown)
  // task-62 P1-17：接收命令面板/其他页面的变量聚焦事件。
  window.addEventListener(DAP_FOCUS_VARIABLE_EVENT, onDapFocusVariable as EventListener)
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
  window.removeEventListener(DAP_FOCUS_VARIABLE_EVENT, onDapFocusVariable as EventListener)
  if (focusScrollTimer) { window.clearTimeout(focusScrollTimer); focusScrollTimer = 0 }
  // task-62 P1-17：页面卸载后 ELF 状态不存在了，发布空索引避免命令面板继续显示旧变量。
  {
    const empty = emptyDapCommandIndex()
    const scope = window as unknown as Record<string, unknown>
    scope[DAP_COMMAND_INDEX_WINDOW_KEY] = empty
    commandIndexVersion += 1
    scope[DAP_COMMAND_INDEX_VERSION_KEY] = commandIndexVersion
    try {
      window.dispatchEvent(new CustomEvent(DAP_COMMAND_INDEX_EVENT, {detail: empty}))
    } catch { /* ignore */ }
  }
  stopPlayback()
  if (framesWorker) { framesWorker.terminate(); framesWorker = null }
  if (chartWorker) { chartWorker.terminate(); chartWorker = null }
  themeObserver?.disconnect()
  themeObserver = null
  chartPaletteCache = null
  delete (window as unknown as {__curveDebug?: () => Record<string, unknown>}).__curveDebug
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

.object-view {
  display: flex;
  height: 560px;
  min-height: 360px;
  margin-top: 0.5rem;
}

.object-view > * {
  flex: 1 1 auto;
  min-width: 0;
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
  grid-template-columns: 1.5rem 0.75rem minmax(0, 1.5fr) minmax(0, 1fr) 6.5rem 5.5rem 4.5rem;
  align-items: center;
  gap: 0.375rem;
  font-size: 0.75rem;
  padding: 0.125rem 0;
  color: var(--bb-text, #151a21);
}

/* task-55：本地模式多一列“目标/实际刷新率”（divisor 下拉 + ≈ 实际 Hz）。 */
.selected-row.with-rate {
  grid-template-columns: 1.5rem 0.75rem minmax(0, 1.5fr) minmax(0, 1fr) 6.5rem 6.6rem 5.5rem 4.5rem;
}

.rate-cell {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 0.05rem;
  line-height: 1.15;
}

.rate-cell .rate-select {
  width: 5.8rem;
}

.rate-actual {
  font-size: 0.66rem;
  opacity: 0.7;
  white-space: nowrap;
}

.selected-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 0.05rem;
}

.function-snapshot {
  border-color: var(--bb-border, #dde1e8);
  color: var(--bb-text-secondary, #3d4552);
}

.snapshot-armed {
  color: #d97706;
  font-weight: 600;
}

.snapshot-hit {
  color: #2563eb;
  font-weight: 600;
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

/* task-62：?focusVar= 聚焦的变量行高亮（命令面板跳转） */
.var-row.focus-target {
  outline: 1px solid #0ea5e9;
  outline-offset: -1px;
  background: rgba(14, 165, 233, 0.08);
}
/* task-62：每变量目标 Hz 输入框（窄） */
.rate-cell .rate-hz {
  width: 3.6rem;
}
.rate-cell .rate-hz :deep(.el-input__wrapper) {
  padding-left: 4px;
  padding-right: 4px;
}
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

/* task-58：固件调试数据（UART trace / 帧指针 backtrace）面板 */
.debug-subpanel {
  border-top: 1px solid #eef0f3;
  padding-top: 0.45rem;
}

.debug-subpanel:first-of-type {
  border-top: 0;
  padding-top: 0;
}

.debug-pre {
  max-height: 14rem;
  overflow: auto;
  border-radius: 0.375rem;
  background: #f8fafc;
  padding: 0.5rem;
  font-size: 0.7rem;
  line-height: 1.35;
  white-space: pre-wrap;
  word-break: break-word;
}

.debug-boundaries {
  border-top: 1px dashed #e5e7eb;
  padding-top: 0.45rem;
}

.playback-range { flex: 1; min-width: 8rem; }


@media (max-width: 760px) {
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

/* task-62：trace 行触发命中（固件 triggered/trigger_seq 回读） */
.render-success {
  color: var(--bb-success, #16a34a);
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
