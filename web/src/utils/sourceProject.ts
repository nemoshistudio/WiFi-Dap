/**
 * 本地源码项目（File System Access / `<input webkitdirectory>`）与 DWARF 变量映射的纯函数模块。
 *
 * 目标：在浏览器里只读打开用户选择的项目根目录，扫描 C/C++ 源码文件，把
 * DWARF 变量（VariableNode：relativeFile/file/compDir/line/name/...）映射到
 * 项目文件 / 行；页面源码视图据此高亮“可点击的变量 token”，点击后返回
 * 同名/同行候选，交给曲线页选变量（Ozone 式操作）。
 *
 * 本模块**不访问 DOM**（不做 showDirectoryPicker、不构造 File/Blob），因此可在
 * Node 里直接单测（web/scripts/test-source-project.mjs）。浏览器侧的文件句柄 /
 * LRU 文本读取在 `web/src/components/source/projectLoader.ts`。
 *
 * 纯函数分组：
 *  1. 常量与路径规范化（Windows/Unix 混用、`..`/`.`、根目录名前缀）；
 *  2. 目录条目扫描（扩展名/忽略目录/大小/数量上限）与文件树构建；
 *  3. ELF 发现（按 lastModified 取最新）；
 *  4. 变量→源码文件路径匹配（精确/后缀/文件名）、matched/unmatched 统计；
 *  5. 点击位置的候选变量（同一行同名多候选）；
 *  6. 源码行 tokenizer（变量高亮、块注释跨行状态）+ 行虚拟化窗口；
 *  7. LRU 缓存（供 loader 按需读文件、限制内存）。
 */

// ---------------------------------------------------------------------------
// 1. 常量
// ---------------------------------------------------------------------------

/** 参与源码浏览的扩展名（大小写不敏感；不含 `.S` 汇编）。 */
export const SOURCE_EXTENSIONS = [
    '.c',
    '.cc',
    '.cpp',
    '.cxx',
    '.h',
    '.hh',
    '.hpp',
    '.hxx',
] as const

/** 自动发现调试符号文件的扩展名。 */
export const ELF_EXTENSIONS = ['.elf'] as const

/**
 * 默认忽略的目录名（大小写不敏感，`.hidden` 目录一律忽略）。
 *
 * 注意：忽略目录只影响**源码扫描**；ELF 自动发现会额外进入 build 等目录找
 * `.elf`（构建产物通常就在那里），但不会读取其它文件内容。
 */
export const DEFAULT_IGNORED_DIRECTORIES = [
    '.git',
    '.svn',
    '.hg',
    '.bzr',
    '.idea',
    '.vs',
    '.vscode',
    '.cache',
    '.tmp',
    'node_modules',
    'bower_components',
    '__pycache__',
    'build',
    'build-debug',
    'build-release',
    'out',
    'output',
    'dist',
    'binaries',
    'CMakeFiles',
    'Debug',
    'Release',
    'x64',
    'x86',
] as const

/** 单个源码文件大小上限（2 MB，超出不进入索引，避免点击时卡死）。 */
export const MAX_SOURCE_FILE_BYTES = 2 * 1024 * 1024
/** 源码文件数量上限（超出按路径排序截断）。 */
export const MAX_SOURCE_FILES = 2000
/** 自动发现保留的 ELF 数量上限（按 lastModified 降序）。 */
export const MAX_ELF_FILES = 16
/** 源码树最大深度（防御恶意/递归很深的目录）。 */
export const MAX_TREE_DEPTH = 32
/** 变量展开节点上限（防御异常巨大的 struct/数组树）。 */
export const MAX_VARIABLE_NODES = 20000
/** loader 的文本 LRU 容量（文件数）。 */
export const DEFAULT_SOURCE_CACHE_CAPACITY = 8

const DEFAULT_SOURCE_EXTENSION_SET: ReadonlySet<string> = new Set(SOURCE_EXTENSIONS)
const DEFAULT_ELF_EXTENSION_SET: ReadonlySet<string> = new Set(ELF_EXTENSIONS)
const DEFAULT_IGNORED_DIRECTORY_SET: ReadonlySet<string> = new Set(
    DEFAULT_IGNORED_DIRECTORIES.map((name) => name.toLowerCase()),
)

// ---------------------------------------------------------------------------
// 2. 路径规范化（Windows / Unix 兼用）
// ---------------------------------------------------------------------------

/**
 * 规范化路径：统一 `/`、合并重复分隔符、解析 `.`/`..`、去尾部 `/`。
 * 保留绝对路径前导 `/` 与 Windows 盘符（小写，带冒号，如 `c:/work/src`）。
 * 空输入返回 `''`。
 */
export function normalizePath(input: string): string {
    const raw = String(input ?? '').trim().replace(/\\/g, '/')
    if (!raw) return ''
    const driveMatch = /^([A-Za-z]):/.exec(raw)
    const drive = driveMatch ? `${driveMatch[1].toLowerCase()}:` : ''
    let rest = driveMatch ? raw.slice(driveMatch[0].length) : raw
    const absolute = rest.startsWith('/')
    const parts: string[] = []
    for (const segment of rest.split('/')) {
        if (!segment || segment === '.') continue
        if (segment === '..') {
            if (parts.length > 0 && parts[parts.length - 1] !== '..') parts.pop()
            else if (!absolute) parts.push('..')
            continue
        }
        parts.push(segment)
    }
    rest = parts.join('/')
    if (drive) return `${drive}/${rest}`
    if (absolute) return `/${rest}`
    return rest
}

/** 规范化为项目相对路径：去掉盘符/前导 `/`，`..` 在根处截断。 */
export function normalizeRelativePath(input: string): string {
    let path = normalizePath(input)
    if (!path) return ''
    path = path.replace(/^[A-Za-z]:\//, '').replace(/^\/+/, '')
    const parts: string[] = []
    for (const segment of path.split('/')) {
        if (!segment || segment === '.') continue
        if (segment === '..') {
            parts.pop()
            continue
        }
        parts.push(segment)
    }
    return parts.join('/')
}

/** DWARF 路径是否绝对（`/...` 或 `c:/...`）。 */
export function isAbsoluteDwarfPath(path: string): boolean {
    return /^\/|^[A-Za-z]:[\\/]/.test(String(path ?? '').trim())
}

export function pathSegments(path: string): string[] {
    const normalized = normalizePath(path)
    if (!normalized) return []
    return normalized.split('/').filter(Boolean)
}

export function basename(path: string): string {
    const segments = pathSegments(path)
    return segments.length > 0 ? segments[segments.length - 1] : ''
}

export function dirname(path: string): string {
    const normalized = normalizePath(path)
    const index = normalized.lastIndexOf('/')
    if (index <= 0) return ''
    return normalized.slice(0, index)
}

/** 扩展名（含点，原样大小写；无扩展名返回 `''`）。 */
export function extname(path: string): string {
    const name = basename(path)
    const index = name.lastIndexOf('.')
    return index > 0 ? name.slice(index) : ''
}

/** 拼接并规范化路径（用于 compDir + file）。 */
export function joinPath(...parts: string[]): string {
    return normalizePath(parts.filter((part) => !!part).join('/'))
}

/**
 * 拆分 `<input webkitdirectory>` 给出的 `File.webkitRelativePath`：
 *   `proj/src/main.c` -> `{rootName: 'proj', relativePath: 'src/main.c'}`
 *   `main.c`（无根目录） -> `{rootName: '', relativePath: 'main.c'}`
 */
export function splitWebkitRelativePath(webkitRelativePath: string): { rootName: string; relativePath: string } {
    const segments = pathSegments(webkitRelativePath)
    if (segments.length <= 1) return { rootName: '', relativePath: segments[0] ?? '' }
    return { rootName: segments[0], relativePath: segments.slice(1).join('/') }
}

/** 去掉一层根目录名（仅当首段匹配时）；用于把 webkit 路径转成项目相对路径。 */
export function stripRootSegment(path: string, rootName: string): string {
    const relative = normalizeRelativePath(path)
    const root = normalizeRelativePath(rootName)
    if (!root) return relative
    if (relative === root) return ''
    if (relative.startsWith(`${root}/`)) return relative.slice(root.length + 1)
    return relative
}

function normalizeExtensionList(extensions: readonly string[] | undefined, fallback: ReadonlySet<string>): ReadonlySet<string> {
    if (!extensions) return fallback
    const set = new Set<string>()
    for (const extension of extensions) {
        const trimmed = String(extension ?? '').trim().toLowerCase()
        if (!trimmed) continue
        set.add(trimmed.startsWith('.') ? trimmed : `.${trimmed}`)
    }
    return set
}

/** 是否源码扩展名（大小写不敏感）。 */
export function hasSourceExtension(nameOrPath: string, extensions?: readonly string[]): boolean {
    const ext = extname(nameOrPath).toLowerCase()
    if (!ext) return false
    return normalizeExtensionList(extensions, DEFAULT_SOURCE_EXTENSION_SET).has(ext)
}

/** 是否 ELF 扩展名（大小写不敏感）。 */
export function hasElfExtension(nameOrPath: string, extensions?: readonly string[]): boolean {
    const ext = extname(nameOrPath).toLowerCase()
    if (!ext) return false
    return normalizeExtensionList(extensions, DEFAULT_ELF_EXTENSION_SET).has(ext)
}

/**
 * 目录名是否忽略：默认列表 + 隐藏目录 + `build-*` / `out-*` / `cmake-build-*`
 * 等常见构建目录；可通过 `extra` 追加。
 */
export function isIgnoredDirectory(name: string, extra?: readonly string[]): boolean {
    const lower = String(name ?? '').trim().toLowerCase()
    if (!lower) return false
    if (lower.startsWith('.')) return true
    if (DEFAULT_IGNORED_DIRECTORY_SET.has(lower)) return true
    if (extra) {
        for (const item of extra) {
            if (String(item ?? '').trim().toLowerCase() === lower) return true
        }
    }
    return /^(build|out|dist|cmake-build|build-debug|build-release)[-_].*$/.test(lower)
}

// ---------------------------------------------------------------------------
// 3. 项目元信息 / 目录扫描
// ---------------------------------------------------------------------------

/** loader 从浏览器拿到的原始文件条目（路径为项目相对路径）。 */
export interface RawProjectEntry {
    path: string
    size?: number
    lastModified?: number
}

export interface SourceFileMeta {
    /** 项目相对路径（规范化，`/` 分隔） */
    path: string
    /** 文件名 */
    name: string
    /** 小写扩展名（含点） */
    ext: string
    size: number
    lastModified: number
}

export interface ElfFileMeta {
    path: string
    name: string
    size: number
    lastModified: number
}

/** 源码目录树（根节点的 `path` 为空串）。 */
export interface SourceTreeNode {
    name: string
    path: string
    dirs: SourceTreeNode[]
    files: SourceFileMeta[]
    /** 直属文件数 */
    fileCount: number
    /** 含子目录的文件总数 */
    totalFileCount: number
}

export type ProjectAccessKind = 'fs-access' | 'input' | 'provided'

export interface ScanLimits {
    maxFiles: number
    maxFileBytes: number
    maxElfFiles: number
}

export interface ProjectScanStats {
    /** 传入条目总数 */
    totalEntries: number
    /** 最终纳入索引的源码文件数 */
    sourceCount: number
    /** 因位于忽略目录（build/.git/...）而跳过的文件数 */
    ignoredDirectoryEntries: number
    /** 非源码/非 ELF 文件数 */
    unsupportedFiles: number
    /** 超过单文件大小上限而跳过的源码文件数 */
    tooLargeFiles: number
    /** 超过文件数量上限而被截断的源码文件数 */
    overLimitFiles: number
    /** 发现的 ELF 文件总数（截断前） */
    elfCount: number
}

export interface ScanProjectOptions {
    /** 项目根目录名（仅用于展示/树根名，不参与路径拼接） */
    rootName?: string
    access?: ProjectAccessKind
    maxFiles?: number
    maxFileBytes?: number
    maxElfFiles?: number
    /** 追加忽略的目录名（大小写不敏感） */
    ignoredDirectories?: readonly string[]
    sourceExtensions?: readonly string[]
    elfExtensions?: readonly string[]
    /**
     * 文件上限截断前的优先路径前缀（按顺序优先，默认 reference firmware 的 src/、robot/）。
     * 大型工程根目录常包含 hw/vendor 等大量文件；不优先会按字母序把 src 截掉。
     */
    priorityPrefixes?: readonly string[]
}

export interface SourceProjectMeta {
    rootName: string
    access: ProjectAccessKind
    files: SourceFileMeta[]
    tree: SourceTreeNode
    sourceCount: number
    elfFiles: ElfFileMeta[]
    stats: ProjectScanStats
    limits: ScanLimits
}

function compareText(left: string, right: string): number {
    return left < right ? -1 : left > right ? 1 : 0
}

function ignoredParentDirectory(segments: readonly string[], extra?: readonly string[]): boolean {
    for (let index = 0; index < segments.length - 1; index++) {
        if (isIgnoredDirectory(segments[index], extra)) return true
    }
    return false
}

function toFiniteNumber(value: unknown): number {
    return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

/**
 * 扫描原始文件条目，返回项目元信息（源码文件列表 + 树 + ELF 列表 + 统计）。
 *
 * 过滤顺序（先 ELF 后忽略目录，保证 build/ 下的 .elf 能被发现）：
 *  1. 目录条目（路径以 `/` 结尾或空）忽略；
 *  2. `.elf` 记为 ELF 候选（不受忽略目录/大小限制影响）；
 *  3. 位于忽略目录的其它文件 -> `ignoredDirectoryEntries`；
 *  4. 非源码扩展名 -> `unsupportedFiles`；
 *  5. 超过单文件大小上限 -> `tooLargeFiles`；
 *  6. 按路径排序后截断到 `maxFiles`（`overLimitFiles` 记录被截断数）。
 */
export function scanProjectEntries(entries: readonly RawProjectEntry[], options: ScanProjectOptions = {}): SourceProjectMeta {
    const limits: ScanLimits = {
        maxFiles: Math.max(1, Math.floor(toFiniteNumber(options.maxFiles) || MAX_SOURCE_FILES)),
        maxFileBytes: Math.max(1, Math.floor(toFiniteNumber(options.maxFileBytes) || MAX_SOURCE_FILE_BYTES)),
        maxElfFiles: Math.max(1, Math.floor(toFiniteNumber(options.maxElfFiles) || MAX_ELF_FILES)),
    }
    const sourceExtensionSet = normalizeExtensionList(options.sourceExtensions, DEFAULT_SOURCE_EXTENSION_SET)
    const elfExtensionSet = normalizeExtensionList(options.elfExtensions, DEFAULT_ELF_EXTENSION_SET)

    const stats: ProjectScanStats = {
        totalEntries: entries.length,
        sourceCount: 0,
        ignoredDirectoryEntries: 0,
        unsupportedFiles: 0,
        tooLargeFiles: 0,
        overLimitFiles: 0,
        elfCount: 0,
    }
    const accepted: SourceFileMeta[] = []
    const elfMap = new Map<string, ElfFileMeta>()

    for (const entry of entries) {
        const rawPath = String(entry?.path ?? '').trim()
        if (!rawPath || rawPath.endsWith('/') || rawPath.endsWith('\\')) continue
        const relativePath = normalizeRelativePath(rawPath)
        if (!relativePath) continue
        const ext = extname(relativePath).toLowerCase()
        if (ext && elfExtensionSet.has(ext)) {
            if (!elfMap.has(relativePath)) {
                elfMap.set(relativePath, {
                    path: relativePath,
                    name: basename(relativePath),
                    size: toFiniteNumber(entry.size),
                    lastModified: toFiniteNumber(entry.lastModified),
                })
            }
            continue
        }
        const segments = relativePath.split('/')
        if (ignoredParentDirectory(segments, options.ignoredDirectories)) {
            stats.ignoredDirectoryEntries += 1
            continue
        }
        if (!ext || !sourceExtensionSet.has(ext)) {
            stats.unsupportedFiles += 1
            continue
        }
        const size = toFiniteNumber(entry.size)
        if (size > limits.maxFileBytes) {
            stats.tooLargeFiles += 1
            continue
        }
        accepted.push({
            path: relativePath,
            name: basename(relativePath),
            ext,
            size,
            lastModified: toFiniteNumber(entry.lastModified),
        })
    }

    accepted.sort((left, right) => compareText(left.path, right.path))
    stats.elfCount = elfMap.size
    if (accepted.length > limits.maxFiles) {
        const priorityPrefixes = (options.priorityPrefixes ?? ['src/', 'robot/'])
            .map((prefix) => normalizeRelativePath(prefix).replace(/\/+$/, '') + '/')
            .filter((prefix) => prefix.length > 1)
        const priorityRank = (path: string): number => {
            for (let index = 0; index < priorityPrefixes.length; index++) {
                if (path.startsWith(priorityPrefixes[index])) return index
            }
            return priorityPrefixes.length
        }
        accepted.sort((left, right) => {
            const rank = priorityRank(left.path) - priorityRank(right.path)
            return rank !== 0 ? rank : compareText(left.path, right.path)
        })
        stats.overLimitFiles = accepted.length - limits.maxFiles
        accepted.length = limits.maxFiles
    }
    stats.sourceCount = accepted.length

    const elfFiles = [...elfMap.values()].sort((left, right) => {
        if (right.lastModified !== left.lastModified) return right.lastModified - left.lastModified
        return compareText(left.path, right.path)
    })
    if (elfFiles.length > limits.maxElfFiles) elfFiles.length = limits.maxElfFiles

    const rootName = String(options.rootName ?? '').trim()
    return {
        rootName,
        access: options.access ?? 'provided',
        files: accepted,
        tree: buildSourceTree(accepted, rootName),
        sourceCount: accepted.length,
        elfFiles,
        stats,
        limits,
    }
}

/** 由源码文件列表构建目录树（目录在前、按名称排序；文件按名称排序）。 */
export function buildSourceTree(files: readonly SourceFileMeta[], rootName = ''): SourceTreeNode {
    const root: SourceTreeNode = {
        name: rootName,
        path: '',
        dirs: [],
        files: [],
        fileCount: 0,
        totalFileCount: 0,
    }
    const directoryMap = new Map<string, SourceTreeNode>([['', root]])
    const sortedFiles = [...files].sort((left, right) => compareText(normalizeRelativePath(left.path), normalizeRelativePath(right.path)))
    for (const file of sortedFiles) {
        const path = normalizeRelativePath(file.path)
        if (!path) continue
        const segments = path.split('/')
        if (segments.length > MAX_TREE_DEPTH) continue
        let current = root
        for (let index = 0; index < segments.length - 1; index++) {
            const segment = segments[index]
            const childPath = current.path ? `${current.path}/${segment}` : segment
            let child = directoryMap.get(childPath)
            if (!child) {
                child = {
                    name: segment,
                    path: childPath,
                    dirs: [],
                    files: [],
                    fileCount: 0,
                    totalFileCount: 0,
                }
                directoryMap.set(childPath, child)
                current.dirs.push(child)
            }
            current = child
        }
        current.files.push({
            path,
            name: segments[segments.length - 1],
            ext: file.ext || extname(path).toLowerCase(),
            size: toFiniteNumber(file.size),
            lastModified: toFiniteNumber(file.lastModified),
        })
    }
    const finalize = (node: SourceTreeNode): number => {
        node.dirs.sort((left, right) => compareText(left.name, right.name))
        node.files.sort((left, right) => compareText(left.name, right.name))
        node.fileCount = node.files.length
        let total = node.files.length
        for (const child of node.dirs) total += finalize(child)
        node.totalFileCount = total
        return total
    }
    finalize(root)
    return root
}

/** 按 lastModified 取最新 ELF（同时间按路径排序）。 */
export function pickLatestElf(elfFiles: readonly ElfFileMeta[]): ElfFileMeta | null {
    if (elfFiles.length === 0) return null
    const sorted = [...elfFiles].sort((left, right) => {
        if (right.lastModified !== left.lastModified) return right.lastModified - left.lastModified
        return compareText(left.path, right.path)
    })
    return sorted[0] ?? null
}

/** 源码文件搜索（路径子串，大小写不敏感；空查询返回前 `limit` 个）。 */
export function searchSourceFiles(files: readonly SourceFileMeta[], query: string, limit = 200): SourceFileMeta[] {
    const max = Math.max(1, Math.floor(toFiniteNumber(limit) || 200))
    const needle = String(query ?? '').trim().toLowerCase()
    if (!needle) return files.slice(0, max)
    const result: SourceFileMeta[] = []
    for (const file of files) {
        if (file.path.toLowerCase().includes(needle)) {
            result.push(file)
            if (result.length >= max) break
        }
    }
    return result
}

// ---------------------------------------------------------------------------
// 4. 文件树扁平化（侧栏渲染用；只展开 expanded 中的目录）
// ---------------------------------------------------------------------------

export type SourceTreeRow =
    | { kind: 'directory'; path: string; name: string; depth: number; totalFileCount: number }
    | { kind: 'file'; path: string; name: string; depth: number; file: SourceFileMeta }

/**
 * 把目录树摊平成可渲染行（根节点本身不输出，其子项 depth=0）。
 * 只递归 expanded 中的目录路径；其余目录折叠。
 */
export function flattenSourceTree(tree: SourceTreeNode, expanded: ReadonlySet<string>): SourceTreeRow[] {
    const rows: SourceTreeRow[] = []
    const walk = (node: SourceTreeNode, depth: number): void => {
        for (const dir of node.dirs) {
            rows.push({kind: 'directory', path: dir.path, name: dir.name, depth, totalFileCount: dir.totalFileCount})
            if (expanded.has(dir.path)) walk(dir, depth + 1)
        }
        for (const file of node.files) {
            rows.push({kind: 'file', path: file.path, name: file.name, depth, file})
        }
    }
    walk(tree, 0)
    return rows
}

// ---------------------------------------------------------------------------
// 5. DWARF 变量 -> 源码文件匹配
// ---------------------------------------------------------------------------

/**
 * 精简变量接口：`VariableNode`（elfTypes.ts）结构上可直接传入。
 * 只列出映射/高亮需要的字段，避免本模块依赖 DWARF 解析器。
 */
export interface SourceVariableLike {
    /** 节点名（根为限定名，成员为 `.member`，数组元素为 `[i]`） */
    name?: string
    shortName?: string
    qualifiedName?: string
    mangledName?: string
    typeName?: string
    /** DWARF 源文件完整路径 */
    file?: string
    /** 去掉构建/项目根前缀的相对路径（优先用于匹配） */
    relativeFile?: string
    /** CU compDir（file 为相对路径时用于拼出完整路径；可选） */
    compDir?: string
    line?: number
    /** 节点地址（用于生成与 DebugCurve 一致的选中 key） */
    address?: number
    size?: number
    sampleable?: boolean
    /** 可采样叶子（只用到地址/寄存器是否存在） */
    sample?: { address?: number; register?: number; kind?: string } | null
    location?: unknown
    children?: readonly SourceVariableLike[]
}

/** 展开后的变量条目（`path`/`key` 与 DebugCurve 变量树同语义）。 */
export interface SourceVariableEntry<V extends SourceVariableLike = SourceVariableLike> {
    variable: V
    /** 变量树路径：根为 name，子节点为 parentPath + name（如 `obj.member`） */
    path: string
    /** 选中键：`${path}@${address.toString(16)}`（与 DebugCurve nodeKey 一致） */
    key: string
    depth: number
}

/** 与 DebugCurve `nodeKey(path, node)` 完全一致：`path@address(hex)`。 */
export function sourceVariableKey(path: string, variable: SourceVariableLike): string {
    const address = (toFiniteNumber(variable?.address) >>> 0).toString(16)
    return `${path}@${address}`
}

/**
 * 深度优先展开变量树（默认含 children）。`maxNodes` 防御异常大的结构树。
 * 传入 `VariableNode[]` 时返回的条目 `variable` 仍是原对象引用。
 */
export function walkSourceVariables<V extends SourceVariableLike>(
    variables: readonly V[],
    options: { includeChildren?: boolean; maxNodes?: number } = {},
): SourceVariableEntry<V>[] {
    const maxNodes = Math.max(1, Math.floor(toFiniteNumber(options.maxNodes) || MAX_VARIABLE_NODES))
    const includeChildren = options.includeChildren !== false
    const out: SourceVariableEntry<V>[] = []
    const visit = (variable: V, parentPath: string, depth: number): void => {
        if (out.length >= maxNodes) return
        const name = String(variable?.name ?? '')
        const path = depth === 0 ? name : `${parentPath}${name}`
        out.push({variable, path, key: sourceVariableKey(path, variable), depth})
        if (!includeChildren || !variable?.children || variable.children.length === 0) return
        for (const child of variable.children) {
            if (out.length >= maxNodes) return
            visit(child as V, path, depth + 1)
        }
    }
    for (const variable of variables) {
        if (out.length >= maxNodes) break
        visit(variable, '', 0)
    }
    return out
}

function lastScopeSegment(name: string): string {
    const scopeIndex = name.lastIndexOf('::')
    return scopeIndex >= 0 ? name.slice(scopeIndex + 2) : name
}

/**
 * 源码中的变量标识符：优先 `shortName`，否则从 `qualifiedName`/`name` 取最后一段，
 * 并去掉成员前缀 `.` / 数组下标 `[i]`。数组元素（`[0]` 等）返回空串（不参与高亮）。
 */
export function variableIdentifier(variable: SourceVariableLike): string {
    if (variable?.shortName) return variable.shortName
    let name = lastScopeSegment(String(variable?.qualifiedName ?? variable?.name ?? ''))
    name = name.replace(/^[.[\]]+/, '').replace(/\]$/, '')
    const dotIndex = name.lastIndexOf('.')
    if (dotIndex >= 0) name = name.slice(dotIndex + 1)
    if (!/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(name)) return ''
    return name
}

/** 该变量是否可被页面选中采样（结构型判断：`VariableNode.sampleable` 或 sample/location 存在）。 */
export function isVariableSampleable(variable: SourceVariableLike): boolean {
    if (typeof variable?.sampleable === 'boolean') return variable.sampleable
    return !!(variable?.sample || variable?.location)
}

/** 变量用于匹配的候选源码路径（优先级：relativeFile > compDir+file > file）。 */
export function variableSourcePaths(variable: SourceVariableLike): string[] {
    const out: string[] = []
    const push = (path?: string): void => {
        if (!path) return
        const normalized = normalizePath(path)
        if (!normalized) return
        if (!out.includes(normalized)) out.push(normalized)
    }
    if (variable?.relativeFile) push(variable.relativeFile)
    if (variable?.file) {
        if (!isAbsoluteDwarfPath(variable.file) && variable.compDir) {
            push(joinPath(variable.compDir, variable.file))
        }
        push(variable.file)
    }
    return out
}

export interface SourceFileIndex {
    files: readonly SourceFileMeta[]
    byPath: Map<string, SourceFileMeta>
    byLowerPath: Map<string, SourceFileMeta>
    /** 小写后缀 -> 文件（如 `src/a.c`、`a.c` 都能索引到 `src/a.c`） */
    bySuffix: Map<string, SourceFileMeta[]>
    /** 小写文件名 -> 文件 */
    byName: Map<string, SourceFileMeta[]>
}

function pushMapList<K, V>(map: Map<K, V[]>, key: K, value: V): void {
    const list = map.get(key)
    if (list) list.push(value)
    else map.set(key, [value])
}

/** 建立项目文件索引（供变量路径匹配；2000 文件规模下 O(路径深度) 查询）。 */
export function createSourceFileIndex(files: readonly SourceFileMeta[]): SourceFileIndex {
    const byPath = new Map<string, SourceFileMeta>()
    const byLowerPath = new Map<string, SourceFileMeta>()
    const bySuffix = new Map<string, SourceFileMeta[]>()
    const byName = new Map<string, SourceFileMeta[]>()
    for (const file of files) {
        const path = normalizeRelativePath(file.path) || String(file.path ?? '')
        if (!path) continue
        byPath.set(path, file)
        const lower = path.toLowerCase()
        byLowerPath.set(lower, file)
        const segments = lower.split('/').filter(Boolean)
        for (let start = 0; start < segments.length; start++) {
            pushMapList(bySuffix, segments.slice(start).join('/'), file)
        }
        pushMapList(byName, segments[segments.length - 1] ?? lower, file)
    }
    return {files, byPath, byLowerPath, bySuffix, byName}
}

function chooseShortestPath(candidates: readonly SourceFileMeta[]): SourceFileMeta {
    let best = candidates[0]
    let bestDepth = pathSegments(best.path).length
    for (const candidate of candidates) {
        const depth = pathSegments(candidate.path).length
        if (depth < bestDepth || (depth === bestDepth && compareText(candidate.path, best.path) < 0)) {
            best = candidate
            bestDepth = depth
        }
    }
    return best
}

export interface PathFileMatch {
    file: SourceFileMeta
    /** 匹配得分（越大越可靠；仅用于同一变量的多个候选路径间排序） */
    score: number
    /** 有多个同分候选文件（如同名文件），页面应提示 */
    ambiguous: boolean
}

/**
 * 单个路径 -> 项目文件匹配。顺序：
 *  1. 完整相对路径精确匹配（大小写敏感 110 / 不敏感 105）；
 *  2. 变量路径的“目录后缀”命中项目文件（绝对路径 -> 项目相对路径，90+）；
 *  3. 项目文件路径以变量路径结尾（变量路径更短，80+）；
 *  4. 仅文件名相同（50，ambiguous）。
 */
export function matchPathToProjectFile(path: string, index: SourceFileIndex): PathFileMatch | null {
    const relativePath = normalizeRelativePath(path)
    if (!relativePath) return null
    const exact = index.byPath.get(relativePath)
    if (exact) return {file: exact, score: 110, ambiguous: false}
    const exactLower = index.byLowerPath.get(relativePath.toLowerCase())
    if (exactLower) return {file: exactLower, score: 105, ambiguous: false}

    const segments = relativePath.split('/')
    // 变量路径更长：逐级丢弃开头的目录段（保留至少“目录/文件名”两段以上）
    for (let start = 1; start <= segments.length - 2; start++) {
        const suffix = segments.slice(start).join('/').toLowerCase()
        const candidates = index.bySuffix.get(suffix)
        if (!candidates || candidates.length === 0) continue
        return {
            file: chooseShortestPath(candidates),
            score: 90 + Math.min(segments.length - start, 8),
            ambiguous: candidates.length > 1,
        }
    }
    // 变量路径更短：项目文件以该路径结尾（如变量只有 `system/a.c`，项目为 `src/system/a.c`）
    const shorterCandidates = index.bySuffix.get(relativePath.toLowerCase())
    if (shorterCandidates && shorterCandidates.length > 0) {
        return {
            file: chooseShortestPath(shorterCandidates),
            score: 80 + Math.min(segments.length, 8),
            ambiguous: shorterCandidates.length > 1,
        }
    }
    // 最后退化为文件名匹配
    const nameCandidates = index.byName.get(segments[segments.length - 1].toLowerCase())
    if (!nameCandidates || nameCandidates.length === 0) return null
    return {file: chooseShortestPath(nameCandidates), score: 50, ambiguous: nameCandidates.length > 1}
}

/** 变量 -> 项目文件：依次尝试 relativeFile / compDir+file / file，取最高得分。 */
export function matchVariableToProjectFile(variable: SourceVariableLike, index: SourceFileIndex): PathFileMatch | null {
    let best: PathFileMatch | null = null
    for (const path of variableSourcePaths(variable)) {
        const match = matchPathToProjectFile(path, index)
        if (!match) continue
        if (!best || match.score > best.score) best = match
    }
    return best
}

/** 映射到某文件的变量引用（供高亮/候选/跳转）。 */
export interface FileVariableRef<V extends SourceVariableLike = SourceVariableLike> {
    entry: SourceVariableEntry<V>
    variable: V
    /** 变量树路径（如 `sn_tools.data`） */
    path: string
    /** 选中键（与 DebugCurve 一致） */
    key: string
    /** 源码标识符（可能为空：数组元素等） */
    identifier: string
    line?: number
}

export interface VariableFileMatch<V extends SourceVariableLike = SourceVariableLike> {
    entry: SourceVariableEntry<V>
    file: SourceFileMeta
    score: number
    ambiguous: boolean
    identifier: string
    line?: number
}

export interface VariableFileMatchResult<V extends SourceVariableLike = SourceVariableLike> {
    variables: SourceVariableEntry<V>[]
    index: SourceFileIndex
    matches: VariableFileMatch<V>[]
    refsByFile: Map<string, FileVariableRef<V>[]>
    unmatched: SourceVariableEntry<V>[]
    matchedCount: number
    unmatchedCount: number
    ambiguousCount: number
    total: number
}

function normalizeLine(line: unknown): number | undefined {
    if (typeof line !== 'number' || !Number.isFinite(line) || line <= 0) return undefined
    return Math.floor(line)
}

function compareRefs<V extends SourceVariableLike>(left: FileVariableRef<V>, right: FileVariableRef<V>): number {
    const leftLine = left.line ?? Number.MAX_SAFE_INTEGER
    const rightLine = right.line ?? Number.MAX_SAFE_INTEGER
    if (leftLine !== rightLine) return leftLine - rightLine
    return compareText(left.path, right.path)
}

/** 把已展开变量条目映射到项目文件（核心匹配入口；`mapVariablesToProjectFiles` 是便捷封装）。 */
export function matchVariablesToFiles<V extends SourceVariableLike>(
    entries: readonly SourceVariableEntry<V>[],
    index: SourceFileIndex,
): VariableFileMatchResult<V> {
    const matches: VariableFileMatch<V>[] = []
    const refsByFile = new Map<string, FileVariableRef<V>[]>()
    const unmatched: SourceVariableEntry<V>[] = []
    let ambiguousCount = 0
    for (const entry of entries) {
        const identifier = variableIdentifier(entry.variable)
        const line = normalizeLine(entry.variable?.line)
        const match = matchVariableToProjectFile(entry.variable, index)
        if (!match) {
            unmatched.push(entry)
            continue
        }
        if (match.ambiguous) ambiguousCount += 1
        const filePath = normalizeRelativePath(match.file.path) || match.file.path
        const ref: FileVariableRef<V> = {
            entry,
            variable: entry.variable,
            path: entry.path,
            key: entry.key,
            identifier,
            line,
        }
        pushMapList(refsByFile, filePath, ref)
        matches.push({
            entry,
            file: match.file,
            score: match.score,
            ambiguous: match.ambiguous,
            identifier,
            line,
        })
    }
    for (const refs of refsByFile.values()) refs.sort(compareRefs)
    matches.sort((left, right) => {
        const pathCompare = compareText(left.file.path, right.file.path)
        if (pathCompare !== 0) return pathCompare
        return (left.line ?? Number.MAX_SAFE_INTEGER) - (right.line ?? Number.MAX_SAFE_INTEGER)
    })
    return {
        variables: [...entries],
        index,
        matches,
        refsByFile,
        unmatched,
        matchedCount: matches.length,
        unmatchedCount: unmatched.length,
        ambiguousCount,
        total: entries.length,
    }
}

/** 便捷封装：变量树 + 项目文件列表 -> 映射结果（内部创建索引并展开变量）。 */
export function mapVariablesToProjectFiles<V extends SourceVariableLike>(
    variables: readonly V[],
    files: readonly SourceFileMeta[],
    options: { includeChildren?: boolean; maxNodes?: number; index?: SourceFileIndex } = {},
): VariableFileMatchResult<V> {
    const entries = walkSourceVariables(variables, options)
    return matchVariablesToFiles(entries, options.index ?? createSourceFileIndex(files))
}

/** 取某文件的变量引用（文件路径不存在时返回空数组）。 */
export function refsForFile<V extends SourceVariableLike>(
    result: VariableFileMatchResult<V> | null | undefined,
    filePath: string,
): FileVariableRef<V>[] {
    if (!result || !filePath) return []
    return result.refsByFile.get(normalizeRelativePath(filePath)) ?? []
}

/** 某文件里出现过的变量标识符集合（供 tokenizer 高亮）。 */
export function collectIdentifiers<V extends SourceVariableLike>(refs: readonly FileVariableRef<V>[]): Set<string> {
    const identifiers = new Set<string>()
    for (const ref of refs) {
        if (ref.identifier) identifiers.add(ref.identifier)
    }
    return identifiers
}

/** 含变量定义/引用的行号（升序去重）。 */
export function variableLines<V extends SourceVariableLike>(refs: readonly FileVariableRef<V>[]): number[] {
    const lines = new Set<number>()
    for (const ref of refs) {
        if (ref.line !== undefined) lines.add(ref.line)
    }
    return [...lines].sort((left, right) => left - right)
}

/** 行号 -> 该行的变量引用。 */
export function groupReferencesByLine<V extends SourceVariableLike>(
    refs: readonly FileVariableRef<V>[],
): Map<number, FileVariableRef<V>[]> {
    const grouped = new Map<number, FileVariableRef<V>[]>()
    for (const ref of refs) {
        if (ref.line === undefined) continue
        pushMapList(grouped, ref.line, ref)
    }
    return grouped
}

/** 某行上的变量引用。 */
export function referencesAtLine<V extends SourceVariableLike>(
    refs: readonly FileVariableRef<V>[],
    line: number,
): FileVariableRef<V>[] {
    return refs.filter((ref) => ref.line === line)
}

/** 初始打开文件：变量命中数最多的文件（平票按路径）。没有匹配返回 null。 */
export function pickInitialSourceFile<V extends SourceVariableLike>(result: VariableFileMatchResult<V> | null | undefined): string | null {
    if (!result || result.refsByFile.size === 0) return null
    let bestPath = ''
    let bestCount = -1
    for (const [path, refs] of result.refsByFile) {
        if (refs.length > bestCount || (refs.length === bestCount && compareText(path, bestPath) < 0)) {
            bestPath = path
            bestCount = refs.length
        }
    }
    return bestPath || null
}

export type CandidateScope = 'line' | 'file' | 'project'

export interface VariableCandidate<V extends SourceVariableLike = SourceVariableLike> {
    ref: FileVariableRef<V>
    scope: CandidateScope
    /** 变量行号与点击行一致（同名同一行多候选的核心判据） */
    lineMatched: boolean
    sampleable: boolean
}

export interface VariableCandidateQuery {
    filePath: string
    /** 点击所在行（1 起始；缺失时只按名字匹配） */
    line?: number
    /** 点击的标识符文本 */
    identifier: string
    /** 是否回退到项目内同名变量（默认 true） */
    includeProject?: boolean
}

/**
 * 点击源码 token -> 候选变量列表。顺序：
 *  1. 同一文件、同一行、同名（`scope=line`，同名同行多候选全部返回）；
 *  2. 同一文件、同名（声明/使用行不同，`scope=file`）；
 *  3. 项目内其它文件的同名变量（`scope=project`，仅在 includeProject 时）。
 * 相同 key 只保留一次；每组内按行号/路径排序。
 */
export function findVariableCandidates<V extends SourceVariableLike>(
    result: VariableFileMatchResult<V> | null | undefined,
    query: VariableCandidateQuery,
): VariableCandidate<V>[] {
    if (!result) return []
    const identifier = String(query.identifier ?? '').trim()
    if (!identifier) return []
    const filePath = normalizeRelativePath(query.filePath)
    const fileRefs = result.refsByFile.get(filePath) ?? []
    const line = normalizeLine(query.line)
    const seen = new Set<string>()
    const out: VariableCandidate<V>[] = []
    const push = (ref: FileVariableRef<V>, scope: CandidateScope, lineMatched: boolean): void => {
        if (seen.has(ref.key)) return
        seen.add(ref.key)
        out.push({ref, scope, lineMatched, sampleable: isVariableSampleable(ref.variable)})
    }
    if (line !== undefined) {
        for (const ref of fileRefs) {
            if (ref.identifier === identifier && ref.line === line) push(ref, 'line', true)
        }
    }
    for (const ref of fileRefs) {
        if (ref.identifier === identifier) push(ref, 'file', false)
    }
    if (query.includeProject !== false) {
        for (const match of result.matches) {
            if (match.identifier !== identifier) continue
            if (normalizeRelativePath(match.file.path) === filePath) continue
            const refs = result.refsByFile.get(normalizeRelativePath(match.file.path))
            const ref = refs?.find((candidate) => candidate.key === match.entry.key)
            if (ref) push(ref, 'project', line !== undefined && ref.line === line)
        }
    }
    return out
}

// ---------------------------------------------------------------------------
// 6. 源码 tokenizer（变量高亮 + 简单语法高亮；不使用 Monaco/CodeMirror）
// ---------------------------------------------------------------------------

export type CodeTokenKind =
    | 'plain'
    | 'punctuation'
    | 'number'
    | 'string'
    | 'char'
    | 'comment'
    | 'keyword'
    | 'preprocessor'
    | 'identifier'
    | 'variable'

export interface CodeToken {
    text: string
    kind: CodeTokenKind
    /** kind === 'variable' 时的标识符文本 */
    variable?: string
}

export interface TokenizeOptions {
    /** 需要高亮的变量标识符（通常为当前文件匹配到的变量名） */
    variableNames?: ReadonlySet<string>
    /** 关键字集合（默认 C/C++ 常用关键字；传空集可关闭语法高亮） */
    keywords?: ReadonlySet<string>
}

export interface TokenizeLineResult {
    tokens: CodeToken[]
    /** 本行结束时是否仍处于块注释 */
    inBlockComment: boolean
}

/** C/C++ 常用关键字（用于轻量语法着色，不是完整词法器）。 */
export const C_KEYWORDS: ReadonlySet<string> = new Set([
    'alignas', 'alignof', 'asm', 'auto', 'bool', 'break', 'case', 'catch', 'char', 'char8_t',
    'char16_t', 'char32_t', 'class', 'const', 'consteval', 'constexpr', 'constinit', 'const_cast',
    'continue', 'co_await', 'co_return', 'co_yield', 'decltype', 'default', 'delete', 'do',
    'double', 'dynamic_cast', 'else', 'enum', 'explicit', 'export', 'extern', 'false', 'float',
    'for', 'friend', 'goto', 'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new',
    'noexcept', 'nullptr', 'operator', 'private', 'protected', 'public', 'register',
    'reinterpret_cast', 'requires', 'return', 'short', 'signed', 'sizeof', 'static',
    'static_assert', 'static_cast', 'struct', 'switch', 'template', 'this', 'thread_local',
    'throw', 'true', 'try', 'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using',
    'virtual', 'void', 'volatile', 'wchar_t', 'while',
])

const IDENTIFIER_START_RE = /[A-Za-z_$]/
const IDENTIFIER_PART_RE = /[A-Za-z0-9_$]/
const DIGIT_RE = /[0-9]/
const WHITESPACE_RE = /\s/
const NUMBER_RE = /^(?:0[xX][0-9a-fA-F']+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)[uUlLfF]*/
const PREPROCESSOR_LINE_RE = /^\s*#/

/**
 * 行 tokenizer：`inBlockComment` 传入上一行结束时的块注释状态（见
 * `computeBlockCommentStates`）。保证 `tokens.map(t => t.text).join('') === line`。
 */
export function tokenizeCodeLine(line: string, inBlockComment = false, options: TokenizeOptions = {}): TokenizeLineResult {
    const text = String(line ?? '')
    const variableNames = options.variableNames
    const keywords = options.keywords ?? C_KEYWORDS
    const tokens: CodeToken[] = []
    const push = (tokenText: string, kind: CodeTokenKind, variable?: string): void => {
        if (!tokenText) return
        const last = tokens[tokens.length - 1]
        if (!variable && last && last.kind === kind && last.variable === undefined) {
            last.text += tokenText
            return
        }
        tokens.push(variable ? {text: tokenText, kind, variable} : {text: tokenText, kind})
    }
    let block = inBlockComment
    const preprocessor = !block && PREPROCESSOR_LINE_RE.test(text)
    let index = 0
    while (index < text.length) {
        if (block) {
            const end = text.indexOf('*/', index)
            if (end === -1) {
                push(text.slice(index), 'comment')
                index = text.length
            } else {
                push(text.slice(index, end + 2), 'comment')
                index = end + 2
                block = false
            }
            continue
        }
        const char = text[index]
        if (char === '/' && text[index + 1] === '/') {
            push(text.slice(index), 'comment')
            index = text.length
            continue
        }
        if (char === '/' && text[index + 1] === '*') {
            block = true
            push('/*', 'comment')
            index += 2
            continue
        }
        if (char === '"' || char === "'") {
            let cursor = index + 1
            while (cursor < text.length) {
                if (text[cursor] === '\\') {
                    cursor += 2
                    continue
                }
                if (text[cursor] === char) {
                    cursor += 1
                    break
                }
                cursor += 1
            }
            cursor = Math.min(cursor, text.length)
            push(text.slice(index, cursor), char === '"' ? 'string' : 'char')
            index = cursor
            continue
        }
        if (IDENTIFIER_START_RE.test(char)) {
            let cursor = index + 1
            while (cursor < text.length && IDENTIFIER_PART_RE.test(text[cursor])) cursor += 1
            const word = text.slice(index, cursor)
            if (preprocessor) push(word, 'preprocessor')
            else if (variableNames?.has(word)) push(word, 'variable', word)
            else if (keywords.has(word)) push(word, 'keyword')
            else push(word, 'identifier')
            index = cursor
            continue
        }
        if (DIGIT_RE.test(char) || (char === '.' && DIGIT_RE.test(text[index + 1] ?? ''))) {
            const match = NUMBER_RE.exec(text.slice(index))
            if (match && match[0]) {
                push(match[0], 'number')
                index += match[0].length
                continue
            }
        }
        if (WHITESPACE_RE.test(char)) {
            let cursor = index + 1
            while (cursor < text.length && WHITESPACE_RE.test(text[cursor])) cursor += 1
            push(text.slice(index, cursor), 'plain')
            index = cursor
            continue
        }
        push(char, 'punctuation')
        index += 1
    }
    return {tokens, inBlockComment: block}
}

function skipQuotedText(line: string, startIndex: number): number {
    const quote = line[startIndex]
    let index = startIndex + 1
    while (index < line.length) {
        if (line[index] === '\\') {
            index += 2
            continue
        }
        if (line[index] === quote) return index + 1
        index += 1
    }
    return line.length
}

/** 轻量扫描单行结束时的块注释状态（跳过字符串/字符字面量，避免 `"/*"` 误判）。 */
export function lineEndsInBlockComment(line: string, startInBlock = false): boolean {
    const text = String(line ?? '')
    let block = startInBlock
    let index = 0
    while (index < text.length) {
        if (block) {
            const end = text.indexOf('*/', index)
            if (end === -1) return true
            index = end + 2
            block = false
            continue
        }
        const char = text[index]
        if (char === '/' && text[index + 1] === '/') return false
        if (char === '/' && text[index + 1] === '*') {
            block = true
            index += 2
            continue
        }
        if (char === '"' || char === "'") {
            index = skipQuotedText(text, index)
            continue
        }
        index += 1
    }
    return block
}

/**
 * 预计算每一行**行首**是否处于块注释（1 = 是），供虚拟滚动时按需 tokenize
 * 可见行；避免一次性为整个文件生成 token 对象。
 */
export function computeBlockCommentStates(lines: readonly string[]): Uint8Array {
    const states = new Uint8Array(lines.length)
    let inBlock = false
    for (let index = 0; index < lines.length; index++) {
        states[index] = inBlock ? 1 : 0
        inBlock = lineEndsInBlockComment(lines[index], inBlock)
    }
    return states
}

// ---------------------------------------------------------------------------
// 7. 行虚拟化窗口
// ---------------------------------------------------------------------------

export interface VirtualWindowParams {
    scrollTop: number
    viewportHeight: number
    rowHeight: number
    lineCount: number
    /** 上下多渲染的行数（减少快速滚动白屏） */
    overscan?: number
}

export interface VirtualWindow {
    /** 起始行下标（含） */
    start: number
    /** 结束行下标（不含） */
    end: number
    /** 起始行顶部偏移（px） */
    offsetY: number
    totalHeight: number
    overscan: number
}

/** 计算需要渲染的行窗口（纯函数，供绝对定位行渲染）。 */
export function computeVirtualWindow(params: VirtualWindowParams): VirtualWindow {
    const rowHeight = params.rowHeight > 0 ? params.rowHeight : 1
    const lineCount = Math.max(0, Math.floor(toFiniteNumber(params.lineCount)))
    const overscan = Math.max(0, Math.floor(toFiniteNumber(params.overscan)))
    const totalHeight = lineCount * rowHeight
    if (lineCount === 0) return {start: 0, end: 0, offsetY: 0, totalHeight: 0, overscan}
    const scrollTop = Math.max(0, toFiniteNumber(params.scrollTop))
    const viewportHeight = Math.max(0, toFiniteNumber(params.viewportHeight))
    const visibleCount = Math.max(1, Math.ceil(viewportHeight / rowHeight))
    let start = Math.floor(scrollTop / rowHeight) - overscan
    if (start > lineCount - 1) start = lineCount - 1
    if (start < 0) start = 0
    const end = Math.min(lineCount, start + visibleCount + overscan * 2)
    return {start, end, offsetY: start * rowHeight, totalHeight, overscan}
}

// ---------------------------------------------------------------------------
// 8. LRU 缓存（loader 按需读取文件文本，限制内存）
// ---------------------------------------------------------------------------

/**
 * 简单 LRU 缓存：`get` 命中会刷新最近使用顺序；超出容量淘汰最久未使用项。
 * 纯 JS 实现，不依赖浏览器 API。
 */
export class LruCache<K, V> {
    private readonly entries = new Map<K, V>()
    private readonly capacity: number

    constructor(capacity: number) {
        this.capacity = Math.max(1, Math.floor(toFiniteNumber(capacity) || 1))
    }

    get maxSize(): number {
        return this.capacity
    }

    get size(): number {
        return this.entries.size
    }

    /** 读取并刷新最近使用顺序；不存在返回 undefined。 */
    get(key: K): V | undefined {
        if (!this.entries.has(key)) return undefined
        const value = this.entries.get(key) as V
        this.entries.delete(key)
        this.entries.set(key, value)
        return value
    }

    /** 读取但不刷新顺序（统计/调试用）。 */
    peek(key: K): V | undefined {
        return this.entries.get(key)
    }

    has(key: K): boolean {
        return this.entries.has(key)
    }

    set(key: K, value: V): void {
        if (this.entries.has(key)) this.entries.delete(key)
        this.entries.set(key, value)
        while (this.entries.size > this.capacity) {
            const oldest = this.entries.keys().next()
            if (oldest.done) break
            this.entries.delete(oldest.value)
        }
    }

    delete(key: K): boolean {
        return this.entries.delete(key)
    }

    clear(): void {
        this.entries.clear()
    }

    /** 从最久未使用到最近使用排列的 key 列表（测试/调试用）。 */
    keys(): K[] {
        return [...this.entries.keys()]
    }
}

/** 人类可读文件大小。 */
export function formatFileSize(bytes: number): string {
    const value = toFiniteNumber(bytes)
    if (value < 0) return '-'
    if (value < 1024) return `${Math.round(value)} B`
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`
    return `${(value / (1024 * 1024)).toFixed(1)} MB`
}

/** 点击变量后 `select-variable` 事件的 payload。 */
export interface SourceVariableSelectionPayload<V extends SourceVariableLike = SourceVariableLike> {
    /** 被选中的变量（传入 `VariableNode[]` 时为原对象引用） */
    variable: V
    /** 选中键（与 DebugCurve `CurveSeries.key` = `path@address` 一致） */
    key: string
    /** 变量树路径（如 `sn_tools.data`） */
    path: string
    /** 变量映射到的项目相对文件路径 */
    filePath: string
    /** 变量声明行（若有） */
    line?: number
    /** 点击处的所有候选（同名/同行；按相关度排序，含被选中项） */
    candidates: SourceVariableEntry<V>[]
}
