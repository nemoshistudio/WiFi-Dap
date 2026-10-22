<!--
  SourceBrowser：本地项目文件夹浏览 + 源码视图点击变量（Ozone 式选变量）。

  独立组件，不依赖 DebugCurve/i18n：
    - 自己负责选择项目目录（File System Access API 或 webkitdirectory 回退）；
    - props.project 也可由父组件直接传入（推荐：加载一次，Source tab 复用）；
    - labels 由父组件覆盖（默认英文，避免硬编码中文触发 i18n 契约检查）；
    - 变量映射/高亮/候选全部由 @/utils/sourceProject 纯函数完成。

  详见 docs/source-browser.md。
-->
<template>
  <section ref="rootRef" class="source-browser">
    <header class="sb-toolbar">
      <button type="button" class="sb-btn sb-btn-primary" :disabled="loading" @click="onOpenProject">
        {{ directoryPickerAvailable ? labels.openProject : labels.openProjectFallback }}
      </button>
      <button v-if="currentProject" type="button" class="sb-btn" :disabled="loading" @click="onOpenProject">
        {{ labels.changeFolder }}
      </button>
      <button type="button" class="sb-btn" :disabled="loading || !!elfBusyPath" @click="onChooseElf">
        {{ labels.chooseElf }}
      </button>
      <input
          ref="dirInputRef"
          class="sb-hidden"
          type="file"
          webkitdirectory
          multiple
          @change="onDirectoryInputChange"
      />
      <input ref="elfInputRef" class="sb-hidden" type="file" accept=".elf,.axf,.out" @change="onElfInputChange"/>

      <span v-if="loading" class="sb-status">{{ scanLabel }}</span>
      <template v-else-if="currentProject">
        <span class="sb-chip" :title="currentProject.rootName || '-'">
          {{ formatLabel(labels.projectRoot, {name: currentProject.rootName || '-'}) }}
        </span>
        <span class="sb-chip">{{ formatLabel(labels.filesCount, {count: currentProject.sourceCount}) }}</span>
        <span v-if="matchResult" class="sb-chip" :class="{'sb-chip-warn': matchResult.unmatchedCount > 0}">
          {{ formatLabel(labels.matchedSummary, {matched: matchResult.matchedCount, total: matchResult.total}) }}
        </span>
        <span v-if="matchResult && matchResult.ambiguousCount > 0" class="sb-chip sb-chip-warn">
          {{ formatLabel(labels.ambiguousSummary, {count: matchResult.ambiguousCount}) }}
        </span>
        <span class="sb-spacer"></span>
        <span class="sb-privacy" :title="labels.privacyNote">{{ labels.privacyNote }}</span>
      </template>
    </header>

    <div v-if="currentProject && detectedElf" class="sb-elfbar">
      <span class="sb-elf-icon">ELF</span>
      <span class="sb-elf-name" :title="detectedElf.path">{{ formatLabel(labels.elfDetected, {name: detectedElf.path}) }}</span>
      <span class="sb-elf-time">{{ formatLabel(labels.elfTime, {time: formatTimestamp(detectedElf.lastModified)}) }}</span>
      <button type="button" class="sb-btn sb-btn-small" :disabled="!!elfBusyPath" @click="onUseDetectedElf">
        {{ labels.useElf }}
      </button>
      <span v-if="elfError" class="sb-error-inline">{{ elfError }}</span>
    </div>
    <div v-else-if="currentProject && !loading" class="sb-elfbar sb-elfbar-muted">
      <span>{{ labels.elfNone }}</span>
    </div>

    <div v-if="projectError" class="sb-banner sb-banner-error">
      <span>{{ projectError }}</span>
      <button v-if="needsInputFallback" type="button" class="sb-btn sb-btn-small" @click="openDirectoryInput">
        {{ labels.openProjectFallback }}
      </button>
    </div>
    <div v-if="pickerNotice" class="sb-banner sb-banner-info">{{ labels.secureContextHint }}</div>

    <div v-if="!currentProject" class="sb-empty">
      <h3 class="sb-empty-title">{{ labels.noProjectTitle }}</h3>
      <p class="sb-empty-body">{{ labels.noProjectBody }}</p>
      <p class="sb-empty-hint">{{ labels.privacyNote }}</p>
      <p v-if="directoryPickerAvailable" class="sb-empty-hint">{{ labels.limitsHintDefault }}</p>
      <p v-else class="sb-empty-hint">{{ labels.secureContextHint }}</p>
    </div>

    <div v-else-if="currentProject.sourceCount === 0" class="sb-empty">
      <h3 class="sb-empty-title">{{ labels.noSourceFiles }}</h3>
      <p class="sb-empty-body">{{ labels.noSourceFilesHint }}</p>
      <p class="sb-empty-hint">{{ limitsText }}</p>
    </div>

    <div v-else class="sb-body">
      <aside class="sb-sidebar">
        <div class="sb-sidebar-head">
          <input v-model="fileSearch" class="sb-search" type="search" :placeholder="labels.searchPlaceholder"/>
          <div class="sb-filter-row">
            <label class="sb-toggle" :title="labels.onlyFilesWithVariables">
              <input v-model="showAllFiles" type="checkbox"/>
              <span>{{ labels.showAllProjectFiles }}</span>
            </label>
            <span class="sb-search-count">{{ searchCountText }}</span>
          </div>
        </div>
        <div ref="treeViewportRef" class="sb-tree" @scroll.passive="onTreeScroll">
          <div class="sb-tree-canvas" :style="{height: `${treeTotalHeight}px`}">
            <template v-for="row in visibleTreeRows" :key="`${row.kind}:${row.path}`">
              <button
                  v-if="row.kind === 'directory'"
                  type="button"
                  class="sb-tree-row sb-tree-dir"
                  :style="{top: `${row.top}px`, paddingLeft: `${8 + row.depth * 12}px`}"
                  @click="toggleDirectory(row.path)"
              >
                <span class="sb-caret">{{ expandedDirs.has(row.path) ? '▾' : '▸' }}</span>
                <span class="sb-tree-name">{{ row.name }}</span>
                <span class="sb-tree-badge">{{ row.totalFileCount }}</span>
              </button>
              <button
                  v-else
                  type="button"
                  class="sb-tree-row sb-tree-file"
                  :class="{active: row.path === openFilePath, 'has-vars': fileVariableCount(row.path) > 0}"
                  :style="{top: `${row.top}px`, paddingLeft: `${8 + row.depth * 12}px`}"
                  :title="row.path"
                  @click="openFile(row.path)"
              >
                <span class="sb-file-dot">•</span>
                <span class="sb-tree-name">{{ row.name }}</span>
                <span v-if="fileVariableCount(row.path) > 0" class="sb-tree-badge sb-tree-badge-var">
                  {{ formatLabel(labels.fileVariables, {count: fileVariableCount(row.path)}) }}
                </span>
              </button>
            </template>
          </div>
          <div v-if="treeRows.length === 0" class="sb-sidebar-empty">
            <template v-if="fileSearch.trim()">{{ labels.noSearchResults }}</template>
            <template v-else-if="filesWithVariablesMode">{{ labels.noFilesWithVariables }}</template>
            <template v-else>{{ labels.noSearchResults }}</template>
          </div>
        </div>
        <details v-if="matchResult && matchResult.unmatchedCount > 0" class="sb-unmatched">
          <summary>{{ formatLabel(labels.unmatchedSummary, {count: matchResult.unmatchedCount}) }}</summary>
          <p class="sb-unmatched-hint">{{ labels.unmatchedHint }}</p>
          <ul class="sb-unmatched-list">
            <li v-for="item in unmatchedPreview" :key="item.key">
              <span class="sb-unmatched-name">{{ item.path }}</span>
              <span class="sb-unmatched-file">{{ item.variable.relativeFile ?? item.variable.file ?? '-' }}</span>
            </li>
          </ul>
          <button
              v-if="matchResult.unmatchedCount > unmatchedPreview.length"
              type="button"
              class="sb-link"
              @click="showAllUnmatched = true"
          >{{ labels.showMore }}
          </button>
          <button v-else-if="showAllUnmatched" type="button" class="sb-link" @click="showAllUnmatched = false">
            {{ labels.showLess }}
          </button>
        </details>
      </aside>

      <main class="sb-main">
        <div v-if="!openFilePath" class="sb-empty sb-empty-main">
          <h3 class="sb-empty-title">{{ labels.selectFileTitle }}</h3>
          <p class="sb-empty-body">{{ labels.selectFileHint }}</p>
          <p v-if="!hasVariables" class="sb-empty-hint">{{ labels.noVariablesHint }}</p>
        </div>
        <template v-else>
          <div class="sb-code-head">
            <div class="sb-code-path" :title="openFilePath">{{ openFilePath }}</div>
            <div class="sb-code-actions">
              <select v-if="jumpRefs.length > 0" class="sb-jump" :value="''" @change="onJumpChange">
                <option value="">{{ labels.jumpToVariable }}</option>
                <option v-for="ref in jumpRefs" :key="ref.key" :value="ref.key">{{ jumpOptionText(ref) }}</option>
              </select>
              <button
                  type="button"
                  class="sb-btn sb-btn-small"
                  :disabled="jumpRefs.length === 0"
                  :title="labels.prevVariable"
                  @click="jumpBy(-1)"
              >◀
              </button>
              <button
                  type="button"
                  class="sb-btn sb-btn-small"
                  :disabled="jumpRefs.length === 0"
                  :title="labels.nextVariable"
                  @click="jumpBy(1)"
              >▶
              </button>
              <span v-if="fileRefs.length" class="sb-chip">{{ formatLabel(labels.fileVariables, {count: fileRefs.length}) }}</span>
            </div>
          </div>
          <div v-if="openError" class="sb-banner sb-banner-error">
            <span>{{ formatLabel(labels.readFailed, {err: openError}) }}</span>
            <button type="button" class="sb-btn sb-btn-small" @click="retryOpen">{{ labels.retryRead }}</button>
          </div>
          <div v-else-if="openLoading" class="sb-msg">{{ labels.readingFile }}</div>
          <div v-else-if="fileRefs.length === 0" class="sb-banner sb-banner-info">{{ labels.noMatchInFile }}</div>
          <div
              v-if="!openError"
              ref="viewportRef"
              class="sb-viewport"
              @scroll.passive="onScroll"
              @click="onCodeClick"
              @mouseover="onCodeMouseOver"
              @mouseleave="hideHover"
          >
            <div class="sb-canvas" :style="{height: `${virtualWindow.totalHeight}px`, width: canvasWidth}">
              <div
                  v-for="row in visibleRows"
                  :key="row.index"
                  class="sb-line"
                  :class="{
                    'sb-line-vars': row.hasVariables,
                    'sb-line-selected': row.selected,
                    'sb-line-active': row.active,
                    'sb-line-pc': row.pcLine,
                    'sb-line-flash': row.index + 1 === flashLine,
                  }"
                  :style="{top: `${row.top}px`, height: `${ROW_HEIGHT}px`}"
              >
                <button
                    type="button"
                    class="sb-gutter"
                    :class="{'no-address': row.gutterAddress === undefined}"
                    :title="row.gutterTitle"
                    :aria-label="row.gutterTitle"
                    data-gutter
                    :data-address="row.gutterAddress"
                    @click.stop="onGutterClick(row.index + 1)"
                >{{ row.index + 1 }}</button>
                <span class="sb-line-text"><span
                    v-for="(token, tokenIndex) in row.tokens"
                    :key="tokenIndex"
                    :class="[
                      tokenClass(token),
                      token.kind === 'variable' && row.liveIdentifiers.has(token.variable ?? '') ? 'sb-token-live' : '',
                      token.kind === 'variable' && row.activeIdentifiers.has(token.variable ?? '') ? 'sb-token-active' : '',
                    ]"
                    :data-variable="token.kind === 'variable' ? token.variable : null"
                    :data-line="row.index + 1"
                >{{ token.text }}</span></span>
                <span v-if="row.pcLine" class="sb-pc-badge" :title="activePcLineLabel()">{{ activePcLineLabel() }}</span>
                <span v-if="row.liveEntries.length > 0" class="sb-live-values" :title="labels.liveValues">
                  <span
                      v-for="entry in row.liveEntries"
                      :key="entry.key"
                      class="sb-live-value"
                      :class="{stale: entry.value.stale}"
                      :title="entry.path"
                  >
                    <span class="sb-live-dot" :style="{background: entry.value.color || 'var(--sb-accent)'}"></span>
                    <span class="sb-live-name">{{ entry.identifier }}</span>
                    <span class="sb-live-text">{{ entry.value.text }}</span>
                    <span v-if="entry.value.stale" class="sb-live-stale">{{ labels.liveValueStale }}</span>
                  </span>
                </span>
              </div>
            </div>
          </div>
        </template>
      </main>
    </div>

    <div v-if="hover && hoverPrimary" class="sb-tooltip" :style="{left: `${hover.x}px`, top: `${hover.y}px`}">
      <div class="sb-tooltip-head">
        <span class="sb-tooltip-name">{{ hoverPrimary.ref.identifier }}</span>
        <span class="sb-tooltip-sample" :class="{ok: hoverPrimary.sampleable}">
          {{ hoverPrimary.sampleable ? labels.sampleable : labels.unsampleable }}
        </span>
      </div>
      <div
          v-for="candidate in hover.candidates.slice(0, 3)"
          :key="candidate.ref.key"
          class="sb-tooltip-row"
      >
        <span class="sb-tooltip-path">{{ candidate.ref.path }}</span>
        <span v-if="candidate.ref.variable.typeName" class="sb-tooltip-type">
          {{ formatLabel(labels.typeLabel, {type: candidate.ref.variable.typeName}) }}
        </span>
        <span v-if="candidate.ref.line" class="sb-tooltip-src">
          {{ formatLabel(labels.fileLine, {file: shortFileName(candidate.ref.variable.relativeFile ?? candidate.ref.variable.file ?? ''), line: candidate.ref.line}) }}
        </span>
      </div>
      <div v-if="hover.candidates.length > 3" class="sb-tooltip-more">
        {{ formatLabel(labels.hoverMore, {count: hover.candidates.length - 3}) }}
      </div>
      <div v-if="hoverLiveValue" class="sb-tooltip-row sb-tooltip-live">
        <span class="sb-live-dot" :style="{background: hoverLiveValue.color || 'var(--sb-accent)'}"></span>
        <span class="sb-tooltip-live-text">{{ hoverLiveValue.text }}</span>
        <span v-if="hoverLiveValue.stale" class="sb-live-stale">{{ labels.liveValueStale }}</span>
      </div>
    </div>

    <div
        v-if="candidateMenu"
        ref="candidateMenuRef"
        class="sb-popover"
        :style="{left: `${candidateMenu.x}px`, top: `${candidateMenu.y}px`}"
        @click.stop
    >
      <div class="sb-popover-head">
        <span>{{ formatLabel(labels.candidateTitle, {count: candidateMenu.candidates.length}) }}</span>
        <button type="button" class="sb-link" @click="candidateMenu = null">{{ labels.close }}</button>
      </div>
      <button
          v-for="(candidate, index) in candidateMenu.candidates"
          :key="candidate.ref.key"
          type="button"
          class="sb-candidate"
          @click="chooseCandidate(index)"
      >
        <span class="sb-candidate-path">{{ candidate.ref.path }}</span>
        <span class="sb-candidate-meta">
          <template v-if="candidate.ref.variable.typeName">{{ candidate.ref.variable.typeName }}</template>
          <template v-if="candidate.ref.line"> · {{ formatLabel(labels.lineLabel, {line: candidate.ref.line}) }}</template>
        </span>
        <span class="sb-candidate-scope">{{ scopeLabel(candidate.scope) }}</span>
        <span class="sb-candidate-sample" :class="{ok: candidate.sampleable}">
          {{ candidate.sampleable ? labels.sampleable : labels.unsampleable }}
        </span>
      </button>
    </div>
  </section>
</template>

<script setup lang="ts" generic="V extends SourceVariableLike = SourceVariableLike">
import {computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, watch} from 'vue'
import {
  basename,
  buildSourceTree,
  collectIdentifiers,
  collectLiveValueEntries,
  computeBlockCommentStates,
  computeVirtualWindow,
  createSourceFileIndex,
  filterSourceFiles,
  findVariableCandidates,
  findVariableLocationByKey,
  flattenSourceTree,
  formatFileSize,
  groupReferencesByLine,
  matchVariablesToFiles,
  normalizeRelativePath,
  pickInitialSourceFile,
  refsForFile,
  resolveLineAddress,
  searchSourceFiles,
  tokenizeCodeLine,
  variableLines,
  walkSourceVariables,
  type CodeToken,
  type FileVariableRef,
  type SourceFileMeta,
  type SourceLineAddressMap,
  type SourceLineClickPayload,
  type SourceLiveValue,
  type SourceLiveValueEntry,
  type SourceLiveValueMap,
  type SourceTreeRow,
  type SourceTreeNode,
  type SourceVariableEntry,
  type SourceVariableLike,
  type SourceVariableSelectionPayload,
  type VariableCandidate,
  type VariableFileMatchResult,
} from '@/utils/sourceProject'
import {formatSourceBrowserLabel, resolveSourceBrowserLabels, type SourceBrowserLabels} from './labels'
import {
  createProjectFromDirectoryHandle,
  createProjectFromFileList,
  getDirectoryPickerAvailability,
  isAbortError,
  pickProjectDirectory,
  supportsDirectoryPicker,
  type SourceProject,
} from './projectLoader'
import {resolveFileLineTarget, type SourceActiveFileLine} from './lineTarget'

defineOptions({name: 'SourceBrowser'})

const props = defineProps<{
  /** 由父组件加载/缓存的项目；为空时组件自己在内部加载 */
  project?: SourceProject | null
  /** DWARF 变量（VariableNode[] 或精简接口均可）；结构上需含 name/file/line 等 */
  variables?: readonly V[]
  /** 已选变量的 key（与 DebugCurve CurveSeries.key 一致：`path@address`） */
  selectedKeys?: readonly string[]
  /** 文案覆盖（默认英文；父组件可传中文，避免本组件直接依赖 i18n） */
  labels?: Partial<SourceBrowserLabels>
  /** 初始打开的文件（项目相对路径） */
  initialFile?: string | null
  /**
   * 实时值（task-29）：key 与 selectedKeys/CurveSeries.key 一致（`path@address`）。
   * 组件只负责展示（token 高亮 + 行尾值），不做采样/解码；stale 灰显。
   */
  liveValues?: SourceLiveValueMap
  /**
   * 当前活动变量 key：源码中高亮“当前选中/正在查看”的变量（可与 selectedKeys 不同）。
   * 父页面可在点击曲线时传入，并调用 expose 的 scrollToVariableByKey() 跳转。
   */
  activeKey?: string | null
  /**
   * 行地址表（task-29，可选）：项目相对路径 -> 行号 -> 可执行地址。
   * 父页面有 ELF 行表/符号地址时传入，`line-click` 才会携带 address；否则只给 line。
   */
  lineAddresses?: SourceLineAddressMap | null
  /**
   * 当前 PC / 活动源码行（task-43）：声明式高亮/跟随。
   * 父组件把 task-42 `pcToLine(...)` 得到的文件+行传来即可；每次变化会解析项目文件、必要时打开文件并滚动到该行，
   * 以 PC 行样式高亮（不清除 selectedKeys/liveValues）。传 null 停止跟随/清除高亮。
   */
  activeFileLine?: SourceActiveFileLine | null
}>()

const emit = defineEmits<{
  (event: 'select-variable', payload: SourceVariableSelectionPayload<V>): void
  (event: 'project-loaded', project: SourceProject): void
  (event: 'elf-file', file: File): void
  (event: 'file-open', path: string): void
  (event: 'error', message: string): void
  /** 点击源码行号 gutter（断点槽）：父页面可据此设断点/触发采样；address 仅在行地址表提供时存在 */
  (event: 'line-click', payload: SourceLineClickPayload): void
}>()

const ROW_HEIGHT = 20
const OVERSCAN = 6
const DEFAULT_VIEWPORT_HEIGHT = 480
/** 文件树行高 / 视线外预渲染行（虚拟树，2000+ 文件只渲染可见窗口）。 */
const TREE_ROW_HEIGHT = 24
const TREE_OVERSCAN = 8
const DEFAULT_TREE_HEIGHT = 320

const labels = computed<SourceBrowserLabels>(() => resolveSourceBrowserLabels(props.labels))
const directoryPickerAvailable = computed<boolean>(() => supportsDirectoryPicker())
const availability = computed(() => getDirectoryPickerAvailability())

const rootRef = ref<HTMLElement | null>(null)
const dirInputRef = ref<HTMLInputElement | null>(null)
const elfInputRef = ref<HTMLInputElement | null>(null)
const viewportRef = ref<HTMLDivElement | null>(null)
const candidateMenuRef = ref<HTMLDivElement | null>(null)

/** task-43：当前高亮/跟随的源码行（prop activeFileLine 或 expose scrollToLine/highlightLine 驱动）。 */
const activeFileLineState = shallowRef<{filePath: string; line: number; address?: number} | null>(null)
let lastActiveFileLineSignature = ''
/** task-43 labels 的英文 fallback（未放入 DEFAULT，避免破坏 task-26 的 labels 完整性契约）。 */
const ACTIVE_PC_LINE_FALLBACK = 'PC'
const LINE_NO_ADDRESS_FALLBACK = 'Line {line}: no executable address in the ELF line table'

const loadedProject = shallowRef<SourceProject | null>(null)
const currentProject = computed<SourceProject | null>(() => props.project ?? loadedProject.value)

const loading = ref(false)
const scannedEntries = ref(0)
const projectError = ref('')
const pickerNotice = ref(false)
const needsInputFallback = ref(false)
const elfBusyPath = ref('')
const elfError = ref('')

// ---- 变量映射 -------------------------------------------------------------

const variables = computed<readonly V[]>(() => props.variables ?? [])
const hasVariables = computed<boolean>(() => variables.value.length > 0)
const variableEntries = computed<SourceVariableEntry<V>[]>(() => walkSourceVariables<V>(variables.value))
const fileIndex = computed(() => {
  const project = currentProject.value
  return project ? createSourceFileIndex(project.files) : null
})
const matchResult = computed<VariableFileMatchResult<V> | null>(() => {
  const index = fileIndex.value
  if (!index) return null
  return matchVariablesToFiles<V>(variableEntries.value, index)
})
const refsByFile = computed<Map<string, FileVariableRef<V>[]>>(
    () => matchResult.value?.refsByFile ?? new Map<string, FileVariableRef<V>[]>(),
)
const variableCounts = computed<Map<string, number>>(() => {
  const counts = new Map<string, number>()
  for (const [path, refs] of refsByFile.value) counts.set(normalizeRelativePath(path), refs.length)
  return counts
})
const selectedKeySet = computed<Set<string>>(() => new Set(props.selectedKeys ?? []))
/** 内部活动 key（点击变量 / scrollToVariableByKey 时更新；父组件 activeKey 变化时同步）。 */
const internalActiveKey = ref<string | null>(null)
/** 有效 active key：组件内交互（点击/跳转）优先，其次是父组件传入的 activeKey。 */
const activeKeyValue = computed<string | null>(() => internalActiveKey.value ?? props.activeKey ?? null)
// 父组件更新 activeKey（如点击另一条曲线）时，覆盖内部临时高亮。
watch(() => props.activeKey, (key) => {
  internalActiveKey.value = key ?? null
}, {immediate: true})
const unmatchedPreview = computed<SourceVariableEntry<V>[]>(() => {
  const unmatched = matchResult.value?.unmatched ?? []
  return unmatched.slice(0, showAllUnmatched.value ? 500 : 20)
})
const showAllUnmatched = ref(false)

function fileVariableCount(path: string): number {
  return variableCounts.value.get(normalizeRelativePath(path)) ?? 0
}

// ---- 目录树 / 搜索（默认只显示有变量的文件 + 虚拟滚动） ------------------

const fileSearch = ref('')
const expandedDirs = ref<ReadonlySet<string>>(new Set())
/** false（默认）= Files with variables；true = 显示全部项目文件。 */
const showAllFiles = ref(false)

/** 已匹配到变量的文件路径集合（来自 refsByFile，路径已规范化）。 */
const matchedFilePaths = computed<Set<string>>(() => new Set(refsByFile.value.keys()))
/**
 * 默认视图是否生效：已加载变量且至少一个文件匹配。
 * 无 ELF / 全部未匹配时回退显示全部项目文件（仍可浏览源码），并显示切换提示。
 */
const filesWithVariablesMode = computed<boolean>(
    () => !showAllFiles.value && hasVariables.value && matchedFilePaths.value.size > 0,
)
/** 当前文件树/搜索的数据源（过滤后 或 全部项目文件）。 */
const visibleProjectFiles = computed<SourceFileMeta[]>(() => {
  const project = currentProject.value
  if (!project) return []
  if (!filesWithVariablesMode.value) return project.files
  return filterSourceFiles(project.files, matchedFilePaths.value)
})
/** 当前视图的目录树（过滤模式下目录/计数只统计有变量的文件）。 */
const treeSource = computed<SourceTreeNode | null>(() => {
  const project = currentProject.value
  if (!project) return null
  if (!filesWithVariablesMode.value) return project.tree
  return buildSourceTree(visibleProjectFiles.value, project.rootName)
})
const treeRows = computed<SourceTreeRow[]>(() => {
  const query = fileSearch.value.trim()
  if (query) {
    return searchSourceFiles(visibleProjectFiles.value, query, Math.max(300, visibleProjectFiles.value.length)).map((file) => ({
      kind: 'file' as const,
      path: file.path,
      name: file.path,
      depth: 0,
      file,
    }))
  }
  const tree = treeSource.value
  return tree ? flattenSourceTree(tree, expandedDirs.value) : []
})

// 虚拟树：只渲染侧栏可见窗口（2000+ 文件/展开大目录时不创建全部 DOM）
const treeViewportRef = ref<HTMLDivElement | null>(null)
const treeScrollTop = ref(0)
const treeViewportHeight = ref(DEFAULT_TREE_HEIGHT)
const treeVirtualWindow = computed(() => computeVirtualWindow({
  scrollTop: treeScrollTop.value,
  viewportHeight: treeViewportHeight.value || DEFAULT_TREE_HEIGHT,
  rowHeight: TREE_ROW_HEIGHT,
  lineCount: treeRows.value.length,
  overscan: TREE_OVERSCAN,
}))
const treeTotalHeight = computed<number>(() => treeRows.value.length * TREE_ROW_HEIGHT)
const visibleTreeRows = computed<Array<SourceTreeRow & { top: number }>>(() => {
  const window = treeVirtualWindow.value
  return treeRows.value
      .slice(window.start, window.end)
      .map((row, offset) => ({...row, top: (window.start + offset) * TREE_ROW_HEIGHT}))
})

const searchCountText = computed<string>(() => {
  const project = currentProject.value
  if (!project) return ''
  const query = fileSearch.value.trim()
  if (query) {
    return formatLabel(labels.value.searchResultsCount, {
      shown: treeRows.value.length,
      total: visibleProjectFiles.value.length,
    })
  }
  if (filesWithVariablesMode.value) {
    return formatLabel(labels.value.searchResultsCount, {
      shown: visibleProjectFiles.value.length,
      total: project.sourceCount,
    })
  }
  return formatLabel(labels.value.filesCount, {count: project.sourceCount})
})

const limitsText = computed<string>(() => {
  const limits = currentProject.value?.limits
  if (!limits) return ''
  return formatLabel(labels.value.limitsHint, {
    maxFiles: limits.maxFiles,
    maxBytes: formatFileSize(limits.maxFileBytes),
  })
})

function toggleDirectory(path: string): void {
  const next = new Set(expandedDirs.value)
  if (next.has(path)) next.delete(path)
  else next.add(path)
  expandedDirs.value = next
}

function expandPathToFile(path: string): void {
  const segments = normalizeRelativePath(path).split('/')
  if (segments.length <= 1) return
  const next = new Set(expandedDirs.value)
  let prefix = ''
  for (let index = 0; index < segments.length - 1; index++) {
    prefix = prefix ? `${prefix}/${segments[index]}` : segments[index]
    next.add(prefix)
  }
  expandedDirs.value = next
}

function onTreeScroll(event: Event): void {
  const target = event.currentTarget as HTMLElement | null
  treeScrollTop.value = target?.scrollTop ?? 0
}

/** 侧栏虚拟树滚动到指定文件行（openFile / scrollToVariableByKey 联动）。 */
function scrollTreeToPath(path: string): void {
  const normalized = normalizeRelativePath(path)
  const index = treeRows.value.findIndex((row) => row.path === normalized)
  const viewport = treeViewportRef.value
  if (index < 0 || !viewport) return
  const rowTop = index * TREE_ROW_HEIGHT
  const viewHeight = treeViewportHeight.value || DEFAULT_TREE_HEIGHT
  if (rowTop < viewport.scrollTop || rowTop + TREE_ROW_HEIGHT > viewport.scrollTop + viewHeight) {
    viewport.scrollTop = Math.max(0, rowTop - Math.floor(viewHeight / 2))
    treeScrollTop.value = viewport.scrollTop
  }
}

// ---- 打开文件 / 代码视图 --------------------------------------------------

const openFilePath = ref('')
const openLines = shallowRef<string[]>([])
const lineStates = shallowRef<Uint8Array>(new Uint8Array(0))
const openLoading = ref(false)
const openError = ref('')
const maxLineLength = ref(0)
const flashLine = ref<number | null>(null)
const lineTokenCache = new Map<number, CodeToken[]>()
let openRequestId = 0
let lastAutoOpenedInitial = ''

const fileRefs = computed<FileVariableRef<V>[]>(() => refsForFile(matchResult.value, openFilePath.value))
const identifierSet = computed<Set<string>>(() => collectIdentifiers(fileRefs.value))
const lineRefs = computed(() => groupReferencesByLine(fileRefs.value))
/** 每行实时值条目（props.liveValues 与本文件变量引用求交；不参与采样逻辑）。 */
const liveEntriesByLine = computed<Map<number, SourceLiveValueEntry<V>[]>>(() => {
  const liveValues = props.liveValues
  const map = new Map<number, SourceLiveValueEntry<V>[]>()
  if (!liveValues) return map
  for (const [line, refs] of lineRefs.value) {
    const entries = collectLiveValueEntries(refs, liveValues)
    if (entries.length > 0) map.set(line, entries)
  }
  return map
})
/** 活动变量位置（跨文件）与行号：用于源码行/变量 token 的 active 高亮。 */
const activeLocation = computed(() => findVariableLocationByKey(matchResult.value, activeKeyValue.value))
const selectedLineNumbers = computed<Set<number>>(() => {
  const lines = new Set<number>()
  for (const ref of fileRefs.value) {
    if (ref.line !== undefined && selectedKeySet.value.has(ref.key)) lines.add(ref.line)
  }
  return lines
})
const jumpRefs = computed<FileVariableRef<V>[]>(() => fileRefs.value.filter((ref) => ref.line !== undefined))
const canvasWidth = computed<string>(() => {
  const characters = Math.max(60, Math.min(maxLineLength.value + 10, 4000))
  return `max(100%, ${characters}ch)`
})

const scrollTop = ref(0)
const viewportHeight = ref(DEFAULT_VIEWPORT_HEIGHT)
let resizeObserver: ResizeObserver | null = null
let treeResizeObserver: ResizeObserver | null = null

const virtualWindow = computed(() => computeVirtualWindow({
  scrollTop: scrollTop.value,
  viewportHeight: viewportHeight.value || DEFAULT_VIEWPORT_HEIGHT,
  rowHeight: ROW_HEIGHT,
  lineCount: openLines.value.length,
  overscan: OVERSCAN,
}))

interface RenderRow {
  index: number
  top: number
  hasVariables: boolean
  selected: boolean
  active: boolean
  /** task-43：当前 PC/活动行 */
  pcLine: boolean
  /** task-43：该行可执行地址（lineAddresses 或 PC 行地址；无则 undefined） */
  gutterAddress?: number
  /** task-43：gutter hover 提示（有地址=断点提示，无地址=行表缺失提示） */
  gutterTitle: string
  liveEntries: SourceLiveValueEntry<V>[]
  liveIdentifiers: ReadonlySet<string>
  activeIdentifiers: ReadonlySet<string>
  tokens: CodeToken[]
}

const visibleRows = computed<RenderRow[]>(() => {
  const window = virtualWindow.value
  const rows: RenderRow[] = []
  const grouped = lineRefs.value
  const selected = selectedLineNumbers.value
  const names = identifierSet.value
  const liveByLine = liveEntriesByLine.value
  const active = activeLocation.value
  const pc = activeFileLineState.value
  const openedFile = openFilePath.value
  for (let index = window.start; index < window.end; index++) {
    const lineNumber = index + 1
    const lineRefList = grouped.get(lineNumber) ?? []
    const liveEntries = liveByLine.get(lineNumber) ?? []
    const liveIdentifiers = new Set(liveEntries.map((entry) => entry.identifier))
    const activeIdentifiers = new Set<string>()
    const rowActive = active !== null
        && active.ref.line === lineNumber
        && normalizeRelativePath(active.filePath) === openedFile
    if (rowActive) {
      for (const ref of lineRefList) activeIdentifiers.add(ref.identifier)
    }
    const pcLine = pc !== null && pc.line === lineNumber && pc.filePath === openedFile
    const gutterAddress = addressForLine(lineNumber)
    rows.push({
      index,
      top: index * ROW_HEIGHT,
      hasVariables: grouped.has(lineNumber),
      selected: selected.has(lineNumber),
      active: rowActive,
      pcLine,
      ...(gutterAddress !== undefined ? {gutterAddress} : {}),
      gutterTitle: gutterHint(lineNumber, gutterAddress),
      liveEntries,
      liveIdentifiers,
      activeIdentifiers,
      tokens: tokensForLine(index, names),
    })
  }
  return rows
})

watch(identifierSet, () => {
  lineTokenCache.clear()
})

function tokensForLine(index: number, names: ReadonlySet<string>): CodeToken[] {
  const cached = lineTokenCache.get(index)
  if (cached) return cached
  const result = tokenizeCodeLine(openLines.value[index] ?? '', lineStates.value[index] === 1, {variableNames: names})
  lineTokenCache.set(index, result.tokens)
  if (lineTokenCache.size > 4000) lineTokenCache.clear()
  return result.tokens
}

function tokenClass(token: CodeToken): string {
  switch (token.kind) {
    case 'variable':
      return 'sb-token-variable'
    case 'keyword':
      return 'sb-token-keyword'
    case 'string':
    case 'char':
      return 'sb-token-string'
    case 'number':
      return 'sb-token-number'
    case 'comment':
      return 'sb-token-comment'
    case 'preprocessor':
      return 'sb-token-preprocessor'
    case 'identifier':
      return 'sb-token-identifier'
    case 'punctuation':
      return 'sb-token-punctuation'
    default:
      return 'sb-token-plain'
  }
}

async function openFile(path: string, options: { line?: number } = {}): Promise<void> {
  const project = currentProject.value
  if (!project) return
  const normalized = normalizeRelativePath(path)
  if (!project.files.some((file) => file.path === normalized)) return
  const requestId = ++openRequestId
  openFilePath.value = normalized
  expandPathToFile(normalized)
  openLoading.value = true
  openError.value = ''
  hover.value = null
  candidateMenu.value = null
  openLines.value = []
  lineStates.value = new Uint8Array(0)
  lineTokenCache.clear()
  maxLineLength.value = 0
  try {
    const text = await project.readText(normalized)
    if (requestId !== openRequestId) return
    const lines = text.split(/\r\n|\r|\n/)
    openLines.value = lines
    lineStates.value = computeBlockCommentStates(lines)
    let longest = 0
    for (const line of lines) {
      if (line.length > longest) longest = line.length
    }
    maxLineLength.value = longest
    await nextTick()
    scrollTreeToPath(normalized)
    const target = options.line ?? variableLines(fileRefs.value)[0]
    if (target) scrollCodeLine(target)
    else scrollToTop()
    emit('file-open', normalized)
  } catch (error) {
    if (requestId !== openRequestId) return
    openError.value = errorMessage(error)
    emit('error', openError.value)
  } finally {
    if (requestId === openRequestId) openLoading.value = false
  }
}

function retryOpen(): void {
  if (openFilePath.value) void openFile(openFilePath.value)
}

function scrollToTop(): void {
  scrollTop.value = 0
  if (viewportRef.value) viewportRef.value.scrollTop = 0
}

/** 滚动到指定行（1 起始），并把该行短暂高亮。 */
function scrollCodeLine(line: number, highlight = true): void {
  const viewport = viewportRef.value
  if (!viewport) return
  const height = viewportHeight.value || DEFAULT_VIEWPORT_HEIGHT
  const top = Math.max(0, (line - 1) * ROW_HEIGHT - Math.floor(height / 3))
  viewport.scrollTop = top
  scrollTop.value = top
  if (highlight) flashLine.value = line
}

function onScroll(event: Event): void {
  const target = event.currentTarget as HTMLElement | null
  scrollTop.value = target?.scrollTop ?? 0
  hideHover()
}

// ---- 行号 gutter：断点请求（line-click） ---------------------------------

function activePcLineLabel(): string {
  return labels.value.activePcLine ?? ACTIVE_PC_LINE_FALLBACK
}

/** 行没有可执行地址时的 gutter hover 提示（task-43）。 */
function lineNoAddressHint(line: number): string {
  return formatLabel(labels.value.lineNoAddressHint ?? LINE_NO_ADDRESS_FALLBACK, {line})
}

function formatAddressHex(address: number): string {
  return `0x${(address >>> 0).toString(16).toUpperCase().padStart(8, '0')}`
}

/**
 * 行可执行地址（task-43）：`lineAddresses`（task-42 精确行表）优先；
 * 当前 PC 行的 `activeFileLine.address` 作为兜底（即使父组件还没生成完整行地址表）。
 */
function addressForLine(line: number): number | undefined {
  const path = openFilePath.value
  if (!path) return undefined
  const mapped = resolveLineAddress(props.lineAddresses, path, line)
  if (mapped !== undefined) return mapped
  const pc = activeFileLineState.value
  if (pc && pc.filePath === path && pc.line === line && pc.address !== undefined) return pc.address
  return undefined
}

/** gutter hover 提示：有地址显示断点提示+地址；无地址（lineAddresses 为空/该行不在行表）显示行表缺失提示。 */
function gutterHint(line: number, address: number | undefined): string {
  if (address === undefined) return lineNoAddressHint(line)
  const template = labels.value.breakpointLineHint ?? 'Click line {line} to request a breakpoint'
  return `${formatLabel(template, {line})} · ${formatAddressHex(address)}`
}

/**
 * 点击行号 gutter -> emit `line-click`。
 * 若父组件提供了 `lineAddresses`（ELF 行表/符号地址）或当前 PC 行带地址则附带可执行地址，否则只给行号。
 */
function onGutterClick(line: number): void {
  const path = openFilePath.value
  if (!path) return
  const address = addressForLine(line)
  const payload: SourceLineClickPayload = address === undefined ? {path, line} : {path, line, address}
  flashLine.value = line
  window.setTimeout(() => {
    if (flashLine.value === line) flashLine.value = null
  }, 1600)
  emit('line-click', payload)
}

// ---- task-43：当前 PC / 活动源码行高亮与跟随 ------------------------------

/**
 * 应用“文件:行”高亮：路径按 exact -> suffix -> basename 匹配项目文件；
 * 文件不同则打开，同一文件则滚动（同一行重复更新不反复滚动，避免 PC 抖动）。
 * 返回 false 表示找不到文件/项目未加载（task-44 可据此提示）。
 */
async function applyActiveFileLine(
  target: SourceActiveFileLine | null | undefined,
  options: {scroll?: boolean} = {},
): Promise<boolean> {
  const project = currentProject.value
  if (!project) return false
  if (!target) {
    activeFileLineState.value = null
    lastActiveFileLineSignature = ''
    return false
  }
  const resolved = resolveFileLineTarget(target, fileIndex.value ?? project.files)
  if (!resolved) return false
  const signature = `${resolved.filePath}:${resolved.line}`
  const sameTarget = lastActiveFileLineSignature === signature
  lastActiveFileLineSignature = signature
  activeFileLineState.value = {
    filePath: resolved.filePath,
    line: resolved.line,
    ...(resolved.address !== undefined ? {address: resolved.address} : {}),
  }
  expandPathToFile(resolved.filePath)
  if (resolved.filePath !== openFilePath.value) {
    await openFile(resolved.filePath, {line: resolved.line})
    return true
  }
  if (options.scroll !== false && !sameTarget) scrollCodeLine(resolved.line, false)
  return true
}

// ---- 变量 token 点击 / hover / 候选浮层 ----------------------------------

interface HoverState {
  x: number
  y: number
  candidates: VariableCandidate<V>[]
}

interface CandidateMenuState {
  x: number
  y: number
  candidates: VariableCandidate<V>[]
}

const hover = shallowRef<HoverState | null>(null)
const candidateMenu = shallowRef<CandidateMenuState | null>(null)
const hoverPrimary = computed<VariableCandidate<V> | null>(() => hover.value?.candidates[0] ?? null)
/** hover 变量对应的实时值（若有）：tooltip 内展示，便于确认数值。 */
const hoverLiveValue = computed<SourceLiveValue | null>(() => {
  const primary = hoverPrimary.value
  if (!primary) return null
  return props.liveValues?.[primary.ref.key] ?? null
})

function closestVariableToken(target: EventTarget | null): HTMLElement | null {
  if (!(target instanceof Element)) return null
  const element = target.closest('[data-variable]')
  return element instanceof HTMLElement ? element : null
}

function relativePointer(event: MouseEvent): { x: number; y: number } {
  const rect = rootRef.value?.getBoundingClientRect()
  return {x: event.clientX - (rect?.left ?? 0), y: event.clientY - (rect?.top ?? 0)}
}

function candidatesAt(line: number, identifier: string): VariableCandidate<V>[] {
  const result = matchResult.value
  if (!result || !identifier) return []
  return findVariableCandidates(result, {
    filePath: openFilePath.value,
    line,
    identifier,
    includeProject: true,
  })
}

function onCodeMouseOver(event: MouseEvent): void {
  const token = closestVariableToken(event.target)
  if (!token) {
    hideHover()
    return
  }
  const identifier = token.dataset.variable ?? ''
  const line = Number(token.dataset.line ?? '0')
  if (!identifier || !line) {
    hideHover()
    return
  }
  const candidates = candidatesAt(line, identifier)
  if (candidates.length === 0) {
    hideHover()
    return
  }
  const point = relativePointer(event)
  hover.value = {x: point.x + 14, y: point.y + 16, candidates}
}

function hideHover(): void {
  hover.value = null
}

function onCodeClick(event: MouseEvent): void {
  const token = closestVariableToken(event.target)
  if (!token) {
    candidateMenu.value = null
    return
  }
  const identifier = token.dataset.variable ?? ''
  const line = Number(token.dataset.line ?? '0')
  if (!identifier || !line) return
  const candidates = candidatesAt(line, identifier)
  if (candidates.length === 0) return
  if (candidates.length === 1) {
    selectCandidate(candidates[0], candidates, false)
    return
  }
  const point = relativePointer(event)
  hover.value = null
  candidateMenu.value = {
    x: Math.max(8, Math.min(point.x + 10, (rootRef.value?.clientWidth ?? 600) - 340)),
    y: Math.max(8, point.y + 12),
    candidates,
  }
}

function scopeLabel(scope: VariableCandidate<V>['scope']): string {
  switch (scope) {
    case 'line':
      return labels.value.candidateLine
    case 'file':
      return labels.value.candidateFile
    default:
      return labels.value.candidateProject
  }
}

function chooseCandidate(index: number): void {
  const menu = candidateMenu.value
  if (!menu) return
  const candidate = menu.candidates[index]
  if (!candidate) return
  selectCandidate(candidate, menu.candidates, true)
}

/**
 * 选中变量：emit `select-variable`（payload 含点击处全部候选）。
 * `jump=true` 时滚动到变量声明行（从候选列表显式选择时）。
 */
function selectCandidate(candidate: VariableCandidate<V>, candidates: readonly VariableCandidate<V>[], jump: boolean): void {
  candidateMenu.value = null
  hover.value = null
  const ref = candidate.ref
  if (jump && ref.line !== undefined) {
    scrollCodeLine(ref.line)
  } else if (ref.line !== undefined) {
    flashLine.value = ref.line
  }
  if (ref.line !== undefined) {
    const flashTarget = ref.line
    window.setTimeout(() => {
      if (flashLine.value === flashTarget) flashLine.value = null
    }, 1600)
  }
  internalActiveKey.value = ref.key
  emit('select-variable', {
    variable: ref.variable,
    key: ref.key,
    path: ref.path,
    filePath: openFilePath.value,
    line: ref.line,
    candidates: candidates.map((item) => item.ref.entry),
  })
}

// ---- 打开文件跳转 ---------------------------------------------------------

function jumpOptionText(ref: FileVariableRef<V>): string {
  const name = ref.identifier || ref.path
  return ref.line === undefined ? name : `${formatLabel(labels.value.lineLabel, {line: ref.line})} · ${name}`
}

function onJumpChange(event: Event): void {
  const select = event.target as HTMLSelectElement | null
  if (!select) return
  const ref = fileRefs.value.find((candidate) => candidate.key === select.value)
  select.value = ''
  if (ref?.line !== undefined) scrollCodeLine(ref.line)
}

/** 上一个/下一个变量行（以当前视口顶部行号为准，循环）。 */
function jumpBy(direction: number): void {
  const lines = variableLines(fileRefs.value)
  if (lines.length === 0) return
  const currentLine = Math.floor(scrollTop.value / ROW_HEIGHT) + 1
  let target: number
  if (direction > 0) {
    target = lines.find((line) => line > currentLine) ?? lines[0]
  } else {
    const before = lines.filter((line) => line < currentLine)
    target = before.length > 0 ? before[before.length - 1] : lines[lines.length - 1]
  }
  scrollCodeLine(target)
}

/**
 * 按选中 key（`path@address`）滚动到对应变量（task-29，父页面通过 ref 调用）：
 * 变量在其它文件时自动切换文件；找到并滚动返回 true，未匹配返回 false。
 */
async function scrollToVariableByKey(key: string): Promise<boolean> {
  const needle = String(key ?? '').trim()
  if (!needle) return false
  internalActiveKey.value = needle
  const location = findVariableLocationByKey(matchResult.value, needle)
  if (!location) return false
  const filePath = normalizeRelativePath(location.filePath)
  expandPathToFile(filePath)
  if (filePath !== openFilePath.value) {
    await openFile(filePath, {line: location.ref.line})
    return true
  }
  if (location.ref.line !== undefined) scrollCodeLine(location.ref.line)
  scrollTreeToPath(filePath)
  return true
}

// ---- 项目加载 / ELF -------------------------------------------------------

const scanLabel = computed<string>(() => formatLabel(labels.value.scanProgress, {count: scannedEntries.value}))
const detectedElf = computed(() => currentProject.value?.elfFiles[0] ?? null)

function onProgress(count: number): void {
  scannedEntries.value = count
}

function applyLoadedProject(project: SourceProject): void {
  loadedProject.value = project
  projectError.value = ''
  elfError.value = ''
  pickerNotice.value = false
  needsInputFallback.value = false
  fileSearch.value = ''
  expandedDirs.value = new Set()
  showAllUnmatched.value = false
  showAllFiles.value = false
  internalActiveKey.value = null
  treeScrollTop.value = 0
  resetFileState()
  emit('project-loaded', project)
}

function resetFileState(): void {
  openRequestId += 1
  openFilePath.value = ''
  openLines.value = []
  lineStates.value = new Uint8Array(0)
  lineTokenCache.clear()
  openError.value = ''
  openLoading.value = false
  maxLineLength.value = 0
  hover.value = null
  candidateMenu.value = null
  flashLine.value = null
  scrollTop.value = 0
}

function openDirectoryInput(): void {
  needsInputFallback.value = false
  dirInputRef.value?.click()
}

async function onOpenProject(): Promise<void> {
  if (loading.value) return
  projectError.value = ''
  elfError.value = ''
  if (availability.value !== 'available') {
    pickerNotice.value = true
    openDirectoryInput()
    return
  }
  loading.value = true
  scannedEntries.value = 0
  try {
    const handle = await pickProjectDirectory()
    if (!handle) return
    const project = await createProjectFromDirectoryHandle(handle, {onProgress})
    applyLoadedProject(project)
  } catch (error) {
    if (isAbortError(error)) return
    projectError.value = formatLabel(labels.value.projectError, {err: errorMessage(error)})
    needsInputFallback.value = true
  } finally {
    loading.value = false
  }
}

async function onDirectoryInputChange(event: Event): Promise<void> {
  const input = event.target as HTMLInputElement | null
  const files = Array.from(input?.files ?? [])
  if (input) input.value = ''
  if (files.length === 0) return
  loading.value = true
  scannedEntries.value = 0
  projectError.value = ''
  try {
    const project = await createProjectFromFileList(files, {onProgress})
    applyLoadedProject(project)
  } catch (error) {
    projectError.value = formatLabel(labels.value.projectError, {err: errorMessage(error)})
  } finally {
    loading.value = false
  }
}

function onChooseElf(): void {
  elfError.value = ''
  elfInputRef.value?.click()
}

function onElfInputChange(event: Event): void {
  const input = event.target as HTMLInputElement | null
  const file = input?.files?.[0] ?? null
  if (input) input.value = ''
  if (file) emit('elf-file', file)
}

async function onUseDetectedElf(): Promise<void> {
  const project = currentProject.value
  const elf = detectedElf.value
  if (!project || !elf) return
  elfBusyPath.value = elf.path
  elfError.value = ''
  try {
    const file = await project.readElfFile(elf.path)
    if (!file) throw new Error(labels.value.elfMissing)
    emit('elf-file', file)
  } catch (error) {
    elfError.value = formatLabel(labels.value.elfReadFailed, {err: errorMessage(error)})
  } finally {
    elfBusyPath.value = ''
  }
}

function formatTimestamp(value: number): string {
  if (!value) return '-'
  try {
    return new Date(value).toLocaleString()
  } catch {
    return '-'
  }
}

function shortFileName(path: string): string {
  return basename(path) || path
}

function errorMessage(error: unknown): string {
  if (error instanceof Error) return error.message
  return String(error ?? 'unknown error')
}

// ---- 生命周期 / 响应式计算 ------------------------------------------------

watch([currentProject, matchResult, () => props.initialFile], () => {
  const project = currentProject.value
  const result = matchResult.value
  if (!project) {
    resetFileState()
    return
  }
  if (openFilePath.value && !project.files.some((file) => file.path === openFilePath.value)) {
    resetFileState()
  }
  const requested = props.initialFile ? normalizeRelativePath(props.initialFile) : ''
  if (requested && requested !== lastAutoOpenedInitial && project.files.some((file) => file.path === requested)) {
    lastAutoOpenedInitial = requested
    expandPathToFile(requested)
    void openFile(requested)
    return
  }
  if (!openFilePath.value && !openLoading.value && project.sourceCount > 0) {
    const first = pickInitialSourceFile(result) ?? project.files[0]?.path
    if (first) {
      expandPathToFile(first)
      void openFile(first)
    }
  }
}, {immediate: true})

watch(viewportRef, () => {
  void nextTick(observeViewport)
})

// task-43：prop activeFileLine（声明式 PC 行跟随）。项目/行变化时解析项目文件并打开/滚动/高亮。
watch(() => props.activeFileLine, (target) => {
  void applyActiveFileLine(target, {scroll: true})
}, {immediate: true})

// 先设置 PC 行、后加载项目/ELF 时补一次（applyActiveFileLine 依赖 currentProject/fileIndex）。
watch([currentProject, () => props.activeFileLine], () => {
  if (props.activeFileLine) void applyActiveFileLine(props.activeFileLine, {scroll: true})
})

watch(treeViewportRef, () => {
  void nextTick(observeTreeViewport)
})

// 过滤/搜索变化时虚拟树回到顶部，避免沿用旧 scrollTop 造成“空白列表”。
watch([showAllFiles, filesWithVariablesMode, fileSearch], () => {
  treeScrollTop.value = 0
  if (treeViewportRef.value) treeViewportRef.value.scrollTop = 0
})

function observeViewport(): void {
  resizeObserver?.disconnect()
  resizeObserver = null
  const element = viewportRef.value
  if (!element) return
  viewportHeight.value = element.clientHeight || DEFAULT_VIEWPORT_HEIGHT
  if (typeof ResizeObserver === 'undefined') return
  resizeObserver = new ResizeObserver(() => {
    viewportHeight.value = element.clientHeight || DEFAULT_VIEWPORT_HEIGHT
  })
  resizeObserver.observe(element)
}

function observeTreeViewport(): void {
  treeResizeObserver?.disconnect()
  treeResizeObserver = null
  const element = treeViewportRef.value
  if (!element) return
  treeViewportHeight.value = element.clientHeight || DEFAULT_TREE_HEIGHT
  if (typeof ResizeObserver === 'undefined') return
  treeResizeObserver = new ResizeObserver(() => {
    treeViewportHeight.value = element.clientHeight || DEFAULT_TREE_HEIGHT
  })
  treeResizeObserver.observe(element)
}

function onDocumentPointerDown(event: PointerEvent): void {
  if (!candidateMenu.value) return
  const target = event.target
  if (target instanceof Node && candidateMenuRef.value?.contains(target)) return
  if (target instanceof Element && target.closest('[data-variable]')) return
  candidateMenu.value = null
}

onMounted(() => {
  document.addEventListener('pointerdown', onDocumentPointerDown, true)
  observeViewport()
  observeTreeViewport()
})

onBeforeUnmount(() => {
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
  resizeObserver?.disconnect()
  resizeObserver = null
  treeResizeObserver?.disconnect()
  treeResizeObserver = null
})

function formatLabel(template: string, values: Record<string, string | number> = {}): string {
  return formatSourceBrowserLabel(template, values)
}

defineExpose({
  /** 打开指定项目相对路径的文件（父组件联动用） */
  openFile,
  /**
   * task-43：打开/滚动到指定源码行并高亮（PC 行样式）。
   * - `scrollToLine(path, line)`：path 支持 full path/relativePath/suffix/basename，找不到返回 false；
   * - `scrollToLine(line)`：兼容旧签名，仅滚动当前文件。
   */
  scrollToLine: async (pathOrLine: string | number, line?: number): Promise<boolean> => {
    if (typeof pathOrLine === 'number') {
      scrollCodeLine(pathOrLine)
      return true
    }
    if (!line) return false
    return applyActiveFileLine({path: pathOrLine, line}, {scroll: true})
  },
  /**
   * task-43：高亮指定源码行（不清除 selectedKeys/liveValues）。
   * `options.scroll=false` 时只高亮不滚动；找不到文件返回 false。
   */
  highlightLine: async (path: string, line: number, options: {scroll?: boolean} = {}): Promise<boolean> => {
    return applyActiveFileLine({path, line}, {scroll: options.scroll !== false})
  },
  /** task-43：当前高亮文件行（调试/task-44 用）。 */
  getActiveFileLine: (): {filePath: string; line: number; address?: number} | null => activeFileLineState.value,
  /**
   * task-29：按选中 key（`path@address`）滚动到变量；必要时自动切文件。
   * 父页面点击曲线/图例时调用；找不到返回 false。
   */
  scrollToVariableByKey,
  /** 当前活动变量 key（props.activeKey 或内部最近一次点击） */
  getActiveKey: (): string | null => activeKeyValue.value,
  /** 当前映射结果（调试/父组件统计用） */
  getMatchResult: (): VariableFileMatchResult<V> | null => matchResult.value,
  /** 关闭候选浮层/hover */
  closeOverlays: (): void => {
    candidateMenu.value = null
    hover.value = null
  },
})
</script>

<style scoped>
.source-browser {
  /* 颜色全部来自 design-tokens.css（深色优先，浅色自动切换）。
     var() fallback 为旧的中性色，组件脱离全局 tokens（单测/独立嵌入）时仍可读。 */
  --sb-bg: var(--bb-surface-inset, rgba(127, 127, 127, 0.06));
  --sb-panel: var(--bb-bg-elevated, rgba(127, 127, 127, 0.08));
  --sb-border: var(--bb-border, rgba(127, 127, 127, 0.3));
  --sb-muted: var(--bb-text-muted, rgba(127, 127, 127, 0.9));
  --sb-text: var(--bb-text, #151a21);
  --sb-accent: var(--bb-accent, #38bdf8);
  --sb-accent-soft: var(--bb-accent-soft, rgba(56, 189, 248, 0.12));
  --sb-accent-soft-strong: var(--bb-accent-soft-strong, rgba(56, 189, 248, 0.24));
  --sb-accent-strong: var(--bb-accent-strong, #7cbcff);
  --sb-danger: var(--bb-danger, #f87171);
  --sb-danger-soft: var(--bb-danger-soft, rgba(248, 113, 113, 0.12));
  --sb-warn: var(--bb-warning, #fbbf24);
  --sb-success: var(--bb-success, #34d399);
  --sb-success-soft: var(--bb-success-soft, rgba(52, 211, 153, 0.12));
  --sb-raised: var(--bb-surface-raised, rgba(20, 20, 24, 0.95));
  --sb-shadow: var(--bb-shadow-md, 0 10px 28px rgba(0, 0, 0, 0.4));
  --sb-keyword: var(--bb-syntax-keyword, #a78bfa);
  --sb-string: var(--bb-syntax-string, #34d399);
  --sb-number: var(--bb-syntax-number, #fbbf24);
  --sb-comment: var(--bb-syntax-comment, #94a3b8);
  --sb-preprocessor: var(--bb-syntax-preprocessor, #f472b6);
  position: relative;
  display: flex;
  flex-direction: column;
  height: 100%;
  min-height: 360px;
  color: var(--sb-text);
  font-size: 13px;
  border: 1px solid var(--sb-border);
  border-radius: var(--bb-radius-sm, 8px);
  overflow: hidden;
  background: var(--bb-panel-bg, rgba(127, 127, 127, 0.04));
}

.sb-toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  padding: 8px 10px;
  border-bottom: 1px solid var(--sb-border);
  background: var(--sb-panel);
}

.sb-btn {
  padding: 4px 10px;
  font-size: 12px;
  color: inherit;
  background: transparent;
  border: 1px solid var(--sb-border);
  border-radius: 6px;
  cursor: pointer;
}

.sb-btn:hover:not(:disabled) {
  border-color: var(--sb-accent);
  color: var(--sb-accent);
}

.sb-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.sb-btn-primary {
  border-color: var(--sb-accent);
  color: var(--sb-accent);
}

.sb-btn-small {
  padding: 1px 7px;
  font-size: 11px;
  line-height: 18px;
}

.sb-chip {
  padding: 1px 8px;
  font-size: 11px;
  border: 1px solid var(--sb-border);
  border-radius: 999px;
  white-space: nowrap;
  max-width: 320px;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sb-chip-warn {
  border-color: var(--sb-warn);
  color: var(--sb-warn);
}

.sb-status {
  font-size: 12px;
  color: var(--sb-muted);
}

.sb-privacy {
  margin-left: auto;
  font-size: 11px;
  color: var(--sb-muted);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sb-spacer {
  flex: 1 1 auto;
}

.sb-elfbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 10px;
  font-size: 12px;
  border-bottom: 1px solid var(--sb-border);
}

.sb-elfbar-muted {
  color: var(--sb-muted);
}

.sb-elf-icon {
  padding: 0 5px;
  font-size: 10px;
  font-weight: 700;
  border: 1px solid var(--sb-accent);
  border-radius: 4px;
  color: var(--sb-accent);
}

.sb-elf-name {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  max-width: 55%;
}

.sb-elf-time {
  color: var(--sb-muted);
}

.sb-error-inline {
  color: var(--sb-danger);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sb-banner {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px;
  font-size: 12px;
  border-bottom: 1px solid var(--sb-border);
}

.sb-banner-error {
  color: var(--sb-danger);
}

.sb-banner-info {
  color: var(--sb-muted);
}

.sb-empty {
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 32px;
  text-align: center;
}

.sb-empty-main {
  padding: 24px;
}

.sb-empty-title {
  margin: 0;
  font-size: 16px;
  font-weight: 600;
}

.sb-empty-body {
  margin: 0;
  max-width: 560px;
  line-height: 1.6;
  color: var(--sb-muted);
}

.sb-empty-hint {
  margin: 0;
  max-width: 620px;
  font-size: 12px;
  line-height: 1.5;
  color: var(--sb-muted);
  opacity: 0.85;
}

.sb-body {
  flex: 1 1 auto;
  display: flex;
  min-height: 0;
}

.sb-sidebar {
  display: flex;
  flex-direction: column;
  width: 280px;
  min-width: 200px;
  max-width: 40%;
  border-right: 1px solid var(--sb-border);
  min-height: 0;
}

.sb-sidebar-head {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 6px 8px;
  border-bottom: 1px solid var(--sb-border);
}

.sb-filter-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 6px;
  min-height: 18px;
}

/* “Files with variables / Show all project files” 切换 */
.sb-toggle {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: var(--sb-muted);
  cursor: pointer;
  user-select: none;
  min-width: 0;
}

.sb-toggle input {
  width: 12px;
  height: 12px;
  margin: 0;
  accent-color: var(--sb-accent-strong);
  cursor: pointer;
}

.sb-toggle span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sb-search {
  width: 100%;
  box-sizing: border-box;
  padding: 4px 8px;
  font-size: 12px;
  color: inherit;
  background: transparent;
  border: 1px solid var(--sb-border);
  border-radius: 6px;
  outline: none;
}

.sb-search {
  background: var(--sb-bg);
}

.sb-search:focus {
  border-color: var(--sb-accent);
}

.sb-search-count {
  font-size: 11px;
  color: var(--sb-muted);
}

.sb-tree {
  position: relative;
  flex: 1 1 auto;
  overflow: auto;
  min-height: 0;
}

/* 虚拟树：canvas 撑出总高度，只渲染可见行（行高 = TREE_ROW_HEIGHT） */
.sb-tree-canvas {
  position: relative;
  width: 100%;
}

.sb-tree-row {
  position: absolute;
  left: 0;
  right: 0;
  display: flex;
  align-items: center;
  gap: 4px;
  box-sizing: border-box;
  height: 24px;
  padding: 0 8px;
  font-size: 12px;
  color: inherit;
  background: transparent;
  border: 0;
  text-align: left;
  cursor: pointer;
  white-space: nowrap;
}

.sb-tree-row:hover {
  background: var(--sb-panel);
}

.sb-tree-file.active {
  color: var(--sb-accent);
  background: var(--sb-accent-soft);
}

.sb-tree-dir {
  font-weight: 600;
  color: var(--sb-muted);
}

.sb-caret {
  width: 12px;
  flex: 0 0 12px;
  font-size: 10px;
}

.sb-file-dot {
  width: 12px;
  flex: 0 0 12px;
  font-size: 9px;
  color: var(--sb-muted);
}

.sb-tree-name {
  flex: 1 1 auto;
  overflow: hidden;
  text-overflow: ellipsis;
}

.sb-tree-badge {
  flex: 0 0 auto;
  padding: 0 5px;
  font-size: 10px;
  color: var(--sb-muted);
  border: 1px solid var(--sb-border);
  border-radius: 999px;
}

.sb-tree-badge-var {
  color: var(--sb-accent);
  border-color: var(--sb-accent);
}

.sb-sidebar-empty {
  padding: 12px;
  font-size: 12px;
  color: var(--sb-muted);
}

.sb-unmatched {
  flex: 0 0 auto;
  max-height: 180px;
  overflow: auto;
  padding: 6px 8px;
  font-size: 11px;
  border-top: 1px solid var(--sb-border);
}

.sb-unmatched summary {
  cursor: pointer;
  color: var(--sb-warn);
}

.sb-unmatched-hint {
  margin: 4px 0;
  color: var(--sb-muted);
  line-height: 1.4;
}

.sb-unmatched-list {
  margin: 0;
  padding-left: 16px;
}

.sb-unmatched-list li {
  margin: 1px 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sb-unmatched-file {
  color: var(--sb-muted);
}

.sb-link {
  padding: 0;
  font-size: 11px;
  color: var(--sb-accent);
  background: transparent;
  border: 0;
  cursor: pointer;
}

.sb-main {
  flex: 1 1 auto;
  display: flex;
  flex-direction: column;
  min-width: 0;
  min-height: 0;
}

.sb-code-head {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 8px;
  border-bottom: 1px solid var(--sb-border);
}

.sb-code-path {
  flex: 1 1 auto;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
}

.sb-code-actions {
  display: flex;
  align-items: center;
  gap: 6px;
}

.sb-jump {
  max-width: 260px;
  padding: 1px 4px;
  font-size: 11px;
  color: var(--sb-text);
  background: var(--sb-bg);
  border: 1px solid var(--sb-border);
  border-radius: var(--bb-radius-xs, 4px);
}

.sb-msg {
  padding: 12px;
  font-size: 12px;
  color: var(--sb-muted);
}

.sb-viewport {
  position: relative;
  flex: 1 1 auto;
  overflow: auto;
  min-height: 0;
  background: var(--sb-bg);
}

.sb-canvas {
  position: relative;
  min-height: 100%;
}

.sb-line {
  position: absolute;
  left: 0;
  right: 0;
  display: flex;
  align-items: center;
  white-space: pre;
  font-family: var(--bb-font-mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-size: 12px;
  line-height: 20px;
  tab-size: 4;
}

.sb-line-vars {
  background: var(--sb-accent-soft);
}

.sb-line-selected {
  background: var(--sb-accent-soft-strong);
}

/* task-29：activeKey 对应的源码行（当前曲线选中） */
.sb-line-active {
  background: var(--sb-accent-soft-strong);
  box-shadow: inset 3px 0 0 var(--sb-accent-strong);
}

/* task-43：当前 PC/活动源码行（与变量 active 行独立，使用 warning 色强调） */
.sb-line-pc {
  background: rgba(251, 191, 36, 0.14);
  box-shadow: inset 3px 0 0 var(--sb-warn);
}

.sb-line-pc .sb-gutter {
  color: var(--sb-warn);
  font-weight: 700;
}

.sb-pc-badge {
  flex: 0 0 auto;
  margin-left: 8px;
  padding: 0 5px;
  font-size: 10px;
  line-height: 15px;
  color: var(--sb-warn);
  background: var(--sb-panel);
  border: 1px solid var(--sb-warn);
  border-radius: 999px;
  white-space: nowrap;
}

.sb-line-flash {
  animation: sb-flash 1.4s ease-out;
}

@keyframes sb-flash {
  from {
    background: var(--sb-accent-soft-strong);
  }
  to {
    background: transparent;
  }
}

.sb-gutter {
  position: sticky;
  left: 0;
  flex: 0 0 52px;
  width: 52px;
  margin: 0;
  padding: 0 10px 0 0;
  border: 0;
  border-radius: 0;
  font: inherit;
  line-height: inherit;
  text-align: right;
  color: var(--sb-muted);
  background: var(--sb-bg);
  user-select: none;
  cursor: pointer;
  z-index: 1;
}

/* 行号 gutter = 断点槽：hover 高亮，点击 emit line-click */
.sb-gutter:hover,
.sb-gutter:focus-visible {
  color: var(--sb-accent-strong);
  background: var(--sb-panel);
  outline: none;
}

.sb-gutter:hover::after {
  content: '●';
  margin-left: 4px;
  font-size: 8px;
  color: var(--sb-danger);
}

/* task-43：该行在行表中没有可执行地址（hover title 会说明；点击仍会 emit line-click，由父页面提示） */
.sb-gutter.no-address {
  opacity: 0.72;
  cursor: help;
}

.sb-gutter.no-address:hover::after {
  content: '?';
  color: var(--sb-warn);
}

.sb-line-text {
  flex: 0 0 auto;
}

/* 行尾实时值（task-29）：sticky 在可视区右侧，长行横向滚动时仍可见 */
.sb-live-values {
  position: sticky;
  right: 0;
  z-index: 2;
  display: inline-flex;
  align-items: center;
  gap: 6px;
  margin-left: auto;
  padding: 0 8px 0 14px;
  background: linear-gradient(90deg, transparent, var(--sb-bg) 14px);
  white-space: nowrap;
}

.sb-live-value {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 0 6px;
  font-size: 11px;
  line-height: 16px;
  color: var(--sb-text);
  background: var(--sb-panel);
  border: 1px solid var(--sb-border);
  border-radius: 999px;
}

.sb-live-value.stale {
  opacity: 0.55;
}

.sb-live-dot {
  flex: 0 0 auto;
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--sb-accent);
}

.sb-live-name {
  color: var(--sb-muted);
  font-family: var(--bb-font-mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
}

.sb-live-text {
  font-family: var(--bb-font-mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-weight: 700;
}

.sb-live-value.stale .sb-live-text {
  text-decoration: line-through;
}

.sb-live-stale {
  font-size: 9px;
  text-transform: uppercase;
  color: var(--sb-warn);
}

.sb-tooltip-live {
  margin-top: 2px;
  align-items: center;
}

.sb-tooltip-live-text {
  font-family: var(--bb-font-mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  font-weight: 700;
}

.sb-token-variable {
  color: inherit;
  font-weight: 700;
  text-decoration: underline dotted var(--sb-accent);
  text-underline-offset: 2px;
  cursor: pointer;
  background: var(--sb-accent-soft);
  border-radius: 2px;
}

.sb-token-variable:hover {
  background: var(--sb-accent-soft-strong);
}

/* task-29：有实时值的变量（实线强调） & 当前 active 变量（强高亮） */
.sb-token-live {
  text-decoration: underline solid var(--sb-accent-strong);
  text-underline-offset: 2px;
}

.sb-token-active {
  background: var(--sb-accent-soft-strong);
  color: var(--sb-accent-strong);
  outline: 1px solid var(--sb-accent-strong);
  outline-offset: -1px;
  border-radius: 2px;
}

.sb-token-keyword {
  color: var(--sb-keyword);
}

.sb-token-string {
  color: var(--sb-string);
}

.sb-token-number {
  color: var(--sb-number);
}

.sb-token-comment {
  color: var(--sb-comment);
  font-style: italic;
}

.sb-token-preprocessor {
  color: var(--sb-preprocessor);
}

.sb-token-identifier,
.sb-token-punctuation,
.sb-token-plain {
  color: inherit;
}

.sb-tooltip {
  position: absolute;
  z-index: 20;
  max-width: 420px;
  padding: 6px 8px;
  font-size: 11px;
  line-height: 1.5;
  text-align: left;
  background: var(--sb-raised);
  color: var(--sb-text);
  border: 1px solid var(--sb-border);
  border-radius: var(--bb-radius-sm, 6px);
  box-shadow: var(--sb-shadow);
  pointer-events: none;
}

.sb-tooltip-head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 2px;
}

.sb-tooltip-name {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-weight: 700;
}

.sb-tooltip-sample {
  padding: 0 5px;
  border: 1px solid var(--sb-danger);
  border-radius: 999px;
  color: var(--sb-danger);
  background: var(--sb-danger-soft);
}

.sb-tooltip-sample.ok {
  border-color: var(--sb-success);
  color: var(--sb-success);
  background: var(--sb-success-soft);
}

.sb-tooltip-row {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.sb-tooltip-path {
  font-family: var(--bb-font-mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace);
  color: var(--sb-accent-strong);
}

.sb-tooltip-type,
.sb-tooltip-src,
.sb-tooltip-more {
  color: var(--sb-text-secondary, #3d4552);
}

.sb-popover {
  position: absolute;
  z-index: 30;
  width: 340px;
  max-height: 320px;
  overflow: auto;
  padding: 6px;
  background: var(--sb-raised);
  color: var(--sb-text);
  border: 1px solid var(--sb-border);
  border-radius: var(--bb-radius-md, 8px);
  box-shadow: var(--sb-shadow);
}

.sb-popover-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 2px 4px 6px;
  font-size: 11px;
  color: var(--sb-text-secondary, #3d4552);
}

.sb-candidate {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
  width: 100%;
  padding: 5px 6px;
  font-size: 11px;
  color: inherit;
  text-align: left;
  background: transparent;
  border: 0;
  border-radius: 4px;
  cursor: pointer;
}

.sb-candidate:hover {
  background: var(--sb-accent-soft-strong);
}

.sb-candidate-path {
  flex: 1 1 100%;
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sb-candidate-meta,
.sb-candidate-scope {
  color: var(--sb-muted);
}

.sb-candidate-sample {
  margin-left: auto;
  color: var(--sb-danger);
}

.sb-candidate-sample.ok {
  color: var(--sb-success);
}

.sb-hidden {
  display: none;
}
</style>
