/**
 * DWARF 调试采样定位表达式的纯函数求值工具。
 *
 * 解析器（elfDwarf.ts）把 DW_OP_fbreg/breg/reg/addr 及指针成员展开为
 * `AddressExpr`。实时模式（live）只能处理不含核心寄存器的表达式；
 * 调试模式（halt）在暂停目标后按寄存器值求值。
 *
 * 这里只做纯计算（寄存器用 Map，内存用同步 readWord 回调），页面/测试各自
 * 提供真实/模拟的读取实现。
 */
import type {AddressExpr} from './elfTypes'

/** CFA（DW_OP_call_frame_cfa）近似使用 SP=r13。 */
export const CFA_FRAME_REG = 13

/** 表达式依赖的核心寄存器编号（去重升序；CFA 记为 SP=r13）。 */
export function addressExprRegisters(expr: AddressExpr, out: Set<number> = new Set()): Set<number> {
    switch (expr.kind) {
        case 'reg':
            out.add(expr.reg)
            break
        case 'cfa':
            out.add(CFA_FRAME_REG)
            break
        case 'add':
        case 'deref':
            addressExprRegisters(expr.inner, out)
            break
        default:
            break
    }
    return out
}

/** 表达式是否可在不暂停目标的实时模式求值（只含绝对地址/成员偏移/指针解引用）。 */
export function addressExprIsLive(expr: AddressExpr): boolean {
    switch (expr.kind) {
        case 'addr':
            return true
        case 'reg':
        case 'cfa':
            return false
        case 'add':
        case 'deref':
            return addressExprIsLive(expr.inner)
        default:
            return false
    }
}

/** 人读形式，用于变量列表/曲线 tooltip：`[r7+48]`、`*[0x24000000]+4`。 */
export function addressExprText(expr: AddressExpr): string {
    switch (expr.kind) {
        case 'addr':
            return `0x${(expr.address >>> 0).toString(16).toUpperCase().padStart(8, '0')}`
        case 'reg':
            return `[r${expr.reg}${signedSuffix(expr.offset)}]`
        case 'cfa':
            return `[SP${signedSuffix(expr.offset)}]`
        case 'add':
            return `${addressExprText(expr.inner)}${signedSuffix(expr.offset)}`
        case 'deref':
            return `*${addressExprText(expr.inner)}${signedSuffix(expr.offset)}`
        default:
            return '?'
    }
}

function signedSuffix(value: number): string {
    if (!value) return ''
    return value >= 0 ? `+${value}` : `${value}`
}

/**
 * 同步求值：registers 为寄存器编号 -> 值；readWord 读取地址处的 32-bit 指针值。
 * 表达式非法（未提供寄存器）时抛错，由调用方转成采样失败。
 */
export function evaluateAddress(
    expr: AddressExpr,
    registers: ReadonlyMap<number, number>,
    readWord: (address: number) => number,
): number {
    switch (expr.kind) {
        case 'addr':
            return expr.address >>> 0
        case 'reg': {
            const value = registers.get(expr.reg)
            if (value === undefined) throw new Error(`missing core register r${expr.reg}`)
            return (value + expr.offset) >>> 0
        }
        case 'cfa': {
            const value = registers.get(CFA_FRAME_REG)
            if (value === undefined) throw new Error('missing core register SP (r13) for CFA')
            return (value + expr.offset) >>> 0
        }
        case 'add':
            return (evaluateAddress(expr.inner, registers, readWord) + expr.offset) >>> 0
        case 'deref': {
            const pointerAddress = evaluateAddress(expr.inner, registers, readWord)
            return (readWord(pointerAddress) + expr.offset) >>> 0
        }
        default:
            throw new Error('unsupported DWARF address expression')
    }
}
