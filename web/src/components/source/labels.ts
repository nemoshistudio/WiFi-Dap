/**
 * SourceBrowser.vue 的文案（labels）。
 *
 * 为避免与 Web UI 的 i18n 契约（docs/i18n-keys.md，禁止页面硬编码中文）冲突，
 * 组件不自带 `translate()`：默认英文，父组件可通过 `labels` prop 逐项覆盖为
 * 中文/其它语言（后续接入 DebugCurve 时由 Lead 在 locales 增加 key 后传入）。
 *
 * `{name}` 形式的占位符由 `formatSourceBrowserLabel()` 替换。
 */

export interface SourceBrowserLabels {
    /** 打开项目根目录按钮 */
    openProject: string
    /** 重新选择目录按钮 */
    changeFolder: string
    /** 直接选 ELF 文件按钮 */
    chooseElf: string
    /** HTTP LAN 下 File System Access API 不可用的说明 */
    secureContextHint: string
    /** 只读/不上传隐私说明 */
    privacyNote: string
    /** 项目根名，`{name}` */
    projectRoot: string
    /** 源码文件数，`{count}` */
    filesCount: string
    /** 变量映射统计，`{matched}/{total}` */
    matchedSummary: string
    /** 未匹配变量，`{count}` */
    unmatchedSummary: string
    /** 有多个同分候选文件，`{count}` */
    ambiguousSummary: string
    /** 扫描进度，`{count}` */
    scanProgress: string
    /** 扫描上限说明，`{maxFiles}` `{maxBytes}` */
    limitsHint: string
    /** 未加载项目时的扫描上限简述（已含默认数字） */
    limitsHintDefault: string
    /** 自动发现 ELF，`{name}` */
    elfDetected: string
    /** ELF 修改时间，`{time}` */
    elfTime: string
    /** 使用自动发现的 ELF 按钮 */
    useElf: string
    /** 未发现 ELF 的提示 */
    elfNone: string
    /** 读取 ELF 失败，`{err}` */
    elfReadFailed: string
    /** 项目里没有对应 ELF 文件 */
    elfMissing: string
    /** 打开项目失败，`{err}` */
    projectError: string
    /** 未选择项目时的标题 */
    noProjectTitle: string
    /** 未选择项目时的说明 */
    noProjectBody: string
    /** 没有源码文件 */
    noSourceFiles: string
    /** 没有源码文件的说明 */
    noSourceFilesHint: string
    /** 文件搜索框占位符 */
    searchPlaceholder: string
    /** 搜索无结果 */
    noSearchResults: string
    /** 搜索结果计数，`{shown}`/`{total}` */
    searchResultsCount: string
    /** 未匹配变量列表标题 */
    unmatchedTitle: string
    /** 未匹配变量列表说明 */
    unmatchedHint: string
    /** 展开更多 */
    showMore: string
    /** 收起 */
    showLess: string
    /** 文件旁变量数，`{count}` */
    fileVariables: string
    /** 未打开文件时的标题 */
    selectFileTitle: string
    /** 未打开文件时的说明 */
    selectFileHint: string
    /** 正在读取文件 */
    readingFile: string
    /** 该文件无变量匹配 */
    noMatchInFile: string
    /** 读取源码失败，`{err}` */
    readFailed: string
    /** 重试读取 */
    retryRead: string
    /** 跳转到变量下拉框占位 */
    jumpToVariable: string
    /** 上一个变量 */
    prevVariable: string
    /** 下一个变量 */
    nextVariable: string
    /** 候选列表标题，`{count}` */
    candidateTitle: string
    /** 候选来源：同行 */
    candidateLine: string
    /** 候选来源：同文件 */
    candidateFile: string
    /** 候选来源：项目其它文件 */
    candidateProject: string
    /** 可采样 */
    sampleable: string
    /** 不可采样 */
    unsampleable: string
    /** 类型显示，`{type}` */
    typeLabel: string
    /** 变量位置，`{file}` `{line}` */
    fileLine: string
    /** hover 提示中还有 N 个候选，`{count}` */
    hoverMore: string
    /** 没有变量（还没解析 ELF） */
    noVariables: string
    /** 没有变量的说明 */
    noVariablesHint: string
    /** 加载中 */
    loading: string
    /** 文件选择回退按钮（浏览器不支持目录选择器时） */
    openProjectFallback: string
    /** 文件行号，`{line}` */
    lineLabel: string
    /** 关闭候选浮层 */
    close: string

    // ---- task-29 新增（可选：旧父组件未传时使用下方英文默认，不破坏既有集成） ----
    /** 默认视图开关：“只显示有变量的文件”（Files with variables） */
    onlyFilesWithVariables?: string
    /** “显示全部项目文件”开关 */
    showAllProjectFiles?: string
    /** 默认视图下没有任何文件匹配到变量时的提示 */
    noFilesWithVariables?: string
    /** 实时值已过期（stale）徽标 */
    liveValueStale?: string
    /** 行号 gutter hover 提示（断点），`{line}` */
    breakpointLineHint?: string
    /** 源码行尾实时值区域标题 */
    liveValues?: string
}

export const DEFAULT_SOURCE_BROWSER_LABELS: SourceBrowserLabels = {
    openProject: 'Open project folder…',
    changeFolder: 'Change folder',
    chooseElf: 'Choose ELF file…',
    secureContextHint:
        'File System Access API is only available in secure contexts (HTTPS/localhost). This page is served over HTTP, so the folder file-picker fallback is used; re-select the folder each time.',
    privacyNote: 'Read-only and local: file contents are never uploaded.',
    projectRoot: 'Project: {name}',
    filesCount: '{count} source files',
    matchedSummary: 'Variables mapped: {matched}/{total}',
    unmatchedSummary: '{count} unmatched',
    ambiguousSummary: '{count} ambiguous',
    scanProgress: 'Scanning… {count} files',
    limitsHint: 'Limits: {maxFiles} files, {maxBytes} per file.',
    limitsHintDefault:
        'Scans up to 2000 C/C++ files (2 MB each); build/out/node_modules/hidden folders are skipped and contents are read on demand.',
    elfDetected: 'ELF found: {name}',
    elfTime: 'modified {time}',
    useElf: 'Use this ELF',
    elfNone: 'No .elf file found in the project. Choose one manually to map DWARF variables.',
    elfReadFailed: 'Could not read the ELF file: {err}',
    elfMissing: 'The selected ELF file is no longer available; re-open the project folder.',
    projectError: 'Could not open the project folder: {err}',
    noProjectTitle: 'No project folder loaded',
    noProjectBody:
        'Open a local C/C++ project folder to browse its sources and pick variables directly in the code. Files are read on demand and stay in your browser.',
    noSourceFiles: 'No C/C++ source files found',
    noSourceFilesHint:
        'Check the selected root, or the ignore/size limits below. Build/out/node_modules and hidden folders are skipped.',
    searchPlaceholder: 'Filter source files…',
    noSearchResults: 'No files match the filter',
    searchResultsCount: '{shown} / {total} files',
    unmatchedTitle: 'Unmatched variables',
    unmatchedHint: 'These DWARF variables did not map to any project source file (path/build mismatch).',
    showMore: 'Show more',
    showLess: 'Show less',
    fileVariables: '{count} vars',
    selectFileTitle: 'Select a source file',
    selectFileHint: 'Pick a file on the left. Matched DWARF variables are underlined in the code and can be clicked.',
    readingFile: 'Reading file…',
    noMatchInFile: 'No DWARF variables mapped to this file. You can still browse the source.',
    readFailed: 'Could not read the source file: {err}',
    retryRead: 'Retry',
    jumpToVariable: 'Jump to variable…',
    prevVariable: 'Previous variable',
    nextVariable: 'Next variable',
    candidateTitle: '{count} candidates',
    candidateLine: 'Same line',
    candidateFile: 'Same file',
    candidateProject: 'Other file',
    sampleable: 'Sampleable',
    unsampleable: 'Not sampleable',
    typeLabel: 'Type: {type}',
    fileLine: '{file}:{line}',
    hoverMore: '+{count} more candidates',
    noVariables: 'No variables loaded',
    noVariablesHint: 'Load/parse an ELF on the curve page first; variables with DWARF file/line are mapped here.',
    loading: 'Loading…',
    openProjectFallback: 'Choose project folder…',
    lineLabel: 'Line {line}',
    close: 'Close',
    onlyFilesWithVariables: 'Files with variables',
    showAllProjectFiles: 'Show all project files',
    noFilesWithVariables:
        'No source file matches the loaded variables. Turn on "Show all project files" to browse the whole project.',
    liveValueStale: 'stale',
    breakpointLineHint: 'Click line {line} to request a breakpoint',
    liveValues: 'Live values',
}

/** 用 `{name}` 占位符格式化 label（未知占位符原样保留）。 */
export function formatSourceBrowserLabel(template: string, values: Record<string, string | number> = {}): string {
    return String(template ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (match, key: string) => {
        const value = values[key]
        return value === undefined || value === null ? match : String(value)
    })
}

/** 合并父组件传入的覆盖项与默认英文文案（父组件只传需要的 key）。 */
export function resolveSourceBrowserLabels(overrides?: Partial<SourceBrowserLabels> | null): SourceBrowserLabels {
    if (!overrides) return DEFAULT_SOURCE_BROWSER_LABELS
    return {...DEFAULT_SOURCE_BROWSER_LABELS, ...overrides}
}
