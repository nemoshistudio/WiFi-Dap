import '@/assets/tailwind.css'
import '@/assets/toggle_skewed.css'
import '@/assets/page.css'
import '@/assets/navigation.css'
import 'element-plus/dist/index.css';

import 'vuetify/styles'
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
app.use(createVuetify())

app.mount('#app')
