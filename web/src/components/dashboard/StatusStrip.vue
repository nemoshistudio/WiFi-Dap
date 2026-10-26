<template>
  <!--
    Compact global status strip (App shell, shown on every page except /status).

    Read-only: WS state / WiFi / target detection / last flash result all come
    from the shared stores (`useWsStore`, `dashboardState`). It never starts a
    second poll or WS connection; clicking it opens the detailed /status page.
  -->
  <router-link
      class="bb-status-strip"
      to="/status"
      :title="translate('status.openHint')"
      :aria-label="translate('status.openHint')"
      data-testid="status-strip"
  >
    <span class="bb-status-strip__item">
      <span class="bb-status-strip__dot" :class="wsOk ? 'is-ok' : 'is-off'" aria-hidden="true"></span>
      <span class="bb-status-strip__value" data-testid="strip-ws">{{ wsText }}</span>
    </span>

    <span class="bb-status-strip__sep" aria-hidden="true">·</span>

    <span class="bb-status-strip__item">
      <span class="bb-status-strip__label">{{ translate('status.wifi') }}</span>
      <span class="bb-status-strip__value bb-mono" data-testid="strip-wifi">{{ wifiText }}</span>
    </span>

    <span class="bb-status-strip__sep" aria-hidden="true">·</span>

    <span class="bb-status-strip__item">
      <span class="bb-status-strip__label">{{ translate('status.target') }}</span>
      <span class="bb-status-strip__value" data-testid="strip-target">{{ targetText }}</span>
    </span>

    <span class="bb-status-strip__sep" aria-hidden="true">·</span>

    <span class="bb-status-strip__item">
      <span class="bb-status-strip__label">{{ translate('status.lastFlash') }}</span>
      <span
          class="bb-status-strip__value"
          :class="flashClass"
          data-testid="strip-flash"
      >{{ flashText }}</span>
    </span>

    <span class="bb-status-strip__more">{{ translate('status.open') }} →</span>
  </router-link>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { translate } from '@/locales';
import { ControlEvent } from '@/api';
import { useWsStore } from '@/stores/websocket';
import { dashboardState } from '@/components/dashboard/dashboardState';

const wsStore = useWsStore();

const wsOk = computed(() => wsStore.state === ControlEvent.CONNECTED);
const deviceConnected = wsOk;
const wsText = computed(() => {
  // Explicit literal-key mapping (i18n contract §2.2 forbids dynamic keys).
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

/** One-line WiFi summary: SSID (+ RSSI) when the STA link is known. */
const wifiText = computed(() => {
  const wifi = dashboardState.wifi;
  if (!deviceConnected.value || !wifi.staKnown) {
    return translate('common.unknown');
  }
  if (wifi.staConnected !== true) {
    return translate('dashboard.wifiDisconnected');
  }
  const ssid = wifi.ssid || translate('common.unknown');
  return wifi.rssi === null ? ssid : `${ssid} · ${wifi.rssi} dBm`;
});

/** One-line target summary: detected family/model + first UID word, else "not detected". */
const targetText = computed(() => {
  const info = dashboardState.target.info;
  if (info?.detected !== true) {
    return translate('status.notDetected');
  }
  const family = info.series || info.family || translate('common.unknown');
  const uidHex = String(info.uid ?? '').replace(/^0x/i, '').replace(/[^0-9a-f]/gi, '').toUpperCase();
  const uidShort = uidHex.length >= 8 ? `UID0 0x${uidHex.slice(0, 8)}` : '';
  return uidShort ? `${family} · ${uidShort}` : family;
});

/** Last flash attempt (recorded by the target page in dashboardState.flash). */
const flashText = computed(() => {
  const flash = dashboardState.flash;
  switch (flash.phase) {
    case 'running':
      return translate('status.flashRunning');
    case 'success':
      return flash.at === null
        ? translate('status.flashOk')
        : `${translate('status.flashOk')} · ${new Date(flash.at).toLocaleTimeString()}`;
    case 'error':
      return translate('status.flashFail');
    default:
      return translate('status.flashNone');
  }
});
const flashClass = computed(() => {
  switch (dashboardState.flash.phase) {
    case 'success':
      return 'is-ok';
    case 'error':
      return 'is-error';
    case 'running':
      return 'is-running';
    default:
      return '';
  }
});
</script>

<style scoped>
.bb-status-strip {
  display: flex;
  flex-wrap: nowrap;
  align-items: center;
  gap: var(--bb-space-2);
  flex: 0 0 auto;
  margin: var(--bb-space-2) var(--bb-space-3) 0;
  padding: 4px var(--bb-space-3);
  border: 1px solid var(--bb-border-subtle);
  border-radius: var(--bb-radius-sm);
  background: var(--bb-surface-inset);
  color: var(--bb-text-muted);
  font-size: var(--bb-font-size-xs);
  line-height: 1.4;
  text-decoration: none;
  transition: border-color var(--bb-duration-fast) var(--bb-ease),
    background-color var(--bb-duration-fast) var(--bb-ease);
}

.bb-status-strip:hover {
  border-color: var(--bb-accent);
  background: var(--bb-surface);
}

.bb-status-strip__item {
  display: inline-flex;
  align-items: baseline;
  gap: 4px;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.bb-status-strip__label {
  flex: 0 0 auto;
  color: var(--bb-text-faint);
}

.bb-status-strip__value {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  color: var(--bb-text-secondary);
}

.bb-status-strip__value.is-ok {
  color: var(--bb-success);
}

.bb-status-strip__value.is-error {
  color: var(--bb-danger);
}

.bb-status-strip__value.is-running {
  color: var(--bb-warning);
}

.bb-status-strip__dot {
  flex: 0 0 auto;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--bb-text-faint);
}

.bb-status-strip__dot.is-ok {
  background: var(--bb-success);
  box-shadow: 0 0 6px rgba(61, 220, 132, 0.6);
}

.bb-status-strip__dot.is-off {
  background: var(--bb-danger);
}

.bb-status-strip__sep {
  flex: 0 0 auto;
  color: var(--bb-text-faint);
}

.bb-status-strip__more {
  flex: 0 0 auto;
  margin-left: auto;
  padding-left: var(--bb-space-2);
  color: var(--bb-accent);
  white-space: nowrap;
}

@media (max-width: 639px) {
  /* Keep one line on phones: hide the separators, allow horizontal scroll. */
  .bb-status-strip {
    overflow-x: auto;
    scrollbar-width: none;
  }

  .bb-status-strip::-webkit-scrollbar {
    display: none;
  }

  .bb-status-strip__sep {
    display: none;
  }
}
</style>
