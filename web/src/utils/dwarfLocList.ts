/**
 * DWARF 位置列表（location list）独立解码模块。
 *
 * 支持两类互相独立的调试节：
 *  - `.debug_loc`（DWARF 2/3/4）：无节头；每个条目 = begin/end address +
 *    2 字节表达式长度 + 表达式字节。`begin` 为当前 address_size 的全 1 值时是
 *    base address selection entry（此时 `end` 为新 base），`(0, 0)` 为 terminator。
 *    普通条目的 begin/end 相对当前 base 偏移（base 初值 = CU 的 DW_AT_low_pc，
 *    通常为 0），解码结果统一换算成绝对地址。
 *  - `.debug_loclists`（DWARF 5）：每个 CU contribution 以 header 开头
 *    （unit_length / version / address_size / segment_selector_size /
 *    offset_entry_count(u32) / offset table），表项相对 header 末尾
 *    （即 DW_AT_loclists_base）偏移；列表本体由 1 字节 LLE opcode + 操作数组成，
 *    表达式长度用 ULEB128 编码。
 *
 * 本模块只做“字节 -> 地址区间 + 原始表达式字节”的纯函数解码：
 *  - 不依赖 DOM、不 import 项目内其它模块，可单独在 Node 里单测；
 *  - 不解析 DWARF 表达式语义（DW_OP_* 的求值留给 dwarfLocation/elfDwarf）；
 *  - 不读写文件/网络，输入输出均为普通对象，坏数据通过 `warnings` 上报而不抛异常
 *    （仅参数类型错误抛 TypeError / header 非法抛 LocListError）。
 *
 * 典型用法（集成见 docs/dwarf-loclist-notes.md）：
 *   const list = decodeLocationList({kind: 'debug_loc', data: debugLoc, offset: 0x1234,
 *                                    addrSize: 4, baseAddress: cuLowPc})
 *   const entry = findLocationListEntry(list.entries, pc)
 *   const exprBytes = entry?.expr
 *
 * 参考：DWARF 5 标准 §2.6.2/§7.28；LLVM DWARFListTable/DWARFDebugLoclists；
 * GNU binutils dwarf.c（readelf --debug-dump=loc 的判定规则）。
 */

/** DWARF 5 `.debug_loclists` 条目类型（DW_LLE_*）。 */
export const DW_LLE_END_OF_LIST = 0x00
export const DW_LLE_BASE_ADDRESSX = 0x01
export const DW_LLE_STARTX_ENDX = 0x02
export const DW_LLE_STARTX_LENGTH = 0x03
export const DW_LLE_OFFSET_PAIR = 0x04
export const DW_LLE_DEFAULT_LOCATION = 0x05
export const DW_LLE_BASE_ADDRESS = 0x06
export const DW_LLE_START_END = 0x07
export const DW_LLE_START_LENGTH = 0x08
/**
 * GNU 扩展条目：两个 ULEB128 view 编号。它只服务于 GNU location views
 * （通常由 DW_AT_GNU_locviews 引用、与列表并列存放在同一节中），
 * 对“取哪个地址区间的表达式”没有影响，因此解码时记录到 viewPairs、不产生 entry。
 */
export const DW_LLE_GNU_VIEW_PAIR = 0x09

/** 位置列表所在节。 */
export type LocListKind = 'debug_loc' | 'debug_loclists'

/** 单条位置列表条目（base selection / terminator / view pair 不进入 entries）。 */
export interface LocationListEntry {
    /** 在本次解码结果 entries 中的序号（从 0 开始）。 */
    index: number
    /**
     * 地址区间语义：
     *  - `address_range`：lowPc/highPc 已是绝对地址（debug_loc 普通条目、
     *    DWARF5 start_end/start_length/startx_* 等）；
     *  - `offset_pair`：DWARF5 offset_pair，已按当前 base 换算成绝对地址；
     *  - `default_location`：DW_LLE_default_location，没有区间，作用于列表中
     *    其它条目未覆盖的任意 PC。
     */
    kind: 'address_range' | 'offset_pair' | 'default_location'
    /** 绝对起始地址（含）；default_location 时为 undefined。 */
    lowPc?: number
    /** 绝对结束地址（不含，DWARF 半开区间）；default_location 时为 undefined。 */
    highPc?: number
    /** 原样保留的 DWARF 表达式字节（输入 data 的只读视图，调用方不得修改）。 */
    expr: Uint8Array
    /** 该条目在节内的起始偏移，便于诊断/测试。 */
    entryOffset: number
}

/** GNU location view pair（DW_LLE_GNU_view_pair 条目）。 */
export interface LocationViewPair {
    entryOffset: number
    beginView: number
    endView: number
}

export interface DecodedLocationList {
    kind: LocListKind
    /** 开始解码的节内偏移。 */
    startOffset: number
    /**
     * 解码结束后的节内偏移：正常情况指向 terminator 之后；
     * 数据损坏时指向停止位置（可能等于 data.length）。
     */
    endOffset: number
    /** 是否遇到 terminator。 */
    terminated: boolean
    /** 解码结束时的当前 base address（已应用 base address selection）。 */
    baseAddress: number
    entries: LocationListEntry[]
    viewPairs: LocationViewPair[]
    warnings: string[]
}

export interface DecodeLocationListOptions {
    kind: LocListKind
    /** `.debug_loc` 或 `.debug_loclists` 的完整节字节。 */
    data: Uint8Array
    /** 列表起始的节内偏移（取自 DIE 的 DW_AT_location：sec_offset / loclistx 解析结果）。 */
    offset: number
    /** 目标 address size（字节），ESP32 为 4；默认 4。 */
    addrSize?: number
    /** `.debug_addr` 节字节。DWARF5 的 base_addressx / startx_* 条目用它按 index 取地址。 */
    addr?: Uint8Array | null
    /**
     * `.debug_addr` 中第一条地址条目的节内偏移，即 CU 的 DW_AT_addr_base
     * （注意：它指向 header 之后的第一条地址，不是 contribution 起点）。
     */
    addrBase?: number
    /** `.debug_loclists` header 的 segment_selector_size（`addr` 每条目的附加字节数），默认 0。 */
    segmentSelectorSize?: number
    /**
     * 初始 base address。`.debug_loc` 语义中 base 初值 = CU 的 DW_AT_low_pc
     * （拿不到时传 0，与 readelf 一致）；DWARF5 中也作为 offset_pair 的初始 base。
     */
    baseAddress?: number
    /** 防御性上限，避免损坏数据导致超长 entries；默认 4096。 */
    maxEntries?: number
}

/** `.debug_loclists` 单个 CU contribution 的 header。 */
export interface LocationListHeader {
    /** contribution 起始的节内偏移。 */
    offset: number
    /** contribution 结束（不含）的节内偏移。 */
    endOffset: number
    version: number
    addrSize: number
    segmentSelectorSize: number
    offsetEntryCount: number
    /** offset table 原始值（相对 header 末尾，即相对 listsBase）。 */
    offsets: number[]
    /** header 末尾偏移 = 第一条列表的偏移 = CU 的 DW_AT_loclists_base。 */
    listsBase: number
    /** 是否为 64-bit DWARF 格式（unit_length 首值为 0xffffffff）。 */
    dw64: boolean
}

/** header 结构非法时抛出（列表本体解码不会抛，只记 warnings）。 */
export class LocListError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'LocListError'
    }
}

const DEFAULT_MAX_ENTRIES = 4096
const MAX_WARNINGS = 32
const MAX_ULEB_SHIFT = 56

// ---------------------------------------------------------------------------
// 字节读取
// ---------------------------------------------------------------------------

/** 越界/格式错误：内部使用，由 decodeLocationList 转成 warning。 */
class LocListTruncated extends Error {}

class ByteReader {
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
        if (this.pos + 1 > this.end) throw new LocListTruncated('unexpected end of data (u8)')
        return this.bytes[this.pos++]
    }

    u16(): number {
        return this.unsigned(2)
    }

    u32(): number {
        return this.unsigned(4)
    }

    unsigned(size: number): number {
        if (size < 1 || size > 8) throw new LocListError(`invalid integer size ${size}`)
        if (this.pos + size > this.end) {
            throw new LocListTruncated(`unexpected end of data (need ${size} bytes at 0x${this.pos.toString(16)})`)
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
            if (shift > MAX_ULEB_SHIFT) throw new LocListTruncated('ULEB128 value is too large')
        }
    }

    take(size: number): Uint8Array {
        if (size < 0 || this.pos + size > this.end) {
            throw new LocListTruncated(`unexpected end of data (need ${size} bytes at 0x${this.pos.toString(16)})`)
        }
        const slice = this.bytes.subarray(this.pos, this.pos + size)
        this.pos += size
        return slice
    }
}

/** 小端无符号整数；size=8 时超过 2^53 的地址会损失精度（见模块注释）。 */
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

/** 当前 address size 下“全 1”地址；8 字节无法用 number 精确表示，用字节判断。 */
function isMaxAddress(bytes: Uint8Array, offset: number, size: number): boolean {
    for (let i = 0; i < size; i++) {
        if (bytes[offset + i] !== 0xff) return false
    }
    return true
}

/** 32-bit 目标保持无符号语义（与项目其它地方 >>> 0 一致）；64-bit 原样。 */
function normalizeAddress(value: number, addrSize: number): number {
    return addrSize <= 4 ? value >>> 0 : value
}

function warn(warnings: string[], message: string): void {
    if (warnings.length < MAX_WARNINGS) warnings.push(message)
}

function checkAddrSize(addrSize: number, field: string): number {
    if (!Number.isInteger(addrSize) || addrSize < 1 || addrSize > 8) {
        throw new TypeError(`${field} must be an integer in [1, 8], got ${String(addrSize)}`)
    }
    return addrSize
}

// ---------------------------------------------------------------------------
// DWARF 5 .debug_loclists header
// ---------------------------------------------------------------------------

/**
 * 解析一个 `.debug_loclists` CU contribution header。
 *
 * 结构（DWARF 5 §7.28，DWARF32）：
 *   unit_length(4) version(2) address_size(1) segment_selector_size(1)
 *   offset_entry_count(4) offset_entry[offset_entry_count](4 each)
 * DWARF64 时 unit_length 占 12 字节（0xffffffff + u64），offset table 每项 8 字节。
 * offset table 的值相对 header 末尾（= DW_AT_loclists_base）偏移。
 *
 * @throws LocListError header 结构非法/越界。
 */
export function parseLocationListHeader(data: Uint8Array, offset = 0): LocationListHeader {
    if (!(data instanceof Uint8Array)) throw new TypeError('data must be a Uint8Array')
    if (!Number.isInteger(offset) || offset < 0) throw new TypeError('offset must be a non-negative integer')
    if (offset + 4 > data.length) {
        throw new LocListError(`loclists header at 0x${offset.toString(16)} is outside the section`)
    }

    const reader = new ByteReader(data, offset)
    let lengthFieldSize = 4
    let length = reader.u32()
    let dw64 = false
    if (length === 0xffffffff) {
        dw64 = true
        lengthFieldSize = 12
        length = reader.u64()
    }
    if (!Number.isFinite(length) || length < 8) {
        throw new LocListError(
            `loclists contribution at 0x${offset.toString(16)} has invalid unit_length 0x${length.toString(16)}`,
        )
    }
    const endOffset = offset + lengthFieldSize + length
    if (endOffset > data.length) {
        throw new LocListError(
            `loclists contribution at 0x${offset.toString(16)} extends past section end (0x${endOffset.toString(16)} > 0x${data.length.toString(16)})`,
        )
    }
    const version = reader.u16()
    if (version !== 5) {
        throw new LocListError(`unsupported .debug_loclists version ${version} at 0x${offset.toString(16)} (expected 5)`)
    }
    const addrSize = reader.u8()
    checkAddrSize(addrSize, 'address_size')
    const segmentSelectorSize = reader.u8()
    if (segmentSelectorSize !== 0 && addrSize + segmentSelectorSize > 8) {
        throw new LocListError(
            `unsupported segment_selector_size ${segmentSelectorSize} with address_size ${addrSize} at 0x${offset.toString(16)}`,
        )
    }
    const offsetEntryCount = reader.u32()
    const offsetByteSize = dw64 ? 8 : 4
    const headerSize = lengthFieldSize + 8 + offsetEntryCount * offsetByteSize
    if (offset + headerSize > endOffset) {
        throw new LocListError(
            `loclists header at 0x${offset.toString(16)} declares ${offsetEntryCount} offset entries but contribution is too small`,
        )
    }
    const offsets: number[] = []
    for (let i = 0; i < offsetEntryCount; i++) offsets.push(reader.unsigned(offsetByteSize))
    return {
        offset,
        endOffset,
        version,
        addrSize,
        segmentSelectorSize,
        offsetEntryCount,
        offsets,
        listsBase: offset + headerSize,
        dw64,
    }
}

/**
 * DW_FORM_loclistx 的 index -> 列表的节内偏移：
 * `listsBase + offsets[index]`（offset table 值相对 header 末尾）。
 *
 * @throws LocListError index 越界。
 */
export function resolveLocationListIndex(header: LocationListHeader, index: number): number {
    if (!Number.isInteger(index) || index < 0 || index >= header.offsetEntryCount) {
        throw new LocListError(`loclistx index ${index} out of range [0, ${header.offsetEntryCount})`)
    }
    return header.listsBase + header.offsets[index]
}

// ---------------------------------------------------------------------------
// 位置列表本体解码
// ---------------------------------------------------------------------------

interface ListDecodeContext {
    addrSize: number
    pushEntry: (entryKind: LocationListEntry['kind'], entryOffset: number, expr: Uint8Array, range?: [number, number]) => boolean
    isStopped: () => boolean
}

interface DebugLocContext extends ListDecodeContext {
    base: number
    setBase: (base: number) => void
}

interface LocListsContext extends ListDecodeContext {
    segmentSelectorSize: number
    addr: Uint8Array | null
    addrBase: number
    base: number
    setBase: (base: number) => void
    pushViewPair: (pair: LocationViewPair) => void
}

interface ListDecodeOutcome {
    terminated: boolean
}

/**
 * 解码一条位置列表（从 `offset` 开始，到 terminator 或数据错误为止）。
 *
 * 纯函数：不修改输入，也不依赖/修改任何模块级可变状态；坏长度/越界不会抛异常，
 * 而是给出 warnings 并尽力返回已解出的条目。
 *
 * @throws TypeError 参数（kind/addrSize 等）非法；数据损坏只记 warnings。
 */
export function decodeLocationList(options: DecodeLocationListOptions): DecodedLocationList {
    const {
        kind,
        data,
        offset,
        addrSize = 4,
        addr = null,
        addrBase = 0,
        segmentSelectorSize = 0,
        baseAddress = 0,
        maxEntries = DEFAULT_MAX_ENTRIES,
    } = options
    if (kind !== 'debug_loc' && kind !== 'debug_loclists') {
        throw new TypeError(`unsupported location list kind: ${String(kind)}`)
    }
    if (!(data instanceof Uint8Array)) throw new TypeError('data must be a Uint8Array')
    if (!Number.isInteger(offset) || offset < 0) throw new TypeError('offset must be a non-negative integer')
    checkAddrSize(addrSize, 'addrSize')
    if (!Number.isInteger(segmentSelectorSize) || segmentSelectorSize < 0) {
        throw new TypeError('segmentSelectorSize must be a non-negative integer')
    }
    if (!Number.isInteger(maxEntries) || maxEntries < 1) {
        throw new TypeError('maxEntries must be a positive integer')
    }

    const warnings: string[] = []
    const entries: LocationListEntry[] = []
    const viewPairs: LocationViewPair[] = []
    let base = normalizeAddress(baseAddress, addrSize)
    let stopped = false

    if (offset >= data.length) {
        warn(warnings, `location list offset 0x${offset.toString(16)} is outside the section (size 0x${data.length.toString(16)})`)
        return {
            kind,
            startOffset: offset,
            endOffset: data.length,
            terminated: false,
            baseAddress: base,
            entries,
            viewPairs,
            warnings,
        }
    }

    const reader = new ByteReader(data, offset)
    const pushEntry = (
        entryKind: LocationListEntry['kind'],
        entryOffset: number,
        expr: Uint8Array,
        range?: [number, number],
    ): boolean => {
        if (entries.length >= maxEntries) {
            warn(warnings, `location list at 0x${offset.toString(16)} exceeds maxEntries ${maxEntries}; decoding stopped`)
            stopped = true
            return false
        }
        const entry: LocationListEntry = {index: entries.length, kind: entryKind, expr, entryOffset}
        if (range) {
            entry.lowPc = normalizeAddress(range[0], addrSize)
            entry.highPc = normalizeAddress(range[1], addrSize)
        }
        entries.push(entry)
        return true
    }

    let terminated = false
    try {
        const outcome =
            kind === 'debug_loc'
                ? decodeDebugLocEntries(reader, warnings, {
                      addrSize,
                      base,
                      pushEntry,
                      isStopped: () => stopped,
                      setBase: (next) => {
                          base = next
                      },
                  })
                : decodeLocListsEntries(reader, warnings, {
                      addrSize,
                      segmentSelectorSize,
                      addr,
                      addrBase,
                      base,
                      pushEntry,
                      isStopped: () => stopped,
                      setBase: (next) => {
                          base = next
                      },
                      pushViewPair: (pair) => {
                          if (viewPairs.length < maxEntries) viewPairs.push(pair)
                      },
                  })
        terminated = outcome.terminated
    } catch (error) {
        if (error instanceof LocListTruncated) {
            warn(warnings, `location list at 0x${offset.toString(16)}: ${error.message}`)
        } else {
            throw error
        }
    }

    return {
        kind,
        startOffset: offset,
        endOffset: reader.pos,
        terminated,
        baseAddress: base,
        entries,
        viewPairs,
        warnings,
    }
}

/** 解码 DWARF 2/3/4 `.debug_loc` 条目。 */
function decodeDebugLocEntries(reader: ByteReader, warnings: string[], context: DebugLocContext): ListDecodeOutcome {
    const {addrSize, pushEntry, isStopped, setBase} = context
    let base = context.base
    for (;;) {
        if (isStopped()) return {terminated: false}
        if (reader.remaining === 0) {
            warn(warnings, 'location list is not terminated (reached end of section)')
            return {terminated: false}
        }
        const entryOffset = reader.pos
        if (reader.remaining < 2 * addrSize) {
            warn(warnings, `location list is not terminated: truncated entry at 0x${entryOffset.toString(16)}`)
            return {terminated: false}
        }
        const begin = reader.unsigned(addrSize)
        const end = reader.unsigned(addrSize)
        if (begin === 0 && end === 0) return {terminated: true}
        if (isMaxAddress(reader.bytes, entryOffset, addrSize)) {
            // base address selection entry：begin 全 1，end 为新 base。
            base = normalizeAddress(end, addrSize)
            setBase(base)
            continue
        }
        if (reader.remaining < 2) {
            warn(warnings, `location list is not terminated: missing expression length at 0x${reader.pos.toString(16)}`)
            return {terminated: false}
        }
        const exprLength = reader.u16()
        if (exprLength > reader.remaining) {
            warn(
                warnings,
                `location list entry at 0x${entryOffset.toString(16)} declares expression length ${exprLength} but only ${reader.remaining} bytes remain`,
            )
            return {terminated: false}
        }
        const expr = reader.take(exprLength)
        if (!pushEntry('address_range', entryOffset, expr, [base + begin, base + end])) {
            return {terminated: false}
        }
    }
}

/** 解码 DWARF 5 `.debug_loclists` 条目。 */
function decodeLocListsEntries(reader: ByteReader, warnings: string[], context: LocListsContext): ListDecodeOutcome {
    const {addrSize, segmentSelectorSize, addr, addrBase, pushEntry, isStopped, setBase, pushViewPair} = context
    let base = context.base
    const readExpr = (): Uint8Array => {
        const length = reader.uleb()
        if (length > reader.remaining) {
            throw new LocListTruncated(`expression length ${length} exceeds remaining ${reader.remaining} bytes`)
        }
        return reader.take(length)
    }
    const lookupAddress = (index: number, entryOffset: number): number | null => {
        if (!addr) {
            warn(
                warnings,
                `location list entry at 0x${entryOffset.toString(16)} uses address index ${index} but no .debug_addr was provided`,
            )
            return null
        }
        const stride = addrSize + segmentSelectorSize
        const position = addrBase + index * stride
        if (!Number.isSafeInteger(position) || position < 0 || position + addrSize > addr.length) {
            warn(
                warnings,
                `location list entry at 0x${entryOffset.toString(16)}: address index ${index} resolves outside .debug_addr (0x${position.toString(16)})`,
            )
            return null
        }
        return readUnsignedLE(addr, position, addrSize)
    }

    for (;;) {
        if (isStopped()) return {terminated: false}
        if (reader.remaining === 0) {
            warn(warnings, 'location list is not terminated (reached end of section)')
            return {terminated: false}
        }
        const entryOffset = reader.pos
        const lle = reader.u8()
        if (lle === DW_LLE_END_OF_LIST) return {terminated: true}
        if (lle === DW_LLE_BASE_ADDRESSX) {
            const index = reader.uleb()
            const address = lookupAddress(index, entryOffset)
            if (address === null) return {terminated: false}
            base = normalizeAddress(address, addrSize)
            setBase(base)
            continue
        }
        if (lle === DW_LLE_BASE_ADDRESS) {
            base = normalizeAddress(reader.unsigned(addrSize), addrSize)
            setBase(base)
            continue
        }
        if (lle === DW_LLE_DEFAULT_LOCATION) {
            if (!pushEntry('default_location', entryOffset, readExpr())) return {terminated: false}
            continue
        }
        if (lle === DW_LLE_OFFSET_PAIR) {
            const start = reader.uleb()
            const end = reader.uleb()
            const expr = readExpr()
            if (!pushEntry('offset_pair', entryOffset, expr, [base + start, base + end])) return {terminated: false}
            continue
        }
        if (lle === DW_LLE_START_END) {
            const start = reader.unsigned(addrSize)
            const end = reader.unsigned(addrSize)
            const expr = readExpr()
            if (!pushEntry('address_range', entryOffset, expr, [start, end])) return {terminated: false}
            continue
        }
        if (lle === DW_LLE_START_LENGTH) {
            const start = reader.unsigned(addrSize)
            const length = reader.uleb()
            const expr = readExpr()
            if (!pushEntry('address_range', entryOffset, expr, [start, start + length])) return {terminated: false}
            continue
        }
        if (lle === DW_LLE_STARTX_ENDX) {
            const startIndex = reader.uleb()
            const endIndex = reader.uleb()
            const startAddress = lookupAddress(startIndex, entryOffset)
            const endAddress = lookupAddress(endIndex, entryOffset)
            if (startAddress === null || endAddress === null) return {terminated: false}
            const expr = readExpr()
            if (!pushEntry('address_range', entryOffset, expr, [startAddress, endAddress])) return {terminated: false}
            continue
        }
        if (lle === DW_LLE_STARTX_LENGTH) {
            const startIndex = reader.uleb()
            const length = reader.uleb()
            const startAddress = lookupAddress(startIndex, entryOffset)
            if (startAddress === null) return {terminated: false}
            const expr = readExpr()
            if (!pushEntry('address_range', entryOffset, expr, [startAddress, startAddress + length])) {
                return {terminated: false}
            }
            continue
        }
        if (lle === DW_LLE_GNU_VIEW_PAIR) {
            const beginView = reader.uleb()
            const endView = reader.uleb()
            pushViewPair({entryOffset, beginView, endView})
            continue
        }
        warn(warnings, `unknown DW_LLE opcode 0x${lle.toString(16)} at 0x${entryOffset.toString(16)}; decoding stopped`)
        return {terminated: false}
    }
}

/**
 * 在解出的条目里选择覆盖 PC 的条目：
 *  - 命中的第一个半开区间 [lowPc, highPc) 条目优先；
 *  - 没有命中时回退到 DW_LLE_default_location 条目；
 *  - 都没有返回 null。
 */
export function findLocationListEntry(entries: readonly LocationListEntry[], pc: number): LocationListEntry | null {
    let fallback: LocationListEntry | null = null
    for (const entry of entries) {
        if (entry.kind === 'default_location' || entry.lowPc === undefined || entry.highPc === undefined) {
            if (entry.kind === 'default_location' && !fallback) fallback = entry
            continue
        }
        if (pc >= entry.lowPc && pc < entry.highPc) return entry
    }
    return fallback
}

/** 位置列表条目是否覆盖某个 PC（语义同 findLocationListEntry）。 */
export function locationListCoversPc(entries: readonly LocationListEntry[], pc: number): boolean {
    return findLocationListEntry(entries, pc) !== null
}
