#!/usr/bin/env node
/**
 * task-52: ObjectBrowser + global object root discovery tests.
 *
 * Covers:
 *  - `objectRoots.ts` pure helpers: address/type helpers, root discovery rules
 *    (preferred name, fixed address, children, locals/scalars/unsampleable
 *    excluded, include flags, deterministic ordering, maxRoots), stable object
 *    paths / curve keys, lazy `buildObjectTree` limits, flatten/filter/search,
 *    `findObjectNode(s)`, watch/curve batch collection, bounded node counts,
 *    virtual window math and preset JSON helpers;
 *  - `labels.ts`: English defaults complete + CJK-free, placeholder formatting,
 *    partial override resolution;
 *  - component static contract for `ObjectBrowser.vue` / `index.ts` / docs;
 *  - optional real ELF smoke (reference `firmware.elf`) when the file exists:
 *    preferred root `target_debug_handle`, 5 top modules, 324 nodes, depth 4,
 *    lazy row counts, parser array cap, batch limit and search.
 *
 * Usage:
 *   node scripts/test-object-browser.mjs
 *   OBJECT_ELF=/path/to/firmware.elf node scripts/test-object-browser.mjs
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const rootsOut = path.join(os.tmpdir(), `object-roots-test-${process.pid}.mjs`)
const labelsOut = path.join(os.tmpdir(), `object-labels-test-${process.pid}.mjs`)

execFileSync(esbuild, [
  path.join(root, 'src', 'components', 'object', 'objectRoots.ts'),
  '--bundle', '--format=esm', '--platform=node', '--alias:@=./src', `--outfile=${rootsOut}`,
], {stdio: 'pipe', cwd: root})
execFileSync(esbuild, [
  path.join(root, 'src', 'components', 'object', 'labels.ts'),
  '--bundle', '--format=esm', '--platform=node', `--outfile=${labelsOut}`,
], {stdio: 'pipe', cwd: root})

const or = await import(rootsOut)
const labels = await import(labelsOut)

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
const section = (title) => console.log(`\n${title}`)

// ---------------------------------------------------------------------------
// Fixtures (mirror the VariableNode shape produced by parseElfVariables)
// ---------------------------------------------------------------------------

const HANDLE_ADDRESS = 0x24002fa8
const SNAPSHOT_ADDRESS = 0x20002270
const ARRAY_ADDRESS = 0x08002000
const CJK_RE = /[\u4e00-\u9fff]/

function sample(address, extra = {}) {
  return {address: address >>> 0, size: 4, bitOffset: 0, bitSize: 0, kind: 'uint', signed: false, ...extra}
}

function locationAt(offset) {
  const derefRoot = {kind: 'deref', inner: {kind: 'addr', address: HANDLE_ADDRESS}, offset: 0}
  return offset === 0 ? derefRoot : {kind: 'add', inner: derefRoot, offset}
}

/** Pointer/frame-derived member (address 0 + location expression). */
function field(name, typeName, options = {}) {
  const fixed = options.address ?? 0
  const location = fixed !== 0 ? undefined : locationAt(options.offset ?? 0)
  const node = {
    name,
    typeName,
    address: fixed >>> 0,
    size: options.size ?? 4,
    sampleable: options.sampleable !== false,
    liveSamplable: options.liveSamplable !== false,
    samplingMode: options.liveSamplable === false ? 'halt' : 'live',
    sample: options.sampleable === false
      ? undefined
      : {
        address: fixed >>> 0,
        size: 4,
        bitOffset: 0,
        bitSize: 0,
        kind: 'uint',
        signed: false,
        ...(location ? {location} : {}),
      },
    ...(location ? {location} : {}),
  }
  if (options.file) {
    node.file = options.file
    node.relativeFile = options.relativeFile ?? options.file
    node.line = options.line
  }
  return node
}

/** Aggregate node (struct/array) with children; pointer-derived by default. */
function aggregate(name, typeName, children, options = {}) {
  const fixed = options.address ?? 0
  const location = fixed !== 0 ? undefined : locationAt(options.offset ?? 0)
  return {
    name,
    typeName,
    address: fixed >>> 0,
    size: options.size ?? 16,
    sampleable: options.sampleable !== false,
    liveSamplable: options.liveSamplable !== false,
    samplingMode: options.liveSamplable === false ? 'halt' : 'live',
    ...(location ? {location} : {}),
    sample: options.sampleable === false ? undefined : {
      address: fixed >>> 0,
      size: 8,
      bitOffset: 0,
      bitSize: 0,
      kind: 'raw',
      signed: false,
      ...(location ? {location} : {}),
    },
    children,
  }
}

function arrayChildren(count, address, stride = 4, namePrefix = '[') {
  return Array.from({length: count}, (_, index) => field(`${namePrefix}${index}]`, 'uint32_t', {address: address + index * stride}))
}

function fixtureHandle() {
  return {
    name: 'target_debug_handle',
    shortName: 'target_debug_handle',
    qualifiedName: 'target_debug_handle',
    typeName: 'Robot::TargetApp *',
    address: HANDLE_ADDRESS,
    size: 4,
    sampleable: true,
    liveSamplable: true,
    samplingMode: 'live',
    pointerDepth: 1,
    file: 'src/system/FreeRTOS/system.hpp',
    relativeFile: 'src/system/FreeRTOS/system.hpp',
    line: 110,
    sample: sample(HANDLE_ADDRESS),
    children: [
      aggregate('->cmd_', 'Component::CMD', [
        field('.online_', 'bool', {offset: 0}),
        field('.mode_', 'Mode', {offset: 2}),
        aggregate('.event_', 'Message::Event', [
          field('.group_', 'om_event_group_t *', {offset: 0}),
        ], {offset: 4}),
        aggregate('.data_', 'std::array<Component::CMD::Data, 4U>', [
          field('._M_elems', '_Type', {offset: 0}),
        ], {offset: 8}),
        field('.broken_', 'uint32_t', {offset: 12, sampleable: false}),
      ], {offset: 0, size: 380}),
      aggregate('->can_', 'Device::Can', [
        field('.index', 'uint32_t', {offset: 0}),
        aggregate('.data', 'uint8_t[3]', [
          field('[0]', 'uint8_t', {offset: 0}),
          field('[1]', 'uint8_t', {offset: 1}),
          field('[2]', 'uint8_t', {offset: 2}),
        ], {offset: 4}),
      ], {offset: 384}),
      aggregate('->bmi088_', 'Device::BMI088', [
        aggregate('.accl_', 'Vector3', [
          field('.x', 'float', {offset: 0}),
          field('.y', 'float', {offset: 4}),
          field('.z', 'float', {offset: 8}),
        ], {offset: 220}),
      ], {offset: 388}),
    ],
  }
}

function fixtureSnapshot() {
  return aggregate('g_bmi088_debug_snapshot', 'BmiDebugSnapshot', [
    field('.temp', 'float', {address: SNAPSHOT_ADDRESS, offset: 0}),
    aggregate('.raw_accel', 'int16_t[3]', [
      field('[0]', 'int16_t', {address: SNAPSHOT_ADDRESS + 4}),
      field('[1]', 'int16_t', {address: SNAPSHOT_ADDRESS + 6}),
      field('[2]', 'int16_t', {address: SNAPSHOT_ADDRESS + 8}),
    ], {address: SNAPSHOT_ADDRESS + 4, size: 6}),
  ], {address: SNAPSHOT_ADDRESS, size: 60})
}

function fixtureCrcArray() {
  const materialized = 64
  const children = Array.from({length: materialized}, (_, index) => field(`[${index}]`, 'uint32_t', {address: ARRAY_ADDRESS + index * 4}))
  return aggregate('om_crc32_tab', 'uint32_t[256]', children, {address: ARRAY_ADDRESS, size: 1024})
}

function fixtureVariables() {
  const handle = fixtureHandle()
  const snapshot = fixtureSnapshot()
  const crc = fixtureCrcArray()
  const local = {
    name: 'local_cmd',
    typeName: 'Component::CMD',
    address: 0,
    size: 380,
    sampleable: true,
    liveSamplable: false,
    samplingMode: 'halt',
    location: {kind: 'cfa', offset: -8},
    sample: sample(0, {kind: 'raw', location: {kind: 'cfa', offset: -8}}),
    children: [field('.mode_', 'Mode', {offset: 0})],
  }
  const frameRegister = {
    name: 'frame_reg_struct',
    typeName: 'FrameThing',
    address: 0,
    size: 16,
    sampleable: true,
    liveSamplable: false,
    samplingMode: 'halt',
    location: {kind: 'reg', reg: 7, offset: 0},
    sample: {address: 0, size: 8, bitOffset: 0, bitSize: 0, kind: 'raw', signed: false, register: 7},
    children: [field('.x', 'uint32_t', {offset: 0})],
  }
  const scalar = field('xTickCount', 'TickType_t', {address: 0x20000010})
  const pointerToScalar = field('some_int_ptr', 'int *', {address: 0x20000014})
  const unsampleableAggregate = {
    name: 'optimized_away',
    typeName: 'Gone',
    address: 0x20000020,
    size: 8,
    sampleable: false,
    children: [field('.gone', 'uint32_t', {address: 0x20000020})],
  }
  const duplicateHandle = {...fixtureHandle(), typeName: 'Robot::TargetApp *'}
  return [unsampleableAggregate, scalar, handle, local, snapshot, pointerToScalar, crc, frameRegister, duplicateHandle]
}

// ---------------------------------------------------------------------------
section('[1] Address / type / location helpers')
// ---------------------------------------------------------------------------
eq('formatObjectAddress: 32-bit hex', or.formatObjectAddress(HANDLE_ADDRESS), '0x24002FA8')
eq('formatObjectAddress: zero', or.formatObjectAddress(0), '0x00000000')
eq('isPointerTypeName: pointer', or.isPointerTypeName('Robot::TargetApp *'), true)
eq('isPointerTypeName: const pointer', or.isPointerTypeName('const Foo *'), true)
eq('isPointerTypeName: reference is not pointer', or.isPointerTypeName('Foo &'), false)
eq('isPointerTypeName: object', or.isPointerTypeName('Component::CMD'), false)
eq('arrayLengthFromTypeName: trailing dim', or.arrayLengthFromTypeName('uint32_t[256]'), 256)
eq('arrayLengthFromTypeName: last dim only', or.arrayLengthFromTypeName('float[3][4]'), 4)
eq('arrayLengthFromTypeName: template not array', or.arrayLengthFromTypeName('System::Database::Key<float[3]>'), null)
eq('isArrayTypeName: array', or.isArrayTypeName('const char[9]'), true)
eq('objectRootBaseKind: pointer', or.objectRootBaseKind(fixtureHandle()), 'pointer')
eq('objectRootBaseKind: object', or.objectRootBaseKind(fixtureSnapshot()), 'object')
eq('objectRootBaseKind: array', or.objectRootBaseKind(fixtureCrcArray()), 'array')
eq('objectTotalChildCount: parser capped array', or.objectTotalChildCount(fixtureCrcArray()), 256)
eq('objectTotalChildCount: plain struct', or.objectTotalChildCount(fixtureSnapshot()), 2)
eq('formatObjectLocation: addr', or.formatObjectLocation({kind: 'addr', address: 0x24002fa8}), '0x24002FA8')
eq('formatObjectLocation: add(deref)', or.formatObjectLocation({
  kind: 'add', inner: {kind: 'deref', inner: {kind: 'addr', address: 0x24002fa8}, offset: 0}, offset: 380,
}), '*(0x24002FA8)+380')
eq('formatObjectLocation: reg', or.formatObjectLocation({kind: 'reg', reg: 13, offset: 0}), 'r13')
eq('formatObjectLocation: reg offset', or.formatObjectLocation({kind: 'reg', reg: 13, offset: -4}), 'r13-4')
eq('formatObjectLocation: cfa', or.formatObjectLocation({kind: 'cfa', offset: -8}), 'CFA-8')
eq('objectNodeLocationText: register sample', or.objectNodeLocationText({
  name: 'x', typeName: 'uint8_t', address: 0, size: 1, sample: {address: 0, size: 1, kind: 'uint', signed: false, register: 3},
}), 'r3')

// ---------------------------------------------------------------------------
section('[2] Global object root discovery')
// ---------------------------------------------------------------------------
const variables = fixtureVariables()
const roots = or.discoverObjectRoots(variables)
eq('root count (scalars/locals/unsampleable excluded)', roots.length, 3)
eq('preferred root first', roots.map((item) => item.name), ['target_debug_handle', 'g_bmi088_debug_snapshot', 'om_crc32_tab'])
eq('preferred root kind/baseKind', [roots[0].kind, roots[0].baseKind], ['preferred', 'pointer'])
eq('preferred root fields', {
  key: roots[0].key,
  name: roots[0].name,
  typeName: roots[0].typeName,
  address: roots[0].address,
  depth: roots[0].depth,
  childCount: roots[0].childCount,
}, {
  key: `target_debug_handle@${HANDLE_ADDRESS.toString(16)}`,
  name: 'target_debug_handle',
  typeName: 'Robot::TargetApp *',
  address: HANDLE_ADDRESS,
  depth: 0,
  childCount: 3,
})
eq('object root kind', roots[1].kind, 'object')
eq('array root kind', roots[2].kind, 'array')
eq('no local-variable roots', roots.some((item) => item.name === 'local_cmd' || item.name === 'frame_reg_struct'), false)
eq('no scalar roots', roots.some((item) => item.name === 'xTickCount' || item.name === 'some_int_ptr'), false)
eq('no unsampleable roots', roots.some((item) => item.name === 'optimized_away'), false)
check('all roots are fixed address + sampleable + have children', roots.every((item) =>
  item.address !== 0 && item.sampleable && item.childCount > 0 && item.node.children.length > 0))
eq('default root = preferred', or.defaultObjectRoot(roots)?.name, 'target_debug_handle')
eq('find root by key', or.findObjectRoot(roots, roots[0].key)?.name, 'target_debug_handle')
eq('find root by full name', or.findObjectRoot(roots, 'target_debug_handle')?.name, 'target_debug_handle')
eq('find missing root', or.findObjectRoot(roots, 'nope@0'), null)
eq('matchesPreferredRootName: last scope', or.matchesPreferredRootName({
  name: 'Robot::TargetApp::target_debug_handle',
  shortName: 'target_debug_handle',
  typeName: 'Robot *',
  address: 1,
  size: 4,
}), true)
eq('rootFromNode', (() => {
  const item = or.objectRootFromNode(fixtureHandle())
  return item ? [item.kind, item.baseKind, item.childCount, item.liveSamplable] : null
})(), ['preferred', 'pointer', 3, true])
eq('rootFromNode rejects scalar', or.objectRootFromNode(field('xTickCount', 'TickType_t', {address: 0x20000010})), null)

eq('preferredNames override: no preferred list -> base kind order', or.discoverObjectRoots(variables, {preferredNames: []})
  .map((item) => `${item.name}:${item.kind}`), ['target_debug_handle:pointer', 'g_bmi088_debug_snapshot:object', 'om_crc32_tab:array'])
eq('includeArrays=false', or.discoverObjectRoots(variables, {includeArrays: false}).map((item) => item.name), ['target_debug_handle', 'g_bmi088_debug_snapshot'])
eq('includeObjects=false', or.discoverObjectRoots(variables, {includeObjects: false}).map((item) => item.name), ['target_debug_handle', 'om_crc32_tab'])
eq('includePointers=false (preferred pointer excluded too)', or.discoverObjectRoots(variables, {includePointers: false}).map((item) => item.name), ['g_bmi088_debug_snapshot', 'om_crc32_tab'])
eq('requireFixedAddress=false includes locals', or.discoverObjectRoots(variables, {requireFixedAddress: false}).some((item) => item.name === 'local_cmd'), true)
eq('minChildCount=3', or.discoverObjectRoots(variables, {minChildCount: 3}).map((item) => item.name), ['target_debug_handle', 'om_crc32_tab'])
eq('maxRoots=1 keeps preferred', or.discoverObjectRoots(variables, {maxRoots: 1}).map((item) => item.name), ['target_debug_handle'])
eq('maxRoots=2', or.discoverObjectRoots(variables, {maxRoots: 2}).map((item) => item.name), ['target_debug_handle', 'g_bmi088_debug_snapshot'])
eq('duplicate root deduped by key', roots.filter((item) => item.name === 'target_debug_handle').length, 1)
eq('discoverObjectRoots(null)', or.discoverObjectRoots(null), [])
eq('defaultObjectRoot([])', or.defaultObjectRoot([]), null)

// ---------------------------------------------------------------------------
section('[3] Stable object paths and curve keys')
// ---------------------------------------------------------------------------
const [handleRoot, snapshotRoot, crcRoot] = roots
const rootPath = or.objectPath(handleRoot.node)
const cmd = handleRoot.node.children[0]
const mode = cmd.children[1]
const canData = handleRoot.node.children[1].children[1]
const bmi = handleRoot.node.children[2]
const accl = bmi.children[0]
eq('objectPath(root)', rootPath, 'target_debug_handle')
eq('child key: pointer member', or.objectChildKey(rootPath, cmd), 'target_debug_handle->cmd_')
eq('nested path matches docs example', or.objectPath(mode, or.objectChildKey(rootPath, cmd)), 'target_debug_handle->cmd_.mode_')
eq('objectPath(node, parent)', or.objectPath(mode, 'target_debug_handle->cmd_'), 'target_debug_handle->cmd_.mode_')
eq('array child key', or.objectChildKey('target_debug_handle->can_.data', canData.children[1]), 'target_debug_handle->can_.data[1]')
eq('bare member gets a dot', or.objectChildKey('a', {name: 'member', typeName: 'int', address: 1, size: 4}), 'a.member')
eq('empty parent', or.objectChildKey('', {name: '->x', typeName: 'int', address: 1, size: 4}), '->x')
eq('objectPath(root node) only name', or.objectPath(handleRoot.node), 'target_debug_handle')
eq('curve key matches DebugCurve nodeKey', or.objectCurveKey('target_debug_handle->cmd_.mode_', mode), `target_debug_handle->cmd_.mode_@${mode.address.toString(16)}`)
eq('curve key from node', or.objectCurveKey(handleRoot.node), `target_debug_handle@${HANDLE_ADDRESS.toString(16)}`)
eq('path from curve key', or.objectPathFromCurveKey('a.b[2]@1ff'), 'a.b[2]')
eq('path from curve key without suffix', or.objectPathFromCurveKey('a.b[2]'), 'a.b[2]')
eq('ancestors: fields', or.objectPathAncestors('target_debug_handle->cmd_.mode_'), ['target_debug_handle', 'target_debug_handle->cmd_'])
eq('ancestors: arrays', or.objectPathAncestors('a.b[0][1]'), ['a', 'a.b', 'a.b[0]'])
eq('ancestors: root', or.objectPathAncestors('target_debug_handle'), [])
eq('objectNodeRef fields', (() => {
  const ref = or.objectNodeRef(handleRoot, mode, 'target_debug_handle->cmd_.mode_', 2)
  return {
    key: ref.key,
    curveKey: ref.curveKey,
    path: ref.path,
    name: ref.name,
    rootKey: ref.rootKey,
    depth: ref.depth,
    sampleable: ref.sampleable,
    liveSamplable: ref.liveSamplable,
    fixedAddress: ref.fixedAddress,
    nodeIdentity: ref.node === mode,
  }
})(), {
  key: 'target_debug_handle->cmd_.mode_',
  curveKey: `target_debug_handle->cmd_.mode_@${mode.address.toString(16)}`,
  path: 'target_debug_handle->cmd_.mode_',
  name: 'mode_',
  rootKey: handleRoot.key,
  depth: 2,
  sampleable: true,
  liveSamplable: true,
  fixedAddress: false,
  nodeIdentity: true,
})

// ---------------------------------------------------------------------------
section('[4] Lazy buildObjectTree')
// ---------------------------------------------------------------------------
const collapsed = or.buildObjectTree(handleRoot, {})
const collapsedRows = or.flattenObjectTree(collapsed)
eq('collapsed: root expanded by default', collapsed.expanded, true)
eq('collapsed: root shows top modules', collapsed.children.map((item) => item.displayName), ['cmd_', 'can_', 'bmi088_'])
eq('collapsed: child branch not materialised', collapsed.children[0].children, [])
eq('collapsed: child is expandable', collapsed.children[0].expandable, true)
eq('collapsed: row count', collapsedRows.map((row) => row.path), [
  'target_debug_handle',
  'target_debug_handle->cmd_',
  'target_debug_handle->can_',
  'target_debug_handle->bmi088_',
])
const cmdPath = 'target_debug_handle->cmd_'
const expandedTree = or.buildObjectTree(handleRoot, {expanded: new Set([rootPath, cmdPath])})
const cmdTree = expandedTree.children[0]
eq('expanded: cmd children', cmdTree.children.map((item) => item.displayName), ['online_', 'mode_', 'event_', 'data_', 'broken_'])
eq('expanded: nested row path', or.flattenObjectTree(expandedTree).map((row) => row.path).includes('target_debug_handle->cmd_.mode_'), true)
eq('expanded: commands children remain collapsed', cmdTree.children[2].children, [])
eq('curveKey on tree node', expandedTree.children[0].children[1].curveKey, `target_debug_handle->cmd_.mode_@0`)
eq('expandRoot=false', or.buildObjectTree(handleRoot, {expandRoot: false}).children, [])

const cappedRoot = or.buildObjectTree(handleRoot, {maxChildren: 2})
eq('maxChildren: root slice', cappedRoot.children.length, 2)
eq('maxChildren: truncated flag', [cappedRoot.truncated, cappedRoot.hasMoreChildren], [true, true])
eq('maxChildren: total child count', cappedRoot.totalChildCount, 3)
const budgetTree = or.buildObjectTree(handleRoot, {maxNodes: 2})
eq('maxNodes budget: root+1 child', or.flattenObjectTree(budgetTree).length, 2)
eq('maxNodes budget: truncated', budgetTree.truncated, true)
const shallowTree = or.buildObjectTree(handleRoot, {expanded: new Set([rootPath, cmdPath]), maxDepth: 1})
eq('maxDepth=1: children materialised', shallowTree.children.length, 3)
eq('maxDepth=1: grandchild depth-limited', [shallowTree.children[0].depthLimited, shallowTree.children[0].children.length], [true, 0])
const limitedTree = or.buildObjectTree(handleRoot, {
  expanded: new Set([rootPath, cmdPath]),
  childLimit: (pathValue) => pathValue === cmdPath ? 2 : undefined,
})
eq('childLimit function', limitedTree.children[0].children.map((item) => item.displayName), ['online_', 'mode_'])
eq('childLimit truncated', limitedTree.children[0].truncated, true)

const arrayTree = or.buildObjectTree(crcRoot, {})
eq('array root children materialised', arrayTree.children.length, 64)
eq('array logical total', [arrayTree.totalChildCount, arrayTree.parserCapped], [256, true])
eq('array cap badge counts', arrayTree.childCount, 64)
eq('collapsed tree rows < full node count', collapsedRows.length < or.countObjectNodes(handleRoot.node).count, true)
eq('buildObjectTree(null)', or.buildObjectTree(null), null)
eq('buildObjectTree from bare VariableNode', or.buildObjectTree(handleRoot.node, {}).path, 'target_debug_handle')
const derivedChild = expandedTree.children[0].children[1]
eq('derived node: fixedAddress/pointer/arrayLike', [derivedChild.fixedAddress, derivedChild.pointer, derivedChild.arrayLike], [false, false, false])
eq('derived node: location text', derivedChild.locationText.startsWith('*('), true)

// ---------------------------------------------------------------------------
section('[5] Filtering')
// ---------------------------------------------------------------------------
const fullTree = or.buildObjectTree(handleRoot, {
  expanded: new Set([rootPath, cmdPath, `${cmdPath}.event_`, `${cmdPath}.data_`]),
})
const sampleableFiltered = or.filterObjectTree(fullTree, {sampleableOnly: true})
const sampleablePaths = or.flattenObjectTree(sampleableFiltered).map((row) => row.path)
check('filter sampleableOnly removes unsampleable leaf', !sampleablePaths.includes(`${cmdPath}.broken_`), sampleablePaths.join(','))
check('filter sampleableOnly keeps ancestors', sampleablePaths.includes(`${cmdPath}.mode_`), sampleablePaths.join(','))
eq('filter fixedAddressOnly keeps fixed nodes only', or.flattenObjectTree(or.filterObjectTree(fullTree, {fixedAddressOnly: true})).map((row) => row.path), [rootPath])
eq('filter fixedAddressOnly: root matched flag', or.filterObjectTree(fullTree, {fixedAddressOnly: true}).matched, true)
const modePath = `${cmdPath}.mode_`
const valueFiltered = or.filterObjectTree(fullTree, {withValueOnly: true, hasValue: (node) => node.path === modePath})
eq('filter withValueOnly keeps ancestors', or.flattenObjectTree(valueFiltered).map((row) => row.path), [rootPath, cmdPath, modePath])
eq('filter withValueOnly: ancestor matched flags', or.flattenObjectTree(valueFiltered).map((row) => !!row.matched), [false, false, true])
eq('filter: nothing matches -> null', or.filterObjectTree(fullTree, {withValueOnly: true, hasValue: () => false}), null)
eq('filter: empty options keeps all', or.flattenObjectTree(or.filterObjectTree(fullTree, {})).length, or.flattenObjectTree(fullTree).length)
eq('filter: null tree', or.filterObjectTree(null, {sampleableOnly: true}), null)
eq('node matches filter helper', or.objectTreeNodeMatchesFilter(fullTree, {}), true)

// ---------------------------------------------------------------------------
section('[6] Search')
// ---------------------------------------------------------------------------
const searchMode = or.searchObjectTree(handleRoot, 'mode_')
eq('search name match', searchMode.map((match) => match.node.path), [modePath])
eq('search ancestors', searchMode[0].ancestors, [rootPath, cmdPath])
eq('search case-insensitive type match', or.searchObjectTree(handleRoot, 'component::cmd').map((match) => match.node.path), [cmdPath, `${cmdPath}.data_`])
eq('search address match', or.searchObjectTree(handleRoot, '24002fa8').some((match) => match.node.path === rootPath), true)
eq('search path fragment', or.searchObjectTree(handleRoot, 'bmi088_.accl_.y').map((match) => match.node.path), ['target_debug_handle->bmi088_.accl_.y'])
eq('search empty query', or.searchObjectTree(handleRoot, '   '), [])
eq('search no results', or.searchObjectTree(handleRoot, 'zzz-nope'), [])
eq('search maxMatches', or.searchObjectTree(handleRoot, 'target_debug_handle', {maxMatches: 1}).length <= 1, true)
eq('search maxNodes stops', or.searchObjectTree(handleRoot, 'accl_', {maxNodes: 1}), [])
eq('search filter predicate', or.searchObjectTree(handleRoot, 'broken_', {
  filter: (node) => node.sampleable,
}), [])
eq('search filter predicate allows sampleable', or.searchObjectTree(handleRoot, 'broken_', {
  filter: (node) => !node.sampleable,
}).map((match) => match.node.path), [`${cmdPath}.broken_`])
eq('search from tree node subtree', or.searchObjectTree(expandedTree.children[0], 'mode_').map((match) => match.node.path), [modePath])
eq('search null input', or.searchObjectTree(null, 'a'), [])

const found = or.findObjectNode(handleRoot, modePath)
eq('findObjectNode by path', found ? [found.node.path, found.ancestors] : null, [modePath, [rootPath, cmdPath]])
eq('findObjectNode by curve key', or.findObjectNode(handleRoot, `${modePath}@0`)?.node.path, modePath)
eq('findObjectNode missing', or.findObjectNode(handleRoot, 'target_debug_handle->nope'), null)
eq('findObjectNode bounded', or.findObjectNode(handleRoot, 'target_debug_handle->bmi088_.accl_.z', {maxNodes: 2}), null)
eq('findObjectNodes batch', or.findObjectNodes(handleRoot, [modePath, 'target_debug_handle->can_.data[1]', 'missing'], {})
  .map((match) => match.node.path), [modePath, 'target_debug_handle->can_.data[1]'])
eq('findObjectNodes empty', or.findObjectNodes(handleRoot, []), [])

// ---------------------------------------------------------------------------
section('[7] Watch / curve batches')
// ---------------------------------------------------------------------------
eq('isObjectNodeCurveable: fixed', or.isObjectNodeCurveable(fixtureSnapshot()), true)
eq('isObjectNodeCurveable: derived live', or.isObjectNodeCurveable(fixtureHandle().children[2]), true)
eq('isObjectNodeCurveable: halt frame leaf', or.isObjectNodeCurveable(field('.x', 'uint32_t', {offset: 4, liveSamplable: false})), true)
eq('isObjectNodeCurveable: unsampleable', or.isObjectNodeCurveable({
  name: 'gone', typeName: 'X', address: 0x20000000, size: 4, sampleable: false,
}), false)
const visibleRows = or.flattenObjectTree(expandedTree).filter((row) => row.depth <= 1)
const visibleBatch = or.collectObjectNodes(handleRoot, visibleRows, 'visible', {maxBatch: 2})
eq('visible batch: mode/cap', [visibleBatch.mode, visibleBatch.nodes.length, visibleBatch.truncated], ['visible', 2, true])
eq('visible batch: total eligible', visibleBatch.total, 4)
eq('visible batch: dedupe stable', visibleBatch.nodes.map((ref) => ref.key), [rootPath, cmdPath])
const unsampleableRow = or.findObjectNode(handleRoot, `${cmdPath}.broken_`)?.node
check('visible batch: unsampleable not candidate', !!unsampleableRow && !visibleBatch.nodes.some((ref) => ref.key === unsampleableRow.path))
const singleBatch = or.collectObjectNodes(handleRoot, [visibleRows[0]], 'active', {})
eq('active batch: single node', [singleBatch.nodes.length, singleBatch.truncated, singleBatch.nodes[0].curveKey],
  [1, false, `target_debug_handle@${HANDLE_ADDRESS.toString(16)}`])
const expectedSubtreeCount = (() => {
  let count = 0
  const walk = (node) => {
    if (or.isObjectNodeCurveable(node)) count += 1
    for (const child of node.children ?? []) walk(child)
  }
  walk(handleRoot.node)
  return count
})()
const subtreeBatch = or.collectObjectSubtree(handleRoot, collapsed, {maxBatch: 512})
eq('subtree batch: all curveable nodes', subtreeBatch.nodes.length, expectedSubtreeCount)
eq('subtree batch: mode/not truncated', [subtreeBatch.mode, subtreeBatch.truncated], ['subtree', false])
const cappedSubtree = or.collectObjectSubtree(handleRoot, collapsed, {maxBatch: 2})
eq('subtree batch: cap', [cappedSubtree.nodes.length, cappedSubtree.truncated, cappedSubtree.total > 2], [2, true, true])
const brokenRef = or.objectTreeNodeRef(handleRoot, unsampleableRow)
check('batch refs carry node identity + flags', brokenRef.node === unsampleableRow.node && brokenRef.sampleable === false
  && brokenRef.rootKey === handleRoot.key && typeof brokenRef.curveKey === 'string')
eq('collectObjectNodes(null root)', or.collectObjectNodes(null, [or.flattenObjectTree(collapsed)[0]], 'selected', {}).nodes.length, 1)

// ---------------------------------------------------------------------------
section('[8] Bounded node counting + virtual window')
// ---------------------------------------------------------------------------
const handleCount = (() => {
  let count = 0
  const walk = (node) => {
    count += 1
    for (const child of node.children ?? []) walk(child)
  }
  walk(handleRoot.node)
  return count
})()
eq('countObjectNodes: exact', or.countObjectNodes(handleRoot.node), {count: handleCount, truncated: false})
eq('countObjectNodes: bounded', or.countObjectNodes(handleRoot.node, 2), {count: 2, truncated: true})
eq('countObjectNodes: null', or.countObjectNodes(null), {count: 0, truncated: false})
eq('virtual window: empty', or.computeObjectVirtualWindow({scrollTop: 0, viewportHeight: 240, rowCount: 0}), {
  start: 0, end: 0, offsetY: 0, totalHeight: 0, overscan: or.OBJECT_OVERSCAN,
})
eq('virtual window: first page', or.computeObjectVirtualWindow({
  scrollTop: 0, viewportHeight: 240, rowHeight: 24, rowCount: 1000, overscan: 4,
}), {start: 0, end: 18, offsetY: 0, totalHeight: 24000, overscan: 4})
eq('virtual window: scrolled clamp', or.computeObjectVirtualWindow({
  scrollTop: 100000, viewportHeight: 240, rowHeight: 24, rowCount: 100, overscan: 0,
}).end, 100)
eq('virtual window: negative scroll clamps', or.computeObjectVirtualWindow({
  scrollTop: -50, viewportHeight: 48, rowHeight: 24, rowCount: 10, overscan: 1,
}).start, 0)
eq('virtual window: end clamps to rowCount', or.computeObjectVirtualWindow({
  scrollTop: 0, viewportHeight: 24 * 1000, rowHeight: 24, rowCount: 5, overscan: 3,
}).end, 5)

// ---------------------------------------------------------------------------
section('[9] Preset JSON helpers')
// ---------------------------------------------------------------------------
const created = or.createObjectPreset({
  name: 'BMI watch',
  rootKey: `target_debug_handle@${HANDLE_ADDRESS.toString(16)}`,
  keys: [modePath, `${modePath}@0`, modePath, 'target_debug_handle->bmi088_.accl_.x'],
}, 1700000000000)
check('createObjectPreset: valid', !!created && created.keys.length === 2 && created.createdAt === 1700000000000)
eq('createObjectPreset: canonical keys', created.keys, [modePath, 'target_debug_handle->bmi088_.accl_.x'])
eq('createObjectPreset: rootPath derived', created.rootPath, 'target_debug_handle')
eq('createObjectPreset: empty keys -> null', or.createObjectPreset({name: 'x', rootKey: 'a', keys: []}), null)
eq('createObjectPreset: default name', or.createObjectPreset({name: '  ', rootKey: 'a', keys: [modePath]}, 1).name, 'Object preset 1')
const presetJson = or.objectPresetToJson(created)
const parsed = or.parseObjectPresetJson(presetJson)
eq('preset JSON roundtrip', parsed, created)
eq('preset JSON invalid', or.parseObjectPresetJson('{oops'), null)
eq('preset JSON empty', or.parseObjectPresetJson('   '), null)
eq('preset JSON oversized', or.parseObjectPresetJson('x'.repeat(or.MAX_OBJECT_PRESET_JSON_LENGTH + 1)), null)
const legacyPreset = or.sanitizeObjectPreset({
  id: 'obj-preset-1',
  name: 'Legacy',
  rootPath: 'target_debug_handle',
  fields: [modePath, `${modePath}@0`, 42],
  createdAt: 123,
})
eq('sanitize legacy {rootPath, fields}', legacyPreset, {
  id: 'obj-preset-1',
  name: 'Legacy',
  rootKey: 'target_debug_handle',
  rootPath: 'target_debug_handle',
  keys: [modePath],
  createdAt: 123,
})
eq('sanitize invalid: null', or.sanitizeObjectPreset(null), null)
eq('sanitize invalid: missing id', or.sanitizeObjectPreset({name: 'x', fields: []}), null)
eq('sanitize: keys not array tolerated', or.sanitizeObjectPreset({id: 'p', name: 'n', rootKey: 'r', keys: 'nope'}).keys, [])
eq('normalizeObjectPreset legacy', or.normalizeObjectPreset({id: 'p2', name: 'n2', rootPath: 'target_debug_handle', fields: [modePath]})?.keys, [modePath])
eq('preset JSON null input', or.objectPresetToJson(null), '')

// ---------------------------------------------------------------------------
section('[10] Labels')
// ---------------------------------------------------------------------------
const defaults = labels.DEFAULT_OBJECT_BROWSER_LABELS
const defaultKeys = Object.keys(defaults)
check(`labels: complete default map (${defaultKeys.length} fields)`, defaultKeys.length >= 60)
check('labels: every default is a non-empty string', defaultKeys.every((key) => typeof defaults[key] === 'string' && defaults[key].trim().length > 0))
check('labels: no CJK in default English values', defaultKeys.every((key) => !CJK_RE.test(defaults[key])))
const requiredLabels = [
  'title', 'rootsLabel', 'rootSearchPlaceholder', 'rootMenuEmpty', 'rootCount', 'noRootSelected',
  'preferredBadge', 'pointerBadge', 'objectBadge', 'arrayBadge',
  'searchPlaceholder', 'sampleableOnly', 'fixedAddressOnly', 'withValueOnly',
  'expandAll', 'collapseAll', 'expandField', 'collapseField',
  'watchSubtree', 'watchVisible', 'watchSelected', 'addCurves', 'addSelected', 'selectedCount',
  'presetsLabel', 'presetSelect', 'presetNamePlaceholder', 'presetSave', 'presetLoad', 'presetDelete',
  'colName', 'colType', 'colAddress', 'colValue', 'sourceJump', 'noSourceLocation',
  'watchAdd', 'watchRemove', 'noValue', 'staleValue', 'liveBadge', 'haltBadge', 'derivedAddress',
  'noVariablesTitle', 'noVariablesBody', 'noRootsTitle', 'noRootsBody', 'noRootChildren',
  'invalidRoot', 'invalidRootFallback', 'largeRoot', 'arrayCapped', 'treeLimited', 'showMoreChildren',
  'noRowsFiltered', 'noSearchResults', 'searchResultsCount', 'multiSelectHint', 'batchEmpty',
  'batchTruncated', 'visibleRows', 'rowsLimited', 'treeAriaLabel', 'rootAriaLabel', 'revealInTree',
]
check('labels: component-required fields present', requiredLabels.every((key) => typeof defaults[key] === 'string'))
eq('formatObjectBrowserLabel: replace', labels.formatObjectBrowserLabel('{count} roots', {count: 5}), '5 roots')
eq('formatObjectBrowserLabel: repeat', labels.formatObjectBrowserLabel('{a}-{a}', {a: 'x'}), 'x-x')
eq('formatObjectBrowserLabel: unknown kept', labels.formatObjectBrowserLabel('{missing}', {}), '{missing}')
eq('formatObjectBrowserLabel: numeric zero', labels.formatObjectBrowserLabel('{count}', {count: 0}), '0')
const resolved = labels.resolveObjectBrowserLabels({title: 'Objekte', noValue: 'n/a', colType: ''})
eq('resolve labels: override', [resolved.title, resolved.colType, resolved.noValue], ['Objekte', defaults.colType, 'n/a'])
check('resolve labels: defaults not mutated', defaults.title === 'Objects')
eq('resolve labels: null returns defaults', labels.resolveObjectBrowserLabels(null).title, defaults.title)

// ---------------------------------------------------------------------------
section('[11] ObjectBrowser component static contract')
// ---------------------------------------------------------------------------
const componentDir = path.join(root, 'src', 'components', 'object')
const browserSource = fs.readFileSync(path.join(componentDir, 'ObjectBrowser.vue'), 'utf8')
const rootsSource = fs.readFileSync(path.join(componentDir, 'objectRoots.ts'), 'utf8')
const labelsSource = fs.readFileSync(path.join(componentDir, 'labels.ts'), 'utf8')
const indexSource = fs.readFileSync(path.join(componentDir, 'index.ts'), 'utf8')
const docsSource = fs.readFileSync(path.join(root, '..', 'docs', 'object-watch.md'), 'utf8')

check('component: props roots/variables/rootKey/rootNode', /roots\?: readonly ObjectRoot\[\]/.test(browserSource)
  && /variables\?: readonly VariableNode\[\]/.test(browserSource)
  && /rootKey\?: string \| null/.test(browserSource)
  && /rootNode\?: VariableNode \| null/.test(browserSource))
check('component: props selectedKeys/watchKeys/liveValues/labels/presets', /selectedKeys\?: readonly string\[\]/.test(browserSource)
  && /watchKeys\?: readonly string\[\]/.test(browserSource)
  && /liveValues\?: ObjectLiveValueMap \| null/.test(browserSource)
  && /labels\?: Partial<ObjectBrowserLabels>/.test(browserSource)
  && /presets\?: readonly ObjectPresetLike\[\]/.test(browserSource))
check('component: lazy limits props', ['maxDepth?', 'maxChildren?', 'maxNodes?', 'pageSize?', 'maxBatch?'].every((name) => browserSource.includes(name)))
check('component: emits select-variable/toggle-watch', /\(event: 'select-variable', payload: ObjectNodeRef\): void/.test(browserSource)
  && /\(event: 'toggle-watch', payload: ObjectToggleWatchPayload\): void/.test(browserSource))
check('component: emits batch watch/add-curves', /\(event: 'watch-nodes', payload: ObjectBatchPayload\): void/.test(browserSource)
  && /\(event: 'add-curves', payload: ObjectBatchPayload\): void/.test(browserSource))
check('component: emits jump-source/root-change', /\(event: 'jump-source', payload: ObjectSourceJumpPayload\): void/.test(browserSource)
  && /\(event: 'root-change', key: string, root: ObjectRoot\): void/.test(browserSource))
check('component: emits save/load/delete-preset', /\(event: 'save-preset', payload: ObjectPresetSavePayload\): void/.test(browserSource)
  && /\(event: 'load-preset', preset: ObjectPreset\): void/.test(browserSource)
  && /\(event: 'delete-preset', id: string\): void/.test(browserSource))
check('component: lazy tree uses buildObjectTree + expandedKeys', /buildObjectTree\(root, \{/.test(browserSource)
  && /expandedKeys\.value = next/.test(browserSource)
  && /expanded: expandedKeys\.value/.test(browserSource))
check('component: virtual rows only render the visible window', /viewportRef/.test(browserSource)
  && /ob-canvas/.test(browserSource)
  && /computeObjectVirtualWindow\(/.test(browserSource)
  && /renderedRows/.test(browserSource)
  && /OBJECT_ROW_HEIGHT/.test(browserSource))
check('component: search over raw tree + filters', /searchObjectTree\(root, debouncedQuery\.value/.test(browserSource)
  && /sampleableOnly/.test(browserSource)
  && /fixedAddressOnly/.test(browserSource)
  && /withValueOnly/.test(browserSource))
check('component: watch subtree/visible/selected + add curve/selected', ['onWatchSubtree', 'onWatchVisible', 'onWatchSelected', 'onAddCurve', 'onAddSelected']
  .every((name) => browserSource.includes(`function ${name}(`)))
check('component: row star watch + source jump button + contextmenu', /class="ob-star"/.test(browserSource)
  && /onToggleWatch/.test(browserSource)
  && /@contextmenu\.prevent="onRowContextMenu/.test(browserSource)
  && /onJumpSource/.test(browserSource))
check('component: live value + stale rendering', /liveValueFor\(/.test(browserSource)
  && /stale/.test(browserSource)
  && /value\.color/.test(browserSource))
check('component: empty/invalid/large/parser-cap states', ['noVariablesTitle', 'noRootsTitle', 'invalidRootNotice', 'largeRootNotice',
  'rootHasNoChildren', 'noSearchResults', 'noRowsFiltered', 'rowsLimited', 'parserCapped'].every((needle) => browserSource.includes(needle)))
check('component: presets controls wired', ['onSavePreset', 'onLoadPreset', 'onDeletePreset', 'applyPresetLocally'].every((name) => browserSource.includes(name)))
check('component: expose parent integration API', ['selectRootByKey', 'scrollToPath', 'revealPath', 'getVisibleNodeRefs', 'getMarkedNodeRefs', 'exportPreset']
  .every((name) => browserSource.includes(name)))
check('component: design tokens only (>= 20 --bb- vars, scoped style)', (browserSource.match(/var\(--bb-/g) ?? []).length >= 20
  && /<style scoped>/.test(browserSource))
check('component: no Element Plus / i18n imports',
  !/from ['"]element-plus/.test(browserSource)
  && !/from ['"]vue-i18n/.test(browserSource)
  && !/useI18n\(/.test(browserSource)
  && !/\btranslate\(/.test(browserSource))
check('sources: CJK-free (i18n strict contract)', [browserSource, rootsSource, labelsSource, indexSource].every((text) => !CJK_RE.test(text)))
check('objectRoots: exports required pure API', ['discoverObjectRoots', 'objectRootFromNode', 'objectPath', 'objectChildKey', 'objectCurveKey',
  'buildObjectTree', 'flattenObjectTree', 'filterObjectTree', 'searchObjectTree', 'findObjectNode', 'collectObjectNodes',
  'countObjectNodes', 'computeObjectVirtualWindow', 'sanitizeObjectPreset', 'parseObjectPresetJson'].every((name) => rootsSource.includes(`export function ${name}(`) || rootsSource.includes(`export const ${name}`)))
check('index.ts: exports component + labels + roots API', /export \{default as ObjectBrowser\} from '\.\/ObjectBrowser\.vue'/.test(indexSource)
  && /DEFAULT_OBJECT_BROWSER_LABELS/.test(indexSource)
  && /discoverObjectRoots/.test(indexSource)
  && /type ObjectBrowserLabels/.test(indexSource))
check('docs: object-watch.md covers API + integration', ['ObjectBrowser', 'discoverObjectRoots', 'objectCurveKey', 'watch-nodes',
  'add-curves', 'save-preset', 'DebugCurve', 'target_debug_handle', 'liveValues', 'labels'].every((needle) => docsSource.includes(needle)))

// ---------------------------------------------------------------------------
section('[12] Optional real ELF smoke (reference firmware.elf)')
// ---------------------------------------------------------------------------
const elfPath = process.argv[2] ?? process.env.OBJECT_ELF ?? '$HOME/reference firmware/firmware/reference-board/reference/firmware.elf'
if (!fs.existsSync(elfPath)) {
  console.log(`  skip real ELF smoke: ${elfPath} not found (set OBJECT_ELF or pass a path)`)
} else {
  const parserOut = path.join(os.tmpdir(), `object-elf-parser-${process.pid}.mjs`)
  execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'elfSymbols.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${parserOut}`,
  ], {stdio: 'pipe'})
  const {parseElfVariables} = await import(parserOut)
  const parsed = parseElfVariables(fs.readFileSync(elfPath))
  check('ELF: DWARF variables parsed', parsed.source === 'dwarf' && parsed.variables.length > 3000, `${parsed.source}/${parsed.variables.length}`)
  const realRoots = or.discoverObjectRoots(parsed.variables)
  const realDefault = or.defaultObjectRoot(realRoots)
  check('ELF: preferred root is target_debug_handle', !!realDefault && realDefault.name === 'target_debug_handle', realDefault?.name)
  if (realDefault) {
    check('ELF: root is fixed-address pointer', /Robot::TargetApp \*/.test(realDefault.typeName)
      && realDefault.address !== 0 && realDefault.baseKind === 'pointer' && realDefault.liveSamplable,
    `${realDefault.typeName}@${realDefault.address.toString(16)}`)
    eq('ELF: 5 top-level modules', realDefault.childCount, 5)
    eq('ELF: 324 nodes in subtree', or.countObjectNodes(realDefault.node, 5000).count, 324)
    const depthOf = (node) => 1 + Math.max(0, ...(node.children ?? []).map(depthOf))
    eq('ELF: subtree depth (root + 4 levels)', depthOf(realDefault.node), 5)
    const realPath = or.objectPath(realDefault.node)
    eq('ELF: root path is the variable name', realPath, 'target_debug_handle')
    const lazyRows = or.flattenObjectTree(or.buildObjectTree(realDefault, {expanded: new Set([realPath])}))
    check('ELF: lazy tree only materialises root+top modules', lazyRows.length === 6, String(lazyRows.length))
    const modeNode = or.findObjectNode(realDefault, 'target_debug_handle->cmd_.mode_')
    check('ELF: documented path target_debug_handle->cmd_.mode_ exists', !!modeNode, modeNode?.node.path)
    const bmiPath = or.searchObjectTree(realDefault, 'bmi088_', {maxMatches: 10}).map((match) => match.node.path)
    check('ELF: search finds bmi088_ path', bmiPath.some((item) => item.includes('->bmi088_')), bmiPath.slice(0, 3).join(','))
    const realBatch = or.collectObjectSubtree(realDefault, or.buildObjectTree(realDefault, {}), {maxBatch: 512})
    check('ELF: watch subtree batch collects the object (bounded)', realBatch.nodes.length > 100 && !realBatch.truncated,
      `${realBatch.nodes.length}/${realBatch.total}`)
    const cappedBatch = or.collectObjectSubtree(realDefault, or.buildObjectTree(realDefault, {}), {maxBatch: 32})
    check('ELF: batch cap marks truncation', cappedBatch.nodes.length === 32 && cappedBatch.truncated && cappedBatch.total > 32,
      `${cappedBatch.nodes.length}/${cappedBatch.total}`)
    const parserCappedArray = parsed.variables.find((item) => (item.children?.length ?? 0) === 64
      && or.objectTotalChildCount(item) > 64)
    check('ELF: parser array cap detected on real arrays', !!parserCappedArray,
      parserCappedArray ? `${parserCappedArray.name} ${parserCappedArray.children.length}/${or.objectTotalChildCount(parserCappedArray)}` : 'none')
    check('ELF: all discovered roots are fixed-address with children', realRoots.every((item) => item.address !== 0 && item.childCount > 0))
    check('ELF: roots exclude local variables (address 0 roots)', realRoots.every((item) => item.address !== 0), realRoots.slice(0, 3).map((item) => item.name).join(','))
    check('ELF: root candidate count sane (10..2000)', realRoots.length >= 10 && realRoots.length <= 2000, String(realRoots.length))
    const realMode = or.findObjectNode(realDefault, 'target_debug_handle->cmd_.mode_')
    const ref = realMode ? or.objectTreeNodeRef(realDefault, realMode.node) : null
    check('ELF: node ref is directly usable as a curve key', !!ref
      && ref.curveKey === `${ref.key}@${(ref.node.address >>> 0).toString(16)}`,
      ref?.curveKey)
  }
}

// ---------------------------------------------------------------------------
console.log(`\n${checks - failures}/${checks} checks passed (${failures} failed)`)
process.exit(failures === 0 ? 0 : 1)
