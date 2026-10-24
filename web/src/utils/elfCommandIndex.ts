/**
 * P1-17（task-62 数据源）：ELF 解析后的命令面板索引 + 变量聚焦匹配（纯函数）。
 *
 * 与 web-shell（命令面板）的契约（task-61 对齐）：
 *  - `window.__dapCommandIndex = {variables, functions}`，并 dispatch
 *    `dap:command-index` CustomEvent（detail = 同一 index 对象）；
 *  - variables: `{name, path, address, type}`，address 为 number（u32 ELF DIE 地址，
 *    寄存器/栈变量可能为 0）；path 为 DebugCurve 变量树稳定唯一标识（优先 DWARF
 *    qualifiedName，无则 name；重名追加 `@0x<hex>` 去重）；
 *  - functions: `{name, address}`（Flash 内 FUNC 符号，Thumb bit0 已清 0）；
 *  - 命令面板选中变量后跳转 `/#/debug-curve?focusVar=<encodeURIComponent(path)>`；
 *    DebugCurve 也接受 `dap:focus-variable` 自定义事件（detail `{path}`）。
 *
 * 本模块不访问 DOM（dispatch 在调用方做），便于 Node 单测。
 */
import type {VariableNode} from './elfSymbols'
import type {FunctionSymbol} from './elfFunctionSymbols'

/** window 上的命令索引属性名。 */
export const DAP_COMMAND_INDEX_WINDOW_KEY = '__dapCommandIndex'
/** 命令索引更新事件名（CustomEvent.detail = DapCommandIndex）。 */
export const DAP_COMMAND_INDEX_EVENT = 'dap:command-index'
/** web-shell 聚焦变量事件名（CustomEvent.detail = {path}）。 */
export const DAP_FOCUS_VARIABLE_EVENT = 'dap:focus-variable'
/** 命令索引/聚焦匹配的版本号属性名（每次更新 +1，可选用去抖）。 */
export const DAP_COMMAND_INDEX_VERSION_KEY = '__dapCommandIndexVersion'

export interface DapCommandIndexVariable {
    /** 变量显示名（短名）。 */
    name: string
    /** 唯一路径（DebugCurve 变量树语义；用于 ?focusVar= 匹配）。 */
    path: string
    /** ELF DIE 地址（u32；寄存器/栈变量可能为 0）。 */
    address: number
    /** DWARF 类型名（可能为空）。 */
    type: string
}

export interface DapCommandIndexFunction {
    /** 函数显示名（优先 demangle）。 */
    name: string
    /** Flash 地址（Thumb bit0 已清 0）。 */
    address: number
}

export interface DapCommandIndex {
    variables: DapCommandIndexVariable[]
    functions: DapCommandIndexFunction[]
}

/** 空索引（无 ELF/解析失败时发布，命令面板据此隐藏变量/函数段）。 */
export function emptyDapCommandIndex(): DapCommandIndex {
    return {variables: [], functions: []}
}

/**
 * 构建命令索引（纯函数）：
 *  - variables 只取 ELF 顶层变量（与变量树第一层一致；对象子成员由命令面板按
 *    path 前缀搜索/展开，不重复列出以免索引膨胀）；
 *  - path 优先 qualifiedName（如 `Device::BMI088::accel`），否则 name；
 *  - path 冲突时追加 `@0x<hex>`（与 DebugCurve `path@address` 去重键语义一致）。
 */
export function buildDapCommandIndex(
    variables: readonly VariableNode[],
    functions: readonly FunctionSymbol[],
    maxVariables = 20000,
): DapCommandIndex {
    const limit = Math.max(1, Math.floor(Number.isFinite(maxVariables) ? maxVariables : 20000))
    const seen = new Set<string>()
    const entries: DapCommandIndexVariable[] = []
    for (const node of variables) {
        if (entries.length >= limit) break
        const name = node.shortName || node.name || ''
        if (!name) continue
        const qualified = (node.qualifiedName || '').trim()
        let path = qualified || name
        if (seen.has(path)) path = `${path}@0x${(node.address >>> 0).toString(16)}`
        if (seen.has(path)) continue
        seen.add(path)
        entries.push({
            name,
            path,
            address: node.address >>> 0,
            type: node.typeName || '',
        })
    }
    const functionEntries: DapCommandIndexFunction[] = []
    const seenFunctions = new Set<string>()
    for (const symbol of functions) {
        const name = symbol.displayName || symbol.name
        if (!name) continue
        const key = `${(symbol.address >>> 0).toString(16)}@${name}`
        if (seenFunctions.has(key)) continue
        seenFunctions.add(key)
        functionEntries.push({name, address: symbol.address >>> 0})
    }
    return {variables: entries, functions: functionEntries}
}

/**
 * 聚焦变量匹配（纯函数）：先精确 path，再大小写敏感 fallback 到
 * name / path 的 `@0x` 后缀 / 地址，最后大小写不敏感比较。命中返回索引条目。
 */
export function findCommandIndexVariable(
    index: DapCommandIndex | null | undefined,
    focusPath: string,
): DapCommandIndexVariable | null {
    const wanted = String(focusPath ?? '').trim()
    if (!wanted || !index || index.variables.length === 0) return null
    const exact = index.variables.find((entry) => entry.path === wanted)
    if (exact) return exact
    const lower = wanted.toLowerCase()
    const loose = index.variables.find((entry) => entry.path.toLowerCase() === lower)
        ?? index.variables.find((entry) => entry.name === wanted)
        ?? index.variables.find((entry) => entry.name.toLowerCase() === lower)
    if (loose) return loose
    // `path@0x<addr>` 形式的地址后缀匹配（DebugCurve series key 语义）。
    const addressMatch = /@0x([0-9a-f]+)$/i.exec(wanted)
    if (addressMatch) {
        const address = Number.parseInt(addressMatch[1], 16) >>> 0
        const byAddress = index.variables.find((entry) => entry.address >>> 0 === address
            && entry.path.toLowerCase() === wanted.slice(0, addressMatch.index).toLowerCase())
        if (byAddress) return byAddress
    }
    return null
}
