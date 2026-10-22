/**
 * DWARF `.debug_line` 行号表（line number program）独立解析模块。
 *
 * 目标：为「精确行断点 / PC -> 源码行 / file:line -> 地址」提供纯函数实现，
 * 支持 DWARF v2 / v3 / v4 / v5：
 *  - 头部：unit_length(DWARF32/64)、version、address_size(v5)、segment_selector_size(v5)、
 *    header_length、min_inst_length、max_ops_per_inst(v4+)、default_is_stmt、
 *    line_base、line_range、opcode_base、standard_opcode_lengths；
 *  - 目录表 / 文件表：v2–v4 的 NUL 结尾字符串表；v5 的
 *    directory_entry_format / file_name_entry_format + DW_LNCT_* + form
 *    （string / line_strp / strp / strx* / data16(MD5) / data* / udata…）；
 *  - 行号程序：standard opcodes（copy/advance_pc/advance_line/set_file/set_column/
 *    negate_stmt/set_basic_block/const_add_pc/fixed_advance_pc/set_prologue_end/
 *    set_epilogue_begin/set_isa）、special opcodes（含 v4 max_ops_per_inst 的
 *    op_index 语义）、extended opcodes（end_sequence/set_address/define_file/
 *    set_discriminator，未知扩展 opcode 跳过）；
 *  - 路径：comp_dir + include_directories + file name 拼接与 Unix/Windows 规范化；
 *  - 查询：addressToLine / findLineTableFile / resolveLineTableFiles /
 *    fileLineToAddresses / findFilesByPathPrefix / buildFileLineIndex。
 *
 * 设计约束（与 task 要求一致）：
 *  - 纯 TS、无 DOM、不 import 项目内其它模块，Node 可直接打包单测；
 *  - 输入所有 section 字节与 CU 上下文，不读文件；同一输入必得同一输出；
 *  - 头部结构非法抛 LineTableError；行号程序数据损坏只记 warnings 并返回已解析部分；
 *  - 表达式/地址只做行表语义换算，不依赖现有 elfDwarf/elfTypes。
 *
 * 参考：DWARF 5 §6.2（Line Number Tables）、§7.22（.debug_line）；
 * GNU binutils dwarf.c 的 read_debug_line_header / display_debug_lines_decoded；
 * LLVM DWARFDebugLine.cpp（form 读取、op_index、sequence 语义）。
 */

// ---------------------------------------------------------------------------
// DWARF 常量
// ---------------------------------------------------------------------------
/** DWARF5 line table 目录/文件表字段类型（DW_LNCT_*）。 */
export const DW_LNCT_PATH = 0x01
export const DW_LNCT_DIRECTORY_INDEX = 0x02
export const DW_LNCT_TIMESTAMP = 0x03
export const DW_LNCT_SIZE = 0x04
export const DW_LNCT_MD5 = 0x05

/** 标准行号程序 opcode（DW_LNS_*）。 */
export const DW_LNS_COPY = 0x01
export const DW_LNS_ADVANCE_PC = 0x02
export const DW_LNS_ADVANCE_LINE = 0x03
export const DW_LNS_SET_FILE = 0x04
export const DW_LNS_SET_COLUMN = 0x05
export const DW_LNS_NEGATE_STMT = 0x06
export const DW_LNS_SET_BASIC_BLOCK = 0x07
export const DW_LNS_CONST_ADD_PC = 0x08
export const DW_LNS_FIXED_ADVANCE_PC = 0x09
export const DW_LNS_SET_PROLOGUE_END = 0x0a
export const DW_LNS_SET_EPILOGUE_BEGIN = 0x0b
export const DW_LNS_SET_ISA = 0x0c

/** 扩展行号程序 opcode（DW_LNE_*）。 */
export const DW_LNE_END_SEQUENCE = 0x01
export const DW_LNE_SET_ADDRESS = 0x02
export const DW_LNE_DEFINE_FILE = 0x03
export const DW_LNE_SET_DISCRIMINATOR = 0x04

// 解析 v5 目录/文件表用到的 DWARF form（只用于 line table header）。
const FORM_ADDR = 0x01
const FORM_BLOCK2 = 0x03
const FORM_BLOCK4 = 0x04
const FORM_DATA2 = 0x05
const FORM_DATA4 = 0x06
const FORM_DATA8 = 0x07
const FORM_STRING = 0x08
const FORM_BLOCK = 0x09
const FORM_BLOCK1 = 0x0a
const FORM_DATA1 = 0x0b
const FORM_FLAG = 0x0c
const FORM_SDATA = 0x0d
const FORM_STRP = 0x0e
const FORM_UDATA = 0x0f
const FORM_SEC_OFFSET = 0x17
const FORM_EXPRLOC = 0x18
const FORM_FLAG_PRESENT = 0x19
const FORM_STRX = 0x1a
const FORM_ADDRX = 0x1b
const FORM_DATA16 = 0x1e
const FORM_LINE_STRP = 0x1f
const FORM_IMPLICIT_CONST = 0x21
const FORM_STRX1 = 0x25
const FORM_STRX2 = 0x26
const FORM_STRX3 = 0x27
const FORM_STRX4 = 0x28
const FORM_ADDRX1 = 0x29
const FORM_ADDRX2 = 0x2a
const FORM_ADDRX3 = 0x2b
const FORM_ADDRX4 = 0x2c

const DEFAULT_MAX_ROWS = 2_000_000
const MAX_WARNINGS = 64
const MAX_ULEB_SHIFT = 56

// ---------------------------------------------------------------------------
// 公共类型
// ---------------------------------------------------------------------------

/** 行表文件表项。`path`/`fullPath` 为按 comp_dir + include_dirs 拼接后的路径。 */
export interface LineTableFile {
    /** 在文件表中的 0-based 下标。 */
    index: number
    /** 行号程序 state machine 的 file 寄存器取值（v2–v4 从 1 开始；v5 从 0 开始）。 */
    dwarfIndex: number
    /** 文件表原始 name 字段。 */
    name: string
    /** 原始目录下标（v2–v4 从 1 开始且 0 表示 comp_dir；v5 从 0 开始）。 */
    directoryIndex: number
    /** 解析后的目录（未拼接 comp_dir）。 */
    directory: string
    /** comp_dir + directory + name 规范化后的完整路径。 */
    path: string
    /** 同 path（便于与其它模块字段对齐）。 */
    fullPath: string
    /** 修改时间（v2–v4 为 ULEB；v5 DW_LNCT_timestamp，未知为 0）。 */
    mtime: number
    /** 文件大小（v5 DW_LNCT_size，未知为 0）。 */
    size: number
    /** v5 DW_LNCT_MD5（16 字节）。 */
    md5?: Uint8Array
}

/** 行号表矩阵中的一行（DWARF line table row）。 */
export interface LineRow {
    /** 该行起始地址（v4 max_ops_per_inst>1 时同一地址可有多个 op_index）。 */
    address: number
    /** v4 max_ops_per_inst 的指令内操作下标。 */
    opIndex: number
    /** state machine 的 file 寄存器原始值。 */
    fileIndex: number
    /** 解析到的文件表项（越界为 null）。 */
    file: LineTableFile | null
    /** 行号（endSequence 行无意义，但仍保留状态值）。 */
    line: number
    column: number
    isStmt: boolean
    basicBlock: boolean
    prologueEnd: boolean
    epilogueBegin: boolean
    isa: number
    discriminator: number
    /** 是否为序列结束行（该行的 address 是序列结束地址，不含在覆盖范围内）。 */
    endSequence: boolean
    /** 所属 sequence 下标。 */
    sequence: number
    /** 在 table.rows 中的下标。 */
    rowIndex: number
}

/** 一段地址连续的行号矩阵序列（end_sequence 结束）。 */
export interface LineSequence {
    startRow: number
    /** 最后一个矩阵行（含 endSequence 行）在 table.rows 中的下标。 */
    endRow: number
    /** 序列覆盖的起始地址（含）。 */
    lowPc: number
    /** 序列覆盖的结束地址（不含，= endSequence 行地址）。 */
    highPc: number
    /** 是否遇到 DW_LNE_end_sequence；false 表示 EOF/损坏截断。 */
    terminated: boolean
    /** 序列内矩阵行地址是否单调不减（false 时地址查询退化为线性扫描）。 */
    monotonic: boolean
}

export interface LineTable {
    /** .debug_line 中的贡献偏移。 */
    offset: number
    /** 贡献结束偏移（不含）。 */
    endOffset: number
    version: number
    addressSize: number
    segmentSelectorSize: number
    /** 32-bit / 64-bit DWARF unit_length 格式。 */
    dwarf64: boolean
    headerLength: number
    minInstructionLength: number
    maxOpsPerInstruction: number
    defaultIsStmt: boolean
    lineBase: number
    lineRange: number
    opcodeBase: number
    /** opcode_base-1 项，每项为该标准 opcode 的 ULEB 操作数个数。 */
    standardOpcodeLengths: number[]
    /** 解析后的 include_directories（已尽量拼接 comp_dir）。 */
    directories: string[]
    files: LineTableFile[]
    rows: LineRow[]
    sequences: LineSequence[]
    warnings: string[]
}

export interface ParseLineTableOptions {
    /** `.debug_line` 节完整字节。 */
    data: Uint8Array
    /** contribution 起始偏移（DIE 的 DW_AT_stmt_list）。默认 0。 */
    offset?: number
    /** `.debug_line_str`（v5 DW_FORM_line_strp）。 */
    lineStr?: Uint8Array | null
    /** `.debug_str`（DW_FORM_strp / strx 间接字符串）。 */
    str?: Uint8Array | null
    /** `.debug_str_offsets`（DW_FORM_strx*）。 */
    strOffsets?: Uint8Array | null
    /** CU 的 DW_AT_str_offsets_base（指向第一条 str_offsets 条目）。 */
    strOffsetsBase?: number
    /** v2–v4 的 CU address_size（v5 从 line header 读取）。默认 4。 */
    addressSize?: number
    /** CU 的 DW_AT_comp_dir（用于路径拼接）。 */
    compDir?: string
    /** 防御上限：矩阵行数，默认 2,000,000。 */
    maxRows?: number
}

/** addressToLine 的查询结果。 */
export interface LineHit {
    /** 命中行的起始地址（<= 查询 pc）。 */
    address: number
    file: LineTableFile | null
    line: number
    column: number
    isStmt: boolean
    row: LineRow
}

/** file:line -> 地址查询结果。 */
export interface LineAddress {
    address: number
    opIndex: number
    rowIndex: number
    isStmt: boolean
    /** 行号程序中的 file 寄存器值。 */
    fileIndex: number
}

/** file:line 索引（key 为规范化路径，value 为 line -> rows 下标）。 */
export type FileLineIndex = Map<string, Map<number, number[]>>

/** 文件路径匹配模式。 */
export type FileMatchMode = 'auto' | 'exact' | 'suffix' | 'basename'

/** line table 头部结构非法时抛出。 */
export class LineTableError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'LineTableError'
    }
}

// ---------------------------------------------------------------------------
// 字节读取 / 基础工具
// ---------------------------------------------------------------------------
class LineTruncated extends Error {}

class LineReader {
    pos: number
    readonly end: number
    readonly bytes: Uint8Array

    constructor(bytes: Uint8Array, pos: number, end = bytes.length) {
        this.bytes = bytes
        this.pos = pos
        this.end = Math.min(end, bytes.length)
    }

    get remaining(): number {
        return this.end - this.pos
    }

    u8(): number {
        if (this.pos + 1 > this.end) throw new LineTruncated('unexpected end of data (u8)')
        return this.bytes[this.pos++]
    }

    u16(): number {
        return this.unsigned(2)
    }

    u32(): number {
        return this.unsigned(4)
    }

    unsigned(size: number): number {
        if (size < 1 || size > 8) throw new LineTableError(`invalid integer size ${size}`)
        if (this.pos + size > this.end) {
            throw new LineTruncated(`unexpected end of data (need ${size} bytes at 0x${this.pos.toString(16)})`)
        }
        const value = readUnsignedLE(this.bytes, this.pos, size)
        this.pos += size
        return value
    }

    u64(): number {
        return this.unsigned(8)
    }

    uleb(): number {
        let result = 0
        let shift = 0
        for (;;) {
            const byte = this.u8()
            result += (byte & 0x7f) * Math.pow(2, shift)
            shift += 7
            if ((byte & 0x80) === 0) return result
            if (shift > MAX_ULEB_SHIFT) throw new LineTableError('ULEB128 value is too large')
        }
    }

    sleb(): number {
        let result = 0
        let shift = 0
        let byte = 0
        for (;;) {
            byte = this.u8()
            result += (byte & 0x7f) * Math.pow(2, shift)
            shift += 7
            if ((byte & 0x80) === 0) break
            if (shift > MAX_ULEB_SHIFT) throw new LineTableError('SLEB128 value is too large')
        }
        if (shift < 64 && (byte & 0x40) !== 0) result -= Math.pow(2, shift)
        return result
    }

    cstring(): string {
        const start = this.pos
        while (this.pos < this.end && this.bytes[this.pos] !== 0) this.pos++
        const text = decodeUtf8(this.bytes.subarray(start, this.pos))
        if (this.pos < this.end) this.pos++ // NUL
        return text
    }

    take(size: number): Uint8Array {
        if (size < 0 || this.pos + size > this.end) {
            throw new LineTruncated(`unexpected end of data (need ${size} bytes at 0x${this.pos.toString(16)})`)
        }
        const slice = this.bytes.subarray(this.pos, this.pos + size)
        this.pos += size
        return slice
    }

    skip(size: number): void {
        this.take(size)
    }
}

/** 小端无符号整数；size=8 超过 2^53 时损失精度（地址查询用 32/48-bit 足够）。 */
function readUnsignedLE(bytes: Uint8Array, offset: number, size: number): number {
    let value = 0
    let scale = 1
    let index = 0
    while (index < size) {
        const chunk = Math.min(4, size - index)
        let part = 0
        for (let i = 0; i < chunk; i++) part += bytes[offset + index + i] * Math.pow(2, i * 8)
        value += part * scale
        if (chunk === 4) scale *= 0x100000000
        index += chunk
    }
    return value
}

const UTF8_DECODER = new TextDecoder('utf-8', {fatal: false})

function decodeUtf8(bytes: Uint8Array): string {
    try {
        return UTF8_DECODER.decode(bytes)
    } catch {
        let text = ''
        for (const byte of bytes) text += String.fromCharCode(byte)
        return text
    }
}

function warn(warnings: string[], message: string): void {
    if (warnings.length < MAX_WARNINGS) warnings.push(message)
}

function checkAddressSize(size: number, field: string): number {
    if (!Number.isInteger(size) || size < 1 || size > 8) {
        throw new LineTableError(`${field} must be an integer in [1, 8], got ${String(size)}`)
    }
    return size
}

/** 32-bit 目标保持无符号语义（与项目其它地方 >>> 0 一致）；64-bit 原样。 */
function normalizeAddress(value: number, addressSize: number): number {
    return addressSize <= 4 ? value >>> 0 : value
}

// ---------------------------------------------------------------------------
// 路径规范化 / 拼接
// ---------------------------------------------------------------------------

/** 规范化源码路径：Windows 反斜杠转 `/`、折叠重复 `/`、解析 `.`/`..`、去尾部 `/`。 */
export function normalizeSourcePath(input: string): string {
    if (!input) return ''
    let path = input.replace(/\\/g, '/')
    // Windows 盘符 / UNC 前缀
    let prefix = ''
    const drive = path.match(/^([A-Za-z]:)(\/?)/)
    if (drive) {
        prefix = drive[1].toUpperCase()
        path = path.slice(drive[0].length)
        if (!path.startsWith('/')) path = `/${path}`
    }

    const absolute = path.startsWith('/')
    const stack: string[] = []
    for (const segment of path.split('/')) {
        if (!segment || segment === '.') continue
        if (segment === '..') {
            if (stack.length > 0 && stack[stack.length - 1] !== '..') stack.pop()
            else if (!absolute) stack.push('..')
            continue
        }
        stack.push(segment)
    }
    const body = stack.join('/')
    if (prefix) return absolute || body ? `${prefix}${absolute ? '/' : ''}${body}` : prefix
    return `${absolute ? '/' : ''}${body}`
}

/** 是否绝对路径（Unix `/`、Windows 盘符、UNC `//`）。 */
export function isAbsoluteSourcePath(path: string): boolean {
    return /^\//.test(path) || /^[A-Za-z]:[\\/]/.test(path) || /^\\\\/.test(path) || /^\/\//.test(path)
}

/** 取路径末尾文件名。 */
export function sourceFileBasename(path: string): string {
    const normalized = normalizeSourcePath(path)
    const index = normalized.lastIndexOf('/')
    return index >= 0 ? normalized.slice(index + 1) : normalized
}

function joinPaths(...parts: Array<string | undefined | null>): string {
    const filtered = parts
        .filter((part): part is string => !!part)
        .map((part) => part.replace(/\\/g, '/'))
        .filter((part) => part.length > 0)
    if (filtered.length === 0) return ''
    const last = filtered[filtered.length - 1]
    if (isAbsoluteSourcePath(last)) return normalizeSourcePath(last)
    const joined = filtered.join('/')
    return normalizeSourcePath(joined)
}

// ---------------------------------------------------------------------------
// v5 line header form 读取
// ---------------------------------------------------------------------------
interface LineFormContext {
    offsetSize: number
    addressSize: number
    compDir: string
    lineStr: Uint8Array | null
    str: Uint8Array | null
    strOffsets: Uint8Array | null
    strOffsetsBase: number
}

interface LineFormValue {
    string?: string
    number?: number
    bytes?: Uint8Array
}

function readCStringAt(bytes: Uint8Array | null, offset: number): string {
    if (!bytes || offset < 0 || offset >= bytes.length) return ''
    let end = offset
    while (end < bytes.length && bytes[end] !== 0) end++
    return decodeUtf8(bytes.subarray(offset, end))
}

function readLineStrp(context: LineFormContext, reader: LineReader, isLineStr: boolean): string {
    const offset = reader.unsigned(context.offsetSize)
    return readCStringAt(isLineStr ? context.lineStr : context.str, offset)
}

function readStrx(context: LineFormContext, reader: LineReader, index: number): string {
    if (!context.strOffsets) return ''
    const entrySize = context.offsetSize
    const position = context.strOffsetsBase + index * entrySize
    if (position < 0 || position + entrySize > context.strOffsets.length) return ''
    const stringOffset = readUnsignedLE(context.strOffsets, position, entrySize)
    return readCStringAt(context.str, stringOffset)
}

/** 读取一个 line table header form 值（v5 目录/文件表用）。 */
function readLineForm(reader: LineReader, form: number, context: LineFormContext): LineFormValue {
    switch (form) {
        case FORM_STRING:
            return {string: reader.cstring()}
        case FORM_LINE_STRP:
            return {string: readLineStrp(context, reader, true)}
        case FORM_STRP:
            return {string: readLineStrp(context, reader, false)}
        case FORM_STRX:
            return {string: readStrx(context, reader, reader.uleb())}
        case FORM_STRX1:
            return {string: readStrx(context, reader, reader.u8())}
        case FORM_STRX2:
            return {string: readStrx(context, reader, reader.u16())}
        case FORM_STRX3:
            return {string: readStrx(context, reader, readUnsignedLE(reader.take(3), 0, 3))}
        case FORM_STRX4:
            return {string: readStrx(context, reader, reader.u32())}
        case FORM_DATA1:
            return {number: reader.u8()}
        case FORM_DATA2:
            return {number: reader.u16()}
        case FORM_DATA4:
            return {number: reader.u32()}
        case FORM_DATA8:
            return {number: reader.u64()}
        case FORM_DATA16:
            return {bytes: reader.take(16)}
        case FORM_UDATA:
            return {number: reader.uleb()}
        case FORM_SDATA:
            return {number: reader.sleb()}
        case FORM_FLAG:
            return {number: reader.u8()}
        case FORM_FLAG_PRESENT:
            return {number: 1}
        case FORM_BLOCK1:
            return {bytes: reader.take(reader.u8())}
        case FORM_BLOCK2:
            return {bytes: reader.take(reader.u16())}
        case FORM_BLOCK4:
            return {bytes: reader.take(reader.u32())}
        case FORM_BLOCK:
            return {bytes: reader.take(reader.uleb())}
        case FORM_EXPRLOC:
            return {bytes: reader.take(reader.uleb())}
        case FORM_SEC_OFFSET:
            return {number: reader.unsigned(context.offsetSize)}
        case FORM_ADDR:
            return {number: reader.unsigned(context.addressSize)}
        case FORM_ADDRX:
        case FORM_ADDRX1:
        case FORM_ADDRX2:
        case FORM_ADDRX3:
        case FORM_ADDRX4: {
            // line table 的目录/文件表不使用 addrx；消费掉操作数避免错位。
            const index =
                form === FORM_ADDRX
                    ? reader.uleb()
                    : form === FORM_ADDRX1
                      ? reader.u8()
                      : form === FORM_ADDRX2
                        ? reader.u16()
                        : form === FORM_ADDRX3
                          ? readUnsignedLE(reader.take(3), 0, 3)
                          : reader.u32()
            return {number: index}
        }
        case FORM_IMPLICIT_CONST:
            return {number: 0}
        default:
            throw new LineTableError(`unsupported form 0x${form.toString(16)} in .debug_line v5 table`)
    }
}

interface LineEntryFormat {
    content: number
    form: number
}

function readLineEntryFormats(reader: LineReader): LineEntryFormat[] {
    const count = reader.u8()
    const formats: LineEntryFormat[] = []
    for (let i = 0; i < count; i++) formats.push({content: reader.uleb(), form: reader.uleb()})
    return formats
}

// ---------------------------------------------------------------------------
// .debug_line 解析
// ---------------------------------------------------------------------------
interface SequenceState {
    address: number
    opIndex: number
    file: number
    line: number
    column: number
    isStmt: boolean
    basicBlock: boolean
    prologueEnd: boolean
    epilogueBegin: boolean
    isa: number
    discriminator: number
}

function initialSequenceState(defaultIsStmt: boolean): SequenceState {
    return {
        address: 0,
        opIndex: 0,
        // DWARF 行号程序 file 寄存器初值恒为 1；v2–v4 对应 files[0]，v5 对应 files[1]。
        file: 1,
        line: 1,
        column: 0,
        isStmt: defaultIsStmt,
        basicBlock: false,
        prologueEnd: false,
        epilogueBegin: false,
        isa: 0,
        discriminator: 0,
    }
}

function parseIntValue(reader: LineReader): number {
    return reader.uleb()
}

/**
 * 解析一个 `.debug_line` contribution（DWARF v2–v5）。
 *
 * @throws LineTableError 头部结构非法（版本不支持、长度越界、form 未知等）。
 *         行号程序数据损坏只写 warnings 并返回已解析部分。
 */
export function parseLineTable(options: ParseLineTableOptions): LineTable {
    const {
        data,
        offset = 0,
        lineStr = null,
        str = null,
        strOffsets = null,
        strOffsetsBase = 0,
        addressSize: contextAddressSize = 4,
        compDir = '',
        maxRows = DEFAULT_MAX_ROWS,
    } = options
    if (!(data instanceof Uint8Array)) throw new TypeError('data must be a Uint8Array')
    if (!Number.isInteger(offset) || offset < 0) throw new TypeError('offset must be a non-negative integer')
    if (!Number.isInteger(maxRows) || maxRows < 1) throw new TypeError('maxRows must be a positive integer')
    if (offset + 4 > data.length) {
        throw new LineTableError(`line table offset 0x${offset.toString(16)} is outside the section`)
    }

    const warnings: string[] = []
    const reader = new LineReader(data, offset)

    // ---- contribution header（长度 + version） ----
    let offsetSize = 4
    let lengthFieldSize = 4
    let unitLength = reader.u32()
    let dwarf64 = false
    if (unitLength === 0xffffffff) {
        dwarf64 = true
        offsetSize = 8
        lengthFieldSize = 12 // 0xffffffff(4) + u64 length(8)
        unitLength = reader.u64()
    }
    const endOffset = offset + lengthFieldSize + unitLength
    if (!Number.isFinite(unitLength) || unitLength < 2 || endOffset > data.length) {
        throw new LineTableError(
            `line table contribution at 0x${offset.toString(16)} has invalid unit_length 0x${unitLength.toString(16)}`,
        )
    }
    const version = reader.u16()
    if (version < 2 || version > 5) {
        throw new LineTableError(`unsupported .debug_line version ${version} at 0x${offset.toString(16)}`)
    }
    let addressSize = 4
    let segmentSelectorSize = 0
    if (version >= 5) {
        addressSize = reader.u8()
        checkAddressSize(addressSize, 'address_size')
        segmentSelectorSize = reader.u8()
        if (segmentSelectorSize !== 0 && addressSize + segmentSelectorSize > 8) {
            throw new LineTableError(
                `unsupported segment_selector_size ${segmentSelectorSize} with address_size ${addressSize}`,
            )
        }
    } else {
        addressSize = checkAddressSize(contextAddressSize, 'addressSize')
    }
    const headerLength = reader.unsigned(offsetSize)
    const headerEnd = reader.pos + headerLength
    if (headerEnd > endOffset) {
        throw new LineTableError(
            `line table header at 0x${offset.toString(16)} extends past contribution end (0x${headerEnd.toString(16)} > 0x${endOffset.toString(16)})`,
        )
    }
    const headerReader = new LineReader(data, reader.pos, headerEnd)

    // ---- 固定头部字段 ----
    const minInstructionLength = headerReader.u8()
    if (minInstructionLength === 0) {
        warn(warnings, 'minimum_instruction_length is 0 (invalid but tolerated)')
    }
    const maxOpsPerInstruction = version >= 4 ? headerReader.u8() : 1
    const defaultIsStmt = headerReader.u8() !== 0
    const lineBaseByte = headerReader.u8()
    const lineBase = lineBaseByte >= 0x80 ? lineBaseByte - 0x100 : lineBaseByte
    const lineRangeRaw = headerReader.u8()
    let lineRange = lineRangeRaw
    if (lineRange === 0) {
        warn(warnings, 'line_range is 0 (invalid, using 1)')
        lineRange = 1
    }
    const opcodeBase = headerReader.u8()
    if (opcodeBase < 1) throw new LineTableError('opcode_base must be >= 1')
    if (opcodeBase > headerReader.remaining + 1) {
        throw new LineTableError(`opcode_base ${opcodeBase} exceeds header data`)
    }
    const standardOpcodeLengths: number[] = []
    for (let i = 1; i < opcodeBase; i++) standardOpcodeLengths.push(headerReader.uleb())

    const formContext: LineFormContext = {
        offsetSize,
        addressSize,
        compDir,
        lineStr,
        str,
        strOffsets,
        strOffsetsBase: Number.isInteger(strOffsetsBase) && strOffsetsBase >= 0 ? strOffsetsBase : 0,
    }

    const files: LineTableFile[] = []
    const directories: string[] = []
    const addDirectory = (raw: string): number => {
        const resolved = joinPaths(compDir, raw)
        directories.push(resolved)
        return directories.length - 1
    }
    const addFile = (fields: {
        name: string
        directoryIndex: number
        mtime: number
        size: number
        md5?: Uint8Array
    }): LineTableFile => {
        const normalizedName = fields.name.replace(/\\/g, '/')
        let directory = ''
        if (version >= 5) {
            directory = directories[fields.directoryIndex] ?? ''
            if (!directory) {
                // v5：directory_index 0 通常是 comp_dir；目录表缺失时回退 comp_dir。
                directory = compDir ? normalizeSourcePath(compDir) : ''
            }
        } else if (fields.directoryIndex > 0) {
            directory = directories[fields.directoryIndex - 1] ?? ''
            if (!directory && compDir) directory = normalizeSourcePath(compDir)
        } else {
            directory = compDir ? normalizeSourcePath(compDir) : ''
        }
        const path = joinPaths(directory, normalizedName)
        const file: LineTableFile = {
            index: files.length,
            dwarfIndex: version >= 5 ? files.length : files.length + 1,
            name: fields.name,
            directoryIndex: fields.directoryIndex,
            directory,
            path,
            fullPath: path,
            mtime: fields.mtime,
            size: fields.size,
        }
        if (fields.md5) file.md5 = fields.md5
        files.push(file)
        return file
    }

    if (version >= 5) {
        // ---- v5 目录表 ----
        const directoryFormats = readLineEntryFormats(headerReader)
        const directoryCount = headerReader.uleb()
        if (directoryCount > 1_000_000) throw new LineTableError(`directory count ${directoryCount} is too large`)
        for (let i = 0; i < directoryCount; i++) {
            let path = ''
            for (const format of directoryFormats) {
                const value = readLineForm(headerReader, format.form, formContext)
                if (format.content === DW_LNCT_PATH) path = value.string ?? ''
            }
            addDirectory(path)
        }
        // ---- v5 文件表 ----
        const fileFormats = readLineEntryFormats(headerReader)
        const fileCount = headerReader.uleb()
        if (fileCount > 1_000_000) throw new LineTableError(`file count ${fileCount} is too large`)
        for (let i = 0; i < fileCount; i++) {
            let name = ''
            let directoryIndex = 0
            let mtime = 0
            let size = 0
            let md5: Uint8Array | undefined
            for (const format of fileFormats) {
                const value = readLineForm(headerReader, format.form, formContext)
                switch (format.content) {
                    case DW_LNCT_PATH:
                        name = value.string ?? ''
                        break
                    case DW_LNCT_DIRECTORY_INDEX:
                        directoryIndex = value.number ?? 0
                        break
                    case DW_LNCT_TIMESTAMP:
                        mtime = value.number ?? 0
                        break
                    case DW_LNCT_SIZE:
                        size = value.number ?? 0
                        break
                    case DW_LNCT_MD5:
                        md5 = value.bytes ? new Uint8Array(value.bytes) : undefined
                        break
                    default:
                        break
                }
            }
            addFile({name, directoryIndex, mtime, size, md5})
        }
    } else {
        // ---- v2–v4 字符串目录表 / 文件表 ----
        for (;;) {
            if (headerReader.remaining === 0) {
                warn(warnings, 'v2–v4 directory table is not NUL-terminated')
                break
            }
            const directory = headerReader.cstring()
            if (!directory) break
            addDirectory(directory)
            if (directories.length > 1_000_000) throw new LineTableError('too many include directories')
        }
        for (;;) {
            if (headerReader.remaining === 0) {
                warn(warnings, 'v2–v4 file table is not NUL-terminated')
                break
            }
            const name = headerReader.cstring()
            if (!name) break
            const directoryIndex = headerReader.uleb()
            const mtime = headerReader.uleb()
            const size = headerReader.uleb()
            addFile({name, directoryIndex, mtime, size})
            if (files.length > 1_000_000) throw new LineTableError('too many file entries')
        }
    }
    if (headerReader.pos > headerEnd) throw new LineTableError('line table header overran header_length')
    if (headerReader.pos < headerEnd) {
        // header_length 之后可能有 padding / 厂商扩展：以 header_length 为准跳过。
        warn(
            warnings,
            `line table header parser consumed 0x${headerReader.pos.toString(16)} but header_length ends at 0x${headerEnd.toString(16)}; skipping padding`,
        )
    }

    // ---- 行号程序（从 headerEnd 开始） ----
    const programReader = new LineReader(data, headerEnd, endOffset)
    const rows: LineRow[] = []
    const sequences: LineSequence[] = []
    let state = initialSequenceState(defaultIsStmt)
    let sequenceIndex = 0
    let sequenceStartRow = 0
    let sequenceLowPc = 0
    let sequenceMonotonic = true
    let lastRowAddress = 0
    let programStopped = false

    const resolveFile = (index: number): LineTableFile | null => {
        if (version >= 5) return files[index] ?? null
        return index > 0 ? (files[index - 1] ?? null) : null
    }

    const emitRow = (endSequence: boolean): void => {
        if (rows.length >= maxRows) {
            programStopped = true
            warn(warnings, `line program exceeds maxRows ${maxRows}; decoding stopped`)
            return
        }
        if (rows.length === sequenceStartRow) {
            sequenceLowPc = normalizeAddress(state.address, addressSize)
            sequenceMonotonic = true
            lastRowAddress = sequenceLowPc
        } else if (normalizeAddress(state.address, addressSize) < lastRowAddress) {
            sequenceMonotonic = false
        }
        lastRowAddress = normalizeAddress(state.address, addressSize)
        const fileIndex = state.file
        const file = resolveFile(fileIndex)
        const row: LineRow = {
            address: normalizeAddress(state.address, addressSize),
            opIndex: state.opIndex,
            fileIndex,
            file,
            line: state.line,
            column: state.column,
            isStmt: state.isStmt,
            basicBlock: state.basicBlock,
            prologueEnd: state.prologueEnd,
            epilogueBegin: state.epilogueBegin,
            isa: state.isa,
            discriminator: state.discriminator,
            endSequence,
            sequence: sequenceIndex,
            rowIndex: rows.length,
        }
        rows.push(row)
        state.basicBlock = false
        state.prologueEnd = false
        state.epilogueBegin = false
        state.discriminator = 0
        if (endSequence) {
            sequences.push({
                startRow: sequenceStartRow,
                endRow: row.rowIndex,
                lowPc: sequenceLowPc,
                highPc: row.address,
                terminated: true,
                monotonic: sequenceMonotonic,
            })
            sequenceIndex += 1
            state = initialSequenceState(defaultIsStmt)
            sequenceStartRow = rows.length
        }
    }

    const advanceAddressBy = (operationAdvance: number): void => {
        if (operationAdvance === 0) return
        if (maxOpsPerInstruction <= 1) {
            state.address += operationAdvance * minInstructionLength
            state.opIndex = 0
        } else {
            const total = state.opIndex + operationAdvance
            const addressAdvance = Math.floor(total / maxOpsPerInstruction) * minInstructionLength
            state.address += addressAdvance
            state.opIndex = total % maxOpsPerInstruction
        }
    }

    try {
        while (programReader.remaining > 0 && !programStopped) {
            const opcode = programReader.u8()
            if (opcode >= opcodeBase) {
                // special opcode
                const adjusted = opcode - opcodeBase
                advanceAddressBy(Math.floor(adjusted / lineRange))
                state.line += lineBase + (adjusted % lineRange)
                emitRow(false)
                continue
            }
            if (opcode === 0) {
                // extended opcode
                const extendedLength = programReader.uleb()
                const operandEnd = programReader.pos + extendedLength
                if (extendedLength < 1 || operandEnd > programReader.end) {
                    warn(warnings, `bad extended line opcode length ${extendedLength} at 0x${programReader.pos.toString(16)}`)
                    break
                }
                const subOpcode = programReader.u8()
                const operandReader = new LineReader(programReader.bytes, programReader.pos, operandEnd)
                if (subOpcode === DW_LNE_END_SEQUENCE) {
                    emitRow(true)
                } else if (subOpcode === DW_LNE_SET_ADDRESS) {
                    if (operandReader.remaining < addressSize + segmentSelectorSize) {
                        warn(warnings, 'DW_LNE_set_address operand is too short')
                        break
                    }
                    if (segmentSelectorSize > 0) operandReader.skip(segmentSelectorSize)
                    state.address = operandReader.unsigned(addressSize)
                    state.opIndex = 0
                } else if (subOpcode === DW_LNE_DEFINE_FILE) {
                    const name = operandReader.cstring()
                    const directoryIndex = operandReader.uleb()
                    const mtime = operandReader.uleb()
                    const size = operandReader.uleb()
                    addFile({name, directoryIndex, mtime, size})
                } else if (subOpcode === DW_LNE_SET_DISCRIMINATOR) {
                    state.discriminator = operandReader.uleb()
                } else {
                    warn(warnings, `unknown extended line opcode 0x${subOpcode.toString(16)}; skipped`)
                }
                programReader.pos = operandEnd
                continue
            }
            // standard opcode
            switch (opcode) {
                case DW_LNS_COPY:
                    emitRow(false)
                    break
                case DW_LNS_ADVANCE_PC:
                    advanceAddressBy(parseIntValue(programReader))
                    break
                case DW_LNS_ADVANCE_LINE:
                    state.line += programReader.sleb()
                    break
                case DW_LNS_SET_FILE:
                    state.file = programReader.uleb()
                    break
                case DW_LNS_SET_COLUMN:
                    state.column = programReader.uleb()
                    break
                case DW_LNS_NEGATE_STMT:
                    state.isStmt = !state.isStmt
                    break
                case DW_LNS_SET_BASIC_BLOCK:
                    state.basicBlock = true
                    break
                case DW_LNS_CONST_ADD_PC:
                    advanceAddressBy(Math.floor((255 - opcodeBase) / lineRange))
                    break
                case DW_LNS_FIXED_ADVANCE_PC:
                    state.address += programReader.u16()
                    state.opIndex = 0
                    break
                case DW_LNS_SET_PROLOGUE_END:
                    state.prologueEnd = true
                    break
                case DW_LNS_SET_EPILOGUE_BEGIN:
                    state.epilogueBegin = true
                    break
                case DW_LNS_SET_ISA:
                    state.isa = programReader.uleb()
                    break
                default: {
                    const operandCount = standardOpcodeLengths[opcode - 1] ?? 0
                    warn(warnings, `unknown standard line opcode ${opcode} (${operandCount} operands); skipped`)
                    for (let i = 0; i < operandCount; i++) programReader.uleb()
                    break
                }
            }
        }
        if (!programStopped && rows.length > sequenceStartRow) {
            // EOF 时仍在 sequence 中：补一个未终止的 sequence，方便 PC 查询。
            const startRow = sequenceStartRow
            let lowPc = rows[startRow]?.address ?? 0
            let highPc = lowPc
            let monotonic = true
            let previous = lowPc
            for (let i = startRow; i < rows.length; i++) {
                const address = rows[i].address
                if (address < previous) monotonic = false
                if (address < lowPc) lowPc = address
                if (address > highPc) highPc = address
                previous = address
            }
            sequences.push({
                startRow,
                endRow: rows.length - 1,
                lowPc,
                highPc: highPc + 1,
                terminated: false,
                monotonic,
            })
            warn(warnings, 'line number program ended without DW_LNE_end_sequence')
        }
    } catch (error) {
        if (error instanceof LineTruncated) {
            warn(warnings, `line program at 0x${headerEnd.toString(16)}: ${error.message}`)
        } else {
            throw error
        }
    }

    return {
        offset,
        endOffset,
        version,
        addressSize,
        segmentSelectorSize,
        dwarf64,
        headerLength,
        minInstructionLength,
        maxOpsPerInstruction,
        defaultIsStmt,
        lineBase,
        lineRange,
        opcodeBase,
        standardOpcodeLengths,
        directories,
        files,
        rows,
        sequences,
        warnings,
    }
}

// ---------------------------------------------------------------------------
// PC -> 文件:行
// ---------------------------------------------------------------------------

function findRowInSequence(table: LineTable, sequence: LineSequence, pc: number): LineRow | null {
    const rows = table.rows
    const lastCandidate = sequence.endRow - 1 // 排除 endSequence 行
    if (lastCandidate < sequence.startRow) return null
    if (!sequence.monotonic) {
        let candidate: LineRow | null = null
        for (let i = sequence.startRow; i <= lastCandidate; i++) {
            if (rows[i].address <= pc) candidate = rows[i]
        }
        return candidate
    }
    let low = sequence.startRow
    let high = lastCandidate
    let candidate = -1
    while (low <= high) {
        const middle = (low + high) >> 1
        if (rows[middle].address <= pc) {
            candidate = middle
            low = middle + 1
        } else {
            high = middle - 1
        }
    }
    return candidate >= 0 ? rows[candidate] : null
}

/**
 * PC -> 源码行：在覆盖 pc 的 sequence 中取地址 <= pc 的最后一个矩阵行。
 * 返回 null 表示该 PC 没有行信息（例如启动代码/汇编无行表）。
 */
export function addressToLine(table: LineTable, pc: number): LineHit | null {
    for (const sequence of table.sequences) {
        if (pc < sequence.lowPc || pc >= sequence.highPc) continue
        const row = findRowInSequence(table, sequence, pc)
        if (!row) continue
        return {
            address: row.address,
            file: row.file,
            line: row.line,
            column: row.column,
            isStmt: row.isStmt,
            row,
        }
    }
    return null
}

/** addressToLine 的别名（PC -> 源码行）。 */
export function pcToLine(table: LineTable, pc: number): LineHit | null {
    return addressToLine(table, pc)
}

// ---------------------------------------------------------------------------
// 文件路径查询 / file:line -> 地址
// ---------------------------------------------------------------------------

function fileMatchKeys(file: LineTableFile): string[] {
    const keys = new Set<string>()
    keys.add(normalizeSourcePath(file.path))
    keys.add(normalizeSourcePath(file.name))
    return [...keys]
}

/**
 * 解析一个源码路径查询，返回所有匹配的文件表项。
 * 匹配顺序：规范化后完全相等 > 路径后缀（`.../query`）> 仅文件名相等。
 */
export function resolveLineTableFiles(
    table: LineTable,
    query: string,
    mode: FileMatchMode = 'auto',
): LineTableFile[] {
    const normalized = normalizeSourcePath(query)
    if (!normalized) return []
    const exact: LineTableFile[] = []
    const suffix: LineTableFile[] = []
    const basenameMatches: LineTableFile[] = []
    const basename = sourceFileBasename(normalized)
    for (const file of table.files) {
        const keys = fileMatchKeys(file)
        if (mode === 'exact' || mode === 'auto') {
            if (keys.some((key) => key === normalized)) exact.push(file)
        }
        if (mode === 'suffix' || mode === 'auto') {
            if (keys.some((key) => key.endsWith(`/${normalized}`))) suffix.push(file)
        }
        if (mode === 'basename' || mode === 'auto') {
            if (sourceFileBasename(file.path) === basename) basenameMatches.push(file)
        }
    }
    if (mode === 'exact') return exact
    if (mode === 'suffix') return suffix
    if (mode === 'basename') return basenameMatches
    if (exact.length > 0) return exact
    if (suffix.length > 0) return suffix
    return basenameMatches
}

/** 查找单个文件（多个匹配时返回第一个；需要全部用 resolveLineTableFiles）。 */
export function findLineTableFile(
    table: LineTable,
    query: string,
    mode: FileMatchMode = 'auto',
): LineTableFile | null {
    return resolveLineTableFiles(table, query, mode)[0] ?? null
}

/** 列出路径前缀（目录）下的所有文件表项。 */
export function findFilesByPathPrefix(table: LineTable, prefix: string): LineTableFile[] {
    const normalized = normalizeSourcePath(prefix)
    if (!normalized) return []
    const withSlash = normalized.endsWith('/') ? normalized : `${normalized}/`
    return table.files.filter((file) => {
        const path = normalizeSourcePath(file.path)
        return path === normalized || path.startsWith(withSlash)
    })
}

/** file:line -> 所有命中地址（按地址升序，已去重同址同 opIndex）。 */
export function fileLineToAddresses(
    table: LineTable,
    file: LineTableFile | string,
    line: number,
    mode: FileMatchMode = 'auto',
): LineAddress[] {
    if (!Number.isInteger(line)) return []
    const targets = typeof file === 'string' ? resolveLineTableFiles(table, file, mode) : [file]
    if (targets.length === 0) return []
    const targetIndexes = new Set(targets.map((target) => target.index))
    const results: LineAddress[] = []
    const seen = new Set<string>()
    for (const row of table.rows) {
        if (row.endSequence || row.line !== line) continue
        if (!row.file || !targetIndexes.has(row.file.index)) continue
        const key = `${row.address}:${row.opIndex}`
        if (seen.has(key)) continue
        seen.add(key)
        results.push({
            address: row.address,
            opIndex: row.opIndex,
            rowIndex: row.rowIndex,
            isStmt: row.isStmt,
            fileIndex: row.fileIndex,
        })
    }
    results.sort((left, right) => left.address - right.address || left.opIndex - right.opIndex)
    return results
}

/**
 * 构建 file:line -> rows 下标索引（纯函数，返回普通 Map）。
 * key 为文件表项的规范化 path（与 resolveLineTableFiles 的查询路径可直接配合）。
 */
export function buildFileLineIndex(table: LineTable): FileLineIndex {
    const index: FileLineIndex = new Map()
    for (const row of table.rows) {
        if (row.endSequence || !row.file) continue
        const key = normalizeSourcePath(row.file.path)
        let lines = index.get(key)
        if (!lines) {
            lines = new Map()
            index.set(key, lines)
        }
        const rowIndexes = lines.get(row.line)
        if (rowIndexes) rowIndexes.push(row.rowIndex)
        else lines.set(row.line, [row.rowIndex])
    }
    return index
}

/** 用 buildFileLineIndex 的结果做 file:line 查询。 */
export function queryFileLineIndex(
    table: LineTable,
    index: FileLineIndex,
    file: LineTableFile | string,
    line: number,
    mode: FileMatchMode = 'auto',
): LineAddress[] {
    const targets = typeof file === 'string' ? resolveLineTableFiles(table, file, mode) : [file]
    const results: LineAddress[] = []
    const seen = new Set<number>()
    for (const target of targets) {
        const rowIndexes = index.get(normalizeSourcePath(target.path))?.get(line)
        if (!rowIndexes) continue
        for (const rowIndex of rowIndexes) {
            if (seen.has(rowIndex)) continue
            seen.add(rowIndex)
            const row = table.rows[rowIndex]
            results.push({
                address: row.address,
                opIndex: row.opIndex,
                rowIndex: row.rowIndex,
                isStmt: row.isStmt,
                fileIndex: row.fileIndex,
            })
        }
    }
    results.sort((left, right) => left.address - right.address || left.opIndex - right.opIndex)
    return results
}
