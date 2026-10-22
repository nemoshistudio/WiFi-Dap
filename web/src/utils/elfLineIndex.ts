/**
 * ELF 行号索引：把 `dwarfLineTable.ts` 的行号表解析接到 ELF 解析层，
 * 为 Web（精确行断点 / PC -> 源码行）提供**普通数据类型**的只读索引。
 *
 * 数据来源与流水线：
 *   ELF section table -> .debug_info/.debug_line/... -> `elfDwarf.extractDwarfLineUnits`
 *   （按 DW_AT_stmt_list 缓存每 CU 行表；附带 CU 代码范围 .debug_ranges/.debug_rnglists）
 *   -> 过滤/归一化 -> `ElfLineIndex`（rows/files/sequences 普通数组 + 查询函数）
 *
 * 设计要点：
 *  - 纯函数：不读文件、不写全局状态、不依赖 DOM；Node 可直接单测；
 *  - 地址过滤：丢掉地址 0 / 重定位 CU 的垃圾 sequence，只保留落在 ELF 可执行段
 *    （SHF_EXECINSTR）内的行；Flash 地址单独标记（H7 精确行断点必须落在 Flash）；
 *  - 行断点：`fileLineToAddress` 按 is_stmt 优先、Flash 优先返回地址；
 *  - PC -> 行：先按 CU 代码范围定位 CU，再在 CU 的行表 sequence 内二分；
 *  - 路径：Unix/Windows 规范化 + comp_dir 剥离（`relativePath` 与 DebugCurve
 *    的 `relativeFile` 对齐），查询支持 full path / 相对路径 / 后缀 / basename；
 *  - 大索引（6–8 万行）不要放进 Vue 深 reactive：用 `shallowRef`/`markRaw` 持有。
 *
 * 参考：DWARF 5 §6.2/§7.22；本项目 `dwarfLineTable.ts` / `elfDwarf.ts`。
 */
import type {ElfSectionInfo} from './elfTypes'
import {TARGET_FLASH_RANGE} from './elfTypes'
import {extractDwarfLineUnits, type PcRange} from './elfDwarf'
import {normalizeSourcePath, sourceFileBasename} from './dwarfLineTable'

export type {DwarfLineUnitInfo, PcRange} from './elfDwarf'
export type {ElfSectionInfo} from './elfTypes'

/** ELF/调试信息非法时抛出（合法 ELF 但缺 .debug_line 只返回空索引 + warning）。 */
export class ElfLineIndexError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'ElfLineIndexError'
    }
}

/** 索引中的文件表项（普通对象，路径已规范化）。 */
export interface ElfLineIndexFile {
    /** 在 index.files 中的下标 */
    id: number
    /** 所属 CU 在 extractDwarfLineUnits 中的顺序号 */
    unitIndex: number
    /** DWARF 文件表原始 name */
    name: string
    /** 规范化完整路径（comp_dir + include_dirs + name） */
    path: string
    /** 去掉 comp_dir / 公共构建根前缀后的相对路径（与 DebugCurve relativeFile 对齐） */
    relativePath: string
    /** basename（便于 UI 显示/匹配） */
    basename: string
    /** DWARF directory index（v2–v4 从 1 开始；v5 从 0 开始） */
    directoryIndex: number
    /** 所属 CU 的 DW_AT_comp_dir */
    compDir: string
}

/** 索引中的一行（只保留可执行地址内的非 end_sequence 行）。 */
export interface ElfLineIndexRow {
    address: number
    line: number
    column: number
    isStmt: boolean
    /** index.files 下标（-1 = 文件越界） */
    fileIndex: number
    unitIndex: number
    /** 该行所属 sequence 在 index.sequences 中的下标（构建完成后填充） */
    sequenceIndex: number
    /** 是否位于目标 Flash 地址范围（H7 行断点必须为 true） */
    flash: boolean
}

/** 一段地址连续的行表 sequence（已过滤垃圾行，按 lowPc 升序排序）。 */
export interface ElfLineIndexSequence {
    unitIndex: number
    /** 覆盖起点（含） */
    lowPc: number
    /** 覆盖终点（不含） */
    highPc: number
    /** 在 index.rows 中的起始下标 */
    rowStart: number
    /** 行数（>=1） */
    rowCount: number
    /** 是否整段位于 Flash */
    flash: boolean
}

/** 一个 CU 的索引信息。 */
export interface ElfLineIndexUnit {
    /** extractDwarfLineUnits 的顺序号 */
    unitIndex: number
    version: number
    addrSize: number
    compDir: string
    stmtList: number
    /** DW_AT_ranges 解出的 CU 代码范围（已规范化；可能为空） */
    ranges: PcRange[]
    /** 该 CU 的 sequence 在 index.sequences 中的下标（按 lowPc 升序） */
    sequences: number[]
}

export interface ElfLineIndexStats {
    /** .debug_info CU 总数（有 stmt_list 的） */
    units: number
    /** 成功解析出非空行表的 CU 数 */
    unitsWithLines: number
    files: number
    rows: number
    sequences: number
    /** 因地址 0/不可执行/非单调/截断被丢弃的 sequence 数 */
    droppedSequences: number
    /** 因上述原因被丢弃的行数 */
    droppedRows: number
    /** 互相重叠的 sequence 对数（健康固件应为 0；>0 时 pcToLine 取第一个） */
    overlappingSequences: number
    /** 位于 Flash 的行数 */
    flashRows: number
}

export interface ElfLineIndex {
    /** 行号表 CU（带 CU ranges），普通数组 */
    units: ElfLineIndexUnit[]
    /** 全局文件表（按规范化 path 去重） */
    files: ElfLineIndexFile[]
    /** 全局行数组（仅可执行地址内的非 end_sequence 行） */
    rows: ElfLineIndexRow[]
    /** 全局 sequence（按 lowPc 升序） */
    sequences: ElfLineIndexSequence[]
    /** ELF SHF_EXECINSTR 段地址范围 [start, end) */
    executableRanges: Array<[number, number]>
    /** 目标 Flash 范围（默认 STM32H7 0x08000000–0x09000000） */
    flashRange: [number, number]
    warnings: string[]
    stats: ElfLineIndexStats
}

/** pcToLine 的返回（普通对象）。 */
export interface ElfLineLocation {
    /** 命中的行起始地址（<= 查询 PC） */
    address: number
    line: number
    column: number
    isStmt: boolean
    file: ElfLineIndexFile | null
    unitIndex: number
    sequenceIndex: number
    /** 命中行是否在 Flash */
    flash: boolean
}

/** fileLineToAddress 的返回（普通对象）。 */
export interface ElfLineAddress {
    address: number
    line: number
    isStmt: boolean
    file: ElfLineIndexFile | null
    unitIndex: number
    rowIndex: number
    sequenceIndex: number
    /** 地址是否在 Flash（H7 精确行断点应过滤 false） */
    flash: boolean
}

export interface ElfLineIndexOptions {
    /** 覆盖默认 Flash 范围（默认 STM32H7 0x08000000–0x09000000） */
    flashRange?: readonly [number, number]
    /** 附加可执行地址范围（例如未链接 ELF 的 section 地址为 0 时手动补充） */
    extraExecutableRanges?: ReadonlyArray<readonly [number, number]>
    /** 最多保留行数（防御损坏 ELF；默认 4,000,000） */
    maxRows?: number
}

export interface ElfLineQueryOptions {
    /** 允许非 Flash 地址（默认 false：行断点只返回 Flash 地址；设 true 时非 Flash 仅降级） */
    allowNonFlash?: boolean
    /** 返回结果上限（默认 64） */
    maxResults?: number
}

export type ElfFileMatchMode = 'auto' | 'exact' | 'suffix' | 'basename'

// ---------------------------------------------------------------------------
// ELF section 表解析（ELF32/ELF64 LE，只读）
// ---------------------------------------------------------------------------
const SHF_ALLOC = 0x2
const SHF_EXECINSTR = 0x4
const SHN_XINDEX = 0xffff

export function parseElfSections(buffer: ArrayBuffer | Uint8Array): {bytes: Uint8Array; sections: ElfSectionInfo[]} {
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
    if (bytes.length < 0x40) throw new ElfLineIndexError('ELF: file is too small')
    if (bytes[0] !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46) {
        throw new ElfLineIndexError('not an ELF file')
    }
    const elfClass = bytes[4]
    if (elfClass !== 1 && elfClass !== 2) throw new ElfLineIndexError(`unsupported ELF class ${elfClass}`)
    if (bytes[5] !== 1) throw new ElfLineIndexError('only little-endian ELF is supported')
    const is64 = elfClass === 2
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
    const u16 = (offset: number): number => {
        if (offset + 2 > bytes.length) throw new ElfLineIndexError('ELF: read out of range (u16)')
        return view.getUint16(offset, true)
    }
    const u32 = (offset: number): number => {
        if (offset + 4 > bytes.length) throw new ElfLineIndexError('ELF: read out of range (u32)')
        return view.getUint32(offset, true)
    }
    const u64 = (offset: number): number => {
        if (offset + 8 > bytes.length) throw new ElfLineIndexError('ELF: read out of range (u64)')
        const value = view.getBigUint64(offset, true)
        return value > BigInt(Number.MAX_SAFE_INTEGER) ? Number.MAX_SAFE_INTEGER : Number(value)
    }
    const read = (offset: number, size: number): number => {
        if (size === 8) return u64(offset)
        if (size === 4) return u32(offset)
        if (size === 2) return u16(offset)
        throw new ElfLineIndexError(`ELF: unsupported field size ${size}`)
    }

    const eShoff = read(is64 ? 0x28 : 0x20, is64 ? 8 : 4)
    const eShentsize = is64 ? u16(0x3a) : u16(0x2e)
    let eShnum = is64 ? u16(0x3c) : u16(0x30)
    let eShstrndx = is64 ? u16(0x3e) : u16(0x32)
    if (eShentsize < (is64 ? 64 : 40)) throw new ElfLineIndexError(`ELF: invalid section header size ${eShentsize}`)
    if (eShoff <= 0 || eShoff >= bytes.length) throw new ElfLineIndexError('ELF: invalid section header offset')

    const readHeader = (index: number): ElfSectionInfo => {
        const base = eShoff + index * eShentsize
        if (base + eShentsize > bytes.length) throw new ElfLineIndexError(`ELF: section header ${index} is out of range`)
        return {
            index,
            name: '',
            type: u32(base + 4),
            flags: read(base + 8, is64 ? 8 : 4),
            address: read(is64 ? base + 0x10 : base + 0x0c, is64 ? 8 : 4),
            offset: read(is64 ? base + 0x18 : base + 0x10, is64 ? 8 : 4),
            size: read(is64 ? base + 0x20 : base + 0x14, is64 ? 8 : 4),
            link: u32(is64 ? base + 0x28 : base + 0x18),
            entsize: read(is64 ? base + 0x38 : base + 0x24, is64 ? 8 : 4),
        }
    }
    // 扩展 section 计数/字符串表索引（ELF 大文件时 e_shnum==0 / e_shstrndx==SHN_XINDEX）
    if (eShnum === 0 || eShstrndx === SHN_XINDEX) {
        const first = readHeader(0)
        if (eShnum === 0) eShnum = first.size
        if (eShstrndx === SHN_XINDEX) eShstrndx = first.link
    }
    if (eShnum <= 0 || eShnum > 100000) throw new ElfLineIndexError(`ELF: invalid section count ${eShnum}`)
    if (eShstrndx < 0 || eShstrndx >= eShnum) throw new ElfLineIndexError(`ELF: invalid section name table index ${eShstrndx}`)
    const sections: ElfSectionInfo[] = []
    for (let index = 0; index < eShnum; index++) sections.push(readHeader(index))
    const stringTable = sections[eShstrndx]
    if (stringTable.offset + stringTable.size > bytes.length) throw new ElfLineIndexError('ELF: section name table is out of range')
    for (const section of sections) {
        const base = eShoff + section.index * eShentsize
        const nameOffset = u32(base)
        if (nameOffset >= stringTable.size) {
            section.name = ''
            continue
        }
        const start = stringTable.offset + nameOffset
        let end = start
        while (end < stringTable.offset + stringTable.size && bytes[end] !== 0) end++
        section.name = decodeUtf8(bytes.subarray(start, end))
    }
    return {bytes, sections}
}

function decodeUtf8(bytes: Uint8Array): string {
    try {
        return new TextDecoder('utf-8', {fatal: false}).decode(bytes)
    } catch {
        let text = ''
        for (const byte of bytes) text += String.fromCharCode(byte)
        return text
    }
}

// ---------------------------------------------------------------------------
// 索引构建
// ---------------------------------------------------------------------------

function normalizeRanges(ranges: ReadonlyArray<readonly [number, number]>): Array<[number, number]> {
    const result: Array<[number, number]> = []
    for (const range of ranges) {
        const start = range[0] >>> 0
        const end = range[1] >>> 0
        if (end > start) result.push([start, end])
    }
    result.sort((left, right) => left[0] - right[0])
    return result
}

function isInRanges(ranges: ReadonlyArray<readonly [number, number]>, address: number): boolean {
    for (const [start, end] of ranges) {
        if (address >= start && address < end) return true
    }
    return false
}

function isPcInRanges(ranges: readonly PcRange[], address: number): boolean {
    for (const range of ranges) {
        if (address >= range.pcLow && address < range.pcHigh) return true
    }
    return false
}

/** 计算一组绝对路径的公共目录前缀（用于去掉 /work 之类构建根）。 */
function commonDirectoryPrefix(paths: readonly string[]): string {
    if (paths.length === 0) return ''
    const split = paths.map((path) => {
        const parts = path.split('/')
        if (parts[0] === '') parts.shift()
        return parts
    })
    const first = split[0]
    let count = 0
    for (let index = 0; index < first.length - 1; index++) {
        const segment = first[index]
        if (!segment) break
        if (split.every((parts) => parts.length > index + 1 && parts[index] === segment)) count = index + 1
        else break
    }
    return first.slice(0, count).join('/')
}

/**
 * 从 DWARF 行表构建 Web 可用的普通类型行号索引。
 *
 * @param bytes   整个 ELF 字节（用于取 DWARF sections，不修改）
 * @param sections `parseElfSections()` 得到的 section 表
 * @throws ElfLineIndexError only for invalid arguments（缺 .debug_info 属于容错路径，返回空索引 + warnings）
 */
export function buildElfLineIndex(
    bytes: Uint8Array,
    sections: readonly ElfSectionInfo[],
    options: ElfLineIndexOptions = {},
): ElfLineIndex {
    if (!(bytes instanceof Uint8Array)) throw new TypeError('bytes must be a Uint8Array')
    if (!Array.isArray(sections)) throw new TypeError('sections must be an array')
    const maxRows = options.maxRows ?? 4_000_000
    const flashRange: [number, number] = options.flashRange
        ? [options.flashRange[0] >>> 0, options.flashRange[1] >>> 0]
        : [TARGET_FLASH_RANGE[0] >>> 0, TARGET_FLASH_RANGE[1] >>> 0]
    const executableRanges = normalizeRanges([
        ...sections
            .filter((section) => (section.flags & SHF_EXECINSTR) !== 0
                && (section.flags & SHF_ALLOC) !== 0
                && section.address > 0
                && section.size > 0)
            .map((section) => [section.address >>> 0, (section.address + section.size) >>> 0] as [number, number]),
        ...(options.extraExecutableRanges ?? []).map((range) => [range[0] >>> 0, range[1] >>> 0] as [number, number]),
    ])
    const warnings: string[] = []
    const stats: ElfLineIndexStats = {
        units: 0,
        unitsWithLines: 0,
        files: 0,
        rows: 0,
        sequences: 0,
        droppedSequences: 0,
        droppedRows: 0,
        overlappingSequences: 0,
        flashRows: 0,
    }
    const empty: ElfLineIndex = {
        units: [],
        files: [],
        rows: [],
        sequences: [],
        executableRanges,
        flashRange,
        warnings,
        stats,
    }
    if (executableRanges.length === 0) {
        warnings.push('ELF line index: no executable sections (SHF_EXECINSTR) found; line breakpoints unavailable')
    }
    const extracted = extractDwarfLineUnits(bytes, sections)
    warnings.push(...extracted.warnings)
    stats.units = extracted.units.length
    if (extracted.units.length === 0) return empty

    // ---- 文件表（按规范化 path 去重；relativePath 两遍计算：先去 comp_dir 再公共前缀） ----
    const files: ElfLineIndexFile[] = []
    const fileIdByPath = new Map<string, number>()
    const unitFileIds: number[][] = [] // 每个 unit 的 DWARF file index -> 全局 file id
    const compDirs: string[] = []
    for (const unit of extracted.units) {
        compDirs.push(unit.compDir ?? '')
        const mapping: number[] = []
        unitFileIds.push(mapping)
        const table = unit.lineTable
        if (!table) {
            unitFileIds[unitFileIds.length - 1] = []
            continue
        }
        for (const file of table.files) {
            const path = normalizeSourcePath(file.path)
            let id = fileIdByPath.get(path)
            if (id === undefined) {
                id = files.length
                fileIdByPath.set(path, id)
                files.push({
                    id,
                    unitIndex: unit.index,
                    name: file.name,
                    path,
                    relativePath: path,
                    basename: sourceFileBasename(path),
                    directoryIndex: file.directoryIndex,
                    compDir: unit.compDir ?? '',
                })
            }
            mapping.push(id)
        }
    }
    // 第二遍：先按 CU comp_dir 剥前缀；仍为绝对路径时优先用 DWARF 文件表里的相对 name
    //（clang 的 comp_dir 可能是 /work/build 而文件表目录是 /work，此时 name 才是工程相对路径），
    // 最后再尝试“公共构建根”剥离。
    const unresolved: ElfLineIndexFile[] = []
    for (const file of files) {
        const normalizedCompDir = file.compDir ? normalizeSourcePath(file.compDir) : ''
        if (normalizedCompDir && (file.path === normalizedCompDir || file.path.startsWith(`${normalizedCompDir}/`))) {
            file.relativePath = file.path.slice(normalizedCompDir.length).replace(/^\/+/, '')
            continue
        }
        const name = normalizeSourcePath(file.name)
        if (name && !name.startsWith('/') && !name.startsWith('../') && name.includes('/')) {
            file.relativePath = name
            continue
        }
        if (/^[A-Za-z]:\//.test(file.path) || file.path.startsWith('/')) unresolved.push(file)
        else file.relativePath = file.path
    }
    if (unresolved.length > 0) {
        const commonPrefix = commonDirectoryPrefix(unresolved.map((file) => file.path))
        for (const file of unresolved) {
            const relative = childPathAfter(file.path, commonPrefix)
            file.relativePath = relative || file.path
        }
    }
    stats.files = files.length

    // ---- 行/sequence（只保留可执行段内、地址>0、单调的 sequence） ----
    const rows: ElfLineIndexRow[] = []
    const sequences: ElfLineIndexSequence[] = []
    const unitSequences: number[][] = extracted.units.map(() => [])
    const sequenceCandidates: ElfLineIndexSequence[] = []

    for (const [unitArrayIndex, unit] of extracted.units.entries()) {
        const table = unit.lineTable
        if (!table || table.sequences.length === 0) continue
        let keptSequencesInUnit = 0
        const mapping = unitFileIds[unitArrayIndex] ?? []
        for (const sequence of table.sequences) {
            if (rows.length >= maxRows) {
                if (!warnings.some((warning) => warning.includes('row limit'))) {
                    warnings.push(`ELF line index: row limit ${maxRows} reached; remaining line tables skipped`)
                }
                break
            }
            if (!sequence.terminated || !sequence.monotonic || sequence.endRow <= sequence.startRow) {
                stats.droppedSequences++
                continue
            }
            const startRowIndex = sequence.startRow
            const endRowIndex = sequence.endRow - 1 // 排除 endSequence 行
            const sequenceRows: Array<{address: number; line: number; column: number; isStmt: boolean; fileIndex: number}> = []
            let valid = true
            for (let index = startRowIndex; index <= endRowIndex; index++) {
                const row = table.rows[index]
                const address = row.address >>> 0
                if (address === 0 || !isInRanges(executableRanges, address)) {
                    valid = false
                    break
                }
                const dwarfFileIndex = table.version >= 5 ? row.fileIndex : row.fileIndex - 1
                const globalFileId = dwarfFileIndex >= 0 && dwarfFileIndex < mapping.length ? mapping[dwarfFileIndex] : -1
                sequenceRows.push({
                    address,
                    line: row.line,
                    column: row.column,
                    isStmt: row.isStmt,
                    fileIndex: globalFileId,
                })
            }
            if (!valid || sequenceRows.length === 0) {
                stats.droppedSequences++
                stats.droppedRows += Math.max(0, endRowIndex - startRowIndex + 1)
                continue
            }
            const lowPc = sequenceRows[0].address
            const highPc = Math.max(lowPc + 1, sequence.highPc >>> 0)
            const rowStart = rows.length
            for (const row of sequenceRows) {
                rows.push({
                    address: row.address,
                    line: row.line,
                    column: row.column,
                    isStmt: row.isStmt,
                    fileIndex: row.fileIndex,
                    unitIndex: unit.index,
                    sequenceIndex: -1,
                    flash: row.address >= flashRange[0] && row.address < flashRange[1],
                })
            }
            stats.flashRows += sequenceRows.filter((row) => row.address >= flashRange[0] && row.address < flashRange[1]).length
            sequenceCandidates.push({
                unitIndex: unit.index,
                lowPc,
                highPc,
                rowStart,
                rowCount: sequenceRows.length,
                flash: lowPc >= flashRange[0] && highPc <= flashRange[1],
            })
            keptSequencesInUnit++
        }
        if (keptSequencesInUnit > 0) stats.unitsWithLines++
    }

    // 全局 sequence 按 lowPc 排序；同时维护每个 CU 的 sequence 下标。
    sequenceCandidates.sort((left, right) => left.lowPc - right.lowPc)
    const units: ElfLineIndexUnit[] = extracted.units.map((unit, unitArrayIndex) => ({
        unitIndex: unit.index,
        version: unit.version,
        addrSize: unit.addrSize,
        compDir: unit.compDir ?? '',
        stmtList: unit.stmtList,
        ranges: normalizeRanges(unit.ranges.map((range) => [range.pcLow, range.pcHigh] as [number, number]))
            .map(([pcLow, pcHigh]) => ({pcLow, pcHigh})),
        sequences: unitSequences[unitArrayIndex],
    }))
    const unitByIndex = new Map<number, ElfLineIndexUnit>()
    for (const unit of units) unitByIndex.set(unit.unitIndex, unit)
    for (const [sequenceIndex, sequence] of sequenceCandidates.entries()) {
        sequences.push(sequence)
        for (let offset = 0; offset < sequence.rowCount; offset++) {
            const row = rows[sequence.rowStart + offset]
            if (row) row.sequenceIndex = sequenceIndex
        }
        unitByIndex.get(sequence.unitIndex)?.sequences.push(sequenceIndex)
    }
    stats.sequences = sequences.length
    stats.rows = rows.length
    for (const unit of units) unit.sequences.sort((left, right) => sequences[left].lowPc - sequences[right].lowPc)

    // 重叠检测（健康链接结果应当为 0；>0 时 pcToLine 取第一个匹配 sequence）
    for (let index = 1; index < sequences.length; index++) {
        let left = index - 1
        while (left >= 0 && sequences[left].lowPc < sequences[index].highPc) {
            if (sequences[left].highPc > sequences[index].lowPc) stats.overlappingSequences++
            left--
            if (index - left > 64) break
        }
    }
    if (stats.overlappingSequences > 0) {
        warnings.push(`ELF line index: ${stats.overlappingSequences} overlapping line sequences (address 0/relocated CUs filtered)`)
    }
    stats.files = files.length

    return {
        units,
        files,
        rows,
        sequences,
        executableRanges,
        flashRange,
        warnings,
        stats,
    }
}

function childPathAfter(path: string, prefix: string): string {
    const normalized = normalizeSourcePath(path)
    const normalizedPrefix = normalizeSourcePath(prefix)
    if (!normalizedPrefix) return normalized
    if (normalized === normalizedPrefix) return ''
    if (normalized.startsWith(`${normalizedPrefix}/`)) return normalized.slice(normalizedPrefix.length + 1)
    return normalized
}

// ---------------------------------------------------------------------------
// 对外入口
// ---------------------------------------------------------------------------

/**
 * 解析 ELF 字节并构建行号索引。
 * @throws ElfLineIndexError 非 ELF/结构损坏；合法 ELF 缺 DWARF 行表时返回空索引 + warnings。
 */
export function parseElfLineIndex(
    buffer: ArrayBuffer | Uint8Array,
    options: ElfLineIndexOptions = {},
): ElfLineIndex {
    const {bytes, sections} = parseElfSections(buffer)
    return buildElfLineIndex(bytes, sections, options)
}

// ---------------------------------------------------------------------------
// 查询函数（纯函数，输入 ElfLineIndex 普通对象）
// ---------------------------------------------------------------------------

function isExecutableAddress(index: ElfLineIndex, address: number): boolean {
    return isInRanges(index.executableRanges, address)
}

function isFlashAddress(index: ElfLineIndex, address: number): boolean {
    return address >= index.flashRange[0] && address < index.flashRange[1]
}

function findRowInSequence(index: ElfLineIndex, sequenceIndex: number, pc: number): number {
    const sequence = index.sequences[sequenceIndex]
    let low = sequence.rowStart
    let high = sequence.rowStart + sequence.rowCount - 1
    let candidate = -1
    while (low <= high) {
        const middle = (low + high) >> 1
        if (index.rows[middle].address <= pc) {
            candidate = middle
            low = middle + 1
        } else {
            high = middle - 1
        }
    }
    return candidate >= 0 ? candidate : sequence.rowStart
}

function findByUnitRanges(index: ElfLineIndex, pc: number, sequenceIndices: readonly number[]): number | null {
    if (sequenceIndices.length === 0) return null
    let low = 0
    let high = sequenceIndices.length - 1
    let candidate = -1
    while (low <= high) {
        const middle = (low + high) >> 1
        if (index.sequences[sequenceIndices[middle]].lowPc <= pc) {
            candidate = middle
            low = middle + 1
        } else {
            high = middle - 1
        }
    }
    for (let offset = 0; candidate - offset >= 0 && offset < 16; offset++) {
        const sequenceIndex = sequenceIndices[candidate - offset]
        const sequence = index.sequences[sequenceIndex]
        if (pc >= sequence.lowPc && pc < sequence.highPc) return sequenceIndex
    }
    return null
}

/**
 * PC -> 源码行（精确）。
 *  - 先校验 PC 落在 ELF 可执行段（默认还要求 Flash，`allowNonFlash` 可放开）；
 *  - 优先用 CU 代码范围（DW_AT_ranges）定位 CU，再在该 CU 的行表 sequence 内查找；
 *  - CU ranges 缺失/不匹配时回退到全局 sequence 二分。
 */
export function pcToLine(
    index: ElfLineIndex,
    pc: number,
    options: ElfLineQueryOptions = {},
): ElfLineLocation | null {
    const address = pc >>> 0
    if (!isExecutableAddress(index, address)) return null
    if (!options.allowNonFlash && !isFlashAddress(index, address)) return null

    // 1) CU ranges 定位
    for (const unit of index.units) {
        if (unit.ranges.length === 0) continue
        if (!isPcInRanges(unit.ranges, address)) continue
        const sequenceIndex = findByUnitRanges(index, address, unit.sequences)
        if (sequenceIndex === null) continue
        const rowIndex = findRowInSequence(index, sequenceIndex, address)
        const row = index.rows[rowIndex]
        return {
            address: row.address,
            line: row.line,
            column: row.column,
            isStmt: row.isStmt,
            file: row.fileIndex >= 0 ? (index.files[row.fileIndex] ?? null) : null,
            unitIndex: row.unitIndex,
            sequenceIndex,
            flash: row.flash,
        }
    }

    // 2) 回退：全局 sequence 二分（重叠时向左回溯少量候选）
    let low = 0
    let high = index.sequences.length - 1
    let candidate = -1
    while (low <= high) {
        const middle = (low + high) >> 1
        if (index.sequences[middle].lowPc <= address) {
            candidate = middle
            low = middle + 1
        } else {
            high = middle - 1
        }
    }
    for (let offset = 0; candidate - offset >= 0 && offset < 32; offset++) {
        const sequenceIndex = candidate - offset
        const sequence = index.sequences[sequenceIndex]
        if (address < sequence.lowPc || address >= sequence.highPc) continue
        const rowIndex = findRowInSequence(index, sequenceIndex, address)
        const row = index.rows[rowIndex]
        return {
            address: row.address,
            line: row.line,
            column: row.column,
            isStmt: row.isStmt,
            file: row.fileIndex >= 0 ? (index.files[row.fileIndex] ?? null) : null,
            unitIndex: row.unitIndex,
            sequenceIndex,
            flash: row.flash,
        }
    }
    return null
}

/** pcToLine 的别名，便于页面按“PC -> 源码行”语义调用。 */
export function pcToFileLine(
    index: ElfLineIndex,
    pc: number,
    options: ElfLineQueryOptions = {},
): ElfLineLocation | null {
    return pcToLine(index, pc, options)
}

/** 路径匹配候选键（规范化全路径 / 相对路径 / 原 name / basename）。 */
function fileMatchKeys(file: ElfLineIndexFile): string[] {
    const keys = new Set<string>()
    keys.add(normalizeSourcePath(file.path))
    keys.add(normalizeSourcePath(file.relativePath))
    keys.add(normalizeSourcePath(file.name))
    return [...keys]
}

/**
 * 按源码路径解析文件记录（与 DebugCurve relativeFile 对齐）：
 *  auto：规范化全路径/相对路径相等 -> 路径后缀 `.../query` -> basename。
 */
export function findElfLineFiles(
    index: ElfLineIndex,
    pathOrQuery: string,
    mode: ElfFileMatchMode = 'auto',
): ElfLineIndexFile[] {
    const query = normalizeSourcePath(pathOrQuery)
    if (!query) return []
    const exact: ElfLineIndexFile[] = []
    const suffix: ElfLineIndexFile[] = []
    const basenames: ElfLineIndexFile[] = []
    const basename = sourceFileBasename(query)
    for (const file of index.files) {
        const keys = fileMatchKeys(file)
        if (mode === 'exact' || mode === 'auto') {
            if (keys.some((key) => key === query)) exact.push(file)
        }
        if (mode === 'suffix' || mode === 'auto') {
            if (keys.some((key) => key.endsWith(`/${query}`))) suffix.push(file)
        }
        if (mode === 'basename' || mode === 'auto') {
            if (file.basename === basename) basenames.push(file)
        }
    }
    if (mode === 'exact') return exact
    if (mode === 'suffix') return suffix
    if (mode === 'basename') return basenames
    if (exact.length > 0) return exact
    if (suffix.length > 0) return suffix
    return basenames
}

/**
 * file:line -> 所有候选地址（行断点用）。
 * 排序：is_stmt 优先 -> Flash 优先 -> 地址升序；默认过滤非 Flash 地址。
 * 实现为全表线性扫描（6–8 万行 <1ms，行断点/源码点击不是高频路径），保持纯函数。
 */
/**
 * file:line -> 所有候选地址（行断点用）。
 * 排序：is_stmt 优先 -> Flash 优先 -> 地址升序；默认过滤非 Flash 地址。
 */
export function fileLineToAddresses(
    index: ElfLineIndex,
    pathOrFile: string | ElfLineIndexFile,
    line: number,
    options: ElfLineQueryOptions = {},
): ElfLineAddress[] {
    if (!Number.isInteger(line)) return []
    const targets = typeof pathOrFile === 'string' ? findElfLineFiles(index, pathOrFile) : [pathOrFile]
    if (targets.length === 0) return []
    const maxResults = options.maxResults ?? 64
    const targetIds = new Set(targets.map((file) => file.id))
    const results: ElfLineAddress[] = []
    for (let rowIndex = 0; rowIndex < index.rows.length; rowIndex++) {
        const row = index.rows[rowIndex]
        if (row.line !== line || !targetIds.has(row.fileIndex)) continue
        if (!options.allowNonFlash && !row.flash) continue
        const file = index.files[row.fileIndex] ?? null
        results.push({
            address: row.address,
            line: row.line,
            isStmt: row.isStmt,
            file,
            unitIndex: row.unitIndex,
            rowIndex,
            sequenceIndex: row.sequenceIndex,
            flash: row.flash,
        })
        if (results.length >= maxResults * 4) break
    }
    results.sort((left, right) => {
        if (left.isStmt !== right.isStmt) return left.isStmt ? -1 : 1
        if (left.flash !== right.flash) return left.flash ? -1 : 1
        if (left.address !== right.address) return left.address - right.address
        return left.rowIndex - right.rowIndex
    })
    return results.slice(0, maxResults)
}


/** file:line -> 最佳行断点地址（is_stmt 优先、Flash 优先）；无候选返回 null。 */
export function fileLineToAddress(
    index: ElfLineIndex,
    pathOrFile: string | ElfLineIndexFile,
    line: number,
    options: ElfLineQueryOptions = {},
): ElfLineAddress | null {
    return fileLineToAddresses(index, pathOrFile, line, options)[0] ?? null
}

/** 便捷：按 basename/suffix 解析出文件（多匹配返回第一个）。 */
export function findElfLineFile(
    index: ElfLineIndex,
    query: string,
    mode: ElfFileMatchMode = 'auto',
): ElfLineIndexFile | null {
    return findElfLineFiles(index, query, mode)[0] ?? null
}
