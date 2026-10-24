<template>
  <Teleport to="body">
    <div
        v-if="open"
        class="bb-palette__backdrop"
        @click.self="close()"
    >
      <div
          class="bb-palette"
          role="dialog"
          aria-modal="true"
          :aria-label="translate('palette.title')"
          data-testid="command-palette"
      >
        <div class="bb-palette__head">
          <svg class="bb-palette__search-icon" viewBox="0 0 20 20" aria-hidden="true">
            <circle cx="9" cy="9" r="5.5"/>
            <path d="M13.2 13.2 17.5 17.5"/>
          </svg>
          <input
              ref="inputRef"
              v-model="query"
              class="bb-palette__input"
              type="text"
              role="combobox"
              aria-autocomplete="list"
              :aria-expanded="true"
              aria-controls="bb-palette-list"
              :aria-activedescendant="activeDescendant"
              :aria-label="translate('palette.placeholder')"
              :placeholder="translate('palette.placeholder')"
              autocomplete="off"
              spellcheck="false"
              data-testid="palette-input"
              @keydown="onInputKeydown"
          />
          <button
              type="button"
              class="bb-palette__close"
              :aria-label="translate('common.close')"
              :title="translate('common.close')"
              @click="close()"
          >
            <InlineSvg name="cross" class="bb-palette__close-icon"/>
          </button>
        </div>

        <div id="bb-palette-list" class="bb-palette__body" role="listbox" aria-multiselectable="false">
          <template v-if="sections.length">
            <div v-for="section in sections" :key="section.key" class="bb-palette__section">
              <div class="bb-palette__section-title">{{ section.title }}</div>
              <button
                  v-for="item in section.items"
                  :id="`bb-palette-option-${itemIndex(item)}`"
                  :key="item.id"
                  type="button"
                  role="option"
                  class="bb-palette__item"
                  :class="{ 'is-active': itemIndex(item) === activeIndex }"
                  :aria-selected="itemIndex(item) === activeIndex"
                  @mouseenter="activeIndex = itemIndex(item)"
                  @click="void runItem(item)"
              >
                <span class="bb-palette__item-label">{{ item.label }}</span>
                <span v-if="item.kind === 'page'" class="bb-palette__item-hint bb-mono">{{ item.route }}</span>
                <span v-else-if="item.kind === 'variable' || item.kind === 'function'" class="bb-palette__item-hint bb-mono">
                  {{ item.hint }}
                </span>
                <span v-else-if="item.hint" class="bb-palette__item-hint">{{ item.hint }}</span>
              </button>
            </div>
          </template>
          <p v-else class="bb-palette__empty">{{ translate('palette.empty') }}</p>
        </div>

        <div class="bb-palette__footer">
          <span>{{ translate('palette.footer') }}</span>
          <span class="bb-palette__footer-index" data-testid="palette-index-state">
            {{ indexSummary }}
          </span>
        </div>
      </div>
    </div>
  </Teleport>
</template>

<script lang="ts" setup>
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import InlineSvg from '@/components/InlineSvg.vue';
import { translate } from '@/locales';
import { ControlEvent } from '@/api';
import { isOTAEnabled } from '@/composables/buildMode';
import { globalNotify } from '@/composables/notification';
import { kickDapSession } from '@/api/apiDap';
import { useWsStore } from '@/stores/websocket';
import { copyTextToClipboard } from '@/components/dashboard/clipboard';
import {
  PALETTE_OPEN_EVENT,
  dispatchAction,
} from '@/components/dashboard/actionBus';
import {
  buildDiagnosticsJson,
  recordDapKick,
  refreshWifi,
} from '@/components/dashboard/dashboardState';
import {
  commandIndexState,
  ensureCommandIndexBridge,
  focusVariable,
  formatCommandAddress,
  searchFunctions,
  searchVariables,
  type CommandIndexFunction,
  type CommandIndexVariable,
} from '@/utils/commandIndex';

type PaletteAction =
    | 'detect'
    | 'reset'
    | 'curve-toggle'
    | 'kick'
    | 'copy-diag'
    | 'guided'
    | 'diagnostics'
    | 'refresh-wifi'
    | 'devices';

interface PaletteItem {
  id: string;
  kind: 'page' | 'action' | 'variable' | 'function';
  label: string;
  hint: string;
  keywords: string[];
  route?: string;
  action?: PaletteAction;
  /** ELF variable path (DebugCurve unique key) for kind === 'variable'. */
  variablePath?: string;
  /** Function entry for kind === 'function'. */
  functionInfo?: CommandIndexFunction;
}

interface PaletteSection {
  key: string;
  title: string;
  items: PaletteItem[];
}

const RECENT_STORAGE_KEY = 'birdiebot.palette.recent.v1';
const RECENT_LIMIT = 5;
/** Variable/function search only kicks in from 2 characters (huge ELF indexes). */
const ELF_SECTION_LIMIT = 8;
const FUNCTION_SECTION_LIMIT = 6;

const router = useRouter();
const wsStore = useWsStore();

const open = ref(false);
const query = ref('');
const activeIndex = ref(0);
const inputRef = ref<HTMLInputElement | null>(null);
const recentIds = ref<string[]>(loadRecent());
let restoreFocus: HTMLElement | null = null;

function loadRecent(): string[] {
  try {
    const raw = localStorage.getItem(RECENT_STORAGE_KEY);
    if (!raw) {
      return [];
    }
    const parsed: unknown = JSON.parse(raw);
    if (!Array.isArray(parsed)) {
      return [];
    }
    return parsed.filter((item): item is string => typeof item === 'string').slice(0, RECENT_LIMIT);
  } catch {
    return [];
  }
}

function saveRecent(): void {
  try {
    localStorage.setItem(RECENT_STORAGE_KEY, JSON.stringify(recentIds.value));
  } catch {
    // Private mode: recents are session-only.
  }
}

function pushRecent(id: string): void {
  recentIds.value = [id, ...recentIds.value.filter((item) => item !== id)].slice(0, RECENT_LIMIT);
  saveRecent();
}

const allItems = computed<PaletteItem[]>(() => {
  const pages: PaletteItem[] = [
    {
      id: 'page:target',
      kind: 'page',
      label: translate('target.title'),
      hint: '',
      keywords: ['target', 'flash', 'burn', 'home'],
      route: '/target',
    },
    {
      id: 'page:curve',
      kind: 'page',
      label: translate('page.debugCurve'),
      hint: '',
      keywords: ['curve', 'sample', 'variable', 'elf'],
      route: '/debug-curve',
    },
    {
      id: 'page:uart',
      kind: 'page',
      label: translate('page.uart'),
      hint: '',
      keywords: ['uart', 'serial', 'terminal'],
      route: '/uart',
    },
    {
      id: 'page:wifi',
      kind: 'page',
      label: translate('page.wifi'),
      hint: '',
      keywords: ['wifi', 'ssid', 'network'],
      route: '/wifi',
    },
    {
      id: 'page:about',
      kind: 'page',
      label: translate('page.about'),
      hint: '',
      keywords: ['about', 'version', 'info'],
      route: '/about',
    },
  ];
  if (isOTAEnabled()) {
    pages.push({
      id: 'page:update',
      kind: 'page',
      label: translate('page.update'),
      hint: '',
      keywords: ['ota', 'update', 'firmware', 'upload'],
      route: '/update',
    });
  }
  const actions: PaletteItem[] = [
    {
      id: 'action:detect',
      kind: 'action',
      label: translate('palette.actionDetect'),
      hint: translate('palette.actionDetectHint'),
      keywords: ['detect', 'target', 'probe', 'swd'],
      action: 'detect',
    },
    {
      id: 'action:reset',
      kind: 'action',
      label: translate('palette.actionReset'),
      hint: translate('palette.actionResetHint'),
      keywords: ['reset', 'target', 'swd'],
      action: 'reset',
    },
    {
      id: 'action:curve-toggle',
      kind: 'action',
      label: translate('palette.actionCurveToggle'),
      hint: translate('palette.actionCurveToggleHint'),
      keywords: ['sample', 'start', 'stop', 'curve'],
      action: 'curve-toggle',
    },
    {
      id: 'action:kick',
      kind: 'action',
      label: translate('palette.actionKick'),
      hint: translate('palette.actionKickHint'),
      keywords: ['kick', 'dap', 'usbip', 'session'],
      action: 'kick',
    },
    {
      id: 'action:copy-diag',
      kind: 'action',
      label: translate('palette.actionCopyDiag'),
      hint: translate('palette.actionCopyDiagHint'),
      keywords: ['copy', 'diagnostics', 'json', 'issue'],
      action: 'copy-diag',
    },
    {
      id: 'action:guided',
      kind: 'action',
      label: translate('palette.actionGuided'),
      hint: translate('palette.actionGuidedHint'),
      keywords: ['guide', 'wizard', 'first', 'help'],
      action: 'guided',
    },
    {
      id: 'action:diagnostics',
      kind: 'action',
      label: translate('palette.actionDiagnostics'),
      hint: translate('palette.actionDiagnosticsHint'),
      keywords: ['diagnostics', 'recovery', 'kick', 'reset'],
      action: 'diagnostics',
    },
    {
      id: 'action:refresh-wifi',
      kind: 'action',
      label: translate('palette.actionRefreshWifi'),
      hint: translate('palette.actionRefreshWifiHint'),
      keywords: ['wifi', 'refresh', 'ssid', 'rssi'],
      action: 'refresh-wifi',
    },
    {
      id: 'action:devices',
      kind: 'action',
      label: translate('palette.actionDevices'),
      hint: translate('palette.actionDevicesHint'),
      keywords: ['device', 'discover', 'multi', 'ncm', 'mdns'],
      action: 'devices',
    },
  ];
  return [...pages, ...actions];
});

function itemMatches(item: PaletteItem, needle: string): boolean {
  if (!needle) {
    return true;
  }
  const haystack = [item.label, item.hint, ...item.keywords].join(' ').toLowerCase();
  return haystack.includes(needle);
}

function variableToItem(variable: CommandIndexVariable): PaletteItem {
  const address = formatCommandAddress(variable.address);
  const type = variable.type ? variable.type : '';
  const hintParts = [variable.path];
  if (type) {
    hintParts.push(type);
  }
  if (address) {
    hintParts.push(address);
  }
  return {
    id: `var:${variable.path}`,
    kind: 'variable',
    label: variable.name,
    hint: hintParts.join(' · '),
    keywords: [variable.name, variable.path, variable.type, address],
    variablePath: variable.path,
  };
}

function functionToItem(fn: CommandIndexFunction): PaletteItem {
  const address = formatCommandAddress(fn.address);
  return {
    id: `func:${fn.name}`,
    kind: 'function',
    label: fn.name,
    hint: address || translate('palette.noAddress'),
    keywords: [fn.name, address],
    functionInfo: fn,
  };
}

/**
 * Resolve a recent id back to an item without materializing PaletteItems for
 * the whole ELF index (reference can expose >2e4 variables; opening an empty
 * palette must stay cheap).
 */
function findItemById(id: string): PaletteItem | undefined {
  if (id.startsWith('var:')) {
    const path = id.slice('var:'.length);
    const variable = commandIndexState.variables.find((item) => item.path === path);
    return variable ? variableToItem(variable) : undefined;
  }
  if (id.startsWith('func:')) {
    const name = id.slice('func:'.length);
    const fn = commandIndexState.functions.find((item) => item.name === name);
    return fn ? functionToItem(fn) : undefined;
  }
  return allItems.value.find((item) => item.id === id);
}

const sections = computed<PaletteSection[]>(() => {
  const needle = query.value.trim().toLowerCase();
  const pageItems = allItems.value.filter((item) => item.kind === 'page' && itemMatches(item, needle));
  const actionItems = allItems.value.filter((item) => item.kind === 'action' && itemMatches(item, needle));
  const result: PaletteSection[] = [];
  if (!needle && recentIds.value.length > 0) {
    const recent = recentIds.value
        .map((id) => findItemById(id))
        .filter((item): item is PaletteItem => item !== undefined);
    if (recent.length > 0) {
      result.push({ key: 'recent', title: translate('palette.recent'), items: recent });
    }
  }
  if (pageItems.length > 0) {
    result.push({ key: 'pages', title: translate('palette.pages'), items: pageItems });
  }
  if (actionItems.length > 0) {
    result.push({ key: 'actions', title: translate('palette.actions'), items: actionItems });
  }
  // ELF variable/function search (P1-17): only for a >= 2 char query and when
  // the curve page has published a parsed index.
  if (needle.length >= 2) {
    const variableItems = searchVariables(commandIndexState.variables, needle, ELF_SECTION_LIMIT)
        .map((variable) => variableToItem(variable));
    if (variableItems.length > 0) {
      result.push({ key: 'variables', title: translate('palette.variables'), items: variableItems });
    }
    const functionItems = searchFunctions(commandIndexState.functions, needle, FUNCTION_SECTION_LIMIT)
        .map((fn) => functionToItem(fn));
    if (functionItems.length > 0) {
      result.push({ key: 'functions', title: translate('palette.functions'), items: functionItems });
    }
  }
  return result;
});

const flatItems = computed<PaletteItem[]>(() => sections.value.flatMap((section) => section.items));

const flatIndexById = computed<Map<string, number>>(() => {
  const map = new Map<string, number>();
  flatItems.value.forEach((item, index) => map.set(item.id, index));
  return map;
});

function itemIndex(item: PaletteItem): number {
  return flatIndexById.value.get(item.id) ?? -1;
}

const activeDescendant = computed(() => {
  const item = flatItems.value[activeIndex.value];
  return item ? `bb-palette-option-${itemIndex(item)}` : undefined;
});

const indexSummary = computed(() => {
  if (commandIndexState.available) {
    return translate('palette.elfIndexSummary', {
      count: commandIndexState.variables.length,
      funcs: commandIndexState.functions.length,
    });
  }
  return translate('palette.elfUnavailable');
});

function clampActiveIndex(): void {
  const max = flatItems.value.length - 1;
  if (max < 0) {
    activeIndex.value = -1;
  } else if (activeIndex.value < 0 || activeIndex.value > max) {
    activeIndex.value = 0;
  }
}

function moveActive(delta: number): void {
  const count = flatItems.value.length;
  if (count === 0) {
    return;
  }
  activeIndex.value = (activeIndex.value + delta + count) % count;
}

function onInputKeydown(event: KeyboardEvent): void {
  switch (event.key) {
    case 'ArrowDown':
      event.preventDefault();
      moveActive(1);
      break;
    case 'ArrowUp':
      event.preventDefault();
      moveActive(-1);
      break;
    case 'Home':
      event.preventDefault();
      activeIndex.value = 0;
      break;
    case 'End':
      event.preventDefault();
      activeIndex.value = Math.max(flatItems.value.length - 1, 0);
      break;
    case 'Enter': {
      event.preventDefault();
      const item = flatItems.value[activeIndex.value];
      if (item) {
        void runItem(item);
      }
      break;
    }
    case 'Escape':
      event.preventDefault();
      close();
      break;
    case 'Tab':
      // Single-focus dialog: keep focus in the search input.
      event.preventDefault();
      break;
    default:
      break;
  }
}

function openPalette(): void {
  if (open.value) {
    return;
  }
  restoreFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null;
  open.value = true;
  query.value = '';
  activeIndex.value = 0;
  document.body.style.overflow = 'hidden';
  void nextTick(() => {
    inputRef.value?.focus();
    inputRef.value?.select();
  });
}

function close(): void {
  if (!open.value) {
    return;
  }
  open.value = false;
  document.body.style.overflow = '';
  restoreFocus?.focus();
  restoreFocus = null;
}

function pageAction(action: PaletteAction): void {
  switch (action) {
    case 'detect':
      dispatchAction('target-detect');
      break;
    case 'reset':
      dispatchAction('target-reset');
      break;
    case 'guided':
      dispatchAction('guided-open');
      break;
    case 'diagnostics':
      dispatchAction('diagnostics-open');
      break;
    case 'devices':
      dispatchAction('devices-open');
      break;
    default:
      break;
  }
}

async function navigateThenRun(route: string, action: PaletteAction): Promise<void> {
  await router.push(route);
  await nextTick();
  // The target page listens for one-shot actions; queued when not yet mounted.
  pageAction(action);
}

/**
 * Trigger the Data Curve page's existing Space shortcut (start/stop sampling).
 *
 * DebugCurve.vue is intentionally out of this task's write scope, so the
 * palette uses its documented global keyboard shortcut instead of reaching
 * into the page's internals. The page ignores the key while typing in inputs.
 */
async function toggleCurveSampling(): Promise<void> {
  await router.push('/debug-curve');
  await nextTick();
  window.dispatchEvent(new KeyboardEvent('keydown', {
    key: ' ',
    code: 'Space',
    bubbles: true,
    cancelable: true,
  }));
}

async function runAction(action: PaletteAction): Promise<void> {
  switch (action) {
    case 'detect':
      // Detection uses the HTTP endpoint, so it also works while the main WS is
      // reconnecting; the page itself reports any device/network failure.
      await navigateThenRun('/target', 'detect');
      return;
    case 'reset':
      await navigateThenRun('/target', 'reset');
      return;
    case 'curve-toggle':
      await toggleCurveSampling();
      return;
    case 'kick': {
      const result = await kickDapSession();
      recordDapKick(result);
      if (result.ok) {
        globalNotify(translate('palette.kickOk'), 'success');
      } else {
        globalNotify(translate('palette.kickFail', { err: result.err || `HTTP ${result.httpStatus}` }), 'error');
      }
      return;
    }
    case 'copy-diag': {
      const ok = await copyTextToClipboard(buildDiagnosticsJson());
      if (ok) {
        globalNotify(translate('common.copied'), 'success');
      } else {
        globalNotify(translate('palette.copyFailed'), 'warning');
      }
      return;
    }
    case 'guided':
      await navigateThenRun('/target', 'guided');
      return;
    case 'diagnostics':
      await navigateThenRun('/target', 'diagnostics');
      return;
    case 'refresh-wifi':
      if (wsStore.state !== ControlEvent.CONNECTED) {
        globalNotify(translate('palette.notConnected'), 'warning');
        return;
      }
      refreshWifi();
      globalNotify(translate('palette.refreshWifiSent'), 'info');
      return;
    case 'devices':
      await navigateThenRun('/target', 'devices');
      return;
    default:
      return;
  }
}

/** Focus a curve variable: route query (primary) + fallback window event. */
async function focusCurveVariable(path: string): Promise<void> {
  await router.push({ path: '/debug-curve', query: { focusVar: path } });
  await nextTick();
  focusVariable(path);
}

async function runFunctionItem(fn: CommandIndexFunction): Promise<void> {
  const address = formatCommandAddress(fn.address);
  if (address) {
    const ok = await copyTextToClipboard(address);
    if (ok) {
      globalNotify(translate('palette.functionCopied', { addr: address }), 'success');
    } else {
      globalNotify(translate('palette.copyFailed'), 'warning');
    }
  }
  await router.push('/debug-curve');
}

async function runItem(item: PaletteItem): Promise<void> {
  if (item.kind !== 'page') {
    pushRecent(item.id);
  }
  close();
  if (item.kind === 'page' && item.route) {
    await router.push(item.route);
    return;
  }
  if (item.kind === 'variable' && item.variablePath) {
    await focusCurveVariable(item.variablePath);
    return;
  }
  if (item.kind === 'function' && item.functionInfo) {
    await runFunctionItem(item.functionInfo);
    return;
  }
  if (item.action) {
    await runAction(item.action);
  }
}

function onWindowKeydown(event: KeyboardEvent): void {
  if ((event.ctrlKey || event.metaKey) && (event.key === 'k' || event.key === 'K')) {
    event.preventDefault();
    if (open.value) {
      close();
    } else {
      openPalette();
    }
    return;
  }
  if (event.key === 'Escape' && open.value) {
    event.preventDefault();
    close();
  }
}

function onOpenEvent(): void {
  openPalette();
}

onMounted(() => {
  ensureCommandIndexBridge();
  window.addEventListener('keydown', onWindowKeydown, true);
  window.addEventListener(PALETTE_OPEN_EVENT, onOpenEvent);
});

onUnmounted(() => {
  window.removeEventListener('keydown', onWindowKeydown, true);
  window.removeEventListener(PALETTE_OPEN_EVENT, onOpenEvent);
  document.body.style.overflow = '';
});
</script>

<style scoped>
.bb-palette__backdrop {
  position: fixed;
  inset: 0;
  z-index: 2000;
  display: flex;
  align-items: flex-start;
  justify-content: center;
  padding: 12vh var(--bb-space-4) var(--bb-space-4);
  background: rgba(3, 5, 8, 0.62);
  backdrop-filter: blur(3px);
}

.bb-palette {
  width: min(680px, 100%);
  max-height: 70vh;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border: 1px solid var(--bb-border-strong);
  border-radius: var(--bb-radius-lg);
  background: var(--bb-bg-elevated);
  box-shadow: var(--bb-shadow-lg);
  color: var(--bb-text);
}

.bb-palette__head {
  display: flex;
  align-items: center;
  gap: var(--bb-space-3);
  padding: var(--bb-space-3) var(--bb-space-4);
  border-bottom: 1px solid var(--bb-border);
}

.bb-palette__search-icon {
  flex: 0 0 auto;
  width: 18px;
  height: 18px;
  fill: none;
  stroke: var(--bb-text-muted);
  stroke-width: 1.7;
  stroke-linecap: round;
}

.bb-palette__input {
  flex: 1 1 auto;
  min-width: 0;
  border: none;
  outline: none;
  background: transparent;
  color: var(--bb-text);
  font-size: var(--bb-font-size-lg);
}

.bb-palette__input::placeholder {
  color: var(--bb-text-faint);
}

.bb-palette__close {
  flex: 0 0 auto;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: var(--bb-radius-sm);
  background: transparent;
  color: var(--bb-text-muted);
  cursor: pointer;
}

.bb-palette__close:hover {
  background: var(--bb-nav-link-hover-bg);
  color: var(--bb-text);
}

.bb-palette__close:focus-visible {
  outline: none;
  box-shadow: var(--bb-focus-ring);
}

.bb-palette__close-icon {
  width: 15px;
  height: 15px;
}

.bb-palette__body {
  flex: 1 1 auto;
  overflow-y: auto;
  padding: var(--bb-space-2);
}

.bb-palette__section + .bb-palette__section {
  margin-top: var(--bb-space-2);
}

.bb-palette__section-title {
  padding: var(--bb-space-1) var(--bb-space-3);
  font-size: var(--bb-font-size-xs);
  font-weight: 600;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--bb-text-faint);
}

.bb-palette__item {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--bb-space-3);
  width: 100%;
  padding: 9px var(--bb-space-3);
  border: none;
  border-radius: var(--bb-radius-sm);
  background: transparent;
  color: var(--bb-text-secondary);
  text-align: left;
  cursor: pointer;
}

.bb-palette__item.is-active,
.bb-palette__item:hover {
  background: var(--bb-nav-link-active-bg);
  color: var(--bb-text);
}

.bb-palette__item:focus-visible {
  outline: none;
  box-shadow: var(--bb-focus-ring);
}

.bb-palette__item-label {
  min-width: 0;
  font-size: var(--bb-font-size-md);
  font-weight: 550;
  overflow-wrap: anywhere;
}

.bb-palette__item-hint {
  flex: 0 1 auto;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-faint);
}

.bb-palette__empty {
  margin: 0;
  padding: var(--bb-space-5) var(--bb-space-3);
  text-align: center;
  font-size: var(--bb-font-size-sm);
  color: var(--bb-text-muted);
}

.bb-palette__footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--bb-space-2);
  padding: var(--bb-space-2) var(--bb-space-4);
  border-top: 1px solid var(--bb-border);
  font-size: var(--bb-font-size-xs);
  color: var(--bb-text-faint);
}

.bb-palette__footer-index {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 639px) {
  .bb-palette__backdrop {
    padding-top: 8vh;
  }

  .bb-palette {
    max-height: 80vh;
  }

  .bb-palette__footer-index {
    display: none;
  }
}
</style>
