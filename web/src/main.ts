import '@/assets/tailwind.css'
import '@/assets/toggle_skewed.css'
import '@/assets/page.css'
import '@/assets/navigation.css'
// Global page defaults previously supplied by Vuetify's stylesheet (now removed).
import '@/assets/base-globals.css'
// Full Element Plus stylesheet (kept as-is; the on-demand per-component variant
// was evaluated but is not adopted).
import 'element-plus/dist/index.css';

// Vuetify runtime is kept only because TextDataViewer's <v-virtual-scroll>
// (text-data-viewer page) relies on Vuetify's defaults context. The global
// `vuetify/styles` stylesheet is intentionally NOT imported: the single
// component used pulls in its own CSS, and no other Vuetify components/themes
// are used anywhere in the app.
import { createVuetify } from 'vuetify'

import { createApp } from 'vue'
import { createPinia } from 'pinia'
import i18n from '@/i18n';

import App from './App.vue'
import router from './router'
import { PROTOCOL_VERSION } from '@/api'

declare global {
  interface Window {
    /** Read-only build/runtime metadata (docs/versioning.md §4.2). */
    __BIRDIEBOT_WEB__: {
      webVersion: string
      gitTag: string
      buildTime: string
      protocolVersion: number
    }
  }
}

// Exposed before mount so About/automation can read it even if a page fails.
window.__BIRDIEBOT_WEB__ = Object.freeze({
  webVersion: import.meta.env.VITE_APP_WEB_VERSION || '0.0.0',
  gitTag: import.meta.env.VITE_APP_GIT_TAG || 'v1.0.0-unknown',
  buildTime: import.meta.env.VITE_APP_LAST_COMMIT || 'unknown',
  protocolVersion: PROTOCOL_VERSION,
})

if (import.meta.env.VITE_APP_MODE === 'dev') {
  console.info('[WiFi Dap]', window.__BIRDIEBOT_WEB__)
}

const app = createApp(App)

app.use(createPinia())
app.use(i18n);
app.use(router)
// Provides the defaults context required by VVirtualScroll (only Vuetify
// component used, see text-data-viewer/textDataViewer.vue).
app.use(createVuetify())

app.mount('#app')
