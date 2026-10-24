<template>
  <article class="bb-dash__card bb-health" data-testid="dashboard-health">
    <header class="bb-dash__card-head">
      <h3>{{ translate('dashboard.healthTitle') }}</h3>
      <div class="bb-health__head-actions">
        <el-tag size="small" :type="healthTagType" data-testid="dashboard-health-state">{{ healthText }}</el-tag>
        <el-button
            size="small"
            :loading="refreshing"
            data-testid="dashboard-health-refresh"
            @click="onRefresh"
        >
          {{ translate('dashboard.healthRefresh') }}
        </el-button>
      </div>
    </header>

    <el-alert
        v-if="healthError"
        class="mb-2"
        type="warning"
        :title="healthError"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else-if="!deviceConnected"
        class="mb-2"
        type="info"
        :title="translate('dashboard.deviceOfflineHint')"
        :closable="false"
        show-icon
    />

    <dl class="bb-dash__kv">
      <div>
        <dt>{{ translate('dashboard.wifiRssi') }}</dt>
        <dd data-testid="dashboard-health-rssi">
          <template v-if="wifiRssiText">
            <span class="bb-mono">{{ wifiRssiText }}</span>
            <el-tag v-if="wifiQualityText" class="ml-2" size="small" :type="wifiQualityTagType">{{ wifiQualityText }}</el-tag>
          </template>
          <span v-else class="bb-muted">{{ unknownText }}</span>
        </dd>
      </div>
      <div class="bb-dash__kv-wide">
        <dt>{{ translate('dashboard.rttTitle') }}</dt>
        <dd data-testid="dashboard-health-rtt">
          <template v-if="rttLast !== null">
            <span class="bb-mono">{{ translate('dashboard.rttValue', { last: rttLast }) }}</span>
            <span class="bb-health__sub" data-testid="dashboard-health-rtt-stats">{{ rttStatsText }}</span>
          </template>
          <span v-else class="bb-muted">{{ translate('dashboard.rttNone') }}</span>
        </dd>
      </div>
      <div class="bb-dash__kv-wide">
        <dt>{{ translate('dashboard.statusEndpoint') }}</dt>
        <dd data-testid="dashboard-health-endpoint">{{ statusEndpointText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.dapSession') }}</dt>
        <dd data-testid="dashboard-health-dap-session">{{ dapSessionText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.swdActual') }}</dt>
        <dd data-testid="dashboard-health-swd-clock">{{ swdClockText }}</dd>
      </div>
      <div>
        <dt>{{ translate('target.swdClock') }}</dt>
        <dd data-testid="dashboard-health-swd-manual">{{ swdManualText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.dapRing') }}</dt>
        <dd data-testid="dashboard-health-dap-ring">{{ dapRingText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.heapFreeShort') }}</dt>
        <dd data-testid="dashboard-health-heap-free">{{ heapFreeText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.heapLargestShort') }}</dt>
        <dd data-testid="dashboard-health-heap-largest">{{ heapLargestText }}</dd>
      </div>
      <div v-if="dapLatencyText">
        <dt>{{ translate('dashboard.dapLatency') }}</dt>
        <dd data-testid="dashboard-health-dap-latency">{{ dapLatencyText }}</dd>
      </div>
      <div v-if="dapXferCountText">
        <dt>{{ translate('dashboard.dapXferCount') }}</dt>
        <dd class="bb-mono" data-testid="dashboard-health-dap-xfer-count">{{ dapXferCountText }}</dd>
      </div>
      <div v-if="dapXferLastText">
        <dt>{{ translate('dashboard.dapXferLast') }}</dt>
        <dd class="bb-mono" data-testid="dashboard-health-dap-xfer-last">{{ dapXferLastText }}</dd>
      </div>
      <div v-if="wifiChannelText">
        <dt>{{ translate('dashboard.wifiChannel') }}</dt>
        <dd class="bb-mono" data-testid="dashboard-health-wifi-channel">{{ wifiChannelText }}</dd>
      </div>
      <div>
        <dt>{{ translate('dashboard.lastUpdate') }}</dt>
        <dd class="bb-muted" data-testid="dashboard-health-updated">{{ lastUpdatedText }}</dd>
      </div>
    </dl>

    <p class="bb-muted text-xs bb-dash__hint">{{ translate('dashboard.rttHint') }}</p>
    <p v-if="!deviceConnected" class="bb-muted text-xs bb-dash__hint">{{ translate('dashboard.healthOfflineHint') }}</p>
  </article>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { translate } from '@/locales';
import { ControlEvent } from '@/api';
import { globalNotify } from '@/composables/notification';
import { useWsStore } from '@/stores/websocket';
import {
  dashboardState,
  refreshDashboard,
} from '@/components/dashboard/dashboardState';

const wsStore = useWsStore();
const refreshing = ref(false);
const refreshError = ref('');

const deviceConnected = computed(() => wsStore.state === ControlEvent.CONNECTED);
const unknownText = computed(() => translate('dashboard.unknownField'));

const wifiRssi = computed<number | null>(() => {
  if (dashboardState.wifi.staConnected === true && dashboardState.wifi.rssi !== null) {
    return dashboardState.wifi.rssi;
  }
  // Firmware-reported fallback (optional additive field); never synthesized.
  return dashboardState.target.detail.wifiRssi;
});

const wifiRssiText = computed<string>(() => {
  const rssi = wifiRssi.value;
  return typeof rssi === 'number' && Number.isFinite(rssi) ? `${rssi} dBm` : '';
});

type TagType = 'success' | 'info' | 'warning' | 'danger';

/** Signal quality labels are derived from the measured dBm value (not a backend field). */
const wifiQuality = computed<'excellent' | 'good' | 'fair' | 'poor' | null>(() => {
  const rssi = wifiRssi.value;
  if (typeof rssi !== 'number' || !Number.isFinite(rssi)) {
    return null;
  }
  if (rssi >= -60) {
    return 'excellent';
  }
  if (rssi >= -70) {
    return 'good';
  }
  if (rssi >= -80) {
    return 'fair';
  }
  return 'poor';
});
const wifiQualityText = computed<string>(() => {
  switch (wifiQuality.value) {
    case 'excellent':
      return translate('dashboard.wifiQualityExcellent');
    case 'good':
      return translate('dashboard.wifiQualityGood');
    case 'fair':
      return translate('dashboard.wifiQualityFair');
    case 'poor':
      return translate('dashboard.wifiQualityPoor');
    default:
      return '';
  }
});
const wifiQualityTagType = computed<'success' | 'info' | 'warning' | 'danger'>(() => {
  switch (wifiQuality.value) {
    case 'excellent':
    case 'good':
      return 'success';
    case 'fair':
      return 'warning';
    case 'poor':
      return 'danger';
    default:
      return 'info';
  }
});

const rttLast = computed(() => dashboardState.link.last);
const rttMin = computed(() => dashboardState.link.min);
const rttAvg = computed(() => dashboardState.link.avg);
const rttMax = computed(() => dashboardState.link.max);
const rttStatsText = computed(() => {
  if (rttAvg.value === null || rttMin.value === null || rttMax.value === null) {
    return '';
  }
  return translate('dashboard.rttStats', {
    avg: rttAvg.value,
    min: rttMin.value,
    max: rttMax.value,
  });
});

const detail = computed(() => dashboardState.target.detail);
const statusEndpointText = computed<string>(() => {
  const current = detail.value;
  if (current.available) {
    return translate('dashboard.statusSupported', { status: current.httpStatus });
  }
  if (current.httpStatus === 404) {
    return translate('dashboard.statusUnsupported');
  }
  if (current.error) {
    return translate('dashboard.statusRequestFailed', { err: current.error });
  }
  return unknownText.value;
});

const dapSessionText = computed<string>(() => {
  const active = detail.value.dapSessionActive;
  if (typeof active === 'boolean') {
    return active
      ? translate('dashboard.dapSessionActive')
      : translate('dashboard.dapSessionIdle');
  }
  return unknownText.value;
});

const swdManualText = computed<string>(() => {
  const manual = detail.value.swdClockManualHz;
  if (manual === null) {
    return unknownText.value;
  }
  return manual === 0
    ? translate('target.swdClockAuto')
    : translate('dashboard.swdActualValue', { khz: Math.round(manual / 1000) });
});

const dapRingText = computed<string>(() => {
  const preallocated = detail.value.dapRingPreallocated;
  if (preallocated === null) {
    return unknownText.value;
  }
  return preallocated ? translate('common.yes') : translate('common.no');
});

const swdClockText = computed<string>(() => {
  const hz = detail.value.swdClockHz ?? dashboardState.target.info?.swd_clock_hz ?? null;
  if (typeof hz !== 'number' || !Number.isFinite(hz) || hz <= 0) {
    return unknownText.value;
  }
  return translate('dashboard.swdActualValue', { khz: Math.round(hz / 1000) });
});

function formatBytes(value: number | null): string {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    return unknownText.value;
  }
  if (value >= 1024 * 1024) {
    return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
  }
  if (value >= 1024) {
    return `${(value / 1024).toFixed(1)} KiB`;
  }
  return `${Math.round(value)} B`;
}

const heapFreeText = computed(() => formatBytes(detail.value.heapFree));
const heapLargestText = computed(() => formatBytes(detail.value.heapLargest));
const dapLatencyText = computed<string>(() => {
  const avg = detail.value.dapLatencyAvgUs;
  const max = detail.value.dapLatencyMaxUs;
  if (avg === null && max === null) {
    return '';
  }
  return translate('dashboard.dapLatencyValue', {
    avg: avg === null ? '?' : avg,
    max: max === null ? '?' : max,
  });
});

const dapXferCountText = computed<string>(() => {
  const count = detail.value.dapXferCount;
  return count === null ? '' : `${count}`;
});

const dapXferLastText = computed<string>(() => {
  const last = detail.value.dapXferLastUs;
  return last === null ? '' : `${last} µs`;
});

const wifiChannelText = computed<string>(() => {
  const channel = detail.value.wifiChannel;
  return channel === null ? '' : `${channel}`;
});

const healthError = computed(() => refreshError.value);

const healthState = computed<'ok' | 'warn' | 'error' | 'unknown'>(() => {
  const current = detail.value;
  if (current.failures > 0 || current.error) {
    return 'error';
  }
  if (!deviceConnected.value) {
    return 'unknown';
  }
  if (current.httpStatus === 404) {
    return 'warn';
  }
  if (current.available) {
    const rssi = wifiRssi.value;
    const rtt = dashboardState.link.avg;
    const weakWifi = typeof rssi === 'number' && rssi < -80;
    const slowLink = typeof rtt === 'number' && rtt > 400;
    return weakWifi || slowLink ? 'warn' : 'ok';
  }
  return 'unknown';
});

const healthText = computed<string>(() => {
  switch (healthState.value) {
    case 'ok':
      return translate('dashboard.healthOk');
    case 'warn':
      return translate('dashboard.healthWarn');
    case 'error':
      return translate('dashboard.healthError');
    default:
      return translate('dashboard.healthUnknown');
  }
});

const healthTagType = computed<TagType>(() => {
  switch (healthState.value) {
    case 'ok':
      return 'success';
    case 'warn':
      return 'warning';
    case 'error':
      return 'danger';
    default:
      return 'info';
  }
});

const lastUpdatedText = computed<string>(() => {
  const stamps = [
    detail.value.updatedAt ?? 0,
    dashboardState.link.updatedAt ?? 0,
    dashboardState.wifi.updatedAt ?? 0,
  ];
  const latest = Math.max(...stamps);
  return latest > 0 ? new Date(latest).toLocaleTimeString() : unknownText.value;
});

async function onRefresh(): Promise<void> {
  if (refreshing.value) {
    return;
  }
  refreshing.value = true;
  refreshError.value = '';
  try {
    await refreshDashboard();
    if (dashboardState.target.detail.error) {
      refreshError.value = translate('dashboard.statusRequestFailed', {
        err: dashboardState.target.detail.error,
      });
      globalNotify(refreshError.value, 'warning');
      return;
    }
    globalNotify(translate('dashboard.healthRefreshed'), 'success');
  } catch (error) {
    refreshError.value = translate('dashboard.healthRefreshFailed', {
      err: error instanceof Error ? error.message : 'network error',
    });
    globalNotify(refreshError.value, 'error');
  } finally {
    refreshing.value = false;
  }
}
</script>

<style scoped>
.bb-health {
  grid-column: span 2;
}

.bb-health__head-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
}

.bb-health__sub {
  margin-left: var(--bb-space-2);
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-muted);
}

@media (max-width: 900px) {
  .bb-health {
    grid-column: span 1;
  }
}
</style>
