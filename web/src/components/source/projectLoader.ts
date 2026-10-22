/**
 * SourceBrowser 的浏览器侧数据加载器：
 *
 *  - `showDirectoryPicker()`（File System Access API，仅安全上下文/HTTPS/localhost）；
 *  - `<input type="file" webkitdirectory multiple>` 回退（HTTP LAN 页面主路径）；
 *  - 递归收集源码/ELF 元信息，扫描规则（扩展名、忽略目录、大小/数量上限）全部
 *    委托给 `@/utils/sourceProject` 的纯函数；
 *  - **按需读取**：点击文件时才调用 `File.text()`，并用 LRU（默认 8 个文件）缓存；
 *  - 只读：不写入、不上传任何内容。
 *
 * 注意：本文件只做 DOM/File API 适配，不实现路径匹配/高亮等逻辑（那些在
 * `sourceProject.ts`，可 Node 单测）。
 */
import {
    DEFAULT_SOURCE_CACHE_CAPACITY,
    LruCache,
    MAX_ELF_FILES,
    MAX_SOURCE_FILE_BYTES,
    hasElfExtension,
    hasSourceExtension,
    isIgnoredDirectory,
    normalizeRelativePath,
    scanProjectEntries,
    splitWebkitRelativePath,
    type ElfFileMeta,
    type ProjectAccessKind,
    type RawProjectEntry,
    type ScanProjectOptions,
    type SourceFileMeta,
    type SourceProjectMeta,
} from '@/utils/sourceProject'

// ---------------------------------------------------------------------------
// 类型：File System Access API（TypeScript 5.4 的 lib.dom 尚未包含 picker）
// ---------------------------------------------------------------------------

interface DirectoryPickerOptions {
    mode?: 'read' | 'readwrite'
    id?: string
    startIn?: FileSystemHandle
}

type ShowDirectoryPicker = (options?: DirectoryPickerOptions) => Promise<FileSystemDirectoryHandle>

interface DirectoryPickerWindow extends Window {
    showDirectoryPicker?: ShowDirectoryPicker
}

/** `FileSystemDirectoryHandle` 的异步迭代扩展（lib.dom 未声明 entries()）。 */
interface AsyncDirectoryHandle extends FileSystemDirectoryHandle {
    entries(): AsyncIterableIterator<[string, FileSystemFileHandle | FileSystemDirectoryHandle]>
}

export type DirectoryPickerAvailability = 'available' | 'insecure-context' | 'unsupported'

function pickerWindow(): DirectoryPickerWindow | null {
    if (typeof window === 'undefined') return null
    return window as unknown as DirectoryPickerWindow
}

/** 当前环境是否可直接使用 `showDirectoryPicker()`（安全上下文 + API 存在）。 */
export function getDirectoryPickerAvailability(): DirectoryPickerAvailability {
    const win = pickerWindow()
    if (!win || typeof win.showDirectoryPicker !== 'function') return 'unsupported'
    if (win.isSecureContext === false) return 'insecure-context'
    return 'available'
}

export function supportsDirectoryPicker(): boolean {
    return getDirectoryPickerAvailability() === 'available'
}

/** 是否为用户取消文件选择器（AbortError）。 */
export function isAbortError(error: unknown): boolean {
    if (!error || typeof error !== 'object') return false
    return (error as { name?: unknown }).name === 'AbortError'
}

/**
 * 弹出目录选择器（File System Access API）。
 * 用户取消返回 null；其它错误照常抛出（调用方决定是否回退到 input）。
 */
export async function pickProjectDirectory(): Promise<FileSystemDirectoryHandle | null> {
    const win = pickerWindow()
    if (!win?.showDirectoryPicker) return null
    try {
        return await win.showDirectoryPicker({mode: 'read'})
    } catch (error) {
        if (isAbortError(error)) return null
        throw error
    }
}

// ---------------------------------------------------------------------------
// 已加载项目：元信息 + 按需读取
// ---------------------------------------------------------------------------

export interface SourceProject extends SourceProjectMeta {
    /** 按需读取源码文本（LRU 缓存；失败时 reject） */
    readText(path: string): Promise<string>
    /** 取 ELF 文件对象（用于交给现有 ELF 解析/上传流程）；不存在返回 null */
    readElfFile(path: string): Promise<File | null>
    /** 当前 LRU 中的文本数（调试/文档用） */
    textCacheSize(): number
}

export interface ProjectLoadOptions extends ScanProjectOptions {
    /** 扫描进度（已检查的文件/目录条目数） */
    onProgress?: (scannedEntries: number) => void
    /** 文本 LRU 容量（文件数，默认 8） */
    cacheSize?: number
}

/** 扫描期上限：防御构建目录/巨型仓库拖死页面。 */
const MAX_SCAN_ENTRIES = 50000
const MAX_SCAN_DEPTH = 24
const MAX_ELF_SCAN_DEPTH = 10
/** 忽略目录里“只找 ELF”的独立条目预算。 */
const MAX_ELF_SCAN_ENTRIES = 30000
/** 扫描阶段最多收集的源码条目（最终索引还会按 2000 截断）。 */
const MAX_COLLECTED_SOURCE_ENTRIES = 20000

interface CollectState {
    sourceEntries: RawProjectEntry[]
    elfEntries: RawProjectEntry[]
    files: Map<string, File>
    /** 主动扫描（源码+ELF）的条目预算计数 */
    scannedEntries: number
    /** 忽略目录里“只找 ELF”的独立预算，避免构建目录拖垮源码扫描 */
    elfScannedEntries: number
    lastReported: number
    onProgress?: (scannedEntries: number) => void
}

function createCollectState(options: ProjectLoadOptions): CollectState {
    return {
        sourceEntries: [],
        elfEntries: [],
        files: new Map<string, File>(),
        scannedEntries: 0,
        elfScannedEntries: 0,
        lastReported: 0,
        onProgress: options.onProgress,
    }
}

function reportProgress(state: CollectState, force = false): void {
    if (!state.onProgress) return
    if (!force && state.scannedEntries - state.lastReported < 32) return
    state.lastReported = state.scannedEntries
    state.onProgress(state.scannedEntries)
}

function entryBudgetLeft(state: CollectState): boolean {
    return state.scannedEntries < MAX_SCAN_ENTRIES
}

/** 把句柄文件记录为源码条目（仅记录元信息，不读内容）。 */
async function recordSourceFile(handle: FileSystemFileHandle, path: string, state: CollectState): Promise<void> {
    if (state.sourceEntries.length >= MAX_COLLECTED_SOURCE_ENTRIES) return
    try {
        const file = await handle.getFile()
        const relative = normalizeRelativePath(path)
        state.files.set(relative, file)
        state.sourceEntries.push({path: relative, size: file.size, lastModified: file.lastModified})
    } catch {
        // 权限/句柄失效的单个文件忽略，不影响整体扫描。
    }
}

/** 把句柄文件记录为 ELF 候选。 */
async function recordElfFile(handle: FileSystemFileHandle, path: string, state: CollectState): Promise<void> {
    try {
        const file = await handle.getFile()
        const relative = normalizeRelativePath(path)
        state.files.set(relative, file)
        state.elfEntries.push({path: relative, size: file.size, lastModified: file.lastModified})
    } catch {
        // 忽略不可读文件。
    }
}

/** 读取目录子项（带扫描预算；enumerate 是异步迭代器）。 */
async function readDirectoryChildren(
    dir: FileSystemDirectoryHandle,
    state: CollectState,
    mode: 'active' | 'elf-only',
): Promise<Array<[string, FileSystemFileHandle | FileSystemDirectoryHandle]>> {
    const children: Array<[string, FileSystemFileHandle | FileSystemDirectoryHandle]> = []
    const iterator = (dir as unknown as AsyncDirectoryHandle).entries()
    for (;;) {
        const withinBudget = mode === 'active' ? entryBudgetLeft(state) : state.elfScannedEntries < MAX_ELF_SCAN_ENTRIES
        if (!withinBudget) break
        const next = await iterator.next()
        if (next.done) break
        const [name, child] = next.value
        children.push([name, child])
        if (mode === 'active') state.scannedEntries += 1
        else state.elfScannedEntries += 1
        reportProgress(state)
    }
    return children
}

/**
 * 在“被忽略的目录”（build/out/.git/...）里只找 ELF，不收集源码；
 * 避免构建目录里成千上万中间文件拖慢扫描。
 */
async function scanElfOnlyDirectory(dir: FileSystemDirectoryHandle, prefix: string, depth: number, state: CollectState): Promise<void> {
    if (depth > MAX_ELF_SCAN_DEPTH || state.elfScannedEntries >= MAX_ELF_SCAN_ENTRIES) return
    const children = await readDirectoryChildren(dir, state, 'elf-only')
    const tasks: Array<Promise<void>> = []
    for (const [name, child] of children) {
        const path = prefix ? `${prefix}/${name}` : name
        if (child.kind === 'directory') {
            if (name.startsWith('.')) continue
            tasks.push(scanElfOnlyDirectory(child as FileSystemDirectoryHandle, path, depth + 1, state))
        } else if (hasElfExtension(name) && state.elfEntries.length < MAX_ELF_FILES * 8) {
            tasks.push(recordElfFile(child as FileSystemFileHandle, path, state))
        }
    }
    await Promise.all(tasks)
}

/** 扫描普通目录：源码文件全收，ELF 也收（主动目录下的 .elf 通常很少）。 */
async function scanActiveDirectory(dir: FileSystemDirectoryHandle, prefix: string, depth: number, state: CollectState): Promise<void> {
    if (depth > MAX_SCAN_DEPTH || !entryBudgetLeft(state)) return
    const children = await readDirectoryChildren(dir, state, 'active')
    const tasks: Array<Promise<void>> = []
    for (const [name, child] of children) {
        const path = prefix ? `${prefix}/${name}` : name
        if (child.kind === 'directory') {
            if (isIgnoredDirectory(name)) {
                tasks.push(scanElfOnlyDirectory(child as FileSystemDirectoryHandle, path, depth + 1, state))
            } else {
                tasks.push(scanActiveDirectory(child as FileSystemDirectoryHandle, path, depth + 1, state))
            }
            continue
        }
        if (hasElfExtension(name) && state.elfEntries.length < MAX_ELF_FILES * 8) {
            tasks.push(recordElfFile(child as FileSystemFileHandle, path, state))
        } else if (hasSourceExtension(name)) {
            tasks.push(recordSourceFile(child as FileSystemFileHandle, path, state))
        }
        // 其它扩展名连 getFile() 都不调用（构建目录里有大量无关文件）。
    }
    await Promise.all(tasks)
}

/** 组装 SourceProject（LRU 文本缓存 + 只读 ELF 访问）。 */
function buildLoadedProject(meta: SourceProjectMeta, files: Map<string, File>, options: ProjectLoadOptions): SourceProject {
    const capacity = Math.max(1, Math.floor(options.cacheSize ?? DEFAULT_SOURCE_CACHE_CAPACITY))
    const textCache = new LruCache<string, Promise<string>>(capacity)
    const getFile = (path: string): File | null => files.get(normalizeRelativePath(path)) ?? null

    const readText = (path: string): Promise<string> => {
        const key = normalizeRelativePath(path)
        const cached = textCache.get(key)
        if (cached) return cached
        const pending = (async (): Promise<string> => {
            const file = getFile(key)
            if (!file) throw new Error(`source file not found in project: ${key}`)
            if (file.size > MAX_SOURCE_FILE_BYTES) throw new Error(`source file exceeds size limit: ${key}`)
            return await file.text()
        })()
        textCache.set(key, pending)
        void pending.catch(() => {
            if (textCache.peek(key) === pending) textCache.delete(key)
        })
        return pending
    }

    return {
        ...meta,
        readText,
        readElfFile: async (path: string): Promise<File | null> => getFile(path),
        textCacheSize: (): number => textCache.size,
    }
}

// ---------------------------------------------------------------------------
// 入口 1：File System Access API（安全上下文）
// ---------------------------------------------------------------------------

export interface DirectoryProjectLoadOptions extends ProjectLoadOptions {
    rootName?: string
}

/**
 * 递归扫描用户选择的目录句柄，返回可浏览项目。
 * 用户取消选择时调用方应先用 `pickProjectDirectory()`；本函数只接受已获取的句柄。
 */
export async function createProjectFromDirectoryHandle(
    handle: FileSystemDirectoryHandle,
    options: ProjectLoadOptions = {},
): Promise<SourceProject> {
    const state = createCollectState(options)
    const availability = getDirectoryPickerAvailability()
    await scanActiveDirectory(handle as AsyncDirectoryHandle, '', 0, state)
    reportProgress(state, true)
    const meta = scanProjectEntries([...state.sourceEntries, ...state.elfEntries], {
        ...options,
        rootName: options.rootName?.trim() || handle.name,
        access: availability === 'available' ? 'fs-access' : 'provided',
    })
    return buildLoadedProject(meta, state.files, options)
}

// ---------------------------------------------------------------------------
// 入口 2：<input type="file" webkitdirectory multiple> 回退（HTTP LAN 主路径）
// ---------------------------------------------------------------------------

export interface FileListProjectLoadOptions extends ProjectLoadOptions {
    /** 没有 webkitRelativePath 时（外部直接传 File[]）使用的根名 */
    rootName?: string
}

function fileRelativePath(file: File, fallbackRootName: string): string {
    const webkitPath = typeof file.webkitRelativePath === 'string' ? file.webkitRelativePath : ''
    const path = webkitPath || file.name
    if (webkitPath) return splitWebkitRelativePath(webkitPath).relativePath
    const relative = normalizeRelativePath(path)
    if (!fallbackRootName) return relative
    const root = normalizeRelativePath(fallbackRootName)
    if (root && relative.startsWith(`${root}/`)) return relative.slice(root.length + 1)
    return relative
}

/** 从 `<input webkitdirectory>` 的 FileList 构建项目（也接受外部 File[]）。 */
export async function createProjectFromFileList(
    files: readonly File[],
    options: FileListProjectLoadOptions = {},
): Promise<SourceProject> {
    const list = [...files]
    let detectedRootName = ''
    for (const file of list) {
        const webkitPath = typeof file.webkitRelativePath === 'string' ? file.webkitRelativePath : ''
        if (!webkitPath.includes('/')) continue
        const {rootName, relativePath} = splitWebkitRelativePath(webkitPath)
        if (rootName && relativePath) {
            detectedRootName = rootName
            break
        }
    }
    const rootName = options.rootName?.trim() || detectedRootName
    const state = createCollectState(options)
    const fileMap = new Map<string, File>()
    const allEntries: RawProjectEntry[] = []
    for (const file of list) {
        const relative = fileRelativePath(file, rootName)
        if (!relative) continue
        state.scannedEntries += 1
        reportProgress(state)
        allEntries.push({path: relative, size: file.size, lastModified: file.lastModified})
        if (hasElfExtension(relative)) {
            state.elfEntries.push({path: relative, size: file.size, lastModified: file.lastModified})
            fileMap.set(relative, file)
        } else if (hasSourceExtension(relative)) {
            state.sourceEntries.push({path: relative, size: file.size, lastModified: file.lastModified})
            fileMap.set(relative, file)
        }
        // 其它文件只计入统计（scanProjectEntries 里算 unsupported），不保留 File 引用。
    }
    reportProgress(state, true)
    const meta = scanProjectEntries(allEntries, {
        ...options,
        rootName,
        access: 'input',
    })
    return buildLoadedProject(meta, fileMap, options)
}

// ---------------------------------------------------------------------------
// 便捷类型导出（组件/文档使用）
// ---------------------------------------------------------------------------

export type {ElfFileMeta, ProjectAccessKind, SourceFileMeta, SourceProjectMeta}
export {MAX_ELF_FILES, MAX_SOURCE_FILE_BYTES}
