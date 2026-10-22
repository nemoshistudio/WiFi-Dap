<script setup lang="ts">
import {computed} from "vue";
import {useSystemStore} from "@/stores/useSystemStore";
import {translate} from "@/locales";
import {globalNotify} from "@/composables/notification";
import {PROTOCOL_VERSION} from "@/api";
import {BRAND_MARK_64} from "@/assets/brand/brand-assets";
import {BRAND_NAME} from "@/constants/brand";

const sysStore = useSystemStore();
const webInfo = window.__BIRDIEBOT_WEB__;

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

function copyVersionInfo() {
    const text = [
        `web_version=${webInfo.webVersion}`,
        `protocol_version=${PROTOCOL_VERSION}`,
        `git_tag=${webInfo.gitTag}`,
        `build_time=${webInfo.buildTime}`,
        `firmware_version=${sysStore.curFmInfo.ver}`,
        `firmware_date=${sysStore.curFmInfo.date}`,
        `serial_number=${sysStore.sysInfo.sn}`,
        `hw_version=${sysStore.hwInfo.ver}`,
        `mf_date=${sysStore.hwInfo.date}`,
    ].join('\n');
    // 设备以 HTTP 明文提供页面（非 secure context），navigator.clipboard 可能不存在；
    // 此时只提示失败，不抛异常。
    const clipboard = navigator.clipboard;
    if (!clipboard) {
        globalNotify(translate('common.failed'), 'error');
        return;
    }
    clipboard.writeText(text)
        .then(() => globalNotify(translate('common.copied'), 'success'))
        .catch(() => globalNotify(translate('common.failed'), 'error'));
}
</script>

<template>
  <div class="text-layout bb-page">
    <section class="bb-card about-brand">
      <img class="about-brand__mark bb-invert-on-light" :src="BRAND_MARK_64" alt="" width="48" height="48"/>
      <div class="about-brand__text">
        <h1 class="about-brand__name">{{ BRAND_NAME }}</h1>
        <p class="about-brand__meta">{{ translate('page.about') }} · Web UI v{{ webInfo.webVersion }}</p>
      </div>
      <el-tag :type="protocolTagType" size="small" class="about-brand__tag">{{ protocolStateText }}</el-tag>
    </section>

    <section class="bb-card">
    <el-descriptions :title="translate('protocol.title')" border :column="1"
                     class="description-style">
      <template #extra>
        <el-button size="small" @click="copyVersionInfo">{{ translate('about.copyInfo') }}</el-button>
      </template>
      <el-descriptions-item :label="translate('protocol.version')">
        <el-tag :type="protocolTagType" size="small">{{ protocolStateText }}</el-tag>
        <span class="ml-2">{{ protocolVersionText }}</span>
      </el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.webVersion')">{{ webInfo.webVersion }}</el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.buildTag')">{{ webInfo.gitTag }}</el-descriptions-item>
      <el-descriptions-item :label="translate('protocol.buildTime')">{{ webInfo.buildTime }}</el-descriptions-item>
    </el-descriptions>
    </section>

    <section class="bb-card">
    <el-collapse class="about-collapse">
      <el-collapse-item :title="translate('aboutDebugger')">
        <el-descriptions border :column="1" class="mt-4 description-style">
          <el-descriptions-item :label="translate('protocol.firmwareVersion')">{{ sysStore.curFmInfo.ver }}</el-descriptions-item>
          <el-descriptions-item :label="translate('releaseTime')">{{ sysStore.curFmInfo.date }}</el-descriptions-item>
          <el-descriptions-item :label="translate('about.hardwareVersion')">{{ sysStore.hwInfo.ver }}</el-descriptions-item>
          <el-descriptions-item :label="translate('about.manufactureDate')">{{ sysStore.hwInfo.date }}</el-descriptions-item>
          <el-descriptions-item :label="translate('serialNumber')">{{ sysStore.sysInfo.sn }}</el-descriptions-item>
        </el-descriptions>

        <el-descriptions :title="translate('credit')" border :column="1" class="mt-4 description-style">
          <el-descriptions-item label="windowsair/wireless-esp8266-dap">
            <a target="_blank" href="https://github.com/windowsair/wireless-esp8266-dap">https://github.com/windowsair/wireless-esp8266-dap</a>
          </el-descriptions-item>
          <el-descriptions-item label="windowsair/elaphureLink">
            <a target="_blank" href="https://github.com/windowsair/elaphureLink">https://github.com/windowsair/elaphureLink</a>
          </el-descriptions-item>
          <el-descriptions-item label="espressif/esp-idf">
            <a target="_blank" href="https://github.com/espressif/esp-idf">https://github.com/espressif/esp-idf</a>
          </el-descriptions-item>
          <el-descriptions-item label="openocd-org/openocd">
            <a target="_blank" href="https://github.com/openocd-org/openocd">https://github.com/openocd-org/openocd</a>
          </el-descriptions-item>
        </el-descriptions>
      </el-collapse-item>

      <el-collapse-item :title="translate('aboutWebHost')">
        <el-descriptions border :column="1" class="mt-4 description-style">
          <el-descriptions-item :label="translate('version')">{{ webInfo.webVersion }}</el-descriptions-item>
          <el-descriptions-item :label="translate('releaseTime')">{{ webInfo.buildTime }}</el-descriptions-item>
        </el-descriptions>

        <el-descriptions :title="translate('credit')" border :column="1" class="mt-4 description-style">
          <el-descriptions-item label="kerms/wireless-esp32-tools">
            <a target="_blank" href="https://github.com/kerms/wireless-esp32-tools">https://github.com/kerms/wireless-esp32-tools</a>
          </el-descriptions-item>
          <el-descriptions-item label="kerms/wireless-esp32-tools-web-host">
            <a target="_blank" href="https://github.com/kerms/wireless-esp32-tools-web-host">https://github.com/kerms/wireless-esp32-tools-web-host</a>
          </el-descriptions-item>
          <el-descriptions-item label="vuejs"><a target="_blank" href="https://github.com/vuejs/vue/blob/main/LICENSE">MIT</a>
          </el-descriptions-item>
          <el-descriptions-item label="typescript"><a
              href="https://github.com/microsoft/TypeScript/blob/main/LICENSE.txt">Apache 2.0</a></el-descriptions-item>
          <el-descriptions-item label="vite"><a target="_blank" href="https://github.com/vitejs/vite/blob/main/LICENSE">MIT</a>
          </el-descriptions-item>
          <el-descriptions-item label="tailwindcss"><a
              href="https://github.com/tailwindlabs/tailwindcss/blob/master/LICENSE">MIT</a></el-descriptions-item>
          <el-descriptions-item label="element-plus"><a
              href="https://github.com/element-plus/element-plus/blob/dev/LICENSE">MIT</a></el-descriptions-item>
          <el-descriptions-item label="pinia"><a target="_blank" href="https://github.com/vuejs/pinia/blob/v2/LICENSE">MIT</a>
          </el-descriptions-item>
          <el-descriptions-item label="mitt"><a target="_blank" href="https://github.com/developit/mitt/blob/main/LICENSE">MIT</a>
          </el-descriptions-item>
          <el-descriptions-item label="vue-router"><a
              href="https://github.com/vuejs/vue-router/blob/dev/LICENSE">MIT</a></el-descriptions-item>
          <el-descriptions-item label="vue-i18n"><a target="_blank" href="https://github.com/kazupon/vue-i18n?tab=MIT-1-ov-file#readme">MIT</a>
          </el-descriptions-item>
          <el-descriptions-item label="lightningcss"><a
              href="https://github.com/parcel-bundler/lightningcss/blob/master/LICENSE">MPL-2.0 license</a>
          </el-descriptions-item>
        </el-descriptions>
      </el-collapse-item>
    </el-collapse>
    </section>
  </div>
</template>

<style scoped lang="postcss">
.description-style :deep(.el-descriptions__label) {
  @apply w-32
}

.about-brand {
  display: flex;
  align-items: center;
  gap: var(--bb-space-4);
}

.about-brand__mark {
  flex: 0 0 auto;
  width: 48px;
  height: 48px;
}

.about-brand__text {
  flex: 1 1 auto;
  min-width: 0;
}

.about-brand__name {
  margin: 0;
  font-size: var(--bb-font-size-xl);
  font-weight: 650;
  letter-spacing: -0.01em;
  color: var(--bb-text);
}

.about-brand__meta {
  margin: 2px 0 0;
  font-size: var(--bb-font-size-md);
  color: var(--bb-text-muted);
}

.about-brand__tag {
  flex: 0 0 auto;
}

.about-collapse {
  border: 0;
  --el-collapse-border-color: transparent;
  --el-collapse-header-bg-color: transparent;
  --el-collapse-content-bg-color: transparent;
}

@media (max-width: 639px) {
  .description-style :deep(.el-descriptions__label) {
    @apply w-20;
  }

  .about-brand {
    align-items: flex-start;
  }

  .about-brand__tag {
    display: none;
  }
}
</style>
