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
          <span class="sb-search-count">{{ searchCountText }}</span>
        </div>
        <div class="sb-tree">
          <template v-for="row in treeRows" :key="`${row.kind}:${row.path}`">
            <button
                v-if="row.kind === 'directory'"
                type="button"
                class="sb-tree-row sb-tree-dir"
                :style="{paddingLeft: `${8 + row.depth * 12}px`}"
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
                :style="{paddingLeft: `${8 + row.depth * 12}px`}"
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
          <div v-if="treeRows.length === 0" class="sb-sidebar-empty">{{ labels.noSearchResults }}</div>
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
                    'sb-line-flash': row.index + 1 === flashLine,
                  }"
                  :style="{top: `${row.top}px`, height: `${ROW_HEIGHT}px`}"
              >
                <span class="sb-gutter">{{ row.index + 1 }}</span>
                <span class="sb-line-text"><span
                    v-for="(token, tokenIndex) in row.tokens"
                    :key="tokenIndex"
                    :class="tokenClass(token)"
                    :data-variable="token.kind === 'variable' ? token.variable : null"
                    :data-line="row.index + 1"
                >{{ token.text }}</span></span>
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
  collectIdentifiers,
  computeBlockCommentStates,
  computeVirtualWindow,
  createSourceFileIndex,
  findVariableCandidates,
  flattenSourceTree,
  formatFileSize,
  groupReferencesByLine,
  matchVariablesToFiles,
  normalizeRelativePath,
  pickInitialSourceFile,
  refsForFile,
  searchSourceFiles,
  tokenizeCodeLine,
  variableLines,
  walkSourceVariables,
  type CodeToken,
  type FileVariableRef,
  type SourceTreeRow,
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
}>()

const emit = defineEmits<{
  (event: 'select-variable', payload: SourceVariableSelectionPayload<V>): void
  (event: 'project-loaded', project: SourceProject): void
  (event: 'elf-file', file: File): void
  (event: 'file-open', path: string): void
  (event: 'error', message: string): void
}>()

const ROW_HEIGHT = 20
const OVERSCAN = 6
const DEFAULT_VIEWPORT_HEIGHT = 480

const labels = computed<SourceBrowserLabels>(() => resolveSourceBrowserLabels(props.labels))
const directoryPickerAvailable = computed<boolean>(() => supportsDirectoryPicker())
const availability = computed(() => getDirectoryPickerAvailability())

const rootRef = ref<HTMLElement | null>(null)
const dirInputRef = ref<HTMLInputElement | null>(null)
const elfInputRef = ref<HTMLInputElement | null>(null)
const viewportRef = ref<HTMLDivElement | null>(null)
const candidateMenuRef = ref<HTMLDivElement | null>(null)

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
const unmatchedPreview = computed<SourceVariableEntry<V>[]>(() => {
  const unmatched = matchResult.value?.unmatched ?? []
  return unmatched.slice(0, showAllUnmatched.value ? 500 : 20)
})
const showAllUnmatched = ref(false)

function fileVariableCount(path: string): number {
  return variableCounts.value.get(normalizeRelativePath(path)) ?? 0
}

// ---- 目录树 / 搜索 --------------------------------------------------------

const fileSearch = ref('')
const expandedDirs = ref<ReadonlySet<string>>(new Set())

const treeRows = computed<SourceTreeRow[]>(() => {
  const project = currentProject.value
  if (!project) return []
  const query = fileSearch.value.trim()
  if (query) {
    return searchSourceFiles(project.files, query, 300).map((file) => ({
      kind: 'file' as const,
      path: file.path,
      name: file.path,
      depth: 0,
      file,
    }))
  }
  return flattenSourceTree(project.tree, expandedDirs.value)
})

const searchCountText = computed<string>(() => {
  const project = currentProject.value
  if (!project) return ''
  const query = fileSearch.value.trim()
  if (query) {
    return formatLabel(labels.value.searchResultsCount, {shown: treeRows.value.length, total: project.sourceCount})
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
  tokens: CodeToken[]
}

const visibleRows = computed<RenderRow[]>(() => {
  const window = virtualWindow.value
  const rows: RenderRow[] = []
  const grouped = lineRefs.value
  const selected = selectedLineNumbers.value
  const names = identifierSet.value
  for (let index = window.start; index < window.end; index++) {
    rows.push({
      index,
      top: index * ROW_HEIGHT,
      hasVariables: grouped.has(index + 1),
      selected: selected.has(index + 1),
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
    const target = options.line ?? variableLines(fileRefs.value)[0]
    if (target) scrollToLine(target)
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
function scrollToLine(line: number, highlight = true): void {
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
    scrollToLine(ref.line)
  } else if (ref.line !== undefined) {
    flashLine.value = ref.line
  }
  if (ref.line !== undefined) {
    const flashTarget = ref.line
    window.setTimeout(() => {
      if (flashLine.value === flashTarget) flashLine.value = null
    }, 1600)
  }
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
  if (ref?.line !== undefined) scrollToLine(ref.line)
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
  scrollToLine(target)
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
})

onBeforeUnmount(() => {
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
  resizeObserver?.disconnect()
  resizeObserver = null
})

function formatLabel(template: string, values: Record<string, string | number> = {}): string {
  return formatSourceBrowserLabel(template, values)
}

defineExpose({
  /** 打开指定项目相对路径的文件（父组件联动用） */
  openFile,
  /** 滚动到指定行并高亮 */
  scrollToLine,
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
  flex: 1 1 auto;
  overflow: auto;
  min-height: 0;
  padding: 4px 0;
}

.sb-tree-row {
  display: flex;
  align-items: center;
  gap: 4px;
  width: 100%;
  padding: 2px 8px;
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
  padding-right: 10px;
  text-align: right;
  color: var(--sb-muted);
  background: var(--sb-bg);
  user-select: none;
  z-index: 1;
}

.sb-line-text {
  flex: 0 0 auto;
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
