/**
 * 变量曲线页面共享类型：DWARF 解析结果（可采样叶子 + 可展开成员树）。
 *
 * 与 `elfDwarf.ts`（DWARF 解析/类型求解）和 `elfSymbols.ts`（ELF 入口/回退）
 * 配合使用；页面只需要这里的数据结构。
 */

/** 采样值在图表/读数中的语义（由 DWARF 类型推导）。 */
export type ValueKind = 'uint' | 'int' | 'float32' | 'float64' | 'bool' | 'enum' | 'raw'

/** 用户可选的显示格式；auto = 按 DWARF 类型解码。 */
export type DisplayFormat = 'auto' | 'u32' | 'i32' | 'f32'

/**
 * 一个可采样叶子的读取描述。
 *
 * 读取时按 `address` 开始读 `size` 字节（最多 8），若 `bitSize > 0` 则从
 * 读到的字节中按 `bitOffset` 取位域；值按 `kind`/`signed` 解码。
 */
export interface SampleLeaf {
    address: number
    /** 需要读取的字节数（1..8） */
    size: number
    /** 位偏移（相对 address 起始字节；非位域为 0） */
    bitOffset: number
    /** 位宽；0 表示整个 size*8 */
    bitSize: number
    kind: ValueKind
    signed: boolean
    /** enum 值 -> 名称（按值升序），仅 kind==='enum' 时存在 */
    enumValues?: Array<[number, string]>
}

/** 变量浏览树的一个节点（根变量或 struct 成员 / 数组成员）。 */
export interface VariableNode {
    /** 节点名；根为限定名（Namespace::Class::var），成员为 `.member`，数组元素为 `[i]` */
    name: string
    /** 源码短名（DW_AT_name；用于搜索/显示别名） */
    shortName?: string
    /** 完整限定名（Namespace::Class::member；无作用域时同 shortName） */
    qualifiedName?: string
    /** Itanium mangled name（DW_AT_linkage_name，若有） */
    mangledName?: string
    typeName: string
    address: number
    size: number
    /** 源文件完整路径（若 DWARF 提供） */
    file?: string
    /** 相对源文件路径（去掉项目/构建根前缀，如 src/system/FreeRTOS/db.cpp） */
    relativeFile?: string
    line?: number
    /** 是否有固定地址（false = 不可采样：寄存器/栈/被优化/地址无效） */
    sampleable?: boolean
    /** 可采样叶子；聚合节点（struct/array）没有 */
    sample?: SampleLeaf
    /** struct/union 成员或数组成员（展开深度有限） */
    children?: VariableNode[]
}

export interface ElfSectionInfo {
    index: number
    name: string
    type: number
    flags: number
    address: number
    offset: number
    size: number
    link: number
    entsize: number
}

/** `parseElfVariables()` 的结果。 */
export interface ElfVariablesResult {
    /** dwarf：使用 DWARF；symtab：回退到符号表（无类型信息） */
    source: 'dwarf' | 'symtab'
    variables: VariableNode[]
    /** DWARF 中 DW_TAG_variable 总数（含无固定地址的局部变量） */
    dwarfVariables: number
    /** DWARF 中有名字的 DW_TAG_variable 总数（含局部变量） */
    dwarfNamed: number
    /** 其中有固定地址（DW_OP_addr / DW_OP_addrx）的数量 */
    dwarfWithAddress: number
    /** 固定地址但被过滤（地址范围/对齐/无名字/解析失败）的数量 */
    dwarfSkipped: number
    /** 解析到的 CU 数 */
    dwarfUnits: number
    /** 非致命问题（页面可展示为提示） */
    warnings: string[]
}

/** 可采样地址范围：STM32 典型 Flash / RAM（含 DTCM/AXI SRAM）。 */
export const TARGET_FLASH_RANGE: readonly [number, number] = [0x08000000, 0x09000000]
export const TARGET_RAM_RANGE: readonly [number, number] = [0x20000000, 0x24080000]

export function isPlausibleTargetAddress(address: number): boolean {
    const value = address >>> 0
    return (
        (value >= TARGET_FLASH_RANGE[0] && value < TARGET_FLASH_RANGE[1]) ||
        (value >= TARGET_RAM_RANGE[0] && value < TARGET_RAM_RANGE[1])
    )
}

/** 页面展示用：0x + 8 位大写十六进制。 */
export function formatHex32(value: number): string {
    return `0x${(value >>> 0).toString(16).toUpperCase().padStart(8, '0')}`
}
