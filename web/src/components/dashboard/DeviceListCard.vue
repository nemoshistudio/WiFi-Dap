<template>
  <article class="bb-dash__card bb-devices" data-testid="dashboard-devices">
    <header class="bb-dash__card-head">
      <div>
        <h3>{{ translate('dashboard.devicesTitle') }}</h3>
        <p class="bb-card__hint bb-devices__hint">{{ translate('dashboard.devicesSubtitle') }}</p>
      </div>
      <div class="bb-devices__head-actions">
        <el-button size="small" :loading="discoveryState.loading" data-testid="devices-refresh" @click="onRefresh">
          {{ translate('dashboard.deviceRefresh') }}
        </el-button>
        <el-button size="small" data-testid="devices-add-toggle" @click="addOpen = !addOpen">
          {{ translate('dashboard.deviceAdd') }}
        </el-button>
      </div>
    </header>

    <el-alert
        v-if="discoveryState.supported === false"
        class="mb-2"
        type="info"
        :title="translate('dashboard.devicesUnsupported')"
        :closable="false"
        show-icon
        data-testid="devices-unsupported"
    />
    <el-alert
        v-else-if="discoveryState.busy"
        class="mb-2"
        type="info"
        :title="translate('dashboard.devicesBusy')"
        :closable="false"
        show-icon
        data-testid="devices-busy"
    />
    <el-alert
        v-else-if="discoveryState.error"
        class="mb-2"
        type="warning"
        :title="translate('dashboard.devicesFailed', { err: discoveryState.error })"
        :closable="false"
        show-icon
        data-testid="devices-error"
    />
    <p
        v-if="showNoneFound"
        class="bb-muted text-xs bb-devices__none"
        data-testid="devices-none"
    >{{ translate('dashboard.devicesNone') }}</p>

    <el-alert
        v-if="discoveryState.loading && discoveryState.devices.length === 0"
        class="mb-2"
        type="info"
        :title="translate('dashboard.devicesLoading')"
        :closable="false"
        show-icon
    />

    <div v-if="addOpen" class="bb-devices__add" data-testid="devices-add-form">
      <el-input
          v-model="addHost"
          size="small"
          :placeholder="translate('dashboard.deviceAddHint')"
          data-testid="devices-add-host"
          @keydown.enter="onAdd"
      />
      <el-input
          v-model="addName"
          size="small"
          :placeholder="translate('dashboard.deviceName')"
          data-testid="devices-add-name"
          @keydown.enter="onAdd"
      />
      <el-button type="primary" size="small" data-testid="devices-add-confirm" @click="onAdd">
        {{ translate('dashboard.deviceAdd') }}
      </el-button>
      <el-button size="small" @click="addOpen = false">{{ translate('common.cancel') }}</el-button>
    </div>
    <p v-if="addError" class="bb-devices__error" data-testid="devices-add-error">{{ addError }}</p>

    <ul class="bb-devices__list" data-testid="devices-list">
      <li
          v-for="device in devices"
          :key="device.id"
          class="bb-devices__row"
          :class="{ 'is-current': isCurrent(device.host) }"
          :data-testid="`device-row-${device.host}`"
      >
        <div class="bb-devices__row-main">
          <span class="bb-devices__name">{{ device.name }}</span>
          <el-tag v-if="isCurrent(device.host)" size="small" type="success" data-testid="devices-current-tag">
            {{ translate('dashboard.deviceCurrent') }}
          </el-tag>
          <el-tag v-else size="small" type="info">{{ sourceText(device.source) }}</el-tag>
          <span class="bb-mono bb-devices__host">{{ device.host }}</span>
        </div>
        <div class="bb-devices__meta">
          <span v-if="device.target">{{ translate('dashboard.deviceTarget') }}: <span class="bb-mono">{{ device.target }}</span></span>
          <span v-if="device.version">{{ translate('dashboard.deviceVersion') }}: <span class="bb-mono">{{ device.version }}</span></span>
          <span v-if="device.rssi !== null">{{ translate('dashboard.deviceRssi') }}: <span class="bb-mono">{{ device.rssi }} dBm</span></span>
          <span v-if="device.lastSeenAt">
            {{ translate('dashboard.lastSeen') }}: {{ formatTime(device.lastSeenAt) }}
          </span>
        </div>
        <div class="bb-devices__row-actions">
          <el-button
              size="small"
              :disabled="isCurrent(device.host)"
              :title="translate('dashboard.deviceOpenHint')"
              :data-testid="`device-open-${device.host}`"
              @click="onOpen(device.host)"
          >
            {{ translate('dashboard.deviceOpen') }}
          </el-button>
          <el-button size="small" :data-testid="`device-rename-${device.host}`" @click="onRename(device.host, device.name)">
            {{ translate('dashboard.deviceRename') }}
          </el-button>
          <el-button
              v-if="device.source === 'manual'"
              size="small"
              type="danger"
              plain
              :data-testid="`device-delete-${device.host}`"
              @click="onDelete(device.host)"
          >
            {{ translate('dashboard.deviceDelete') }}
          </el-button>
        </div>
      </li>
      <li v-if="devices.length === 0" class="bb-muted text-xs" data-testid="devices-empty">
        {{ translate('dashboard.deviceEmpty') }}
      </li>
    </ul>

    <p v-if="discoveryState.updatedAt" class="bb-muted text-xs bb-devices__updated" data-testid="devices-updated">
      {{ translate('dashboard.devicesLastUpdated', { time: formatTime(discoveryState.updatedAt) }) }}
    </p>
  </article>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { ElMessageBox } from 'element-plus';
import { translate } from '@/locales';
import { globalNotify } from '@/composables/notification';
import { normalizeDeviceHost } from '@/utils/deviceHost';
import { runWhenIdle } from '@/utils/idle';
import {
  addManualDevice,
  currentDeviceHost,
  discoveryState,
  knownDevices,
  openDevice,
  refreshDiscoveredDevices,
  removeManualDevice,
  renameManualDevice,
  type DeviceSource,
} from '@/utils/deviceDiscovery';

const addOpen = ref(false);
const addHost = ref('');
const addName = ref('');
const addError = ref('');

const devices = computed(() => knownDevices());
const showNoneFound = computed(() => discoveryState.supported === true
  && !discoveryState.busy
  && !discoveryState.error
  && discoveryState.loading === false
  && discoveryState.devices.length === 0
  && discoveryState.updatedAt !== null);

function isCurrent(host: string): boolean {
  return normalizeDeviceHost(host) === currentDeviceHost();
}

function sourceText(source: DeviceSource): string {
  switch (source) {
    case 'current':
      return translate('dashboard.deviceSourceCurrent');
    case 'discovered':
      return translate('dashboard.deviceSourceDiscovered');
    case 'manual':
      return translate('dashboard.deviceSourceManual');
    default:
      return translate('dashboard.deviceSourceRecent');
  }
}

function formatTime(timestamp: number): string {
  try {
    return new Date(timestamp).toLocaleTimeString();
  } catch {
    return translate('common.unknown');
  }
}

async function onRefresh(): Promise<void> {
  await refreshDiscoveredDevices({force: true});
  if (discoveryState.supported === true && !discoveryState.error) {
    globalNotify(translate('dashboard.devicesOk'), 'success');
  }
}

function onAdd(): void {
  addError.value = '';
  if (!normalizeDeviceHost(addHost.value)) {
    addError.value = translate('dashboard.deviceInvalidHost');
    return;
  }
  const ok = addManualDevice(addHost.value, addName.value);
  if (!ok) {
    addError.value = translate('dashboard.deviceAddFailed');
    return;
  }
  globalNotify(translate('dashboard.deviceAdded', { host: normalizeDeviceHost(addHost.value) }), 'success');
  addHost.value = '';
  addName.value = '';
  addOpen.value = false;
}

function onOpen(host: string): void {
  if (isCurrent(host)) {
    globalNotify(translate('dashboard.deviceSameHost'), 'info');
    return;
  }
  const opened = openDevice(host);
  if (!opened) {
    globalNotify(translate('dashboard.deviceInvalidHost'), 'warning');
  }
}

async function onRename(host: string, currentName: string): Promise<void> {
  let value = '';
  try {
    const result = await ElMessageBox.prompt(translate('dashboard.deviceName'), translate('dashboard.deviceRenameTitle'), {
      confirmButtonText: translate('common.confirm'),
      cancelButtonText: translate('common.cancel'),
      inputValue: currentName,
      inputValidator: (input: string) => input.trim().length > 0,
    });
    value = result.value ?? '';
  } catch {
    return;
  }
  if (!value.trim()) {
    return;
  }
  renameManualDevice(host, value);
  globalNotify(translate('dashboard.deviceRenamed'), 'success');
}

async function onDelete(host: string): Promise<void> {
  try {
    await ElMessageBox.confirm(
        translate('dashboard.deviceRemoveConfirm', { host }),
        translate('dashboard.devicesTitle'),
        {
          confirmButtonText: translate('common.delete'),
          cancelButtonText: translate('common.cancel'),
          type: 'warning',
        },
    );
  } catch {
    return;
  }
  removeManualDevice(host);
}

onMounted(() => {
  // P1-16: one discovery probe, deferred until after the first screen so the
  // SWD-clock/status requests are not competing with it. 404 => manual mode.
  runWhenIdle(() => {
    void refreshDiscoveredDevices();
  });
});
</script>

<style scoped>
.bb-devices {
  margin-top: var(--bb-space-3);
}

.bb-devices__hint {
  margin: 2px 0 0;
}

.bb-devices__head-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
}

.bb-devices__add {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
  margin: 0 0 var(--bb-space-2);
}

.bb-devices__add :deep(.el-input) {
  width: 200px;
}

.bb-devices__error {
  margin: 0 0 var(--bb-space-2);
  font-size: var(--bb-font-size-xs);
  color: var(--bb-warning);
}

.bb-devices__none {
  margin: 0 0 var(--bb-space-2);
}

.bb-devices__list {
  margin: 0;
  padding: 0;
  list-style: none;
  display: grid;
  gap: var(--bb-space-2);
}

.bb-devices__row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
  padding: var(--bb-space-2) var(--bb-space-3);
  border: 1px solid var(--bb-border-subtle);
  border-radius: var(--bb-radius-md);
  background: var(--bb-surface-inset);
}

.bb-devices__row.is-current {
  border-color: rgba(61, 220, 132, 0.35);
}

.bb-devices__row-main {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--bb-space-2);
  min-width: 0;
}

.bb-devices__name {
  font-size: var(--bb-font-size-md);
  font-weight: 600;
  color: var(--bb-text);
  overflow-wrap: anywhere;
}

.bb-devices__host {
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-muted);
}

.bb-devices__meta {
  display: flex;
  flex-wrap: wrap;
  gap: var(--bb-space-3);
  width: 100%;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-muted);
}

.bb-devices__row-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--bb-space-1);
}

.bb-devices__updated {
  margin: var(--bb-space-2) 0 0;
}

@media (max-width: 639px) {
  .bb-devices__add :deep(.el-input) {
    width: 100%;
  }
}
</style>
