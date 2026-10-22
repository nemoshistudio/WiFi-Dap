#!/usr/bin/env node
/**
 * 本地源码项目 + DWARF 路径映射纯函数测试（task-24）。
 *
 * 覆盖：
 *  - 路径规范化（Windows/Unix/`..`/根目录前缀/webkitRelativePath）；
 *  - 目录扫描过滤（扩展名、忽略目录、2MB、2000 文件上限、ELF 发现）；
 *  - 文件树构建/扁平化、ELF 最新选择；
 *  - 变量树展开 / 标识符 / 选中 key；
 *  - 变量 -> 项目文件匹配（精确/后缀/文件名/歧义/未匹配统计）；
 *  - 点击候选（同名同行多候选、同文件同名、项目内同名）；
 *  - 源码 tokenizer（变量高亮/字符串/注释/预处理）+ 块注释跨行状态；
 *  - 行虚拟化窗口；LRU 缓存；labels 默认文案/占位符；
 *  - 可选：只读扫描真实项目根目录 $HOME/reference firmware（不存在则 SKIP）。
 *
 * 用法：
 *   node scripts/test-source-project.mjs
 *   REFERENCE_ROOT=/path/to/project node scripts/test-source-project.mjs
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const utilsOut = path.join(os.tmpdir(), `source-project-test-${process.pid}.mjs`)
const labelsOut = path.join(os.tmpdir(), `source-labels-test-${process.pid}.mjs`)
const loaderOut = path.join(os.tmpdir(), `source-loader-test-${process.pid}.mjs`)

execFileSync(esbuild, [
  path.join(root, 'src', 'utils', 'sourceProject.ts'),
  '--bundle', '--format=esm', '--platform=node', `--outfile=${utilsOut}`,
], {stdio: 'pipe'})
execFileSync(esbuild, [
  path.join(root, 'src', 'components', 'source', 'labels.ts'),
  '--bundle', '--format=esm', '--platform=node', `--outfile=${labelsOut}`,
], {stdio: 'pipe'})
// loader 依赖 @/utils/sourceProject 别名（esbuild 0.21+ --alias）
execFileSync(esbuild, [
  path.join(root, 'src', 'components', 'source', 'projectLoader.ts'),
  '--bundle', '--format=esm', '--platform=node', '--alias:@=./src', `--outfile=${loaderOut}`,
], {stdio: 'pipe', cwd: root})

const sp = await import(utilsOut)
const labels = await import(labelsOut)
const loader = await import(loaderOut)

let checks = 0
let failures = 0
const check = (name, condition, detail = '') => {
  checks += 1
  if (condition) {
    console.log(`  ok   ${name}`)
  } else {
    failures += 1
    console.log(`  FAIL ${name}${detail ? ` :: ${detail}` : ''}`)
  }
}
const eq = (name, actual, expected) => {
  const a = JSON.stringify(actual)
  const b = JSON.stringify(expected)
  check(name, a === b, `${a} != ${b}`)
}
const close = (name, actual, expected, epsilon = 1e-6) => {
  check(name, typeof actual === 'number' && Math.abs(actual - expected) <= epsilon, `${actual} != ${expected}`)
}
const fileEntry = (pathValue, size = 1024, lastModified = 0) => ({path: pathValue, size, lastModified})
const fileMetas = (paths) => sp.scanProjectEntries(paths.map((item) => fileEntry(item))).files

// ---------------------------------------------------------------------------
console.log('\n[1] 路径规范化')
// ---------------------------------------------------------------------------
eq('normalizePath: windows + backslashes', sp.normalizePath('C:\\work\\proj\\src\\a.c'), 'c:/work/proj/src/a.c')
eq('normalizePath: resolves . and ..', sp.normalizePath('/a//b/./c/../d'), '/a/b/d')
eq('normalizePath: relative .. kept', sp.normalizePath('a/../../b'), '../b')
eq('normalizePath: trailing slash removed', sp.normalizePath('src/inc/'), 'src/inc')
eq('normalizePath: empty', sp.normalizePath('   '), '')
eq('normalizeRelativePath: drive stripped', sp.normalizeRelativePath('C:\\work\\src\\a.c'), 'work/src/a.c')
eq('normalizeRelativePath: leading slash stripped', sp.normalizeRelativePath('/abs/x.c'), 'abs/x.c')
eq('normalizeRelativePath: .. clamped at root', sp.normalizeRelativePath('a/../../../b'), 'b')
eq('isAbsoluteDwarfPath: unix', sp.isAbsoluteDwarfPath('/work/src/a.c'), true)
eq('isAbsoluteDwarfPath: windows', sp.isAbsoluteDwarfPath('C:\\work\\src\\a.c'), true)
eq('isAbsoluteDwarfPath: relative', sp.isAbsoluteDwarfPath('src/a.c'), false)
eq('basename', sp.basename('a/b/c.h'), 'c.h')
eq('dirname', sp.dirname('a/b/c.h'), 'a/b')
eq('dirname: no dir', sp.dirname('c.h'), '')
eq('extname', sp.extname('a/b/c.HPP'), '.HPP')
eq('extname: dotfile', sp.extname('.gitignore'), '')
eq('pathSegments', sp.pathSegments('/a//b/c.h'), ['a', 'b', 'c.h'])
eq('joinPath', sp.joinPath('/work/proj', 'src/../inc/a.h'), '/work/proj/inc/a.h')
eq('splitWebkitRelativePath: rooted', sp.splitWebkitRelativePath('proj/src/main.c'), {rootName: 'proj', relativePath: 'src/main.c'})
eq('splitWebkitRelativePath: flat', sp.splitWebkitRelativePath('main.c'), {rootName: '', relativePath: 'main.c'})
eq('stripRootSegment: strip', sp.stripRootSegment('proj/src/main.c', 'proj'), 'src/main.c')
eq('stripRootSegment: no match', sp.stripRootSegment('other/src/main.c', 'proj'), 'other/src/main.c')

// ---------------------------------------------------------------------------
console.log('\n[2] 扩展名/忽略目录')
// ---------------------------------------------------------------------------
eq('hasSourceExtension: lower', sp.hasSourceExtension('main.c'), true)
eq('hasSourceExtension: upper', sp.hasSourceExtension('WIN.CPP'), true)
eq('hasSourceExtension: header', sp.hasSourceExtension('include/a.hxx'), true)
eq('hasSourceExtension: asm false', sp.hasSourceExtension('startup.S'), false)
eq('hasSourceExtension: custom', sp.hasSourceExtension('startup.S', ['.S']), true)
eq('hasSourceExtension: no ext', sp.hasSourceExtension('Makefile'), false)
eq('hasElfExtension', sp.hasElfExtension('build/firmware.ELF'), true)
eq('isIgnoredDirectory: .git', sp.isIgnoredDirectory('.git'), true)
eq('isIgnoredDirectory: node_modules', sp.isIgnoredDirectory('node_modules'), true)
eq('isIgnoredDirectory: build', sp.isIgnoredDirectory('build'), true)
eq('isIgnoredDirectory: build-esp32s3', sp.isIgnoredDirectory('build-esp32s3'), true)
eq('isIgnoredDirectory: cmake-build-debug', sp.isIgnoredDirectory('cmake-build-debug'), true)
eq('isIgnoredDirectory: hidden', sp.isIgnoredDirectory('.vscode'), true)
eq('isIgnoredDirectory: src not ignored', sp.isIgnoredDirectory('src'), false)
eq('isIgnoredDirectory: components not ignored', sp.isIgnoredDirectory('components'), false)
eq('isIgnoredDirectory: extra list', sp.isIgnoredDirectory('third_party_tmp', ['third_party_tmp']), true)

// ---------------------------------------------------------------------------
console.log('\n[3] 目录扫描 / 统计 / 文件树 / ELF')
// ---------------------------------------------------------------------------
const oversized = 2 * 1024 * 1024 + 1
const entries = [
  fileEntry('src/main.c', 1000, 100),
  fileEntry('src/util.h', 2000, 200),
  fileEntry('lib/foo.cpp', 3000, 300),
  fileEntry('src/WIN.C', 500, 400),
  fileEntry('build/generated.c', 400, 500),
  fileEntry('build/firmware.elf', 900000, 5000),
  fileEntry('assets/logo.png', 100, 0),
  fileEntry('src/huge.c', oversized, 0),
]
const meta = sp.scanProjectEntries(entries, {rootName: 'demo', access: 'input'})
eq('scan: sourceCount', meta.sourceCount, 4)
eq('scan: rootName', meta.rootName, 'demo')
eq('scan: access', meta.access, 'input')
eq('scan: files sorted', meta.files.map((file) => file.path), ['lib/foo.cpp', 'src/WIN.C', 'src/main.c', 'src/util.h'])
eq('scan: stats.totalEntries', meta.stats.totalEntries, 8)
eq('scan: stats.ignoredDirectoryEntries', meta.stats.ignoredDirectoryEntries, 1)
eq('scan: stats.unsupportedFiles', meta.stats.unsupportedFiles, 1)
eq('scan: stats.tooLargeFiles', meta.stats.tooLargeFiles, 1)
eq('scan: stats.overLimitFiles', meta.stats.overLimitFiles, 0)
eq('scan: stats.elfCount', meta.stats.elfCount, 1)
eq('scan: elfFiles', meta.elfFiles.map((elf) => elf.path), ['build/firmware.elf'])
eq('scan: limits defaults', meta.limits, {maxFiles: 2000, maxFileBytes: 2 * 1024 * 1024, maxElfFiles: 16})
eq('scan: tree root name', meta.tree.name, 'demo')
eq('scan: tree root total', meta.tree.totalFileCount, 4)
eq('scan: tree dirs', meta.tree.dirs.map((dir) => dir.name), ['lib', 'src'])
eq('scan: tree lib total', meta.tree.dirs[0].totalFileCount, 1)
eq('scan: tree src files', meta.tree.dirs[1].files.map((file) => file.name), ['WIN.C', 'main.c', 'util.h'])
eq('scan: tree paths', meta.tree.dirs[1].files.map((file) => file.path), ['src/WIN.C', 'src/main.c', 'src/util.h'])

// 文件数量上限
const many = Array.from({length: 7}, (_, index) => fileEntry(`src/f${index}.c`, 10, index))
const capped = sp.scanProjectEntries(many, {maxFiles: 3})
eq('scan cap: sourceCount', capped.sourceCount, 3)
eq('scan cap: overLimitFiles', capped.stats.overLimitFiles, 4)
eq('scan cap: limits retained', capped.limits.maxFiles, 3)
eq('scan cap: deterministic first paths', capped.files.map((file) => file.path), ['src/f0.c', 'src/f1.c', 'src/f2.c'])

// ELF 按 lastModified 取最新、并支持上限
const elfEntries = [
  fileEntry('build/old.elf', 100, 1000),
  fileEntry('build/new.elf', 100, 3000),
  fileEntry('out/mid.elf', 100, 2000),
]
const elfMeta = sp.scanProjectEntries(elfEntries, {maxElfFiles: 2})
eq('elf: sorted by lastModified desc', elfMeta.elfFiles.map((elf) => elf.path), ['build/new.elf', 'out/mid.elf'])
eq('elf: pickLatestElf', sp.pickLatestElf(elfMeta.elfFiles)?.path, 'build/new.elf')
eq('elf: elfCount counts all', elfMeta.stats.elfCount, 3)
eq('elf: sourceCount 0', elfMeta.sourceCount, 0)

// 自定义扩展名/忽略目录
const custom = sp.scanProjectEntries([
  fileEntry('src/startup.S', 10),
  fileEntry('tmp/code.c', 10),
], {sourceExtensions: ['.S'], ignoredDirectories: ['tmp']})
eq('scan custom: only .S accepted', custom.files.map((file) => file.path), ['src/startup.S'])
eq('scan custom: ignored tmp', custom.stats.ignoredDirectoryEntries, 1)

// buildSourceTree 独立使用
const builtTree = sp.buildSourceTree(fileMetas(['a/x.c', 'a/b/y.h', 'z.c']), 'root')
eq('buildSourceTree: dirs sorted', builtTree.dirs.map((dir) => dir.name), ['a'])
eq('buildSourceTree: a dirs', builtTree.dirs[0].dirs.map((dir) => dir.name), ['b'])
eq('buildSourceTree: fileCount', builtTree.fileCount, 1)
eq('buildSourceTree: totalFileCount', builtTree.totalFileCount, 3)

// 扁平化
const flatExpanded = new Set(['lib', 'src'])
const rows = sp.flattenSourceTree(meta.tree, flatExpanded)
eq('flatten: expanded row kinds', rows.map((row) => `${row.kind}:${row.path}`), [
  'directory:lib', 'file:lib/foo.cpp',
  'directory:src', 'file:src/WIN.C', 'file:src/main.c', 'file:src/util.h',
])
eq('flatten: depths', rows.map((row) => row.depth), [0, 1, 0, 1, 1, 1])
const collapsedRows = sp.flattenSourceTree(meta.tree, new Set())
eq('flatten: collapsed dirs only', collapsedRows.map((row) => row.path), ['lib', 'src'])

// 文件搜索
eq('searchSourceFiles: substring', sp.searchSourceFiles(meta.files, 'pid'), [])
const searchFiles = sp.searchSourceFiles(meta.files, 'src/')
eq('searchSourceFiles: by path', searchFiles.map((file) => file.path), ['src/WIN.C', 'src/main.c', 'src/util.h'])
eq('searchSourceFiles: limit', sp.searchSourceFiles(meta.files, 'src/', 2).length, 2)
eq('searchSourceFiles: empty query', sp.searchSourceFiles(meta.files, '', 2).length, 2)

// ---------------------------------------------------------------------------
console.log('\n[4] LRU 缓存 / 大小格式化')
// ---------------------------------------------------------------------------
const lru = new sp.LruCache(2)
lru.set('a', 1)
lru.set('b', 2)
eq('lru: get hit', lru.get('a'), 1)
eq('lru: recency stored (b oldest)', lru.keys(), ['b', 'a'])
lru.set('c', 3)
eq('lru: evicts oldest', lru.has('b'), false)
eq('lru: keys after eviction', lru.keys(), ['a', 'c'])
eq('lru: size', lru.size, 2)
eq('lru: maxSize', lru.maxSize, 2)
eq('lru: peek', lru.peek('c'), 3)
eq('lru: delete', lru.delete('a'), true)
eq('lru: delete missing', lru.delete('a'), false)
lru.set('d', 4)
eq('lru: overwrite keeps size', lru.set('d', 5), undefined)
eq('lru: overwrite value', lru.get('d'), 5)
lru.clear()
eq('lru: clear', lru.size, 0)
const lruOne = new sp.LruCache(1)
lruOne.set(1, 'x')
lruOne.set(2, 'y')
eq('lru: capacity 1', lruOne.keys(), [2])
eq('formatFileSize: bytes', sp.formatFileSize(512), '512 B')
eq('formatFileSize: KB', sp.formatFileSize(2048), '2.0 KB')
eq('formatFileSize: MB', sp.formatFileSize(2 * 1024 * 1024), '2.0 MB')
eq('formatFileSize: invalid', sp.formatFileSize(Number.NaN), '0 B')

// ---------------------------------------------------------------------------
console.log('\n[5] 变量展开 / 标识符 / 选中 key / 路径候选')
// ---------------------------------------------------------------------------
eq('variableIdentifier: shortName priority', sp.variableIdentifier({name: 'a::b', shortName: 'b'}), 'b')
eq('variableIdentifier: qualified', sp.variableIdentifier({name: 'ns::Class::value'}), 'value')
eq('variableIdentifier: member dotted', sp.variableIdentifier({name: '.member'}), 'member')
eq('variableIdentifier: array element', sp.variableIdentifier({name: '[3]'}), '')
eq('variableIdentifier: invalid chars', sp.variableIdentifier({name: 'foo-bar'}), '')
eq('sourceVariableKey: matches DebugCurve nodeKey', sp.sourceVariableKey('obj.member', {address: 0x20000010}), 'obj.member@20000010')
eq('isVariableSampleable: explicit', sp.isVariableSampleable({sampleable: true}), true)
eq('isVariableSampleable: explicit false', sp.isVariableSampleable({sampleable: false, sample: {address: 1}}), false)
eq('isVariableSampleable: fallback sample', sp.isVariableSampleable({sample: {address: 1}}), true)
eq('isVariableSampleable: fallback location', sp.isVariableSampleable({location: {kind: 'addr'}}), true)
eq('variableSourcePaths: relativeFile first', sp.variableSourcePaths({relativeFile: 'src/a.c', file: '/work/src/a.c'}), ['src/a.c', '/work/src/a.c'])
eq('variableSourcePaths: compDir join', sp.variableSourcePaths({file: 'src/a.c', compDir: '/work/proj'}), ['/work/proj/src/a.c', 'src/a.c'])
eq('variableSourcePaths: windows normalized', sp.variableSourcePaths({file: 'C:\\work\\src\\a.c'}), ['c:/work/src/a.c'])
eq('variableSourcePaths: empty', sp.variableSourcePaths({}), [])

const treeVariables = [
  {
    name: 'sn_tools.data', shortName: 'data', qualifiedName: 'sn_tools::data', typeName: 'int',
    relativeFile: 'src/main.c', line: 10, address: 0x20000010, sampleable: true, sample: {address: 0x20000010},
    children: [
      {name: '.x', shortName: 'x', typeName: 'float', relativeFile: 'src/main.c', line: 10, address: 0x20000014, sampleable: true, sample: {address: 0x20000014}},
      {name: '.y', shortName: 'y', typeName: 'float', relativeFile: 'src/main.c', line: 10, address: 0x20000018, sampleable: true, sample: {address: 0x20000018}},
    ],
  },
  {name: 'g_speed', shortName: 'g_speed', qualifiedName: 'g_speed', typeName: 'float', relativeFile: 'src/main.c', line: 28, address: 0x20000030, sampleable: true, sample: {address: 0x20000030}},
]
const walked = sp.walkSourceVariables(treeVariables)
eq('walk: node count', walked.length, 4)
eq('walk: paths', walked.map((entry) => entry.path), ['sn_tools.data', 'sn_tools.data.x', 'sn_tools.data.y', 'g_speed'])
eq('walk: keys', walked.map((entry) => entry.key), [
  'sn_tools.data@20000010', 'sn_tools.data.x@20000014', 'sn_tools.data.y@20000018', 'g_speed@20000030',
])
eq('walk: depths', walked.map((entry) => entry.depth), [0, 1, 1, 0])
eq('walk: children disabled', sp.walkSourceVariables(treeVariables, {includeChildren: false}).length, 2)
eq('walk: maxNodes', sp.walkSourceVariables(treeVariables, {maxNodes: 3}).length, 3)
eq('walk: original reference kept', walked[0].variable, treeVariables[0])

// ---------------------------------------------------------------------------
console.log('\n[6] 变量 -> 项目文件匹配 / 候选')
// ---------------------------------------------------------------------------
const projectFiles = fileMetas([
  'src/main.c',
  'src/pid.c',
  'lib/motor/motor.cpp',
  'src/system/FreeRTOS/db.cpp',
  'include/common.h',
  'other/common.h',
])
const index = sp.createSourceFileIndex(projectFiles)

const pathMatchExact = sp.matchPathToProjectFile('src/main.c', index)
eq('match path: exact file', pathMatchExact?.file.path, 'src/main.c')
eq('match path: exact score', pathMatchExact?.score, 110)
eq('match path: exact not ambiguous', pathMatchExact?.ambiguous, false)
eq('match path: case insensitive', sp.matchPathToProjectFile('SRC/MAIN.C', index)?.file.path, 'src/main.c')
eq('match path: absolute suffix', sp.matchPathToProjectFile('/work/proj/src/main.c', index)?.file.path, 'src/main.c')
eq('match path: windows absolute suffix', sp.matchPathToProjectFile('C:\\work\\proj\\src\\main.c', index)?.file.path, 'src/main.c')
eq('match path: short suffix', sp.matchPathToProjectFile('FreeRTOS/db.cpp', index)?.file.path, 'src/system/FreeRTOS/db.cpp')
eq('match path: basename fallback', sp.matchPathToProjectFile('motor.cpp', index)?.file.path, 'lib/motor/motor.cpp')
const ambiguousMatch = sp.matchPathToProjectFile('vendor/common.h', index)
eq('match path: ambiguous basename', ambiguousMatch?.ambiguous, true)
eq('match path: ambiguous chooses shortest', ambiguousMatch?.file.path, 'include/common.h')
eq('match path: unmatched', sp.matchPathToProjectFile('not/here/zzz.c', index), null)
eq('match path: empty', sp.matchPathToProjectFile('', index), null)

const matchVariables = [
  {name: 'g_speed', shortName: 'g_speed', typeName: 'float', relativeFile: 'src/main.c', file: '/work/proj/src/main.c', line: 10, address: 0x20000010, sampleable: true, sample: {address: 0x20000010}},
  {name: 'pid.kp', shortName: 'kp', typeName: 'float', relativeFile: 'src/pid.c', line: 42, address: 0x20000020, sampleable: true, sample: {address: 0x20000020}},
  {name: 'value', shortName: 'value', typeName: 'int', relativeFile: 'src/pid.c', line: 50, address: 0x20000030, sampleable: true, sample: {address: 0x20000030}},
  {name: 'value', shortName: 'value', typeName: 'int', relativeFile: 'src/pid.c', line: 50, address: 0x20000034, sampleable: true, sample: {address: 0x20000034}},
  {name: 'winvar', shortName: 'winvar', typeName: 'int', relativeFile: 'src\\main.c', line: 12, address: 0x20000040, sampleable: true, sample: {address: 0x20000040}},
  {name: 'unmatched_local', shortName: 'unmatched_local', relativeFile: '/some/other/x.c', line: 3, address: 0x20000050, sampleable: true, sample: {address: 0x20000050}},
  {name: 'motor_state', shortName: 'motor_state', relativeFile: 'motor.cpp', line: 7, address: 0x20000060, sampleable: true, sample: {address: 0x20000060}},
  {name: 'db_counter', shortName: 'db_counter', relativeFile: 'FreeRTOS/db.cpp', line: 9, address: 0x20000070, sampleable: true, sample: {address: 0x20000070}},
  {
    name: 'sn_tools.data', shortName: 'data', typeName: 'struct', relativeFile: 'src/main.c', line: 30, address: 0x20000080, sampleable: true,
    children: [
      {name: '.x', shortName: 'x', typeName: 'float', relativeFile: 'src/main.c', line: 30, address: 0x20000084, sampleable: true, sample: {address: 0x20000084}},
    ],
  },
]
const matchResult = sp.mapVariablesToProjectFiles(matchVariables, projectFiles)
eq('map: total entries', matchResult.total, 10)
eq('map: matchedCount', matchResult.matchedCount, 9)
eq('map: unmatchedCount', matchResult.unmatchedCount, 1)
eq('map: unmatched variable', matchResult.unmatched[0].variable.shortName, 'unmatched_local')
eq('map: ambiguousCount', matchResult.ambiguousCount, 0)
eq('map: files with variables', [...matchResult.refsByFile.keys()].sort(), [
  'lib/motor/motor.cpp', 'src/main.c', 'src/pid.c', 'src/system/FreeRTOS/db.cpp',
])
eq('map: src/main.c refs', matchResult.refsByFile.get('src/main.c').map((ref) => ref.path), ['g_speed', 'winvar', 'sn_tools.data', 'sn_tools.data.x'])
eq('map: src/main.c lines', matchResult.refsByFile.get('src/main.c').map((ref) => ref.line), [10, 12, 30, 30])
eq('map: winvar windows path matched', sp.refsForFile(matchResult, 'src/main.c').some((ref) => ref.identifier === 'winvar'), true)
eq('map: motor suffix matched', sp.refsForFile(matchResult, 'lib/motor/motor.cpp')[0]?.identifier, 'motor_state')
eq('map: db suffix matched', sp.refsForFile(matchResult, 'src/system/FreeRTOS/db.cpp')[0]?.identifier, 'db_counter')
eq('map: refsForFile unknown', sp.refsForFile(matchResult, 'nope.c'), [])
eq('map: collectIdentifiers', [...sp.collectIdentifiers(matchResult.refsByFile.get('src/pid.c'))].sort(), ['kp', 'value'])
eq('map: variableLines', sp.variableLines(matchResult.refsByFile.get('src/pid.c')), [42, 50])
eq('map: grouped lines', [...sp.groupReferencesByLine(matchResult.refsByFile.get('src/pid.c')).keys()], [42, 50])
eq('map: referencesAtLine count', sp.referencesAtLine(matchResult.refsByFile.get('src/pid.c'), 50).length, 2)
eq('map: pickInitialSourceFile', sp.pickInitialSourceFile(matchResult), 'src/main.c')

// 点击候选：同名同一行两个候选
const candidatesSameLine = sp.findVariableCandidates(matchResult, {filePath: 'src/pid.c', line: 50, identifier: 'value'})
eq('candidates: same line count', candidatesSameLine.length, 2)
eq('candidates: scope line', candidatesSameLine.map((candidate) => candidate.scope), ['line', 'line'])
eq('candidates: lineMatched', candidatesSameLine.map((candidate) => candidate.lineMatched), [true, true])
eq('candidates: sampleable', candidatesSameLine.map((candidate) => candidate.sampleable), [true, true])
eq('candidates: distinct keys', new Set(candidatesSameLine.map((candidate) => candidate.ref.key)).size, 2)
// 使用行（声明行不同）-> 同文件候选
const candidatesUsage = sp.findVariableCandidates(matchResult, {filePath: 'src/pid.c', line: 66, identifier: 'value'})
eq('candidates: usage line scope file', candidatesUsage.map((candidate) => candidate.scope), ['file', 'file'])
eq('candidates: usage line lineMatched false', candidatesUsage.map((candidate) => candidate.lineMatched), [false, false])
// 项目内同名（其它文件）
const candidatesProject = sp.findVariableCandidates(matchResult, {filePath: 'src/main.c', line: 5, identifier: 'value'})
eq('candidates: project fallback', candidatesProject.map((candidate) => candidate.scope), ['project', 'project'])
eq('candidates: project files', candidatesProject.map((candidate) => candidate.ref.entry.variable.relativeFile), ['src/pid.c', 'src/pid.c'])
// 关闭项目回退
eq('candidates: includeProject false', sp.findVariableCandidates(matchResult, {
  filePath: 'src/main.c', line: 5, identifier: 'value', includeProject: false,
}).length, 0)
// 未知标识符/空标识符
eq('candidates: unknown identifier', sp.findVariableCandidates(matchResult, {filePath: 'src/pid.c', line: 50, identifier: 'nope'}), [])
eq('candidates: empty identifier', sp.findVariableCandidates(matchResult, {filePath: 'src/pid.c', line: 50, identifier: '  '}), [])
// 子成员
const memberCandidates = sp.findVariableCandidates(matchResult, {filePath: 'src/main.c', line: 30, identifier: 'x'})
eq('candidates: member identifier', memberCandidates.length, 1)
eq('candidates: member path', memberCandidates[0].ref.path, 'sn_tools.data.x')

// matchVariablesToFiles 直接入口（不重复 walk）
const directResult = sp.matchVariablesToFiles(walked.slice(0, 2), sp.createSourceFileIndex(projectFiles))
eq('matchVariablesToFiles: direct matched', directResult.matchedCount, 2)

// ---------------------------------------------------------------------------
console.log('\n[7] 源码 tokenizer / 块注释状态')
// ---------------------------------------------------------------------------
const sourceLine = 'if (g_speed > value) { /* set // not comment */ x = "a/*b"; }'
const tokenized = sp.tokenizeCodeLine(sourceLine, false, {variableNames: new Set(['g_speed', 'value'])})
eq('tokenize: reconstructs line', tokenized.tokens.map((token) => token.text).join(''), sourceLine)
eq('tokenize: variable tokens', tokenized.tokens.filter((token) => token.kind === 'variable').map((token) => token.variable), ['g_speed', 'value'])
check('tokenize: keyword token', tokenized.tokens.some((token) => token.kind === 'keyword' && token.text === 'if'))
check('tokenize: string token', tokenized.tokens.some((token) => token.kind === 'string' && token.text === '"a/*b"'))
check('tokenize: comment token', tokenized.tokens.some((token) => token.kind === 'comment' && token.text.includes('not comment')))
check('tokenize: no variable inside comment', !tokenized.tokens.filter((token) => token.kind === 'comment').some((token) => token.text.includes('g_speed')))

const openComment = sp.tokenizeCodeLine('/* start', false)
eq('tokenize: opens block comment', openComment.inBlockComment, true)
const insideComment = sp.tokenizeCodeLine('still comment */ g_speed', true, {variableNames: new Set(['g_speed'])})
eq('tokenize: continues block comment', insideComment.inBlockComment, false)
eq('tokenize: variable after comment closed', insideComment.tokens.filter((token) => token.kind === 'variable').map((token) => token.variable), ['g_speed'])
eq('tokenize: comment token text', insideComment.tokens[0].kind, 'comment')
eq('tokenize: comment text', insideComment.tokens[0].text, 'still comment */')

const lineComment = sp.tokenizeCodeLine('// g_speed is a comment', false, {variableNames: new Set(['g_speed'])})
eq('tokenize: line comment whole rest', lineComment.tokens.length, 1)
eq('tokenize: line comment kind', lineComment.tokens[0].kind, 'comment')
eq('tokenize: no variable in line comment', lineComment.tokens.filter((token) => token.kind === 'variable').length, 0)

const charLiteral = sp.tokenizeCodeLine("char c = '\\''; int n = 0x1F;", false, {variableNames: new Set(['c', 'n'])})
eq('tokenize: char literal reconstructs', charLiteral.tokens.map((token) => token.text).join(''), "char c = '\\''; int n = 0x1F;")
eq('tokenize: char literal kind', charLiteral.tokens.find((token) => token.kind === 'char')?.text, "'\\''")
check('tokenize: hex number', charLiteral.tokens.some((token) => token.kind === 'number' && token.text === '0x1F'))

const preprocessor = sp.tokenizeCodeLine('#define MAX_SPEED 100', false, {variableNames: new Set(['MAX_SPEED'])})
eq('tokenize: preprocessor no variable highlight', preprocessor.tokens.filter((token) => token.kind === 'variable').length, 0)
check('tokenize: preprocessor kind', preprocessor.tokens.some((token) => token.kind === 'preprocessor' && token.text === 'MAX_SPEED'))

const states = sp.computeBlockCommentStates([
  'int a; /* c1',
  'c2 */ int b;',
  'const char* s = "/*";',
  'int c;',
])
eq('comment states: line starts', [...states], [0, 1, 0, 0])
eq('comment states: string does not open comment', sp.lineEndsInBlockComment('const char* s = "/*";'), false)
eq('comment states: open comment', sp.lineEndsInBlockComment('int a; /* c1'), true)
eq('comment states: closed comment', sp.lineEndsInBlockComment('c2 */ int b;', true), false)

// 变量名边界：g_speed 不应命中 g_speed2
const boundary = sp.tokenizeCodeLine('g_speed2 = g_speed;', false, {variableNames: new Set(['g_speed'])})
eq('tokenize: identifier boundary', boundary.tokens.filter((token) => token.kind === 'variable').map((token) => token.text), ['g_speed'])

// ---------------------------------------------------------------------------
console.log('\n[8] 行虚拟化窗口')
// ---------------------------------------------------------------------------
const win = sp.computeVirtualWindow({scrollTop: 200, viewportHeight: 100, rowHeight: 20, lineCount: 100, overscan: 2})
eq('virtual: start', win.start, 8)
eq('virtual: end', win.end, 17)
eq('virtual: offsetY', win.offsetY, 160)
eq('virtual: totalHeight', win.totalHeight, 2000)
eq('virtual: overscan', win.overscan, 2)
eq('virtual: top of file', sp.computeVirtualWindow({scrollTop: 0, viewportHeight: 100, rowHeight: 20, lineCount: 100}).start, 0)
const bottomWin = sp.computeVirtualWindow({scrollTop: 1e6, viewportHeight: 100, rowHeight: 20, lineCount: 10})
eq('virtual: clamped start', bottomWin.start, 9)
eq('virtual: clamped end', bottomWin.end, 10)
eq('virtual: empty file', sp.computeVirtualWindow({scrollTop: 0, viewportHeight: 100, rowHeight: 20, lineCount: 0}), {
  start: 0, end: 0, offsetY: 0, totalHeight: 0, overscan: 0,
})
const smallWin = sp.computeVirtualWindow({scrollTop: 0, viewportHeight: 0, rowHeight: 20, lineCount: 50, overscan: 2})
check('virtual: zero viewport still renders overscan rows', smallWin.end > smallWin.start)
eq('virtual: invalid rowHeight fallback', sp.computeVirtualWindow({scrollTop: 0, viewportHeight: 20, rowHeight: 0, lineCount: 5}).totalHeight, 5)

// ---------------------------------------------------------------------------
console.log('\n[9] labels（默认文案 / 占位符覆盖）')
// ---------------------------------------------------------------------------
eq('labels: default openProject', labels.DEFAULT_SOURCE_BROWSER_LABELS.openProject, 'Open project folder…')
eq('labels: format placeholder', labels.formatSourceBrowserLabel('Line {line} of {file}:{line}', {line: 12, file: 'a.c'}), 'Line 12 of a.c:12')
eq('labels: unknown placeholder kept', labels.formatSourceBrowserLabel('x={missing}', {}), 'x={missing}')
const mergedLabels = labels.resolveSourceBrowserLabels({openProject: 'OPEN', noVariables: 'NONE'})
eq('labels: merge override', mergedLabels.openProject, 'OPEN')
eq('labels: merge keeps default', mergedLabels.chooseElf, labels.DEFAULT_SOURCE_BROWSER_LABELS.chooseElf)
eq('labels: resolve null', labels.resolveSourceBrowserLabels(null), labels.DEFAULT_SOURCE_BROWSER_LABELS)
eq('labels: limitsHint placeholder', labels.formatSourceBrowserLabel(labels.DEFAULT_SOURCE_BROWSER_LABELS.limitsHint, {
  maxFiles: 2000, maxBytes: '2.0 MB',
}), 'Limits: 2000 files, 2.0 MB per file.')

// ---------------------------------------------------------------------------
console.log('\n[11] projectLoader（webkitdirectory FileList -> SourceProject，按需读取）')
// ---------------------------------------------------------------------------
if (typeof File === 'undefined') {
  console.log('  SKIP Node 无全局 File（需要 Node >= 20）')
} else {
  const makeFile = (webkitPath, content, options = {}) => {
    const file = new File([content], webkitPath.split('/').pop(), {lastModified: options.lastModified ?? 1000})
    Object.defineProperty(file, 'webkitRelativePath', {value: webkitPath})
    if (options.size !== undefined) Object.defineProperty(file, 'size', {value: options.size})
    return file
  }
  const fileList = [
    makeFile('demo/src/main.c', 'int g_speed = 0;\n', {lastModified: 100}),
    makeFile('demo/src/pid.c', 'float kp = 1.0f;\n', {lastModified: 200}),
    makeFile('demo/build/firmware.elf', 'ELF-BYTES', {lastModified: 5000}),
    makeFile('demo/assets/logo.png', 'PNG'),
    makeFile('demo/src/huge.c', 'x', {size: 2 * 1024 * 1024 + 1}),
  ]
  const loaded = await loader.createProjectFromFileList(fileList)
  eq('loader: rootName from webkitRelativePath', loaded.rootName, 'demo')
  eq('loader: access input', loaded.access, 'input')
  eq('loader: sourceCount', loaded.sourceCount, 2)
  eq('loader: paths stripped of root', loaded.files.map((file) => file.path), ['src/main.c', 'src/pid.c'])
  eq('loader: latest ELF path', loaded.elfFiles[0]?.path, 'build/firmware.elf')
  eq('loader: too-large filtered', loaded.stats.tooLargeFiles, 1)
  eq('loader: unsupported counted', loaded.stats.unsupportedFiles, 1)
  eq('loader: no text read during scan (cache empty)', loaded.textCacheSize(), 0)
  const mainText = await loaded.readText('src/main.c')
  eq('loader: on-demand text read', mainText, 'int g_speed = 0;\n')
  eq('loader: LRU populated after read', loaded.textCacheSize(), 1)
  const elfFile = await loaded.readElfFile('build/firmware.elf')
  check('loader: readElfFile returns File', elfFile instanceof File)
  eq('loader: readElfFile name', elfFile?.name, 'firmware.elf')
  eq('loader: readElfFile missing', await loaded.readElfFile('nope.elf'), null)
  let readError = null
  try {
    await loaded.readText('nope.c')
  } catch (error) {
    readError = error
  }
  check('loader: missing source rejects', readError instanceof Error)
  eq('loader: failed read not cached', loaded.textCacheSize(), 1)
  const tinyCache = await loader.createProjectFromFileList(fileList.slice(0, 2), {cacheSize: 1})
  await tinyCache.readText('src/main.c')
  await tinyCache.readText('src/pid.c')
  eq('loader: LRU capacity respected', tinyCache.textCacheSize(), 1)
  const namedProject = await loader.createProjectFromFileList([
    new File(['int a;'], 'main.c', {lastModified: 1}),
    new File(['int b;'], 'util.h', {lastModified: 2}),
  ], {rootName: 'external'})
  eq('loader: external file list rootName option', namedProject.rootName, 'external')
  eq('loader: external flat file paths', namedProject.files.map((file) => file.path).sort(), ['main.c', 'util.h'])
  eq('loader: directory picker unavailable in Node', loader.getDirectoryPickerAvailability(), 'unsupported')
  eq('loader: supportsDirectoryPicker false in Node', loader.supportsDirectoryPicker(), false)
}

// ---------------------------------------------------------------------------
console.log('\n[12] task-29：有变量文件过滤 / 实时值 / 行地址 / 按 key 定位')
// ---------------------------------------------------------------------------
const keepSet = new Set(['src/main.c', 'src/pid.c'])
const filtered = sp.filterSourceFiles(projectFiles, keepSet)
eq('filter: only matched files', filtered.map((file) => file.path), ['src/main.c', 'src/pid.c'])
eq('filter: Map input (refsByFile)', sp.filterSourceFiles(projectFiles, new Map([['lib/motor/motor.cpp', []], ['src/main.c', []]])).map((file) => file.path), ['lib/motor/motor.cpp', 'src/main.c'])
eq('filter: windows path normalization', sp.filterSourceFiles(projectFiles, new Set(['src\\main.c'])).map((file) => file.path), ['src/main.c'])
eq('filter: null -> empty', sp.filterSourceFiles(projectFiles, null), [])
eq('filter: empty set -> empty', sp.filterSourceFiles(projectFiles, new Set()), [])

const variableTree = sp.buildVariableSourceTree(projectFiles, keepSet, 'proj')
eq('variable tree: only kept dirs', variableTree.dirs.map((dir) => dir.name), ['src'])
eq('variable tree: total kept files', variableTree.totalFileCount, 2)
eq('variable tree: root name', variableTree.name, 'proj')
eq('variable tree: kept files', variableTree.dirs[0].files.map((file) => file.name), ['main.c', 'pid.c'])
const motorTree = sp.buildVariableSourceTree(projectFiles, new Set(['lib/motor/motor.cpp']))
eq('variable tree: nested dirs kept', motorTree.dirs.map((dir) => dir.name), ['lib'])
eq('variable tree: nested total', motorTree.dirs[0].totalFileCount, 1)

// 按 key 定位（复用 [6] 的 matchResult）
const gSpeedKey = sp.sourceVariableKey('g_speed', {address: 0x20000010})
const keyed = sp.findVariableLocationByKey(matchResult, gSpeedKey)
eq('find by key: file', keyed?.filePath, 'src/main.c')
eq('find by key: line', keyed?.ref.line, 10)
eq('find by key: identifier', keyed?.ref.identifier, 'g_speed')
eq('find by key: missing key', sp.findVariableLocationByKey(matchResult, 'nope@0'), null)
eq('find by key: blank key', sp.findVariableLocationByKey(matchResult, '  '), null)
eq('find by key: null key', sp.findVariableLocationByKey(matchResult, null), null)

// 实时值（liveValues）
const mainRefs = matchResult.refsByFile.get('src/main.c')
const liveRefs = mainRefs.slice(0, 2)
const liveMap = {
  [liveRefs[0].key]: {text: '1.5', color: '#38bdf8'},
  [liveRefs[1].key]: {text: '7', stale: true},
  'not-in-file@0': {text: 'x'},
}
const liveEntries = sp.collectLiveValueEntries(liveRefs, liveMap)
eq('live values: count', liveEntries.length, 2)
eq('live values: key order', liveEntries.map((entry) => entry.key), [liveRefs[0].key, liveRefs[1].key])
eq('live values: text', liveEntries.map((entry) => entry.value.text), ['1.5', '7'])
eq('live values: color passthrough', liveEntries[0].value.color, '#38bdf8')
eq('live values: stale passthrough', liveEntries[1].value.stale, true)
eq('live values: identifier', liveEntries[0].identifier, 'g_speed')
eq('live values: dedupe same key', sp.collectLiveValueEntries([liveRefs[0], liveRefs[0]], liveMap).length, 1)
eq('live values: null map', sp.collectLiveValueEntries(liveRefs, null), [])
eq('live values: unknown keys skipped', sp.collectLiveValueEntries(liveRefs, {}), [])
eq('live values: non-string text skipped', sp.collectLiveValueEntries([liveRefs[0]], {[liveRefs[0].key]: {text: 42}}), [])

// 行地址（line-click / 断点）
const lineAddresses = {'src/main.c': {10: 0x08001235, 12: 0}}
eq('line address: direct', sp.resolveLineAddress(lineAddresses, 'src/main.c', 10), 0x08001235)
eq('line address: windows path normalized', sp.resolveLineAddress(lineAddresses, 'src\\main.c', 10), 0x08001235)
eq('line address: other file', sp.resolveLineAddress(lineAddresses, 'src/pid.c', 10), undefined)
eq('line address: zero filtered', sp.resolveLineAddress(lineAddresses, 'src/main.c', 12), undefined)
eq('line address: unknown line', sp.resolveLineAddress(lineAddresses, 'src/main.c', 99), undefined)
eq('line address: invalid line', sp.resolveLineAddress(lineAddresses, 'src/main.c', 0), undefined)
eq('line address: null map', sp.resolveLineAddress(null, 'src/main.c', 10), undefined)
eq('line address: absolute key form', sp.resolveLineAddress({'/abs/src/main.c': {3: 0x08000011}}, '/abs/src/main.c', 3), 0x08000011)

// ---------------------------------------------------------------------------
console.log('\n[13] task-29：SourceBrowser 组件静态契约（API/虚拟树/实时值/断点）')
// ---------------------------------------------------------------------------
const componentRoot = path.join(root, 'src', 'components', 'source')
const sourceBrowserSource = fs.readFileSync(path.join(componentRoot, 'SourceBrowser.vue'), 'utf8')
const sourceLabelsSource = fs.readFileSync(path.join(componentRoot, 'labels.ts'), 'utf8')
const sourceBrowserDocs = fs.readFileSync(path.join(root, '..', 'docs', 'source-browser.md'), 'utf8')
check('component: props liveValues/activeKey/lineAddresses', /liveValues\?: SourceLiveValueMap/.test(sourceBrowserSource)
  && /activeKey\?: string \| null/.test(sourceBrowserSource)
  && /lineAddresses\?: SourceLineAddressMap \| null/.test(sourceBrowserSource))
check('component: emit line-click payload path/line/address', /\(event: 'line-click', payload: SourceLineClickPayload\): void/.test(sourceBrowserSource))
check('component: 默认 Files with variables（showAllFiles=false）', /const showAllFiles = ref\(false\)/.test(sourceBrowserSource)
  && /!showAllFiles\.value && hasVariables\.value && matchedFilePaths\.value\.size > 0/.test(sourceBrowserSource))
check('component: 有变量文件过滤用 filterSourceFiles', /filterSourceFiles\(project\.files, matchedFilePaths\.value\)/.test(sourceBrowserSource)
  && /buildSourceTree\(visibleProjectFiles\.value, project\.rootName\)/.test(sourceBrowserSource))
check('component: Show all project files 开关', /v-model="showAllFiles"/.test(sourceBrowserSource)
  && /labels\.showAllProjectFiles/.test(sourceBrowserSource))
check('component: 虚拟树只渲染可见窗口', /treeViewportRef/.test(sourceBrowserSource)
  && /tree-canvas/.test(sourceBrowserSource)
  && /visibleTreeRows/.test(sourceBrowserSource)
  && /const TREE_ROW_HEIGHT = 24/.test(sourceBrowserSource)
  && /treeVirtualWindow/.test(sourceBrowserSource))
check('component: 行号 gutter 点击 emit line-click + 地址解析', /data-gutter/.test(sourceBrowserSource)
  && /onGutterClick/.test(sourceBrowserSource)
  && /resolveLineAddress\(props\.lineAddresses, path, line\)/.test(sourceBrowserSource)
  && /emit\('line-click'/.test(sourceBrowserSource))
check('component: 实时值渲染（行尾 chip/颜色点/stale）', /collectLiveValueEntries/.test(sourceBrowserSource)
  && /sb-live-value/.test(sourceBrowserSource)
  && /sb-live-dot/.test(sourceBrowserSource)
  && /entry\.value\.stale/.test(sourceBrowserSource))
check('component: activeKey 高亮（行/变量 token）', /activeKeyValue/.test(sourceBrowserSource)
  && /sb-line-active/.test(sourceBrowserSource)
  && /sb-token-active/.test(sourceBrowserSource))
check('component: expose scrollToVariableByKey', /scrollToVariableByKey,/.test(sourceBrowserSource)
  && /findVariableLocationByKey\(matchResult\.value, needle\)/.test(sourceBrowserSource))
check('component: unmatched 默认折叠（details 无 open）', /<details v-if="matchResult && matchResult\.unmatchedCount > 0" class="sb-unmatched">/.test(sourceBrowserSource)
  && !/<details[^>]*\sopen[\s>]/.test(sourceBrowserSource))
const newLabelFields = ['onlyFilesWithVariables', 'showAllProjectFiles', 'noFilesWithVariables', 'liveValueStale', 'breakpointLineHint', 'liveValues']
check('labels: task-29 新增字段可选且有默认英文', newLabelFields.every((field) => {
  const defaultValue = labels.DEFAULT_SOURCE_BROWSER_LABELS[field]
  return sourceLabelsSource.includes(`${field}?: string`) && typeof defaultValue === 'string' && defaultValue.length > 0
}))
eq('labels: showAllProjectFiles 默认文案', labels.DEFAULT_SOURCE_BROWSER_LABELS.showAllProjectFiles, 'Show all project files')
check('docs: SourceBrowser API 文档覆盖 task-29', ['liveValues', 'line-click', 'scrollToVariableByKey', 'lineAddresses', 'Files with variables'].every((needle) => sourceBrowserDocs.includes(needle)))

// ---------------------------------------------------------------------------
console.log('\n[10] 可选：只读扫描真实项目根目录')
// ---------------------------------------------------------------------------
const realRoot = process.env.REFERENCE_ROOT ?? '$HOME/reference firmware'
if (fs.existsSync(realRoot)) {
  const ignored = (name) => sp.isIgnoredDirectory(name)
  const sourceEntries = []
  let walkedEntries = 0
  const queue = [{dir: realRoot, rel: '', depth: 0}]
  while (queue.length > 0 && walkedEntries < 60000 && sourceEntries.length < 5000) {
    const current = queue.shift()
    let children
    try {
      children = fs.readdirSync(current.dir, {withFileTypes: true})
    } catch {
      continue
    }
    // `src` 优先入队，避免真实仓库里 lib/tinyusb 之类的大目录吃满收集上限。
    const dirRank = (entry) => (entry.isDirectory() ? (entry.name === 'src' ? 0 : entry.name === 'lib' ? 2 : 1) : 3)
    const ordered = [...children].sort((left, right) => dirRank(left) - dirRank(right) || left.name.localeCompare(right.name))
    for (const child of ordered) {
      walkedEntries += 1
      const relPath = current.rel ? `${current.rel}/${child.name}` : child.name
      if (child.isDirectory()) {
        if (!ignored(child.name) && current.depth < 16) queue.push({dir: path.join(current.dir, child.name), rel: relPath, depth: current.depth + 1})
        continue
      }
      if (!sp.hasSourceExtension(child.name)) continue
      try {
        const stat = fs.statSync(path.join(current.dir, child.name))
        sourceEntries.push({path: relPath, size: stat.size, lastModified: stat.mtimeMs})
      } catch {
        // ignore unreadable files
      }
    }
  }
  // ELF：构建目录通常被源码扫描忽略，这里单独浅层找（只读，不读内容）。
  const elfEntries = []
  const elfQueue = [{dir: realRoot, rel: '', depth: 0}]
  let elfScanned = 0
  while (elfQueue.length > 0 && elfScanned < 40000) {
    const current = elfQueue.shift()
    let children
    try {
      children = fs.readdirSync(current.dir, {withFileTypes: true})
    } catch {
      continue
    }
    for (const child of children) {
      elfScanned += 1
      const relPath = current.rel ? `${current.rel}/${child.name}` : child.name
      if (child.isDirectory()) {
        if (!child.name.startsWith('.') && current.depth < 6) elfQueue.push({dir: path.join(current.dir, child.name), rel: relPath, depth: current.depth + 1})
      } else if (sp.hasElfExtension(child.name)) {
        try {
          const stat = fs.statSync(path.join(current.dir, child.name))
          elfEntries.push({path: relPath, size: stat.size, lastModified: stat.mtimeMs})
        } catch {
          // ignore
        }
      }
    }
  }
  const realAllEntries = [...sourceEntries, ...elfEntries]
  const realMeta = sp.scanProjectEntries(realAllEntries, {rootName: path.basename(realRoot)})
  const realFullMeta = sp.scanProjectEntries(realAllEntries, {rootName: path.basename(realRoot), maxFiles: 20000})
  console.log(`  info real root: ${realRoot}`)
  console.log(`  info walked=${walkedEntries} sourceEntries=${sourceEntries.length} elfEntries=${elfEntries.length}`)
  console.log(`  info indexed sources=${realMeta.sourceCount} (truncated=${realMeta.stats.overLimitFiles}) ignored=${realMeta.stats.ignoredDirectoryEntries} unsupported=${realMeta.stats.unsupportedFiles} tooLarge=${realMeta.stats.tooLargeFiles}`)
  console.log(`  info latest ELF=${sp.pickLatestElf(realMeta.elfFiles)?.path ?? '-'} (${realMeta.elfFiles.length} found)`)
  check('real: found source files', realMeta.sourceCount > 0)
  check('real: respects max files', realMeta.sourceCount <= 2000)
  check('real: found ELF', realMeta.elfFiles.length > 0)
  const realFirstSource = realMeta.files.find((file) => file.path.startsWith('src/')) ?? realMeta.files[0]
  if (realFirstSource) {
    const realMatch = sp.mapVariablesToProjectFiles([
      {name: 'probe_var', shortName: 'probe_var', relativeFile: realFirstSource.path, line: 1, address: 0x20000000, sampleable: true, sample: {address: 0x20000000}},
    ], realMeta.files)
    eq('real: variable maps to real source path', realMatch.matchedCount, 1)
    eq('real: common source dirs indexed', realFullMeta.tree.dirs.map((dir) => dir.name).includes('src'), true)
  }
} else {
  console.log(`  SKIP real project root not found: ${realRoot}`)
}

// ---------------------------------------------------------------------------
console.log(`\n${failures === 0 ? 'PASS' : 'FAIL'}: ${checks - failures}/${checks} checks passed`)
for (const file of [utilsOut, labelsOut, loaderOut]) {
  try {
    fs.unlinkSync(file)
  } catch {
    // ignore
  }
}
process.exit(failures === 0 ? 0 : 1)
