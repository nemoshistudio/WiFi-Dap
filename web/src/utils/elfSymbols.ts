/**
 * 最小 ELF32 小端 ARM 符号表解析（无第三方依赖，可在浏览器与 Node 中运行）。
 *
 * 曲线页只需要“变量名 -> 地址 -> 大小”：读取 section headers，定位
 * SHT_SYMTAB 及其链接的字符串表（.strtab），提取 STT_OBJECT / STT_FUNC，
 * 再按目标可访问地址范围过滤（STM32 典型 Flash / RAM，含 DTCM/AXI SRAM 区间）。
 * 不做 DWARF 类型解析；显示格式由页面选择（u32/i32/f32）。
 */

export class ElfParseError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'ElfParseError'
    }
}

export type ElfSymbolType = 'object' | 'func'

export interface ElfSymbol {
    name: string
    /** 绝对地址（ET_EXEC/ET_DYN 的 st_value；ET_REL 为节内偏移 + 节地址，通常不可用） */
    address: number
    /** 字节大小（可能为 0，编译器未提供） */
    size: number
    type: ElfSymbolType
    /** st_shndx：所属节索引 */
    sectionIndex: number
}

export interface ElfParseResult {
    /** 已按地址范围过滤、去重并排序的候选符号 */
    symbols: ElfSymbol[]
    /** 符号表中所有符号数 */
    totalSymbols: number
    /** STT_OBJECT / STT_FUNC 数量（过滤前） */
    objectSymbols: number
    funcSymbols: number
    /** 地址落在 Flash 区间的数量 */
    flashSymbols: number
    /** 地址落在 RAM 区间的数量（含 DTCM/AXI SRAM） */
    ramSymbols: number
    /** 其它类型 / 不可用地址被过滤的数量 */
    filteredSymbols: number
    machine: number
    entry: number
    /** 是否是可重定位文件（.o/.obj：地址为节内偏移） */
    relocatable: boolean
    warnings: string[]
}

/** STM32 典型内存窗口（与固件 target 烧录支持的 Flash/RAM 范围一致）。 */
export const TARGET_FLASH_RANGE: readonly [number, number] = [0x08000000, 0x09000000]
export const TARGET_RAM_RANGE: readonly [number, number] = [0x20000000, 0x24080000]

const EHDR_SIZE = 52
const SHDR_SIZE = 40
const SYM_SIZE = 16

const SHT_SYMTAB = 2
const SHT_STRTAB = 3
const SHT_DYNSYM = 11
const SHF_ALLOC = 0x2
const SHN_UNDEF = 0
const SHN_ABS = 0xfff1
const SHN_COMMON = 0xfff2
const SHN_XINDEX = 0xffff
const ELF32_ST_TYPE_MASK = 0x0f
const STT_OBJECT = 1
const STT_FUNC = 2
const EM_ARM = 40
const ET_REL = 1

export function isPlausibleTargetAddress(address: number): boolean {
    const value = address >>> 0
    return (
        (value >= TARGET_FLASH_RANGE[0] && value < TARGET_FLASH_RANGE[1]) ||
        (value >= TARGET_RAM_RANGE[0] && value < TARGET_RAM_RANGE[1])
    )
}

function u8(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 1 > length) {
        throw new ElfParseError(`${what}: offset 0x${offset.toString(16)} out of range`)
    }
    return view.getUint8(offset)
}

function u16(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 2 > length) {
        throw new ElfParseError(`${what}: offset 0x${offset.toString(16)} out of range`)
    }
    return view.getUint16(offset, true)
}

function u32(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 4 > length) {
        throw new ElfParseError(`${what}: offset 0x${offset.toString(16)} out of range`)
    }
    return view.getUint32(offset, true)
}

function readCString(bytes: Uint8Array, offset: number, length: number): string {
    if (offset < 0 || offset >= length) return ''
    let end = offset
    while (end < length && bytes[end] !== 0) end++
    if (end - offset > 512) return ''
    let text = ''
    for (let i = offset; i < end; i++) text += String.fromCharCode(bytes[i])
    return text
}

/**
 * 解析 ELF32 小端 ARM 文件的符号表。
 * @throws {ElfParseError} 格式不支持或关键节损坏
 */
export function parseElf32Symbols(buffer: ArrayBuffer | Uint8Array): ElfParseResult {
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
    const length = bytes.byteLength
    if (length < EHDR_SIZE) {
        throw new ElfParseError('file is too small to be an ELF file')
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
    if (!(bytes[0] === 0x7f && bytes[1] === 0x45 && bytes[2] === 0x4c && bytes[3] === 0x46)) {
        throw new ElfParseError('not an ELF file (bad magic)')
    }
    if (u8(view, 4, length, 'ELF header') !== 1) {
        throw new ElfParseError('unsupported ELF class: only ELF32 is supported')
    }
    if (u8(view, 5, length, 'ELF header') !== 1) {
        throw new ElfParseError('unsupported byte order: only little-endian is supported')
    }

    const machine = u16(view, 18, length, 'ELF header')
    if (machine !== EM_ARM) {
        throw new ElfParseError(`unsupported machine ${machine}: only ARM (EM_ARM) is supported`)
    }
    const elfType = u16(view, 16, length, 'ELF header')
    const entry = u32(view, 24, length, 'ELF header')
    const shoff = u32(view, 32, length, 'ELF header')
    const shentsize = u16(view, 46, length, 'ELF header')
    let shnum = u16(view, 48, length, 'ELF header')
    let shstrndx = u16(view, 50, length, 'ELF header')
    const warnings: string[] = []

    if (shoff === 0 || shentsize < SHDR_SIZE) {
        throw new ElfParseError('no section headers (stripped firmware ELF?)')
    }
    if (shoff + SHDR_SIZE > length) {
        throw new ElfParseError('section header table is out of range')
    }
    if (shnum === 0) {
        // 扩展形式：shnum 存于 0 号 section 的 sh_size
        shnum = u32(view, shoff + 20, length, 'section 0')
        if (shnum === 0 || shnum > 4096) {
            throw new ElfParseError(`implausible section count ${shnum}`)
        }
    }
    if (shoff + shnum * shentsize > length) {
        throw new ElfParseError('section header table is out of range')
    }

    interface SectionHeader {
        index: number
        nameOffset: number
        name: string
        type: number
        flags: number
        address: number
        offset: number
        size: number
        link: number
        entsize: number
    }
    const sectionAt = (index: number): SectionHeader => {
        const base = shoff + index * shentsize
        return {
            index,
            nameOffset: u32(view, base, length, 'section header'),
            name: '',
            type: u32(view, base + 4, length, 'section header'),
            flags: u32(view, base + 8, length, 'section header'),
            address: u32(view, base + 12, length, 'section header'),
            offset: u32(view, base + 16, length, 'section header'),
            size: u32(view, base + 20, length, 'section header'),
            link: u32(view, base + 24, length, 'section header'),
            entsize: u32(view, base + 36, length, 'section header'),
        }
    }

    const sections: SectionHeader[] = []
    for (let i = 0; i < shnum; i++) sections.push(sectionAt(i))

    if (shstrndx === SHN_XINDEX) {
        shstrndx = sections[0].link
    }
    if (shstrndx < sections.length) {
        const shstrtab = sections[shstrndx]
        if (shstrtab.offset < length && shstrtab.offset + shstrtab.size <= length) {
            for (const section of sections) {
                section.name = readCString(bytes, shstrtab.offset + section.nameOffset, length)
            }
        }
    }

    // 优先 .symtab；无符号表时退回 .dynsym（共享库/部分构建）。
    let symtab = sections.find((section) => section.type === SHT_SYMTAB)
    if (!symtab) {
        symtab = sections.find((section) => section.type === SHT_DYNSYM)
        if (symtab) warnings.push('no .symtab: falling back to .dynsym (fewer symbols)')
    }
    if (!symtab) {
        throw new ElfParseError('no symbol table (.symtab/.dynsym) found; strip=false build required')
    }
    const strtab = sections[symtab.link]
    if (!strtab || strtab.type !== SHT_STRTAB) {
        throw new ElfParseError('.symtab has no valid linked string table')
    }
    if (strtab.offset + strtab.size > length) {
        throw new ElfParseError('symbol string table is out of range')
    }

    const symbolSize = symtab.entsize >= SYM_SIZE ? symtab.entsize : SYM_SIZE
    const totalSymbols = Math.floor(symtab.size / symbolSize)
    if (symtab.offset + totalSymbols * symbolSize > length) {
        throw new ElfParseError('symbol table is out of range')
    }

    const symbols: ElfSymbol[] = []
    const seen = new Set<string>()
    let objectSymbols = 0
    let funcSymbols = 0
    let filteredSymbols = 0
    let flashSymbols = 0
    let ramSymbols = 0

    for (let i = 0; i < totalSymbols; i++) {
        const base = symtab.offset + i * symbolSize
        const nameOffset = u32(view, base, length, 'symbol')
        const value = u32(view, base + 4, length, 'symbol')
        const size = u32(view, base + 8, length, 'symbol')
        const info = u8(view, base + 12, length, 'symbol')
        const shndx = u16(view, base + 14, length, 'symbol')
        const type = info & ELF32_ST_TYPE_MASK

        if (type !== STT_OBJECT && type !== STT_FUNC) continue
        if (shndx === SHN_UNDEF || shndx === SHN_ABS || shndx === SHN_COMMON) {
            filteredSymbols += 1
            continue
        }
        if (type === STT_OBJECT) objectSymbols += 1
        else funcSymbols += 1

        const section = shndx < sections.length ? sections[shndx] : null
        if (section && (section.flags & SHF_ALLOC) === 0) {
            filteredSymbols += 1
            continue
        }
        const name = readCString(bytes, strtab.offset + nameOffset, length)
        if (!name) {
            filteredSymbols += 1
            continue
        }
        // ET_REL 的 st_value 是节内偏移；补上节地址后再判断是否可采样。
        const address = (elfType === ET_REL && section ? section.address + value : value) >>> 0
        if (address === 0 || !isPlausibleTargetAddress(address)) {
            filteredSymbols += 1
            continue
        }
        const key = `${name}@${address.toString(16)}`
        if (seen.has(key)) continue
        seen.add(key)
        if (address < TARGET_RAM_RANGE[0]) flashSymbols += 1
        else ramSymbols += 1
        symbols.push({
            name,
            address,
            size,
            type: type === STT_FUNC ? 'func' : 'object',
            sectionIndex: shndx,
        })
    }

    if (elfType === ET_REL) {
        warnings.push('relocatable ELF: addresses are section-relative and usually not samplable')
    }

    // 按地址排序：同一结构体/数组的字段在列表中相邻，方便人工挑选。
    symbols.sort((left, right) => left.address - right.address || left.name.localeCompare(right.name))

    return {
        symbols,
        totalSymbols,
        objectSymbols,
        funcSymbols,
        flashSymbols,
        ramSymbols,
        filteredSymbols,
        machine,
        entry,
        relocatable: elfType === ET_REL,
        warnings,
    }
}

/** 页面展示用：0x + 8 位大写十六进制。 */
export function formatHex32(value: number): string {
    return `0x${(value >>> 0).toString(16).toUpperCase().padStart(8, '0')}`
}
