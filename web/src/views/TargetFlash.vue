<template>
  <div class="text-layout bb-page bb-page--wide">
    <h1 class="page-title">{{ translate('target.title') }}</h1>
    <p class="text-center text-xs bb-muted">{{ translate('target.description') }}</p>

    <el-alert
        class="mt-2"
        type="warning"
        :title="translate('target.securityHint')"
        :closable="false"
        show-icon
    />

    <section class="bb-card">
      <div class="bb-card__head">
        <h2 class="bb-card__title text-lg">{{ translate('target.detect') }}</h2>
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

    </section>

    <section class="bb-card">
      <div class="bb-card__head">
        <h2 class="bb-card__title text-lg">{{ translate('target.selectFile') }}</h2>
        <el-button :disabled="flashing" @click="openFileDialog">{{ translate('target.selectFile') }}</el-button>
      </div>
    <input
        ref="fileInputRef"
        type="file"
        accept=".bin,.elf,.out,.hex,.ihex,.srec,.s19,.mot,application/octet-stream"
        class="hidden"
        @change="onFileSelected"
    />

    <el-alert v-if="converting" class="mt-2" type="info" :title="translate('target.convertComputing')" :closable="false" show-icon/>
    <el-alert v-if="convertError" class="mt-2" type="error" :title="convertError" :closable="false" show-icon/>

    <el-descriptions v-if="selectedFile" class="mt-2" border :column="1">
      <el-descriptions-item :label="translate('target.sourceFile')">{{ selectedFile.name }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.fileSize')">{{ formatBytes(selectedFile.size) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.fileFormat')">{{ detectedFormat ? formatLabel(detectedFormat) : '-' }}</el-descriptions-item>
    </el-descriptions>

    <el-descriptions
        v-if="firmware"
        class="mt-2"
        border
        :column="1"
        :title="translate('target.convertedImage')"
    >
      <el-descriptions-item :label="translate('target.fileFormat')">{{ formatLabel(firmware.format) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.flashBase')">{{ formatRegister(firmware.base) }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.imageSize')">{{ formatBytes(firmware.image.length) }} ({{ firmware.image.length }} B)</el-descriptions-item>
      <el-descriptions-item :label="translate('target.flashUsage')">{{ flashUsageText }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.segments')">{{ firmware.segments.length }}</el-descriptions-item>
      <el-descriptions-item :label="translate('target.sha256')">
        <span v-if="hashing">{{ translate('target.hashComputing') }}</span>
        <span v-else class="font-mono text-xs break-all">{{ fileSha256 || '-' }}</span>
      </el-descriptions-item>
    </el-descriptions>
    <el-alert
        v-if="firmware && firmware.ignoredRamSegments > 0"
        class="mt-2"
        type="info"
        :title="translate('target.ignoredSegments', {count: firmware.ignoredRamSegments})"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="firmware && firmware.filledGapBytes > 0"
        class="mt-2"
        type="info"
        :title="translate('target.filledGaps', {size: formatBytes(firmware.filledGapBytes)})"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="firmware && firmware.format === 'elf'"
        class="mt-2"
        type="info"
        :title="translate('target.debugInfoIgnored')"
        :closable="false"
        show-icon
    />

    </section>

    <section class="bb-card">
    <div class="flex flex-wrap gap-2 justify-center">
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
      <el-descriptions-item :label="translate('target.fileFormat')">{{ firmware ? formatLabel(firmware.format) : '-' }}</el-descriptions-item>
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
    </section>
  </div>
</template>

<script setup lang="ts">
import {computed, onUnmounted, ref, shallowRef} from "vue";
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
import {sha256HexSync} from "@/utils/sha256";
import {
  detectFirmwareFormat,
  firmwareImageFromFile,
  type FirmwareFormat,
  type FirmwareImage,
} from "@/utils/firmwareImage";

type FlashPhase = 'idle' | 'flashing' | 'success' | 'error'

const targetInfo = ref<TargetInfo | null>(null);
const detecting = ref(false);
const detectError = ref('');
const fileInputRef = ref<HTMLInputElement | null>(null);
const selectedFile = ref<File | null>(null);
const firmware = shallowRef<FirmwareImage | null>(null);
const imageBytes = shallowRef<Uint8Array | null>(null);
const detectedFormat = ref<FirmwareFormat | null>(null);
const convertError = ref('');
const converting = ref(false);
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

/** 未检测到目标芯片时的转换默认值（STM32 典型 Flash），检测成功后以设备返回为准。 */
const DEFAULT_FLASH_BASE = 0x08000000;
const DEFAULT_FLASH_SIZE = 0x00100000;

const flashing = computed(() => phase.value === 'flashing');
const canFlash = computed(() => {
  return targetInfo.value?.detected === true
      && targetInfo.value?.supported !== false
      && firmware.value !== null
      && imageBytes.value !== null
      && fileSha256.value.length > 0
      && !hashing.value
      && !converting.value
      && !flashing.value;
});
const flashUsageText = computed(() => {
  const current = firmware.value;
  if (!current) {
    return '-';
  }
  const flashSize = targetInfo.value?.flash_size;
  if (typeof flashSize !== 'number' || flashSize <= 0) {
    return `${formatBytes(current.image.length)} (${current.image.length} B)`;
  }
  const percent = (current.image.length / flashSize) * 100;
  return `${formatBytes(current.image.length)} / ${formatBytes(flashSize)} (${percent.toFixed(1)} %)`;
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

function formatLabel(format: FirmwareFormat): string {
  switch (format) {
    case 'bin':
      return translate('target.formatBin');
    case 'elf':
      return translate('target.formatElf');
    case 'hex':
      return translate('target.formatHex');
    case 'srec':
      return translate('target.formatSrec');
    default:
      return '-';
  }
}

/** 转换使用的目标 Flash 参数（优先设备 /api/target/info 返回值）。 */
function flashParams(): {base: number; size: number} {
  const base = targetInfo.value?.flash_base;
  const size = targetInfo.value?.flash_size;
  return {
    base: typeof base === 'number' && base > 0 ? base : DEFAULT_FLASH_BASE,
    size: typeof size === 'number' && size > 0 ? size : DEFAULT_FLASH_SIZE,
  };
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
  // Flash 基地址/容量可能因芯片而异：检测成功后按设备参数重新转换已选固件。
  if (selectedFile.value && !flashing.value) {
    await convertSelectedFile(selectedFile.value);
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
  selectedFile.value = file;
  firmware.value = null;
  imageBytes.value = null;
  detectedFormat.value = null;
  fileSha256.value = '';
  convertError.value = '';
  if (file.size <= 0) {
    globalNotify(translate('target.emptyFile'), 'warning');
    return;
  }
  await convertSelectedFile(file);
}

/**
 * 把任意支持的固件格式转换为“从 flash_base 开始的裸镜像”，
 * 并对转换后的镜像（而非源文件）计算 SHA-256 / 大小校验。
 */
async function convertSelectedFile(file: File) {
  const params = flashParams();
  converting.value = true;
  convertError.value = '';
  firmware.value = null;
  imageBytes.value = null;
  fileSha256.value = '';
  try {
    const result = await firmwareImageFromFile(file, params.base, params.size);
    firmware.value = result;
    imageBytes.value = result.image;
    detectedFormat.value = result.format;
    hashing.value = true;
    fileSha256.value = sha256HexSync(result.image);
  } catch (error) {
    detectedFormat.value = detectedFormat.value
        ?? detectFirmwareFormat(file.name, null);
    const message = error instanceof Error ? error.message : 'unknown error';
    convertError.value = /unsupported firmware format/.test(message)
        ? translate('target.invalidFile')
        : translate('target.convertFailed', {err: message});
    globalNotify(convertError.value, 'warning');
    if (import.meta.env.VITE_APP_MODE === 'dev') {
      console.warn('[target] firmware conversion failed', error);
    }
  } finally {
    hashing.value = false;
    converting.value = false;
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
  if (!firmware.value || !imageBytes.value) {
    globalNotify(translate('target.selectFirst'), 'warning');
    return;
  }
  if (!fileSha256.value) {
    globalNotify(translate('common.failed'), 'error');
    return;
  }
  const sizeText = formatBytes(imageBytes.value.length);
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
  // 设备端只接收裸镜像：发送转换后的 image（不是源 ELF/HEX 文件）。
  const payload = new Blob([imageBytes.value as BlobPart], {type: 'application/octet-stream'});
  flashHandle = flashTarget(payload, fileSha256.value, {
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
