/**
 * 变量曲线页面共享类型：DWARF 解析结果（可采样叶子 + 可展开成员树）。
 *
 * 与 `elfDwarf.ts`（DWARF 解析/类型求解）和 `elfSymbols.ts`（ELF 入口/回退）
 * 配合使用；页面只需要这里的数据结构。
 */

/** 采样值在图表/读数中的语义（由 DWARF 类型推导）。 */
export type ValueKind = 'uint' | 'int' | 'float32' | 'float64' | 'bool' | 'enum' | 'raw'

/**
 * 调试采样定位表达式：目标暂停时，由核心寄存器/帧基址/指针解引用计算地址。
 *
 * - addr：固定绝对地址（静态变量/全局变量）；
 * - reg：核心寄存器值 + offset（DW_OP_bregN / DW_OP_fbreg 展开后的帧基址）；
 * - cfa：近似 CFA（用 SP=r13 代替，DW_OP_call_frame_cfa）；
 * - add：内部地址加偏移（struct 成员 / 数组元素）；
 * - deref：读取 inner 地址处的 u32 指针值再加 offset（指针成员 / 指针链）。
 *
 * live 采样（不暂停目标）只能使用 addr/add/deref；含 reg/cfa 的表达式必须
 * 在调试模式（halt 后读寄存器）使用。
 */
export type AddressExpr =
    | {kind: 'addr'; address: number}
    | {kind: 'reg'; reg: number; offset: number}
    | {kind: 'cfa'; offset: number}
    | {kind: 'add'; inner: AddressExpr; offset: number}
    | {kind: 'deref'; inner: AddressExpr; offset: number}

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
    /** 调试采样读取位置（存在时 address 不可用，需暂停目标后按表达式取地址） */
    location?: AddressExpr
    /** 值直接来自核心寄存器（DW_OP_regN；调试采样读取该寄存器） */
    register?: number
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
    /** 是否可采样（固定地址 / 调试帧位置 / 指针成员任意一种） */
    sampleable?: boolean
    /** live = 实时固定地址；halt = 需暂停目标读帧/寄存器/指针 */
    samplingMode?: 'live' | 'halt'
    /** 调试/指针采样定位表达式（不暂停的 live 指针链也使用） */
    location?: AddressExpr
    /** 该节点是否可在实时（不暂停）模式采样；固定地址/仅含 addr 的指针链为 true */
    liveSamplable?: boolean
    /** 指针解引用层数（0/undefined = 非指针链节点） */
    pointerDepth?: number
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
    /** 根变量中可在实时模式采样的数量（固定地址 / 无寄存器的指针链） */
    dwarfLiveSamplable: number
    /** 根变量中需调试模式（暂停读帧/寄存器）采样的数量 */
    dwarfHaltSamplable: number
    /** 因指针解引用而展开出的可采样节点数量 */
    dwarfPointerExpanded: number
    /** 有名字但完全没有可用位置（寄存器/栈/被优化/loclist 不支持）的根变量数量 */
    dwarfNoLocation: number
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
