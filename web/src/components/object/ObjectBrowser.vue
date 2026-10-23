<!--
  ObjectBrowser: Ozone-style global object browser (root picker + lazy field tree).

  Standalone component (no vue-i18n / no Element Plus):
    - props.roots / props.variables: global object roots discovered by
      `objectRoots.discoverObjectRoots()` (or pass roots directly);
    - props.rootNode can pin one root without a root list;
    - labels are English by default; the parent page injects translations
      (see docs/object-watch.md);
    - live values are formatted by the parent (`liveValues` map) and only
      rendered here, so sampling/decoding logic stays in DebugCurve;
    - watch / curve / preset / source-jump interactions are emits; the parent
      owns state (series list, watch list, localStorage presets, source view).

  Lazy tree: only expanded branches are materialised (`buildObjectTree` with
  maxDepth/maxChildren/maxNodes) and only the visible window is in the DOM.
-->
<template>
  <section class="object-browser">
    <header class="ob-toolbar">
      <span class="ob-title">{{ labels.title }}</span>

      <div ref="rootPickerRef" class="ob-root-picker">
        <button
            type="button"
            class="ob-root-button"
            :aria-label="labels.rootAriaLabel"
            :aria-expanded="rootMenuOpen ? 'true' : 'false'"
            @click="toggleRootMenu"
        >
          <span class="ob-root-caption">{{ labels.rootsLabel }}</span>
          <span class="ob-root-name">{{ activeRoot ? activeRoot.name : labels.noRootSelected }}</span>
          <span class="ob-caret-static">▾</span>
        </button>
        <div v-if="rootMenuOpen" class="ob-root-menu" @keydown.esc="closeRootMenu">
          <input
              ref="rootSearchInputRef"
              v-model="rootQuery"
              type="search"
              class="ob-input ob-root-search"
              :placeholder="labels.rootSearchPlaceholder"
          />
          <div class="ob-root-list" role="listbox">
            <button
                v-for="root of shownRoots"
                :key="root.key"
                type="button"
                class="ob-root-option"
                :class="{active: !!activeRoot && root.key === activeRoot.key}"
                role="option"
                :aria-selected="!!activeRoot && root.key === activeRoot.key"
                @click="selectRoot(root)"
            >
              <span class="ob-root-option-head">
                <span class="ob-root-option-name">{{ root.name }}</span>
                <span class="ob-badge" :class="'ob-badge-' + root.kind">{{ rootKindLabel(root) }}</span>
                <span class="ob-root-option-count">{{ root.childCount }}</span>
              </span>
              <span class="ob-root-option-meta">
                {{ root.typeName || labels.noValue }} · {{ formatObjectAddress(root.address) }}
              </span>
            </button>
            <p v-if="shownRoots.length === 0" class="ob-menu-empty">{{ labels.rootMenuEmpty }}</p>
            <p v-else-if="filteredRoots.length > shownRoots.length" class="ob-menu-more">
              {{ t(labels.rootMenuMore, {shown: shownRoots.length, total: filteredRoots.length}) }}
            </p>
          </div>
        </div>
      </div>

      <template v-if="activeRoot">
        <span class="ob-chip" :title="activeRoot.qualifiedName">{{ activeRoot.typeName || labels.noValue }}</span>
        <span class="ob-chip ob-mono">{{ formatObjectAddress(activeRoot.address) }}</span>
        <span class="ob-chip">{{ t(labels.rootCount, {count: rootList.length}) }}</span>
        <span v-if="rootSource" class="ob-chip ob-mono" :title="rootSource.path">
          {{ rootSource.path }}:{{ rootSource.line }}
        </span>
      </template>

      <span class="ob-spacer"></span>
      <input
          v-model="searchInput"
          type="search"
          class="ob-input ob-search"
          :placeholder="labels.searchPlaceholder"
          :aria-label="labels.searchPlaceholder"
          @input="onSearchInput"
      />
      <label class="ob-check">
        <input v-model="onlySampleable" type="checkbox"/>
        <span>{{ labels.sampleableOnly }}</span>
      </label>
      <label class="ob-check">
        <input v-model="fixedAddressOnly" type="checkbox"/>
        <span>{{ labels.fixedAddressOnly }}</span>
      </label>
      <label class="ob-check">
        <input v-model="withValueOnly" type="checkbox"/>
        <span>{{ labels.withValueOnly }}</span>
      </label>
    </header>

    <div class="ob-actions">
      <button type="button" class="ob-btn" :disabled="!activeRoot" @click="expandAll">{{ labels.expandAll }}</button>
      <button type="button" class="ob-btn" :disabled="!activeRoot" @click="collapseAll">{{ labels.collapseAll }}</button>
      <span class="ob-sep"></span>
      <button type="button" class="ob-btn" :disabled="!activeRow" @click="onWatchSubtree">{{ labels.watchSubtree }}</button>
      <button type="button" class="ob-btn" :disabled="!activeRoot || displayRows.length === 0" @click="onWatchVisible">
        {{ labels.watchVisible }}
      </button>
      <button type="button" class="ob-btn" :disabled="markedCount === 0" @click="onWatchSelected">{{ labels.watchSelected }}</button>
      <span class="ob-sep"></span>
      <button
          type="button"
          class="ob-btn ob-btn-primary"
          :disabled="!activeRow || !isRowCurveable(activeRow)"
          @click="onAddCurve"
      >{{ labels.addCurves }}
      </button>
      <button type="button" class="ob-btn" :disabled="markedCount === 0" @click="onAddSelected">{{ labels.addSelected }}</button>
      <span class="ob-count">{{ t(labels.selectedCount, {count: markedCount}) }}</span>

      <span class="ob-spacer"></span>
      <span class="ob-presets-label">{{ labels.presetsLabel }}</span>
      <select class="ob-select" :value="internalPresetId" @change="onPresetSelect">
        <option value="">{{ presetList.length > 0 ? labels.presetSelect : labels.presetEmpty }}</option>
        <option v-for="preset of presetList" :key="preset.id" :value="preset.id">{{ preset.name }}</option>
      </select>
      <input
          v-model="presetName"
          type="text"
          class="ob-input ob-preset-name"
          :placeholder="labels.presetNamePlaceholder"
      />
      <button type="button" class="ob-btn" :disabled="markedCount === 0" @click="onSavePreset">{{ labels.presetSave }}</button>
      <button type="button" class="ob-btn" :disabled="!activePreset" @click="onLoadPreset">{{ labels.presetLoad }}</button>
      <button type="button" class="ob-btn" :disabled="!activePreset" @click="onDeletePreset">{{ labels.presetDelete }}</button>
    </div>

    <div v-if="invalidRootNotice" class="ob-banner ob-banner-warning">{{ invalidRootNotice }}</div>
    <div v-if="largeRootNotice" class="ob-banner ob-banner-info">{{ largeRootNotice }}</div>
    <div v-if="rootHasNoChildren" class="ob-banner ob-banner-info">{{ labels.noRootChildren }}</div>
    <div v-if="noticeText" class="ob-banner ob-banner-notice">{{ noticeText }}</div>

    <div v-if="!hasVariables" class="ob-empty">
      <h3 class="ob-empty-title">{{ labels.noVariablesTitle }}</h3>
      <p class="ob-empty-body">{{ labels.noVariablesBody }}</p>
    </div>
    <div v-else-if="rootList.length === 0 || !activeRoot" class="ob-empty">
      <h3 class="ob-empty-title">{{ labels.noRootsTitle }}</h3>
      <p class="ob-empty-body">{{ labels.noRootsBody }}</p>
    </div>
    <div v-else class="ob-main">
      <div class="ob-head" role="row">
        <span></span>
        <span></span>
        <span>{{ labels.colName }}</span>
        <span>{{ labels.colType }}</span>
        <span>{{ labels.colAddress }}</span>
        <span>{{ labels.colValue }}</span>
        <span></span>
      </div>
      <div
          ref="viewportRef"
          class="ob-tree"
          role="tree"
          tabindex="-1"
          :aria-label="labels.treeAriaLabel"
          @scroll.passive="onTreeScroll"
      >
        <div class="ob-canvas" :style="{height: totalHeight + 'px'}">
          <div
              v-for="item of renderedRows"
              :key="item.row.key"
              class="ob-row"
              :class="rowClasses(item.row)"
              :data-row-index="item.index"
              role="treeitem"
              :tabindex="item.row.key === activeRowKey ? 0 : -1"
              :aria-level="item.row.depth + 1"
              :aria-expanded="item.row.expandable ? (item.row.expanded ? 'true' : 'false') : undefined"
              :aria-selected="item.row.key === activeRowKey ? 'true' : 'false'"
              :style="{top: item.top + 'px'}"
              @click="onRowClick(item.row, $event)"
              @dblclick="toggleExpand(item.row)"
              @contextmenu.prevent="onRowContextMenu(item.row)"
              @keydown="onRowKeydown(item.row, $event)"
          >
            <span class="ob-cell ob-cell-caret" @click.stop>
              <button
                  v-if="!searchMode && item.row.expandable"
                  type="button"
                  class="ob-caret"
                  :title="item.row.expanded ? labels.collapseField : labels.expandField"
                  @click="toggleExpand(item.row)"
              >{{ item.row.expanded ? '▾' : '▸' }}
              </button>
              <span v-else-if="searchMode" class="ob-caret-static" :title="labels.revealInTree">›</span>
            </span>
            <span class="ob-cell ob-cell-star" @click.stop>
              <button
                  type="button"
                  class="ob-star"
                  :class="{on: isWatched(item.row)}"
                  :title="isWatched(item.row) ? labels.watchRemove : labels.watchAdd"
                  @click="onToggleWatch(item.row)"
              >{{ isWatched(item.row) ? '★' : '☆' }}
              </button>
            </span>
            <span
                class="ob-cell ob-cell-name"
                :style="{paddingLeft: (item.row.depth * 14) + 'px'}"
                :title="rowTitle(item.row)"
            >
              <span class="ob-name-text">{{ rowPrimaryText(item.row) }}</span>
              <span v-if="item.row.pointer" class="ob-ptr" title="pointer">*</span>
              <span
                  v-if="item.row.parserCapped"
                  class="ob-cap"
                  :title="t(labels.arrayCapped, {shown: item.row.childCount, total: item.row.totalChildCount})"
              >{{ item.row.childCount }}/{{ item.row.totalChildCount }}</span>
              <span v-if="isCurveSelected(item.row)" class="ob-added" :title="labels.addSelected">●</span>
            </span>
            <span class="ob-cell ob-cell-type" :title="item.row.typeName">{{ item.row.typeName || labels.noValue }}</span>
            <span class="ob-cell ob-cell-addr ob-mono" :title="rowAddressTitle(item.row)">{{ rowAddressText(item.row) }}</span>
            <span
                class="ob-cell ob-cell-value ob-mono"
                :class="{'stale': !!liveValueFor(item.row)?.stale}"
                :style="valueStyle(item.row)"
                :title="rowValueTitle(item.row)"
            >{{ liveValueFor(item.row)?.text ?? labels.noValue }}</span>
            <span class="ob-cell ob-cell-actions" @click.stop>
              <button
                  v-if="item.row.truncated"
                  type="button"
                  class="ob-mini"
                  :title="t(labels.showMoreChildren, {remaining: item.row.totalChildCount - item.row.childCount})"
                  @click="showMoreChildren(item.row)"
              >+
              </button>
              <button
                  v-if="searchMode"
                  type="button"
                  class="ob-mini"
                  :title="labels.revealInTree"
                  @click="revealMatch(searchMatchByKey.get(item.row.path))"
              >↗
              </button>
              <button
                  type="button"
                  class="ob-mini"
                  :disabled="!canJumpSource(item.row)"
                  :title="canJumpSource(item.row) ? labels.sourceJump : labels.noSourceLocation"
                  @click="onJumpSource(item.row)"
              >⤴
              </button>
            </span>
          </div>
        </div>
        <div v-if="displayRows.length === 0" class="ob-tree-empty">{{ treeEmptyText }}</div>
      </div>
      <div class="ob-foot">
        <span>{{ t(labels.visibleRows, {count: displayRows.length}) }}</span>
        <span v-if="searchMode">{{ t(labels.searchResultsCount, {count: searchMatches.length}) }}</span>
        <span v-if="rowsLimited" class="ob-foot-warn">{{ labels.rowsLimited }}</span>
        <span class="ob-spacer"></span>
        <span class="ob-foot-hint">{{ labels.multiSelectHint }}</span>
      </div>
    </div>
  </section>
</template>

<script setup lang="ts">
import {computed, nextTick, onBeforeUnmount, onMounted, ref, watch} from 'vue'
import type {VariableNode} from '@/utils/elfTypes'
import {formatObjectBrowserLabel, resolveObjectBrowserLabels, type ObjectBrowserLabels} from './labels'
import {
  DEFAULT_OBJECT_BATCH_LIMIT,
  DEFAULT_OBJECT_LARGE_ROOT_NODES,
  DEFAULT_OBJECT_SEARCH_MAX_MATCHES,
  DEFAULT_OBJECT_TREE_MAX_CHILDREN,
  DEFAULT_OBJECT_TREE_MAX_DEPTH,
  DEFAULT_OBJECT_TREE_MAX_NODES,
  DEFAULT_OBJECT_VIEWPORT_HEIGHT,
  OBJECT_OVERSCAN,
  OBJECT_ROW_HEIGHT,
  buildObjectTree,
  collectObjectNodes,
  collectObjectSubtree,
  computeObjectVirtualWindow,
  countObjectNodes,
  defaultObjectRoot,
  discoverObjectRoots,
  filterObjectTree,
  findObjectNode,
  findObjectNodes,
  findObjectRoot,
  normalizeObjectPreset,
  flattenObjectTree,
  formatObjectAddress,
  isObjectNodeCurveable,
  objectChildKey,
  objectPath,
  objectRootFromNode,
  objectRootKey,
  objectTreeNodeMatchesFilter,
  objectTreeNodeRef,
  parseObjectPresetJson,
  searchObjectTree,
  type ObjectBatchMode,
  type ObjectBatchPayload,
  type ObjectFilterOptions,
  type ObjectLiveValue,
  type ObjectLiveValueMap,
  type ObjectNodeRef,
  type ObjectPreset,
  type ObjectPresetLike,
  type ObjectPresetSavePayload,
  type ObjectRoot,
  type ObjectSearchMatch,
  type ObjectSourceJumpPayload,
  type ObjectToggleWatchPayload,
  type ObjectTreeNode,
} from './objectRoots'

defineOptions({name: 'ObjectBrowser'})

const props = defineProps<{
  /** Global object roots from `discoverObjectRoots()`; parent-managed. */
  roots?: readonly ObjectRoot[]
  /** Raw `parseElfVariables().variables`; roots are discovered internally when `roots` is absent. */
  variables?: readonly VariableNode[]
  /** Selected root key (`objectRootKey`). */
  rootKey?: string | null
  /** Pin one root node directly (takes precedence over `rootKey`). */
  rootNode?: VariableNode | null
  /** Curve series keys already selected in the parent (`path@address` or object path). */
  selectedKeys?: readonly string[]
  /** Watched keys (`path@address` or object path); the parent owns the watch list. */
  watchKeys?: readonly string[]
  /** Current value per field key (object path or `path@address`); rendered only. */
  liveValues?: ObjectLiveValueMap | null
  /** Text overrides; defaults are English (see `labels.ts`). */
  labels?: Partial<ObjectBrowserLabels>
  /** Object presets owned by the parent page (localStorage; `{rootKey,keys}` or `{rootPath,fields}`). */
  presets?: readonly ObjectPresetLike[]
  /** Controlled preset selection id. */
  activePresetId?: string | null
  /** Preferred root names (default `target_debug_handle`). */
  preferredNames?: readonly string[]
  /** Lazy tree depth below the root (default 8). */
  maxDepth?: number
  /** Hard cap of materialised children per node (default 64). */
  maxChildren?: number
  /** Hard cap of materialised tree nodes (default 4000). */
  maxNodes?: number
  /** Children materialised per node before "show more" (default 40). */
  pageSize?: number
  /** Watch/add-curve batch cap (default 512). */
  maxBatch?: number
  /** Descendant count above which the large-object hint is shown (default 1200). */
  largeRootThreshold?: number
  /** Search match cap (default 200). */
  maxSearchMatches?: number
  /** Root picker result cap (default 80). */
  rootSearchLimit?: number
  /** Extra object paths expanded when a root is selected. */
  initialExpanded?: readonly string[]
}>()

const emit = defineEmits<{
  /** Plain row click: the parent may add the field as a curve / set it active. */
  (event: 'select-variable', payload: ObjectNodeRef): void
  /** Star toggle: parent updates its watch list. */
  (event: 'toggle-watch', payload: ObjectToggleWatchPayload): void
  /** Batch watch: `mode` is selected / visible / subtree / active. */
  (event: 'watch-nodes', payload: ObjectBatchPayload): void
  /** Batch add-curve: same payload shape as `watch-nodes`. */
  (event: 'add-curves', payload: ObjectBatchPayload): void
  /** Open the parent source view at the field (or root declaration) file:line. */
  (event: 'jump-source', payload: ObjectSourceJumpPayload): void
  /** User picked another root in the picker. */
  (event: 'root-change', key: string, root: ObjectRoot): void
  /** Save the marked fields as a preset (parent persists it). */
  (event: 'save-preset', payload: ObjectPresetSavePayload): void
  /** Load a parent-provided preset. */
  (event: 'load-preset', preset: ObjectPreset): void
  /** Delete a parent-provided preset by id. */
  (event: 'delete-preset', id: string): void
  /** Actionable UI error (no source location, empty batch, ...). */
  (event: 'error', message: string): void
}>()

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

const labels = computed<ObjectBrowserLabels>(() => resolveObjectBrowserLabels(props.labels))

function t(template: string, values: Readonly<Record<string, string | number>> = {}): string {
  return formatObjectBrowserLabel(template, values)
}

// ---------------------------------------------------------------------------
// Roots: discovery, selection, picker
// ---------------------------------------------------------------------------

const discoveredRoots = computed<ObjectRoot[]>(() =>
  discoverObjectRoots(props.variables ?? [], {preferredNames: props.preferredNames}),
)

const rootList = computed<ObjectRoot[]>(() => {
  const base = props.roots && props.roots.length > 0 ? [...props.roots] : discoveredRoots.value
  const extra = props.rootNode ? objectRootFromNode(props.rootNode, {preferredNames: props.preferredNames}) : null
  if (!extra) return base
  if (base.some((root) => root.key === extra.key)) return base
  return [extra, ...base]
})

const requestedRootKey = computed<string | null>(() => {
  if (props.rootNode) return objectRootKey(props.rootNode)
  return props.rootKey ?? null
})

const internalRootKey = ref<string | null>(null)
watch(requestedRootKey, (key) => {
  if (key) internalRootKey.value = key
}, {immediate: true})

const activeRoot = computed<ObjectRoot | null>(() => {
  const wanted = internalRootKey.value ?? requestedRootKey.value
  const found = findObjectRoot(rootList.value, wanted)
  if (found) return found
  return defaultObjectRoot(rootList.value, {preferredNames: props.preferredNames})
})

const invalidRootNotice = computed<string>(() => {
  const wanted = requestedRootKey.value
  if (!wanted) return ''
  const resolved = findObjectRoot(rootList.value, wanted)
  if (resolved) return ''
  const active = activeRoot.value
  const name = String(props.rootKey ?? props.rootNode?.name ?? wanted)
  if (active) return t(labels.value.invalidRoot, {name, fallback: active.name})
  return t(labels.value.invalidRootFallback, {name})
})

const rootPickerRef = ref<HTMLElement | null>(null)
const rootSearchInputRef = ref<HTMLInputElement | null>(null)
const rootMenuOpen = ref(false)
const rootQuery = ref('')
const rootMenuLimit = computed<number>(() => Math.max(10, Math.floor(props.rootSearchLimit ?? 80)))
const filteredRoots = computed<ObjectRoot[]>(() => {
  const query = rootQuery.value.trim().toLowerCase()
  if (!query) return rootList.value
  return rootList.value.filter((root) => {
    const haystack = [root.name, root.qualifiedName, root.typeName, formatObjectAddress(root.address)]
      .filter(Boolean)
      .join('\n')
      .toLowerCase()
    return haystack.includes(query)
  })
})
const shownRoots = computed<ObjectRoot[]>(() => filteredRoots.value.slice(0, rootMenuLimit.value))

function rootKindLabel(root: ObjectRoot): string {
  if (root.kind === 'preferred') return labels.value.preferredBadge
  if (root.kind === 'pointer') return labels.value.pointerBadge
  if (root.kind === 'array') return labels.value.arrayBadge
  return labels.value.objectBadge
}

function toggleRootMenu(): void {
  rootMenuOpen.value = !rootMenuOpen.value
}

function closeRootMenu(): void {
  rootMenuOpen.value = false
}

function selectRoot(root: ObjectRoot): void {
  internalRootKey.value = root.key
  rootMenuOpen.value = false
  rootQuery.value = ''
  emit('root-change', root.key, root)
}

function onDocumentPointerDown(event: PointerEvent): void {
  if (!rootMenuOpen.value) return
  const picker = rootPickerRef.value
  if (picker && !picker.contains(event.target as Node)) rootMenuOpen.value = false
}

watch(rootMenuOpen, (open) => {
  if (open) void nextTick(() => rootSearchInputRef.value?.focus())
})

// ---------------------------------------------------------------------------
// Lazy tree state
// ---------------------------------------------------------------------------

const expandedKeys = ref<ReadonlySet<string>>(new Set())
const childLimits = ref<Record<string, number>>({})
const markedKeys = ref<ReadonlySet<string>>(new Set())
const activeRowKey = ref<string | null>(null)
const searchInput = ref('')
const debouncedQuery = ref('')
const noticeText = ref('')
const scrollTop = ref(0)
const viewportHeight = ref(DEFAULT_OBJECT_VIEWPORT_HEIGHT)
const viewportRef = ref<HTMLElement | null>(null)
let searchTimer = 0
let noticeTimer = 0
let lastMarkedIndex: number | null = null

const maxDepthValue = computed<number>(() => Math.max(1, Math.floor(props.maxDepth ?? DEFAULT_OBJECT_TREE_MAX_DEPTH)))
const maxChildrenValue = computed<number>(() => Math.max(1, Math.floor(props.maxChildren ?? DEFAULT_OBJECT_TREE_MAX_CHILDREN)))
const maxNodesValue = computed<number>(() => Math.max(64, Math.floor(props.maxNodes ?? DEFAULT_OBJECT_TREE_MAX_NODES)))
const pageSizeValue = computed<number>(() => Math.max(8, Math.floor(props.pageSize ?? 40)))
const batchLimit = computed<number>(() => Math.max(1, Math.floor(props.maxBatch ?? DEFAULT_OBJECT_BATCH_LIMIT)))

// Reset the tree view whenever the active root (or its underlying ELF) changes.
watch(activeRoot, (root) => {
  const next = new Set<string>()
  if (root) {
    next.add(objectPath(root.node))
    for (const path of props.initialExpanded ?? []) {
      if (path) next.add(path)
    }
  }
  expandedKeys.value = next
  childLimits.value = {}
  markedKeys.value = new Set()
  activeRowKey.value = null
  lastMarkedIndex = null
  searchInput.value = ''
  debouncedQuery.value = ''
  scrollTop.value = 0
  if (viewportRef.value) viewportRef.value.scrollTop = 0
}, {immediate: true})

const builtTree = computed<ObjectTreeNode | null>(() => {
  const root = activeRoot.value
  if (!root) return null
  return buildObjectTree(root, {
    expanded: expandedKeys.value,
    maxDepth: maxDepthValue.value,
    maxChildren: maxChildrenValue.value,
    maxNodes: maxNodesValue.value,
    expandRoot: true,
    childLimit: (path) => childLimits.value[path] ?? pageSizeValue.value,
  })
})

// ---------------------------------------------------------------------------
// Live values / filters
// ---------------------------------------------------------------------------

const liveValueMap = computed<ObjectLiveValueMap>(() => props.liveValues ?? {})

function liveValueFor(node: ObjectTreeNode | null | undefined): ObjectLiveValue | null {
  if (!node) return null
  const map = liveValueMap.value
  const direct = map[node.path]
  if (direct) return direct
  const byCurveKey = map[node.curveKey]
  return byCurveKey ?? null
}

const onlySampleable = ref(false)
const fixedAddressOnly = ref(false)
const withValueOnly = ref(false)
const hasActiveFilter = computed<boolean>(() =>
  onlySampleable.value || fixedAddressOnly.value || withValueOnly.value,
)
const filterOptions = computed<ObjectFilterOptions>(() => ({
  sampleableOnly: onlySampleable.value,
  fixedAddressOnly: fixedAddressOnly.value,
  withValueOnly: withValueOnly.value,
  hasValue: (node: ObjectTreeNode) => !!liveValueFor(node),
}))
const filteredTree = computed<ObjectTreeNode | null>(() => {
  // "With value only" must re-filter when live values arrive (parent refresh).
  if (withValueOnly.value) void liveValueMap.value
  return filterObjectTree(builtTree.value, filterOptions.value)
})
const treeRows = computed<ObjectTreeNode[]>(() => flattenObjectTree(filteredTree.value))

// ---------------------------------------------------------------------------
// Search (flat matches over the raw DWARF tree, bounded)
// ---------------------------------------------------------------------------

function onSearchInput(): void {
  if (searchTimer) window.clearTimeout(searchTimer)
  searchTimer = window.setTimeout(() => {
    searchTimer = 0
    debouncedQuery.value = searchInput.value
  }, 150)
}

watch(searchInput, (value) => {
  if (value) return
  if (searchTimer) {
    window.clearTimeout(searchTimer)
    searchTimer = 0
  }
  debouncedQuery.value = ''
})

const searchMode = computed<boolean>(() => debouncedQuery.value.trim().length > 0)
const searchMatches = computed<ObjectSearchMatch[]>(() => {
  const root = activeRoot.value
  if (!root || !searchMode.value) return []
  return searchObjectTree(root, debouncedQuery.value, {
    maxMatches: Math.max(10, Math.floor(props.maxSearchMatches ?? DEFAULT_OBJECT_SEARCH_MAX_MATCHES)),
    filter: (node) => objectTreeNodeMatchesFilter(node, filterOptions.value),
  })
})
const searchMatchByKey = computed<Map<string, ObjectSearchMatch>>(() => {
  const map = new Map<string, ObjectSearchMatch>()
  for (const match of searchMatches.value) map.set(match.node.path, match)
  return map
})
const displayRows = computed<ObjectTreeNode[]>(() =>
  searchMode.value ? searchMatches.value.map((match) => match.node) : treeRows.value,
)

function clearSearch(): void {
  if (searchTimer) {
    window.clearTimeout(searchTimer)
    searchTimer = 0
  }
  searchInput.value = ''
  debouncedQuery.value = ''
  scrollTop.value = 0
  if (viewportRef.value) viewportRef.value.scrollTop = 0
}

// ---------------------------------------------------------------------------
// Virtual rows
// ---------------------------------------------------------------------------

const virtualWindow = computed(() => computeObjectVirtualWindow({
  scrollTop: scrollTop.value,
  viewportHeight: viewportHeight.value || DEFAULT_OBJECT_VIEWPORT_HEIGHT,
  rowHeight: OBJECT_ROW_HEIGHT,
  rowCount: displayRows.value.length,
  overscan: OBJECT_OVERSCAN,
}))
const totalHeight = computed<number>(() => virtualWindow.value.totalHeight)
const renderedRows = computed<Array<{row: ObjectTreeNode; top: number; index: number}>>(() => {
  const window = virtualWindow.value
  const rows: Array<{row: ObjectTreeNode; top: number; index: number}> = []
  for (let index = window.start; index < window.end; index++) {
    const row = displayRows.value[index]
    if (!row) continue
    rows.push({row, top: index * OBJECT_ROW_HEIGHT, index})
  }
  return rows
})
const rowsLimited = computed<boolean>(() =>
  builtTree.value !== null && displayRows.value.length >= maxNodesValue.value,
)

function onTreeScroll(): void {
  scrollTop.value = viewportRef.value?.scrollTop ?? 0
}

function scrollToIndex(index: number): void {
  const viewport = viewportRef.value
  if (!viewport || index < 0) return
  const top = index * OBJECT_ROW_HEIGHT
  const height = viewport.clientHeight || viewportHeight.value || DEFAULT_OBJECT_VIEWPORT_HEIGHT
  if (top < viewport.scrollTop) viewport.scrollTop = top
  else if (top + OBJECT_ROW_HEIGHT > viewport.scrollTop + height) viewport.scrollTop = top - height + OBJECT_ROW_HEIGHT
  scrollTop.value = viewport.scrollTop
}

// ---------------------------------------------------------------------------
// Marked rows / existing curve+watch keys
// ---------------------------------------------------------------------------

const visibleIndexByKey = computed<Map<string, number>>(() => {
  const map = new Map<string, number>()
  displayRows.value.forEach((row, index) => map.set(row.key, index))
  return map
})
const markedRows = computed<ObjectTreeNode[]>(() => {
  const visible = displayRows.value.filter((row) => markedKeys.value.has(row.key))
  if (visible.length >= markedKeys.value.size || !activeRoot.value) return visible
  // Marked rows hidden by the current filter/search are still part of the
  // selection: resolve them from the raw tree so batch actions never drop them.
  const present = new Set(visible.map((row) => row.path))
  const missing = [...markedKeys.value].filter((key) => !present.has(key))
  const matches = findObjectNodes(activeRoot.value, missing)
  return [...visible, ...matches.map((match) => match.node)]
})
const markedCount = computed<number>(() => markedKeys.value.size)
const activeRow = computed<ObjectTreeNode | null>(() => {
  const key = activeRowKey.value
  if (!key) return null
  return displayRows.value.find((row) => row.key === key) ?? null
})
const selectedCurveKeySet = computed<Set<string>>(() => new Set(props.selectedKeys ?? []))
const watchedKeySet = computed<Set<string>>(() => new Set(props.watchKeys ?? []))

function isCurveSelected(row: ObjectTreeNode): boolean {
  const keys = selectedCurveKeySet.value
  return keys.has(row.path) || keys.has(row.curveKey)
}

function isWatched(row: ObjectTreeNode): boolean {
  const keys = watchedKeySet.value
  return keys.has(row.path) || keys.has(row.curveKey)
}

function isRowCurveable(row: ObjectTreeNode): boolean {
  return isObjectNodeCurveable(row.node)
}

function rowPrimaryText(row: ObjectTreeNode): string {
  return searchMode.value ? row.path : row.displayName
}

function rowTitle(row: ObjectTreeNode): string {
  const parts = [row.path, row.locationText]
  const source = sourceTargetFor(row)
  if (source) parts.push(`${source.path}:${source.line}`)
  return parts.filter(Boolean).join('\n')
}

function rowAddressText(row: ObjectTreeNode): string {
  if (row.address !== 0) return formatObjectAddress(row.address)
  if (row.sampleAddress !== 0) return formatObjectAddress(row.sampleAddress)
  return labels.value.derivedAddress
}

function rowAddressTitle(row: ObjectTreeNode): string {
  return row.locationText
}

function rowValueTitle(row: ObjectTreeNode): string {
  const value = liveValueFor(row)
  if (value?.stale) return labels.value.staleValue
  return row.locationText || ''
}

function valueStyle(row: ObjectTreeNode): Record<string, string> {
  const value = liveValueFor(row)
  if (!value || value.stale || !value.color) return {}
  return {color: value.color}
}

function rowClasses(row: ObjectTreeNode): Record<string, boolean> {
  return {
    active: row.key === activeRowKey.value,
    marked: markedKeys.value.has(row.key),
    watched: isWatched(row),
    'curve-selected': isCurveSelected(row),
    aggregate: row.expandable,
    pointer: row.pointer,
    'is-stale': !!liveValueFor(row)?.stale,
    'search-match': searchMode.value,
  }
}

// ---------------------------------------------------------------------------
// Row interactions
// ---------------------------------------------------------------------------

function onRowClick(row: ObjectTreeNode, event: MouseEvent): void {
  const index = visibleIndexByKey.value.get(row.key) ?? -1
  const additive = event.ctrlKey || event.metaKey
  const range = event.shiftKey && lastMarkedIndex !== null && index >= 0
  if (range) {
    const start = Math.min(lastMarkedIndex as number, index)
    const end = Math.max(lastMarkedIndex as number, index)
    const next = new Set<string>(additive ? markedKeys.value : [])
    for (let cursor = start; cursor <= end; cursor++) {
      const candidate = displayRows.value[cursor]
      if (candidate) next.add(candidate.key)
    }
    markedKeys.value = next
  } else if (additive) {
    const next = new Set(markedKeys.value)
    if (next.has(row.key)) next.delete(row.key)
    else next.add(row.key)
    markedKeys.value = next
    lastMarkedIndex = index
  } else {
    markedKeys.value = new Set([row.key])
    lastMarkedIndex = index
  }
  activeRowKey.value = row.key
  if (!additive && !range) emitFieldSelection(row)
}

function onRowKeydown(row: ObjectTreeNode, event: KeyboardEvent): void {
  switch (event.key) {
    case 'Enter':
    case ' ':
      event.preventDefault()
      markedKeys.value = new Set([row.key])
      lastMarkedIndex = visibleIndexByKey.value.get(row.key) ?? null
      activeRowKey.value = row.key
      emitFieldSelection(row)
      break
    case 'ArrowRight':
      event.preventDefault()
      expandRow(row)
      break
    case 'ArrowLeft':
      event.preventDefault()
      collapseRow(row)
      break
    case 'ArrowDown':
      event.preventDefault()
      focusRowAt((visibleIndexByKey.value.get(row.key) ?? 0) + 1)
      break
    case 'ArrowUp':
      event.preventDefault()
      focusRowAt((visibleIndexByKey.value.get(row.key) ?? 0) - 1)
      break
    default:
      break
  }
}

function focusRowAt(index: number): void {
  if (displayRows.value.length === 0) return
  const clamped = Math.max(0, Math.min(displayRows.value.length - 1, index))
  const row = displayRows.value[clamped]
  if (!row) return
  activeRowKey.value = row.key
  scrollToIndex(clamped)
  void nextTick(() => {
    viewportRef.value?.querySelector<HTMLElement>(`[data-row-index="${clamped}"]`)?.focus()
  })
}

function emitFieldSelection(row: ObjectTreeNode): void {
  const root = activeRoot.value
  if (!root) return
  emit('select-variable', objectTreeNodeRef(root, row))
}

function toggleExpand(row: ObjectTreeNode): void {
  if (!row.expandable) return
  const next = new Set(expandedKeys.value)
  if (next.has(row.path)) next.delete(row.path)
  else next.add(row.path)
  expandedKeys.value = next
}

function expandRow(row: ObjectTreeNode): void {
  if (!row.expandable || expandedKeys.value.has(row.path)) return
  const next = new Set(expandedKeys.value)
  next.add(row.path)
  expandedKeys.value = next
}

function collapseRow(row: ObjectTreeNode): void {
  if (!expandedKeys.value.has(row.path)) return
  const next = new Set(expandedKeys.value)
  next.delete(row.path)
  expandedKeys.value = next
}

function showMoreChildren(row: ObjectTreeNode): void {
  const current = childLimits.value[row.path] ?? pageSizeValue.value
  const target = Math.min(Math.max(row.totalChildCount, current), current + pageSizeValue.value)
  childLimits.value = {...childLimits.value, [row.path]: target}
}

function revealMatch(match: ObjectSearchMatch | null | undefined): void {
  if (!match) return
  clearSearch()
  const next = new Set(expandedKeys.value)
  for (const ancestor of match.ancestors) next.add(ancestor)
  next.add(match.node.path)
  expandedKeys.value = next
  const targetPath = match.node.path
  activeRowKey.value = targetPath
  markedKeys.value = new Set([targetPath])
  void nextTick(() => {
    const index = displayRows.value.findIndex((row) => row.key === targetPath)
    if (index >= 0) scrollToIndex(index)
  })
}

// ---------------------------------------------------------------------------
// Expand / collapse all (bounded by the tree node budget)
// ---------------------------------------------------------------------------

function expandAll(): void {
  const root = activeRoot.value
  if (!root) return
  const limit = maxNodesValue.value
  const paths = new Set<string>()
  const visited = new WeakSet<VariableNode>()
  const stack: Array<{node: VariableNode; path: string}> = [{node: root.node, path: objectPath(root.node)}]
  let count = 0
  while (stack.length > 0 && count < limit) {
    const current = stack.pop() as {node: VariableNode; path: string}
    if (visited.has(current.node)) continue
    visited.add(current.node)
    count += 1
    const children = current.node.children ?? []
    if (children.length > 0) paths.add(current.path)
    for (let index = children.length - 1; index >= 0; index--) {
      const child = children[index]
      stack.push({node: child, path: objectChildKey(current.path, child)})
    }
  }
  expandedKeys.value = paths
  if (count >= limit) showNotice(labels.value.rowsLimited)
}

function collapseAll(): void {
  const root = activeRoot.value
  if (!root) return
  expandedKeys.value = new Set([objectPath(root.node)])
}

// ---------------------------------------------------------------------------
// Watch / curve batches
// ---------------------------------------------------------------------------

type BatchKind = 'watch' | 'curve'

function runBatch(kind: BatchKind, mode: ObjectBatchMode, rows: readonly ObjectTreeNode[], subtree?: ObjectTreeNode): void {
  const root = activeRoot.value
  if (!root) return
  const options = {maxBatch: batchLimit.value, curveableOnly: true}
  const payload = subtree && mode === 'subtree'
    ? collectObjectSubtree(root, subtree, options)
    : collectObjectNodes(root, rows, mode, options)
  if (payload.nodes.length === 0) {
    showNotice(labels.value.batchEmpty)
    emit('error', labels.value.batchEmpty)
    return
  }
  if (payload.truncated) {
    showNotice(t(labels.value.batchTruncated, {max: payload.nodes.length, total: payload.total}))
  }
  if (kind === 'watch') emit('watch-nodes', payload)
  else emit('add-curves', payload)
}

function onWatchSubtree(): void {
  const row = activeRow.value
  if (!row) return
  runBatch('watch', 'subtree', [], row)
}

function onWatchVisible(): void {
  runBatch('watch', 'visible', treeRows.value)
}

function onWatchSelected(): void {
  runBatch('watch', 'selected', markedRows.value)
}

function onAddCurve(): void {
  const row = activeRow.value
  if (!row) return
  runBatch('curve', 'active', [row])
}

function onAddSelected(): void {
  runBatch('curve', 'selected', markedRows.value)
}

function onToggleWatch(row: ObjectTreeNode): void {
  const root = activeRoot.value
  if (!root) return
  emit('toggle-watch', {...objectTreeNodeRef(root, row), watched: !isWatched(row)})
}

// ---------------------------------------------------------------------------
// Source jump
// ---------------------------------------------------------------------------

const rootSource = computed<{path: string; line: number} | null>(() => {
  const root = activeRoot.value
  if (!root) return null
  const path = root.relativeFile || root.file || ''
  const line = root.line ?? 0
  return path && line > 0 ? {path, line} : null
})

function sourceTargetFor(row: ObjectTreeNode): {path: string; line: number; address?: number} | null {
  const path = row.relativeFile || row.file || rootSource.value?.path || ''
  const line = row.line ?? rootSource.value?.line ?? 0
  if (!path || line <= 0) return null
  const address = row.sampleAddress || row.address || undefined
  return {path, line, address}
}

function canJumpSource(row: ObjectTreeNode): boolean {
  return sourceTargetFor(row) !== null
}

function onJumpSource(row: ObjectTreeNode): void {
  const root = activeRoot.value
  const target = sourceTargetFor(row)
  if (!root || !target) {
    showNotice(labels.value.noSourceLocation)
    emit('error', labels.value.noSourceLocation)
    return
  }
  emit('jump-source', {
    path: target.path,
    relativeFile: target.path,
    file: row.file,
    line: target.line,
    address: target.address,
    node: objectTreeNodeRef(root, row),
  })
}

function onRowContextMenu(row: ObjectTreeNode): void {
  onJumpSource(row)
}

// ---------------------------------------------------------------------------
// Presets (the parent owns persistence + the list)
// ---------------------------------------------------------------------------

const presetList = computed<ObjectPreset[]>(() => {
  const normalized: ObjectPreset[] = []
  for (const preset of props.presets ?? []) {
    const item = normalizeObjectPreset(preset)
    if (item) normalized.push(item)
  }
  return normalized
})
const internalPresetId = ref<string>(props.activePresetId ?? '')
watch(() => props.activePresetId, (id) => {
  internalPresetId.value = id ?? ''
})
const activePreset = computed<ObjectPreset | null>(() =>
  presetList.value.find((preset) => preset.id === internalPresetId.value) ?? null,
)
const presetName = ref('')

function onPresetSelect(event: Event): void {
  internalPresetId.value = (event.target as HTMLSelectElement).value
}

function onSavePreset(): void {
  const root = activeRoot.value
  const keys = [...markedKeys.value]
  if (!root || keys.length === 0) {
    showNotice(labels.value.batchEmpty)
    emit('error', labels.value.batchEmpty)
    return
  }
  const name = presetName.value.trim() || t(labels.value.presetDefaultName, {count: presetList.value.length + 1})
  emit('save-preset', {name, rootKey: root.key, rootPath: root.qualifiedName || root.name, keys})
  presetName.value = ''
}

function onLoadPreset(): void {
  const preset = activePreset.value
  if (!preset) return
  emit('load-preset', preset)
  void applyPresetLocally(preset)
}

function onDeletePreset(): void {
  const preset = activePreset.value
  if (!preset) return
  emit('delete-preset', preset.id)
}

function findRootForPreset(preset: ObjectPreset): ObjectRoot | null {
  const byKey = findObjectRoot(rootList.value, preset.rootKey)
  if (byKey) return byKey
  const wantedPath = (preset.rootPath || preset.rootKey).trim()
  if (!wantedPath) return null
  return rootList.value.find((root) =>
    root.qualifiedName === wantedPath
    || root.name === wantedPath
    || root.key === wantedPath
  ) ?? null
}

async function applyPresetLocally(preset: ObjectPreset): Promise<void> {
  const targetRoot = findRootForPreset(preset)
  if (targetRoot && targetRoot.key !== activeRoot.value?.key) internalRootKey.value = targetRoot.key
  await nextTick()
  const root = activeRoot.value
  if (!root) return
  const matches = findObjectNodes(root, preset.keys)
  const nextExpanded = new Set(expandedKeys.value)
  const marks = new Set<string>()
  for (const match of matches) {
    marks.add(match.node.path)
    for (const ancestor of match.ancestors) nextExpanded.add(ancestor)
  }
  expandedKeys.value = nextExpanded
  markedKeys.value = marks
  const firstPath = matches[0]?.node.path
  if (firstPath) {
    activeRowKey.value = firstPath
    void nextTick(() => {
      const index = displayRows.value.findIndex((row) => row.key === firstPath)
      if (index >= 0) scrollToIndex(index)
    })
  }
}

// ---------------------------------------------------------------------------
// Notices
// ---------------------------------------------------------------------------

function showNotice(message: string): void {
  noticeText.value = message
  if (noticeTimer) window.clearTimeout(noticeTimer)
  noticeTimer = window.setTimeout(() => {
    noticeText.value = ''
    noticeTimer = 0
  }, 4000)
}

// ---------------------------------------------------------------------------
// Empty / warning states
// ---------------------------------------------------------------------------

const hasVariables = computed<boolean>(() => {
  if (props.variables) return props.variables.length > 0
  return rootList.value.length > 0
})
const rootHasNoChildren = computed<boolean>(() => !!activeRoot.value && activeRoot.value.childCount === 0)
const treeEmptyText = computed<string>(() => {
  if (searchMode.value) return labels.value.noSearchResults
  if (hasActiveFilter.value) return labels.value.noRowsFiltered
  if (activeRoot.value && activeRoot.value.childCount === 0) return labels.value.noRootChildren
  return labels.value.noRowsFiltered
})
const largeRootNotice = computed<string>(() => {
  const root = activeRoot.value
  if (!root) return ''
  const threshold = Math.max(100, Math.floor(props.largeRootThreshold ?? DEFAULT_OBJECT_LARGE_ROOT_NODES))
  const counted = countObjectNodes(root.node, threshold + 1)
  if (counted.count <= threshold) return ''
  const count = counted.truncated ? `${counted.count}+` : String(counted.count)
  return t(labels.value.largeRoot, {count})
})

// ---------------------------------------------------------------------------
// Exposed API for the parent page (DebugCurve Objects tab)
// ---------------------------------------------------------------------------

function selectRootByKey(key: string): boolean {
  const root = findObjectRoot(rootList.value, key)
  if (!root) return false
  internalRootKey.value = root.key
  return true
}

function scrollToPath(pathOrKey: string): boolean {
  const root = activeRoot.value
  if (!root) return false
  const match = findObjectNode(root, pathOrKey)
  if (!match) return false
  const next = new Set(expandedKeys.value)
  for (const ancestor of match.ancestors) next.add(ancestor)
  next.add(match.node.path)
  expandedKeys.value = next
  const targetPath = match.node.path
  activeRowKey.value = targetPath
  void nextTick(() => {
    const index = displayRows.value.findIndex((row) => row.key === targetPath)
    if (index >= 0) scrollToIndex(index)
  })
  return true
}

function revealPath(pathOrKey: string): boolean {
  const root = activeRoot.value
  if (!root) return false
  const match = findObjectNode(root, pathOrKey)
  if (!match) return false
  revealMatch(match)
  return true
}

function getVisibleNodeRefs(): ObjectNodeRef[] {
  const root = activeRoot.value
  if (!root) return []
  return treeRows.value.filter(isRowCurveable).map((row) => objectTreeNodeRef(root, row))
}

function getMarkedNodeRefs(): ObjectNodeRef[] {
  const root = activeRoot.value
  if (!root) return []
  return markedRows.value.filter(isRowCurveable).map((row) => objectTreeNodeRef(root, row))
}

function exportPreset(name?: string): ObjectPresetSavePayload | null {
  const root = activeRoot.value
  const keys = [...markedKeys.value]
  if (!root || keys.length === 0) return null
  return {
    name: String(name ?? presetName.value).trim() || t(labels.value.presetDefaultName, {count: presetList.value.length + 1}),
    rootKey: root.key,
    rootPath: root.qualifiedName || root.name,
    keys,
  }
}

defineExpose({
  selectRootByKey,
  scrollToPath,
  revealPath,
  expandAll,
  collapseAll,
  getVisibleNodeRefs,
  getMarkedNodeRefs,
  exportPreset,
  importPresetJson: (text: string): ObjectPreset | null => parseObjectPresetJson(text),
  getActiveRoot: (): ObjectRoot | null => activeRoot.value,
  getRoots: (): ObjectRoot[] => rootList.value,
  getActiveRow: (): ObjectTreeNode | null => activeRow.value,
})

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

let resizeObserver: ResizeObserver | null = null

onMounted(() => {
  document.addEventListener('pointerdown', onDocumentPointerDown)
  const viewport = viewportRef.value
  if (!viewport) return
  viewportHeight.value = viewport.clientHeight || DEFAULT_OBJECT_VIEWPORT_HEIGHT
  if (typeof ResizeObserver !== 'undefined') {
    resizeObserver = new ResizeObserver(() => {
      const current = viewportRef.value
      if (current) viewportHeight.value = current.clientHeight || DEFAULT_OBJECT_VIEWPORT_HEIGHT
    })
    resizeObserver.observe(viewport)
  }
})

onBeforeUnmount(() => {
  document.removeEventListener('pointerdown', onDocumentPointerDown)
  resizeObserver?.disconnect()
  resizeObserver = null
  if (searchTimer) window.clearTimeout(searchTimer)
  if (noticeTimer) window.clearTimeout(noticeTimer)
  searchTimer = 0
  noticeTimer = 0
})
</script>

<style scoped>
/* Dark-first styling: every color comes from design-tokens.css (no external CSS). */
.object-browser {
  display: flex;
  flex-direction: column;
  gap: 6px;
  height: 100%;
  min-height: 0;
  color: var(--bb-text, #e8ebf0);
  font-family: var(--bb-font-sans, sans-serif);
  font-size: var(--bb-font-size-sm, 12px);
}

.ob-toolbar,
.ob-actions {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
}

.ob-title {
  font-size: var(--bb-font-size-md, 13px);
  font-weight: 650;
  color: var(--bb-text, #e8ebf0);
  margin-right: 2px;
}

.ob-spacer {
  flex: 1 1 auto;
}

.ob-sep {
  width: 1px;
  height: 18px;
  background: var(--bb-border, #29303a);
  margin: 0 2px;
}

/* Root picker ------------------------------------------------------------ */
.ob-root-picker {
  position: relative;
  min-width: 210px;
}

.ob-root-button {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  width: 100%;
  max-width: 340px;
  padding: 3px 8px;
  color: var(--bb-text, #e8ebf0);
  background: var(--bb-surface-inset, #0d0f13);
  border: 1px solid var(--bb-border, #29303a);
  border-radius: var(--bb-radius-sm, 6px);
  cursor: pointer;
  font: inherit;
}

.ob-root-button:hover,
.ob-root-button[aria-expanded='true'] {
  border-color: var(--bb-accent, #4da3ff);
}

.ob-root-caption {
  color: var(--bb-text-muted, #7d8795);
  font-size: var(--bb-font-size-xs, 11px);
  white-space: nowrap;
}

.ob-root-name {
  font-family: var(--bb-font-mono, monospace);
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ob-root-menu {
  position: absolute;
  z-index: 40;
  top: calc(100% + 4px);
  left: 0;
  width: 380px;
  padding: 6px;
  background: var(--bb-surface-raised, #1a1f26);
  border: 1px solid var(--bb-border, #29303a);
  border-radius: var(--bb-radius-md, 10px);
  box-shadow: var(--bb-shadow-md, 0 10px 28px rgba(0, 0, 0, 0.45));
}

.ob-root-search {
  width: 100%;
  margin-bottom: 6px;
}

.ob-root-list {
  max-height: 320px;
  overflow: auto;
}

.ob-root-option {
  display: flex;
  flex-direction: column;
  gap: 1px;
  width: 100%;
  padding: 5px 7px;
  color: var(--bb-text, #e8ebf0);
  text-align: left;
  background: transparent;
  border: 0;
  border-radius: var(--bb-radius-xs, 4px);
  cursor: pointer;
  font: inherit;
}

.ob-root-option:hover {
  background: var(--bb-nav-link-hover-bg, rgba(255, 255, 255, 0.06));
}

.ob-root-option.active {
  background: var(--bb-accent-soft, rgba(77, 163, 255, 0.14));
  box-shadow: inset 2px 0 0 var(--bb-accent, #4da3ff);
}

.ob-root-option-head {
  display: flex;
  align-items: center;
  gap: 6px;
  min-width: 0;
}

.ob-root-option-name {
  font-family: var(--bb-font-mono, monospace);
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ob-root-option-count {
  margin-left: auto;
  color: var(--bb-text-muted, #7d8795);
}

.ob-root-option-meta {
  color: var(--bb-text-muted, #7d8795);
  font-size: var(--bb-font-size-xs, 11px);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ob-menu-empty,
.ob-menu-more {
  margin: 4px 6px;
  color: var(--bb-text-muted, #7d8795);
  font-size: var(--bb-font-size-xs, 11px);
}

/* Chips / inputs / buttons ---------------------------------------------- */
.ob-chip {
  display: inline-flex;
  align-items: center;
  max-width: 260px;
  padding: 1px 6px;
  color: var(--bb-text-secondary, #b3bac6);
  background: var(--bb-surface-inset, #0d0f13);
  border: 1px solid var(--bb-border-subtle, #1f242c);
  border-radius: var(--bb-radius-pill, 999px);
  font-size: var(--bb-font-size-xs, 11px);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ob-mono {
  font-family: var(--bb-font-mono, monospace);
}

.ob-input,
.ob-select {
  padding: 3px 7px;
  color: var(--bb-text, #e8ebf0);
  background: var(--bb-surface-inset, #0d0f13);
  border: 1px solid var(--bb-border, #29303a);
  border-radius: var(--bb-radius-sm, 6px);
  font: inherit;
}

.ob-input::placeholder {
  color: var(--bb-text-faint, #5c6675);
}

.ob-input:focus,
.ob-select:focus {
  outline: none;
  border-color: var(--bb-accent, #4da3ff);
  box-shadow: 0 0 0 1px var(--bb-accent-soft, rgba(77, 163, 255, 0.14));
}

.ob-search {
  min-width: 180px;
  flex: 1 1 220px;
  max-width: 320px;
}

.ob-preset-name {
  width: 130px;
}

.ob-check {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  color: var(--bb-text-secondary, #b3bac6);
  white-space: nowrap;
  font-size: var(--bb-font-size-xs, 11px);
}

.ob-check input {
  accent-color: var(--bb-accent, #4da3ff);
}

.ob-btn {
  padding: 2px 8px;
  color: var(--bb-text-secondary, #b3bac6);
  background: var(--bb-surface-raised, #1a1f26);
  border: 1px solid var(--bb-border, #29303a);
  border-radius: var(--bb-radius-sm, 6px);
  cursor: pointer;
  font: inherit;
  font-size: var(--bb-font-size-xs, 11px);
  white-space: nowrap;
}

.ob-btn:hover:not(:disabled) {
  color: var(--bb-text, #e8ebf0);
  border-color: var(--bb-border-strong, #3a4350);
}

.ob-btn:disabled {
  color: var(--bb-text-faint, #5c6675);
  cursor: not-allowed;
  opacity: 0.7;
}

.ob-btn-primary {
  color: var(--bb-accent, #4da3ff);
  border-color: var(--bb-accent, #4da3ff);
  background: var(--bb-accent-soft, rgba(77, 163, 255, 0.14));
}

.ob-btn-primary:hover:not(:disabled) {
  color: var(--bb-accent-strong, #7cbcff);
  background: var(--bb-accent-soft-strong, rgba(77, 163, 255, 0.24));
}

.ob-count,
.ob-presets-label {
  color: var(--bb-text-muted, #7d8795);
  font-size: var(--bb-font-size-xs, 11px);
  white-space: nowrap;
}

.ob-badge {
  display: inline-flex;
  align-items: center;
  padding: 0 5px;
  border-radius: var(--bb-radius-pill, 999px);
  font-size: 10px;
  line-height: 15px;
  white-space: nowrap;
  border: 1px solid var(--bb-border-subtle, #1f242c);
  color: var(--bb-text-secondary, #b3bac6);
  background: var(--bb-surface-inset, #0d0f13);
}

.ob-badge-preferred {
  color: var(--bb-accent-strong, #7cbcff);
  background: var(--bb-accent-soft, rgba(77, 163, 255, 0.14));
  border-color: var(--bb-accent, #4da3ff);
}

.ob-badge-pointer {
  color: var(--bb-info, #60a5fa);
  background: var(--bb-info-soft, rgba(96, 165, 250, 0.14));
  border-color: var(--bb-info, #60a5fa);
}

.ob-badge-array {
  color: var(--bb-success, #3ddc84);
  background: var(--bb-success-soft, rgba(61, 220, 132, 0.14));
  border-color: var(--bb-success, #3ddc84);
}

/* Banners / empty states ------------------------------------------------ */
.ob-banner {
  padding: 3px 8px;
  border-radius: var(--bb-radius-sm, 6px);
  border: 1px solid var(--bb-border, #29303a);
  font-size: var(--bb-font-size-xs, 11px);
}

.ob-banner-warning {
  color: var(--bb-warning, #fbbf24);
  background: var(--bb-warning-soft, rgba(251, 191, 36, 0.14));
  border-color: var(--bb-warning, #fbbf24);
}

.ob-banner-info {
  color: var(--bb-info, #60a5fa);
  background: var(--bb-info-soft, rgba(96, 165, 250, 0.14));
  border-color: var(--bb-info, #60a5fa);
}

.ob-banner-notice {
  color: var(--bb-text-secondary, #b3bac6);
  background: var(--bb-surface-inset, #0d0f13);
}

.ob-empty {
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: 6px;
  min-height: 180px;
  padding: 16px;
  color: var(--bb-text-muted, #7d8795);
  border: 1px dashed var(--bb-border-strong, #3a4350);
  border-radius: var(--bb-radius-md, 10px);
  background: var(--bb-surface-inset, #0d0f13);
}

.ob-empty-title {
  margin: 0;
  color: var(--bb-text, #e8ebf0);
  font-size: var(--bb-font-size-lg, 15px);
}

.ob-empty-body {
  margin: 0;
  max-width: 640px;
  line-height: 1.5;
}

/* Object table ---------------------------------------------------------- */
.ob-main {
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  min-height: 0;
  border: 1px solid var(--bb-border, #29303a);
  border-radius: var(--bb-radius-sm, 6px);
  background: var(--bb-surface, #14171c);
  overflow: hidden;
}

.ob-head,
.ob-row {
  display: grid;
  grid-template-columns: 1.1rem 1.2rem minmax(0, 2fr) minmax(0, 1.25fr) 7.8rem minmax(0, 1.1fr) 3.2rem;
  align-items: center;
  gap: 2px;
  padding: 0 6px;
}

.ob-head {
  flex: 0 0 auto;
  height: 24px;
  color: var(--bb-text-muted, #7d8795);
  background: var(--bb-surface-inset, #0d0f13);
  border-bottom: 1px solid var(--bb-border, #29303a);
  font-size: var(--bb-font-size-xs, 11px);
  font-weight: 600;
}

.ob-tree {
  position: relative;
  flex: 1 1 auto;
  min-height: 240px;
  overflow: auto;
  outline: none;
}

.ob-canvas {
  position: relative;
  width: 100%;
}

.ob-row {
  position: absolute;
  left: 0;
  right: 0;
  height: 24px;
  color: var(--bb-text, #e8ebf0);
  border-bottom: 1px solid var(--bb-border-subtle, #1f242c);
  cursor: pointer;
  font-size: var(--bb-font-size-xs, 11px);
  white-space: nowrap;
}

.ob-row:hover {
  background: var(--bb-nav-link-hover-bg, rgba(255, 255, 255, 0.06));
}

.ob-row.active {
  background: var(--bb-accent-soft, rgba(77, 163, 255, 0.14));
  box-shadow: inset 2px 0 0 var(--bb-accent, #4da3ff);
}

.ob-row.marked {
  background: var(--bb-accent-soft-strong, rgba(77, 163, 255, 0.24));
}

.ob-row.curve-selected .ob-added {
  color: var(--bb-success, #3ddc84);
}

.ob-row.watched .ob-name-text {
  color: var(--bb-text, #e8ebf0);
}

.ob-row.aggregate .ob-name-text {
  font-weight: 600;
}

.ob-row.pointer .ob-ptr {
  color: var(--bb-info, #60a5fa);
}

.ob-row:focus-visible {
  outline: 2px solid var(--bb-accent, #4da3ff);
  outline-offset: -2px;
}

.ob-cell {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ob-cell-caret,
.ob-cell-star,
.ob-cell-actions {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 2px;
  overflow: visible;
}

.ob-caret,
.ob-star,
.ob-mini {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  padding: 0;
  color: var(--bb-text-muted, #7d8795);
  background: transparent;
  border: 0;
  border-radius: var(--bb-radius-xs, 4px);
  cursor: pointer;
  font: inherit;
  line-height: 1;
}

.ob-caret:hover,
.ob-mini:hover:not(:disabled) {
  color: var(--bb-text, #e8ebf0);
  background: var(--bb-surface-raised, #1a1f26);
}

.ob-star:hover {
  color: var(--bb-warning, #fbbf24);
}

.ob-star.on {
  color: var(--bb-warning, #fbbf24);
}

.ob-caret-static {
  color: var(--bb-text-faint, #5c6675);
}

.ob-mini:disabled {
  color: var(--bb-text-faint, #5c6675);
  cursor: not-allowed;
  opacity: 0.55;
}

.ob-name-text {
  overflow: hidden;
  text-overflow: ellipsis;
}

.ob-ptr {
  margin-left: 2px;
  font-family: var(--bb-font-mono, monospace);
}

.ob-cap {
  margin-left: 4px;
  padding: 0 4px;
  color: var(--bb-text-muted, #7d8795);
  background: var(--bb-surface-inset, #0d0f13);
  border-radius: var(--bb-radius-xs, 4px);
  font-size: 10px;
}

.ob-added {
  margin-left: 4px;
  font-size: 9px;
}

.ob-cell-type,
.ob-cell-addr {
  color: var(--bb-text-muted, #7d8795);
}

.ob-cell-value {
  color: var(--bb-text, #e8ebf0);
}

.ob-cell-value.stale,
.ob-row.is-stale .ob-cell-value {
  color: var(--bb-text-faint, #5c6675);
  font-style: italic;
}

.ob-tree-empty {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 16px;
  color: var(--bb-text-muted, #7d8795);
  pointer-events: none;
}

.ob-foot {
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  gap: 10px;
  min-height: 22px;
  padding: 2px 8px;
  color: var(--bb-text-muted, #7d8795);
  background: var(--bb-surface-inset, #0d0f13);
  border-top: 1px solid var(--bb-border, #29303a);
  font-size: var(--bb-font-size-xs, 11px);
}

.ob-foot-warn {
  color: var(--bb-warning, #fbbf24);
}

.ob-foot-hint {
  color: var(--bb-text-faint, #5c6675);
}
</style>
