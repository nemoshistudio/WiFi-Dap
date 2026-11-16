/**
 * ELF32 小端 ARM 解析入口：
 *  - 读取 ELF header / section headers / .symtab+.strtab；
 *  - 优先走 DWARF（`elfDwarf.ts`）提取“源变量名 + 类型 + 地址 + file:line + 成员树”；
 *  - 无 DWARF（或 DWARF 无固定地址变量）时回退到符号表，并用内置 Itanium C++
 *    反混淆器把 `_ZL8sn_tools` 之类变成可读名（类型标记为未知）。
 *
 * 页面只依赖 `parseElfVariables()` 与本文件的类型/工具导出。
 */
import type {ElfSectionInfo, ElfVariablesResult, SampleLeaf, VariableNode} from './elfTypes'
import {extractDwarfVariables} from './elfDwarf'
import {
    formatHex32,
    addressInAllocSections,
    isPlausibleTargetAddress,
    TARGET_FLASH_RANGE,
    TARGET_RAM_RANGE,
} from './elfTypes'

export type {AddressExpr, DisplayFormat, ElfSectionInfo, ElfVariablesResult, SampleLeaf, ValueKind, VariableNode} from './elfTypes'
export {addressInAllocSections, formatHex32, isPlausibleTargetAddress, TARGET_FLASH_RANGE, TARGET_RAM_RANGE}
export {DwarfParseError} from './elfDwarf'

export class ElfParseError extends Error {
    constructor(message: string) {
        super(message)
        this.name = 'ElfParseError'
    }
}

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

function u8(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 1 > length) throw new ElfParseError(`${what}: offset out of range`)
    return view.getUint8(offset)
}

function u16(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 2 > length) throw new ElfParseError(`${what}: offset out of range`)
    return view.getUint16(offset, true)
}

function u32(view: DataView, offset: number, length: number, what: string): number {
    if (offset < 0 || offset + 4 > length) throw new ElfParseError(`${what}: offset out of range`)
    return view.getUint32(offset, true)
}

function readCString(bytes: Uint8Array, offset: number): string {
    if (offset < 0 || offset >= bytes.length) return ''
    let end = offset
    while (end < bytes.length && bytes[end] !== 0) end++
    if (end - offset > 4096) return ''
    let text = ''
    for (let i = offset; i < end; i++) text += String.fromCharCode(bytes[i])
    return text
}

interface RawSymbol {
    name: string
    value: number
    size: number
    info: number
    shndx: number
}

/** 解析后的 ELF 文件（仅保留曲线页需要的字段）。 */
class ElfImage {
    readonly bytes: Uint8Array
    readonly view: DataView
    readonly machine: number
    readonly entry: number
    readonly elfType: number
    readonly sections: ElfSectionInfo[]

    private constructor(bytes: Uint8Array, view: DataView, machine: number, entry: number, elfType: number, sections: ElfSectionInfo[]) {
        this.bytes = bytes
        this.view = view
        this.machine = machine
        this.entry = entry
        this.elfType = elfType
        this.sections = sections
    }

    static parse(buffer: ArrayBuffer | Uint8Array): ElfImage {
        const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
        const length = bytes.byteLength
        if (length < EHDR_SIZE) throw new ElfParseError('file is too small to be an ELF file')
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

        if (shoff === 0 || shentsize < SHDR_SIZE) {
            throw new ElfParseError('no section headers (stripped firmware ELF?)')
        }
        if (shoff + SHDR_SIZE > length) throw new ElfParseError('section header table is out of range')
        if (shnum === 0) {
            shnum = u32(view, shoff + 20, length, 'section 0')
            if (shnum === 0 || shnum > 4096) throw new ElfParseError(`implausible section count ${shnum}`)
        }
        if (shoff + shnum * shentsize > length) throw new ElfParseError('section header table is out of range')

        const base = (index: number) => shoff + index * shentsize
        const sections: ElfSectionInfo[] = []
        for (let i = 0; i < shnum; i++) {
            const at = base(i)
            sections.push({
                index: i,
                name: '',
                type: u32(view, at + 4, length, 'section header'),
                flags: u32(view, at + 8, length, 'section header'),
                address: u32(view, at + 12, length, 'section header'),
                offset: u32(view, at + 16, length, 'section header'),
                size: u32(view, at + 20, length, 'section header'),
                link: u32(view, at + 24, length, 'section header'),
                entsize: u32(view, at + 36, length, 'section header'),
            })
        }
        if (shstrndx === SHN_XINDEX) shstrndx = sections[0].link
        const shstrtab = sections[shstrndx]
        if (shstrtab && shstrtab.offset + shstrtab.size <= length) {
            for (const section of sections) {
                const nameOffset = u32(view, base(section.index), length, 'section header')
                section.name = readCString(bytes, shstrtab.offset + nameOffset)
            }
        }
        return new ElfImage(bytes, view, machine, entry, elfType, sections)
    }

    section(name: string): ElfSectionInfo | null {
        return this.sections.find((section) => section.name === name) ?? null
    }

    /** 读取符号表（优先 .symtab，其次 .dynsym）；无符号表时返回空。 */
    readSymbols(): {symbols: RawSymbol[]; totalSymbols: number} {
        const length = this.bytes.byteLength
        let symtab = this.sections.find((section) => section.type === SHT_SYMTAB)
        if (!symtab) symtab = this.sections.find((section) => section.type === SHT_DYNSYM)
        if (!symtab) return {symbols: [], totalSymbols: 0}
        const strtab = this.sections[symtab.link]
        if (!strtab || strtab.type !== SHT_STRTAB) return {symbols: [], totalSymbols: 0}
        if (strtab.offset + strtab.size > length || symtab.offset + symtab.size > length) {
            return {symbols: [], totalSymbols: 0}
        }
        const entsize = symtab.entsize >= SYM_SIZE ? symtab.entsize : SYM_SIZE
        const total = Math.floor(symtab.size / entsize)
        const symbols: RawSymbol[] = []
        for (let i = 0; i < total; i++) {
            const at = symtab.offset + i * entsize
            const nameOffset = u32(this.view, at, length, 'symbol')
            symbols.push({
                name: readCString(this.bytes, strtab.offset + nameOffset),
                value: u32(this.view, at + 4, length, 'symbol'),
                size: u32(this.view, at + 8, length, 'symbol'),
                info: u8(this.view, at + 12, length, 'symbol'),
                shndx: u16(this.view, at + 14, length, 'symbol'),
            })
        }
        return {symbols, totalSymbols: total}
    }
}

// ---------------------------------------------------------------------------
// Itanium C++ 反混淆（覆盖符号表回退路径的常见形态）
// ---------------------------------------------------------------------------
const BUILTIN_TYPES: Record<string, string> = {
    v: 'void',
    w: 'wchar_t',
    b: 'bool',
    c: 'char',
    a: 'signed char',
    h: 'unsigned char',
    s: 'short',
    t: 'unsigned short',
    i: 'int',
    j: 'unsigned int',
    l: 'long',
    m: 'unsigned long',
    x: 'long long',
    y: 'unsigned long long',
    n: '__int128',
    o: 'unsigned __int128',
    f: 'float',
    d: 'double',
    e: 'long double',
    g: '__float128',
    z: '...',
}

const OPERATORS: Record<string, string> = {
    pl: '+', mi: '-', ml: '*', dv: '/', rm: '%', an: '&', or: '|', eo: '^', aS: '=',
    eq: '==', ne: '!=', lt: '<', gt: '>', le: '<=', ge: '>=', ls: '<<', rs: '>>',
    nt: '!', aa: '&&', oo: '||', co: '~', cl: '()', ix: '[]', pt: '->', mm: '--', pp: '++',
    cm: ',', pm: '->*', nw: ' new', na: ' new[]', dl: ' delete', da: ' delete[]',
}

class Demangler {
    private readonly source: string
    private index: number
    private readonly substitutions: string[] = []

    constructor(source: string) {
        this.source = source
        this.index = 2
    }

    parse(): string | null {
        if (this.peek() === 'L') this.index++
        let name: string | null
        if (this.peek() === 'N') name = this.parseNestedName()
        else name = this.parseSourceNameOrBuiltin()
        if (!name) return null
        // 函数参数列表（数据符号没有）；解析失败不影响已得名称。
        const params: string[] = []
        let guard = 0
        while (this.index < this.source.length && guard++ < 64) {
            const param = this.parseType()
            if (!param) break
            params.push(param)
        }
        if (params.length > 0) name += `(${params.join(', ')})`
        return name
    }

    private peek(): string {
        return this.source[this.index] ?? ''
    }

    private parseSourceName(): string | null {
        let digits = ''
        while (this.index < this.source.length && this.source[this.index] >= '0' && this.source[this.index] <= '9') {
            digits += this.source[this.index++]
        }
        if (!digits) return null
        const length = Number.parseInt(digits, 10)
        if (!Number.isFinite(length) || length <= 0 || length > 4096) return null
        const name = this.source.slice(this.index, this.index + length)
        if (name.length !== length) return null
        this.index += length
        return name
    }

    private parseSourceNameOrBuiltin(): string | null {
        const char = this.peek()
        if (char >= '0' && char <= '9') return this.parseSourceName()
        if (char === 'S') return this.parseSubstitution()
        if (char === 'C' || char === 'D') return this.parseCtorDtor(null)
        return this.parseOperatorOrBuiltin()
    }

    private parseSubstitution(): string | null {
        if (this.peek() !== 'S') return null
        this.index++
        const next = this.peek()
        if (next === 't' || next === 's') {
            this.index++
            if (next === 't' && this.peek() === 'd') this.index++
            return 'std'
        }
        if (next === 'a' || next === 'b' || next === 'i' || next === 'o' || next === 'd') {
            this.index++
            return 'std'
        }
        let digits = ''
        while (this.index < this.source.length && this.source[this.index] >= '0' && this.source[this.index] <= '9') {
            digits += this.source[this.index++]
        }
        if (this.peek() === '_') this.index++
        if (digits) {
            const index = Number.parseInt(digits, 10)
            return this.substitutions[index] ?? ''
        }
        return this.substitutions[0] ?? ''
    }

    private parseNestedName(): string | null {
        if (this.peek() !== 'N') return null
        this.index++
        while ('rVKRO'.includes(this.peek())) this.index++
        const parts: string[] = []
        let guard = 0
        while (this.index < this.source.length && this.peek() !== 'E') {
            if (guard++ > 256) return null
            const char = this.peek()
            if (char === 'I') {
                const args = this.parseTemplateArgs()
                if (args && parts.length > 0) parts[parts.length - 1] += args
                continue
            }
            let component: string | null
            if (char === 'C' || char === 'D') component = this.parseCtorDtor(parts[parts.length - 1] ?? null)
            else component = this.parseSourceNameOrBuiltin()
            if (component === null) return null
            parts.push(component)
        }
        if (this.peek() === 'E') this.index++
        return parts.length > 0 ? parts.join('::') : null
    }

    private parseCtorDtor(classHint: string | null): string | null {
        const kind = this.peek()
        this.index++
        this.index++ // 1/2/3 或 0/1/2
        const name = classHint ?? ''
        if (kind === 'D') return `~${name}`
        return name
    }

    private parseTemplateArgs(): string | null {
        if (this.peek() !== 'I') return null
        this.index++
        const args: string[] = []
        let guard = 0
        while (this.index < this.source.length && this.peek() !== 'E') {
            if (guard++ > 128) return null
            if (this.peek() === 'L') {
                const end = this.source.indexOf('E', this.index)
                if (end < 0) return null
                args.push(this.source.slice(this.index + 1, end))
                this.index = end + 1
                continue
            }
            const type = this.parseType()
            if (type === null) return null
            args.push(type)
        }
        if (this.peek() === 'E') this.index++
        return `<${args.join(', ')}>`
    }

    private parseOperatorOrBuiltin(): string | null {
        const char = this.peek()
        if (BUILTIN_TYPES[char]) {
            this.index++
            return BUILTIN_TYPES[char]
        }
        if (this.source.startsWith('operator', this.index)) {
            this.index += 'operator'.length
            const code = this.source.slice(this.index, this.index + 2)
            if (OPERATORS[code]) {
                this.index += 2
                return `operator${OPERATORS[code]}`
            }
            return 'operator'
        }
        const code = this.source.slice(this.index, this.index + 2)
        if (OPERATORS[code]) {
            this.index += 2
            return `operator${OPERATORS[code]}`
        }
        return null
    }

    private parseType(): string | null {
        const char = this.peek()
        if (!char) return null
        if (char >= '0' && char <= '9') return this.parseSourceName()
        switch (char) {
            case 'P': {
                this.index++
                const inner = this.parseType()
                return inner === null ? null : `${inner} *`
            }
            case 'R': {
                this.index++
                const inner = this.parseType()
                return inner === null ? null : `${inner} &`
            }
            case 'O': {
                this.index++
                const inner = this.parseType()
                return inner === null ? null : `${inner} &&`
            }
            case 'K': {
                this.index++
                const inner = this.parseType()
                return inner === null ? null : `const ${inner}`
            }
            case 'V': {
                this.index++
                const inner = this.parseType()
                return inner === null ? null : `volatile ${inner}`
            }
            case 'S':
                return this.parseSubstitution()
            case 'N': {
                const name = this.parseNestedName()
                if (name) this.substitutions.push(name)
                return name
            }
            case 'F': {
                this.index++
                if (this.peek() === 'Y') this.index++
                const params: string[] = []
                let guard = 0
                while (this.index < this.source.length && this.peek() !== 'E' && guard++ < 32) {
                    const param = this.parseType()
                    if (param === null) break
                    params.push(param)
                }
                if (this.peek() === 'E') this.index++
                return `void (*)(${params.join(', ')})`
            }
            case 'A': {
                this.index++
                let digits = ''
                while (this.index < this.source.length && this.source[this.index] >= '0' && this.source[this.index] <= '9') {
                    digits += this.source[this.index++]
                }
                if (this.peek() === '_') this.index++
                const inner = this.parseType()
                return inner === null ? null : `${inner} [${digits}]`
            }
            case 'T': {
                this.index++
                if (this.peek() === '_') this.index++
                return 'T'
            }
            case 'D': {
                const start = this.index
                this.index++
                let depth = 0
                while (this.index < this.source.length) {
                    const current = this.source[this.index]
                    if (current === 'E') {
                        if (depth === 0) {
                            this.index++
                            break
                        }
                        depth--
                    } else if (current === 'I' || current === 'F') {
                        depth++
                    }
                    this.index++
                }
                return `decltype(${this.source.slice(start, this.index)})`
            }
            default: {
                const builtin = BUILTIN_TYPES[char]
                if (builtin) {
                    this.index++
                    return builtin
                }
                return this.parseOperatorOrBuiltin()
            }
        }
    }
}

/** Itanium C++ 反混淆；无法识别时返回 null。 */
export function demangleItanium(name: string): string | null {
    if (!name.startsWith('_Z')) return null
    try {
        const demangler = new Demangler(name)
        const result = demangler.parse()
        return result && result.length > 0 ? result : null
    } catch {
        return null
    }
}

/** 极简兜底：抽出 `_Z...` 中所有“长度+标识符”，用于解析器失败时。 */
function fallbackDemangle(name: string): string {
    if (!name.startsWith('_Z')) return name
    const rest = name.slice(2).replace(/^L/, '')
    const parts: string[] = []
    let index = 0
    while (index < rest.length && parts.length < 16) {
        let digits = ''
        while (index < rest.length && rest[index] >= '0' && rest[index] <= '9') {
            digits += rest[index++]
        }
        if (!digits) break
        const length = Number.parseInt(digits, 10)
        const part = rest.slice(index, index + length)
        if (part.length !== length) break
        parts.push(part)
        index += length
    }
    return parts.length > 0 ? parts.join('::') : name
}

function displaySymbolName(rawName: string): string {
    if (!rawName.startsWith('_Z')) return rawName
    return demangleItanium(rawName) ?? fallbackDemangle(rawName)
}

const ARTIFACT_RE =
    /^(__EH_FRAME|_GLOBAL__sub_I_|_GLOBAL__I_|__cxx_|_ZGV|__init_array_|__fini_array_|__preinit_array_|object\.\d+$|completed\.\d+$|_ZN12_GLOBAL__N_)/

function isCompilerArtifact(name: string): boolean {
    if (!name) return true
    if (ARTIFACT_RE.test(name)) return true
    if (name.includes('_array_entry')) return true
    if (name.startsWith('unlikely.') || name.startsWith('likely.')) return true
    if (name === '_etext' || name === '_edata' || name === '_end' || name === '__bss_start' || name === '__bss_end__') return true
    if (name.startsWith('__') && name.endsWith('_start')) return true
    return false
}

// ---------------------------------------------------------------------------
// 公共 API
// ---------------------------------------------------------------------------
export interface DwarfCounts {
    dwarfVariables: number
    dwarfWithAddress: number
    dwarfSkipped: number
    dwarfUnits: number
}

/**
 * 解析 ELF：优先返回 DWARF 变量（带类型/成员树），无 DWARF 时回退符号表。
 * @throws {ElfParseError} 非 ELF32 LE ARM 或关键结构损坏
 */
export function parseElfVariables(buffer: ArrayBuffer | Uint8Array): ElfVariablesResult {
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
    const image = ElfImage.parse(bytes)
    const warnings: string[] = []

    let dwarf: ReturnType<typeof extractDwarfVariables> | null = null
    try {
        dwarf = extractDwarfVariables(bytes, image.sections, {
            demangle: demangleItanium,
            relocatable: image.elfType === ET_REL,
        })
        warnings.push(...dwarf.warnings)
    } catch (error) {
        warnings.push(`DWARF parse failed: ${error instanceof Error ? error.message : String(error)}`)
    }

    if (dwarf && dwarf.variables.length > 0) {
        return {
            source: 'dwarf',
            variables: dwarf.variables,
            dwarfVariables: dwarf.dwarfVariables,
            dwarfNamed: dwarf.dwarfNamed,
            dwarfWithAddress: dwarf.dwarfWithAddress,
            dwarfSkipped: dwarf.dwarfSkipped,
            dwarfLiveSamplable: dwarf.dwarfLiveSamplable,
            dwarfHaltSamplable: dwarf.dwarfHaltSamplable,
            dwarfPointerExpanded: dwarf.dwarfPointerExpanded,
            dwarfNoLocation: dwarf.dwarfNoLocation,
            dwarfUnits: dwarf.units,
            warnings,
        }
    }

    // ---- 回退：符号表（仅 DATA/OBJECT，无类型信息）----
    // 合并 XRDAP-Host 的做法：保留符号真实大小（1/2/4/8），地址校验优先用
    // SHF_ALLOC 段归属；可重定位 ELF 退回 STM32 可访问窗口。
    const relocatable = image.elfType === ET_REL
    const {symbols} = image.readSymbols()
    const variables: VariableNode[] = []
    const seen = new Set<string>()
    for (const symbol of symbols) {
        const type = symbol.info & ELF32_ST_TYPE_MASK
        if (type !== STT_OBJECT) continue
        if (symbol.shndx === SHN_UNDEF || symbol.shndx === SHN_ABS || symbol.shndx === SHN_COMMON) continue
        if (!symbol.name || isCompilerArtifact(symbol.name)) continue
        const address = symbol.value >>> 0
        const symbolSize = symbol.size >>> 0
        const placed = relocatable
            ? isPlausibleTargetAddress(address)
            : addressInAllocSections(address, image.sections)
        const leafSize = symbolSize === 1 || symbolSize === 2 || symbolSize === 4 || symbolSize === 8
            ? symbolSize
            : 4
        const alignment = Math.max(1, Math.min(leafSize, 4))
        if (address === 0 || !placed || address % alignment !== 0) continue
        const name = displaySymbolName(symbol.name)
        const key = `${name}@${address.toString(16)}`
        if (seen.has(key)) continue
        seen.add(key)
        const leaf: SampleLeaf = {
            address,
            size: leafSize,
            bitOffset: 0,
            bitSize: 0,
            kind: 'uint',
            signed: false,
        }
        variables.push({
            name,
            typeName: leafSize === 4 && symbolSize !== 4 ? `raw/${symbolSize}B` : `u${leafSize * 8}`,
            address,
            size: symbolSize,
            sample: leaf,
        })
    }
    variables.sort((left, right) => left.name.localeCompare(right.name) || left.address - right.address)
    if (variables.length === 0) {
        warnings.push('No DWARF variables and no usable .symtab objects; upload a non-stripped ELF (.elf/.out)')
    }
    return {
        source: 'symtab',
        variables,
        dwarfVariables: dwarf?.dwarfVariables ?? 0,
        dwarfNamed: dwarf?.dwarfNamed ?? 0,
        dwarfWithAddress: dwarf?.dwarfWithAddress ?? 0,
        dwarfSkipped: dwarf?.dwarfSkipped ?? 0,
        dwarfLiveSamplable: dwarf?.dwarfLiveSamplable ?? 0,
        dwarfHaltSamplable: dwarf?.dwarfHaltSamplable ?? 0,
        dwarfPointerExpanded: dwarf?.dwarfPointerExpanded ?? 0,
        dwarfNoLocation: dwarf?.dwarfNoLocation ?? 0,
        dwarfUnits: dwarf?.units ?? 0,
        warnings,
    }
}

// ---------------------------------------------------------------------------
// 兼容旧 ELF 符号表 API（此前的曲线页/单测使用；保留以免破坏调用方）
// ---------------------------------------------------------------------------
export type ElfSymbolType = 'object' | 'func'

export interface ElfSymbol {
    name: string
    address: number
    size: number
    type: ElfSymbolType
    sectionIndex: number
}

export interface ElfParseResult {
    symbols: ElfSymbol[]
    totalSymbols: number
    objectSymbols: number
    funcSymbols: number
    flashSymbols: number
    ramSymbols: number
    filteredSymbols: number
    machine: number
    entry: number
    relocatable: boolean
    warnings: string[]
}

/** 兼容旧接口：全部 OBJECT/FUNC 符号（不做 DWARF）。 */
export function parseElf32Symbols(buffer: ArrayBuffer | Uint8Array): ElfParseResult {
    const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer)
    const image = ElfImage.parse(bytes)
    const warnings: string[] = []
    const sections = image.sections
    const symbols: ElfSymbol[] = []
    const seen = new Set<string>()
    let objectSymbols = 0
    let funcSymbols = 0
    let filteredSymbols = 0
    let flashSymbols = 0
    let ramSymbols = 0
    const {symbols: raw} = image.readSymbols()
    for (const symbol of raw) {
        const type = symbol.info & ELF32_ST_TYPE_MASK
        if (type !== STT_OBJECT && type !== STT_FUNC) continue
        if (symbol.shndx === SHN_UNDEF || symbol.shndx === SHN_ABS || symbol.shndx === SHN_COMMON) {
            filteredSymbols += 1
            continue
        }
        if (type === STT_OBJECT) objectSymbols += 1
        else funcSymbols += 1
        const section = symbol.shndx < sections.length ? sections[symbol.shndx] : null
        if (section && (section.flags & SHF_ALLOC) === 0) {
            filteredSymbols += 1
            continue
        }
        if (!symbol.name) {
            filteredSymbols += 1
            continue
        }
        const address = (image.elfType === ET_REL && section ? section.address + symbol.value : symbol.value) >>> 0
        if (address === 0 || !isPlausibleTargetAddress(address)) {
            filteredSymbols += 1
            continue
        }
        const key = `${symbol.name}@${address.toString(16)}`
        if (seen.has(key)) continue
        seen.add(key)
        if (address < TARGET_RAM_RANGE[0]) flashSymbols += 1
        else ramSymbols += 1
        symbols.push({
            name: symbol.name,
            address,
            size: symbol.size,
            type: type === STT_FUNC ? 'func' : 'object',
            sectionIndex: symbol.shndx,
        })
    }
    if (image.elfType === ET_REL) warnings.push('relocatable ELF: addresses are section-relative')
    symbols.sort((left, right) => left.address - right.address || left.name.localeCompare(right.name))
    return {
        symbols,
        totalSymbols: raw.length,
        objectSymbols,
        funcSymbols,
        flashSymbols,
        ramSymbols,
        filteredSymbols,
        machine: image.machine,
        entry: image.entry,
        relocatable: image.elfType === ET_REL,
        warnings,
    }
}
