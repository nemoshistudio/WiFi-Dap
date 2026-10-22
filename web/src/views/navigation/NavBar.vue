<template>
  <div class="bb-nav-host">
    <nav class="bb-nav">
      <!-- Left: menu + brand (logo, product name, current page) -->
      <div class="bb-nav__left">
        <button
            type="button"
            class="bb-icon-btn bb-menu-btn"
            :title="translate('navbar.navigationSidebar')"
            :aria-label="translate('navbar.navigationSidebar')"
            @click.prevent="sideMenuOpen = true"
        >
          <svg class="bb-menu-btn__icon" viewBox="0 0 20 20" aria-hidden="true">
            <path d="M2 4.5h16M2 10h16M2 15.5h16" />
          </svg>
          <el-badge v-if="updateStore.canUpdate" is-dot class="bb-menu-btn__badge"/>
        </button>

        <router-link to="/" class="bb-brand" :title="translate('navbar.getSomeFries')">
          <img
              class="bb-brand__mark"
              :class="{ 'is-inverted': theme === 'light' }"
              :src="BRAND_MARK_64"
              alt=""
              width="28"
              height="28"
          />
          <span class="bb-brand__text">
            <span class="bb-brand__name">{{ BRAND_NAME }}</span>
            <span class="bb-brand__page">{{ currentPageTitle }}</span>
          </span>
        </router-link>

        <div id="nav-right-slot"></div>
      </div>

      <!-- Center: primary routes (desktop); current page pill on small screens -->
      <ul class="bb-nav__links">
        <li v-for="(item, index) in menuItems" :key="index">
          <router-link :to="item.href" class="bb-nav__link">{{ item.name }}</router-link>
        </li>
      </ul>
      <span class="bb-nav__page">{{ currentPageTitle }}</span>

      <!-- Right: page actions + connection + language + theme -->
      <div class="bb-nav__right">
        <div id="page-spec-slot" class="bb-nav__spec"></div>

        <div class="bb-status" :class="statusKind" role="status" :aria-label="wsState" :title="wsState">
          <span class="bb-status__dot" aria-hidden="true"></span>
          <span class="bb-status__text">{{ wsState }}</span>
        </div>

        <div class="bb-nav__lang">
          <el-select v-model="language" class="bb-lang" size="small" @change="handleLanguageChange">
            <el-option
                v-for="item in LANGUAGES"
                :key="item.code"
                :value="item.code"
                :label="item.flag + ' ' + item.label"
            ></el-option>
            <template #label>
              <span class="bb-lang__label">
                <InlineSvg name="translate" class="bb-lang__icon"/>
                <span>{{ languageFlag }}</span>
              </span>
            </template>
          </el-select>
        </div>

        <button
            type="button"
            class="bb-icon-btn bb-theme-btn"
            :title="themeActionLabel"
            :aria-label="themeActionLabel"
            @click="toggleTheme"
        >
          <svg v-if="theme === 'dark'" class="bb-theme-btn__icon" viewBox="0 0 20 20" aria-hidden="true">
            <circle cx="10" cy="10" r="3.6"/>
            <path d="M10 1.6v2.2M10 16.2v2.2M1.6 10h2.2M16.2 10h2.2M3.9 3.9l1.6 1.6M14.5 14.5l1.6 1.6M16.1 3.9l-1.6 1.6M5.5 14.5l-1.6 1.6"/>
          </svg>
          <svg v-else class="bb-theme-btn__icon" viewBox="0 0 20 20" aria-hidden="true">
            <path d="M16.8 12.4A7.4 7.4 0 0 1 7.6 3.2 7.4 7.4 0 1 0 16.8 12.4Z"/>
          </svg>
        </button>
      </div>
    </nav>
  </div>

  <!-- Mobile / compact navigation drawer.
       Teleported to <body>: the sticky/hairline-blurred header creates a
       containing block for fixed-position descendants, which would clip the
       drawer/overlay if it stayed inside <header>. -->
  <Teleport to="body">
  <div :class='["custom-drawer", { open: sideMenuOpen }]'>
    <el-drawer
        v-model="sideMenuOpen"
        class="bb-drawer"
        :with-header="false"
        size="300px"
        direction="ltr"
    >
      <div class="bb-drawer__head">
        <router-link to="/" class="bb-brand bb-brand--drawer" @click="sideMenuOpen = false">
          <img
              class="bb-brand__mark"
              :class="{ 'is-inverted': theme === 'light' }"
              :src="BRAND_MARK_64"
              alt=""
              width="30"
              height="30"
          />
          <span class="bb-brand__text">
            <span class="bb-brand__name">{{ BRAND_NAME }}</span>
            <span class="bb-brand__page">{{ currentPageTitle }}</span>
          </span>
        </router-link>
        <button
            type="button"
            class="bb-icon-btn"
            :title="translate('page.close')"
            :aria-label="translate('page.close')"
            @click="sideMenuOpen = false"
        >
          <InlineSvg name="cross" class="bb-icon-btn__svg"/>
        </button>
      </div>

      <div class="bb-drawer__status">
        <div class="bb-status" :class="statusKind" role="status" :aria-label="wsState">
          <span class="bb-status__dot" aria-hidden="true"></span>
          <span class="bb-status__text">{{ wsState }}</span>
        </div>
      </div>

      <nav class="bb-drawer__nav">
        <ul>
          <li v-for="(item, index) in sideBarItems" :key="index">
            <router-link
                @click="sideMenuOpen = false"
                :title="item.name"
                :to="item.href"
                :class="['bb-drawer__link', item?.class]"
            >
              <span class="bb-drawer__link-text">{{ item.name }}</span>
              <el-badge v-if="item?.badge?.value" is-dot/>
            </router-link>
          </li>
        </ul>
      </nav>

      <template #footer>
        <div class="bb-drawer__footer">
          <el-select v-model="language" class="bb-drawer__lang" size="small" @change="handleLanguageChange">
            <el-option
                v-for="item in LANGUAGES"
                :key="item.code"
                :value="item.code"
                :label="item.flag + ' ' + item.label"
            ></el-option>
            <template #label>
              <span class="bb-lang__label">
                <InlineSvg name="translate" class="bb-lang__icon"/>
                <span>{{ languageFlag }}</span>
              </span>
            </template>
          </el-select>
          <div class="bb-drawer__actions">
            <button
                type="button"
                class="bb-icon-btn"
                :title="isFullscreen ? translate('page.windowed') : translate('page.fullscreen')"
                :aria-label="isFullscreen ? translate('page.windowed') : translate('page.fullscreen')"
                @click="toggle"
            >
              <InlineSvg v-if="!isFullscreen" name="open-in-full" class="bb-icon-btn__svg"/>
              <InlineSvg v-else name="close-fullscreen" class="bb-icon-btn__svg"/>
            </button>
            <button
                type="button"
                class="bb-icon-btn"
                :title="themeActionLabel"
                :aria-label="themeActionLabel"
                @click="toggleTheme"
            >
              <svg v-if="theme === 'dark'" class="bb-theme-btn__icon" viewBox="0 0 20 20" aria-hidden="true">
                <circle cx="10" cy="10" r="3.6"/>
                <path d="M10 1.6v2.2M10 16.2v2.2M1.6 10h2.2M16.2 10h2.2M3.9 3.9l1.6 1.6M14.5 14.5l1.6 1.6M16.1 3.9l-1.6 1.6M5.5 14.5l-1.6 1.6"/>
              </svg>
              <svg v-else class="bb-theme-btn__icon" viewBox="0 0 20 20" aria-hidden="true">
                <path d="M16.8 12.4A7.4 7.4 0 0 1 7.6 3.2 7.4 7.4 0 1 0 16.8 12.4Z"/>
              </svg>
            </button>
          </div>
        </div>
      </template>
    </el-drawer>
  </div>
  </Teleport>
</template>

<script lang="ts" setup>
import InlineSvg from "@/components/InlineSvg.vue";
import { computed, ref, type ComputedRef, type Ref } from "vue";
import { useWsStore } from "@/stores/websocket";
import { translate } from "@/locales";
import { ControlEvent } from "@/api";
import { useRoute } from "vue-router";
import { useFullscreen } from '@vueuse/core'
import { useUpdateStore } from "@/stores/useUpdateStore";
import { isOTAEnabled } from "@/composables/buildMode";
import { BRAND_NAME } from "@/constants/brand";
import { BRAND_MARK_64 } from "@/assets/brand/brand-assets";
import { locale, setLang } from "@/i18n"
import { LANGUAGES, getLanguageOption } from "@/constants/languages"

type Theme = 'dark' | 'light'
/** Keep in sync with the bootstrap snippet in src/main.ts. */
const THEME_STORAGE_KEY = 'birdiebot.theme'

const wsStore = useWsStore();
const updateStore = useUpdateStore();
const { isFullscreen, toggle } = useFullscreen();
const route = useRoute();
const language = ref(locale);

const currentPageTitle = computed(() => {
  const title = route.meta.title;
  return typeof title === 'string' && title.length > 0 ? title : BRAND_NAME;
});

const languageFlag = computed(() => {
  return getLanguageOption(language.value)?.flag ?? '🏳️';
});

function handleLanguageChange(lang: string) {
  setLang(lang);
}

/* -------------------------------------------------------------------------
 * Theme (dark-first; persisted locally, applied to <html class="dark">)
 * ---------------------------------------------------------------------- */
const theme = ref<Theme>(document.documentElement.classList.contains('dark') ? 'dark' : 'light');

const themeActionLabel = computed(() =>
    theme.value === 'dark' ? 'Switch to light theme' : 'Switch to dark theme'
);

function applyTheme(next: Theme) {
  theme.value = next;
  document.documentElement.classList.toggle('dark', next === 'dark');
  document.documentElement.dataset.theme = next;
  try {
    localStorage.setItem(THEME_STORAGE_KEY, next);
  } catch {
    // localStorage 不可用（隐私模式）时仅本次生效
  }
  const themeColor = document.querySelector('meta[name="theme-color"]');
  if (themeColor) {
    themeColor.setAttribute('content', next === 'dark' ? '#0a0b0d' : '#f4f5f7');
  }
}

function toggleTheme() {
  applyTheme(theme.value === 'dark' ? 'light' : 'dark');
}

/* -------------------------------------------------------------------------
 * Drawer / menu / connection status
 * ---------------------------------------------------------------------- */
const sideMenuOpen = ref(false);

const statusKind = computed(() => {
  switch (wsStore.state) {
    case ControlEvent.CONNECTED:
      return 'is-ok';
    case ControlEvent.CONNECTING:
    case ControlEvent.LOADED:
      return 'is-pending';
    default:
      return 'is-off';
  }
});

/**
 * 连接状态 -> i18n key 的显式映射（i18n 契约 §2.3）。
 * 禁止用 translate(wsStore.state.toLocaleLowerCase()) 这类动态 key。
 */
function wsStateText(state: ControlEvent): string {
  switch (state) {
    case ControlEvent.CONNECTED:
      return translate('ws.connected');
    case ControlEvent.CONNECTING:
    case ControlEvent.LOADED:
      return translate('ws.connecting');
    default:
      // DISCONNECTED / BROKEN
      return translate('ws.disconnected');
  }
}

const wsState = computed(() => {
  return wsStateText(wsStore.state);
});

type Item = {
  name: string;
  href: string;
  class?: string;
  badge?: Ref<boolean>;
};

const menuItems: ComputedRef<Item[]> = computed(() => ([
  {
    name: translate("page.uart"),
    href: "/uart",
  }, {
    name: translate("page.wifi"),
    href: "/wifi",
  }, {
    name: translate("target.title"),
    href: "/target",
  }, {
    name: translate("page.debugCurve"),
    href: "/debug-curve",
  },
]));

const sideBarItems: ComputedRef<Item[]> = computed(() => {
  const items: Item[] = [
    {
      name: translate("page.wifi"),
      href: "/wifi",
    }, {
      name: translate("target.title"),
      href: "/target",
    }, {
      name: translate("page.debugCurve"),
      href: "/debug-curve",
    }, {
      name: translate("page.uart"),
      href: "/uart",
    }, {
      name: translate("page.about"),
      href: "/about",
    },
  ];
  if (isOTAEnabled()) {
    items.push({
      name: translate("page.update"),
      href: "/update",
      badge: computed(() => updateStore.canUpdate),
    })
  }
  return items;
});
</script>

<style scoped>
/* ==========================================================================
 * Header navigation (tokens: web/src/assets/design-tokens.css)
 * ======================================================================== */
.bb-nav-host {
  height: 100%;
}

.bb-nav {
  display: flex;
  align-items: center;
  gap: var(--bb-space-2);
  height: 100%;
  padding: 0 var(--bb-space-3);
}

.bb-nav__left {
  display: flex;
  align-items: center;
  gap: var(--bb-space-1);
  min-width: 0;
}

.bb-nav__right {
  display: flex;
  align-items: center;
  gap: var(--bb-space-1);
  margin-left: auto;
  flex: 0 0 auto;
}

/* Buttons ----------------------------------------------------------------- */
.bb-icon-btn {
  position: relative;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  padding: 0;
  border: 1px solid transparent;
  border-radius: var(--bb-radius-sm);
  background: transparent;
  color: var(--bb-nav-link);
  cursor: pointer;
  transition: background-color var(--bb-duration-fast) var(--bb-ease),
    color var(--bb-duration-fast) var(--bb-ease),
    border-color var(--bb-duration-fast) var(--bb-ease);
}

.bb-icon-btn:hover {
  background: var(--bb-nav-link-hover-bg);
  color: var(--bb-text);
}

.bb-icon-btn:active {
  background: var(--bb-nav-link-active-bg);
  color: var(--bb-nav-link-active);
}

.bb-icon-btn:focus-visible {
  outline: none;
  box-shadow: var(--bb-focus-ring);
}

.bb-icon-btn__svg {
  width: 17px;
  height: 17px;
  color: currentColor;
}

.bb-menu-btn__icon {
  width: 17px;
  height: 17px;
  fill: none;
  stroke: currentColor;
  stroke-width: 1.7;
  stroke-linecap: round;
}

.bb-menu-btn__badge {
  position: absolute;
  top: 2px;
  right: 0;
}

/* Brand ------------------------------------------------------------------- */
.bb-brand {
  display: flex;
  align-items: center;
  gap: var(--bb-space-2);
  min-width: 0;
  padding: 4px 6px;
  border-radius: var(--bb-radius-sm);
  color: var(--bb-text);
  text-decoration: none;
  transition: background-color var(--bb-duration-fast) var(--bb-ease);
}

.bb-brand:hover {
  background: var(--bb-nav-link-hover-bg);
}

.bb-brand:focus-visible {
  outline: none;
  box-shadow: var(--bb-focus-ring);
}

.bb-brand__mark {
  flex: 0 0 auto;
  width: 28px;
  height: 28px;
  filter: drop-shadow(0 0 8px rgba(77, 163, 255, 0.24));
}

/* Dark theme: the supplied logo is white. Light theme: invert to near-black. */
.bb-brand__mark.is-inverted {
  filter: invert(1) drop-shadow(0 0 8px rgba(31, 111, 235, 0.18));
}

.bb-brand__text {
  display: flex;
  flex-direction: column;
  justify-content: center;
  min-width: 0;
  line-height: 1.15;
}

.bb-brand__name {
  font-size: var(--bb-font-size-lg);
  font-weight: 650;
  letter-spacing: 0.01em;
  color: var(--bb-text);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.bb-brand__page {
  font-size: var(--bb-font-size-xs);
  font-weight: 500;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--bb-text-muted);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

/* Center route links ------------------------------------------------------ */
.bb-nav__links {
  display: flex;
  flex: 1 1 auto;
  align-items: center;
  justify-content: center;
  gap: 2px;
  min-width: 0;
  margin: 0;
  padding: 0;
  list-style: none;
}

.bb-nav__link {
  display: inline-flex;
  align-items: center;
  padding: 6px 13px;
  border-radius: var(--bb-radius-pill);
  font-size: var(--bb-font-size-md);
  font-weight: 550;
  color: var(--bb-nav-link);
  text-decoration: none;
  white-space: nowrap;
  transition: background-color var(--bb-duration-fast) var(--bb-ease),
    color var(--bb-duration-fast) var(--bb-ease);
}

.bb-nav__link:hover {
  background: var(--bb-nav-link-hover-bg);
  color: var(--bb-text);
}

.bb-nav__link.router-link-active {
  background: var(--bb-nav-link-active-bg);
  color: var(--bb-nav-link-active);
  font-weight: 650;
}

.bb-nav__link:focus-visible {
  outline: none;
  box-shadow: var(--bb-focus-ring);
}

/* Current page pill (mobile replacement for the desktop links) */
.bb-nav__page {
  display: none;
  flex: 1 1 auto;
  min-width: 0;
  font-size: var(--bb-font-size-md);
  font-weight: 600;
  color: var(--bb-text-secondary);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  text-align: center;
  padding: 0 var(--bb-space-2);
}

/* Page-spec slot (Uart teleports its layout control here) ---------------- */
.bb-nav__spec {
  display: flex;
  align-items: center;
  height: 32px;
}

.bb-nav__spec :deep(.el-button) {
  height: 100%;
  margin: 0;
}

.bb-nav__spec :deep(.el-popover__reference) {
  height: 100%;
}

/* Connection status chip -------------------------------------------------- */
.bb-status {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  height: 28px;
  padding: 0 10px;
  border-radius: var(--bb-radius-pill);
  border: 1px solid var(--bb-border);
  background: var(--bb-surface-inset);
  font-size: var(--bb-font-size-sm);
  font-weight: 550;
  color: var(--bb-text-secondary);
  white-space: nowrap;
}

.bb-status__dot {
  width: 7px;
  height: 7px;
  flex: 0 0 auto;
  border-radius: 50%;
  background: var(--bb-text-faint);
}

.bb-status.is-ok {
  border-color: rgba(61, 220, 132, 0.4);
  background: var(--bb-success-soft);
  color: var(--bb-success);
}

.bb-status.is-ok .bb-status__dot {
  background: var(--bb-success);
  box-shadow: 0 0 8px rgba(61, 220, 132, 0.6);
}

.bb-status.is-pending {
  border-color: rgba(251, 191, 36, 0.4);
  background: var(--bb-warning-soft);
  color: var(--bb-warning);
}

.bb-status.is-pending .bb-status__dot {
  background: var(--bb-warning);
  animation: bb-status-pulse 1.4s var(--bb-ease) infinite;
}

.bb-status.is-off {
  color: var(--bb-danger);
  border-color: rgba(255, 107, 107, 0.35);
  background: var(--bb-danger-soft);
}

.bb-status.is-off .bb-status__dot {
  background: var(--bb-danger);
}

@keyframes bb-status-pulse {
  0%,
  100% {
    opacity: 1;
  }
  50% {
    opacity: 0.35;
  }
}

/* Language selector ------------------------------------------------------- */
.bb-nav__lang {
  display: flex;
  align-items: center;
}

.bb-lang {
  width: 78px;
}

.bb-lang :deep(.el-select__wrapper) {
  min-height: 28px;
  padding: 2px 8px;
  border-radius: var(--bb-radius-pill);
  background: transparent;
  box-shadow: 0 0 0 1px var(--bb-border) inset;
}

.bb-lang :deep(.el-select__wrapper:hover) {
  box-shadow: 0 0 0 1px var(--bb-border-strong) inset;
}

.bb-lang :deep(.el-select__placeholder) {
  color: var(--bb-text-secondary);
}

.bb-lang__label {
  display: inline-flex;
  align-items: center;
  gap: 3px;
  font-size: var(--bb-font-size-sm);
}

.bb-lang__icon {
  width: 14px;
  height: 14px;
  color: var(--bb-text-muted);
}

.bb-theme-btn__icon {
  width: 17px;
  height: 17px;
  fill: none;
  stroke: currentColor;
  stroke-width: 1.6;
  stroke-linecap: round;
  stroke-linejoin: round;
}

/* ==========================================================================
 * Responsive
 * ======================================================================== */
@media (max-width: 1023px) {
  .bb-nav__links {
    display: none;
  }

  /* Current page moves to the centered pill, so drop the brand breadcrumb. */
  .bb-nav__page {
    display: block;
  }

  .bb-nav .bb-brand__page {
    display: none;
  }

  .bb-nav__lang {
    display: none;
  }
}

@media (max-width: 767px) {
  .bb-nav {
    padding: 0 var(--bb-space-2);
  }
}

@media (max-width: 639px) {
  /* Header only: the drawer keeps the product name / status text. */
  .bb-nav .bb-brand__name {
    display: none;
  }

  .bb-nav .bb-status__text {
    display: none;
  }

  .bb-nav .bb-status {
    padding: 0 7px;
    gap: 0;
  }

  .bb-nav__page {
    font-size: var(--bb-font-size-sm);
  }
}

/* ==========================================================================
 * Drawer (mobile navigation)
 * ======================================================================== */
.custom-drawer :deep(.el-overlay) {
  transition: all 0s;
}

.custom-drawer :deep(.el-drawer) {
  transition: all 0s;
  max-width: 82vw;
  border-right: 1px solid var(--bb-border);
  background: var(--bb-bg-elevated);
}

.custom-drawer.open :deep(.el-drawer) {
  transition: all 0.05s;
}

.custom-drawer :deep(.el-drawer__body) {
  display: flex;
  flex-direction: column;
  padding: 0;
}

.custom-drawer :deep(.el-drawer__footer) {
  padding: var(--bb-space-3) var(--bb-space-4);
  border-top: 1px solid var(--bb-border);
  background: var(--bb-surface-inset);
}

.bb-drawer__head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
  padding: var(--bb-space-3) var(--bb-space-3) var(--bb-space-3) var(--bb-space-4);
  border-bottom: 1px solid var(--bb-border);
}

.bb-brand--drawer {
  padding: 2px 4px;
}

.bb-drawer__status {
  padding: var(--bb-space-3) var(--bb-space-4) 0;
}

.bb-drawer__nav {
  flex: 1 1 auto;
  overflow-y: auto;
  padding: var(--bb-space-3) var(--bb-space-3);
}

.bb-drawer__nav ul {
  margin: 0;
  padding: 0;
  list-style: none;
}

.bb-drawer__link {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
  margin-bottom: 2px;
  padding: 10px 12px;
  border-radius: var(--bb-radius-sm);
  border-left: 2px solid transparent;
  font-size: var(--bb-font-size-md);
  font-weight: 550;
  color: var(--bb-text-secondary);
  text-decoration: none;
  transition: background-color var(--bb-duration-fast) var(--bb-ease),
    color var(--bb-duration-fast) var(--bb-ease);
}

.bb-drawer__link:hover {
  background: var(--bb-nav-link-hover-bg);
  color: var(--bb-text);
}

.bb-drawer__link.router-link-active {
  border-left-color: var(--bb-accent);
  background: var(--bb-nav-link-active-bg);
  color: var(--bb-nav-link-active);
  font-weight: 650;
}

.bb-drawer__footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
}

.bb-drawer__lang {
  flex: 1 1 auto;
  max-width: 130px;
}

.bb-drawer__lang :deep(.el-select__wrapper) {
  border-radius: var(--bb-radius-sm);
  background: var(--bb-surface);
  box-shadow: 0 0 0 1px var(--bb-border) inset;
}

.bb-drawer__actions {
  display: flex;
  align-items: center;
  gap: var(--bb-space-1);
}
</style>
