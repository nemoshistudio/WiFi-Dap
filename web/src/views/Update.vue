<template>
  <div class="text-layout description-style">
    <h1 class="page-title">{{ translate('page.update') }}</h1>
    <p class="text-center text-xs opacity-70">{{ translate('protocol.refreshHint') }}</p>

    <el-descriptions class="mt-4" :title="translate('protocol.title')" border :column="1">
      <el-descriptions-item :label="translate('protocol.firmwareVersion')">
        {{ currentRunningVersion }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('ota.upload.title')">
        <el-tag :type="otaEnabled ? 'success' : 'info'" size="small">
          {{ otaEnabled ? translate('common.yes') : translate('common.no') }}
        </el-tag>
      </el-descriptions-item>
      <el-descriptions-item :label="translate('ota.upload.current')">
        {{ store.info.running }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('ota.upload.slotFree')">
        {{ formatBytes(store.info.free) }} / {{ formatBytes(store.info.slot_size) }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.version')">
        <el-tag :type="protocolTagType" size="small">{{ protocolStateText }}</el-tag>
        <span class="ml-2">{{ protocolVersionText }}</span>
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.webVersion')">
        {{ webInfo.webVersion }} ({{ webInfo.gitTag }})
      </el-descriptions-item>
      <el-descriptions-item :label="translate('about.hardwareVersion')">
        {{ sysStore.hwInfo.ver }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('serialNumber')">
        {{ sysStore.sysInfo.sn }}
      </el-descriptions-item>
    </el-descriptions>

    <el-divider></el-divider>

    <el-alert
        v-if="!otaEnabled || !store.otaSupported"
        class="mt-2"
        type="warning"
        :title="translate('common.unsupported')"
        :closable="false"
        show-icon
    />

    <template v-if="otaEnabled && store.otaSupported">
    <h2 class="text-lg font-bold">{{ translate('ota.upload.title') }}</h2>
    <p class="text-sm text-gray-600 mt-1">{{ translate('ota.upload.description') }}</p>

    <el-alert
        v-if="protocolMismatch"
        class="mt-2"
        type="error"
        :title="translate('protocol.mismatchOtaDisabled')"
        :closable="false"
        show-icon
    />

    <div
        class="mt-3 border-2 border-dashed rounded-md p-6 text-center cursor-pointer hover:border-blue-400"
        :class="{ 'opacity-50 cursor-not-allowed': !canSelectFile }"
        @click="openFileDialog"
        @dragover.prevent
        @drop.prevent="onFileDrop"
    >
      <input ref="fileInputRef" type="file" accept=".bin,application/octet-stream" class="hidden"
             @change="onFileInputChange"/>
      <p class="text-sm text-gray-600">{{ translate('ota.upload.dragHint') }}</p>
      <el-button type="primary" size="small" class="mt-2" :disabled="!canSelectFile">
        {{ translate('ota.upload.selectFile') }}
      </el-button>
    </div>

    <el-descriptions v-if="store.selectedFile" class="mt-3" border :column="1">
      <el-descriptions-item :label="translate('ota.upload.fileName')">
        {{ store.selectedFile.name }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('ota.upload.fileSize')">
        {{ formatBytes(store.selectedFile.size) }}
      </el-descriptions-item>
      <el-descriptions-item v-if="store.selectedFile.image.version" :label="translate('ota.upload.fileVersion')">
        {{ store.selectedFile.image.version }}
      </el-descriptions-item>
      <el-descriptions-item v-if="store.selectedFile.image.buildDate" :label="translate('ota.upload.fileDate')">
        {{ store.selectedFile.image.buildDate }} {{ store.selectedFile.image.buildTime }}
      </el-descriptions-item>
      <el-descriptions-item v-if="store.deviceSha256" :label="translate('ota.upload.sha256')">
        <span class="font-mono text-xs break-all">{{ store.deviceSha256 }}</span>
      </el-descriptions-item>
    </el-descriptions>

    <div class="flex flex-wrap gap-2 mt-3 justify-center">
      <el-button
          type="primary"
          :loading="store.phase === 'uploading'"
          :disabled="!canStartUpload"
          @click="startUpload"
      >
        {{ store.phase === 'uploading' ? translate('ota.upload.uploading') : translate('ota.upload.upload') }}
      </el-button>
      <el-button v-if="canCancel" type="danger" plain @click="cancelUpload">
        {{ translate('ota.upload.cancel') }}
      </el-button>
      <el-button v-if="store.phase === 'error'" @click="resetForRetry">
        {{ translate('common.retry') }}
      </el-button>
    </div>

    <div v-if="showProgress" class="mt-4">
      <div class="flex justify-between text-sm">
        <span>{{ translate('ota.upload.uploading') }}</span>
        <span>{{ store.browserPercent.toFixed(1) }} %</span>
      </div>
      <el-progress :percentage="store.browserPercent" :status="barStatus()"/>

      <div class="flex justify-between text-sm mt-3">
        <span>{{ translate('ota.upload.progress') }}</span>
        <span>{{ store.devicePercent.toFixed(1) }} %</span>
      </div>
      <el-progress :percentage="store.devicePercent" :status="barStatus()"/>

      <div class="grid grid-cols-2 gap-2 mt-2 text-xs text-gray-600">
        <div>{{ translate('ota.upload.speed') }}: {{ speedText }}</div>
        <div>{{ translate('ota.upload.eta') }}: {{ etaText }}</div>
        <div>{{ formatBytes(store.progress.offset) }} / {{ formatBytes(store.progress.total) }}</div>
        <div>
          <el-tag size="small">{{ deviceStatusText }}</el-tag>
        </div>
      </div>
    </div>

    <el-alert
        v-if="store.phase === 'verifying'"
        class="mt-3"
        type="info"
        :title="translate('ota.upload.verifying')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="store.phase === 'ready'"
        class="mt-3"
        type="success"
        :title="translate('ota.upload.ready')"
        :description="translate('ota.upload.rebootNote')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="store.phase === 'applying'"
        class="mt-3"
        type="success"
        :title="translate('ota.upload.success')"
        :description="translate('ota.upload.rebootNote')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="store.phase === 'success'"
        class="mt-3"
        type="success"
        :title="translate('ota.upload.success')"
        :description="translate('protocol.refreshHint')"
        :closable="false"
        show-icon
    />
    <el-alert
        v-if="store.phase === 'error'"
        class="mt-3"
        type="error"
        :title="store.errorMessage || translate('common.failed')"
        :description="store.errorDetail"
        :closable="false"
        show-icon
    />
    <el-alert
        v-else-if="protocolMismatch && store.hasSelectedFile"
        class="mt-3"
        type="warning"
        :title="translate('protocol.mismatchOtaDisabled')"
        :closable="false"
        show-icon
    />
    </template>

    <el-divider></el-divider>

    <div class="flex justify-center">
      <el-button type="warning" :disabled="sysStore.rebootInProgress" @click="doReboot">
        {{ sysStore.rebootInProgress ? translate('system.rebooting') : translate('system.reboot') }}
      </el-button>
    </div>
  </div>
</template>

<script setup lang="ts">
import {computed, onMounted, onUnmounted, ref, watch} from "vue";
import {ElMessageBox} from "element-plus";
import {useSystemStore} from "@/stores/useSystemStore";
import {useUpdateStore} from "@/stores/useUpdateStore";
import {
    OTA_DEFAULT_SLOT_SIZE,
    OTA_IMAGE_MIN_SIZE,
    OtaStatus,
    parseEspAppImage,
    wt_ota_abort,
    wt_ota_get_info,
    wt_ota_get_progress,
    wt_ota_upload,
    type IOtaHttpError,
    type IOtaUploadHandle,
} from "@/api/apiOTA";
import {wt_sys_get_fm_info, wt_sys_get_sys_info, wt_sys_reboot} from "@/api/apiSystem";
import {translate} from "@/locales";
import {PROTOCOL_VERSION} from "@/api";
import {isOTAEnabled} from "@/composables/buildMode";
import {globalNotify} from "@/composables/notification";

const sysStore = useSystemStore();
const store = useUpdateStore();
/** Wave2 构建开关：OTA 上传 UI 是否随本次构建启用（默认 false，联调时 VITE_ENABLE_OTA=true） */
const otaEnabled = isOTAEnabled();
const webInfo = window.__BIRDIEBOT_WEB__;

const fileInputRef = ref<HTMLInputElement | null>(null);
const speedText = ref('-');
const etaText = ref('-');

let selectedRawFile: File | null = null;
let uploadHandle: IOtaUploadHandle | null = null;
let rebootWatchdogId = 0;
let speedSamples: Array<{ time: number; loaded: number }> = [];

/** 设备返回的状态/错误为固定英文枚举，仅用于调试展示（契约 §9.2） */
const protocolStateText = computed(() => {
    switch (sysStore.protocolState) {
        case 'ok':
            return translate('protocol.match');
        case 'mismatch':
            return translate('protocol.mismatchTitle');
        case 'legacy':
            return translate('protocol.mismatchLegacy');
        default:
            return translate('protocol.unknownVersion');
    }
});

const protocolTagType = computed(() => {
    switch (sysStore.protocolState) {
        case 'ok':
            return 'success';
        case 'mismatch':
            return 'danger';
        default:
            return 'warning';
    }
});

const protocolVersionText = computed(() => {
    const fw = sysStore.fwProtocolVersion === null ? '?' : `v${sysStore.fwProtocolVersion}`;
    return `${fw} / v${PROTOCOL_VERSION}`;
});

const currentRunningVersion = computed(() => {
    return store.info.running_ver && store.info.running_ver !== '-'
        ? store.info.running_ver
        : sysStore.curFmInfo.ver;
});

const protocolMismatch = computed(() => sysStore.protocolState !== 'ok');
const canSelectFile = computed(() => !store.isBusy);
const canCancel = computed(() => store.phase === 'uploading');
const canStartUpload = computed(() => {
    return store.hasSelectedFile
        && !store.isBusy
        && !protocolMismatch.value
        && store.otaSupported
        && store.info.slot_size > 0;
});
const showProgress = computed(() => {
    return store.phase === 'uploading'
        || store.phase === 'verifying'
        || store.phase === 'ready'
        || store.phase === 'applying'
        || store.phase === 'success';
});

const deviceStatusText = computed(() => {
    const status = store.progress.status !== OtaStatus.IDLE ? store.progress.status : store.info.status;
    switch (status) {
        case OtaStatus.RECEIVING:
            return translate('ota.status.receiving');
        case OtaStatus.VERIFYING:
            return translate('ota.status.verifying');
        case OtaStatus.READY:
            return translate('ota.status.ready');
        case OtaStatus.APPLYING:
            return translate('ota.status.applying');
        case OtaStatus.ERROR:
            return translate('ota.status.error');
        default:
            return translate('ota.status.idle');
    }
});

function barStatus(): 'success' | 'exception' | 'warning' | undefined {
    switch (store.progressBarStatus) {
        case 'success':
            return 'success';
        case 'exception':
            return 'exception';
        case 'warning':
            return 'warning';
        default:
            return undefined;
    }
}

function formatBytes(bytes: number): string {
    if (!bytes || bytes < 0) {
        return '0 B';
    }
    const units = ['B', 'KB', 'MB', 'GB'];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit < units.length - 1) {
        value /= 1024;
        unit += 1;
    }
    const fixed = unit === 0 ? value.toFixed(0) : value.toFixed(2);
    return `${fixed} ${units[unit]}`;
}

function formatDuration(seconds: number): string {
    if (!Number.isFinite(seconds) || seconds <= 0) {
        return '-';
    }
    if (seconds < 60) {
        return `${Math.ceil(seconds)}s`;
    }
    const minutes = Math.floor(seconds / 60);
    const rest = Math.ceil(seconds % 60);
    return `${minutes}m ${rest}s`;
}

function normalizeVersion(version: string): string {
    const match = /^v?(\d+\.\d+\.\d+)/.exec((version || '').trim());
    return match ? match[1] : '';
}

function compareSemver(a: string, b: string): number {
    const left = a.split('.').map((part) => Number.parseInt(part, 10) || 0);
    const right = b.split('.').map((part) => Number.parseInt(part, 10) || 0);
    for (let i = 0; i < Math.max(left.length, right.length); i += 1) {
        const diff = (left[i] ?? 0) - (right[i] ?? 0);
        if (diff !== 0) {
            return diff > 0 ? 1 : -1;
        }
    }
    return 0;
}

function confirmDialog(message: string, title: string): Promise<boolean> {
    return ElMessageBox.confirm(message, title, {
        confirmButtonText: translate('common.confirm'),
        cancelButtonText: translate('common.cancel'),
        type: 'warning',
    }).then(() => true).catch(() => false);
}

function openFileDialog() {
    if (canSelectFile.value) {
        fileInputRef.value?.click();
    }
}

function onFileInputChange(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    input.value = '';
    if (file) {
        void handleFile(file);
    }
}

function onFileDrop(event: DragEvent) {
    if (!canSelectFile.value) {
        return;
    }
    const file = event.dataTransfer?.files?.[0];
    if (file) {
        void handleFile(file);
    }
}

async function handleFile(file: File) {
    selectedRawFile = null;
    store.resetUpload();
    if (!file.name.toLowerCase().endsWith('.bin')) {
        store.failUpload(translate('ota.upload.fileInvalid'), file.name);
        return;
    }
    const slotSize = store.info.slot_size > 0 ? store.info.slot_size : OTA_DEFAULT_SLOT_SIZE;
    if (file.size < OTA_IMAGE_MIN_SIZE) {
        store.failUpload(translate('ota.upload.fileInvalid'), `size=${file.size}`);
        return;
    }
    if (file.size > slotSize) {
        store.failUpload(translate('ota.upload.tooLarge', {size: formatBytes(slotSize)}), `size=${file.size}`);
        return;
    }
    const image = await parseEspAppImage(file);
    if (!image.valid) {
        store.failUpload(translate('ota.upload.fileInvalid'), 'bad ESP application image');
        return;
    }
    const currentVersion = normalizeVersion(currentRunningVersion.value);
    const fileVersion = normalizeVersion(image.version);
    if (fileVersion && currentVersion) {
        const comparison = compareSemver(fileVersion, currentVersion);
        if (comparison === 0) {
            if (!await confirmDialog(translate('ota.upload.sameVersionWarn'), translate('ota.upload.title'))) {
                return;
            }
        } else if (comparison < 0) {
            if (!await confirmDialog(translate('ota.upload.downgradeWarn'), translate('ota.upload.title'))) {
                return;
            }
        }
    }
    selectedRawFile = file;
    store.setSelectedFile({name: file.name, size: file.size, image});
}

function updateBrowserProgress(loaded: number, total: number) {
    store.setBrowserProgress(loaded, total);
    const now = Date.now();
    speedSamples.push({time: now, loaded});
    while (speedSamples.length > 2 && now - speedSamples[0].time > 5000) {
        speedSamples.shift();
    }
    const first = speedSamples[0];
    const elapsed = (now - first.time) / 1000;
    if (elapsed > 0.2) {
        const speed = (loaded - first.loaded) / elapsed;
        if (speed > 0) {
            speedText.value = `${formatBytes(speed)}/s`;
            etaText.value = total > loaded ? formatDuration((total - loaded) / speed) : '-';
            return;
        }
    }
    speedText.value = '-';
    etaText.value = '-';
}

function describeHttpError(failure: IOtaHttpError): { message: string; detail: string } {
    const slotSize = formatBytes(store.info.slot_size > 0 ? store.info.slot_size : OTA_DEFAULT_SLOT_SIZE);
    const raw = failure.err ?? (failure.httpStatus > 0 ? `HTTP ${failure.httpStatus}` : 'network error');
    switch (failure.httpStatus) {
        case 413:
            return {message: translate('ota.upload.tooLarge', {size: slotSize}), detail: raw};
        case 415:
            return {message: translate('ota.upload.fileInvalid'), detail: raw};
        default:
            return {message: translate('ota.upload.failed', {err: raw}), detail: raw};
    }
}

function startRebootWatchdog() {
    window.clearTimeout(rebootWatchdogId);
    // 设备回复 HTTP 200 后 1s 重启；等待重连后 GET_INFO 的 running_ver 变化（契约 §7.2-5）
    rebootWatchdogId = window.setTimeout(() => {
        if (store.phase === 'applying' || store.phase === 'ready') {
            store.failUpload(
                translate('ota.upload.failed', {err: translate('protocol.unknownVersion')}),
                'running_ver unchanged after reboot',
            );
        }
    }, 90000);
}

function handleUploadFailure(failure: IOtaHttpError) {
    if (failure.err === 'aborted') {
        return;
    }
    const described = describeHttpError(failure);
    store.failUpload(described.message, described.detail);
    globalNotify(described.message, 'error');
}

function startUpload() {
    if (!canStartUpload.value || !selectedRawFile) {
        return;
    }
    const preUploadVersion = currentRunningVersion.value;
    speedSamples = [];
    speedText.value = '-';
    etaText.value = '-';
    store.beginUpload(preUploadVersion);
    uploadHandle = wt_ota_upload(selectedRawFile, {
        onUploadProgress: updateBrowserProgress,
        onSuccess: (result) => {
            uploadHandle = null;
            store.onUploadSuccess(result);
            globalNotify(translate('ota.upload.ready'), 'success');
            startRebootWatchdog();
        },
        onFailure: (failure) => {
            uploadHandle = null;
            handleUploadFailure(failure);
        },
    });
}

async function cancelUpload() {
    if (!canCancel.value || !uploadHandle) {
        const confirmed = await confirmDialog(translate('ota.upload.cancelConfirm'), translate('ota.upload.cancel'));
        if (confirmed) {
            // 固件仅允许 RECEIVING 状态 ABORT，其它状态会返回 code=5 invalid state
            wt_ota_abort();
        }
        return;
    }
    const confirmed = await confirmDialog(translate('ota.upload.cancelConfirm'), translate('ota.upload.cancel'));
    if (!confirmed) {
        return;
    }
    const handle = uploadHandle;
    uploadHandle = null;
    handle?.abort();
    wt_ota_abort();
    store.markAborted();
    globalNotify(translate('ota.upload.aborted'), 'info');
}

function resetForRetry() {
    window.clearTimeout(rebootWatchdogId);
    store.clearError();
    if (store.selectedFile) {
        // 保留已选文件，直接允许重试上传
        store.setSelectedFile(store.selectedFile);
    } else {
        store.resetUpload();
        selectedRawFile = null;
    }
}

function doReboot() {
    wt_sys_reboot();
}

function refreshDeviceState() {
    wt_ota_get_info();
    wt_ota_get_progress();
    wt_sys_get_fm_info();
    wt_sys_get_sys_info();
}

watch(() => store.phase, (phase) => {
    if (phase === 'success' || phase === 'error') {
        window.clearTimeout(rebootWatchdogId);
    }
});

onMounted(() => {
    refreshDeviceState();
});

onUnmounted(() => {
    window.clearTimeout(rebootWatchdogId);
    store.clearProgressInterval();
    speedSamples = [];
    // 上传由 XHR 独立进行，页面卸载不主动中止（设备侧有 10s 空闲/300s 上限保护）
});
</script>

<style scoped>
.description-style :deep(.el-descriptions__label) {
  @apply w-32
}
</style>
