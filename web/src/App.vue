<script setup lang="ts">
import { useWsStore } from '@/stores/websocket'
import type { IWebsocketService } from '@/composables/websocket/websocketService'
import { getWebsocketService } from '@/composables/websocket/websocketService'
import { onMounted } from 'vue'
import { changeFavicon } from '@/composables/importFavicon'
import { logHelloMessage } from '@/composables/logConsoleMsg'
import NavBar from '@/views/navigation/NavBar.vue'
import type { ControlMsg, ServerMsg } from '@/api'
import { ControlEvent, ControlMsgType } from '@/api'
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

const wsState = useWsStore()

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
  // OTA flag stays false in Wave 1; WP-207 enables it with the upload flow.
  if (isOTAEnabled()) {
    useUpdateModule()
  }
}

let websocketService: IWebsocketService
onMounted(() => {
  logHelloMessage()

  registerModules()

  // Production always talks to the device that served this page. The dev
  // override exists only for `npm run dev` against a physical device.
  const host = isDevMode()
    ? import.meta.env.VITE_DEVICE_HOST_NAME || 'dap.local'
    : window.location.host
  if (import.meta.env.VITE_DISABLE_CONNECTION !== 'true') {
    websocketService = getWebsocketService()
    websocketService.init(host, onServerMsg, onClientCtrl)
    websocketService.getSocketStatus()
  }
  changeFavicon()
})
</script>

<template>
  <div class="flex flex-col wt-h-100">
    <header>
      <nav-bar />
    </header>
    <RouterView />
  </div>
</template>

<style>
.wt-h-100 {
  height: 100vh;
}
</style>
