import '@/assets/tailwind.css'
import '@/assets/toggle_skewed.css'
import '@/assets/page.css'
import '@/assets/navigation.css'
// Global page defaults previously supplied by Vuetify's stylesheet (now removed).
import '@/assets/base-globals.css'
// Full Element Plus stylesheet (kept as-is; the on-demand per-component variant
// was evaluated but is not adopted).
import 'element-plus/dist/index.css';
// Element Plus dark theme variables (html.dark). Kept before design-tokens.css
// so the token file can override the shell-relevant values.
import 'element-plus/theme-chalk/dark/css-vars.css';
// WiFi Dap design tokens + app shell base styles. MUST stay last.
import '@/assets/design-tokens.css';

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

/**
 * Theme bootstrap (dark-first). The static `class="dark"` on <html> covers the
 * very first paint; this applies the user's persisted choice before mount so
 * the light theme does not flash dark. Keep the storage key in sync with
 * src/views/navigation/NavBar.vue (the toggle lives there).
 */
const THEME_STORAGE_KEY = 'wifi-dap.theme'
const theme: 'dark' | 'light' = (() => {
  try {
    return localStorage.getItem(THEME_STORAGE_KEY) === 'light' ? 'light' : 'dark'
  } catch {
    // localStorage 不可用（隐私模式）时使用默认深色
    return 'dark'
  }
})()

document.documentElement.classList.toggle('dark', theme === 'dark')
document.documentElement.dataset.theme = theme
document.querySelector('meta[name="theme-color"]')
  ?.setAttribute('content', theme === 'dark' ? '#0a0b0d' : '#f4f5f7')

const app = createApp(App)

app.use(createPinia())
app.use(i18n);
app.use(router)
// Provides the defaults context required by VVirtualScroll (only Vuetify
// component used, see text-data-viewer/textDataViewer.vue).
app.use(createVuetify())

app.mount('#app')
