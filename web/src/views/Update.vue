<template>
  <div class="text-layout description-style">
    <h1 class="page-title">{{ translate('page.update') }}</h1>
    <p class="text-center text-xs opacity-70">{{ translate('protocol.refreshHint') }}</p>
    <el-divider></el-divider>

    <el-descriptions :title="translate('protocol.title')" border :column="1">
      <el-descriptions-item :label="translate('protocol.firmwareVersion')">
        {{ sysStore.curFmInfo.ver }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.version')">
        <el-tag :type="protocolTagType" size="small">{{ protocolStateText }}</el-tag>
        <span class="ml-2">{{ formatProtocolVersion }}</span>
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.webVersion')">
        {{ webInfo.webVersion }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.buildTag')">
        {{ webInfo.gitTag }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.buildTime')">
        {{ webInfo.buildTime }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('about.hardwareVersion')">
        {{ sysStore.hwInfo.ver }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('about.manufactureDate')">
        {{ sysStore.hwInfo.date }}
      </el-descriptions-item>
      <el-descriptions-item :label="translate('serialNumber')">
        {{ sysStore.sysInfo.sn }}
      </el-descriptions-item>
    </el-descriptions>

    <el-divider></el-divider>

    <div class="flex justify-center">
      <el-button type="warning" :disabled="sysStore.rebootInProgress" @click="doReboot">
        {{ sysStore.rebootInProgress ? translate('system.rebooting') : translate('system.reboot') }}
      </el-button>
    </div>
  </div>
</template>

<script setup lang="ts">
import {computed, onMounted} from "vue";
import {useSystemStore} from "@/stores/useSystemStore";
import {wt_sys_get_fm_info, wt_sys_get_sys_info, wt_sys_reboot} from "@/api/apiSystem";
import {translate} from "@/locales";
import {PROTOCOL_VERSION} from "@/api";

const sysStore = useSystemStore();
const webInfo = window.__BIRDIEBOT_WEB__;

/**
 * Wave 1：OTA flag 固定 false，本页由路由隐藏（不可达），仅保留固件/协议只读信息
 * 与重启入口；浏览器的固件上传 UI 属 Wave 2 的 WP-207。
 */
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

const formatProtocolVersion = computed(() => {
    if (sysStore.fwProtocolVersion === null) {
        return '?';
    }
    return `v${sysStore.fwProtocolVersion} / v${PROTOCOL_VERSION}`;
});

function doReboot() {
    wt_sys_reboot();
}

onMounted(() => {
    wt_sys_get_fm_info();
    wt_sys_get_sys_info();
});
</script>

<style scoped>
.description-style :deep(.el-descriptions__label) {
  @apply w-32
}
</style>
