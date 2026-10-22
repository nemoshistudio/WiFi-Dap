/**
 * SourceBrowser 的“文件:行”目标解析（task-43，纯函数，供当前 PC 行高亮/scrollToLine 使用）。
 *
 * 背景：task-42 的 `elfLineIndex` 给出精确行表（`.debug_line`），其文件记录同时带
 * `path`（规范化全路径）与 `relativePath`（去掉 comp_dir/构建根前缀，和 DebugCurve 的
 * `relativeFile` 对齐）。把 PC -> 行结果交给 SourceBrowser 时，路径可能是：
 *   - relativePath（如 `src/main.c`，与项目相对路径一致，最可靠）；
 *   - full path（如 `/work/build/../src/main.c`、`C:\proj\src\main.c`）；
 *   - 只有 basename（如 `main.c`，需要项目内后缀/文件名兜底）。
 *
 * 本模块把候选路径按 exact -> suffix -> basename 的得分匹配到项目文件，供组件:
 *   - prop `activeFileLine`（声明式 PC 行跟随）；
 *   - expose `scrollToLine(path, line)` / `highlightLine(path, line)`（命令式）。
 * 找不到文件返回 `null`（组件 expose 返回 false），不做任何猜测性打开。
 *
 * 依赖 `@/utils/sourceProject` 的路径规范化与 `matchPathToProjectFile`（read-only 复用），
 * 不修改该模块；Node 可单测（test-source-project.mjs §14）。
 */
import {
    createSourceFileIndex,
    matchPathToProjectFile,
    normalizeRelativePath,
    type SourceFileIndex,
    type SourceFileMeta,
} from '@/utils/sourceProject'

/** 组件 prop `activeFileLine` 的形态（{path,line}，可带该行可执行地址）。 */
export interface SourceActiveFileLine {
    /**
     * 源码路径：full path / relativePath / basename 均可。
     * 推荐传 task-42 `ElfLineIndexFile.relativePath`（或 `path`）——组件会按
     * exact -> suffix -> basename 匹配项目文件。
     */
    path: string
    /** 1 起始行号 */
    line: number
    /** 可选：该行在 ELF `.debug_line` 中的可执行地址（gutter 提示/断点可用） */
    address?: number
}

/** 路径匹配方式（exact 最可靠；suffix/basename 可能歧义）。 */
export type SourceFileLineMatch = 'exact' | 'suffix' | 'basename'

export interface ResolvedSourceFileLine {
    /** 匹配到的项目相对路径（规范化后的 `SourceFileMeta.path`） */
    filePath: string
    /** 1 起始行号 */
    line: number
    /** 路径匹配方式 */
    match: SourceFileLineMatch
    /** 是否有多个同分候选文件（同名文件） */
    ambiguous: boolean
    /** 匹配得分（仅用于多个候选路径之间排序） */
    score: number
    /** 目标携带的可执行地址（若存在且有效） */
    address?: number
}

/**
 * 路径候选容器：组件从 `ElfLineIndexFile` 传值时可直接展开（relativePath/path/name），
 * 也可只用 `{path, line}`。候选按 relativePath -> path -> fullPath -> name 顺序尝试。
 */
export interface SourceFileLineCandidate {
    path?: string
    relativePath?: string
    fullPath?: string
    name?: string
    line: number
    address?: number
}

function normalizeLine(value: unknown): number | null {
    if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) return null
    return Math.floor(value)
}

function normalizeAddress(value: unknown): number | undefined {
    if (typeof value !== 'number' || !Number.isFinite(value)) return undefined
    const address = value >>> 0
    return address > 0 ? address : undefined
}

function createIndex(filesOrIndex: readonly SourceFileMeta[] | SourceFileIndex): SourceFileIndex {
    if (Array.isArray(filesOrIndex)) return createSourceFileIndex(filesOrIndex as readonly SourceFileMeta[])
    return filesOrIndex as SourceFileIndex
}

/**
 * 把 `{path(s), line}` 解析为项目文件 + 行。
 *
 * - 路径先做 Windows/Unix 规范化；候选顺序 `relativePath -> path -> fullPath -> name`，
 *   同一目标多个候选时取最高得分（relativePath 精确匹配优先）；
 * - 行号必须为正整数（内部 floor）；非法行/空路径/空项目返回 null；
 * - 返回 `filePath` 为项目相对路径，供组件打开文件/滚动；`match='suffix'/'basename'`
 *   且 `ambiguous=true` 时说明项目内有重名文件（已按最短路径选择，可提示用户）。
 */
export function resolveFileLineTarget(
    target: SourceFileLineCandidate | null | undefined,
    filesOrIndex: readonly SourceFileMeta[] | SourceFileIndex | null | undefined,
): ResolvedSourceFileLine | null {
    if (!target || !filesOrIndex) return null
    const line = normalizeLine(target.line)
    if (line === null) return null
    const index = createIndex(filesOrIndex)
    if (index.files.length === 0) return null
    const rawCandidates = [target.relativePath, target.path, target.fullPath, target.name]
    const candidates: string[] = []
    for (const candidate of rawCandidates) {
        if (typeof candidate !== 'string') continue
        const trimmed = candidate.trim()
        if (trimmed && !candidates.includes(trimmed)) candidates.push(trimmed)
    }
    if (candidates.length === 0) return null
    const address = normalizeAddress(target.address)
    let best: ResolvedSourceFileLine | null = null
    for (const candidate of candidates) {
        const match = matchPathToProjectFile(candidate, index)
        if (!match) continue
        const normalized = normalizeRelativePath(candidate)
        const exact = index.byPath.has(normalized) || index.byLowerPath.has(normalized.toLowerCase())
        const kind: SourceFileLineMatch = exact ? 'exact' : match.score >= 80 ? 'suffix' : 'basename'
        const resolved: ResolvedSourceFileLine = {
            filePath: normalizeRelativePath(match.file.path) || match.file.path,
            line,
            match: kind,
            ambiguous: match.ambiguous,
            score: match.score,
            ...(address !== undefined ? {address} : {}),
        }
        if (!best || resolved.score > best.score) best = resolved
    }
    return best
}
