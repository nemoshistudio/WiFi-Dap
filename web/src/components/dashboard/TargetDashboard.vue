<template>
  <section class="bb-card bb-dash" data-testid="target-dashboard">
    <div class="bb-card__head bb-dash__head">
      <div>
        <h2 class="bb-card__title text-lg">{{ translate('dashboard.title') }}</h2>
        <p class="bb-card__hint">{{ translate('dashboard.subtitle') }}</p>
      </div>
      <div class="bb-dash__head-actions">
        <span v-if="lastUpdated" class="bb-dash__updated">{{ translate('dashboard.lastUpdated', { time: lastUpdated }) }}</span>
        <el-button size="small" :loading="refreshing" data-testid="dashboard-refresh" @click="onRefresh">
          {{ translate('dashboard.refresh') }}
        </el-button>
        <el-button size="small" data-testid="dashboard-diagnostics" @click="emit('open-diagnostics')">
          {{ translate('dashboard.openDiagnostics') }}
        </el-button>
      </div>
    </div>

    <el-alert
        v-if="!deviceConnected"
        class="mb-3"
        type="info"
        :title="translate('dashboard.deviceOfflineHint')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="refreshError"
        class="mb-3"
        type="warning"
        :title="refreshError"
        :closable="false"
        show-icon
        data-testid="dashboard-refresh-error"
    />

    <div class="bb-dash__grid">
      <!-- DAP -->
      <article class="bb-dash__card" data-testid="dashboard-dap">
        <header class="bb-dash__card-head">
          <h3>{{ translate('dashboard.dapTitle') }}</h3>
          <span class="bb-dash__dot" :class="wsDotClass" aria-hidden="true"></span>
        </header>
        <dl class="bb-dash__kv">
          <div>
            <dt>{{ translate('dashboard.wsConnection') }}</dt>
            <dd>
              <el-tag size="small" :type="wsTagType" data-testid="dashboard-ws-state">{{ wsStateText }}</el-tag>
            </dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wsHost') }}</dt>
            <dd class="bb-mono">{{ deviceHost }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.dapSession') }}</dt>
            <dd :class="{'bb-muted': true}" data-testid="dashboard-dap-session">{{ dapSessionText }}</dd>
          </div>
        </dl>
      </article>

      <!-- WiFi -->
      <article class="bb-dash__card" data-testid="dashboard-wifi">
        <header class="bb-dash__card-head">
          <h3>{{ translate('dashboard.wifiTitle') }}</h3>
          <el-tag v-if="deviceConnected" size="small" :type="wifiTagType">{{ wifiStateText }}</el-tag>
        </header>
        <dl class="bb-dash__kv">
          <div>
            <dt>{{ translate('dashboard.wifiSsid') }}</dt>
            <dd data-testid="dashboard-wifi-ssid">{{ wifiSsidText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiRssi') }}</dt>
            <dd data-testid="dashboard-wifi-rssi">{{ wifiRssiText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiIp') }}</dt>
            <dd class="bb-mono">{{ wifiIpText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiGateway') }}</dt>
            <dd class="bb-mono">{{ wifiGatewayText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiMode') }}</dt>
            <dd>{{ wifiModeText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiApOn') }}</dt>
            <dd>{{ wifiApOnText }}</dd>
          </div>
          <div>
            <dt>{{ translate('dashboard.wifiApSsid') }}</dt>
            <dd>{{ wifiApSsidText }}</dd>
          </div>
        </dl>
      </article>

      <!-- Target -->
      <article class="bb-dash__card" data-testid="dashboard-target">
        <header class="bb-dash__card-head">
          <h3>{{ translate('dashboard.targetTitle') }}</h3>
          <el-tag v-if="flashing" size="small" type="warning">{{ translate('dashboard.flashing') }}</el-tag>
          <el-tag v-else-if="flashSuccess" size="small" type="success">{{ translate('dashboard.flashSuccess') }}</el-tag>
          <el-tag v-else-if="detectedCandidate" size="small" type="success">{{ translate('curve.targetDetected') }}</el-tag>
        </header>
        <template v-if="detectedCandidate">
          <dl class="bb-dash__kv">
            <div>
              <dt>{{ translate('target.family') }}</dt>
              <dd>{{ detectedCandidate.series || detectedCandidate.baseGroup || translate('common.unknown') }}</dd>
            </div>
            <div>
              <dt>{{ translate('dashboard.targetModel') }}</dt>
              <dd data-testid="dashboard-target-model">{{ autoMatch?.profile.orderCode || translate('common.unknown') }}</dd>
            </div>
            <div>
              <dt>DEV_ID</dt>
              <dd class="bb-mono">{{ devIdText }}</dd>
            </div>
            <div>
              <dt>{{ translate('target.flashSize') }}</dt>
              <dd>{{ flashText }}</dd>
            </div>
            <div class="bb-dash__kv-wide">
              <dt>{{ translate('target.uid') }}</dt>
              <dd class="bb-mono">
                <template v-if="uidWords.length">
                  <div v-for="(word, index) in uidWords" :key="index">UID{{ index }}: {{ word }}</div>
                </template>
                <span v-else>{{ translate('common.unknown') }}</span>
              </dd>
            </div>
          </dl>
          <p v-if="targetInfo?.supported === false" class="bb-dash__warn">{{ translate('target.unsupportedFamily') }}</p>
        </template>
        <template v-else>
          <p class="bb-dash__empty">{{ translate('dashboard.targetUnknown') }}</p>
          <p class="bb-muted text-xs">{{ translate('dashboard.targetHint') }}</p>
        </template>
        <el-alert
            v-if="detectError"
            class="mt-2"
            type="warning"
            :title="detectError"
            :closable="false"
            show-icon
        />
      </article>

      <!-- Wired / wireless fusion (P1-20) + current physical link -->
      <article class="bb-dash__card bb-dash__link" data-testid="dashboard-link">
        <header class="bb-dash__card-head">
          <h3>{{ translate('dashboard.linkTitle') }}</h3>
          <el-tag size="small" :type="linkTagType" data-testid="dashboard-link-kind">{{ linkText }}</el-tag>
        </header>

        <div class="bb-dash__link-current">
          <span class="bb-muted text-xs">{{ translate('dashboard.currentPhysical') }}</span>
          <span class="bb-mono" data-testid="dashboard-link-hostname">{{ linkInfo.hostname || translate('common.unknown') }}</span>
        </div>
        <p v-if="linkHint" class="bb-muted text-xs bb-dash__hint" data-testid="dashboard-link-hint">{{ linkHint }}</p>

        <div class="bb-dash__link-section" data-testid="dashboard-link-wifi">
          <h4>{{ translate('dashboard.linkWifi') }}</h4>
          <dl class="bb-dash__kv">
            <div>
              <dt>{{ translate('dashboard.wifiSsid') }}</dt>
              <dd>{{ wifiSsidText }}</dd>
            </div>
            <div>
              <dt>{{ translate('dashboard.linkAddress') }}</dt>
              <dd class="bb-mono">{{ wifiIpText }}</dd>
            </div>
          </dl>
        </div>

        <div class="bb-dash__link-section" data-testid="dashboard-link-ncm">
          <h4>{{ translate('dashboard.linkWired') }}</h4>
          <dl class="bb-dash__kv">
            <div>
              <dt>{{ translate('dashboard.ncmState') }}</dt>
              <dd data-testid="dashboard-ncm-state">{{ ncmStateText }}</dd>
            </div>
            <div>
              <dt>{{ translate('dashboard.dhcpState') }}</dt>
              <dd data-testid="dashboard-dhcp-state">{{ dhcpStateText }}</dd>
            </div>
            <div>
              <dt>{{ translate('dashboard.wiredDeviceIp') }}</dt>
              <dd class="bb-mono" data-testid="dashboard-wired-ip">{{ wiredIpText }}</dd>
            </div>
          </dl>
          <p v-if="ncmHint" class="bb-muted text-xs bb-dash__hint" data-testid="dashboard-ncm-hint">{{ ncmHint }}</p>
        </div>
      </article>

      <!-- Link health (P0-8: browser-measured RTT / RSSI / session / SWD clock / heap) -->
      <LinkHealthCard />
    </div>

    <!-- Error summary -->
    <div class="bb-dash__errors" data-testid="dashboard-errors">
      <h3 class="text-sm font-semibold">{{ translate('dashboard.errors') }}</h3>
      <ul v-if="errors.length" class="bb-dash__error-list">
        <li v-for="(item, index) in errors" :key="index">{{ item }}</li>
      </ul>
      <p v-else class="bb-muted text-xs">{{ translate('dashboard.noErrors') }}</p>
    </div>

    <!-- Quick actions -->
    <div class="bb-dash__actions" data-testid="dashboard-actions">
      <span class="bb-muted text-xs">{{ translate('dashboard.quickActions') }}</span>
      <div class="flex flex-wrap gap-2">
        <el-button
            type="primary"
            size="small"
            :loading="detecting"
            :disabled="flashing"
            data-testid="dashboard-detect"
            @click="emit('detect')"
        >
          {{ detecting ? translate('target.detecting') : translate('target.detect') }}
        </el-button>
        <el-button size="small" :disabled="flashing" data-testid="dashboard-goto-flash" @click="emit('goto-flash')">
          {{ translate('dashboard.gotoTarget') }}
        </el-button>
        <el-button size="small" data-testid="dashboard-goto-curve" @click="router.push('/debug-curve')">
          {{ translate('dashboard.gotoCurve') }}
        </el-button>
        <el-button size="small" data-testid="dashboard-goto-uart" @click="router.push('/uart')">
          {{ translate('dashboard.gotoUart') }}
        </el-button>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { translate } from '@/locales';
import { ControlEvent } from '@/api';
import type { TargetInfo } from '@/api/apiTarget';
import { useWsStore } from '@/stores/websocket';
import { useSystemStore } from '@/stores/useSystemStore';
import {
  autoMatchTargetProfile,
  formatDevId,
  formatFlashKib,
  parseTargetDetection,
} from '@/utils/targetProfile';
import {
  dashboardState,
  getLinkInfo,
  refreshDashboard,
} from '@/components/dashboard/dashboardState';
import LinkHealthCard from '@/components/dashboard/LinkHealthCard.vue';

const props = defineProps<{
  targetInfo: TargetInfo | null;
  detecting: boolean;
  detectError: string;
  flashing: boolean;
  flashSuccess: boolean;
}>();

const emit = defineEmits<{
  (e: 'detect'): void;
  (e: 'goto-flash'): void;
  (e: 'open-diagnostics'): void;
}>();

const router = useRouter();
const wsStore = useWsStore();
const systemStore = useSystemStore();
const refreshing = ref(false);
const refreshError = ref('');

const deviceConnected = computed(() => wsStore.state === ControlEvent.CONNECTED);
const deviceHost = computed(() => {
  if (import.meta.env.VITE_APP_MODE === 'dev') {
    return import.meta.env.VITE_DEVICE_HOST_NAME || 'dap.local';
  }
  return window.location.host;
});

/* ---- WebSocket (DAP) ---- */
/**
 * Connection state -> literal i18n key via explicit mapping (i18n contract
 * §2.2 forbids dynamic keys such as translate(state.toLowerCase())).
 */
function wsStateTextFor(state: ControlEvent): string {
  switch (state) {
    case ControlEvent.CONNECTED:
      return translate('ws.connected');
    case ControlEvent.CONNECTING:
    case ControlEvent.LOADED:
      return translate('ws.connecting');
    default:
      return translate('ws.disconnected');
  }
}
const wsStateText = computed(() => wsStateTextFor(wsStore.state));
const wsTagType = computed<'success' | 'warning' | 'danger'>(() => {
  switch (wsStore.state) {
    case ControlEvent.CONNECTED:
      return 'success';
    case ControlEvent.CONNECTING:
    case ControlEvent.LOADED:
      return 'warning';
    default:
      return 'danger';
  }
});
const wsDotClass = computed(() => (deviceConnected.value ? 'is-ok' : 'is-off'));
const dapSessionText = computed(() => {
  const target = dashboardState.target;
  if (typeof target.detail.dapSessionActive === 'boolean') {
    return target.detail.dapSessionActive
      ? translate('dashboard.dapSessionActive')
      : translate('dashboard.dapSessionIdle');
  }
  if (target.statusKnown && typeof target.status?.dapSessionActive === 'boolean') {
    return target.status.dapSessionActive
      ? translate('dashboard.dapSessionActive')
      : translate('dashboard.dapSessionIdle');
  }
  if (dashboardState.dap.lastKick) {
    return translate('dashboard.dapSessionAfterKick');
  }
  return translate('dashboard.dapSessionUnknown');
});

/* ---- WiFi ---- */
const wifiTagType = computed<'success' | 'info' | 'danger'>(() => {
  if (dashboardState.wifi.staConnected === true) {
    return 'success';
  }
  if (dashboardState.wifi.staKnown) {
    return 'danger';
  }
  return 'info';
});
const wifiStateText = computed(() => {
  if (dashboardState.wifi.staConnected === true) {
    return translate('ws.connected');
  }
  if (dashboardState.wifi.staKnown) {
    return translate('ws.disconnected');
  }
  return translate('common.unknown');
});
function wifiUnknown(): string {
  return deviceConnected.value
    ? translate('dashboard.wifiUnknown')
    : translate('common.unknown');
}
const wifiSsidText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.staKnown) {
    return wifiUnknown();
  }
  if (dashboardState.wifi.staConnected !== true) {
    return translate('dashboard.wifiDisconnected');
  }
  return dashboardState.wifi.ssid || translate('common.unknown');
});
const wifiRssiText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.staKnown) {
    return wifiUnknown();
  }
  if (dashboardState.wifi.staConnected !== true || dashboardState.wifi.rssi === null) {
    return translate('common.unknown');
  }
  return `${dashboardState.wifi.rssi} dBm`;
});
const wifiIpText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.staKnown) {
    return wifiUnknown();
  }
  if (dashboardState.wifi.staConnected !== true) {
    return translate('dashboard.wifiDisconnected');
  }
  return dashboardState.wifi.ip || translate('common.unknown');
});
const wifiGatewayText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.staKnown
      || dashboardState.wifi.staConnected !== true || !dashboardState.wifi.gateway) {
    return translate('common.unknown');
  }
  return dashboardState.wifi.gateway;
});
const wifiModeText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.modeKnown || dashboardState.wifi.status === null) {
    return wifiUnknown();
  }
  // WifiStatus: 0 null / 1 STA / 2 AP / 3 APSTA (docs/protocol.md §2.2).
  switch (dashboardState.wifi.status) {
    case 3:
      return translate('dashboard.wifiModeApSta');
    case 2:
      return translate('dashboard.wifiModeAp');
    case 1:
      return translate('dashboard.wifiModeSta');
    case 0:
      return translate('dashboard.wifiModeOff');
    default:
      return translate('common.unknown');
  }
});
const wifiApOnText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.modeKnown || dashboardState.wifi.status === null) {
    return wifiUnknown();
  }
  const apOn = dashboardState.wifi.status === 2 || dashboardState.wifi.status === 3;
  return apOn ? translate('common.yes') : translate('common.no');
});
const wifiApSsidText = computed(() => {
  if (!deviceConnected.value || !dashboardState.wifi.apKnown) {
    return wifiUnknown();
  }
  return dashboardState.wifi.apSsid || translate('common.unknown');
});

/* ---- Target ---- */
const detectedCandidate = computed(() => {
  const target = props.targetInfo;
  if (!target?.detected) {
    return null;
  }
  const parsed = parseTargetDetection(target);
  return parsed.detected ? parsed : null;
});
const autoMatch = computed(() => autoMatchTargetProfile(props.targetInfo));
const devIdText = computed(() => {
  const devId = detectedCandidate.value?.devId;
  return typeof devId === 'number' && devId > 0 ? formatDevId(devId) : translate('common.unknown');
});
const flashText = computed(() => {
  const kib = detectedCandidate.value?.flashKib;
  const text = formatFlashKib(kib);
  return text === '-' ? translate('common.unknown') : text;
});
const uidWords = computed(() => {
  const raw = String(props.targetInfo?.uid ?? '')
      .replace(/^0x/i, '')
      .replace(/[^0-9a-f]/gi, '')
      .toUpperCase();
  if (raw.length !== 24) {
    return raw ? [`0x${raw}`] : [];
  }
  return [raw.slice(0, 8), raw.slice(8, 16), raw.slice(16, 24)].map((word) => `0x${word}`);
});

/* ---- Physical link + WiFi/NCM fusion (P1-20) ---- */
const linkInfo = getLinkInfo();
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
const linkTagType = computed<'success' | 'info' | 'warning'>(() => {
  switch (linkInfo.kind) {
    case 'wired':
      return 'success';
    case 'wifi':
      return 'info';
    default:
      return 'warning';
  }
});
const ncmActive = computed<boolean | null>(() => dashboardState.target.detail.wiredNcmActive);
const dhcpActive = computed<boolean | null>(() => dashboardState.target.detail.wiredDhcpActive);
const dhcpState = computed<number | null>(() => dashboardState.target.detail.wiredDhcpState);
const dhcpError = computed<number | null>(() => dashboardState.target.detail.wiredDhcpError);
const wiredIp = computed(() => dashboardState.target.detail.wiredIp);

const ncmKnown = computed(() => ncmActive.value !== null || dhcpActive.value !== null
    || dhcpState.value !== null || wiredIp.value !== '');

const ncmStateText = computed<string>(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmNotReported');
  }
  if (ncmActive.value === true) {
    return translate('dashboard.ncmActive');
  }
  if (ncmActive.value === false) {
    return translate('dashboard.ncmInactive');
  }
  return translate('common.unknown');
});
const dhcpStateText = computed<string>(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmNotReported');
  }
  if (dhcpError.value !== null && dhcpError.value !== 0) {
    return translate('dashboard.dhcpError', { code: dhcpError.value });
  }
  if (dhcpActive.value === true) {
    return translate('dashboard.dhcpStateStarted');
  }
  if (dhcpActive.value === false) {
    return translate('dashboard.dhcpStateStopped');
  }
  switch (dhcpState.value) {
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
const wiredIpText = computed<string>(() => {
  if (wiredIp.value) {
    return wiredIp.value;
  }
  if (!ncmKnown.value) {
    return translate('dashboard.ncmNotReported');
  }
  return translate('common.unknown');
});

const ncmHint = computed<string>(() => {
  if (!ncmKnown.value) {
    return translate('dashboard.ncmHintUnavailable');
  }
  const ready = ncmActive.value === true
      && (dhcpActive.value === true)
      && (dhcpError.value === null || dhcpError.value === 0);
  if (ready) {
    return translate('dashboard.ncmHintOk');
  }
  return translate('dashboard.ncmHintReplug');
});

const linkHint = computed<string>(() => {
  switch (linkInfo.kind) {
    case 'wired':
      // 192.168.7.x is the wired USB-NCM subnet: prefer wired, still tell the
      // user WiFi also exists without ever changing host networking.
      return ncmActive.value === true || dhcpActive.value === true || wiredIp.value !== ''
        ? translate('dashboard.wiredPreferred')
        : translate('dashboard.linkWiredHint');
    case 'wifi':
      return ncmKnown.value && (ncmActive.value === true || wiredIp.value !== '')
        ? translate('dashboard.wifiLinkNcmAvailable')
        : translate('dashboard.linkWifiHint');
    default:
      return translate('dashboard.linkUnknownHint');
  }
});

/* ---- Errors + refresh ---- */
const errors = computed<string[]>(() => {
  const list: string[] = [];
  if (props.detectError) {
    list.push(props.detectError);
  }
  if (dashboardState.wifi.lastError) {
    list.push(`${translate('diag.wifiError')}: ${dashboardState.wifi.lastError}`);
  }
  if (dashboardState.target.detail.error) {
    list.push(`${translate('dashboard.statusEndpoint')}: ${dashboardState.target.detail.error}`);
  }
  if (dashboardState.dap.lastKickError) {
    list.push(`${translate('diag.kickFailed', { err: dashboardState.dap.lastKickError })}`);
  }
  if (systemStore.protocolState === 'mismatch') {
    list.push(translate('protocol.mismatchTitle'));
  } else if (systemStore.protocolState === 'legacy') {
    list.push(translate('protocol.mismatchLegacy'));
  }
  return list;
});

const lastUpdated = computed(() => {
  const stamps = [
    dashboardState.wifi.updatedAt ?? 0,
    dashboardState.target.probedAt ?? 0,
    dashboardState.target.statusAt ?? 0,
    dashboardState.link.updatedAt ?? 0,
  ];
  const latest = Math.max(...stamps);
  return latest > 0 ? new Date(latest).toLocaleTimeString() : '';
});

async function onRefresh(): Promise<void> {
  if (refreshing.value) {
    return;
  }
  refreshing.value = true;
  refreshError.value = '';
  try {
    // Parallel: WiFi WS queries + HTTP status snapshot (single deduped path).
    await refreshDashboard();
    if (dashboardState.target.detail.error) {
      refreshError.value = translate('dashboard.statusRequestFailed', {
        err: dashboardState.target.detail.error,
      });
    }
  } catch (error) {
    refreshError.value = translate('dashboard.healthRefreshFailed', {
      err: error instanceof Error ? error.message : 'network error',
    });
  } finally {
    refreshing.value = false;
  }
}
</script>

<style scoped>
.bb-dash {
  margin-top: var(--bb-space-4);
}

.bb-dash__head {
  align-items: flex-start;
}

.bb-dash__head-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--bb-space-2);
}

.bb-dash__updated {
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-faint);
}

.bb-dash__grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
  gap: var(--bb-space-3);
}

.bb-dash__card {
  min-width: 0;
  padding: var(--bb-space-3);
  border: 1px solid var(--bb-border-subtle);
  border-radius: var(--bb-radius-md);
  background: var(--bb-surface-inset);
}

.bb-dash__card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
  margin-bottom: var(--bb-space-2);
}

.bb-dash__card-head h3 {
  margin: 0;
  font-size: var(--bb-font-size-md);
  font-weight: 650;
  color: var(--bb-text);
}

.bb-dash__dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--bb-text-faint);
}

.bb-dash__dot.is-ok {
  background: var(--bb-success);
  box-shadow: 0 0 8px rgba(61, 220, 132, 0.6);
}

.bb-dash__dot.is-off {
  background: var(--bb-danger);
}

.bb-dash__kv {
  display: grid;
  gap: 6px;
  margin: 0;
}

.bb-dash__kv > div {
  display: grid;
  grid-template-columns: minmax(88px, 34%) 1fr;
  gap: var(--bb-space-2);
  align-items: baseline;
}

.bb-dash__kv dt {
  margin: 0;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-muted);
}

.bb-dash__kv dd {
  margin: 0;
  min-width: 0;
  overflow-wrap: anywhere;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-secondary);
}

.bb-dash__kv-wide {
  grid-template-columns: minmax(88px, 34%) 1fr;
}

.bb-dash__warn {
  margin: var(--bb-space-2) 0 0;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-warning);
}

.bb-dash__empty {
  margin: 0 0 var(--bb-space-1);
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-secondary);
}

.bb-dash__hint {
  margin: var(--bb-space-2) 0 0;
  line-height: 1.5;
}

/* Link fusion card */
.bb-dash__link {
  grid-column: span 2;
}

.bb-dash__link-current {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--bb-space-2);
  margin-bottom: var(--bb-space-2);
}

.bb-dash__link-section {
  margin-top: var(--bb-space-2);
  padding-top: var(--bb-space-2);
  border-top: 1px solid var(--bb-border-subtle);
}

.bb-dash__link-section h4 {
  margin: 0 0 var(--bb-space-1);
  font-size: var(--bb-font-size-sm);
  font-weight: 600;
  color: var(--bb-text);
}

.bb-dash__errors {
  margin-top: var(--bb-space-4);
  padding-top: var(--bb-space-3);
  border-top: 1px solid var(--bb-border-subtle);
}

.bb-dash__error-list {
  margin: var(--bb-space-1) 0 0;
  padding-left: 18px;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-warning);
}

.bb-dash__actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-3);
  margin-top: var(--bb-space-4);
  padding-top: var(--bb-space-3);
  border-top: 1px solid var(--bb-border-subtle);
}

@media (max-width: 900px) {
  .bb-dash__link {
    grid-column: span 1;
  }
}

@media (max-width: 639px) {
  .bb-dash__head-actions {
    justify-content: flex-start;
  }

  .bb-dash__kv > div {
    grid-template-columns: 1fr;
    gap: 0;
  }

  .bb-dash__kv-wide {
    grid-template-columns: 1fr;
  }
}
</style>
