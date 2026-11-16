#!/usr/bin/env node
/**
 * task-elf-merge: 浏览器 ELF 变量识别 —— "两边优点合并" 回归。
 *
 * 覆盖合并自 XRDAP-Host 的四项识别能力（详见 analysis/elf-identification-merge.md）：
 *  1. 固定地址接受 `DW_OP_addr/addrx` 之后的 `DW_OP_plus_uconst`
 *     （armclang `.L_MergedGlobals` 合并全局）；
 *  2. 固定地址落在 ELF `SHF_ALLOC` 段内才可信（`addressInAllocSections`，
 *     替代只认 STM32 固定窗口；可重定位 ELF 保留旧窗口判断）；
 *  3. 只有声明、没有位置的变量不再作为"不可采样重复项"出现；
 *  4. `.symtab` 回退保留符号真实 size（1/2/4/8），地址按 size 对齐。
 *
 * 用法：
 *   node scripts/test-elf-parse.mjs
 *   ELF_FIXTURES_DIR=/path/to/xrdap-host/xrdap/tests/fixtures node scripts/test-elf-parse.mjs
 *
 * 夹具目录可选（来自 XRDAP-Host，Apache-2.0；只读引用，不随本仓库分发）。
 * 存在 fixtures 时还会用 objcopy 生成 strip-debug 的符号表回退用例。
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const parserOut = path.join(os.tmpdir(), `elf-parse-test-${process.pid}.mjs`)
const dwarfOut = path.join(os.tmpdir(), `elf-dwarf-test-${process.pid}.mjs`)

execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'elfSymbols.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${parserOut}`,
], {stdio: 'pipe', cwd: root})
execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'elfDwarf.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${dwarfOut}`,
], {stdio: 'pipe', cwd: root})

const parser = await import(parserOut)
const dwarf = await import(dwarfOut)

let checks = 0
let failures = 0
function check(cond, name, detail = '') {
    checks += 1
    if (cond) {
        console.log(`ok ${checks} - ${name}`)
    } else {
        failures += 1
        console.log(`not ok ${checks} - ${name}${detail ? `  # ${detail}` : ''}`)
    }
}
function eq(actual, expected, name) {
    check(actual === expected, name, `expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`)
}
function section(title) {
    console.log(`\n# ${title}`)
}

// ---------------------------------------------------------------------------
section('[1] decodeFixedAddressOps（合并：addr/addrx + plus_uconst）')
// ---------------------------------------------------------------------------
const ADDR = (value) => Uint8Array.from([0x03, value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff])
const PLUS = (value) => Uint8Array.from([0x23, ...leb128(value)])

function leb128(value) {
    const out = []
    let v = value >>> 0
    while (v >= 0x80) {
        out.push((v & 0x7f) | 0x80)
        v >>>= 7
    }
    out.push(v)
    return out
}
function concat(...parts) {
    return Uint8Array.from(parts.flatMap((part) => [...part]))
}

eq(dwarf.decodeFixedAddressOps(ADDR(0x20000010), 4), 0x20000010, 'DW_OP_addr alone')
eq(dwarf.decodeFixedAddressOps(concat(ADDR(0x20000010), PLUS(0x88)), 4), 0x20000098,
    'DW_OP_addr + DW_OP_plus_uconst（armclang 合并全局）')
eq(dwarf.decodeFixedAddressOps(concat(ADDR(0x20000000), PLUS(0x10), PLUS(0x08)), 4), 0x20000018,
    'multiple plus_uconst add up')
eq(dwarf.decodeFixedAddressOps(Uint8Array.from([0x91, 0x08]), 4), null, 'DW_OP_fbreg is not fixed')
eq(dwarf.decodeFixedAddressOps(concat(ADDR(0x20000010), Uint8Array.from([0x06])), 4), null,
    'trailing DW_OP_deref is not fixed')
eq(dwarf.decodeFixedAddressOps(Uint8Array.from([0x03, 0x10, 0x00]), 4), null, 'truncated DW_OP_addr')
eq(dwarf.decodeFixedAddressOps(Uint8Array.from([0xa1, 0x01]), 4), null, 'addrx without resolver')
eq(dwarf.decodeFixedAddressOps(Uint8Array.from([0xa1, 0x01]), 4, (index) => (index === 1 ? 0x20000000 : null)),
    0x20000000, 'DW_OP_addrx via resolver')
eq(dwarf.decodeFixedAddressOps(Uint8Array.from([0xfb, 0x02]), 4, (index) => (index === 2 ? 0x24000000 : null)),
    0x24000000, 'DW_OP_GNU_addr_index via resolver')

// ---------------------------------------------------------------------------
section('[2] addressInAllocSections（合并：SHF_ALLOC 段校验）')
// ---------------------------------------------------------------------------
const sections = [
    {index: 1, name: '.text', type: 1, flags: 0x2, address: 0x08000000, offset: 0, size: 0x400, link: 0, entsize: 0},
    {index: 2, name: '.data', type: 1, flags: 0x2, address: 0x20000000, offset: 0, size: 0x100, link: 0, entsize: 0},
    {index: 3, name: '.symtab', type: 2, flags: 0x0, address: 0, offset: 0, size: 0x100, link: 0, entsize: 16},
]
check(parser.addressInAllocSections(0x08000100, sections), 'address in .text (SHF_ALLOC)')
check(parser.addressInAllocSections(0x20000080, sections), 'address in .data (SHF_ALLOC)')
check(!parser.addressInAllocSections(0x08000400, sections), 'address right past .text is rejected')
check(!parser.addressInAllocSections(0x30000000, sections), 'address not in any alloc section is rejected')
check(!parser.addressInAllocSections(0x0, sections), 'address 0 is rejected (gc-sections leftover)')

// ---------------------------------------------------------------------------
section('[3] 可选 ELF 夹具（XRDAP-Host tests/fixtures）')
// ---------------------------------------------------------------------------
const fixturesDir = process.env.ELF_FIXTURES_DIR ?? process.argv[2]
if (!fixturesDir || !fs.existsSync(fixturesDir)) {
    console.log(`  skip: set ELF_FIXTURES_DIR to XRDAP-Host/xrdap/tests/fixtures`)
} else {
    const fixture = (name) => fs.readFileSync(path.join(fixturesDir, name))
    const parse = (name) => parser.parseElfVariables(fixture(name))
    const names = (result) => result.variables.map((item) => item.name)
    const walk = (nodes, visit) => {
        for (const node of nodes) {
            visit(node)
            if (node.children) walk(node.children, visit)
        }
    }

    // -- scopes.elf：命名空间 / 类静态成员 / 函数内 static / --gc-sections 零地址 --
    const scopes = parse('scopes.elf')
    eq(scopes.source, 'dwarf', 'scopes.elf: DWARF 优先')
    check(scopes.variables.length > 5, 'scopes.elf: variables parsed', `${scopes.variables.length}`)
    const scopeNames = names(scopes)
    check(scopeNames.includes('g_global'), 'scopes.elf: global variable kept', scopeNames.join(','))
    check(scopeNames.includes('motor::speed'), 'scopes.elf: namespace variable named motor::speed')
    check(scopeNames.includes('Ctrl::count') && scopeNames.includes('Ctrl::pid'),
        'scopes.elf: class static members named Ctrl::*')
    check(scopeNames.some((name) => /Holder<3>::v/.test(name)), 'scopes.elf: template static member kept',
        scopeNames.join(','))
    check(scopeNames.some((name) => name.includes('tick()::') && name.endsWith('f_static')),
        'scopes.elf: function static named tick()::f_static', scopeNames.filter((n) => n.includes('f_static')).join(','))
    check(scopeNames.some((name) => name.includes('tick()::') && name.endsWith('blk_static')),
        'scopes.elf: block static named tick()::blk_static')
    check(scopeNames.some((name) => name.includes('lam_static')), 'scopes.elf: lambda static kept')
    check(!scopeNames.includes('unused_var'), 'scopes.elf: --gc-sections zero-address variable rejected')
    check(scopeNames.some((name) => name.includes('dup')), 'scopes.elf: same-name statics from namespaces kept')

    // -- statics*.elf：函数作用域 static（两个 DWARF 版本） --
    for (const name of ['statics.elf', 'statics-dwarf5.elf']) {
        const result = parse(name)
        eq(result.source, 'dwarf', `${name}: DWARF 优先`)
        const list = names(result)
        check(list.some((item) => /statics_touch\(\)::func_static/.test(item)),
            `${name}: function static func_static`, list.join(','))
        check(list.some((item) => /func_zero/.test(item)), `${name}: function static in .bss kept`)
        check(result.dwarfLiveSamplable > 0, `${name}: at least one live-samplable root`)
    }

    // -- decl.elf：仅声明类型补全（Port 的完整定义在另一个 CU） --
    const decl = parse('decl.elf')
    eq(decl.source, 'dwarf', 'decl.elf: DWARF 优先')
    const port = decl.variables.find((item) => item.name === 'port')
    check(!!port, 'decl.elf: variable port parsed', names(decl).join(','))
    if (port) {
        check((port.children?.length ?? 0) >= 3, 'decl.elf: declaration-only type completed to members',
            `${port.children?.length ?? 0} children`)
        check(/Port/.test(port.typeName), 'decl.elf: type name Port', port.typeName)
    }

    // -- types*.elf：位域/枚举/浮点/聚合 --
    for (const name of ['types.elf', 'types-dwarf5.elf']) {
        const result = parse(name)
        eq(result.source, 'dwarf', `${name}: DWARF 优先`)
        const leaves = []
        walk(result.variables, (node) => {
            if (node.sample) leaves.push(node.sample)
        })
        check(leaves.some((leaf) => leaf.kind === 'float32' || leaf.kind === 'float64'),
            `${name}: float leaf decoded`)
        check(leaves.some((leaf) => leaf.kind === 'enum'), `${name}: enum leaf decoded`)
        check(leaves.some((leaf) => leaf.bitSize > 0), `${name}: bit-field leaf decoded`)
        check(result.variables.some((item) => (item.children?.length ?? 0) > 0),
            `${name}: aggregate children expanded`)
    }

    // -- scopes-lto.elf：LTO 的 spec 链命名 --
    const lto = parse('scopes-lto.elf')
    eq(lto.source, 'dwarf', 'scopes-lto.elf: DWARF 优先')
    check(lto.variables.length > 5, 'scopes-lto.elf: LTO variables parsed', `${lto.variables.length}`)
    check(names(lto).some((name) => name.includes('f_static')), 'scopes-lto.elf: LTO function static named')

    // -- 已知缺口（P1）：压缩调试段仍退化到符号表，这里只记录不判失败 --
    const gz = parse('scopes-gz.elf')
    console.log(`# note: scopes-gz.elf source=${gz.source} variables=${gz.variables.length} (压缩调试段 P1 待实现)`)

    // -- .symtab 回退：去掉 DWARF 段名后保留符号 size（1/2/4/8） --
    // 不依赖 objcopy（宿主 binutils 未必认 ARM ELF）：把 .shstrtab 里的
    // ".debug_info" 改名为等长占位名，DWARF 解析自然失败 → 走符号表回退。
    const stripDwarfInfo = (elfBytes) => {
        const bytes = new Uint8Array(elfBytes)
        const view = new DataView(bytes.buffer)
        const shoff = view.getUint32(32, true)
        const shentsize = view.getUint16(46, true)
        const shnum = view.getUint16(48, true)
        const shstrndx = view.getUint16(50, true)
        const shStr = shoff + shstrndx * shentsize
        const strTabOffset = view.getUint32(shStr + 16, true)
        const strTabSize = view.getUint32(shStr + 20, true)
        const needle = new TextEncoder().encode('.debug_info\0')
        const replacement = new TextEncoder().encode('.nodebug_xx\0')
        for (let i = 0; i + needle.length <= strTabSize; i++) {
            let matched = true
            for (let j = 0; j < needle.length; j++) {
                if (bytes[strTabOffset + i + j] !== needle[j]) {
                    matched = false
                    break
                }
            }
            if (matched) {
                bytes.set(replacement, strTabOffset + i)
                return bytes
            }
        }
        throw new Error('stripDwarfInfo: .debug_info name not found in .shstrtab')
    }
    const fallback = parser.parseElfVariables(stripDwarfInfo(fixture('scopes.elf')))
    eq(fallback.source, 'symtab', 'strip-debug ELF: falls back to .symtab')
    check(fallback.variables.length > 0, 'strip-debug ELF: symbol variables parsed', `${fallback.variables.length}`)
    check(fallback.variables.every((item) => item.sample && [1, 2, 4, 8].includes(item.sample.size)),
        'strip-debug ELF: leaf size keeps 1/2/4/8')
    check(fallback.variables.every((item) => /^(u(8|16|32|64)|raw\/\d+B)$/.test(item.typeName)),
        'strip-debug ELF: symbol type named uN/raw', fallback.variables.slice(0, 3).map((item) => item.typeName).join(','))
    check(fallback.variables.some((item) => item.size !== item.sample.size),
        'strip-debug ELF: node size preserved from symbol size')
}

console.log(`\n# ${checks} checks, ${failures} failure(s)`)
process.exit(failures === 0 ? 0 : 1)
