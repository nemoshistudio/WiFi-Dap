<script setup lang="ts">
import { useWsStore } from '@/stores/websocket'
import { useSystemStore } from '@/stores/useSystemStore'
import type { IWebsocketService } from '@/composables/websocket/websocketService'
import { getWebsocketService } from '@/composables/websocket/websocketService'
import { computed, onMounted } from 'vue'
import { changeFavicon } from '@/composables/importFavicon'
import { logHelloMessage } from '@/composables/logConsoleMsg'
import NavBar from '@/views/navigation/NavBar.vue'
import type { ControlMsg, ServerMsg } from '@/api'
import { ControlEvent, ControlMsgType, PROTOCOL_VERSION } from '@/api'
import { routeCtrlMsg, routeModuleServerMsg } from '@/router/msgRouter'
import { globalNotify } from '@/composables/notification'
import {
  isDataFlowEnabled,
  isDevMode,
  isOTAEnabled,
  isUartEnabled
} from '@/composables/buildMode'
import { useSystemModule } from '@/composables/useSystemModule'
import { useDataFlowModule } from '@/composables/useDataFlowModule'
import { useUartModule } from '@/composables/useUartModule'
import { useUpdateModule } from '@/composables/useUpdateModule'
import { translate } from '@/locales'
import CommandPalette from '@/components/command/CommandPalette.vue'
import { ensureDashboardTelemetry } from '@/components/dashboard/dashboardState'
import { ensureGuidedSignalBridge } from '@/components/dashboard/guidedSignals'
import { ensureCommandIndexBridge } from '@/utils/commandIndex'
import { runAfterPaint, runWhenIdle } from '@/utils/idle'

const wsState = useWsStore()
const systemStore = useSystemStore()

const onClientCtrl = (msg: ControlMsg) => {
  if (isDevMode()) {
    console.log('App.vue:', msg)
  }
  if (msg.type === ControlMsgType.WS_EVENT) {
    wsState.$patch({ state: msg.data as ControlEvent })
    routeCtrlMsg(msg)
    if (msg.data === ControlEvent.CONNECTED) {
      globalNotify(translate('common.debuggerConnected'), 'success')
    }
  }
}

const onServerMsg = (msg: ServerMsg) => {
  if (isDevMode()) {
    console.log('App.vue:', msg)
  }
  routeModuleServerMsg(msg)
}

/**
 * Register WS modules before opening the socket, gated by buildMode so a
 * trimmed build can drop features without touching views.
 * Trial mode is removed by contract (docs/i18n-keys.md §10.2).
 */
function registerModules() {
  useSystemModule()
  if (isDataFlowEnabled()) {
    useDataFlowModule()
  }
  if (isUartEnabled()) {
    useUartModule()
  }
  if (isOTAEnabled()) {
    useUpdateModule()
  }
}

function deviceHost(): string {
  // Production always talks to the device that served this page. The dev
  // override exists only for `npm run dev` against a physical device.
  return isDevMode()
    ? import.meta.env.VITE_DEVICE_HOST_NAME || 'dap.local'
    : window.location.host
}

let websocketService: IWebsocketService | null = null

function initWebsocket() {
  if (import.meta.env.VITE_DISABLE_CONNECTION === 'true') {
    return
  }
  websocketService = getWebsocketService()
  websocketService.init(deviceHost(), onServerMsg, onClientCtrl)
  websocketService.getSocketStatus()
}

onMounted(() => {
  // First-screen timing baseline (P1-16): read with
  // performance.getEntriesByType('mark') / Playwright.
  if (typeof performance !== 'undefined' && typeof performance.mark === 'function') {
    performance.mark('bb:app-mounted')
  }
  logHelloMessage()
  // Critical path: register WS modules and open the socket before the first
  // dashboard poll; the handshake/telemetry queries are queued on CONNECTED.
  registerModules()
  initWebsocket()
  // Process-wide dashboard/diagnostics observers (WIFI module + WS state).
  // They are read-only and do not register/unregister any module handler.
  ensureDashboardTelemetry()
  // P1-16: non-first-screen work waits for the first painted frame / idle time.
  runAfterPaint(() => {
    changeFavicon()
    // P1-17/P1-18 bridges: the palette reads the curve page's ELF index; the
    // guide listens for real events (detection / ELF index / sampling / flash).
    ensureCommandIndexBridge()
    ensureGuidedSignalBridge()
  })
  runWhenIdle(() => {
    // Mark the first-screen baseline end (read in Playwright / diagnostics).
    if (typeof performance !== 'undefined' && typeof performance.mark === 'function') {
      performance.mark('bb:first-screen-idle')
    }
  })
})

/**
 * 全局持久提示条（非阻塞）：协议版本 mismatch / legacy / unknown 常驻提示
 * （OTA 上传另行禁用）。
 */
const banner = computed<{ type: 'error' | 'warning' | 'info'; title: string; description: string } | null>(() => {
  const fwProtocol = systemStore.fwProtocolVersion === null ? '?' : systemStore.fwProtocolVersion
  switch (systemStore.protocolState) {
    case 'mismatch':
      return {
        type: 'error',
        title: translate('protocol.mismatchTitle'),
        description: `${translate('protocol.mismatchDetail', { web: PROTOCOL_VERSION, fw: fwProtocol })} ${translate('protocol.mismatchOtaDisabled')} ${translate('protocol.refreshHint')}`,
      }
    case 'legacy':
      return {
        type: 'warning',
        title: translate('protocol.mismatchLegacy'),
        description: translate('protocol.refreshHint'),
      }
    case 'unknown':
      // 仅在手握已结算（超时/请求失败/未上报版本）后提示，避免握手窗口误报
      if (systemStore.handshakeSettled && wsState.state === ControlEvent.CONNECTED) {
        return {
          type: 'info',
          title: translate('protocol.unknownVersion'),
          description: translate('protocol.refreshHint'),
        }
      }
      return null
    default:
      return null
  }
})
</script>

<template>
  <!--
    App shell: dark-first debug workbench chrome.
    Layout/colors live in assets/design-tokens.css (`.app-shell`, `.app-header`,
    `.app-banner`, `.app-main`) so pages can reuse the same tokens.
  -->
  <div class="app-shell">
    <header class="app-header">
      <nav-bar />
    </header>
    <el-alert
        v-if="banner"
        class="app-banner"
        :type="banner.type"
        :title="banner.title"
        :description="banner.description"
        :closable="false"
        show-icon
    />
    <main id="main-content" class="app-main">
      <RouterView />
    </main>
    <!-- Global Ctrl+K command palette (pages + actions + recents). -->
    <CommandPalette />
  </div>
</template>
