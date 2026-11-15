#!/usr/bin/env node
/* eslint-env node */
/**
 * task-17 测试：DWARF 位置列表独立解码模块（web/src/utils/dwarfLocList.ts）。
 *
 * 覆盖：
 *  1) synthetic `.debug_loc`（DWARF 2/3/4）block：普通条目、base address selection、
 *     (0,0) terminator、无 base 时的初始 base、16 位表达式长度、截断/越界/坏长度、
 *     32/64-bit 地址、防御性 maxEntries；
 *  2) synthetic `.debug_loclists`（DWARF 5）block：header 解析（32/64-bit DWARF、
 *     offset table、segment selector）、DW_LLE_* 全部 0x00–0x08 opcode、
 *     GNU view pair（0x09）、通过 .debug_addr 的 address index 解析、
 *     offset_pair 的 base 换算、default_location、未知 opcode/截断；
 *  3) findLocationListEntry 的 PC 选择语义（半开区间 / default_location / 无命中）；
 *  4) 真实 ELF：只读提取 reference（clang 18 DWARF4）的 `.debug_loc`，
 *     用系统 readelf --debug-dump=loc 做交叉验证（readelf 不可用时跳过该项）。
 *
 * 只读：不修改仓库/设备，不打包、不 OTA、不 commit。临时产物只写 os.tmpdir()。
 *
 * 用法：node scripts/test-dwarf-loclists.mjs [elf-path]
 *   默认 ELF: ~/firmware/reference/firmware.elf
 *   ELF/readelf 不存在时对应项输出 [skip] 并以 0 退出（不阻塞无固件环境）。
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const elfPath = process.argv[2] ?? process.env.DWARF_ELF ?? '~/firmware/reference/firmware.elf'

// ---------------------------------------------------------------------------
// 用项目自带 esbuild 打包 TS 模块（离线，避免引入额外依赖）
// ---------------------------------------------------------------------------
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
if (!fs.existsSync(esbuild)) {
    console.error(`esbuild not found: ${esbuild} (run npm install in web/)`)
    process.exit(1)
}
const bundleFile = path.join(os.tmpdir(), `dwarf-loclists-${process.pid}.mjs`)
execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'dwarfLocList.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${bundleFile}`,
], {stdio: 'pipe'})

const {
    decodeLocationList,
    parseLocationListHeader,
    resolveLocationListIndex,
    findLocationListEntry,
    locationListCoversPc,
    LocListError,
    DW_LLE_END_OF_LIST,
    DW_LLE_BASE_ADDRESSX,
    DW_LLE_STARTX_ENDX,
    DW_LLE_STARTX_LENGTH,
    DW_LLE_OFFSET_PAIR,
    DW_LLE_DEFAULT_LOCATION,
    DW_LLE_BASE_ADDRESS,
    DW_LLE_START_END,
    DW_LLE_START_LENGTH,
    DW_LLE_GNU_VIEW_PAIR,
} = await import(bundleFile)

let failures = 0
const check = (name, cond, detail = '') => {
    if (cond) console.log(`  ok   ${name}`)
    else {
        console.log(`  FAIL ${name}${detail ? ' :: ' + detail : ''}`)
        failures++
    }
}
const checkEq = (name, actual, expected) => {
    check(name, actual === expected, `actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)}`)
}

// ---------------------------------------------------------------------------
// 合成字节工具
// ---------------------------------------------------------------------------
const hex = (value) => `0x${(value >>> 0).toString(16)}`
const le = (value, size) => {
    const out = new Uint8Array(size)
    for (let i = 0; i < size; i++) out[i] = Math.floor(value / Math.pow(2, i * 8)) & 0xff
    return out
}
const u8 = (value) => le(value, 1)
const u16 = (value) => le(value, 2)
const u32 = (value) => le(value, 4)
const u64 = (value) => le(value, 8)
const addr = (value, size = 4) => le(value, size)
const uleb = (value) => {
    const out = []
    let rest = value
    do {
        let byte = rest & 0x7f
        rest = Math.floor(rest / 128)
        if (rest) byte |= 0x80
        out.push(byte)
    } while (rest)
    return Uint8Array.from(out)
}
const cat = (...parts) => {
    const arrays = parts.flat().map((part) => (typeof part === 'number' ? Uint8Array.of(part) : part))
    const total = arrays.reduce((sum, part) => sum + part.length, 0)
    const out = new Uint8Array(total)
    let offset = 0
    for (const part of arrays) {
        out.set(part, offset)
        offset += part.length
    }
    return out
}
const exprOf = (bytes) => cat(uleb(bytes.length), bytes)
const sameBytes = (actual, expected) =>
    actual.length === expected.length && expected.every((value, index) => actual[index] === value)

// .debug_loclists CU contribution：header（12/20 字节）+ offset table + 列表本体。
const locListsSection = ({body, addrSize = 4, seg = 0, offsets = [], dw64 = false}) => {
    const table = offsets.flatMap((value) => (dw64 ? [u64(value)] : [u32(value)]))
    const inner = cat(u16(5), u8(addrSize), u8(seg), u32(offsets.length), ...table, body)
    const lengthField = dw64 ? cat(u32(0xffffffff), u64(inner.length)) : u32(inner.length)
    return cat(lengthField, inner)
}

// ---------------------------------------------------------------------------
// 1. synthetic .debug_loc（DWARF 2/3/4）
// ---------------------------------------------------------------------------
console.log('unit: .debug_loc (DWARF 4)')
{
    // base selection -> 两条区间 -> terminator
    const section = cat(
        addr(0xffffffff), addr(0x80000000), // base = 0x80000000
        addr(0x10), addr(0x20), u16(2), [0x91, 0x10],
        addr(0x20), addr(0x30), u16(1), [0x50],
        addr(0), addr(0),
    )
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    checkEq('debug_loc: terminated', list.terminated, true)
    checkEq('debug_loc: entry count', list.entries.length, 2)
    checkEq('debug_loc: base address applied', list.entries[0].lowPc, 0x80000010)
    checkEq('debug_loc: end address applied', list.entries[0].highPc, 0x80000020)
    checkEq('debug_loc: second range', `${hex(list.entries[1].lowPc)}..${hex(list.entries[1].highPc)}`, '0x80000020..0x80000030')
    check('debug_loc: expression bytes kept verbatim', sameBytes(list.entries[0].expr, [0x91, 0x10]))
    checkEq('debug_loc: endOffset after terminator', list.endOffset, section.length)
    checkEq('debug_loc: final base address', list.baseAddress, 0x80000000)
    checkEq('debug_loc: no warnings', list.warnings.length, 0)
}
{
    // 无 base selection：初始 baseAddress 生效（readelf 用 CU DW_AT_low_pc 作为初值）
    const section = cat(addr(0x1004), addr(0x1008), u16(1), [0x03], addr(0), addr(0))
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4, baseAddress: 0x4000})
    checkEq('debug_loc: initial baseAddress applied', list.entries[0].lowPc, 0x5004)
    checkEq('debug_loc: initial base high', list.entries[0].highPc, 0x5008)
}
{
    // base selection 到 0：之后条目即绝对地址
    const section = cat(addr(0xffffffff), addr(0), addr(0x4000), addr(0x4010), u16(0), addr(0), addr(0))
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    checkEq('debug_loc: base 0 range', `${hex(list.entries[0].lowPc)}..${hex(list.entries[0].highPc)}`, '0x4000..0x4010')
    checkEq('debug_loc: empty expression allowed', list.entries[0].expr.length, 0)
}
{
    // 空列表：只有 terminator
    const section = cat(addr(0), addr(0))
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    check('debug_loc: empty list terminated', list.terminated)
    checkEq('debug_loc: empty list entries', list.entries.length, 0)
    checkEq('debug_loc: empty list endOffset', list.endOffset, 8)
}
{
    // 从节中间 offset 开始解码 + 前缀不参与
    const listBytes = cat(addr(0x10), addr(0x20), u16(2), [0xaa, 0xbb], addr(0), addr(0))
    const section = cat([0xde, 0xad, 0xbe, 0xef], listBytes)
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 4, addrSize: 4})
    checkEq('debug_loc: decode at nonzero offset', list.entries[0].lowPc, 0x10)
    checkEq('debug_loc: startOffset recorded', list.startOffset, 4)
    checkEq('debug_loc: endOffset absolute section offset', list.endOffset, 4 + listBytes.length)
}
{
    // 地址 8 字节（x86-64 风格）：base selection 用 8 个 0xff 识别
    const maxAddr64 = Uint8Array.from({length: 8}, () => 0xff)
    const section = cat(
        maxAddr64, addr(0x400000, 8),
        addr(0x10, 8), addr(0x20, 8), u16(1), [0x9f],
        addr(0, 8), addr(0, 8),
    )
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 8})
    checkEq('debug_loc: 64-bit base selection', list.baseAddress, 0x400000)
    checkEq('debug_loc: 64-bit range low', list.entries[0].lowPc, 0x400010)
    checkEq('debug_loc: 64-bit range high', list.entries[0].highPc, 0x400020)
    check('debug_loc: 64-bit terminated', list.terminated)
}
{
    // 16-bit 表达式长度是 2-byte LE：0x0102 = 258 字节（而不是 ULEB）
    const expr = Uint8Array.from({length: 258}, (_, index) => index & 0xff)
    const section = cat(addr(1), addr(2), u16(258), expr, addr(0), addr(0))
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    checkEq('debug_loc: u16 expression length (258)', list.entries[0].expr.length, 258)
    check('debug_loc: u16 expression bytes', sameBytes(list.entries[0].expr, expr))
}
{
    // 截断：只有半个条目
    const section = cat(addr(1), addr(2), [0x91])
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    check('debug_loc: truncated length -> warning', list.warnings.length > 0)
    check('debug_loc: truncated list not terminated', list.terminated === false)
    checkEq('debug_loc: truncated keeps decoded entries', list.entries.length, 0)
}
{
    // 表达式长度超出剩余字节
    const section = cat(addr(1), addr(2), u16(64), [0x91, 0x02], addr(0), addr(0))
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    check('debug_loc: oversize expr length -> warning', list.warnings.some((warning) => warning.includes('expression length')))
    checkEq('debug_loc: oversize expr no entries', list.entries.length, 0)
}
{
    // 没有 terminator
    const section = cat(addr(1), addr(2), u16(1), [0x91])
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    check('debug_loc: missing terminator -> warning', list.warnings.some((warning) => warning.includes('not terminated')))
    checkEq('debug_loc: missing terminator still yields entry', list.entries.length, 1)
}
{
    // 非法 / 越界 offset：不抛异常，给 warnings
    const section = cat(addr(0), addr(0))
    const outOfRange = decodeLocationList({kind: 'debug_loc', data: section, offset: 99, addrSize: 4})
    check('debug_loc: offset outside section -> warning', outOfRange.warnings.length > 0)
    check('debug_loc: offset outside section -> no entries', outOfRange.entries.length === 0)
    const atEnd = decodeLocationList({kind: 'debug_loc', data: section, offset: section.length, addrSize: 4})
    check('debug_loc: offset at section end handled', atEnd.entries.length === 0 && atEnd.warnings.length > 0)
}
{
    // maxEntries 防御
    const section = cat(
        ...[1, 2, 3, 4].flatMap((value) => cat(addr(value), addr(value + 1), u16(0))),
        addr(0), addr(0),
    )
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4, maxEntries: 2})
    checkEq('debug_loc: maxEntries respected', list.entries.length, 2)
    check('debug_loc: maxEntries warning', list.warnings.some((warning) => warning.includes('maxEntries')))
}
{
    // 参数校验：编程错误抛 TypeError
    let threw = false
    try {
        decodeLocationList({kind: 'debug_rnglists', data: Uint8Array.of(0), offset: 0})
    } catch (error) {
        threw = error instanceof TypeError
    }
    check('debug_loc: invalid kind throws TypeError', threw)
    threw = false
    try {
        decodeLocationList({kind: 'debug_loc', data: Uint8Array.of(0), offset: 0, addrSize: 0})
    } catch (error) {
        threw = error instanceof TypeError
    }
    check('debug_loc: invalid addrSize throws TypeError', threw)
}

// ---------------------------------------------------------------------------
// 2. synthetic .debug_loclists（DWARF 5）
// ---------------------------------------------------------------------------
console.log('\nunit: .debug_loclists (DWARF 5)')
{
    // header：无 offset table
    const section = locListsSection({body: cat(u8(DW_LLE_END_OF_LIST))})
    const header = parseLocationListHeader(section)
    checkEq('loclists header: version', header.version, 5)
    checkEq('loclists header: addrSize', header.addrSize, 4)
    checkEq('loclists header: segmentSelectorSize', header.segmentSelectorSize, 0)
    checkEq('loclists header: offsetEntryCount', header.offsetEntryCount, 0)
    checkEq('loclists header: listsBase (12-byte header)', header.listsBase, 12)
    checkEq('loclists header: endOffset', header.endOffset, section.length)
    checkEq('loclists header: dw64 false', header.dw64, false)
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: header.listsBase, addrSize: 4})
    check('loclists: immediate end_of_list', list.terminated && list.entries.length === 0)
    checkEq('loclists: endOffset after 1-byte terminator', list.endOffset, header.listsBase + 1)
}
{
    // header：offset table（2 项）
    const body = cat(
        u8(DW_LLE_START_LENGTH), addr(0x1000), uleb(4), exprOf([0x50]), u8(DW_LLE_END_OF_LIST),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body, offsets: [0, 0x10]})
    const header = parseLocationListHeader(section)
    checkEq('loclists header: offset entries parsed', header.offsets.join(','), '0,16')
    checkEq('loclists header: listsBase with table', header.listsBase, 12 + 2 * 4)
    checkEq('loclists: resolve index 0', resolveLocationListIndex(header, 0), header.listsBase)
    checkEq('loclists: resolve index 1', resolveLocationListIndex(header, 1), header.listsBase + 0x10)
    let threw = false
    try {
        resolveLocationListIndex(header, 2)
    } catch (error) {
        threw = error instanceof LocListError
    }
    check('loclists: resolve out-of-range index throws LocListError', threw)
}
{
    // header：64-bit DWARF 格式（unit_length=0xffffffff + u64；offset table 每项 8 字节）
    const body = cat(u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body, offsets: [8], dw64: true})
    const header = parseLocationListHeader(section)
    checkEq('loclists header: dw64 detected', header.dw64, true)
    checkEq('loclists header: dw64 listsBase', header.listsBase, 20 + 8)
    checkEq('loclists header: dw64 offset entry', header.offsets[0], 8)
    checkEq('loclists header: dw64 endOffset', header.endOffset, section.length)
}
{
    // header 非法：version 非 5 / 长度越界 / 空数据
    const badVersion = cat(u32(10), u16(4), u8(4), u8(0), u32(0), [0, 0, 0, 0])
    let error = null
    try {
        parseLocationListHeader(badVersion)
    } catch (caught) {
        error = caught
    }
    check('loclists header: bad version throws LocListError', error instanceof LocListError)
    error = null
    try {
        parseLocationListHeader(cat(u32(1000), u16(5)))
    } catch (caught) {
        error = caught
    }
    check('loclists header: truncated contribution throws LocListError', error instanceof LocListError)
    error = null
    try {
        parseLocationListHeader(Uint8Array.of(0))
    } catch (caught) {
        error = caught
    }
    check('loclists header: short data throws LocListError', error instanceof LocListError)
}
{
    // DW_LLE_base_address(0x06) + offset_pair(0x04)
    const body = cat(
        u8(DW_LLE_BASE_ADDRESS), addr(0x80000000),
        u8(DW_LLE_OFFSET_PAIR), uleb(0x10), uleb(0x20), exprOf([0x91, 0x10]),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('loclists: base_address -> offset_pair low', list.entries[0].lowPc, 0x80000010)
    checkEq('loclists: base_address -> offset_pair high', list.entries[0].highPc, 0x80000020)
    checkEq('loclists: offset_pair kind', list.entries[0].kind, 'offset_pair')
    checkEq('loclists: offset_pair base address', list.baseAddress, 0x80000000)
    check('loclists: offset_pair terminated', list.terminated)
}
{
    // DW_LLE_start_end(0x07)：绝对地址，不受 base 影响
    const body = cat(
        u8(DW_LLE_BASE_ADDRESS), addr(0x80000000),
        u8(DW_LLE_START_END), addr(0x4000), addr(0x4010), exprOf([0x50]),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('loclists: start_end low', list.entries[0].lowPc, 0x4000)
    checkEq('loclists: start_end high', list.entries[0].highPc, 0x4010)
    checkEq('loclists: start_end kind', list.entries[0].kind, 'address_range')
}
{
    // DW_LLE_start_length(0x08)
    const body = cat(u8(DW_LLE_START_LENGTH), addr(0x2000), uleb(0x100), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('loclists: start_length high = start + length', list.entries[0].highPc, 0x2100)
}
{
    // DW_LLE_default_location(0x05)
    const body = cat(u8(DW_LLE_DEFAULT_LOCATION), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('loclists: default_location kind', list.entries[0].kind, 'default_location')
    check('loclists: default_location has no range', list.entries[0].lowPc === undefined && list.entries[0].highPc === undefined)
}
{
    // .debug_addr + DW_LLE_startx_endx(0x02) / startx_length(0x03)
    const debugAddr = cat(addr(0xdead0000), addr(0xdead1000), addr(0xdead2000))
    const body = cat(
        u8(DW_LLE_STARTX_ENDX), uleb(0), uleb(2), exprOf([0x50]),
        u8(DW_LLE_STARTX_LENGTH), uleb(1), uleb(0x40), exprOf([0x51]),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body, addrSize: 4})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4, addr: debugAddr, addrBase: 0})
    checkEq('loclists: startx_endx low from .debug_addr', list.entries[0].lowPc, 0xdead0000)
    checkEq('loclists: startx_endx high from .debug_addr', list.entries[0].highPc, 0xdead2000)
    checkEq('loclists: startx_length low from .debug_addr', list.entries[1].lowPc, 0xdead1000)
    checkEq('loclists: startx_length high = low + length', list.entries[1].highPc, 0xdead1040)
}
{
    // .debug_addr 带 addrBase 偏移（DW_AT_addr_base 指向第一条地址条目）
    const debugAddr = cat([0, 0, 0, 0, 0, 0, 0, 0], addr(0x11110000), addr(0x11112000))
    const body = cat(u8(DW_LLE_STARTX_ENDX), uleb(0), uleb(1), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4, addr: debugAddr, addrBase: 8})
    checkEq('loclists: addrBase offset low', list.entries[0].lowPc, 0x11110000)
    checkEq('loclists: addrBase offset high', list.entries[0].highPc, 0x11112000)
}
{
    // .debug_addr segment selector：每条目 stride = addrSize + segmentSelectorSize
    const debugAddr = cat(addr(0xa000), [0x01, 0x00], addr(0xb000), [0x02, 0x00])
    const body = cat(u8(DW_LLE_STARTX_ENDX), uleb(0), uleb(1), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body, seg: 2})
    const list = decodeLocationList({
        kind: 'debug_loclists', data: section, offset: 12, addrSize: 4,
        addr: debugAddr, addrBase: 0, segmentSelectorSize: 2,
    })
    checkEq('loclists: segment selector stride low', list.entries[0].lowPc, 0xa000)
    checkEq('loclists: segment selector stride high', list.entries[0].highPc, 0xb000)
}
{
    // DW_LLE_base_addressx(0x01) 设置 base，后续 offset_pair 使用
    const debugAddr = cat(addr(0x88000000))
    const body = cat(
        u8(DW_LLE_BASE_ADDRESSX), uleb(0),
        u8(DW_LLE_OFFSET_PAIR), uleb(1), uleb(2), exprOf([0x50]),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4, addr: debugAddr, addrBase: 0})
    checkEq('loclists: base_addressx base', list.baseAddress, 0x88000000)
    checkEq('loclists: base_addressx offset_pair low', list.entries[0].lowPc, 0x88000001)
    checkEq('loclists: base_addressx offset_pair high', list.entries[0].highPc, 0x88000002)
}
{
    // GNU view pair(0x09)：记录但不产生 entry
    const body = cat(
        u8(DW_LLE_GNU_VIEW_PAIR), uleb(1), uleb(3),
        u8(DW_LLE_START_LENGTH), addr(0x100), uleb(1), exprOf([0x50]),
        u8(DW_LLE_END_OF_LIST),
    )
    const section = locListsSection({body})
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('loclists: view pair recorded', list.viewPairs.length, 1)
    checkEq('loclists: view pair values', `${list.viewPairs[0].beginView}..${list.viewPairs[0].endView}`, '1..3')
    checkEq('loclists: view pair does not create entry', list.entries.length, 1)
    check('loclists: view pair list terminated', list.terminated)
}
{
    // 未知 opcode / 截断表达式 / 缺 .debug_addr
    const unknown = decodeLocationList({
        kind: 'debug_loclists',
        data: locListsSection({body: cat(u8(DW_LLE_START_LENGTH), addr(0x100), uleb(1), exprOf([0x50]), u8(0x0a))}),
        offset: 12, addrSize: 4,
    })
    check('loclists: unknown opcode -> warning', unknown.warnings.some((warning) => warning.includes('unknown DW_LLE')))
    check('loclists: unknown opcode -> not terminated', unknown.terminated === false)

    const truncated = decodeLocationList({
        kind: 'debug_loclists',
        data: locListsSection({body: cat(u8(DW_LLE_START_LENGTH), addr(0x100), uleb(1), uleb(8), [0x50])}),
        offset: 12, addrSize: 4,
    })
    check('loclists: truncated expression -> warning', truncated.warnings.length > 0)

    const missingAddr = decodeLocationList({
        kind: 'debug_loclists',
        data: locListsSection({body: cat(u8(DW_LLE_STARTX_LENGTH), uleb(0), uleb(4), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))}),
        offset: 12, addrSize: 4,
    })
    check('loclists: startx without .debug_addr -> warning', missingAddr.warnings.some((warning) => warning.includes('no .debug_addr')))
}
{
    // 64-bit 地址的 loclists 条目（address_size = 8）
    const body = cat(u8(DW_LLE_START_END), addr(0x400000000000, 8), addr(0x400000000010, 8), exprOf([0x50]), u8(DW_LLE_END_OF_LIST))
    const section = locListsSection({body, addrSize: 8})
    const header = parseLocationListHeader(section)
    checkEq('loclists: address_size 8 header', header.addrSize, 8)
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: header.listsBase, addrSize: 8})
    checkEq('loclists: 64-bit start_end low', list.entries[0].lowPc, 0x400000000000)
    checkEq('loclists: 64-bit start_end high', list.entries[0].highPc, 0x400000000010)
}
{
    // maxEntries 防御
    const body = cat(
        u8(DW_LLE_DEFAULT_LOCATION), exprOf([0x50]),
        u8(DW_LLE_DEFAULT_LOCATION), exprOf([0x51]),
        u8(DW_LLE_END_OF_LIST),
    )
    const list = decodeLocationList({kind: 'debug_loclists', data: locListsSection({body}), offset: 12, addrSize: 4, maxEntries: 1})
    checkEq('loclists: maxEntries respected', list.entries.length, 1)
}

// ---------------------------------------------------------------------------
// 3. findLocationListEntry PC 选择语义
// ---------------------------------------------------------------------------
console.log('\nunit: findLocationListEntry')
{
    const section = cat(
        addr(0x1000), addr(0x1010), u16(1), [0x50],
        addr(0x1010), addr(0x1020), u16(1), [0x51],
        addr(0), addr(0),
    )
    const list = decodeLocationList({kind: 'debug_loc', data: section, offset: 0, addrSize: 4})
    checkEq('find: pc in first range', findLocationListEntry(list.entries, 0x1008)?.expr[0], 0x50)
    checkEq('find: pc on shared boundary -> second range (high exclusive)', findLocationListEntry(list.entries, 0x1010)?.expr[0], 0x51)
    checkEq('find: pc at high end -> null', findLocationListEntry(list.entries, 0x1020), null)
    check('find: covers helper false outside', locationListCoversPc(list.entries, 0x1000) && !locationListCoversPc(list.entries, 0x2000))
}
{
    const section = locListsSection({
        body: cat(
            u8(DW_LLE_START_LENGTH), addr(0x1000), uleb(0x10), exprOf([0x50]),
            u8(DW_LLE_DEFAULT_LOCATION), exprOf([0x9f]),
            u8(DW_LLE_END_OF_LIST),
        ),
    })
    const list = decodeLocationList({kind: 'debug_loclists', data: section, offset: 12, addrSize: 4})
    checkEq('find: default_location fallback', findLocationListEntry(list.entries, 0x5000)?.expr[0], 0x9f)
    checkEq('find: range wins over default', findLocationListEntry(list.entries, 0x1004)?.expr[0], 0x50)
}

// ---------------------------------------------------------------------------
// 4. 真实 ELF：reference .debug_loc + readelf 交叉验证
// ---------------------------------------------------------------------------
console.log(`\nreal ELF: ${elfPath}`)

/** 最小只读 ELF section 表解析（不依赖 objcopy/readelf，支持 ELF32/64 LE）。 */
const readElfSections = (file) => {
    const raw = fs.readFileSync(file)
    if (raw.length < 0x40 || raw[0] !== 0x7f || raw[1] !== 0x45 || raw[2] !== 0x4c || raw[3] !== 0x46) {
        throw new Error('not an ELF file')
    }
    const is64 = raw[4] === 2
    if (raw[5] !== 1) throw new Error('only little-endian ELF is supported')
    const u16 = (offset) => raw[offset] | (raw[offset + 1] << 8)
    const u32 = (offset) => raw.readUInt32LE(offset)
    const u64 = (offset) => Number(raw.readBigUInt64LE(offset))
    const eShoff = is64 ? u64(0x28) : u32(0x20)
    const eShentsize = is64 ? u16(0x3a) : u16(0x2e)
    const eShnum = is64 ? u16(0x3c) : u16(0x30)
    const eShstrndx = is64 ? u16(0x3e) : u16(0x32)
    const sections = []
    for (let index = 0; index < eShnum; index++) {
        const base = eShoff + index * eShentsize
        const nameOffset = u32(base)
        const type = u32(base + 4)
        const offset = is64 ? u64(base + 0x18) : u32(base + 0x10)
        const size = is64 ? u64(base + 0x20) : u32(base + 0x14)
        sections.push({nameOffset, type, offset, size})
    }
    const strtab = sections[eShstrndx]
    const readName = (offset) => {
        const start = strtab.offset + offset
        const end = raw.indexOf(0, start)
        return raw.toString('utf8', start, end)
    }
    const byName = new Map()
    for (const section of sections) byName.set(readName(section.nameOffset), section)
    return {raw, is64, sections: byName}
}

/** 从 ELF 里只读切出 .debug_* 节。 */
const sectionBytesOf = (elf, name) => {
    const section = elf.sections.get(name)
    if (!section || section.offset + section.size > elf.raw.length) return null
    return elf.raw.subarray(section.offset, section.offset + section.size)
}

/**
 * 解析 `readelf --debug-dump=loc`（DWARF4 .debug_loc 文本格式）。
 * 输出逐行扫描，遇到 `<End of list>` 结束当前 list；`(base address)` 行更新 base。
 * 返回按 list offset 升序的 [{offset, ranges, baseAddress, terminated}]。
 */
const parseReadelfLoc = (text) => {
    const lists = []
    const rangePattern = /^([0-9a-f]{4,16})\s+([0-9a-f]{4,16})\s+([0-9a-f]{4,16})\s+\((.+)\)$/
    const endPattern = /^([0-9a-f]{4,16})\s+<End of list>$/
    let current = null
    const flush = () => {
        if (current !== null) lists.push(current)
        current = null
    }
    for (const rawLine of text.split('\n')) {
        const line = rawLine.trim()
        if (!line) continue
        if (/location view pair|views at|View pair entry/.test(line)) continue
        if (line.startsWith('Contents of') || line.includes('Offset') && line.includes('Expression')) continue
        if (line === 'Contents of the .debug_loc section:') continue
        const endMatch = line.match(endPattern)
        if (endMatch) {
            if (current === null) current = {offset: parseInt(endMatch[1], 16), ranges: [], baseAddress: null, terminated: true}
            current.terminated = true
            flush()
            continue
        }
        const match = line.match(rangePattern)
        if (!match) continue
        const entryOffset = parseInt(match[1], 16)
        const begin = parseInt(match[2], 16)
        const end = parseInt(match[3], 16)
        const exprText = match[4]
        if (current === null) current = {offset: entryOffset, ranges: [], baseAddress: null, terminated: false}
        if (exprText === 'base address') current.baseAddress = end
        else current.ranges.push({begin, end, exprText})
    }
    flush()
    lists.sort((left, right) => left.offset - right.offset)
    return lists
}

/**
 * 解析 `readelf --debug-dump=info`，建立 .debug_loc list offset -> 所属 CU 的
 * DW_AT_low_pc 映射。DWARF4 中 .debug_loc 条目的初始 base address 就是
 * CU 顶 DIE 的 DW_AT_low_pc（没有该属性时为 0），readelf 显示区间时也用它。
 */
const parseReadelfInfoLocBases = (text) => {
    const bases = new Map()
    const cuStartPattern = /^\s*Compilation Unit @ offset/
    const diePattern = /^\s*<(\d+)><([0-9a-f]+)>:/
    const lowPcPattern = /^\s*<[0-9a-f]+>\s+DW_AT_low_pc\s+:\s+(0x[0-9a-f]+|0)\s*$/
    const locListPattern = /DW_AT_location\s*:\s*(0x[0-9a-f]+)\s+\(location list\)/
    let cuBase = null
    let inRootDie = false
    for (const rawLine of text.split('\n')) {
        const line = rawLine.trimEnd()
        if (cuStartPattern.test(line)) {
            cuBase = 0
            inRootDie = false
            continue
        }
        const dieMatch = line.match(diePattern)
        if (dieMatch) {
            inRootDie = dieMatch[1] === '0'
            continue
        }
        if (cuBase === null) continue
        if (inRootDie) {
            const lowPcMatch = line.match(lowPcPattern)
            if (lowPcMatch) {
                cuBase = lowPcMatch[1] === '0' ? 0 : parseInt(lowPcMatch[1], 16)
                continue
            }
        }
        const locMatch = line.match(locListPattern)
        if (locMatch) {
            const offset = parseInt(locMatch[1], 16)
            if (!bases.has(offset)) bases.set(offset, cuBase)
        }
    }
    return bases
}

const elfExists = fs.existsSync(elfPath)
if (!elfExists) {
    console.log(`  [skip] ELF not found: ${elfPath} (pass a path or set DWARF_ELF)`)
} else {
    let elf = null
    let debugLoc = null
    try {
        elf = readElfSections(elfPath)
        debugLoc = sectionBytesOf(elf, '.debug_loc')
    } catch (error) {
        console.log(`  [skip] cannot read ELF sections: ${error.message}`)
    }
    if (elf) {
        check('real: reference is 32-bit ELF (addrSize 4)', elf.is64 === false)
        check('real: .debug_loc section present', debugLoc !== null && debugLoc.length > 0, debugLoc ? hex(debugLoc.length) : 'missing')
        check('real: .debug_loclists section absent (DWARF4 firmware)', sectionBytesOf(elf, '.debug_loclists') === null)
    }
    if (elf && debugLoc && debugLoc.length > 0) {
        const addrSize = elf.is64 ? 8 : 4
        let readelfAvailable = true
        let dump = ''
        try {
            dump = execFileSync('readelf', ['--debug-dump=loc', elfPath], {maxBuffer: 64 * 1024 * 1024}).toString()
        } catch {
            readelfAvailable = false
        }
        let infoBases = new Map()
        if (readelfAvailable) {
            try {
                const infoDump = execFileSync('readelf', ['--debug-dump=info', elfPath], {maxBuffer: 256 * 1024 * 1024}).toString()
                infoBases = parseReadelfInfoLocBases(infoDump)
            } catch {
                infoBases = new Map()
            }
        }
        if (!readelfAvailable) {
            console.log('  [skip] readelf not available, cannot cross-check real .debug_loc')
        } else {
            const expectedLists = parseReadelfLoc(dump)
            // 优化版 reference 的 .debug_loc 很大（5074 个 list）；调试版/其它 ELF 可能很小，
            // 小 section 放宽数量阈值，但仍做完整交叉验证。
            const strictReal = debugLoc.length >= 0x10000
            if (!strictReal) console.log(`  info small .debug_loc section ${hex(debugLoc.length)}, count thresholds relaxed`)
            check('real: CU base address map from readelf -wi', infoBases.size >= (strictReal ? 1000 : 1), infoBases.size)
            check('real: readelf ground truth has many lists', expectedLists.length >= (strictReal ? 1000 : 1), expectedLists.length)
            let matched = 0
            let rangeMismatch = 0
            let entryCountMismatch = 0
            let terminatedMismatch = 0
            let warningLists = 0
            let baseMismatch = 0
            let contiguous = 0
            const mismatchSamples = []
            let decodedEntries = 0
            let expectedEntries = 0
            for (let index = 0; index < expectedLists.length; index++) {
                const expected = expectedLists[index]
                if (expected.offset >= debugLoc.length) continue
                // DWARF4 .debug_loc 初始 base = 所属 CU 顶 DIE 的 DW_AT_low_pc；
                // 大多数 CU 为 0，少数 CU（链接后低 PC 为绝对地址）非 0。
                const cuBase = infoBases.get(expected.offset) ?? 0
                const list = decodeLocationList({
                    kind: 'debug_loc',
                    data: debugLoc,
                    offset: expected.offset,
                    addrSize,
                    baseAddress: cuBase,
                })
                expectedEntries += expected.ranges.length
                decodedEntries += list.entries.length
                let ok = true
                if (list.warnings.length > 0) {
                    warningLists++
                    ok = false
                }
                if (list.terminated !== expected.terminated) {
                    terminatedMismatch++
                    ok = false
                }
                if (expected.baseAddress !== null && list.baseAddress !== expected.baseAddress) {
                    baseMismatch++
                    ok = false
                }
                if (list.entries.length !== expected.ranges.length) {
                    entryCountMismatch++
                    ok = false
                } else if (list.entries.some((entry, entryIndex) =>
                    entry.lowPc !== expected.ranges[entryIndex].begin || entry.highPc !== expected.ranges[entryIndex].end)) {
                    rangeMismatch++
                    ok = false
                }
                if (ok) matched++
                else if (mismatchSamples.length < 5) {
                    mismatchSamples.push({
                        offset: hex(expected.offset),
                        expectedRanges: expected.ranges.length,
                        decodedRanges: list.entries.length,
                        warnings: list.warnings,
                    })
                }
                const next = expectedLists[index + 1]
                if (next && list.endOffset === next.offset) contiguous++
            }
            const matchRatio = expectedLists.length ? matched / expectedLists.length : 1
            check('real: readelf lists decoded without warnings', warningLists === 0, `${warningLists} lists with warnings`)
            check('real: readelf list terminated status matches', terminatedMismatch === 0, `${terminatedMismatch} mismatches`)
            check('real: entry counts match readelf', entryCountMismatch === 0, `${entryCountMismatch} mismatches`)
            check('real: entry ranges match readelf (base-adjusted absolute)', rangeMismatch === 0, `${rangeMismatch} mismatches`)
            check('real: last base selection matches readelf', baseMismatch === 0, `${baseMismatch} mismatches`)
            check('real: list boundaries contiguous (>95% adjacent lists)', contiguous >= (expectedLists.length - 1) * 0.95,
                `${contiguous}/${expectedLists.length - 1} contiguous`)
            check('real: decoded entry total matches readelf', decodedEntries === expectedEntries,
                `decoded=${decodedEntries} expected=${expectedEntries}`)
            check('real: overall cross-check ratio >= 99%', matchRatio >= 0.99, `${(matchRatio * 100).toFixed(2)}%`)
            console.log(`  info matched=${matched}/${expectedLists.length} entries=${decodedEntries}/${expectedEntries} section=${hex(debugLoc.length)} contiguous=${contiguous}`)
            if (mismatchSamples.length > 0) console.log(`  info mismatch samples: ${JSON.stringify(mismatchSamples)}`)
        }
    }
}

// ---------------------------------------------------------------------------
// 汇总
// ---------------------------------------------------------------------------
if (failures > 0) {
    console.error(`\n${failures} failure(s)`)
    process.exit(1)
}
console.log('\nall DWARF location-list checks passed')
