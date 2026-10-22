/**
 * 最小但完整的 DWARF 调试信息解析器（纯 TS，无第三方依赖）。
 *
 * 目标：从 ELF 的 .debug_info/.debug_abbrev/.debug_str 中提取全局/静态变量，
 * 解析 DW_AT_name / DW_AT_type / DW_AT_location(DW_OP_addr) / decl_file/line，
 * 并把类型 DIE 链解析为人类可读的 C/C++ 类型字符串 + 字节大小 + 可采样语义，
 * 同时为 struct/union/array 生成可展开的成员/元素（带绝对地址），供页面做
 * Ozone 风格的变量浏览器。
 *
 * 支持：
 *  - DWARF 2/3/4/5 编译单元头；32/64-bit 初始长度与偏移；
 *  - 常用 form（addr / data* / sdata / udata / string / strp / line_strp / strx* / addrx* /
 *    ref* / exprloc / block* / sec_offset / flag* / implicit_const / indirect…）；
 *  - DWARF5 的 .debug_str_offsets/.debug_addr/.debug_line_str（strx/addrx/line_strp）；
 *  - 类型：base/typedef/const/volatile/restrict/atomic/pointer/reference/
 *    rvalue_reference/ptr_to_member/array(多维)/struct/class/union/enum/
 *    subroutine/unspecified；typedef 保留人类可读别名；
 *  - 成员：DW_AT_data_member_location（常量或简单 DWARF 表达式）、
 *    DW_AT_data_bit_offset/DW_AT_bit_size 位域、匿名成员展开、继承；
 *  - decl_file/line：解析 .debug_line 文件表（v2–v5），取 basename:line。
 *
 * 支持（调试采样 A2）：
 *  - 位置表达式：DW_OP_addr/addrx（实时）、DW_OP_fbreg + 子程序 DW_AT_frame_base
 *    （reg7/reg11/CFA）、DW_OP_bregN/bregx（栈帧局部变量）、DW_OP_regN（值在寄存器）；
 *  - 指针成员展开：指针变量 → 被指向 struct/union 成员 / 数组元素（`->member`），
 *    带解引用表达式，可在实时（两阶段）或调试（暂停）模式采样。
 *
 * 限制（页面 UI 会提示/降级）：
 *  - .debug_loc 位置列表不解析（局部变量，标记为无位置）；
 *  - 展开深度 3、每节点最多 64 个子项，防止巨型结构体爆 UI；
 *  - 64-bit 值以两个 32-bit word 读取，>64-bit 聚合只读前 8 字节。
 */
import type {AddressExpr, DisplayFormat, ElfSectionInfo, LocationVariant, SampleLeaf, ValueKind, VariableNode} from './elfTypes'
import {decodeLocationList, type LocationListEntry} from './dwarfLocList'
import {isPlausibleTargetAddress, TARGET_FLASH_RANGE} from './elfTypes'
import {addressExprIsLive} from './dwarfLocation'

export class DwarfParseError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'DwarfParseError'
    }
}

// ---------------------------------------------------------------------------
// DWARF 常量（只列用到的）
// ---------------------------------------------------------------------------
const TAG_ARRAY_TYPE = 0x01
const TAG_CLASS_TYPE = 0x02
const TAG_ENUMERATION_TYPE = 0x04
const TAG_FORMAL_PARAMETER = 0x05
const TAG_MEMBER = 0x0d
const TAG_POINTER_TYPE = 0x0f
const TAG_REFERENCE_TYPE = 0x10
const TAG_COMPILE_UNIT = 0x11
const TAG_STRUCTURE_TYPE = 0x13
const TAG_SUBROUTINE_TYPE = 0x15
const TAG_TYPEDEF = 0x16
const TAG_UNION_TYPE = 0x17
const TAG_INHERITANCE = 0x1c
const TAG_PTR_TO_MEMBER_TYPE = 0x1f
const TAG_SUBRANGE_TYPE = 0x21
const TAG_BASE_TYPE = 0x24
const TAG_CONST_TYPE = 0x26
const TAG_ENUMERATOR = 0x28
const TAG_VARIABLE = 0x34
const TAG_VOLATILE_TYPE = 0x35
const TAG_RESTRICT_TYPE = 0x37
const TAG_NAMESPACE = 0x39
const TAG_UNSPECIFIED_TYPE = 0x3b
const TAG_SUBPROGRAM = 0x2e
const TAG_RVALUE_REFERENCE_TYPE = 0x42
const TAG_ATOMIC_TYPE = 0x47
const TAG_PACKED_TYPE = 0x2d

const AT_NAME = 0x03
const AT_LOCATION = 0x02
const AT_FRAME_BASE = 0x40
const AT_BYTE_SIZE = 0x0b
const AT_LOW_PC = 0x11
const AT_HIGH_PC = 0x12
const AT_LOCLISTS_BASE = 0x74
const AT_BIT_SIZE = 0x0d
const AT_BIT_OFFSET = 0x0c
const AT_DATA_MEMBER_LOCATION = 0x38
const AT_DATA_BIT_OFFSET = 0x6b
const AT_LOWER_BOUND = 0x22
const AT_UPPER_BOUND = 0x2f
const AT_COUNT = 0x37
const AT_TYPE = 0x49
const AT_DECL_FILE = 0x3a
const AT_DECL_LINE = 0x3b
const AT_DECLARATION = 0x3c
const AT_SPECIFICATION = 0x47
const AT_ABSTRACT_ORIGIN = 0x31
const AT_LINKAGE_NAME = 0x6e
const AT_COMP_DIR = 0x1b
const AT_STMT_LIST = 0x10
const AT_STR_OFFSETS_BASE = 0x72
const AT_ADDR_BASE = 0x73
const AT_ENCODING = 0x3e
const AT_CONST_VALUE = 0x1c

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
const FORM_REF_ADDR = 0x10
const FORM_REF1 = 0x11
const FORM_REF2 = 0x12
const FORM_REF4 = 0x13
const FORM_REF8 = 0x14
const FORM_REF_UDATA = 0x15
const FORM_INDIRECT = 0x16
const FORM_SEC_OFFSET = 0x17
const FORM_EXPRLOC = 0x18
const FORM_FLAG_PRESENT = 0x19
const FORM_STRX = 0x1a
const FORM_ADDRX = 0x1b
const FORM_REF_SUP4 = 0x1c
const FORM_STRP_SUP = 0x1d
const FORM_DATA16 = 0x1e
const FORM_LINE_STRP = 0x1f
const FORM_REF_SIG8 = 0x20
const FORM_IMPLICIT_CONST = 0x21
const FORM_LOCLISTX = 0x22
const FORM_RNGLISTX = 0x23
const FORM_REF_SUP8 = 0x24
const FORM_STRX1 = 0x25
const FORM_STRX2 = 0x26
const FORM_STRX3 = 0x27
const FORM_STRX4 = 0x28
const FORM_ADDRX1 = 0x29
const FORM_ADDRX2 = 0x2a
const FORM_ADDRX3 = 0x2b
const FORM_ADDRX4 = 0x2c

const OP_ADDR = 0x03
const OP_CONSTU = 0x10
const OP_PLUS = 0x22
const OP_PLUS_UCONST = 0x23
const OP_LIT0 = 0x30
const OP_LIT31 = 0x4f
const OP_ADDRX = 0xa1
const OP_GNU_ADDR_INDEX = 0xfb
const OP_REG0 = 0x50
const OP_REG31 = 0x6f
const OP_BREG0 = 0x70
const OP_BREG31 = 0x8f
const OP_REGX = 0x90
const OP_FBREG = 0x91
const OP_BREGX = 0x92
const OP_CALL_FRAME_CFA = 0x9c
const OP_STACK_VALUE = 0x9f
const OP_NOP = 0x96

const DW_ATE_ADDRESS = 0x01
const DW_ATE_BOOLEAN = 0x02
const DW_ATE_COMPLEX_FLOAT = 0x03
const DW_ATE_FLOAT = 0x04
const DW_ATE_SIGNED = 0x05
const DW_ATE_SIGNED_CHAR = 0x06
const DW_ATE_UNSIGNED = 0x07
const DW_ATE_UNSIGNED_CHAR = 0x08
const DW_ATE_IMAGINARY_FLOAT = 0x09
const DW_ATE_SIGNED_FIXED = 0x0d
const DW_ATE_UNSIGNED_FIXED = 0x0e

const MAX_EXPAND_DEPTH = 3
const MAX_CHILDREN = 64
const MAX_NODES_PER_VARIABLE = 320

// ---------------------------------------------------------------------------
// 字节读取
// ---------------------------------------------------------------------------
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

class ByteReader {
    offset: number
    private readonly bytes: Uint8Array
    private readonly end: number

    constructor(bytes: Uint8Array, offset = 0, end = bytes.length) {
        this.bytes = bytes
        this.offset = offset
        this.end = Math.min(end, bytes.length)
    }

    get remaining(): number {
        return this.end - this.offset
    }

    u8(): number {
        if (this.offset + 1 > this.end) throw new DwarfParseError('DWARF: unexpected end of data (u8)')
        return this.bytes[this.offset++]
    }

    u16(): number {
        this.ensure(2)
        const value = this.bytes[this.offset] | (this.bytes[this.offset + 1] << 8)
        this.offset += 2
        return value
    }

    u32(): number {
        this.ensure(4)
        const base = this.offset
        const value =
            (this.bytes[base] |
                (this.bytes[base + 1] << 8) |
                (this.bytes[base + 2] << 16) |
                (this.bytes[base + 3] << 24)) >>>
            0
        this.offset += 4
        return value
    }

    /** 32/64-bit 长度值（DWARF 64-bit 格式用 u64；JS number 足够）。 */
    u64(): number {
        const low = this.u32()
        const high = this.u32()
        return high * 0x100000000 + low
    }

    uleb(): number {
        let result = 0
        let shift = 0
        for (;;) {
            const byte = this.u8()
            if (shift < 53) result += (byte & 0x7f) * Math.pow(2, shift)
            shift += 7
            if ((byte & 0x80) === 0 || shift > 63) return result >>> 0
        }
    }

    sleb(): number {
        let result = 0
        let shift = 0
        let byte = 0
        for (;;) {
            byte = this.u8()
            if (shift < 53) result += (byte & 0x7f) * Math.pow(2, shift)
            shift += 7
            if ((byte & 0x80) === 0 || shift > 63) break
        }
        if (shift < 64 && (byte & 0x40) !== 0) result -= Math.pow(2, shift)
        return result
    }

    cstring(): string {
        const start = this.offset
        while (this.offset < this.end && this.bytes[this.offset] !== 0) this.offset++
        const text = decodeUtf8(this.bytes.subarray(start, this.offset))
        if (this.offset < this.end) this.offset++ // NUL
        return text
    }

    take(length: number): Uint8Array {
        this.ensure(length)
        const slice = this.bytes.subarray(this.offset, this.offset + length)
        this.offset += length
        return slice
    }

    skip(length: number): void {
        this.ensure(length)
        this.offset += length
    }

    private ensure(length: number): void {
        if (length < 0 || this.offset + length > this.end) {
            throw new DwarfParseError(`DWARF: unexpected end of data (need ${length} at 0x${this.offset.toString(16)})`)
        }
    }
}

function readUint(bytes: Uint8Array, offset: number, size: number): number {
    if (offset < 0 || offset + size > bytes.length) return 0
    let value = 0
    for (let i = 0; i < size; i++) {
        value += bytes[offset + i] * Math.pow(2, i * 8)
    }
    return value >>> 0
}

// ---------------------------------------------------------------------------
// 段表辅助
// ---------------------------------------------------------------------------
interface DwarfSections {
    info: Uint8Array
    abbrev: Uint8Array
    str: Uint8Array
    lineStr: Uint8Array | null
    strOffsets: Uint8Array | null
    addr: Uint8Array | null
    line: Uint8Array | null
    /** DWARF2/3/4 位置列表节 */
    loc: Uint8Array | null
    /** DWARF5 位置列表节 */
    loclists: Uint8Array | null
}

function sectionBytes(bytes: Uint8Array, sections: readonly ElfSectionInfo[], name: string): Uint8Array | null {
    const section = sections.find((item) => item.name === name)
    if (!section || section.offset + section.size > bytes.length) return null
    return bytes.subarray(section.offset, section.offset + section.size)
}

// ---------------------------------------------------------------------------
// DIE / Unit 模型
// ---------------------------------------------------------------------------
interface AttrSpec {
    name: number
    form: number
    implicit?: number
}

interface Abbrev {
    tag: number
    children: boolean
    attrs: AttrSpec[]
}

interface AttrValue {
    form: number
    number?: number
    string?: string
    block?: Uint8Array
    ref?: number
    refAbsolute?: boolean
}

interface Die {
    offset: number
    tag: number
    attrs: Map<number, AttrValue>
    parent: Die | null
    children: Die[]
    unit: Unit
    declFile?: number
    declLine?: number
}

interface Unit {
    start: number
    end: number
    version: number
    addrSize: number
    abbrevOffset: number
    dieOffset: number
    strOffsetsBase: number
    addrBase: number
    compDir: string
    stmtList: number | null
    root: Die | null
    dies: Die[]
    lineFiles: string[] | null
    lineFilesLoaded: boolean
    declFileCache: Map<number, string>
    /** CU 的 DW_AT_low_pc（.debug_loc base 初值） */
    lowPc: number
    /** DWARF5 DW_AT_loclists_base（offset table 起始） */
    loclistsBase: number
    /** DWARF32=4 / DWARF64=8（loclistx offset 表项宽度） */
    offsetSize: number
}

interface Ctx {
    bytes: Uint8Array
    sections: DwarfSections
    units: Unit[]
    dies: Map<number, Die>
    /** 聚合类型定义索引（qualified/short name -> DIEs，用于补全跨 CU 的前向声明）。 */
    aggregateDefs?: Map<string, Die[]>
    /** 正在补全的声明 DIE offset，防止 A<->B 互相补全导致栈溢出。 */
    completing?: Set<number>
    warning: (message: string) => void
    /** Itanium C++ 反混淆（由 elfSymbols 注入，避免循环依赖）。 */
    demangle?: (mangled: string) => string | null
}

// ---------------------------------------------------------------------------
// Abbrev 表
// ---------------------------------------------------------------------------
function parseAbbrevTable(bytes: Uint8Array, offset: number): Map<number, Abbrev> {
    const reader = new ByteReader(bytes, offset)
    const table = new Map<number, Abbrev>()
    for (;;) {
        const code = reader.uleb()
        if (code === 0) break
        const tag = reader.uleb()
        const children = reader.u8() !== 0
        const attrs: AttrSpec[] = []
        for (;;) {
            const name = reader.uleb()
            const form = reader.uleb()
            if (name === 0 && form === 0) break
            const spec: AttrSpec = {name, form}
            if (form === FORM_IMPLICIT_CONST) spec.implicit = reader.sleb()
            attrs.push(spec)
        }
        table.set(code, {tag, children, attrs})
        if (table.size > 100000) throw new DwarfParseError('DWARF: abbrev table is too large')
    }
    return table
}

// ---------------------------------------------------------------------------
// Form 读取
// ---------------------------------------------------------------------------
function readForm(reader: ByteReader, form: number, unit: Unit, implicit?: number): AttrValue {
    switch (form) {
        case FORM_ADDR:
            return {form, number: readUint(reader.take(unit.addrSize), 0, unit.addrSize)}
        case FORM_BLOCK2:
            return {form, block: reader.take(reader.u16())}
        case FORM_BLOCK4:
            return {form, block: reader.take(reader.u32())}
        case FORM_BLOCK:
            return {form, block: reader.take(reader.uleb())}
        case FORM_BLOCK1:
            return {form, block: reader.take(reader.u8())}
        case FORM_DATA2:
            return {form, number: reader.u16()}
        case FORM_DATA4:
            return {form, number: reader.u32()}
        case FORM_DATA8:
            return {form, number: reader.u64()}
        case FORM_DATA1:
            return {form, number: reader.u8()}
        case FORM_DATA16: {
            const raw = reader.take(16)
            return {form, number: readUint(raw, 0, 8)}
        }
        case FORM_STRING:
            return {form, string: reader.cstring()}
        case FORM_FLAG:
            return {form, number: reader.u8()}
        case FORM_FLAG_PRESENT:
            return {form, number: 1}
        case FORM_SDATA:
            return {form, number: reader.sleb()}
        case FORM_UDATA:
            return {form, number: reader.uleb()}
        case FORM_STRP:
        case FORM_STRP_SUP:
        case FORM_LINE_STRP:
        case FORM_REF_SUP4:
        case FORM_SEC_OFFSET:
        case FORM_REF4:
        case FORM_STRX:
        case FORM_ADDRX:
        case FORM_LOCLISTX:
        case FORM_RNGLISTX:
            return {form, number: reader.u32()}
        case FORM_REF_ADDR:
            return {form, number: reader.u32(), refAbsolute: true}
        case FORM_REF1:
            return {form, number: reader.u8()}
        case FORM_REF2:
            return {form, number: reader.u16()}
        case FORM_REF8:
            return {form, number: reader.u64()}
        case FORM_REF_UDATA:
            return {form, number: reader.uleb()}
        case FORM_REF_SIG8:
            return {form, number: reader.u64()}
        case FORM_INDIRECT: {
            const realForm = reader.uleb()
            return readForm(reader, realForm, unit)
        }
        case FORM_EXPRLOC:
            return {form, block: reader.take(reader.uleb())}
        case FORM_IMPLICIT_CONST:
            return {form, number: implicit ?? 0}
        case FORM_STRX1:
            return {form: FORM_STRX, number: reader.u8()}
        case FORM_STRX2:
            return {form: FORM_STRX, number: reader.u16()}
        case FORM_STRX3:
            return {form: FORM_STRX, number: readUint(reader.take(3), 0, 3)}
        case FORM_STRX4:
            return {form: FORM_STRX, number: reader.u32()}
        case FORM_ADDRX1:
            return {form: FORM_ADDRX, number: reader.u8()}
        case FORM_ADDRX2:
            return {form: FORM_ADDRX, number: reader.u16()}
        case FORM_ADDRX3:
            return {form: FORM_ADDRX, number: readUint(reader.take(3), 0, 3)}
        case FORM_ADDRX4:
            return {form: FORM_ADDRX, number: reader.u32()}
        default:
            throw new DwarfParseError(`DWARF: unsupported form 0x${form.toString(16)}`)
    }
}

// ---------------------------------------------------------------------------
// 属性值解析（字符串 / 地址 / 引用）
// ---------------------------------------------------------------------------
function attrString(ctx: Ctx, unit: Unit, attr: AttrValue | undefined): string {
    if (!attr) return ''
    if (attr.string !== undefined) return attr.string
    if (attr.number === undefined) return ''
    if (attr.form === FORM_STRP) return readCString(ctx.sections.str, attr.number)
    if (attr.form === FORM_LINE_STRP && ctx.sections.lineStr) {
        return readCString(ctx.sections.lineStr, attr.number)
    }
    if (attr.form === FORM_STRX && ctx.sections.strOffsets) {
        const index = attr.number
        const offset = unit.strOffsetsBase + index * 4
        if (offset + 4 > ctx.sections.strOffsets.length) return ''
        const stringOffset = readUint(ctx.sections.strOffsets, offset, 4)
        return readCString(ctx.sections.str, stringOffset)
    }
    return ''
}

function readCString(bytes: Uint8Array, offset: number): string {
    if (offset < 0 || offset >= bytes.length) return ''
    let end = offset
    while (end < bytes.length && bytes[end] !== 0) end++
    if (end - offset > 4096) return ''
    return decodeUtf8(bytes.subarray(offset, end))
}

function attrAddress(ctx: Ctx, unit: Unit, attr: AttrValue | undefined): number | null {
    if (!attr) return null
    if (attr.form === FORM_ADDR && attr.number !== undefined) return attr.number >>> 0
    if (attr.form === FORM_ADDRX && ctx.sections.addr && attr.number !== undefined) {
        const offset = unit.addrBase + attr.number * unit.addrSize
        if (offset + unit.addrSize > ctx.sections.addr.length) return null
        return readUint(ctx.sections.addr, offset, unit.addrSize)
    }
    return null
}

/** 解析 DW_AT_location 中的固定地址：DW_OP_addr / DW_OP_addrx / GNU_addr_index。 */
function fixedAddressFromBlock(ctx: Ctx, unit: Unit, block: Uint8Array | undefined): number | null {
    if (!block || block.length === 0) return null
    const opcode = block[0]
    if (opcode === OP_ADDR) {
        if (block.length < 1 + unit.addrSize) return null
        return readUint(block, 1, unit.addrSize)
    }
    if (opcode === OP_ADDRX || opcode === OP_GNU_ADDR_INDEX) {
        if (!ctx.sections.addr) return null
        try {
            const reader = new ByteReader(block, 1)
            const index = reader.uleb()
            const offset = unit.addrBase + index * unit.addrSize
            if (offset + unit.addrSize > ctx.sections.addr.length) return null
            return readUint(ctx.sections.addr, offset, unit.addrSize)
        } catch {
            return null
        }
    }
    return null
}

function locationFixedAddress(ctx: Ctx, unit: Unit, attr: AttrValue | undefined): number | null {
    return fixedAddressFromBlock(ctx, unit, attr?.block)
}


// ---------------------------------------------------------------------------
// 调试采样位置：DW_AT_frame_base / DW_OP_fbreg / breg / reg
// ---------------------------------------------------------------------------
interface FrameBaseLocation {
    /** true = DW_OP_call_frame_cfa（用 SP=r13 近似） */
    cfa: boolean
    reg: number
    offset: number
}

function parseFrameBaseBlock(block: Uint8Array): FrameBaseLocation | null {
    if (!block || block.length === 0) return null
    const reader = new ByteReader(block)
    try {
        const opcode = reader.u8()
        let cfa = false
        let reg = 0
        let offset = 0
        if (opcode === OP_CALL_FRAME_CFA) {
            cfa = true
        } else if (opcode >= OP_REG0 && opcode <= OP_REG31) {
            reg = opcode - OP_REG0
        } else if (opcode >= OP_BREG0 && opcode <= OP_BREG31) {
            reg = opcode - OP_BREG0
            offset = reader.sleb()
        } else if (opcode === OP_REGX) {
            reg = reader.uleb()
        } else if (opcode === OP_BREGX) {
            reg = reader.uleb()
            offset = reader.sleb()
        } else {
            return null
        }
        while (reader.remaining > 0) {
            const next = reader.u8()
            if (next === OP_PLUS_UCONST) offset += reader.uleb()
            else if (next === OP_STACK_VALUE || next === OP_NOP) break
            else return null
        }
        return {cfa, reg, offset}
    } catch {
        return null
    }
}

/** 最近的外层 DW_TAG_subprogram 的 DW_AT_frame_base（帧基址寄存器）。 */
function frameBaseForDIE(ctx: Ctx, die: Die): FrameBaseLocation | null {
    const seen = new Set<number>()
    let current: Die | null = die
    let guard = 0
    while (current && guard++ < 24 && !seen.has(current.offset)) {
        seen.add(current.offset)
        if (current.tag === TAG_SUBPROGRAM) {
            const attr = current.attrs.get(AT_FRAME_BASE)
            if (attr?.block) {
                const parsed = parseFrameBaseBlock(attr.block)
                if (parsed) return parsed
            }
            const specification = resolveReference(ctx, current.unit, current.attrs.get(AT_SPECIFICATION))
            if (specification?.tag === TAG_SUBPROGRAM) {
                const specAttr = specification.attrs.get(AT_FRAME_BASE)
                if (specAttr?.block) {
                    const parsed = parseFrameBaseBlock(specAttr.block)
                    if (parsed) return parsed
                }
            }
        }
        current = current.parent
    }
    return null
}

interface ParsedLocation {
    kind: 'fixed' | 'memory' | 'register' | 'none' | 'unsupported'
    address?: number
    expr?: AddressExpr
    register?: number
    reason?: string
    /** loclist 多段变体（仅 multi 时提供；首段同时作为 expr/address） */
    variants?: LocationVariant[]
    /** loclist 条目数（含 default；undefined = 非 loclist） */
    loclistEntries?: number
    /** 位置列表有多个段：当前仅使用第一段 */
    loclistMulti?: boolean
}

/** 解析 DW_AT_location：固定地址 / 帧+寄存器内存位置 / 寄存器值。 */
/** 解析 DWARF 表达式字节（非 loclist 递归：expr 内不会再出现 loclist offset）。 */
function parseLocationBlock(ctx: Ctx, die: Die, block: Uint8Array): ParsedLocation {
    const fixed = fixedAddressFromBlock(ctx, die.unit, block)
    if (fixed !== null) return {kind: 'fixed', address: fixed >>> 0}
    const reader = new ByteReader(block)
    try {
        const opcode = reader.u8()
        if (opcode === OP_FBREG) {
            const offset = reader.sleb()
            const frame = frameBaseForDIE(ctx, die)
            if (!frame) return {kind: 'unsupported', reason: 'missing DW_AT_frame_base'}
            const total = frame.offset + offset
            return frame.cfa
                ? {kind: 'memory', expr: {kind: 'cfa', offset: total}}
                : {kind: 'memory', expr: {kind: 'reg', reg: frame.reg, offset: total}}
        }
        if (opcode >= OP_BREG0 && opcode <= OP_BREG31) {
            return {kind: 'memory', expr: {kind: 'reg', reg: opcode - OP_BREG0, offset: reader.sleb()}}
        }
        if (opcode === OP_BREGX) {
            const reg = reader.uleb()
            return {kind: 'memory', expr: {kind: 'reg', reg, offset: reader.sleb()}}
        }
        if (opcode >= OP_REG0 && opcode <= OP_REG31) {
            return {kind: 'register', register: opcode - OP_REG0}
        }
        if (opcode === OP_REGX) {
            return {kind: 'register', register: reader.uleb()}
        }
        return {kind: 'unsupported', reason: `unsupported location opcode 0x${opcode.toString(16)}`}
    } catch {
        return {kind: 'unsupported', reason: 'malformed location expression'}
    }
}

/** 解析 DW_AT_location 的 loclist offset（DWARF4 .debug_loc / DWARF5 .debug_loclists）。 */
function locationListTarget(ctx: Ctx, unit: Unit, attr: AttrValue): {kind: 'debug_loc' | 'debug_loclists'; offset: number} | null {
    if (attr.number === undefined) return null
    if (attr.form === FORM_LOCLISTX) {
        const section = ctx.sections.loclists
        if (!section || unit.loclistsBase <= 0) return null
        const entrySize = unit.offsetSize
        const tableOffset = unit.loclistsBase + attr.number * entrySize
        if (tableOffset + entrySize > section.length) return null
        const relative = readUint(section, tableOffset, entrySize)
        return {kind: 'debug_loclists', offset: (unit.loclistsBase + relative) >>> 0}
    }
    if (unit.version >= 5 && ctx.sections.loclists) {
        return {kind: 'debug_loclists', offset: attr.number >>> 0}
    }
    if (ctx.sections.loc) return {kind: 'debug_loc', offset: attr.number >>> 0}
    if (ctx.sections.loclists) return {kind: 'debug_loclists', offset: attr.number >>> 0}
    return null
}

export interface LocationListSelection {
    /** 选中的条目：优先第一个 ranged entry，否则 default_location。 */
    entry: LocationListEntry
    /** ranged entry 数量 > 1（当前只使用第一段）。 */
    multi: boolean
    /** 有效条目总数（含 default）。 */
    total: number
    rangedCount: number
}

/**
 * loclist 条目选择规则（纯函数，便于 synthetic 单测）：
 *  - 只考虑 expr 非空的条目；
 *  - 优先第一个 address_range/offset_pair entry，否则 default_location；
 *  - ranged entry 多于 1 个时 multi=true（当前实现只使用第一段，Phase 1）。
 */
export function selectLocationListEntry(entries: readonly LocationListEntry[]): LocationListSelection | null {
    const usable = entries.filter((entry) => entry.expr.length > 0)
    const ranged = usable.filter((entry) => entry.kind !== 'default_location'
        && typeof entry.lowPc === 'number' && typeof entry.highPc === 'number')
    const defaults = usable.filter((entry) => entry.kind === 'default_location')
    const entry = ranged[0] ?? defaults[0]
    if (!entry) return null
    return {entry, multi: ranged.length > 1, total: usable.length, rangedCount: ranged.length}
}

/**
 * D 第一阶段 loclist 集成：解码 location list，单 entry/default 直接使用；
 * 多 entry 时记录 locationVariants（每段可解析的 AddressExpr），采样仍用第一段，
 * 并标 loclistMulti/ loclistEntries 供 UI 提示限制。
 */
function decodeListLocation(ctx: Ctx, die: Die, attr: AttrValue): ParsedLocation {
    const target = locationListTarget(ctx, die.unit, attr)
    if (!target) return {kind: 'unsupported', reason: 'location list (missing .debug_loc/.debug_loclists)'}
    const data = target.kind === 'debug_loc' ? ctx.sections.loc : ctx.sections.loclists
    if (!data) return {kind: 'unsupported', reason: 'location list (missing section)'}
    let decoded
    try {
        decoded = decodeLocationList({
            kind: target.kind,
            data,
            offset: target.offset,
            addrSize: die.unit.addrSize,
            addr: ctx.sections.addr,
            addrBase: die.unit.addrBase,
            baseAddress: die.unit.lowPc,
        })
    } catch {
        return {kind: 'unsupported', reason: 'location list decode failed'}
    }
    const selection = selectLocationListEntry(decoded.entries)
    if (!selection) return {kind: 'unsupported', reason: 'location list has no entries'}
    const entries = decoded.entries.filter((entry) => entry.expr.length > 0)
    const ranged = entries.filter((entry) => entry.kind !== 'default_location'
        && typeof entry.lowPc === 'number' && typeof entry.highPc === 'number')
    const candidates = ranged.length > 0 ? ranged : [selection.entry]
    const parsed: Array<{entry: typeof candidates[number]; location: ParsedLocation; variant?: LocationVariant}> = []
    for (const entry of candidates) {
        const location = parseLocationBlock(ctx, die, entry.expr)
        if (location.kind !== 'fixed' && location.kind !== 'memory' && location.kind !== 'register') continue
        const variant = typeof entry.lowPc === 'number' && typeof entry.highPc === 'number'
            ? {pcLow: entry.lowPc >>> 0, pcHigh: entry.highPc >>> 0, expr: location.kind === 'fixed'
                ? {kind: 'addr' as const, address: location.address!}
                : location.expr!}
            : undefined
        parsed.push({entry, location, variant})
    }
    if (parsed.length === 0) return {kind: 'unsupported', reason: 'location list expressions unsupported'}
    const chosen = parsed.find((item) => item.entry === selection.entry) ?? parsed[0]
    const result: ParsedLocation = {
        ...chosen.location,
        loclistEntries: selection.total,
        loclistMulti: selection.multi,
    }
    if (selection.multi) {
        result.variants = parsed
            .filter((item) => item.variant !== undefined && item.entry.kind !== 'default_location')
            .map((item) => item.variant as LocationVariant)
    }
    return result
}

/** 解析 DW_AT_location：普通 block / 固定地址 / 帧+寄存器位置 / loclist offset。 */
function parseLocationExpr(ctx: Ctx, die: Die, attr: AttrValue | undefined): ParsedLocation {
    if (!attr) return {kind: 'none'}
    if (attr.block && attr.block.length > 0) return parseLocationBlock(ctx, die, attr.block)
    if (attr.number !== undefined) return decodeListLocation(ctx, die, attr)
    return {kind: 'none'}
}

/** 沿 spec/origin 链找第一个可用 location（定义缺失时继承声明位置）。 */
function chainParsedLocation(ctx: Ctx, chain: readonly Die[]): ParsedLocation {
    let unsupported: ParsedLocation | null = null
    for (const candidate of chain) {
        const attr = candidate.attrs.get(AT_LOCATION)
        if (!attr) continue
        const parsed = parseLocationExpr(ctx, candidate, attr)
        if (parsed.kind === 'fixed' || parsed.kind === 'memory' || parsed.kind === 'register') return parsed
        if (parsed.kind === 'unsupported' && !unsupported) unsupported = parsed
    }
    return unsupported ?? {kind: 'none'}
}

/** 解析简单 DWARF 表达式里的常量偏移（成员偏移/位域用）。 */
function exprConstantOffset(block: Uint8Array): number | null {
    if (block.length === 0) return null
    try {
        const reader = new ByteReader(block)
        const stack: number[] = []
        const push = (value: number) => stack.push(value >>> 0)
        while (reader.remaining > 0) {
            const opcode = reader.u8()
            if (opcode >= OP_LIT0 && opcode <= OP_LIT31) {
                push(opcode - OP_LIT0)
            } else if (opcode === OP_CONSTU) {
                push(reader.uleb())
            } else if (opcode === OP_PLUS_UCONST) {
                const value = (stack.pop() ?? 0) + reader.uleb()
                push(value)
            } else if (opcode === OP_PLUS) {
                const right = stack.pop() ?? 0
                const left = stack.pop() ?? 0
                push(left + right)
            } else if (opcode === 0x08) {
                // DW_OP_const1u
                push(reader.u8())
            } else if (opcode === 0x09) {
                // DW_OP_const1s
                push(reader.u8())
            } else if (opcode === 0x0a) {
                // DW_OP_const2u
                push(reader.u16())
            } else if (opcode === 0x0b) {
                push(reader.u16())
            } else if (opcode === 0x0c) {
                // DW_OP_const4u
                push(reader.u32())
            } else if (opcode === 0x9f) {
                // DW_OP_stack_value
                break
            } else {
                return null
            }
        }
        return stack.length > 0 ? stack[stack.length - 1] >>> 0 : null
    } catch {
        return null
    }
}

function resolveReference(ctx: Ctx, unit: Unit, attr: AttrValue | undefined): Die | null {
    if (!attr || attr.number === undefined) return null
    if (attr.form === FORM_REF_ADDR) {
        return ctx.dies.get(attr.refAbsolute ? attr.number : unit.start + attr.number) ?? null
    }
    return ctx.dies.get(unit.start + attr.number) ?? null
}

// ---------------------------------------------------------------------------
// DIE 遍历
// ---------------------------------------------------------------------------
interface UnitHeader {
    start: number
    end: number
    version: number
    addrSize: number
    abbrevOffset: number
    dieOffset: number
    offsetSize: number
}

function parseUnitHeader(bytes: Uint8Array, offset: number): UnitHeader {
    const reader = new ByteReader(bytes, offset)
    const start = offset
    let offsetSize = 4
    let length = reader.u32()
    if (length === 0xffffffff) {
        offsetSize = 8
        length = reader.u64()
    }
    const version = reader.u16()
    let addrSize = 4
    let abbrevOffset = 0
    if (version >= 5) {
        const unitType = reader.u8()
        addrSize = reader.u8()
        abbrevOffset = offsetSize === 8 ? reader.u64() : reader.u32()
        if (unitType === 0x02 || unitType === 0x04 || unitType === 0x06) {
            // type / skeleton / split_type：跳过多余的 signature/offset
            reader.skip(8)
            if (offsetSize === 8) reader.u64()
            else reader.u32()
        }
    } else {
        abbrevOffset = offsetSize === 8 ? reader.u64() : reader.u32()
        addrSize = reader.u8()
    }
    const end = start + (offsetSize === 8 ? 12 : 4) + length
    if (end > bytes.length || end < start) throw new DwarfParseError('DWARF: unit length out of range')
    return {start, end, version, addrSize, abbrevOffset, dieOffset: reader.offset, offsetSize}
}

function walkUnit(
    ctx: Ctx,
    header: UnitHeader,
    abbrevCache: Map<number, Map<number, Abbrev>>,
): Unit {
    const unit: Unit = {
        start: header.start,
        end: header.end,
        version: header.version,
        addrSize: header.addrSize,
        abbrevOffset: header.abbrevOffset,
        dieOffset: header.dieOffset,
        strOffsetsBase: header.version >= 5 ? 8 : 0,
        addrBase: header.version >= 5 ? 8 : 0,
        compDir: '',
        stmtList: null,
        root: null,
        dies: [],
        lineFiles: null,
        lineFilesLoaded: false,
        declFileCache: new Map(),
        lowPc: 0,
        loclistsBase: 0,
        offsetSize: header.offsetSize,
    }
    let table = abbrevCache.get(header.abbrevOffset)
    if (!table) {
        table = parseAbbrevTable(ctx.sections.abbrev, header.abbrevOffset)
        abbrevCache.set(header.abbrevOffset, table)
    }
    const reader = new ByteReader(ctx.bytes, header.dieOffset, header.end)
    const stack: Die[] = []
    let guard = 0
    while (reader.offset < header.end) {
        if (++guard > 2_000_000) throw new DwarfParseError('DWARF: DIE walk exceeded guard')
        const dieOffset = reader.offset
        const code = reader.uleb()
        if (code === 0) {
            if (stack.length === 0) break
            stack.pop()
            continue
        }
        const abbrev = table.get(code)
        if (!abbrev) throw new DwarfParseError(`DWARF: unknown abbrev code ${code} at 0x${dieOffset.toString(16)}`)
        const parent = stack.length > 0 ? stack[stack.length - 1] : null
        const die: Die = {
            offset: dieOffset,
            tag: abbrev.tag,
            attrs: new Map(),
            parent,
            children: [],
            unit,
            declFile: parent?.declFile,
            declLine: parent?.declLine,
        }
        if (parent) parent.children.push(die)
        for (const spec of abbrev.attrs) {
            die.attrs.set(spec.name, readForm(reader, spec.form, unit, spec.implicit))
        }
        const declFile = numericAttr(die, AT_DECL_FILE)
        const declLine = numericAttr(die, AT_DECL_LINE)
        if (declFile !== null) die.declFile = declFile
        if (declLine !== null) die.declLine = declLine
        if (!parent && abbrev.tag === TAG_COMPILE_UNIT) {
            unit.root = die
            unit.compDir = attrString(ctx, unit, die.attrs.get(AT_COMP_DIR))
            const stmt = die.attrs.get(AT_STMT_LIST)
            if (stmt && stmt.number !== undefined) unit.stmtList = stmt.number
            const strBase = numericAttr(die, AT_STR_OFFSETS_BASE)
            if (strBase !== null) unit.strOffsetsBase = strBase
            const addrBase = numericAttr(die, AT_ADDR_BASE)
            if (addrBase !== null) unit.addrBase = addrBase
            const loclistsBase = numericAttr(die, AT_LOCLISTS_BASE)
            if (loclistsBase !== null) unit.loclistsBase = loclistsBase
            const lowPc = attrAddress(ctx, unit, die.attrs.get(AT_LOW_PC))
            if (lowPc !== null) unit.lowPc = lowPc >>> 0
        }
        ctx.dies.set(die.offset, die)
        unit.dies.push(die)
        if (abbrev.children) stack.push(die)
    }
    return unit
}

function numericAttr(die: Die, name: number): number | null {
    const attr = die.attrs.get(name)
    if (!attr || attr.number === undefined) return null
    return attr.number
}

// ---------------------------------------------------------------------------
// .debug_line 文件表（decl_file -> 路径）
// ---------------------------------------------------------------------------
interface LineEntryFormat {
    content: number
    form: number
}

function readLineFormString(ctx: Ctx, reader: ByteReader, form: number, unit: Unit): string {
    switch (form) {
        case FORM_STRING:
            return reader.cstring()
        case FORM_STRP:
            return readCString(ctx.sections.str, reader.u32())
        case FORM_LINE_STRP:
            return ctx.sections.lineStr ? readCString(ctx.sections.lineStr, reader.u32()) : ''
        case FORM_STRX:
        case FORM_STRX1:
        case FORM_STRX2:
        case FORM_STRX3:
        case FORM_STRX4: {
            const attr = readForm(reader, form, unit)
            return attrString(ctx, unit, attr)
        }
        default:
            // 其它类型（udata/data*）不是字符串：消费后返回空
            readForm(reader, form, {addrSize: 4} as Unit)
            return ''
    }
}

function readLineFormNumber(reader: ByteReader, form: number, unit: Unit): number {
    switch (form) {
        case FORM_UDATA:
        case FORM_DATA1:
        case FORM_DATA2:
        case FORM_DATA4:
        case FORM_DATA8:
        case FORM_SDATA: {
            const attr = readForm(reader, form, unit)
            return attr.number ?? 0
        }
        case FORM_STRING:
            reader.cstring()
            return 0
        default: {
            const attr = readForm(reader, form, unit)
            return attr.number ?? 0
        }
    }
}

function parseLineFiles(ctx: Ctx, offset: number, unit: Unit): string[] | null {
    const line = ctx.sections.line
    if (!line) return null
    try {
        const reader = new ByteReader(line, offset)
        let offsetSize = 4
        let unitLength = reader.u32()
        if (unitLength === 0xffffffff) {
            offsetSize = 8
            unitLength = reader.u64()
        }
        const unitEnd = Math.min(line.length, offset + offsetSize + unitLength)
        const version = reader.u16()
        let headerLength = 0
        if (version >= 5) {
            reader.u8() // address_size
            reader.u8() // segment_selector_size
            headerLength = offsetSize === 8 ? reader.u64() : reader.u32()
        } else {
            headerLength = offsetSize === 8 ? reader.u64() : reader.u32()
        }
        const headerEnd = reader.offset + headerLength
        if (headerEnd > unitEnd) return null
        reader.u8() // min_inst_length
        if (version >= 4) reader.u8() // max_ops
        reader.u8() // default_is_stmt
        reader.u8() // line_base
        reader.u8() // line_range
        const opcodeBase = reader.u8()
        reader.skip(Math.max(0, opcodeBase - 1))
        const directories: string[] = []
        const files: string[] = []
        if (version >= 5) {
            // 目录表
            const dirFormatCount = reader.u8()
            const dirFormats: LineEntryFormat[] = []
            for (let i = 0; i < dirFormatCount; i++) {
                dirFormats.push({content: reader.uleb(), form: reader.uleb()})
            }
            const dirCount = reader.uleb()
            for (let i = 0; i < dirCount; i++) {
                let path = ''
                for (const format of dirFormats) {
                    if (format.content === 1) path = readLineFormString(ctx, reader, format.form, unit)
                    else readLineFormNumber(reader, format.form, unit)
                }
                directories.push(path)
            }
            // 文件表
            const fileFormatCount = reader.u8()
            const fileFormats: LineEntryFormat[] = []
            for (let i = 0; i < fileFormatCount; i++) {
                fileFormats.push({content: reader.uleb(), form: reader.uleb()})
            }
            const fileCount = reader.uleb()
            for (let i = 0; i < fileCount; i++) {
                let path = ''
                let dirIndex = 0
                for (const format of fileFormats) {
                    if (format.content === 1) path = readLineFormString(ctx, reader, format.form, unit)
                    else if (format.content === 2) dirIndex = readLineFormNumber(reader, format.form, unit)
                    else readLineFormNumber(reader, format.form, unit)
                }
                files.push(joinLinePath('', dirIndex, path, directories))
            }
        } else {
            for (;;) {
                const dir = reader.cstring()
                if (!dir) break
                directories.push(dir)
                if (directories.length > 10000) return null
            }
            for (;;) {
                const name = reader.cstring()
                if (!name) break
                const dirIndex = reader.uleb()
                reader.uleb() // mtime
                reader.uleb() // size
                files.push(joinLinePath('', dirIndex, name, directories))
                if (files.length > 100000) return null
            }
        }
        return files
    } catch {
        return null
    }
}

function joinLinePath(_prefix: string, dirIndex: number, name: string, directories: readonly string[]): string {
    if (!name) return ''
    const normalized = name.replace(/\\/g, '/')
    if (/^([A-Za-z]:\/|\/)/.test(normalized)) return normalized
    const dir = dirIndex > 0 ? directories[dirIndex - 1] ?? '' : ''
    const normalizedDir = dir.replace(/\\/g, '/')
    if (!normalizedDir) return normalized
    return `${normalizedDir.replace(/\/+$/, '')}/${normalized}`
}

function resolveDeclFile(ctx: Ctx, unit: Unit, index: number): string | undefined {
    if (index <= 0) return undefined
    const cached = unit.declFileCache.get(index)
    if (cached !== undefined) return cached || undefined
    if (!unit.lineFilesLoaded) {
        unit.lineFilesLoaded = true
        unit.lineFiles = unit.stmtList !== null ? parseLineFiles(ctx, unit.stmtList, unit) : null
    }
    const files = unit.lineFiles
    if (!files) {
        unit.declFileCache.set(index, '')
        return undefined
    }
    const file = files[index - 1] ?? ''
    // 相对路径补上 compile unit 的 comp_dir（若 line 表里本身没有目录）。
    let result = file
    if (result && unit.compDir && !/^([A-Za-z]:\/|\/)/.test(result)) {
        const normalizedCompDir = unit.compDir.replace(/\\/g, '/')
        result = `${normalizedCompDir.replace(/\/+$/, '')}/${result}`
    }
    unit.declFileCache.set(index, result)
    return result || undefined
}

// ---------------------------------------------------------------------------
// 类型求解
// ---------------------------------------------------------------------------
interface MemberInfo {
    name: string
    type: TypeInfo
    byteOffset: number
    /** 位域：相对该字节的位偏移（0..7） */
    bitOffset: number
    bitSize: number
    anonymous: boolean
}

interface TypeInfo {
    display: string
    size: number
    kind: 'base' | 'typedef' | 'qualifier' | 'pointer' | 'reference' | 'array' | 'struct' | 'union' | 'enum' | 'function' | 'unknown'
    baseEncoding?: number
    isFloat?: boolean
    isSigned?: boolean
    isBool?: boolean
    enumValues?: Array<[number, string]>
    /** 数组：元素类型（含内层维度） */
    element?: TypeInfo
    /** 数组各维长度（外层在前；null = 未知） */
    dimensions?: Array<number | null>
    /** 数组：去掉维度后的基础类型显示（如 "float"） */
    arrayBaseDisplay?: string
    /** 数组成员步长 */
    elementSize?: number
    members?: MemberInfo[]
    /** typedef 别名（用于显示） */
    typedefName?: string
    /** 基础类型名（用于列表/自动格式） */
    baseName?: string
}

const UNKNOWN_TYPE: TypeInfo = {display: '?', size: 0, kind: 'unknown'}

function typeDisplayOfBase(encoding: number, name: string, size: number): string {
    if (name) return name
    switch (encoding) {
        case DW_ATE_ADDRESS:
            return 'void *'
        case DW_ATE_BOOLEAN:
            return 'bool'
        case DW_ATE_FLOAT:
            return size === 8 ? 'double' : 'float'
        case DW_ATE_COMPLEX_FLOAT:
            return size === 8 ? 'float _Complex' : 'double _Complex'
        case DW_ATE_SIGNED:
        case DW_ATE_SIGNED_CHAR:
            return size === 1 ? 'signed char' : size === 2 ? 'short' : size === 8 ? 'long long' : 'int'
        case DW_ATE_UNSIGNED:
        case DW_ATE_UNSIGNED_CHAR:
            return size === 1 ? 'unsigned char' : size === 2 ? 'unsigned short' : size === 8 ? 'unsigned long long' : 'unsigned int'
        case DW_ATE_SIGNED_FIXED:
            return '_Fixed'
        case DW_ATE_UNSIGNED_FIXED:
            return '_Accum'
        default:
            return '?'
    }
}

function qualifiedDieName(ctx: Ctx, die: Die): string {
    const own = attrString(ctx, die.unit, die.attrs.get(AT_NAME))
    const scopes: string[] = []
    let parent = die.parent
    let guard = 0
    while (parent && guard++ < 16) {
        if (
            parent.tag === TAG_NAMESPACE ||
            parent.tag === TAG_CLASS_TYPE ||
            parent.tag === TAG_STRUCTURE_TYPE ||
            parent.tag === TAG_UNION_TYPE
        ) {
            const name = attrString(ctx, parent.unit, parent.attrs.get(AT_NAME))
            if (name) scopes.unshift(name)
        }
        parent = parent.parent
    }
    if (!own) return scopes.join('::')
    return scopes.length > 0 ? `${scopes.join('::')}::${own}` : own
}

function resolveType(ctx: Ctx, ref: number | null, depth = 0, visited?: Set<number>): TypeInfo {
    if (ref === null) return UNKNOWN_TYPE
    if (depth > 16) return UNKNOWN_TYPE
    const die = ctx.dies.get(ref)
    if (!die) return UNKNOWN_TYPE
    if (!visited) visited = new Set()
    if (visited.has(ref)) {
        // 递归类型（如 struct Node { Node *next; }）：给出类型名占位，避免 opaque。
        const recursiveName = qualifiedDieName(ctx, die) || attrString(ctx, die.unit, die.attrs.get(AT_NAME))
        return recursiveName
            ? {display: recursiveName, size: numericAttr(die, AT_BYTE_SIZE) ?? 0, kind: 'unknown'}
            : {display: 'opaque', size: 0, kind: 'unknown'}
    }
    visited.add(ref)
    try {
        return resolveTypeDie(ctx, die, depth, visited)
    } finally {
        visited.delete(ref)
    }
}

function typeRefOf(die: Die): number | null {
    const attr = die.attrs.get(AT_TYPE)
    if (!attr || attr.number === undefined) return null
    if (attr.form === FORM_REF_ADDR) return attr.refAbsolute ? attr.number : die.unit.start + attr.number
    return die.unit.start + attr.number
}

function underlyingBase(type: TypeInfo): TypeInfo {
    let current: TypeInfo = type
    let guard = 0
    while (guard++ < 12) {
        if (current.kind === 'base') return current
        if (current.kind === 'typedef' || current.kind === 'qualifier') {
            if (!current.element) return current
            current = current.element
            continue
        }
        break
    }
    return current
}

/** 去掉 typedef / const / volatile 等包装，拿到真实类型（聚合/数组判断用）。 */
function unwrapType(type: TypeInfo): TypeInfo {
    let current: TypeInfo = type
    let guard = 0
    while (guard++ < 12) {
        if (current.kind === 'typedef' || current.kind === 'qualifier') {
            if (!current.element) return current
            current = current.element
            continue
        }
        return current
    }
    return current
}

function resolveTypeDie(ctx: Ctx, die: Die, depth: number, visited: Set<number>): TypeInfo {
    const unit = die.unit
    switch (die.tag) {
        case TAG_BASE_TYPE: {
            const encoding = numericAttr(die, AT_ENCODING) ?? 0
            const size = numericAttr(die, AT_BYTE_SIZE) ?? 0
            const name = attrString(ctx, unit, die.attrs.get(AT_NAME)) || typeDisplayOfBase(encoding, '', size)
            return {
                display: name,
                size,
                kind: 'base',
                baseEncoding: encoding,
                baseName: name,
                isFloat: encoding === DW_ATE_FLOAT || encoding === DW_ATE_COMPLEX_FLOAT || encoding === DW_ATE_IMAGINARY_FLOAT,
                isSigned:
                    encoding === DW_ATE_SIGNED ||
                    encoding === DW_ATE_SIGNED_CHAR ||
                    encoding === DW_ATE_SIGNED_FIXED,
                isBool: encoding === DW_ATE_BOOLEAN,
            }
        }
        case TAG_TYPEDEF: {
            const name = attrString(ctx, unit, die.attrs.get(AT_NAME))
            const inner = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const base = underlyingBase(inner)
            return {
                display: name || inner.display,
                size: inner.size,
                kind: 'typedef',
                element: inner,
                typedefName: name,
                baseEncoding: base.baseEncoding,
                isFloat: base.isFloat,
                isSigned: base.isSigned,
                isBool: base.isBool,
                enumValues: inner.enumValues,
                baseName: base.baseName,
            }
        }
        case TAG_CONST_TYPE:
        case TAG_VOLATILE_TYPE:
        case TAG_RESTRICT_TYPE:
        case TAG_ATOMIC_TYPE:
        case TAG_PACKED_TYPE: {
            const inner = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const prefix =
                die.tag === TAG_CONST_TYPE
                    ? 'const '
                    : die.tag === TAG_VOLATILE_TYPE
                      ? 'volatile '
                      : die.tag === TAG_RESTRICT_TYPE
                        ? 'restrict '
                        : die.tag === TAG_ATOMIC_TYPE
                          ? '_Atomic '
                          : ''
            const base = underlyingBase(inner)
            return {
                display: prefix + inner.display,
                size: inner.size,
                kind: 'qualifier',
                element: inner,
                baseEncoding: base.baseEncoding,
                isFloat: base.isFloat,
                isSigned: base.isSigned,
                isBool: base.isBool,
                enumValues: inner.enumValues,
                baseName: base.baseName,
            }
        }
        case TAG_POINTER_TYPE: {
            const pointee = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const display =
                pointee.kind === 'function'
                    ? pointee.display
                    : pointee.display.endsWith(' *') || pointee.display.endsWith(' &')
                      ? `${pointee.display}*`
                      : `${pointee.display} *`
            return {
                display,
                size: unit.addrSize,
                kind: 'pointer',
                element: pointee,
                baseEncoding: DW_ATE_UNSIGNED,
                isSigned: false,
                baseName: 'pointer',
            }
        }
        case TAG_REFERENCE_TYPE:
        case TAG_RVALUE_REFERENCE_TYPE: {
            const target = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const suffix = die.tag === TAG_REFERENCE_TYPE ? ' &' : ' &&'
            return {display: `${target.display}${suffix}`, size: unit.addrSize, kind: 'reference', element: target, isSigned: false}
        }
        case TAG_PTR_TO_MEMBER_TYPE: {
            const target = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            return {display: `${target.display} Class::*`, size: unit.addrSize, kind: 'pointer', element: target, isSigned: false}
        }
        case TAG_ARRAY_TYPE: {
            const element = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const ownDims: Array<number | null> = []
            for (const child of die.children) {
                if (child.tag !== TAG_SUBRANGE_TYPE) continue
                const count = numericAttr(child, AT_COUNT)
                const upper = numericAttr(child, AT_UPPER_BOUND)
                const lower = numericAttr(child, AT_LOWER_BOUND)
                if (count !== null) ownDims.push(count)
                else if (upper !== null) ownDims.push(upper - (lower ?? 0) + 1)
                else ownDims.push(null)
            }
            if (ownDims.length === 0) ownDims.push(null)
            const innerDims = element.kind === 'array' ? element.dimensions ?? [] : []
            const arrayBaseDisplay = element.kind === 'array' ? element.arrayBaseDisplay ?? element.display : element.display
            const dimensions = [...ownDims, ...innerDims]
            const elementSize = element.kind === 'array' ? element.elementSize ?? 0 : element.size
            let total = elementSize
            for (const dim of dimensions) {
                if (dim === null || dim <= 0) {
                    total = 0
                    break
                }
                total *= dim
            }
            const display =
                arrayBaseDisplay + dimensions.map((dim) => (dim === null ? '[]' : `[${dim}]`)).join('')
            return {
                display,
                size: total,
                kind: 'array',
                element,
                dimensions,
                arrayBaseDisplay,
                elementSize: elementSize || (element.kind === 'array' ? element.elementSize : 0),
                baseEncoding: element.baseEncoding,
                isFloat: element.isFloat,
                isSigned: element.isSigned,
                isBool: element.isBool,
            }
        }
        case TAG_STRUCTURE_TYPE:
        case TAG_CLASS_TYPE:
        case TAG_UNION_TYPE: {
            const name = qualifiedDieName(ctx, die)
            const size = numericAttr(die, AT_BYTE_SIZE) ?? 0
            const members: MemberInfo[] = []
            for (const child of die.children) {
                if (child.tag === TAG_MEMBER) {
                    const member = resolveMember(ctx, child, depth, visited)
                    if (member) members.push(member)
                } else if (child.tag === TAG_INHERITANCE) {
                    const baseType = resolveType(ctx, typeRefOf(child), depth + 1, visited)
                    const location = numericAttr(child, AT_DATA_MEMBER_LOCATION) ?? 0
                    members.push({
                        name: '',
                        type: baseType,
                        byteOffset: location,
                        bitOffset: 0,
                        bitSize: 0,
                        anonymous: true,
                    })
                }
            }
            const completing = ctx.completing ?? (ctx.completing = new Set<number>())
            const bareName = attrString(ctx, die.unit, die.attrs.get(AT_NAME))
            const declared = numericAttr(die, AT_DECLARATION) === 1
            if (
                members.length === 0 &&
                declared &&
                bareName &&
                ctx.aggregateDefs &&
                !completing.has(die.offset)
            ) {
                // 只补全“命名的前向声明”（跨 CU 常见）；匿名空结构不做补全，
                // 否则会在大 ELF 上触发组合爆炸。
                const candidateSet = new Set<Die>()
                for (const key of [name || '', bareName]) {
                    for (const candidate of (ctx.aggregateDefs.get(key) ?? []).slice(0, 8)) {
                        candidateSet.add(candidate)
                    }
                }
                // 同名 DIE 可能包含前向声明/局部特化/嵌套定义；优先选成员最多、
                // 字节大小最大的完整定义，避免补全成同名的小声明（例如只含 rot_mat
                // 的 Rotation 片段）。
                const ordered = [...candidateSet]
                    .filter((candidate) => candidate.offset !== die.offset)
                    .sort((left, right) => {
                        const byMembers = right.children.length - left.children.length
                        if (byMembers !== 0) return byMembers
                        const leftSize = numericAttr(left, AT_BYTE_SIZE) ?? 0
                        const rightSize = numericAttr(right, AT_BYTE_SIZE) ?? 0
                        return rightSize - leftSize
                    })
                completing.add(die.offset)
                try {
                    for (const candidate of ordered) {
                        const completed = resolveTypeDie(ctx, candidate, depth + 1, visited)
                        if (completed.kind === 'struct' || completed.kind === 'union') {
                            if ((completed.members?.length ?? 0) > 0) {
                                return {
                                    ...completed,
                                    display: name || completed.display,
                                    size: size || completed.size,
                                }
                            }
                        }
                    }
                } finally {
                    completing.delete(die.offset)
                }
            }
            return {
                display: name || (die.tag === TAG_UNION_TYPE ? '(anonymous union)' : '(anonymous struct)'),
                size,
                kind: die.tag === TAG_UNION_TYPE ? 'union' : 'struct',
                members,
            }
        }
        case TAG_ENUMERATION_TYPE: {
            const name = qualifiedDieName(ctx, die)
            let size = numericAttr(die, AT_BYTE_SIZE) ?? 0
            const values: Array<[number, string]> = []
            for (const child of die.children) {
                if (child.tag !== TAG_ENUMERATOR) continue
                const value = numericAttr(child, AT_CONST_VALUE)
                const enumName = attrString(ctx, unit, child.attrs.get(AT_NAME))
                if (value !== null) values.push([value, enumName])
            }
            values.sort((left, right) => left[0] - right[0])
            if (size === 0) {
                const base = resolveType(ctx, typeRefOf(die), depth + 1, visited)
                size = base.size || 4
            }
            return {
                display: name || '(anonymous enum)',
                size,
                kind: 'enum',
                enumValues: values,
                isSigned: true,
                baseEncoding: DW_ATE_SIGNED,
            }
        }
        case TAG_SUBROUTINE_TYPE: {
            const returnType = resolveType(ctx, typeRefOf(die), depth + 1, visited)
            const params: string[] = []
            for (const child of die.children) {
                if (child.tag !== TAG_FORMAL_PARAMETER) continue
                const paramType = resolveType(ctx, typeRefOf(child), depth + 1, visited)
                const paramName = attrString(ctx, unit, child.attrs.get(AT_NAME))
                params.push(paramName ? `${paramType.display} ${paramName}` : paramType.display)
            }
            return {
                display: `${returnType.display === '?' ? 'void' : returnType.display} (*)(${params.join(', ')})`,
                size: unit.addrSize,
                kind: 'function',
                isSigned: false,
            }
        }
        case TAG_UNSPECIFIED_TYPE:
            return {display: 'void', size: 0, kind: 'unknown'}
        default:
            return UNKNOWN_TYPE
    }
}

function resolveMember(ctx: Ctx, die: Die, depth: number, visited: Set<number>): MemberInfo | null {
    if (numericAttr(die, AT_DECLARATION) === 1) return null
    const name = attrString(ctx, die.unit, die.attrs.get(AT_NAME))
    const type = resolveType(ctx, typeRefOf(die), depth + 1, visited)
    const bitSize = numericAttr(die, AT_BIT_SIZE)
    const dataBitOffset = numericAttr(die, AT_DATA_BIT_OFFSET)
    let byteOffset = 0
    let bitOffset = 0
    if (bitSize !== null && dataBitOffset !== null) {
        byteOffset = Math.floor(dataBitOffset / 8)
        bitOffset = dataBitOffset % 8
    } else {
        const location = die.attrs.get(AT_DATA_MEMBER_LOCATION)
        if (location) {
            if (location.number !== undefined) byteOffset = location.number >>> 0
            else if (location.block) {
                const constant = exprConstantOffset(location.block)
                if (constant !== null) byteOffset = constant
            }
        }
        if (bitSize !== null) {
            const legacyBitOffset = numericAttr(die, AT_BIT_OFFSET)
            if (legacyBitOffset !== null) {
                // DWARF2/3 的 bit_offset 从存储单元最高位算起，且无 storage size
                // 信息时无法安全换算；这里保守地按整字（0 位偏移）处理。
                bitOffset = 0
            }
        }
    }
    return {
        name,
        type,
        byteOffset,
        bitOffset,
        bitSize: bitSize ?? 0,
        anonymous: !name,
    }
}

// ---------------------------------------------------------------------------
// 变量 -> 可采样叶子 / 展开树
// ---------------------------------------------------------------------------
function scalarKind(type: TypeInfo): {kind: ValueKind; signed: boolean} {
    const base = underlyingBase(type)
    if (base.isBool) return {kind: 'bool', signed: false}
    if (base.isFloat) return {kind: base.size === 8 ? 'float64' : 'float32', signed: true}
    if (base.kind === 'enum' || (type.enumValues && type.enumValues.length > 0)) {
        return {kind: 'enum', signed: true}
    }
    if (base.isSigned) return {kind: 'int', signed: true}
    return {kind: 'uint', signed: false}
}

function enumValuesOf(type: TypeInfo): Array<[number, string]> | undefined {
    let current: TypeInfo = type
    let guard = 0
    while (guard++ < 8) {
        if (current.enumValues) return current.enumValues
        if (!current.element) return undefined
        current = current.element
    }
    return current.enumValues
}

function makeLeaf(type: TypeInfo, address: number, bitOffset = 0, bitSize = 0): SampleLeaf | undefined {
    const resolved = unwrapType(type)
    if (resolved.kind === 'struct' || resolved.kind === 'union' || resolved.kind === 'array' || resolved.kind === 'function') {
        // 聚合类型：读前 8 字节，按低 32 位绘图/读数（页面提示为 composed/hex）。
        return {
            address: address >>> 0,
            size: Math.min(Math.max(resolved.size || type.size, 1), 8),
            bitOffset: 0,
            bitSize: 0,
            kind: 'raw',
            signed: false,
        }
    }
    const {kind, signed} = scalarKind(type)
    const baseSize = type.size || underlyingBase(type).size || 4
    let size = Math.min(Math.max(baseSize, 1), 8)
    if (bitSize > 0) {
        const bytesNeeded = Math.ceil((bitOffset + bitSize) / 8)
        size = Math.min(Math.max(bytesNeeded, 1), 8)
        bitSize = Math.min(bitSize, 64)
    }
    const leaf: SampleLeaf = {
        address: address >>> 0,
        size,
        bitOffset: bitOffset % 8,
        bitSize,
        kind,
        signed,
    }
    if (kind === 'enum') {
        leaf.enumValues = enumValuesOf(type)
        leaf.signed = true
    }
    return leaf
}

// ---------------------------------------------------------------------------
// PC 作用域：变量所属 subprogram 的 DW_AT_low_pc/high_pc（high 可为地址或长度）
// ---------------------------------------------------------------------------
interface PcRange {
    pcLow: number
    pcHigh: number
}

function parseSubprogramRange(ctx: Ctx, subprogram: Die, visited: Set<number>): PcRange | null {
    if (visited.has(subprogram.offset)) return null
    visited.add(subprogram.offset)
    const low = attrAddress(ctx, subprogram.unit, subprogram.attrs.get(AT_LOW_PC))
    if (low !== null) {
        const highAttr = subprogram.attrs.get(AT_HIGH_PC)
        let high: number | null = null
        if (highAttr) {
            // DWARF2/3：high_pc 可能是绝对地址；DWARF4+：常见为 code length 常量。
            const absolute = attrAddress(ctx, subprogram.unit, highAttr)
            if (absolute !== null) high = absolute >>> 0
            else if (highAttr.number !== undefined) high = (low + highAttr.number) >>> 0
        }
        if (high !== null && high > low) {
            const pcLow = low >>> 0
            const pcHigh = high >>> 0
            // 只接受 Flash 内的有效范围（函数代码段）。
            if (pcLow >= TARGET_FLASH_RANGE[0] && pcHigh <= TARGET_FLASH_RANGE[1]) {
                return {pcLow, pcHigh}
            }
        }
    }
    // 声明/定义分离：subprogram 的 spec/origin 可能才有 low_pc/high_pc。
    for (const attr of [AT_SPECIFICATION, AT_ABSTRACT_ORIGIN]) {
        const target = resolveReference(ctx, subprogram.unit, subprogram.attrs.get(attr))
        if (target && target.tag === TAG_SUBPROGRAM) {
            const inherited = parseSubprogramRange(ctx, target, visited)
            if (inherited) return inherited
        }
    }
    return null
}

function enclosingSubprogramRange(ctx: Ctx, die: Die): PcRange | null {
    let parent = die.parent
    let guard = 0
    while (parent && guard++ < 32) {
        if (parent.tag === TAG_SUBPROGRAM) {
            const range = parseSubprogramRange(ctx, parent, new Set())
            if (range) return range
        }
        parent = parent.parent
    }
    return null
}

/**
 * 变量的 PC 作用域：优先沿 DIE parent 链找 subprogram；若定义 DIE 处于内联/
 * 声明链中，再沿 spec/origin 链的候选 DIE 找其 subprogram 范围。
 */
function subprogramRangeForDIE(ctx: Ctx, die: Die): PcRange | null {
    const direct = enclosingSubprogramRange(ctx, die)
    if (direct) return direct
    const seen = new Set<number>([die.offset])
    for (const candidate of referenceChain(ctx, die)) {
        if (seen.has(candidate.offset)) continue
        seen.add(candidate.offset)
        if (candidate.tag === TAG_SUBPROGRAM) {
            const range = parseSubprogramRange(ctx, candidate, new Set())
            if (range) return range
            continue
        }
        const inherited = enclosingSubprogramRange(ctx, candidate)
        if (inherited) return inherited
    }
    return null
}

let nodeBudget = 0

/**
 * 节点位置：固定地址（实时静态展开）或调试位置表达式（帧/寄存器/指针链）。
 * payloadInRegister=true 表示 expr 的值本身就是变量值（DW_OP_reg），
 * 指针展开时不再从内存读取指针。
 */
interface BaseLocation {
    address: number | null
    expr: AddressExpr | null
    payloadInRegister?: boolean
}

function baseExprOf(base: BaseLocation): AddressExpr {
    if (base.expr) return base.expr
    return {kind: 'addr', address: base.address ?? 0}
}

function offsetBase(base: BaseLocation, offset: number): BaseLocation {
    if (base.address !== null) return {address: (base.address + offset) >>> 0, expr: null}
    return {address: null, expr: {kind: 'add', inner: baseExprOf(base), offset}, payloadInRegister: base.payloadInRegister}
}

/** 指针值所在位置的表达式：内存变量先读 u32 指针；寄存器变量直接用寄存器值。 */
function pointeeBase(base: BaseLocation, memberOffset: number): BaseLocation {
    if (base.payloadInRegister) {
        const expr = base.expr
        if (expr && expr.kind === 'reg') {
            return {address: null, expr: {kind: 'reg', reg: expr.reg, offset: expr.offset + memberOffset}, payloadInRegister: true}
        }
        return {address: null, expr: {kind: 'add', inner: baseExprOf(base), offset: memberOffset}, payloadInRegister: true}
    }
    return {address: null, expr: {kind: 'deref', inner: baseExprOf(base), offset: memberOffset}}
}

function buildNode(
    name: string,
    type: TypeInfo,
    base: BaseLocation,
    depth: number,
    bitOffset = 0,
    bitSize = 0,
    pointerDepth = 0,
    pcLow?: number,
    pcHigh?: number,
    loclistMulti?: boolean,
): VariableNode {
    const node: VariableNode = {
        name,
        typeName: type.display,
        address: base.address !== null ? base.address >>> 0 : 0,
        size: type.size,
    }
    if (pcLow !== undefined && pcHigh !== undefined) {
        node.pcLow = pcLow
        node.pcHigh = pcHigh
    }
    if (loclistMulti) node.loclistMulti = true
    if (base.address !== null) {
        node.sample = bitOffset > 0 || bitSize > 0
            ? makeLeaf(type, base.address, bitOffset, bitSize)
            : makeLeaf(type, base.address)
        node.sampleable = !!node.sample
        node.liveSamplable = node.sampleable
        node.samplingMode = 'live'
    } else if (base.expr) {
        node.location = base.expr
        node.sample = makeLeaf(type, 0, bitOffset, bitSize)
        if (node.sample) {
            node.sample.address = 0
            if (base.payloadInRegister && base.expr.kind === 'reg') node.sample.register = base.expr.reg
            else node.sample.location = base.expr
        }
        node.sampleable = !!node.sample
        node.liveSamplable = node.sampleable && addressExprIsLive(base.expr)
        node.samplingMode = node.liveSamplable ? 'live' : 'halt'
    } else {
        node.sampleable = false
    }
    if (pointerDepth > 0) node.pointerDepth = pointerDepth
    const resolved = unwrapType(type)
    if (resolved.kind === 'struct' || resolved.kind === 'union' || resolved.kind === 'array') {
        node.children = buildChildren(resolved, base, depth + 1, pcLow, pcHigh, loclistMulti)
    } else if (resolved.kind === 'pointer' && resolved.element) {
        node.children = buildPointeeChildren(resolved, base, depth + 1, 1, pcLow, pcHigh, loclistMulti)
    }
    return node
}

function buildChildren(type: TypeInfo, base: BaseLocation, depth: number, pcLow?: number, pcHigh?: number, loclistMulti?: boolean): VariableNode[] | undefined {
    type = unwrapType(type)
    if (depth > MAX_EXPAND_DEPTH || nodeBudget <= 0) return undefined
    const children: VariableNode[] = []
    if (type.kind === 'struct' || type.kind === 'union') {
        for (const member of type.members ?? []) {
            if (children.length >= MAX_CHILDREN || nodeBudget <= 0) break
            const memberBase = offsetBase(base, member.byteOffset)
            if (member.anonymous) {
                const nested = buildChildren(member.type, memberBase, depth, pcLow, pcHigh, loclistMulti)
                if (nested) {
                    for (const child of nested) {
                        if (children.length >= MAX_CHILDREN) break
                        children.push(child)
                        nodeBudget -= 1
                    }
                }
                continue
            }
            children.push(buildNode(`.${member.name}`, member.type, memberBase, depth, member.bitOffset, member.bitSize, 0, pcLow, pcHigh, loclistMulti))
            nodeBudget -= 1
        }
        return children.length > 0 ? children : undefined
    }
    if (type.kind === 'array') {
        const element = type.element
        const count = type.dimensions && type.dimensions.length > 0 ? type.dimensions[0] : null
        const stride = type.elementSize ?? element?.size ?? 0
        if (!element || count === null || count === undefined || count <= 0 || stride <= 0) return undefined
        const limit = Math.min(count, MAX_CHILDREN)
        for (let i = 0; i < limit; i++) {
            if (nodeBudget <= 0) break
            children.push(buildNode(`[${i}]`, element, offsetBase(base, i * stride), depth, 0, 0, 0, pcLow, pcHigh, loclistMulti))
            nodeBudget -= 1
        }
        return children.length > 0 ? children : undefined
    }
    return undefined
}

/** 指针节点：展开被指向的 struct/union 成员或数组元素（名字 `->member` / `->[i]`）。 */
function buildPointeeChildren(pointerType: TypeInfo, base: BaseLocation, depth: number, pointerDepth = 1, pcLow?: number, pcHigh?: number, loclistMulti?: boolean): VariableNode[] | undefined {
    if (depth > MAX_EXPAND_DEPTH + 1 || nodeBudget <= 0) return undefined
    const pointee = unwrapType(pointerType.element ?? UNKNOWN_TYPE)
    if (pointee.kind !== 'struct' && pointee.kind !== 'union' && pointee.kind !== 'array') return undefined
    const children: VariableNode[] = []
    if (pointee.kind === 'struct' || pointee.kind === 'union') {
        for (const member of pointee.members ?? []) {
            if (children.length >= MAX_CHILDREN || nodeBudget <= 0) break
            if (member.anonymous) {
                const nested = buildChildren(member.type, pointeeBase(base, member.byteOffset), depth, pcLow, pcHigh, loclistMulti)
                if (nested) {
                    for (const child of nested) {
                        if (children.length >= MAX_CHILDREN) break
                        children.push({...child, name: child.name.startsWith('.') ? `->${child.name.slice(1)}` : child.name})
                        nodeBudget -= 1
                    }
                }
                continue
            }
            children.push(buildNode(`->${member.name}`, member.type, pointeeBase(base, member.byteOffset), depth, 0, 0, pointerDepth, pcLow, pcHigh, loclistMulti))
            nodeBudget -= 1
        }
        return children.length > 0 ? children : undefined
    }
    const element = pointee.element
    const count = pointee.dimensions && pointee.dimensions.length > 0 ? pointee.dimensions[0] : null
    const stride = pointee.elementSize ?? element?.size ?? 0
    if (!element || count === null || count === undefined || count <= 0 || stride <= 0) return undefined
    for (let i = 0; i < Math.min(count, MAX_CHILDREN); i++) {
        if (nodeBudget <= 0) break
        children.push(buildNode(`->[${i}]`, element, pointeeBase(base, i * stride), depth, 0, 0, pointerDepth, pcLow, pcHigh, loclistMulti))
        nodeBudget -= 1
    }
    return children.length > 0 ? children : undefined
}

/** 统计指针展开出的可采样节点数量。 */
function countPointerNodes(node: VariableNode): number {
    let count = node.pointerDepth && node.sampleable ? 1 : 0
    for (const child of node.children ?? []) count += countPointerNodes(child)
    return count
}

// ---------------------------------------------------------------------------
// 变量收集：声明/定义链（DW_AT_specification / DW_AT_abstract_origin）
// ---------------------------------------------------------------------------
/**
 * 沿 DW_AT_specification / DW_AT_abstract_origin 递归收集 DIE 链（定义优先）。
 * C++ 类静态成员/函数内 static 局部变量的名字、类型、decl_file/line 常只在
 * 声明 DIE 上；某些实现也会把 location 放在被引用 DIE 上，因此统一沿链查找。
 */
function referenceChain(ctx: Ctx, die: Die): Die[] {
    const chain: Die[] = []
    const seen = new Set<number>()
    let current: Die | null = die
    let guard = 0
    while (current && guard++ < 16 && !seen.has(current.offset)) {
        seen.add(current.offset)
        chain.push(current)
        const specification = resolveReference(ctx, current.unit, current.attrs.get(AT_SPECIFICATION))
        const origin = resolveReference(ctx, current.unit, current.attrs.get(AT_ABSTRACT_ORIGIN))
        current = specification ?? origin
    }
    return chain
}

function chainAttrString(ctx: Ctx, chain: readonly Die[], name: number): string {
    for (const die of chain) {
        const value = attrString(ctx, die.unit, die.attrs.get(name))
        if (value) return value
    }
    return ''
}

/** 类型引用：每个候选 DIE 的 ref 是相对其所在 CU 的，必须用各自 unit 计算绝对偏移。 */
function chainTypeRef(chain: readonly Die[]): number | null {
    for (const die of chain) {
        const ref = typeRefOf(die)
        if (ref !== null) return ref
    }
    return null
}

/**
 * 位置：沿链找第一个 DW_AT_location。
 * address !== null = 固定地址；hasLocation=true 但 address=null = 寄存器/栈/loclist
 * （可列出但不可采样）。
 */
/** decl_file/line：定义 DIE 自身（含父作用域继承）优先，其次沿 spec/origin 链。 */
function chainDecl(ctx: Ctx, chain: readonly Die[]): {fileIndex?: number; unit?: Unit; line?: number} {
    for (const die of chain) {
        if (die.declFile !== undefined) return {fileIndex: die.declFile, unit: die.unit, line: die.declLine}
        if (die.declLine !== undefined) return {fileIndex: undefined, unit: die.unit, line: die.declLine}
    }
    return {}
}

/** 作用域链：namespace/class/struct/union + 最近的外层函数（用于 static 局部变量）。 */
function enclosingScope(ctx: Ctx, die: Die): string[] {
    const scopes: string[] = []
    let parent = die.parent
    let guard = 0
    while (parent && guard++ < 24) {
        if (
            parent.tag === TAG_NAMESPACE ||
            parent.tag === TAG_CLASS_TYPE ||
            parent.tag === TAG_STRUCTURE_TYPE ||
            parent.tag === TAG_UNION_TYPE ||
            parent.tag === TAG_SUBPROGRAM
        ) {
            const name = attrString(ctx, parent.unit, parent.attrs.get(AT_NAME))
            if (name) scopes.unshift(parent.tag === TAG_SUBPROGRAM ? `${name}()` : name)
        }
        parent = parent.parent
    }
    return scopes
}

interface VariableIdentity {
    shortName: string
    qualifiedName: string
    mangledName: string
    nameDie: Die | null
}

/** 源码名解析：DW_AT_name（沿 spec/origin 链）→ DW_AT_linkage_name 反混淆兜底。 */
function resolveVariableIdentity(ctx: Ctx, die: Die): VariableIdentity | null {
    const chain = referenceChain(ctx, die)
    let shortName = ''
    let nameDie: Die | null = null
    for (const candidate of chain) {
        const name = attrString(ctx, candidate.unit, candidate.attrs.get(AT_NAME))
        if (name) {
            shortName = name
            nameDie = candidate
            break
        }
    }
    const mangledName = chainAttrString(ctx, chain, AT_LINKAGE_NAME)
    let qualifiedFromLinkage = ''
    if (!shortName && mangledName) {
        const demangled = ctx.demangle ? ctx.demangle(mangledName) : null
        if (demangled) {
            qualifiedFromLinkage = demangled
            shortName = demangled.split('::').pop() ?? demangled
        } else {
            qualifiedFromLinkage = mangledName
            shortName = mangledName
        }
    }
    if (!shortName) return null
    let qualifiedName = shortName
    if (nameDie) {
        // 名字来自 spec/声明 DIE 时，作用域要用该 DIE 的父链（C++ 类静态成员）。
        const scopes = enclosingScope(ctx, nameDie)
        qualifiedName = scopes.length > 0 ? `${scopes.join('::')}::${shortName}` : shortName
    } else if (qualifiedFromLinkage) {
        qualifiedName = qualifiedFromLinkage
    }
    return {shortName, qualifiedName, mangledName, nameDie}
}

// ---------------------------------------------------------------------------
// 相对源文件路径（去掉项目/构建根前缀）
// ---------------------------------------------------------------------------
function normalizePath(file: string): string {
    return file.replace(/\\/g, '/')
}

/** 去掉 CU 的 comp_dir 前缀（如 /work、E:/.../build），得到相对源码路径。 */
function stripCompDirPrefix(file: string, compDir: string | undefined): string | null {
    if (!compDir) return null
    const normalizedFile = normalizePath(file)
    const normalizedDir = normalizePath(compDir).replace(/\/+$/, '')
    if (normalizedDir && normalizedFile.startsWith(`${normalizedDir}/`)) {
        return normalizedFile.slice(normalizedDir.length + 1)
    }
    return null
}

/** 解析路径中的 . / .. 段（DWARF 行表常给出相对目录）。 */
function normalizeSegments(file: string): string[] {
    const parts = normalizePath(file).split('/')
    const out: string[] = []
    for (const part of parts) {
        if (part === '' && out.length > 0) continue
        if (part === '.') continue
        if (part === '..') {
            if (out.length > 1) out.pop()
            continue
        }
        out.push(part)
    }
    return out
}

/** 源码根目录名（用于从构建树绝对路径中截出项目相对路径）。 */
const SOURCE_ROOT_SEGMENTS = new Set(['src', 'lib', 'hw', 'mcu', 'modules', 'app', 'bsp', 'core', 'drivers', 'components', 'user'])

/**
 * 计算相对源码路径：
 *  1) 先去掉 CU comp_dir 前缀（/work/build、E:/.../build 等）；
 *  2) 剩余绝对路径按“第一个 src/lib/hw/...”段截出项目相对路径
 *     （如 /work/src/a.c -> src/a.c）；工具链系统头再尝试公共前缀。
 */
function postProcessRelativeFiles(variables: readonly VariableNode[]): void {
    const unresolved: VariableNode[] = []
    for (const variable of variables) {
        if (!variable.file) continue
        const parts = normalizeSegments(variable.file)
        let rootIndex = -1
        for (let index = 1; index < parts.length - 1; index++) {
            if (SOURCE_ROOT_SEGMENTS.has(parts[index])) {
                rootIndex = index
                break
            }
        }
        if (rootIndex > 0) {
            variable.relativeFile = parts.slice(rootIndex).join('/')
        } else {
            unresolved.push(variable)
        }
    }
    for (const variable of variables) {
        if (variable.file && !variable.relativeFile) variable.relativeFile = normalizePath(variable.file)
    }
    if (unresolved.length === 0) return
    const files = unresolved.map((item) => normalizeSegments(item.file as string))
    const common: string[] = []
    const first = files[0]
    for (let index = 0; index < first.length - 1; index++) {
        const segment = first[index]
        if (files.every((parts) => parts.length > index + 1 && parts[index] === segment)) {
            common.push(segment)
        } else {
            break
        }
    }
    if (common.length === 0) return
    for (const variable of unresolved) {
        const parts = normalizeSegments(variable.file as string)
        variable.relativeFile = parts.slice(common.length).join('/')
    }
}

interface DwarfCollectResult {
    variables: VariableNode[]
    dwarfVariables: number
    dwarfNamed: number
    dwarfWithAddress: number
    dwarfSkipped: number
    dwarfLiveSamplable: number
    dwarfHaltSamplable: number
    dwarfPointerExpanded: number
    dwarfNoLocation: number
    units: number
    warnings: string[]
}

export function extractDwarfVariables(
    bytes: Uint8Array,
    sections: readonly ElfSectionInfo[],
    options: {demangle?: (mangled: string) => string | null} = {},
): DwarfCollectResult {
    const warnings: string[] = []
    const info = sectionBytes(bytes, sections, '.debug_info')
    const abbrev = sectionBytes(bytes, sections, '.debug_abbrev')
    const str = sectionBytes(bytes, sections, '.debug_str')
    if (!info || !abbrev || !str) {
        return {
            variables: [],
            dwarfVariables: 0,
            dwarfNamed: 0,
            dwarfWithAddress: 0,
            dwarfSkipped: 0,
            dwarfLiveSamplable: 0,
            dwarfHaltSamplable: 0,
            dwarfPointerExpanded: 0,
            dwarfNoLocation: 0,
            units: 0,
            warnings: ['DWARF: missing .debug_info/.debug_abbrev/.debug_str'],
        }
    }
    const ctx: Ctx = {
        bytes: info,
        sections: {
            info,
            abbrev,
            str,
            lineStr: sectionBytes(bytes, sections, '.debug_line_str'),
            strOffsets: sectionBytes(bytes, sections, '.debug_str_offsets'),
            addr: sectionBytes(bytes, sections, '.debug_addr'),
            line: sectionBytes(bytes, sections, '.debug_line'),
            loc: sectionBytes(bytes, sections, '.debug_loc'),
            loclists: sectionBytes(bytes, sections, '.debug_loclists'),
        },
        units: [],
        dies: new Map(),
        warning: (message: string) => {
            if (warnings.length < 20) warnings.push(message)
        },
        demangle: options.demangle,
    }

    const abbrevCache = new Map<number, Map<number, Abbrev>>()
    const units: Unit[] = []
    let offset = 0
    let guard = 0
    while (offset < ctx.bytes.length) {
        if (++guard > 20000) throw new DwarfParseError('DWARF: too many units')
        const header = parseUnitHeader(ctx.bytes, offset)
        const unit = walkUnit({...ctx, units}, header, abbrevCache)
        units.push(unit)
        ctx.units = units
        offset = header.end
    }
    ctx.units = units

    // 跨 CU 引用经常先命中前向声明（DW_AT_declaration，无成员）；按名字/index 建
    // 聚合类型定义表，解析声明时补全为完整定义（否则 target_debug_handle->bmi088_
    // 这类对象只能展开到成员本身，无法继续展开 accel/gyro）。
    const aggregateDefs = new Map<string, Die[]>()
    for (const unit of units) {
        for (const die of unit.dies) {
            if (
                die.tag !== TAG_STRUCTURE_TYPE &&
                die.tag !== TAG_CLASS_TYPE &&
                die.tag !== TAG_UNION_TYPE
            ) continue
            if (die.children.length === 0) continue
            const qualified = qualifiedDieName(ctx, die)
            const bare = attrString(ctx, die.unit, die.attrs.get(AT_NAME))
            for (const key of new Set([qualified, bare])) {
                if (!key) continue
                const list = aggregateDefs.get(key)
                if (list) list.push(die)
                else aggregateDefs.set(key, [die])
            }
        }
    }
    ctx.aggregateDefs = aggregateDefs

    const variables: VariableNode[] = []
    let dwarfVariables = 0
    let dwarfNamed = 0
    let dwarfWithAddress = 0
    let dwarfSkipped = 0
    let dwarfLiveSamplable = 0
    let dwarfHaltSamplable = 0
    let dwarfPointerExpanded = 0
    let dwarfNoLocation = 0

    /** 去重：同一变量可能同时存在声明/定义 DIE（spec 链）与内联副本。 */
    const byPlace = new Map<string, number>()
    const insertVariable = (node: VariableNode) => {
        // 调试帧变量要按限定名（含函数作用域）区分，避免同文件同行的不同函数局部变量被合并；
        // 固定地址/全局变量沿用短名键，保持与旧版列表数量一致。
        const nameKey = node.samplingMode === 'halt' && !node.liveSamplable
            ? node.qualifiedName ?? node.name
            : node.shortName ?? node.name
        const place = `${nameKey}|${node.relativeFile ?? node.file ?? ''}|${node.line ?? ''}`
        const existingIndex = byPlace.get(place)
        if (existingIndex !== undefined) {
            const existing = variables[existingIndex]
            // 声明（不可采样）与定义（可采样）冲突时保留可采样定义。
            if (node.sampleable && !existing.sampleable) variables[existingIndex] = node
            return
        }
        byPlace.set(place, variables.length)
        variables.push(node)
    }

    for (const unit of units) {
        for (const die of unit.dies) {
            if (die.tag !== TAG_VARIABLE) continue
            dwarfVariables += 1
            const identity = resolveVariableIdentity(ctx, die)
            if (!identity) {
                dwarfSkipped += 1
                continue
            }
            dwarfNamed += 1
            const chain = referenceChain(ctx, die)
            const location = chainParsedLocation(ctx, chain)
            if (location.kind === 'fixed') dwarfWithAddress += 1
            const type = resolveType(ctx, chainTypeRef(chain))
            const decl = chainDecl(ctx, chain)
            let file: string | undefined
            if (decl.fileIndex !== undefined && decl.unit) {
                file = resolveDeclFile(ctx, decl.unit, decl.fileIndex)
            }
            let base: BaseLocation | null = null
            if (location.kind === 'fixed' && location.address !== undefined) {
                const address = location.address >>> 0
                if (address !== 0 && isPlausibleTargetAddress(address)) {
                    base = {address, expr: null}
                }
            } else if (location.kind === 'memory' && location.expr) {
                base = {address: null, expr: location.expr}
            } else if (location.kind === 'register' && location.register !== undefined) {
                base = {address: null, expr: {kind: 'reg', reg: location.register, offset: 0}, payloadInRegister: true}
            }
            const pcRange = subprogramRangeForDIE(ctx, die)
            let node: VariableNode
            if (base) {
                nodeBudget = MAX_NODES_PER_VARIABLE
                node = buildNode(identity.qualifiedName, type, base, 0, 0, 0, 0, pcRange?.pcLow, pcRange?.pcHigh, location.loclistMulti)
                nodeBudget = 0
            } else {
                node = {
                    name: identity.qualifiedName,
                    typeName: type.display,
                    address: 0,
                    size: type.size,
                    sampleable: false,
                    pcLow: pcRange?.pcLow,
                    pcHigh: pcRange?.pcHigh,
                    loclistMulti: location.loclistMulti,
                }
            }
            node.locationVariants = location.variants
            node.loclistEntries = location.loclistEntries
            node.shortName = identity.shortName
            node.qualifiedName = identity.qualifiedName
            node.mangledName = identity.mangledName || undefined
            node.file = file
            node.relativeFile = file ? stripCompDirPrefix(file, decl.unit?.compDir) ?? undefined : undefined
            node.line = decl.line
            if (node.sampleable) {
                if (node.liveSamplable) dwarfLiveSamplable += 1
                else dwarfHaltSamplable += 1
            } else {
                dwarfNoLocation += 1
            }
            dwarfPointerExpanded += countPointerNodes(node)
            insertVariable(node)
        }
    }

    // 相对路径：优先去掉 CU comp_dir 前缀；剩余（工具链头文件）再尝试公共前缀。
    postProcessRelativeFiles(variables)

    variables.sort((left, right) => {
        const rank = (node: VariableNode) => (node.sampleable ? (node.liveSamplable ? 0 : 1) : 2)
        const delta = rank(left) - rank(right)
        if (delta !== 0) return delta
        return left.name.localeCompare(right.name) || left.address - right.address
    })

    return {
        variables,
        dwarfVariables,
        dwarfNamed,
        dwarfWithAddress,
        dwarfSkipped,
        dwarfLiveSamplable,
        dwarfHaltSamplable,
        dwarfPointerExpanded,
        dwarfNoLocation,
        units: units.length,
        warnings,
    }
}
