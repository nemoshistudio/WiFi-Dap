<template>
  <section v-if="visible" class="bb-card bb-guide" data-testid="guided-flow">
    <div class="bb-card__head">
      <div>
        <h2 class="bb-card__title text-lg">{{ translate('guided.title') }}</h2>
        <p class="bb-card__hint">{{ translate('guided.subtitle') }}</p>
      </div>
      <div class="bb-guide__head-actions">
        <span class="bb-guide__progress-text">{{ translate('guided.progress', { count: doneCount, total: steps.length }) }}</span>
        <el-button size="small" data-testid="guided-reset" @click="onReset">{{ translate('guided.reset') }}</el-button>
        <el-button size="small" data-testid="guided-skip" @click="onSkip">{{ translate('guided.skip') }}</el-button>
      </div>
    </div>

    <el-progress
        class="bb-guide__bar"
        :percentage="percent"
        :show-text="false"
        :stroke-width="8"
        striped
        striped-flow
    />

    <el-alert
        v-if="!connected"
        class="mt-2"
        type="info"
        :title="translate('guided.connectionRequired')"
        :closable="false"
        show-icon
    />

    <ol class="bb-guide__steps">
      <li
          v-for="(step, index) in steps"
          :key="step.id"
          class="bb-guide__step"
          :class="{
            'is-done': step.done,
            'is-current': !step.done && index === currentIndex,
          }"
          :data-testid="`guided-step-${step.id}`"
      >
        <span class="bb-guide__marker" aria-hidden="true">{{ step.done ? '✓' : index + 1 }}</span>
        <div class="bb-guide__body">
          <div class="bb-guide__title-row">
            <h3>{{ step.title }}</h3>
            <el-tag v-if="step.done && step.auto" size="small" type="success" data-testid="guided-auto-tag">
              {{ translate('guided.autoDetected') }}
            </el-tag>
            <el-tag v-else-if="step.done && step.manual" size="small" type="success" data-testid="guided-manual-tag">
              {{ translate('guided.manualConfirmed') }}
            </el-tag>
            <el-tag v-else-if="step.done" size="small" type="success">{{ translate('guided.stateDone') }}</el-tag>
            <el-tag v-else-if="index === currentIndex" size="small" type="warning">{{ translate('guided.current') }}</el-tag>
            <el-tag v-else size="small" type="info">{{ translate('guided.stateTodo') }}</el-tag>
          </div>
          <p class="bb-guide__hint">{{ step.hint }}</p>
          <p v-if="step.autoHint" class="bb-guide__auto-hint" data-testid="guided-auto-hint">{{ step.autoHint }}</p>
          <div v-if="step.id === 'detect'" class="bb-guide__actions">
            <el-button
                type="primary"
                size="small"
                :loading="detecting"
                :disabled="!connected || flashing"
                data-testid="guided-detect"
                @click="emit('detect')"
            >
              {{ detecting ? translate('target.detecting') : translate('target.detect') }}
            </el-button>
          </div>
          <div v-else-if="step.id === 'elf'" class="bb-guide__actions">
            <el-button size="small" data-testid="guided-open-curve-elf" @click="openCurve">
              {{ translate('guided.openCurve') }}
            </el-button>
            <template v-if="!step.done">
              <el-button size="small" data-testid="guided-manual-elf" @click="confirmManual('elf')">
                {{ translate('guided.manualConfirm') }}
              </el-button>
              <span class="bb-muted text-xs">{{ translate('guided.manualHint') }}</span>
            </template>
          </div>
          <div v-else-if="step.id === 'sample'" class="bb-guide__actions">
            <el-button size="small" data-testid="guided-open-curve-sample" @click="openCurve">
              {{ translate('guided.openCurve') }}
            </el-button>
            <template v-if="!step.done">
              <el-button size="small" data-testid="guided-manual-sample" @click="confirmManual('sample')">
                {{ translate('guided.manualConfirm') }}
              </el-button>
              <span class="bb-muted text-xs">{{ translate('guided.manualHint') }}</span>
            </template>
          </div>
          <div v-else-if="step.id === 'flash'" class="bb-guide__actions">
            <el-button
                type="primary"
                size="small"
                :disabled="flashing"
                data-testid="guided-goto-flash"
                @click="emit('goto-flash')"
            >
              {{ translate('guided.gotoFlash') }}
            </el-button>
          </div>
        </div>
      </li>
    </ol>

    <el-alert
        v-if="allDone"
        class="mt-2"
        type="success"
        :title="translate('guided.allDone')"
        :closable="false"
        show-icon
    />
    <p class="bb-muted text-xs bb-guide__note">{{ translate('guided.notAuto') }}</p>
  </section>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { translate } from '@/locales';
import { globalNotify } from '@/composables/notification';
import { onAction } from '@/components/dashboard/actionBus';
import { dashboardState } from '@/components/dashboard/dashboardState';
import {
  guidedSignalState,
  resetAllGuidedSignals,
} from '@/components/dashboard/guidedSignals';

/**
 * localStorage contract (per browser, survives reloads). v2 adds the explicit
 * manual confirmations; v1 is still read for backward compatibility.
 * Auto-observed progress is session state and is NOT persisted (stale "done"
 * steps would be misleading after a device reconnect / page restart).
 */
const STORAGE_KEY = 'birdiebot.guidedFlow.v2';
const LEGACY_STORAGE_KEY = 'birdiebot.guidedFlow.v1';

interface PersistedState {
  /** User skipped/closed the guide: do not auto-open again. */
  dismissed: boolean;
  /** Explicit manual confirmations for steps the UI cannot observe. */
  manualElf: boolean;
  manualSample: boolean;
}

function loadState(): PersistedState {
  const fallback: PersistedState = { dismissed: false, manualElf: false, manualSample: false };
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      const parsed = JSON.parse(raw) as Partial<PersistedState>;
      return {
        dismissed: parsed.dismissed === true,
        manualElf: parsed.manualElf === true,
        manualSample: parsed.manualSample === true,
      };
    }
    const legacy = localStorage.getItem(LEGACY_STORAGE_KEY);
    if (legacy) {
      const parsed = JSON.parse(legacy) as { dismissed?: boolean; elfDone?: boolean; sampleDone?: boolean };
      return {
        dismissed: parsed.dismissed === true,
        manualElf: parsed.elfDone === true,
        manualSample: parsed.sampleDone === true,
      };
    }
    return fallback;
  } catch {
    return fallback;
  }
}

function saveState(state: PersistedState): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
  } catch {
    // Private mode: guide still works for this session.
  }
}

interface GuidedStep {
  id: 'connect' | 'detect' | 'elf' | 'sample' | 'flash';
  title: string;
  hint: string;
  done: boolean;
  /** Completed via a real observed event (not a manual confirmation). */
  auto: boolean;
  /** Completed via the explicit "manual confirm" button. */
  manual: boolean;
  /** Optional explanation of what real event completed the step. */
  autoHint: string;
}

const props = defineProps<{
  connected: boolean;
  detected: boolean;
  detecting: boolean;
  flashing: boolean;
  flashed: boolean;
}>();

const emit = defineEmits<{
  (e: 'detect'): void;
  (e: 'goto-flash'): void;
}>();

const router = useRouter();
const persisted = reactive<PersistedState>(loadState());
const visible = ref(!persisted.dismissed);
const manualElf = ref(persisted.manualElf);
const manualSample = ref(persisted.manualSample);

function persist(): void {
  persisted.manualElf = manualElf.value;
  persisted.manualSample = manualSample.value;
  saveState(persisted);
}

watch(manualElf, persist);
watch(manualSample, persist);

/* Real events (P1-18): detection / ELF index / sampling / flash success. */
const elfLoaded = computed(() => guidedSignalState.lastAt['elf-loaded'] !== null);
const samplingStarted = computed(() => guidedSignalState.lastAt['sampling-started'] !== null);
const detectionSignal = computed(() => guidedSignalState.lastAt['target-detected'] !== null);
const flashSignal = computed(() => guidedSignalState.lastAt['flash-success'] !== null);

const detectedDone = computed(() => props.detected
    || dashboardState.target.info?.detected === true
    || detectionSignal.value);
const flashedDone = computed(() => props.flashed
    || dashboardState.target.detail.state === 'done'
    || flashSignal.value);

const steps = computed<GuidedStep[]>(() => [
  {
    id: 'connect',
    title: translate('guided.stepConnect'),
    hint: translate('guided.stepConnectHint'),
    done: props.connected,
    auto: props.connected,
    manual: false,
    autoHint: '',
  },
  {
    id: 'detect',
    title: translate('guided.stepDetect'),
    hint: translate('guided.stepDetectHint'),
    done: detectedDone.value,
    auto: detectedDone.value,
    manual: false,
    autoHint: '',
  },
  {
    id: 'elf',
    title: translate('guided.stepElf'),
    hint: translate('guided.stepElfHint'),
    done: elfLoaded.value || manualElf.value,
    auto: elfLoaded.value,
    manual: !elfLoaded.value && manualElf.value,
    autoHint: elfLoaded.value ? translate('guided.elfAutoHint') : '',
  },
  {
    id: 'sample',
    title: translate('guided.stepSample'),
    hint: translate('guided.stepSampleHint'),
    done: samplingStarted.value || manualSample.value,
    auto: samplingStarted.value,
    manual: !samplingStarted.value && manualSample.value,
    autoHint: samplingStarted.value ? translate('guided.sampleAutoHint') : '',
  },
  {
    id: 'flash',
    title: translate('guided.stepFlash'),
    hint: translate('guided.stepFlashHint'),
    done: flashedDone.value,
    auto: flashedDone.value,
    manual: false,
    autoHint: '',
  },
]);

const doneCount = computed(() => steps.value.filter((step) => step.done).length);
const percent = computed(() => Math.round((doneCount.value / steps.value.length) * 100));
const currentIndex = computed(() => steps.value.findIndex((step) => !step.done));
const allDone = computed(() => doneCount.value === steps.value.length);

function openCurve(): void {
  void router.push('/debug-curve');
}

function confirmManual(step: 'elf' | 'sample'): void {
  if (step === 'elf') {
    manualElf.value = true;
  } else {
    manualSample.value = true;
  }
  persist();
  globalNotify(translate('guided.manualConfirmed'), 'success');
}

function onSkip(): void {
  persisted.dismissed = true;
  saveState(persisted);
  visible.value = false;
  globalNotify(translate('guided.closedHint'), 'info');
}

function onReset(): void {
  persisted.dismissed = false;
  manualElf.value = false;
  manualSample.value = false;
  saveState(persisted);
  // Real signals are also cleared so the guide re-evaluates observed state.
  resetAllGuidedSignals();
  visible.value = true;
  globalNotify(translate('guided.resetDone'), 'info');
}

/** Reopen from the dashboard or the command palette (explicit user action). */
function open(): void {
  persisted.dismissed = false;
  saveState(persisted);
  visible.value = true;
}

defineExpose({ open });

let offAction: (() => void) | null = null;
onMounted(() => {
  offAction = onAction((action) => {
    if (action === 'guided-open') {
      open();
    }
  });
});
onUnmounted(() => {
  offAction?.();
  offAction = null;
});
</script>

<style scoped>
.bb-guide__head-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
}

.bb-guide__progress-text {
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-muted);
}

.bb-guide__bar {
  margin-bottom: var(--bb-space-2);
}

.bb-guide__steps {
  margin: var(--bb-space-3) 0 0;
  padding: 0;
  list-style: none;
  display: grid;
  gap: var(--bb-space-2);
}

.bb-guide__step {
  display: flex;
  gap: var(--bb-space-3);
  padding: var(--bb-space-3);
  border: 1px solid var(--bb-border-subtle);
  border-radius: var(--bb-radius-md);
  background: var(--bb-surface-inset);
}

.bb-guide__step.is-current {
  border-color: rgba(251, 191, 36, 0.45);
  background: var(--bb-warning-soft);
}

.bb-guide__step.is-done {
  border-color: rgba(61, 220, 132, 0.35);
}

.bb-guide__marker {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  border: 1px solid var(--bb-border-strong);
  background: var(--bb-surface);
  color: var(--bb-text-secondary);
  font-size: var(--bb-font-size-sm);
  font-weight: 650;
}

.bb-guide__step.is-done .bb-guide__marker {
  border-color: var(--bb-success);
  background: var(--bb-success-soft);
  color: var(--bb-success);
}

.bb-guide__step.is-current .bb-guide__marker {
  border-color: var(--bb-warning);
  color: var(--bb-warning);
}

.bb-guide__body {
  min-width: 0;
  flex: 1 1 auto;
}

.bb-guide__title-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
}

.bb-guide__title-row h3 {
  margin: 0;
  font-size: var(--bb-font-size-md);
  font-weight: 600;
  color: var(--bb-text);
}

.bb-guide__hint {
  margin: 2px 0 0;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-muted);
  line-height: 1.5;
}

.bb-guide__auto-hint {
  margin: 2px 0 0;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-success);
}

.bb-guide__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-3);
  margin-top: var(--bb-space-2);
}

.bb-guide__note {
  margin: var(--bb-space-3) 0 0;
}

@media (max-width: 639px) {
  .bb-guide__head-actions {
    justify-content: flex-start;
  }
}
</style>
