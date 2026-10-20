<template>
  <div class="text-layout">
    <h1 class="page-title">{{ translate('target.title') }}</h1>
    <p class="text-center text-xs opacity-70">{{ translate('target.description') }}</p>

    <el-alert
        class="mt-2"
        type="warning"
        :title="translate('target.securityHint')"
        :closable="false"
        show-icon
    />

    <el-divider></el-divider>

    <div class="flex flex-wrap items-center justify-between gap-2">
      <h2 class="text-lg font-bold">{{ translate('target.detect') }}</h2>
      <el-button type="primary" :loading="detecting" :disabled="flashing" @click="onDetect">
        {{ detecting ? translate('target.detecting') : translate('target.detect') }}
      </el-button>
    </div>

    <el-alert
        v-if="detectError"
        class="mt-2"
        type="warning"
        :title="detectError"
        :closable="false"
        show-icon
    />

    <el-descriptions v-if="targetInfo?.detected" class="mt-3" border :column="1">
      <el-descriptions-item :label="translate('target.family')">{{ targetInfo.series || targetInfo.family || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.model')">{{ targetInfo.device || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.cpuid')">{{ formatRegister(targetInfo.cpuid) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.idcode')">{{ formatRegister(targetInfo.idcode) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.uid')">{{ targetInfo.uid || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.flashBase')">{{ formatRegister(targetInfo.flash_base) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.flashSize')">{{ formatFlashSize(targetInfo.flash_size) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.running')">{{ formatBoolean(targetInfo.running) }}</el-descriptions-item>
    </el-descriptions>
    <el-alert
        v-if="targetInfo?.detected && targetInfo.supported === false"
        class="mt-2"
        type="warning"
        :title="translate('target.unsupportedFamily')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else-if="!detectError"
        class="mt-2"
        type="info"
        :title="translate('target.noTargetHint')"
        :closable="false"
        show-icon
    />

    <el-divider></el-divider>

    <div class="flex flex-wrap items-center justify-between gap-2">
      <h2 class="text-lg font-bold">{{ translate('target.selectFile') }}</h2>
      <el-button :disabled="flashing" @click="openFileDialog">{{ translate('target.selectFile') }}</el-button>
    </div>
    <input
        ref="fileInputRef"
        type="file"
        accept=".bin,application/octet-stream"
        class="hidden"
        @change="onFileSelected"
    />

    <el-descriptions v-if="selectedFile" class="mt-2" border :column="1">
      <el-descriptions-item :label="translate('target.fileName')">{{ selectedFile.name }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.fileSize')">{{ formatBytes(selectedFile.size) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.sha256')">
        <span v-if="hashing">{{ translate('target.hashComputing') }}</span>
        <span v-else class="font-mono text-xs break-all">{{ fileSha256 || '-' }}</span>
      </el-descriptions-item>
    </el-descriptions>

    <div class="flex flex-wrap gap-2 justify-center mt-4">
      <el-button
          type="primary"
          :loading="flashing"
          :disabled="!canFlash"
          @click="onStartFlash"
      >
        {{ flashing ? translate('target.flashing') : translate('target.start') }}
      </el-button>
      <el-button :loading="resetting" :disabled="flashing" @click="onResetTarget">
        {{ resetting ? translate('target.resetting') : translate('target.reset') }}
      </el-button>
    </div>

    <el-alert
        v-if="flashing"
        class="mt-3"
        type="info"
        :title="translate('target.flashingHint')"
        :closable="false"
        show-icon
    />

    <div v-if="flashing" class="mt-4">
      <div class="flex justify-between text-sm">
        <span>{{ translate('target.uploadProgress') }}</span>
        <span>{{ uploadPercent.toFixed(1) }} %</span>
      </div>
      <el-progress :percentage="uploadPercent"/>
      <div class="flex justify-between text-sm mt-3">
        <span>{{ translate('target.deviceStatus') }}</span>
        <span class="font-mono">{{ deviceStatusText }}</span>
      </div>
    </div>

    <el-descriptions
        v-if="phase === 'success' && result"
        class="mt-4"
        :title="translate('target.result')"
        border
        :column="1"
    >
      <el-descriptions-item :label="translate('target.family')">{{ result.series || targetInfo?.series || result.family || targetInfo?.family || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.model')">{{ targetInfo?.device || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.idcode')">
        {{ formatRegister(result.idcode ?? targetInfo?.idcode) }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('target.uid')">{{ result.uid || targetInfo?.uid || '-' }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.written')">{{ formatBytes(result.written) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.sha256')">
        <span class="font-mono text-xs break-all">{{ result.sha256 || fileSha256 || '-' }}</span>
      </el-descriptions-item>
    </el-descriptions>

    <el-alert
        v-if="phase === 'success'"
        class="mt-3"
        type="success"
        :title="translate('target.success')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else-if="phase === 'error'"
        class="mt-3"
        type="error"
        :title="errorMessage || translate('common.failed')"
        :description="errorDetail"
        :closable="false"
        show-icon
    />
  </div>
</template>

<script setup lang="ts">
import {computed, onUnmounted, ref} from "vue";
import {ElMessageBox} from "element-plus";
import {translate} from "@/locales";
import {globalNotify} from "@/composables/notification";
import {
  flashTarget,
  getTargetInfo,
  getTargetStatus,
  resetTarget,
  type TargetApiError,
  type TargetFlashSuccess,
  type TargetInfo,
  type TargetStatus,
} from "@/api/apiTarget";
import {sha256Hex} from "@/utils/sha256";

type FlashPhase = 'idle' | 'flashing' | 'success' | 'error'

const targetInfo = ref<TargetInfo | null>(null);
const detecting = ref(false);
const detectError = ref('');
const fileInputRef = ref<HTMLInputElement | null>(null);
const selectedFile = ref<File | null>(null);
const fileSha256 = ref('');
const hashing = ref(false);
const phase = ref<FlashPhase>('idle');
const uploadPercent = ref(0);
const deviceStatus = ref<TargetStatus | null>(null);
const result = ref<TargetFlashSuccess | null>(null);
const errorMessage = ref('');
const errorDetail = ref('');
const resetting = ref(false);

let statusTimer = 0;
let flashHandle: { abort(): void } | null = null;

const flashing = computed(() => phase.value === 'flashing');
const canFlash = computed(() => {
  return targetInfo.value?.detected === true
      && targetInfo.value?.supported !== false
      && selectedFile.value !== null
      && fileSha256.value.length > 0
      && !hashing.value
      && !flashing.value;
});
const deviceStatusText = computed(() => {
  const status = deviceStatus.value;
  if (!status || (!status.state && status.percent === undefined)) {
    return '-';
  }
  const percent = typeof status.percent === 'number' ? ` (${status.percent}%)` : '';
  return `${status.state ?? '-'}${percent}`;
});

function formatBytes(bytes?: number): string {
  if (!bytes || bytes < 0) {
    return '-';
  }
  const units = ['B', 'KB', 'MB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return unit === 0 ? `${value} ${units[unit]}` : `${value.toFixed(2)} ${units[unit]}`;
}

function formatFlashSize(bytes?: number): string {
  if (!bytes) {
    return '-';
  }
  return `${formatBytes(bytes)} (${bytes} B)`;
}

/** 寄存器数值统一显示为 0x + 8 位大写 hex（协议原始字段，不做本地化）。 */
function formatRegister(value?: number | string): string {
  if (value === undefined || value === null || value === '') {
    return '-';
  }
  if (typeof value === 'number') {
    return `0x${(value >>> 0).toString(16).toUpperCase().padStart(8, '0')}`;
  }
  return value;
}

function formatBoolean(value?: boolean): string {
  if (typeof value !== 'boolean') {
    return '-';
  }
  return value ? translate('common.yes') : translate('common.no');
}

function stopStatusPolling() {
  if (statusTimer) {
    window.clearInterval(statusTimer);
    statusTimer = 0;
  }
}

function startStatusPolling() {
  stopStatusPolling();
  // /api/target/status 为可选端点：不可用时返回 null，静默停止轮询（MVP 不依赖进度）
  statusTimer = window.setInterval(async () => {
    const status = await getTargetStatus();
    if (!status) {
      stopStatusPolling();
      return;
    }
    deviceStatus.value = status;
  }, 1000);
}

async function onDetect() {
  if (detecting.value || flashing.value) {
    return;
  }
  detecting.value = true;
  detectError.value = '';
  targetInfo.value = null;
  try {
    const info = await getTargetInfo();
    targetInfo.value = info;
    if (info.httpStatus === 404) {
      detectError.value = translate('target.notSupported');
    } else if (!info.detected) {
      detectError.value = info.err
          ? translate('target.detectFailed', {err: info.err})
          : translate('target.notDetected');
    }
  } catch (error) {
    detectError.value = translate('target.detectFailed', {
      err: error instanceof Error ? error.message : 'network error',
    });
  } finally {
    detecting.value = false;
  }
}

function openFileDialog() {
  if (!flashing.value) {
    fileInputRef.value?.click();
  }
}

async function onFileSelected(event: Event) {
  const input = event.target as HTMLInputElement;
  const file = input.files?.[0];
  input.value = '';
  if (file) {
    await selectFile(file);
  }
}

function resetResultState() {
  phase.value = 'idle';
  result.value = null;
  errorMessage.value = '';
  errorDetail.value = '';
  uploadPercent.value = 0;
  deviceStatus.value = null;
}

async function selectFile(file: File) {
  if (flashing.value) {
    return;
  }
  resetResultState();
  if (!file.name.toLowerCase().endsWith('.bin')) {
    selectedFile.value = null;
    globalNotify(translate('target.invalidFile'), 'warning');
    return;
  }
  if (file.size <= 0) {
    selectedFile.value = null;
    globalNotify(translate('target.emptyFile'), 'warning');
    return;
  }
  const flashSize = targetInfo.value?.flash_size;
  if (typeof flashSize === 'number' && flashSize > 0 && file.size > flashSize) {
    selectedFile.value = null;
    globalNotify(translate('target.fileTooLarge', {size: formatBytes(flashSize)}), 'warning');
    return;
  }
  selectedFile.value = file;
  fileSha256.value = '';
  hashing.value = true;
  try {
    const buffer = await file.arrayBuffer();
    fileSha256.value = await sha256Hex(buffer);
  } catch (error) {
    fileSha256.value = '';
    globalNotify(translate('common.failed'), 'error');
    if (import.meta.env.VITE_APP_MODE === 'dev') {
      console.warn('[target] sha256 failed', error);
    }
  } finally {
    hashing.value = false;
  }
}

function handleFlashFailure(failure: TargetApiError) {
  if (failure.httpStatus === 404) {
    errorMessage.value = translate('target.notSupported');
  } else {
    errorMessage.value = translate('target.failed', {err: failure.err});
  }
  const parts = [failure.err];
  if (failure.code !== undefined) {
    parts.push(`code=${failure.code}`);
  }
  if (failure.httpStatus) {
    parts.push(`HTTP ${failure.httpStatus}`);
  }
  errorDetail.value = parts.filter(Boolean).join(' · ');
  globalNotify(errorMessage.value, 'error');
}

async function onStartFlash() {
  if (flashing.value) {
    return;
  }
  if (targetInfo.value?.detected !== true) {
    globalNotify(translate('target.noTargetHint'), 'warning');
    return;
  }
  if (targetInfo.value?.supported === false) {
    globalNotify(translate('target.unsupportedFamily'), 'warning');
    return;
  }
  if (!selectedFile.value) {
    globalNotify(translate('target.selectFirst'), 'warning');
    return;
  }
  if (!fileSha256.value) {
    globalNotify(translate('common.failed'), 'error');
    return;
  }
  const sizeText = formatBytes(selectedFile.value.size);
  const confirmed = await ElMessageBox.confirm(
      translate('target.confirmFlash', {size: sizeText}),
      translate('target.confirmFlashTitle'),
      {
        confirmButtonText: translate('common.confirm'),
        cancelButtonText: translate('common.cancel'),
        type: 'warning',
      },
  ).then(() => true).catch(() => false);
  if (!confirmed) {
    return;
  }
  resetResultState();
  phase.value = 'flashing';
  flashHandle = flashTarget(selectedFile.value, fileSha256.value, {
    onUploadProgress: (loaded, total) => {
      uploadPercent.value = total > 0 ? Math.min(100, (loaded / total) * 100) : 0;
    },
    onUploadComplete: () => {
      uploadPercent.value = 100;
      startStatusPolling();
    },
    onSuccess: (flashResult) => {
      stopStatusPolling();
      flashHandle = null;
      phase.value = 'success';
      result.value = flashResult;
      globalNotify(translate('target.success'), 'success');
    },
    onFailure: (failure) => {
      stopStatusPolling();
      flashHandle = null;
      phase.value = 'error';
      handleFlashFailure(failure);
    },
  });
}

async function onResetTarget() {
  if (resetting.value || flashing.value) {
    return;
  }
  resetting.value = true;
  try {
    const response = await resetTarget(true);
    if (response.ok) {
      globalNotify(translate('target.resetSuccess'), 'success');
    } else {
      globalNotify(translate('target.resetFailed', {err: response.err ?? 'unknown'}), 'error');
    }
  } finally {
    resetting.value = false;
  }
}

onUnmounted(() => {
  stopStatusPolling();
  // 烧录请求不主动 abort：页面离开/刷新时保持设备侧流程完成，避免半写状态更不可控
  flashHandle = null;
});
</script>

<style scoped>
</style>
