<template>
  <section class="bb-card bb-diag" data-testid="diagnostics-panel">
    <div class="bb-card__head">
      <div>
        <h2 class="bb-card__title text-lg">{{ translate('diag.title') }}</h2>
        <p class="bb-card__hint">{{ translate('diag.subtitle') }}</p>
      </div>
      <div class="bb-diag__actions">
        <el-button size="small" :loading="refreshing" data-testid="diag-refresh" @click="onRefresh">
          {{ refreshing ? translate('diag.refreshing') : translate('diag.refresh') }}
        </el-button>
        <el-button size="small" :loading="kicking" data-testid="diag-kick" @click="onKick">
          {{ translate('diag.kick') }}
        </el-button>
        <el-button size="small" :loading="resetting" data-testid="diag-reset" @click="onReset">
          {{ translate('diag.reset') }}
        </el-button>
        <el-button size="small" @click="onCopy">{{ copied ? translate('common.copied') : translate('diag.copy') }}</el-button>
      </div>
    </div>

    <el-alert
        v-if="!deviceConnected"
        class="mb-3"
        type="info"
        :title="translate('diag.refreshDeviceOffline')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-for="(stageError, index) in stageErrorList"
        :key="index"
        class="mb-2"
        type="warning"
        :title="stageError"
        :closable="false"
        show-icon
    />

    <div class="bb-diag__grid">
      <article class="bb-diag__card">
        <h3>{{ translate('diag.dap') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('dashboard.wsConnection') }}</dt>
            <dd>{{ wsStateText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wsHost') }}</dt>
            <dd class="bb-mono">{{ deviceHost }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.dapSession') }}</dt>
            <dd>{{ dapSessionText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.lastKick') }}</dt>
            <dd data-testid="diag-kick-result">{{ kickText }}</dd>
          </div>
          <div v-if="dapLatencyText">
            <dt>{{ translate('dashboard.dapLatency') }}</dt>
            <dd data-testid="diag-dap-latency">{{ dapLatencyText }}</dd>
          </div>
        </dl>
        <p class="bb-muted text-xs bb-diag__note">{{ translate('diag.unsupportedHint') }}</p>
      </article>

      <article class="bb-diag__card">
        <h3>{{ translate('diag.swd') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('diag.lastProbe') }}</dt>
            <dd data-testid="diag-probe">{{ probeText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.swdClock') }}</dt>
            <dd>{{ swdClockText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.statusState') }}</dt>
            <dd>{{ targetStatusText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.statusBusy') }}</dt>
            <dd>{{ statusBusyText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.targetError') }}</dt>
            <dd>{{ targetErrorText }}</dd>
          </div>
          <div v-if="statusFailuresText">
            <dt>{{ translate('dashboard.statusEndpoint') }}</dt>
            <dd data-testid="diag-status-endpoint">{{ statusFailuresText }}</dd>
          </div>
        </dl>
      </article>

      <article class="bb-diag__card">
        <h3>{{ translate('diag.wifi') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('diag.wifiState') }}</dt>
            <dd>{{ wifiStateText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiSsid') }}</dt>
            <dd>{{ wifiSsidText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiIp') }}</dt>
            <dd class="bb-mono">{{ wifiIpText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.wifiError') }}</dt>
            <dd>{{ wifiErrorText }}</dd>
          </div>
        </dl>
      </article>

      <article class="bb-diag__card">
        <h3>{{ translate('diag.target') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('target.family') }}</dt>
            <dd>{{ targetFamilyText }}</dd>
          </div>
          <div>
            <dt>DEV_ID</dt>
            <dd class="bb-mono">{{ targetDevIdText }}</dd>
          </div>
          <div>
            <dt>{{ translate('target.flashSize') }}</dt>
            <dd>{{ targetFlashText }}</dd>
          </div>
          <div>
            <dt>{{ translate('target.uid') }}</dt>
            <dd class="bb-mono bb-diag__uid">{{ targetUidText }}</dd>
          </div>
        </dl>
      </article>

      <article class="bb-diag__card">
        <h3>{{ translate('diag.heap') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('diag.heapFree') }}</dt>
            <dd class="bb-mono">{{ formatBytes(heapFree) }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.heapLargest') }}</dt>
            <dd class="bb-mono">{{ formatBytes(heapLargest) }}</dd>
          </div>
        </dl>
      </article>

      <article class="bb-diag__card">
        <h3>{{ translate('diag.protocol') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('protocol.version') }}</dt>
            <dd>{{ protocolText }}</dd>
          </div>
          <div>
            <dt>{{ translate('diag.link') }}</dt>
            <dd>{{ linkText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.linkHostname') }}</dt>
            <dd class="bb-mono">{{ linkInfo.hostname || translate('common.unknown') }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.rttTitle') }}</dt>
            <dd data-testid="diag-rtt">{{ rttText }}</dd>
          </div>
        </dl>
      </article>

      <article class="bb-diag__card" data-testid="diag-ncm">
        <h3>{{ translate('dashboard.linkWired') }}</h3>
        <dl class="bb-diag__kv">
          <div>
            <dt>{{ translate('dashboard.ncmState') }}</dt>
            <dd>{{ ncmStateText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.dhcpState') }}</dt>
            <dd>{{ dhcpStateText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wiredDeviceIp') }}</dt>
            <dd class="bb-mono">{{ wiredIpText }}</dd>
          </div>
        </dl>
        <p class="bb-muted text-xs bb-diag__note">{{ ncmHintText }}</p>
      </article>
    </div>

    <div class="bb-diag__errors" data-testid="diag-errors">
      <h3 class="text-sm font-semibold">{{ translate('diag.errors') }}</h3>
      <ul v-if="errors.length">
        <li v-for="(item, index) in errors" :key="index">{{ item }}</li>
      </ul>
      <p v-else class="bb-muted text-xs">{{ translate('diag.noErrors') }}</p>
    </div>

    <details class="bb-diag__json">
      <summary class="text-sm cursor-pointer">{{ translate('diag.jsonTitle') }}</summary>
      <pre class="bb-diag__pre">{{ diagnosticsJson }}</pre>
    </details>
  </section>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { ElMessage } from 'element-plus';
import { translate } from '@/locales';
import { ControlEvent, PROTOCOL_VERSION } from '@/api';
import { kickDapSession } from '@/api/apiDap';
import { resetTarget } from '@/api/apiTarget';
import {
  formatDevId,
  formatFlashKib,
  parseTargetDetection,
} from '@/utils/targetProfile';
import { useWsStore } from '@/stores/websocket';
import { useSystemStore } from '@/stores/useSystemStore';
import { globalNotify } from '@/composables/notification';
import { copyTextToClipboard } from '@/components/dashboard/clipboard';
import {
  buildDiagnosticsJson,
  dashboardState,
  getLinkInfo,
  probeTargetForDiagnostics,
  recordDapKick,
  refreshTargetStatus,
  refreshWifi,
} from '@/components/dashboard/dashboardState';

const wsStore = useWsStore();
const systemStore = useSystemStore();
const linkInfo = getLinkInfo();

const refreshing = ref(false);
const kicking = ref(false);
const resetting = ref(false);
const copied = ref(false);
const stageErrors = ref<string[]>([]);

const deviceConnected = computed(() => wsStore.state === ControlEvent.CONNECTED);
const deviceHost = computed(() => {
  if (import.meta.env.VITE_APP_MODE === 'dev') {
    return import.meta.env.VITE_DEVICE_HOST_NAME || 'dap.local';
  }
  return window.location.host;
});

const wsStateText = computed(() => {
  switch (wsStore.state) {
    case ControlEvent.CONNECTED:
      return translate('ws.connected');
    case ControlEvent.CONNECTING:
    case ControlEvent.LOADED:
      return translate('ws.connecting');
    default:
      return translate('ws.disconnected');
  }
});

const detected = computed(() => {
  const info = dashboardState.target.info;
  if (!info?.detected) {
    return null;
  }
  const parsed = parseTargetDetection(info);
  return parsed.detected ? parsed : null;
});

const dapSessionText = computed(() => {
  const detail = dashboardState.target.detail;
  if (typeof detail.dapSessionActive === 'boolean') {
    return detail.dapSessionActive
      ? translate('dashboard.dapSessionActive')
      : translate('dashboard.dapSessionIdle');
  }
  if (dashboardState.target.statusKnown && typeof dashboardState.target.status?.dapSessionActive === 'boolean') {
    return dashboardState.target.status.dapSessionActive
      ? translate('dashboard.dapSessionActive')
      : translate('dashboard.dapSessionIdle');
  }
  return translate('diag.sessionUnknown');
});

const dapLatencyText = computed(() => {
  const detail = dashboardState.target.detail;
  if (detail.dapLatencyAvgUs === null && detail.dapLatencyMaxUs === null) {
    return '';
  }
  return translate('dashboard.dapLatencyValue', {
    avg: detail.dapLatencyAvgUs === null ? '?' : detail.dapLatencyAvgUs,
    max: detail.dapLatencyMaxUs === null ? '?' : detail.dapLatencyMaxUs,
  });
});

const detail = computed(() => dashboardState.target.detail);
const heapFree = computed(() => detail.value.heapFree);
const heapLargest = computed(() => detail.value.heapLargest);
const swdClockText = computed(() => {
  const hz = detail.value.swdClockHz ?? dashboardState.target.info?.swd_clock_hz;
  return typeof hz === 'number' && hz > 0
    ? translate('dashboard.swdActualValue', { khz: Math.round(hz / 1000) })
    : translate('common.unknown');
});

const statusFailuresText = computed(() => {
  const current = detail.value;
  if (current.failures > 0 && current.error) {
    return translate('dashboard.statusRequestFailed', { err: current.error });
  }
  if (!current.available) {
    return current.httpStatus === 404
      ? translate('dashboard.statusUnsupported')
      : translate('common.unknown');
  }
  return translate('dashboard.statusSupported', { status: current.httpStatus });
});

const rttText = computed(() => {
  const link = dashboardState.link;
  if (link.last === null) {
    return translate('dashboard.rttNone');
  }
  const parts = [translate('dashboard.rttValue', { last: link.last })];
  if (link.avg !== null && link.min !== null && link.max !== null) {
    parts.push(translate('dashboard.rttStats', { avg: link.avg, min: link.min, max: link.max }));
  }
  return parts.join(' · ');
});

const ncmKnown = computed(() => detail.value.wiredNcmActive !== null
    || detail.value.wiredDhcpActive !== null || detail.value.wiredDhcpState !== null
    || detail.value.wiredIp !== '');
const ncmStateText = computed(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmNotReported');
  }
  if (detail.value.wiredNcmActive === true) {
    return translate('dashboard.ncmActive');
  }
  if (detail.value.wiredNcmActive === false) {
    return translate('dashboard.ncmInactive');
  }
  return translate('common.unknown');
});
const dhcpStateText = computed(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmNotReported');
  }
  const error = detail.value.wiredDhcpError;
  if (error !== null && error !== 0) {
    return translate('dashboard.dhcpError', { code: error });
  }
  if (detail.value.wiredDhcpActive === true) {
    return translate('dashboard.dhcpStateStarted');
  }
  if (detail.value.wiredDhcpActive === false) {
    return translate('dashboard.dhcpStateStopped');
  }
  switch (detail.value.wiredDhcpState) {
    case 1:
      return translate('dashboard.dhcpStateStarted');
    case 0:
      return translate('dashboard.dhcpStateInit');
    case 2:
      return translate('dashboard.dhcpStateStopped');
    default:
      return translate('common.unknown');
  }
});
const wiredIpText = computed(() => detail.value.wiredIp || translate('common.unknown'));
const ncmHintText = computed(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmHintUnavailable');
  }
  const ready = detail.value.wiredNcmActive === true && detail.value.wiredDhcpActive === true;
  return ready ? translate('dashboard.ncmHintOk') : translate('dashboard.ncmHintReplug');
});

function formatBytes(value?: number | null): string {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    return translate('common.unknown');
  }
  if (value >= 1024 * 1024) {
    return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
  }
  if (value >= 1024) {
    return `${(value / 1024).toFixed(1)} KiB`;
  }
  return `${Math.round(value)} B`;
}

const kickText = computed(() => {
  const kick = dashboardState.dap.lastKick;
  if (!kick) {
    return translate('diag.kickNone');
  }
  if (!kick.supported) {
    return translate('diag.kickUnsupported');
  }
  if (kick.ok) {
    return translate('diag.kickOk');
  }
  return translate('diag.kickFailed', { err: kick.err || `HTTP ${kick.httpStatus}` });
});

const probeText = computed(() => {
  const target = dashboardState.target;
  if (!target.probed) {
    return translate('diag.probeNone');
  }
  if (target.info?.detected) {
    return translate('diag.probeOk');
  }
  const err = target.lastError || `HTTP ${target.httpStatus ?? '?'}`;
  return translate('diag.probeFailed', { err });
});

const targetStatusText = computed(() => {
  const target = dashboardState.target;
  if (!target.statusKnown || !target.status) {
    return translate('diag.statusUnavailable');
  }
  const state = target.status.state || translate('common.unknown');
  const percent = typeof target.status.percent === 'number' ? ` (${target.status.percent.toFixed(1)} %)` : '';
  return `${state}${percent}`;
});

const statusBusyText = computed(() => {
  const target = dashboardState.target;
  if (!target.statusKnown || !target.status) {
    return translate('common.unknown');
  }
  const percent = typeof target.status.offset === 'number' && typeof target.status.total === 'number' && target.status.total > 0
    ? ` (${((target.status.offset / target.status.total) * 100).toFixed(1)} %)`
    : '';
  return `${target.status.state ?? translate('common.unknown')}${percent}`;
});

const targetErrorText = computed(() => {
  const target = dashboardState.target;
  const parts = [
    target.lastError,
    target.statusError,
    target.status?.err ?? '',
  ].filter((part) => part !== '');
  return parts.length ? parts.join(' · ') : translate('diag.noErrors');
});

const wifiStateText = computed(() => {
  const wifi = dashboardState.wifi;
  if (wifi.staConnected === true) {
    return translate('ws.connected');
  }
  if (wifi.staKnown) {
    return translate('ws.disconnected');
  }
  return translate('common.unknown');
});
const wifiSsidText = computed(() => (
  dashboardState.wifi.staConnected === true
    ? dashboardState.wifi.ssid || translate('common.unknown')
    : translate('common.unknown')
));
const wifiIpText = computed(() => (
  dashboardState.wifi.staConnected === true
    ? dashboardState.wifi.ip || translate('common.unknown')
    : translate('common.unknown')
));
const wifiErrorText = computed(() => dashboardState.wifi.lastError || translate('diag.noErrors'));

const targetFamilyText = computed(() => {
  const info = dashboardState.target.info;
  return info?.series || info?.family || detected.value?.baseGroup || translate('common.unknown');
});
const targetDevIdText = computed(() => {
  const devId = detected.value?.devId;
  return typeof devId === 'number' && devId > 0 ? formatDevId(devId) : translate('common.unknown');
});
const targetFlashText = computed(() => {
  const text = formatFlashKib(detected.value?.flashKib);
  return text === '-' ? translate('common.unknown') : text;
});
const targetUidText = computed(() => {
  const raw = String(dashboardState.target.info?.uid ?? '')
      .replace(/^0x/i, '')
      .replace(/[^0-9a-f]/gi, '')
      .toUpperCase();
  if (!raw) {
    return translate('common.unknown');
  }
  if (raw.length !== 24) {
    return `0x${raw}`;
  }
  return [raw.slice(0, 8), raw.slice(8, 16), raw.slice(16, 24)]
      .map((word, index) => `UID${index}: 0x${word}`)
      .join(' · ');
});

const protocolText = computed(() => {
  const fw = systemStore.fwProtocolVersion === null ? '?' : `v${systemStore.fwProtocolVersion}`;
  return `${fw} / v${PROTOCOL_VERSION}`;
});

const linkText = computed(() => {
  switch (linkInfo.kind) {
    case 'wired':
      return translate('dashboard.linkWired');
    case 'wifi':
      return translate('dashboard.linkWifi');
    default:
      return translate('dashboard.linkUnknown');
  }
});

const errors = computed<string[]>(() => {
  const list: string[] = [];
  if (dashboardState.target.lastError) {
    list.push(`${translate('diag.targetError')}: ${dashboardState.target.lastError}`);
  }
  if (dashboardState.target.detail.error) {
    list.push(`${translate('dashboard.statusEndpoint')}: ${dashboardState.target.detail.error}`);
  }
  if (dashboardState.target.detail.lastFlashError) {
    list.push(`${translate('diag.targetError')}: ${dashboardState.target.detail.lastFlashError}`);
  }
  if (dashboardState.wifi.lastError) {
    list.push(`${translate('diag.wifiError')}: ${dashboardState.wifi.lastError}`);
  }
  if (dashboardState.dap.lastKickError) {
    list.push(translate('diag.kickFailed', { err: dashboardState.dap.lastKickError }));
  }
  if (systemStore.protocolState === 'mismatch') {
    list.push(translate('protocol.mismatchTitle'));
  } else if (systemStore.protocolState === 'legacy') {
    list.push(translate('protocol.mismatchLegacy'));
  }
  return list;
});

const stageErrorList = computed(() => stageErrors.value);

const diagnosticsJson = computed(() => buildDiagnosticsJson());

async function onRefresh(): Promise<void> {
  if (refreshing.value) {
    return;
  }
  refreshing.value = true;
  stageErrors.value = [];
  try {
    const probe = await probeTargetForDiagnostics();
    if (!probe.ok) {
      stageErrors.value.push(translate('diag.refreshProbeFailed', { err: probe.err }));
    }
    await refreshTargetStatus({ force: true });
    refreshWifi();
    if (!deviceConnected.value) {
      stageErrors.value.push(translate('diag.refreshDeviceOffline'));
    }
    if (stageErrors.value.length === 0) {
      globalNotify(translate('diag.refreshOk'), 'success');
    } else {
      globalNotify(translate('diag.refreshProbeFailed', { err: stageErrors.value[0] }), 'warning');
    }
  } finally {
    refreshing.value = false;
  }
}

async function onKick(): Promise<void> {
  if (kicking.value) {
    return;
  }
  kicking.value = true;
  try {
    const result = await kickDapSession();
    recordDapKick(result);
    if (result.ok) {
      globalNotify(translate('diag.kickOk'), 'success');
    } else if (!result.supported) {
      globalNotify(translate('diag.kickUnsupported'), 'warning');
    } else {
      globalNotify(translate('diag.kickFailed', { err: result.err || `HTTP ${result.httpStatus}` }), 'error');
    }
  } finally {
    kicking.value = false;
  }
}

async function onReset(): Promise<void> {
  if (resetting.value) {
    return;
  }
  resetting.value = true;
  try {
    const response = await resetTarget(true);
    if (response.ok) {
      globalNotify(translate('diag.resetOk'), 'success');
    } else {
      globalNotify(translate('diag.resetFailed', { err: response.err || `HTTP ${response.httpStatus}` }), 'error');
    }
  } finally {
    resetting.value = false;
  }
}

async function onCopy(): Promise<void> {
  const ok = await copyTextToClipboard(buildDiagnosticsJson());
  if (!ok) {
    ElMessage.warning(translate('diag.copyFailed'));
    return;
  }
  copied.value = true;
  window.setTimeout(() => {
    copied.value = false;
  }, 2000);
  globalNotify(translate('common.copied'), 'success');
}
</script>

<style scoped>
.bb-diag__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--bb-space-2);
}

.bb-diag__grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: var(--bb-space-3);
}

.bb-diag__card {
  min-width: 0;
  padding: var(--bb-space-3);
  border: 1px solid var(--bb-border-subtle);
  border-radius: var(--bb-radius-md);
  background: var(--bb-surface-inset);
}

.bb-diag__card h3 {
  margin: 0 0 var(--bb-space-2);
  font-size: var(--bb-font-size-md);
  font-weight: 650;
  color: var(--bb-text);
}

.bb-diag__kv {
  display: grid;
  gap: 6px;
  margin: 0;
}

.bb-diag__kv > div {
  display: grid;
  grid-template-columns: minmax(88px, 34%) 1fr;
  gap: var(--bb-space-2);
  align-items: baseline;
}

.bb-diag__kv dt {
  margin: 0;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-muted);
}

.bb-diag__kv dd {
  margin: 0;
  min-width: 0;
  overflow-wrap: anywhere;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-secondary);
}

.bb-diag__uid {
  font-size: var(--bb-font-size-xs);
}

.bb-diag__note {
  margin: var(--bb-space-2) 0 0;
  line-height: 1.5;
}

.bb-diag__errors {
  margin-top: var(--bb-space-4);
  padding-top: var(--bb-space-3);
  border-top: 1px solid var(--bb-border-subtle);
}

.bb-diag__errors ul {
  margin: var(--bb-space-1) 0 0;
  padding-left: 18px;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-warning);
}

.bb-diag__json {
  margin-top: var(--bb-space-3);
}

.bb-diag__pre {
  margin: var(--bb-space-2) 0 0;
  max-height: 320px;
  overflow: auto;
  padding: var(--bb-space-3);
  border-radius: var(--bb-radius-sm);
  border: 1px solid var(--bb-border-subtle);
  background: var(--bb-surface-inset);
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-secondary);
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

@media (max-width: 639px) {
  .bb-diag__actions {
    justify-content: flex-start;
  }

  .bb-diag__kv > div {
    grid-template-columns: 1fr;
    gap: 0;
  }
}
</style>
