#!/usr/bin/env node
/**
 * A2 解析器测试：调试采样位置（DW_OP_fbreg/breg/reg）+ 指针成员展开。
 *
 * 校验两份固件：
 *  - reference-debug-O0：accel/gyro/delay/bmi088 等局部变量得到 halt/frame 位置，
 *    指针 bmi088 展开 `->member` 子节点，帧/寄存器变量不再被当作“不可采样”；
 *  - reference（-O2）：原有 269 live 固定地址路径保持不变，固定地址指针可展开。
 *
 * 另含 AddressExpr 求值的纯单元测试（模拟寄存器/内存）。
 *
 * 用法：node scripts/test-dwarf-locations.mjs [release.elf] [debug.elf]
 */
import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const releaseElf = process.argv[2] ?? '$HOME/reference firmware/firmware/reference-board/reference/firmware.elf'
const debugElf = process.argv[3] ?? '$HOME/reference firmware/firmware/reference-board/reference-debug-O0/firmware.elf'
for (const [label, elf] of [['release', releaseElf], ['debug', debugElf]]) {
  if (!fs.existsSync(elf)) {
    console.log(`[skip] ${label} ELF not found: ${elf}`)
    process.exit(0)
  }
}

const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const bundle = (entry, out) => {
  execFileSync(esbuild, [entry, '--bundle', '--format=esm', '--platform=node', `--outfile=${out}`], {stdio: 'pipe'})
  return out
}
const parserOut = path.join(os.tmpdir(), `dwarf-loc-parser-${process.pid}.mjs`)
const evalOut = path.join(os.tmpdir(), `dwarf-loc-eval-${process.pid}.mjs`)
const dwarfOut = path.join(os.tmpdir(), `dwarf-loc-core-${process.pid}.mjs`)
bundle(path.join(root, 'src/utils/elfSymbols.ts'), parserOut)
bundle(path.join(root, 'src/utils/dwarfLocation.ts'), evalOut)
bundle(path.join(root, 'src/utils/elfDwarf.ts'), dwarfOut)
const {parseElfVariables} = await import(parserOut)
const {addressExprIsLive, addressExprRegisters, addressExprText, evaluateAddress} = await import(evalOut)
const {selectLocationListEntry} = await import(dwarfOut)

let failures = 0
const check = (name, cond, detail = '') => {
  if (cond) console.log(`  ok   ${name}`)
  else { console.log(`  FAIL ${name}${detail ? ' :: ' + detail : ''}`); failures++ }
}

// ---------- 纯单元测试：AddressExpr 求值 ----------
console.log('unit: AddressExpr evaluation')
const registers = new Map([[7, 0x24003200], [13, 0x24003100], [0, 0x12345678]])
const memory = new Map([
  [0x24000000, 0x24001000],
  [0x24001000, 0x24002000],
])
const readWord = (address) => memory.get(address >>> 0) ?? 0
check('addr evaluates to absolute address', evaluateAddress({kind: 'addr', address: 0x08000000}, registers, readWord) === 0x08000000)
check('breg/frame register + offset', evaluateAddress({kind: 'reg', reg: 13, offset: 48}, registers, readWord) === 0x24003130)
check('negative frame offset', evaluateAddress({kind: 'reg', reg: 7, offset: -16}, registers, readWord) === 0x240031f0)
check('cfa uses SP(r13)', evaluateAddress({kind: 'cfa', offset: 16}, registers, readWord) === 0x24003110)
check('add offset', evaluateAddress({kind: 'add', inner: {kind: 'reg', reg: 7, offset: 0}, offset: 8}, registers, readWord) === 0x24003208)
check('deref pointer + member offset', evaluateAddress({kind: 'deref', inner: {kind: 'addr', address: 0x24000000}, offset: 4}, registers, readWord) === 0x24001004)
check('nested pointer chain', evaluateAddress({
  kind: 'deref',
  inner: {kind: 'deref', inner: {kind: 'addr', address: 0x24000000}, offset: 0},
  offset: 8,
}, registers, readWord) === 0x24002008)
check('registers collected', [...addressExprRegisters({kind: 'add', inner: {kind: 'reg', reg: 7, offset: 4}, offset: 1})].join(',') === '7')
check('cfa register is r13', [...addressExprRegisters({kind: 'cfa', offset: 0})].join(',') === '13')
check('addressExprIsLive: addr/deref live', addressExprIsLive({kind: 'deref', inner: {kind: 'addr', address: 0x24000000}, offset: 4}))
check('addressExprIsLive: reg not live', addressExprIsLive({kind: 'reg', reg: 13, offset: 0}) === false)
check('addressExprText formats', addressExprText({kind: 'deref', inner: {kind: 'reg', reg: 7, offset: 48}, offset: 4}) === '*[r7+48]+4')
check('missing register throws', (() => {
  try { evaluateAddress({kind: 'reg', reg: 5, offset: 0}, registers, readWord); return false } catch { return true }
})())

// ---------- D：loclist 条目选择（synthetic 单 entry/default/multi） ----------
console.log('loclist: entry selection (synthetic)')
const exprBytes = Uint8Array.from([0x91, 0x00]) // DW_OP_fbreg 0（仅用于选择逻辑）
const entry = (index, kind, lowPc, highPc) => ({
  index, kind, lowPc, highPc, expr: exprBytes, entryOffset: index * 10,
})
const single = selectLocationListEntry([entry(0, 'address_range', 0x08001000, 0x08002000)])
check('loclist: single entry selected directly', !!single && single.multi === false
  && single.entry.lowPc === 0x08001000 && single.rangedCount === 1 && single.total === 1, single)
const defaultValue = selectLocationListEntry([entry(0, 'default_location', undefined, undefined)])
check('loclist: default_location used when no ranged entry', !!defaultValue && defaultValue.multi === false
  && defaultValue.entry.kind === 'default_location', defaultValue)
const multi = selectLocationListEntry([
  entry(0, 'address_range', 0x08001000, 0x08002000),
  entry(1, 'address_range', 0x08003000, 0x08004000),
  entry(2, 'default_location', undefined, undefined),
])
check('loclist: multi entry -> first range + multi flag', !!multi && multi.multi === true
  && multi.rangedCount === 2 && multi.total === 3 && multi.entry.lowPc === 0x08001000, multi)
check('loclist: empty entries -> null', selectLocationListEntry([]) === null)

// ---------- 调试 ELF：帧/寄存器位置 + 指针展开 ----------
console.log(`\ndebug ELF: ${debugElf}`)
const debug = parseElfVariables(fs.readFileSync(debugElf))
const debugRoots = debug.variables
console.log(`  listed=${debugRoots.length} live=${debug.dwarfLiveSamplable} halt=${debug.dwarfHaltSamplable} pointer=${debug.dwarfPointerExpanded} noLocation=${debug.dwarfNoLocation} fixed=${debug.dwarfWithAddress}`)
check('debug: halt-samplable frame variables parsed', debug.dwarfHaltSamplable >= 3000, debug.dwarfHaltSamplable)
check('debug: few truly no-location names (only loclists/optimized)', debug.dwarfNoLocation <= 200, debug.dwarfNoLocation)
check('debug: pointer members expanded', debug.dwarfPointerExpanded >= 1000, debug.dwarfPointerExpanded)

const findRoot = (name) => debugRoots.find((node) => node.shortName === name)
for (const name of ['gyro', 'delay', 'bmi088']) {
  const node = findRoot(name)
  check(`debug: ${name} has halt/frame location`, !!node && node.samplingMode === 'halt'
    && (!!node.sample?.location || node.sample?.register !== undefined || !!node.location),
  node ? `${node.samplingMode} ${node.typeName}` : 'MISSING')
}
const accelInv = findRoot('accel_inv_norm')
check('debug: accel_inv_norm still listed (fixed or halt)', !!accelInv)
check('debug: fixed-address variables keep live mode', debugRoots.some((node) => node.sample?.address > 0 && node.samplingMode === 'live'))

const bmi = findRoot('bmi088')
check('debug: bmi088 expands pointee members', !!bmi?.children && bmi.children.some((child) => child.name.startsWith('->')), bmi ? (bmi.children ?? []).slice(0, 4).map((c) => c.name).join(',') : 'MISSING')
const findPath = (root, target) => {
  const walk = (node, walkPath) => {
    if (walkPath === target) return node
    for (const child of node.children ?? []) {
      const found = walk(child, walkPath + child.name)
      if (found) return found
    }
    return null
  }
  return walk(root, root.name)
}
const pointeeMember = bmi ? (bmi.children ?? []).find((child) => child.pointerDepth > 0 && child.sampleable) : null
check('debug: pointee member carries pointerDepth + location', !!pointeeMember && pointeeMember.pointerDepth > 0 && !!pointeeMember.location,
  pointeeMember ? `${pointeeMember.name} depth=${pointeeMember.pointerDepth}` : 'MISSING')
const nestedArrayPath = bmi ? findPath(bmi, 'bmi088->rot_.rot_mat[0]') : null
check('debug: nested pointer->struct->array element path', !!nestedArrayPath && !!nestedArrayPath.sample?.location,
  nestedArrayPath ? addressExprText(nestedArrayPath.sample.location) : 'MISSING')
const sampleLocLeaf = debugRoots.find((node) => node.sample?.location && node.sampleable)
check('debug: halt leaf has evaluable AddressExpr', !!sampleLocLeaf)

// ---------- A：DWARF subprogram PC 范围（pcLow/pcHigh） ----------
console.log('pc scope: DWARF subprogram ranges')
const isFlashPc = (node) => typeof node.pcLow === 'number' && typeof node.pcHigh === 'number'
  && node.pcLow >= 0x08000000 && node.pcHigh <= 0x09000000 && node.pcHigh > node.pcLow
const debugPcRoots = debugRoots.filter(isFlashPc)
check('debug: root variables carry Flash PC ranges', debugPcRoots.length >= 500, debugPcRoots.length)
for (const name of ['gyro', 'delay', 'bmi088']) {
  const node = findRoot(name)
  check(`debug: ${name} has valid PC range`, !!node && isFlashPc(node), node ? `0x${node.pcLow?.toString(16)}-0x${node.pcHigh?.toString(16)}` : 'MISSING')
}
const bmiPc = findRoot('bmi088')
const childWithPc = bmiPc?.children?.find((child) => isFlashPc(child))
check('debug: children inherit parent PC range', !!childWithPc && childWithPc.pcLow === bmiPc.pcLow && childWithPc.pcHigh === bmiPc.pcHigh,
  childWithPc ? `${childWithPc.name}` : 'MISSING')
const globalSn = findRoot('sn_tools')
check('debug: file-scope global has no PC range', !!globalSn && globalSn.pcLow === undefined && globalSn.pcHigh === undefined)
check('debug: all PC ranges are within Flash', debugPcRoots.every((node) => isFlashPc(node)))

// ---------- D：debug-O0 的真实 loclist 变量（printf/comp_utils） ----------
const debugLoclist = debugRoots.filter((node) => node.loclistEntries !== undefined)
console.log(`  loclist variables: ${debugLoclist.length} (multi=${debugLoclist.filter((node) => node.loclistMulti).length})`)
check('debug: loclist variables become sampleable', debugLoclist.length >= 1 && debugLoclist.every((node) => node.sampleable),
  debugLoclist.slice(0, 4).map((node) => `${node.shortName}:${node.loclistEntries}`))
check('debug: multi-range loclist flagged + variants recorded', debugLoclist.some((node) => node.loclistMulti
  && (node.locationVariants?.length ?? 0) > 0), debugLoclist.slice(0, 3).map((node) => `${node.shortName}/${node.locationVariants?.length ?? 0}`))
check('debug: no unsampleable loclist variable left unsupported', debugLoclist.every((node) => node.sampleable))

// ---------- Release ELF：旧 live 路径不变 + 固定地址指针可展开 ----------
console.log(`\nrelease ELF: ${releaseElf}`)
const release = parseElfVariables(fs.readFileSync(releaseElf))
const liveRoots = release.variables.filter((node) => node.sampleable && node.liveSamplable)
console.log(`  listed=${release.variables.length} live=${release.dwarfLiveSamplable} halt=${release.dwarfHaltSamplable} pointer=${release.dwarfPointerExpanded} fixed=${release.dwarfWithAddress}`)
check('release: 269 named fixed-address variables preserved', release.dwarfWithAddress === 269, release.dwarfWithAddress)
check('release: original 255-live root sampling path preserved', release.dwarfLiveSamplable === 255, release.dwarfLiveSamplable)
check('release: live roots still have concrete addresses', liveRoots.length >= 255 && liveRoots.every((node) => node.sample?.address || node.liveSamplable))
const livePointer = release.variables.find((node) => node.children?.some((child) => child.liveSamplable && child.pointerDepth > 0))
check('release: fixed-address pointer members expand for live two-phase read', !!livePointer, livePointer ? livePointer.shortName : 'MISSING')
const releasePcRoots = release.variables.filter(isFlashPc)
check('release: PC ranges parsed for optimized local variables', releasePcRoots.length >= 100, releasePcRoots.length)
const releaseGlobal = release.variables.find((node) => node.shortName === 'sn_tools')
check('release: global variable has no PC range', !!releaseGlobal && releaseGlobal.pcLow === undefined)
const selfVar = release.variables.find((node) => node.mangledName === '_ZN6System5Timer5self_E')
check('release: spec-chain source names unchanged', !!selfVar && selfVar.name === 'System::Timer::self_')

// ---------- D：优化 ELF loclist 集成统计 ----------
const releaseLoclist = release.variables.filter((node) => node.loclistEntries !== undefined)
const releaseMulti = releaseLoclist.filter((node) => node.loclistMulti)
console.log(`  loclist variables: ${releaseLoclist.length} (multi=${releaseMulti.length})`)
check('release: many loclist variables resolved (previously unsupported)', releaseLoclist.length >= 1000, releaseLoclist.length)
check('release: multi-range loclist flagged with variants', releaseMulti.length >= 100
  && releaseMulti.some((node) => (node.locationVariants?.length ?? 0) > 0), releaseMulti.length)
check('release: all loclist variables sampleable (none dropped as unsupported)', releaseLoclist.every((node) => node.sampleable))
check('release: loclist examples keep source name/file:line', releaseLoclist.some((node) => node.shortName === 'reg_value' && node.loclistMulti && node.relativeFile), releaseLoclist.slice(0, 2).map((node) => node.shortName))

fs.rmSync(parserOut, {force: true})
fs.rmSync(evalOut, {force: true})
fs.rmSync(dwarfOut, {force: true})
console.log('')
if (failures) { console.error(`A2 DWARF location test FAILED: ${failures} check(s)`); process.exit(1) }
console.log('A2 DWARF location test PASSED')
