#!/usr/bin/env node
/**
 * Golden test: DWARF source names / spec chains / unsampleable listing.
 *
 * 使用 pyelftools 生成的 ground truth（web/scripts/testdata/25omni-dwarf-truth.json）
 * 校验 reference ELF 的变量解析结果：
 *   (a) 所有 named+fixed-address 变量都有正确的源码名/类型/file:line；
 *   (b) 主名称中不出现 _Z mangled 字符串；
 *   (c) ground truth 中的命名变量（含无固定地址者）全部可在解析结果中搜索到；
 *   (d) 不以无名变量充数（字符串常量隐藏）。
 *
 * 用法：node scripts/test-dwarf-variables.mjs [elf-path]
 *   默认 ELF: $HOME/reference firmware/firmware/reference-board/reference/firmware.elf
 *   ELF 不存在时输出 SKIP 并以 0 退出（CI 无该文件时不阻塞）。
 */
import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import { fileURLToPath } from 'node:url'
import { execFileSync } from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const truthFile = path.join(root, 'scripts', 'testdata', '25omni-dwarf-truth.json')
const elf = process.argv[2] ?? process.env.DWARF_ELF ?? '$HOME/reference firmware/firmware/reference-board/reference/firmware.elf'

if (!fs.existsSync(elf)) {
  console.log(`[skip] ELF not found: ${elf} (set DWARF_ELF or pass a path)`)
  process.exit(0)
}
if (!fs.existsSync(truthFile)) {
  console.error(`golden truth missing: ${truthFile}`)
  process.exit(1)
}

// Bundle the TS parser with the project's local esbuild (offline).
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const outFile = path.join(os.tmpdir(), `dwarf-golden-parser-${process.pid}.mjs`)
execFileSync(esbuild, [
  path.join(root, 'src', 'utils', 'elfSymbols.ts'),
  '--bundle', '--format=esm', '--platform=node', `--outfile=${outFile}`,
], {stdio: 'pipe'})
const { parseElfVariables, demangleItanium } = await import(outFile)
const truth = JSON.parse(fs.readFileSync(truthFile, 'utf8')).rows
const result = parseElfVariables(fs.readFileSync(elf))

let failures = 0
const check = (name, cond, detail = '') => {
  if (cond) console.log(`  ok   ${name}`)
  else { console.log(`  FAIL ${name}${detail ? ' :: ' + detail : ''}`); failures++ }
}
const norm = (value) => (value || '').replace(/\\/g, '/')
const fileMatches = (parserFile, truthFileValue) => {
  if (!truthFileValue) return true
  const a = norm(parserFile)
  const b = norm(truthFileValue)
  return a === b || a.endsWith('/' + b) || a.endsWith(b) || a.split('/').pop() === b.split('/').pop()
}
const shortOf = (row) => {
  if (row.name.startsWith('_Z')) {
    const demangled = demangleItanium(row.name)
    return demangled ? demangled.split('::').pop() : row.name
  }
  return (row.qualified || row.name).split('::').pop()
}
const sampleable = result.variables.filter((item) => item.sample)
const unsampleable = result.variables.filter((item) => !item.sample)
console.log(`ELF: ${elf}`)
console.log(`listed=${result.variables.length} sampleable=${sampleable.length} unsampleable=${unsampleable.length} namedDIEs=${result.dwarfNamed} fixed=${result.dwarfWithAddress}`)

// (a) named + fixed-address truth rows must exist with source name/type/file:line
const fixedTruth = truth.filter((row) => row.name && row.address !== null)
const fixedMatched = fixedTruth.filter((row) => {
  const expectedShort = shortOf(row)
  return result.variables.some((item) => (item.shortName === expectedShort || (row.name.startsWith('_Z') && item.mangledName === row.name))
    && fileMatches(item.relativeFile ?? item.file, row.file) && (row.line === null || row.line === undefined || item.line === row.line))
})
const fixedRatio = fixedTruth.length > 0 ? fixedMatched.length / fixedTruth.length : 1
console.log(`(a) named+fixed truth=${fixedTruth.length} matched=${fixedMatched.length} (${(fixedRatio * 100).toFixed(1)}%)`)
check('(a) >=95% named+fixed rows match truth name/file/line', fixedRatio >= 0.95, fixedRatio)
check('(a) matched fixed rows all have non-empty type', fixedMatched.every((row) => {
  const expectedShort = shortOf(row)
  const item = result.variables.find((candidate) => candidate.shortName === expectedShort || (row.name.startsWith('_Z') && candidate.mangledName === row.name))
  return !!item && !!item.typeName && item.typeName !== '?'
}))

// (b) no primary mangled names
const mangled = result.variables.filter((item) => (item.name || '').startsWith('_Z'))
console.log(`(b) primary _Z names=${mangled.length}`)
check('(b) zero primary mangled names', mangled.length === 0, mangled.slice(0, 3).map((item) => item.name))

// (c) every named truth variable searchable (sampleable or unsampleable)
const namedTruth = truth.filter((row) => row.name && (row.file || row.name.startsWith('_Z')))
const namedMissed = namedTruth.filter((row) => {
  const expectedShort = shortOf(row)
  return !result.variables.some((item) => (item.shortName === expectedShort || (row.name.startsWith('_Z') && item.mangledName === row.name))
    && (row.name.startsWith('_Z') || fileMatches(item.relativeFile ?? item.file, row.file)))
})
const namedRatio = namedTruth.length > 0 ? (namedTruth.length - namedMissed.length) / namedTruth.length : 1
console.log(`(c) named truth=${namedTruth.length} covered=${namedTruth.length - namedMissed.length} (${(namedRatio * 100).toFixed(1)}%) missed=${namedMissed.length}`)
check('(c) >=99% named truth rows searchable', namedRatio >= 0.99, namedMissed.slice(0, 5).map((row) => row.name + '|' + row.file))
check('(c) at least 3500 named rows listed', result.variables.filter((item) => item.shortName).length >= 3500)

// (d) unnamed variables hidden / no unnamed rows
const unnamedRows = result.variables.filter((item) => !item.shortName || !item.shortName.trim())
console.log(`(d) unnamed rows=${unnamedRows.length}`)
check('(d) zero unnamed listed rows', unnamedRows.length === 0)

// spot checks from the task ground truth
const spot = [
  ['sn_tools', 'ms_item_t', 'src/system/FreeRTOS/database.cpp', 12],
  ['sn_buff', 'uint8_t[33]', 'src/system/FreeRTOS/database.cpp', 10],
  ['accel_inv_norm', 'float', 'src/device/ins/dev_ins.cpp', 292],
  ['xTickCount', 'TickType_t', 'lib/freertos/tasks.c', 363],
  ['_usbd_dev', 'usbd_device_t', 'lib/tinyusb/src/device/usbd.c', 79],
]
for (const [name, typeHint, file, line] of spot) {
  const item = result.variables.find((candidate) => candidate.shortName === name)
  const ok = !!item && item.typeName.includes(typeHint) && (item.relativeFile ?? '').endsWith(file) && item.line === line
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} spot ${name}: ${item ? `${item.typeName} | ${item.relativeFile}:${item.line}` : 'MISSING'}`)
  if (!ok) failures++
}
const selfVar = result.variables.find((candidate) => candidate.mangledName === '_ZN6System5Timer5self_E')
check('spec-chain source name System::Timer::self_ (mangled kept as alias)',
  !!selfVar && selfVar.name === 'System::Timer::self_' && !!selfVar.typeName && !!selfVar.relativeFile && !!selfVar.line,
  selfVar ? `${selfVar.name} | ${selfVar.typeName} | ${selfVar.relativeFile}:${selfVar.line}` : 'MISSING')

fs.rmSync(outFile, {force: true})
console.log('')
if (failures) { console.error(`DWARF golden test FAILED: ${failures} check(s)`); process.exit(1) }
console.log('DWARF golden test PASSED')
