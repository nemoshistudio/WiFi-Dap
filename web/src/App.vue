<script setup lang="ts">
import { useWsStore } from '@/stores/websocket'
import { useSystemStore } from '@/stores/useSystemStore'
import type { IWebsocketService } from '@/composables/websocket/websocketService'
import { getWebsocketService } from '@/composables/websocket/websocketService'
import { computed, onMounted, watch } from 'vue'
import { changeFavicon } from '@/composables/importFavicon'
import { logHelloMessage } from '@/composables/logConsoleMsg'
import NavBar from '@/views/navigation/NavBar.vue'
import AuthDialog from '@/components/AuthDialog.vue'
import type { ControlMsg, ServerMsg } from '@/api'
import { ControlEvent, ControlMsgType, PROTOCOL_VERSION } from '@/api'
import { routeCtrlMsg, routeModuleServerMsg } from '@/router/msgRouter'
import { globalNotify } from '@/composables/notification'
import {
  isDataFlowEnabled,
  isDevMode,
  isOTAEnabled,
  isUartEnabled,
  isWidgetEnabled
} from '@/composables/buildMode'
import { useSystemModule } from '@/composables/useSystemModule'
import { useDataFlowModule } from '@/composables/useDataFlowModule'
import { useUartModule } from '@/composables/useUartModule'
import { useUpdateModule } from '@/composables/useUpdateModule'
import { translate } from '@/locales'
import { authState } from '@/composables/useAuthSession'

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
  // The widget panel has no JSON module of its own; it consumes UART data.
  if (isUartEnabled() || isWidgetEnabled()) {
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
  // auth.ts 的 resolveAuthToken() 在 init 内读取 ?token=/localStorage，
  // token 变更后再次 init 会让 wrapper 以新 token 重建连接。
  websocketService.init(deviceHost(), onServerMsg, onClientCtrl)
  websocketService.getSocketStatus()
}

onMounted(() => {
  logHelloMessage()
  registerModules()
  initWebsocket()
  changeFavicon()
})

// token 保存/清除后重连 WS（URL ?token= / 首帧 SYSTEM cmd0 AUTH 由 wrapper 处理）
watch(() => authState.epoch, () => {
  initWebsocket()
})

/**
 * 全局持久提示条（非阻塞）：
 * 1) 未授权（JSON code=7 / HTTP 401/403）优先提示输入 pairing token；
 * 2) 协议版本 mismatch / legacy / unknown 常驻提示（OTA 上传另行禁用）。
 */
const banner = computed<{ type: 'error' | 'warning' | 'info'; title: string; description: string } | null>(() => {
  if (authState.unauthorized) {
    return {
      type: 'warning',
      title: translate('auth.required'),
      description: translate('auth.description'),
    }
  }
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
  <div class="flex flex-col wt-h-100">
    <header>
      <nav-bar />
    </header>
    <el-alert
        v-if="banner"
        class="mx-2 mt-2"
        :type="banner.type"
        :title="banner.title"
        :description="banner.description"
        :closable="false"
        show-icon
    />
    <RouterView />
    <AuthDialog />
  </div>
</template>

<style>
.wt-h-100 {
  height: 100vh;
}
</style>
