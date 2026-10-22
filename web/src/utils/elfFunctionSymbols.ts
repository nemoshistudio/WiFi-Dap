/**
 * ELF 函数符号 -> 本地断点（FPB）函数选择器数据。
 *
 * 复用 `elfSymbols.parseElf32Symbols()`：只保留 Flash 中的 FUNC 符号，
 * Thumb bit0 清 0，去重排序后用 `demangleItanium()` 显示 C++ 符号名。
 * 该模块是纯函数（除 `buildFunctionSymbolsFromElf` 解析 ELF 外无副作用），
 * 便于单测覆盖过滤/排序/反混淆 fallback/选函数填地址/offset 边界。
 */
import {
    demangleItanium,
    parseElf32Symbols,
    type ElfSymbol,
} from './elfSymbols'
import {formatHex32, TARGET_FLASH_RANGE} from './elfTypes'
import {
    isValidBreakpointAddress,
    normalizeBreakpointAddress,
    parseBreakpointAddressInput,
} from '../api/curveLocal'

/** 函数下拉最多保留的符号数（避免超大 ELF 卡顿；按地址升序截断）。 */
export const MAX_BREAKPOINT_FUNCTION_SYMBOLS = 2000

/** 断点函数选择项。 */
export interface FunctionSymbol {
    /** ELF 原始符号名（C++ 为 mangled，例如 _ZN...）。 */
    name: string
    /** 显示名：Itanium demangle 成功则用之，否则 fallback 原符号名。 */
    displayName: string
    /** Flash 地址（Thumb bit0 已清 0）。 */
    address: number
    size: number
    /** 原符号地址是否带 Thumb bit0。 */
    thumb: boolean
}

export interface BuildFunctionSymbolsOptions {
    demangle?: (name: string) => string | null
    max?: number
}

type RawSymbol = Pick<ElfSymbol, 'name' | 'address' | 'size' | 'type'>

/**
 * 过滤/规范化/去重/排序函数符号。
 * - 仅 `type==='func'`、地址非 0、在目标 Flash（0x08000000..0x08FFFFFF）；
 * - `address = raw & 0xFFFFFFFE`（Thumb bit0 忽略）；
 * - 同地址同名去重；按地址升序、同地址按显示名排序；最多 max 项。
 */
export function buildFunctionSymbols(
    symbols: readonly RawSymbol[],
    options: BuildFunctionSymbolsOptions = {},
): FunctionSymbol[] {
    const demangle = options.demangle ?? demangleItanium
    const max = Math.max(1, Math.floor(options.max ?? MAX_BREAKPOINT_FUNCTION_SYMBOLS))
    const seen = new Set<string>()
    const result: FunctionSymbol[] = []
    for (const symbol of symbols) {
        if (symbol.type !== 'func') continue
        const name = String(symbol.name ?? '')
        if (!name) continue
        const rawAddress = symbol.address >>> 0
        const address = (rawAddress & 0xfffffffe) >>> 0
        if (address === 0) continue
        if (address < TARGET_FLASH_RANGE[0] || address >= TARGET_FLASH_RANGE[1]) continue
        const key = `${address.toString(16)}@${name}`
        if (seen.has(key)) continue
        seen.add(key)
        const demangled = demangle(name)
        result.push({
            name,
            displayName: demangled && demangled.trim() ? demangled : name,
            address,
            size: symbol.size >>> 0,
            thumb: (rawAddress & 1) === 1,
        })
    }
    result.sort((left, right) => left.address - right.address || left.displayName.localeCompare(right.displayName))
    return result.slice(0, max)
}

/** 解析 ELF 并构建函数选择列表（解析失败返回空列表，由页面提示）。 */
export function buildFunctionSymbolsFromElf(buffer: ArrayBuffer | Uint8Array): FunctionSymbol[] {
    try {
        return buildFunctionSymbols(parseElf32Symbols(buffer).symbols)
    } catch {
        return []
    }
}

/**
 * 下拉项文本：`0x08001234  demangledName`；mangled 名与显示名不同时追加
 * `[mangled]`，使 filterable 搜索能同时匹配地址/ demangled / mangled。
 */
export function formatFunctionOption(symbol: FunctionSymbol): string {
    const base = `${formatHex32(symbol.address)}  ${symbol.displayName}`
    return symbol.name !== symbol.displayName ? `${base}  [${symbol.name}]` : base
}

/** 根据地址选函数（地址 Thumb bit0 忽略）；找不到返回 null。 */
export function chooseBreakpointFunction(functions: readonly FunctionSymbol[], address: number): FunctionSymbol | null {
    const normalized = normalizeBreakpointAddress(address)
    return functions.find((symbol) => symbol.address === normalized) ?? null
}

/** 断点地址展示格式：0x%08X（大写、bit0 清 0）。 */
export function formatBreakpointAddress(address: number): string {
    return formatHex32(normalizeBreakpointAddress(address))
}

/**
 * 解析 Offset 输入（可选）：空 = 0；`0x...` 按十六进制，纯数字按十进制；
 * 非负且 <= 0x10000000，否则返回 null。
 */
export function parseBreakpointOffsetInput(text: string): number | null {
    const raw = String(text ?? '').trim()
    if (!raw) return 0
    let value: number
    if (/^0[xX][0-9a-fA-F]+$/.test(raw)) {
        value = Number.parseInt(raw.slice(2), 16)
    } else if (/^\d+$/.test(raw)) {
        value = Number.parseInt(raw, 10)
    } else {
        return null
    }
    if (!Number.isSafeInteger(value) || value < 0 || value > 0x10000000) return null
    return value
}

/** 断点最终地址 = 函数/地址 + offset（Thumb bit0 清 0）。 */
export function applyBreakpointOffset(address: number, offset: number): number {
    return ((normalizeBreakpointAddress(address) + Math.trunc(offset)) & 0xfffffffe) >>> 0
}

export type BreakpointResolveFailure = 'address' | 'offset' | 'range'

export type BreakpointResolveResult =
    | {ok: true; address: number}
    | {ok: false; reason: BreakpointResolveFailure}

/**
 * 组合校验：地址输入（可手工/函数自动填）+ 可选 Offset -> 最终 FPB 地址。
 * 地址非法 -> 'address'；Offset 非法 -> 'offset'；相加后超出 Flash -> 'range'。
 */
export function resolveBreakpointWithOffset(addressText: string, offsetText: string): BreakpointResolveResult {
    const base = parseBreakpointAddressInput(addressText)
    if (base === null) return {ok: false, reason: 'address'}
    const offset = parseBreakpointOffsetInput(offsetText)
    if (offset === null) return {ok: false, reason: 'offset'}
    const address = applyBreakpointOffset(base, offset)
    if (!isValidBreakpointAddress(address)) return {ok: false, reason: 'range'}
    return {ok: true, address}
}
