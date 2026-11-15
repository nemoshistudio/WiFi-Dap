/**
 * ObjectBrowser pure helpers: global object root discovery, stable object paths,
 * lazy object-tree building, filtering / search, watch+curve batch collection,
 * preset JSON helpers and the virtual-row window math.
 *
 * The component (`ObjectBrowser.vue`) is a thin view over this module; the
 * functions here are DOM-free and Node-testable (`web/scripts/test-object-browser.mjs`).
 *
 * Inputs are the `VariableNode` trees produced by `parseElfVariables()`
 * (`web/src/utils/elfSymbols.ts`). Those trees already materialise:
 *   - struct/union members as `.member`
 *   - pointee members as `->member` / `->[i]`
 *   - array elements as `[i]`
 * with fixed addresses for globals and `location: AddressExpr` for pointer /
 * frame-derived nodes. See `docs/object-watch.md` for the data contract.
 */
import type {AddressExpr, VariableNode} from '@/utils/elfTypes'

// ---------------------------------------------------------------------------
// Limits / defaults
// ---------------------------------------------------------------------------

/** Roots preferred by name (matched against short name / full name / last scope). */
export const DEFAULT_OBJECT_ROOT_NAMES: readonly string[] = ['target_debug_handle']

/** Lazy tree defaults: depth below the root, children per node, total nodes. */
export const DEFAULT_OBJECT_TREE_MAX_DEPTH = 8
export const DEFAULT_OBJECT_TREE_MAX_CHILDREN = 64
export const DEFAULT_OBJECT_TREE_MAX_NODES = 4000

/** Search defaults: flat matches / visited nodes. */
export const DEFAULT_OBJECT_SEARCH_MAX_MATCHES = 200
export const DEFAULT_OBJECT_SEARCH_MAX_NODES = 20000

/** Batch (watch / add curve) defaults. */
export const DEFAULT_OBJECT_BATCH_LIMIT = 512
export const DEFAULT_OBJECT_BATCH_MAX_VISITS = 20000

/** A root with more reachable nodes than this shows the "large object" hint. */
export const DEFAULT_OBJECT_LARGE_ROOT_NODES = 1200

/** Virtual list geometry (px). */
export const OBJECT_ROW_HEIGHT = 24
export const OBJECT_OVERSCAN = 8
export const DEFAULT_OBJECT_VIEWPORT_HEIGHT = 420

/** Preset JSON guard rails (localStorage is written by the parent page). */
export const MAX_OBJECT_PRESET_KEYS = 4096
export const MAX_OBJECT_PRESET_JSON_LENGTH = 512 * 1024

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

function toFiniteNumber(value: unknown): number {
    return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

function toAddress(value: unknown): number {
    return toFiniteNumber(value) >>> 0
}

/** `0x` + 8 uppercase hex digits (same format as `formatHex32()` in elfTypes). */
export function formatObjectAddress(address: number): string {
    return `0x${toAddress(address).toString(16).toUpperCase().padStart(8, '0')}`
}

/** Last `::` scope segment (C++ names); falls back to the whole name. */
export function lastScopeSegment(name: string | null | undefined): string {
    const text = String(name ?? '').trim()
    if (!text) return ''
    const index = text.lastIndexOf('::')
    return index >= 0 ? text.slice(index + 2) : text
}

/** Display name: DWARF short name, else the last scope segment. */
export function objectDisplayName(node: VariableNode | null | undefined): string {
    if (!node) return ''
    return String(node.shortName || lastScopeSegment(node.name) || node.name || '')
}

/** Bare child label: `->cmd_` / `.mode_` / `[0]` -> `cmd_` / `mode_` / `[0]`. */
export function objectNodeLabel(node: VariableNode | null | undefined): string {
    if (!node) return ''
    const raw = String(node.shortName || node.name || '')
    if (!raw) return ''
    const trimmed = raw.replace(/^->/, '').replace(/^\./, '')
    return trimmed || raw
}

/** Root selection key: `<full name>@<hex address>` (stable across re-parses). */
export function objectRootKey(node: VariableNode | null | undefined): string {
    if (!node) return ''
    return `${String(node.name ?? '')}@${toAddress(node.address).toString(16)}`
}

/**
 * DebugCurve-compatible selection key: `${path}@${address.toString(16)}`.
 * Mirrors `nodeKey()` in `DebugCurve.vue` so object fields can be used directly
 * as `CurveSeries.key` / `liveValues` keys (see docs/object-watch.md).
 */
export function objectCurveKey(path: string, node: VariableNode): string
export function objectCurveKey(node: VariableNode): string
export function objectCurveKey(pathOrNode: string | VariableNode, node?: VariableNode): string {
    if (typeof pathOrNode === 'string') {
        const target = node ?? ({} as VariableNode)
        return `${pathOrNode}@${toAddress(target.address).toString(16)}`
    }
    return `${objectPath(pathOrNode)}@${toAddress(pathOrNode.address).toString(16)}`
}

/** Strip the trailing `@<hex>` from a curve key; returns the object path. */
export function objectPathFromCurveKey(key: string | null | undefined): string {
    const text = String(key ?? '')
    if (!text) return ''
    return text.replace(/@[0-9a-fA-F]+$/, '')
}

/** A node is "fixed address" when its storage address (or sample address) is known. */
export function isFixedAddressNode(node: VariableNode | null | undefined): boolean {
    if (!node) return false
    if (toAddress(node.address) !== 0) return true
    return !!node.sample && toAddress(node.sample.address) !== 0
}

/**
 * Pointer type display (`Foo *`, `TargetApp *`).
 * Qualifiers (`const`) are ignored so `const Foo *` still counts as a pointer.
 */
export function isPointerTypeName(typeName: string | null | undefined): boolean {
    let text = String(typeName ?? '').trim()
    if (!text) return false
    text = text.replace(/\b(const|volatile|restrict)\b/g, '').trim()
    return text.endsWith('*')
}

/**
 * Array element count parsed from the trailing `[N]` of a type display.
 * Only the *trailing* dimension is parsed so template types such as
 * `System::Database::Key<float[3]>` are not mistaken for arrays.
 */
export function arrayLengthFromTypeName(typeName: string | null | undefined): number | null {
    const text = String(typeName ?? '').trim()
    const match = /\[(\d+)\]\s*$/.exec(text)
    if (!match) return null
    const value = Number.parseInt(match[1], 10)
    return Number.isFinite(value) && value > 0 ? value : null
}

/** True when the type display looks like an array (trailing `[N]`). */
export function isArrayTypeName(typeName: string | null | undefined): boolean {
    return arrayLengthFromTypeName(typeName) !== null
}

/** Children generated for array elements (`[i]` / `->[i]`). */
function isArrayLikeChildren(node: VariableNode): boolean {
    const first = node.children?.[0]
    if (!first) return false
    return first.name.startsWith('[') || first.name.startsWith('->[')
}

/**
 * Logical child count: array types keep their declared length even when the
 * DWARF parser materialised only the first `MAX_CHILDREN` (64) elements.
 */
export function objectTotalChildCount(node: VariableNode | null | undefined): number {
    if (!node) return 0
    const materialized = node.children?.length ?? 0
    if (materialized === 0 || !isArrayLikeChildren(node)) return materialized
    const declared = arrayLengthFromTypeName(node.typeName)
    return declared !== null && declared > materialized ? declared : materialized
}

// ---------------------------------------------------------------------------
// Path keys
// ---------------------------------------------------------------------------

/**
 * Stable object path. Roots use the DWARF variable name (qualified for class
 * statics); children append their generated segment (`->child`, `.child`,
 * `[i]`, `->[i]`). Example: `target_debug_handle->cmd_.mode_`.
 *
 * Pass `parentPath` for every non-root node; calling with only a child node
 * cannot know its ancestors and returns the node segment alone.
 */
export function objectPath(node: VariableNode | null | undefined, parentPath?: string | null): string {
    if (!node) return ''
    if (parentPath) return objectChildKey(parentPath, node)
    return String(node.name ?? '')
}

/** Append one child segment to `parentKey` (`target_debug_handle` + `->cmd_`). */
export function objectChildKey(parentKey: string | null | undefined, child: VariableNode): string {
    const parent = String(parentKey ?? '')
    const segment = String(child?.name ?? '')
    if (!segment) return parent
    if (segment.startsWith('->') || segment.startsWith('.') || segment.startsWith('[')) {
        return `${parent}${segment}`
    }
    // Defensive: members without the generated separator still get a dot so the
    // key cannot silently collide with the parent.
    return parent ? `${parent}.${segment}` : segment
}

/** Ancestor path list (root first, excluding the node itself). */
export function objectPathAncestors(path: string | null | undefined): string[] {
    const text = String(path ?? '')
    if (!text) return []
    const cuts: number[] = []
    for (let i = 1; i < text.length - 1; i++) {
        const char = text[i]
        if (char === '.' && text[i + 1] !== '.') cuts.push(i)
        else if (char === '-' && text[i + 1] === '>' && i > 0) cuts.push(i)
        else if (char === '[') cuts.push(i)
    }
    const ancestors: string[] = []
    for (const cut of cuts) {
        const prefix = text.slice(0, cut)
        if (prefix && !ancestors.includes(prefix)) ancestors.push(prefix)
    }
    return ancestors
}

// ---------------------------------------------------------------------------
// Location expression text
// ---------------------------------------------------------------------------

function offsetText(offset: number): string {
    if (!offset) return ''
    return offset > 0 ? `+${offset}` : `-${Math.abs(offset)}`
}

/** Human-readable `AddressExpr` (address column tooltip / search text). */
export function formatObjectLocation(expr: AddressExpr | null | undefined, depth = 0): string {
    if (!expr || depth > 16) return ''
    switch (expr.kind) {
        case 'addr':
            return formatObjectAddress(expr.address)
        case 'add':
            return `${formatObjectLocation(expr.inner, depth + 1)}${offsetText(expr.offset)}`
        case 'deref': {
            const inner = formatObjectLocation(expr.inner, depth + 1)
            return `*(${inner})${offsetText(expr.offset)}`
        }
        case 'reg':
            return `r${expr.reg}${offsetText(expr.offset)}`
        case 'cfa':
            return `CFA${offsetText(expr.offset)}`
        default:
            return ''
    }
}

/** Location text for a node: `location`, sample location or core register. */
export function objectNodeLocationText(node: VariableNode | null | undefined): string {
    if (!node) return ''
    const location = node.location ?? node.sample?.location
    if (location) return formatObjectLocation(location)
    if (node.sample?.register !== undefined) return `r${node.sample.register}`
    return ''
}

// ---------------------------------------------------------------------------
// Root discovery
// ---------------------------------------------------------------------------

/** Root classification: pointer to object, array object, or plain struct/class. */
export type ObjectRootBaseKind = 'pointer' | 'object' | 'array'

/** Root badge kind: preferred roots keep their name match visible in the UI. */
export type ObjectRootKind = 'preferred' | ObjectRootBaseKind

export interface ObjectRootOptions {
    /** Names treated as preferred roots (default `target_debug_handle`). */
    preferredNames?: readonly string[]
    /** Include fixed-address arrays (default true). */
    includeArrays?: boolean
    /** Include fixed-address pointers to struct/class/array (default true). */
    includePointers?: boolean
    /** Include fixed-address struct/class/union objects (default true). */
    includeObjects?: boolean
    /** Minimum materialised children (default 1 = exclude scalars). */
    minChildCount?: number
    /** Require a fixed (non-frame) address (default true; excludes locals). */
    requireFixedAddress?: boolean
    /** Keep at most N roots after sorting (0/undefined = unlimited). */
    maxRoots?: number
}

export interface ObjectRoot {
    /** Stable selection key `${fullName}@${hex address}`. */
    key: string
    /** Display name (DWARF short name / last scope segment). */
    name: string
    /** Full DWARF name used as the object path root. */
    qualifiedName: string
    typeName: string
    address: number
    node: VariableNode
    /** Depth of the root itself (always 0). */
    depth: number
    /** Materialised top-level children. */
    childCount: number
    kind: ObjectRootKind
    baseKind: ObjectRootBaseKind
    sampleable: boolean
    liveSamplable: boolean
    file?: string
    relativeFile?: string
    line?: number
}

/** Preferred-name test (short name, full name or last `::` segment). */
export function matchesPreferredRootName(
    node: VariableNode | null | undefined,
    preferredNames: readonly string[] = DEFAULT_OBJECT_ROOT_NAMES,
): boolean {
    if (!node) return false
    const candidates = [node.shortName, node.name, node.qualifiedName, lastScopeSegment(node.name)]
    const wanted = preferredNames.map((name) => String(name ?? '').trim()).filter(Boolean)
    if (wanted.length === 0) return false
    return candidates.some((candidate) => !!candidate && wanted.includes(String(candidate)))
}

/** Base classification (ignores the preferred-name overlay). */
export function objectRootBaseKind(node: VariableNode | null | undefined): ObjectRootBaseKind {
    if (!node) return 'object'
    if (isPointerTypeName(node.typeName)) return 'pointer'
    if (isArrayTypeName(node.typeName)) return 'array'
    return 'object'
}

/** Classification used by the UI badge. */
export function objectRootKind(
    node: VariableNode | null | undefined,
    preferredNames: readonly string[] = DEFAULT_OBJECT_ROOT_NAMES,
): ObjectRootKind {
    if (matchesPreferredRootName(node, preferredNames)) return 'preferred'
    return objectRootBaseKind(node)
}

/**
 * Root candidate rule (see docs/object-watch.md §3):
 *   - sampleable (has a readable location) and has materialised children
 *     (aggregate object, pointer-to-aggregate, or array), otherwise scalars are
 *     excluded;
 *   - fixed (non-frame) address by default, which excludes locals / parameters /
 *     temporaries living in registers or on the stack;
 *   - optionally filtered by kind via `include*` options.
 */
export function isGlobalObjectRoot(
    node: VariableNode | null | undefined,
    options: ObjectRootOptions = {},
): boolean {
    if (!node || typeof node !== 'object') return false
    if (!node.sampleable) return false
    const minChildren = Math.max(1, Math.floor(toFiniteNumber(options.minChildCount ?? 1)))
    if ((node.children?.length ?? 0) < minChildren) return false
    if ((options.requireFixedAddress ?? true) && !isFixedAddressNode(node)) return false
    const baseKind = objectRootBaseKind(node)
    if (baseKind === 'array' && options.includeArrays === false) return false
    if (baseKind === 'pointer' && options.includePointers === false) return false
    if (baseKind === 'object' && options.includeObjects === false) return false
    return true
}

/** Convert a `VariableNode` into an `ObjectRoot`, or `null` when ineligible. */
export function objectRootFromNode(
    node: VariableNode | null | undefined,
    options: ObjectRootOptions = {},
): ObjectRoot | null {
    if (!node || !isGlobalObjectRoot(node, options)) return null
    const childCount = node.children?.length ?? 0
    return {
        key: objectRootKey(node),
        name: objectDisplayName(node),
        qualifiedName: String(node.name ?? ''),
        typeName: String(node.typeName ?? ''),
        address: toAddress(node.address),
        node,
        depth: 0,
        childCount,
        kind: objectRootKind(node, options.preferredNames),
        baseKind: objectRootBaseKind(node),
        sampleable: !!node.sampleable,
        liveSamplable: !!node.liveSamplable,
        file: node.file,
        relativeFile: node.relativeFile,
        line: node.line,
    }
}

function preferredRank(root: ObjectRoot, preferredNames: readonly string[]): number {
    const index = preferredNames.findIndex((name) => {
        const wanted = String(name ?? '').trim()
        if (!wanted) return false
        const candidates = [root.node.shortName, root.node.name, root.node.qualifiedName, lastScopeSegment(root.node.name)]
        return candidates.some((candidate) => candidate === wanted)
    })
    return index
}

const BASE_KIND_ORDER: Record<ObjectRootBaseKind, number> = {pointer: 0, object: 1, array: 2}

function compareObjectRoots(left: ObjectRoot, right: ObjectRoot, preferredNames: readonly string[]): number {
    const leftPreferred = preferredRank(left, preferredNames)
    const rightPreferred = preferredRank(right, preferredNames)
    if (leftPreferred !== rightPreferred) {
        return (leftPreferred < 0 ? Number.MAX_SAFE_INTEGER : leftPreferred)
            - (rightPreferred < 0 ? Number.MAX_SAFE_INTEGER : rightPreferred)
    }
    const kindDelta = BASE_KIND_ORDER[left.baseKind] - BASE_KIND_ORDER[right.baseKind]
    if (kindDelta !== 0) return kindDelta
    if (left.childCount !== right.childCount) return right.childCount - left.childCount
    const nameDelta = left.name.localeCompare(right.name)
    if (nameDelta !== 0) return nameDelta
    return left.address - right.address
}

/**
 * Find global object roots in `parseElfVariables().variables`.
 *
 * Sorting is deterministic: preferred names first, then pointers -> objects ->
 * arrays, then child count desc, name asc, address asc. Duplicate names at the
 * same address are dropped as well as duplicate keys.
 */
export function discoverObjectRoots(
    variables: readonly VariableNode[] | null | undefined,
    options: ObjectRootOptions = {},
): ObjectRoot[] {
    if (!variables || variables.length === 0) return []
    const preferredNames = options.preferredNames ?? DEFAULT_OBJECT_ROOT_NAMES
    const seen = new Set<string>()
    const roots: ObjectRoot[] = []
    for (const variable of variables) {
        const root = objectRootFromNode(variable, options)
        if (!root || seen.has(root.key)) continue
        seen.add(root.key)
        roots.push(root)
    }
    roots.sort((left, right) => compareObjectRoots(left, right, preferredNames))
    const maxRoots = Math.floor(toFiniteNumber(options.maxRoots))
    if (maxRoots > 0 && roots.length > maxRoots) {
        // Preferred candidates are sorted first, so the cap keeps them.
        return roots.slice(0, maxRoots)
    }
    return roots
}

/** First preferred root (by name/list order), else the first candidate. */
export function defaultObjectRoot(
    roots: readonly ObjectRoot[] | null | undefined,
    options: ObjectRootOptions = {},
): ObjectRoot | null {
    if (!roots || roots.length === 0) return null
    const preferredNames = options.preferredNames ?? DEFAULT_OBJECT_ROOT_NAMES
    for (const name of preferredNames) {
        const wanted = String(name ?? '').trim()
        if (!wanted) continue
        const match = roots.find((root) => {
            const candidates = [root.node.shortName, root.node.name, root.qualifiedName, lastScopeSegment(root.node.name)]
            return candidates.some((candidate) => candidate === wanted)
        })
        if (match) return match
    }
    return roots[0] ?? null
}

/** Root lookup by `objectRootKey()` (or by full variable name when unique). */
export function findObjectRoot(
    roots: readonly ObjectRoot[] | null | undefined,
    key: string | null | undefined,
): ObjectRoot | null {
    const needle = String(key ?? '').trim()
    if (!roots || !needle) return null
    const direct = roots.find((root) => root.key === needle)
    if (direct) return direct
    const byName = roots.filter((root) => root.qualifiedName === needle || root.node.name === needle)
    return byName.length === 1 ? byName[0] : null
}

// ---------------------------------------------------------------------------
// Lazy object tree
// ---------------------------------------------------------------------------

export interface ObjectTreeNode {
    /** Stable object path (same as `path`). */
    key: string
    path: string
    parentPath: string | null
    rootKey: string
    /** Raw DWARF name (`->member` / `.member` / `[i]`). */
    name: string
    /** Bare display label (`member` / `[i]`). */
    displayName: string
    typeName: string
    address: number
    sampleAddress: number
    locationText: string
    depth: number
    /** Materialised children in this tree (0 when collapsed). */
    childCount: number
    /** Logical children (array declared length may exceed the parser cap). */
    totalChildCount: number
    /** `totalChildCount > childCount` (parser cap or tree limit). */
    hasMoreChildren: boolean
    /** Declared array length exceeds the materialised children (parser cap). */
    parserCapped: boolean
    expandable: boolean
    expanded: boolean
    /** Children were cut by maxChildren / maxNodes / childLimit. */
    truncated: boolean
    /** Children were cut by maxDepth (deeper expansion not materialised). */
    depthLimited: boolean
    children: ObjectTreeNode[]
    node: VariableNode
    curveKey: string
    sampleable: boolean
    liveSamplable: boolean
    fixedAddress: boolean
    pointer: boolean
    arrayLike: boolean
    /** Set by `filterObjectTree()`: node itself matches the filter (ancestors are kept too). */
    matched?: boolean
    file?: string
    relativeFile?: string
    line?: number
}

export interface ObjectTreeOptions {
    /** Object paths whose children should be materialised (lazy expansion). */
    expanded?: Iterable<string>
    /** Maximum depth below the root (root = 0; default 8). */
    maxDepth?: number
    /** Maximum materialised children per node (default 64). */
    maxChildren?: number
    /** Total tree node budget (default 4000); protects 20k+ pointer trees. */
    maxNodes?: number
    /** Optional per-path child limit (component pagination overrides the default). */
    childLimit?: (path: string, depth: number, childCount: number) => number | undefined
    /** Materialise root children immediately (default true). */
    expandRoot?: boolean
}

interface MakeTreeNodeInput {
    rootKey: string
    node: VariableNode
    path: string
    parentPath: string | null
    depth: number
}

function makeObjectTreeNode(input: MakeTreeNodeInput): ObjectTreeNode {
    const {rootKey, node, path, parentPath, depth} = input
    const materialized = node.children?.length ?? 0
    const total = objectTotalChildCount(node)
    const fixedAddress = isFixedAddressNode(node)
    return {
        key: path,
        path,
        parentPath,
        rootKey,
        name: String(node.name ?? ''),
        displayName: objectNodeLabel(node),
        typeName: String(node.typeName ?? ''),
        address: toAddress(node.address),
        sampleAddress: toAddress(node.sample?.address),
        locationText: objectNodeLocationText(node),
        depth,
        childCount: 0,
        totalChildCount: total,
        hasMoreChildren: total > 0,
        parserCapped: total > materialized,
        expandable: materialized > 0,
        expanded: false,
        truncated: false,
        depthLimited: false,
        children: [],
        node,
        curveKey: objectCurveKey(path, node),
        sampleable: !!node.sampleable,
        liveSamplable: !!node.liveSamplable,
        fixedAddress,
        pointer: isPointerTypeName(node.typeName),
        arrayLike: materialized > 0 && isArrayLikeChildren(node),
        file: node.file,
        relativeFile: node.relativeFile,
        line: node.line,
    }
}

function normalizeExpanded(options: ObjectTreeOptions): ReadonlySet<string> {
    const source = options.expanded
    if (!source) return new Set<string>()
    if (source instanceof Set) return source as ReadonlySet<string>
    return new Set<string>(source)
}

function isObjectTreeNodeLike(value: unknown): value is ObjectTreeNode {
    if (!value || typeof value !== 'object') return false
    const candidate = value as ObjectTreeNode
    return typeof candidate.path === 'string' && typeof candidate.curveKey === 'string' && !!candidate.node
}

function isObjectRootLike(value: unknown): value is ObjectRoot {
    if (!value || typeof value !== 'object') return false
    const candidate = value as ObjectRoot
    return !!candidate.node && typeof candidate.key === 'string' && typeof candidate.qualifiedName === 'string'
}

function variableRootOf(input: ObjectRoot | ObjectTreeNode | VariableNode): {node: VariableNode; rootKey: string; rootPath: string} {
    if (isObjectTreeNodeLike(input)) {
        return {node: input.node, rootKey: input.rootKey, rootPath: input.path}
    }
    if (isObjectRootLike(input)) {
        return {node: input.node, rootKey: input.key || objectRootKey(input.node), rootPath: objectPath(input.node)}
    }
    const node = input as VariableNode
    return {node, rootKey: objectRootKey(node), rootPath: objectPath(node)}
}

/**
 * Build a lazily-expanded object tree.
 *
 * Only roots and the children of `expanded` paths are materialised, so a root
 * whose DWARF subtree has 20k+ pointer nodes costs O(visible nodes), not O(tree).
 * `maxDepth` / `maxChildren` / `maxNodes` are hard caps; node metadata always
 * records truncation so the view can show a "show more"/limit hint.
 */
export function buildObjectTree(
    input: ObjectRoot | ObjectTreeNode | VariableNode | null | undefined,
    options: ObjectTreeOptions = {},
): ObjectTreeNode | null {
    if (!input) return null
    const {node: rootNode, rootKey, rootPath} = variableRootOf(input)
    if (!rootNode) return null
    const expanded = normalizeExpanded(options)
    const maxDepth = Math.max(1, Math.floor(toFiniteNumber(options.maxDepth ?? DEFAULT_OBJECT_TREE_MAX_DEPTH)))
    const maxChildren = Math.max(0, Math.floor(toFiniteNumber(options.maxChildren ?? DEFAULT_OBJECT_TREE_MAX_CHILDREN)))
    const maxNodes = Math.max(1, Math.floor(toFiniteNumber(options.maxNodes ?? DEFAULT_OBJECT_TREE_MAX_NODES)))
    const expandRoot = options.expandRoot !== false
    const budget = {remaining: maxNodes}

    const visit = (current: VariableNode, path: string, parentPath: string | null, depth: number): ObjectTreeNode => {
        budget.remaining -= 1
        const treeNode = makeObjectTreeNode({rootKey, node: current, path, parentPath, depth})
        const materialized = current.children?.length ?? 0
        const shouldExpand = depth === 0 ? expandRoot : expanded.has(path)
        if (!shouldExpand || materialized === 0) {
            treeNode.hasMoreChildren = treeNode.totalChildCount > 0
            return treeNode
        }
        treeNode.expanded = true
        if (depth >= maxDepth) {
            treeNode.depthLimited = true
            treeNode.hasMoreChildren = true
            return treeNode
        }
        const customLimit = options.childLimit?.(path, depth, materialized)
        const wanted = customLimit === undefined
            ? maxChildren
            : Math.max(0, Math.floor(toFiniteNumber(customLimit)))
        const slice = Math.min(materialized, wanted, Math.max(0, budget.remaining))
        const children: ObjectTreeNode[] = []
        for (let index = 0; index < slice; index++) {
            const child = current.children![index]
            if (budget.remaining <= 0) break
            children.push(visit(child, objectChildKey(path, child), path, depth + 1))
        }
        treeNode.children = children
        treeNode.childCount = children.length
        treeNode.truncated = children.length < materialized
        treeNode.hasMoreChildren = children.length < treeNode.totalChildCount
        return treeNode
    }

    return visit(rootNode, rootPath, null, 0)
}

/** DFS pre-order rows for a built tree (collapsed children are not present). */
export function flattenObjectTree(tree: ObjectTreeNode | null | undefined): ObjectTreeNode[] {
    const rows: ObjectTreeNode[] = []
    if (!tree) return rows
    const stack: ObjectTreeNode[] = [tree]
    while (stack.length > 0) {
        const current = stack.pop() as ObjectTreeNode
        rows.push(current)
        for (let index = current.children.length - 1; index >= 0; index--) {
            stack.push(current.children[index])
        }
    }
    return rows
}

/** Count reachable DWARF nodes below (and including) a node, bounded. */
export function countObjectNodes(
    node: VariableNode | null | undefined,
    maxNodes = 20000,
): {count: number; truncated: boolean} {
    if (!node) return {count: 0, truncated: false}
    const limit = Math.max(1, Math.floor(toFiniteNumber(maxNodes) || 20000))
    let count = 0
    let truncated = false
    const visited = new WeakSet<VariableNode>()
    const stack: VariableNode[] = [node]
    while (stack.length > 0) {
        if (count >= limit) {
            truncated = true
            break
        }
        const current = stack.pop() as VariableNode
        if (visited.has(current)) continue
        visited.add(current)
        count += 1
        for (const child of current.children ?? []) stack.push(child)
    }
    return {count, truncated}
}

// ---------------------------------------------------------------------------
// Filtering / search
// ---------------------------------------------------------------------------

export interface ObjectFilterOptions {
    /** Keep only nodes with a readable sample location. */
    sampleableOnly?: boolean
    /** Keep only nodes with a fixed (non-frame) address. */
    fixedAddressOnly?: boolean
    /** Keep only nodes that have a current live value (requires `hasValue`). */
    withValueOnly?: boolean
    /** Parent-provided live-value lookup (keyed by object path or curve key). */
    hasValue?: (node: ObjectTreeNode) => boolean
}

/** Does a single tree node pass the filter (ancestors are always kept)? */
export function objectTreeNodeMatchesFilter(
    node: ObjectTreeNode | null | undefined,
    options: ObjectFilterOptions = {},
): boolean {
    if (!node) return false
    if (options.sampleableOnly && !node.sampleable) return false
    if (options.fixedAddressOnly && !node.fixedAddress) return false
    if (options.withValueOnly) {
        if (!options.hasValue || !options.hasValue(node)) return false
    }
    return true
}

/**
 * Prune a built tree with `options`, keeping ancestors of matching nodes.
 * Returns `null` when nothing matches (view shows the "no rows" state).
 */
export function filterObjectTree(
    tree: ObjectTreeNode | null | undefined,
    options: ObjectFilterOptions = {},
): ObjectTreeNode | null {
    if (!tree) return null
    const visit = (node: ObjectTreeNode): ObjectTreeNode | null => {
        const matched = objectTreeNodeMatchesFilter(node, options)
        const children: ObjectTreeNode[] = []
        for (const child of node.children) {
            const filtered = visit(child)
            if (filtered) children.push(filtered)
        }
        if (!matched && children.length === 0) return null
        const next: ObjectTreeNode = {...node, matched, children, childCount: children.length}
        return next
    }
    return visit(tree)
}

function objectSearchText(node: VariableNode, path: string): string {
    const fields = [
        path,
        node.name,
        node.shortName,
        node.qualifiedName,
        node.typeName,
        node.mangledName,
        formatObjectAddress(node.address),
        node.sample ? formatObjectAddress(node.sample.address) : '',
        objectNodeLocationText(node),
        node.relativeFile,
        node.file,
        node.line !== undefined ? `:${node.line}` : '',
    ]
    return fields.filter((value) => !!value).join('\n').toLowerCase()
}

export interface ObjectSearchOptions {
    /** Maximum matches returned (default 200). */
    maxMatches?: number
    /** Maximum visited DWARF nodes (default 20000). */
    maxNodes?: number
    /** Extra filter applied to each candidate (e.g. sampleable-only). */
    filter?: (node: ObjectTreeNode) => boolean
}

export interface ObjectSearchMatch {
    node: ObjectTreeNode
    /** Ancestor object paths root..parent (for "reveal in tree"). */
    ancestors: string[]
}

function rootIdentityOf(
    input: ObjectRoot | ObjectTreeNode | VariableNode,
): {node: VariableNode; rootKey: string; rootPath: string; startDepth: number; ancestors: string[]} {
    if (isObjectTreeNodeLike(input)) {
        return {
            node: input.node,
            rootKey: input.rootKey,
            rootPath: input.path,
            startDepth: input.depth,
            ancestors: objectPathAncestors(input.path),
        }
    }
    if (isObjectRootLike(input)) {
        return {
            node: input.node,
            rootKey: input.key || objectRootKey(input.node),
            rootPath: objectPath(input.node),
            startDepth: 0,
            ancestors: [],
        }
    }
    const node = input as VariableNode
    return {node, rootKey: objectRootKey(node), rootPath: objectPath(node), startDepth: 0, ancestors: []}
}

/**
 * Search names / paths / types / addresses below a root (or a subtree node).
 * Traverses the raw DWARF tree (works while branches are collapsed) with a
 * visited-node budget; returns flat matches plus ancestor paths for reveal.
 */
export function searchObjectTree(
    input: ObjectRoot | ObjectTreeNode | VariableNode | null | undefined,
    query: string | null | undefined,
    options: ObjectSearchOptions = {},
): ObjectSearchMatch[] {
    if (!input) return []
    const needle = String(query ?? '').trim().toLowerCase()
    if (!needle) return []
    const identity = rootIdentityOf(input)
    const maxMatches = Math.max(1, Math.floor(toFiniteNumber(options.maxMatches ?? DEFAULT_OBJECT_SEARCH_MAX_MATCHES)))
    const maxNodes = Math.max(1, Math.floor(toFiniteNumber(options.maxNodes ?? DEFAULT_OBJECT_SEARCH_MAX_NODES)))
    const matches: ObjectSearchMatch[] = []
    const visited = new WeakSet<VariableNode>()
    let visitCount = 0
    const walk = (node: VariableNode, path: string, depth: number, ancestors: string[]): void => {
        if (matches.length >= maxMatches || visitCount >= maxNodes) return
        if (visited.has(node)) return
        visited.add(node)
        visitCount += 1
        const treeNode = makeObjectTreeNode({
            rootKey: identity.rootKey,
            node,
            path,
            parentPath: ancestors.length > 0 ? ancestors[ancestors.length - 1] : null,
            depth,
        })
        if (objectSearchText(node, path).includes(needle) && (!options.filter || options.filter(treeNode))) {
            matches.push({node: treeNode, ancestors: [...ancestors]})
            if (matches.length >= maxMatches) return
        }
        const children = node.children ?? []
        if (children.length === 0) return
        const nextAncestors = [...ancestors, path]
        for (const child of children) {
            walk(child, objectChildKey(path, child), depth + 1, nextAncestors)
            if (matches.length >= maxMatches || visitCount >= maxNodes) return
        }
    }
    walk(identity.node, identity.rootPath, identity.startDepth, identity.ancestors)
    return matches
}

/**
 * Resolve several object paths (e.g. a preset's key list) in a single tree walk.
 * Keys may include curve-key suffixes (`path@hex`); unknown keys are ignored.
 */
export function findObjectNodes(
    input: ObjectRoot | ObjectTreeNode | VariableNode | null | undefined,
    paths: readonly string[] | null | undefined,
    options: {maxNodes?: number} = {},
): ObjectSearchMatch[] {
    if (!input || !paths || paths.length === 0) return []
    const identity = rootIdentityOf(input)
    const wanted = new Set<string>()
    for (const raw of paths) {
        const key = objectPathFromCurveKey(String(raw ?? '').trim())
        if (key) wanted.add(key)
    }
    if (wanted.size === 0) return []
    const maxNodes = Math.max(1, Math.floor(toFiniteNumber(options.maxNodes ?? DEFAULT_OBJECT_SEARCH_MAX_NODES)))
    const matches: ObjectSearchMatch[] = []
    const visited = new WeakSet<VariableNode>()
    let visitCount = 0
    const walk = (node: VariableNode, path: string, depth: number, ancestors: string[]): void => {
        if (wanted.size === 0 || visitCount >= maxNodes) return
        if (visited.has(node)) return
        visited.add(node)
        visitCount += 1
        if (wanted.has(path)) {
            matches.push({
                node: makeObjectTreeNode({
                    rootKey: identity.rootKey,
                    node,
                    path,
                    parentPath: ancestors.length > 0 ? ancestors[ancestors.length - 1] : null,
                    depth,
                }),
                ancestors: [...ancestors],
            })
            wanted.delete(path)
            if (wanted.size === 0) return
        }
        const children = node.children ?? []
        if (children.length === 0) return
        const nextAncestors = [...ancestors, path]
        for (const child of children) {
            walk(child, objectChildKey(path, child), depth + 1, nextAncestors)
            if (wanted.size === 0 || visitCount >= maxNodes) return
        }
    }
    walk(identity.node, identity.rootPath, identity.startDepth, identity.ancestors)
    return matches
}

/** Locate a node by object path (or curve key) below a root, bounded. */
export function findObjectNode(
    input: ObjectRoot | ObjectTreeNode | VariableNode | null | undefined,
    pathOrKey: string | null | undefined,
    options: {maxNodes?: number} = {},
): ObjectSearchMatch | null {
    if (!input) return null
    const target = objectPathFromCurveKey(String(pathOrKey ?? '').trim())
    if (!target) return null
    const identity = rootIdentityOf(input)
    const maxNodes = Math.max(1, Math.floor(toFiniteNumber(options.maxNodes ?? DEFAULT_OBJECT_SEARCH_MAX_NODES)))
    let visitCount = 0
    let result: ObjectSearchMatch | null = null
    const visited = new WeakSet<VariableNode>()
    const walk = (node: VariableNode, path: string, depth: number, ancestors: string[]): boolean => {
        if (visitCount >= maxNodes) return false
        if (visited.has(node)) return false
        visited.add(node)
        visitCount += 1
        if (path === target) {
            result = {
                node: makeObjectTreeNode({
                    rootKey: identity.rootKey,
                    node,
                    path,
                    parentPath: ancestors.length > 0 ? ancestors[ancestors.length - 1] : null,
                    depth,
                }),
                ancestors: [...ancestors],
            }
            return true
        }
        const children = node.children ?? []
        const nextAncestors = [...ancestors, path]
        for (const child of children) {
            if (walk(child, objectChildKey(path, child), depth + 1, nextAncestors)) return true
        }
        return false
    }
    walk(identity.node, identity.rootPath, identity.startDepth, identity.ancestors)
    return result
}

// ---------------------------------------------------------------------------
// Node refs + watch/curve batches
// ---------------------------------------------------------------------------

/** Formatted current value passed by the parent page (component only renders it). */
export interface ObjectLiveValue {
    /** Already formatted value text. */
    text: string
    /** Curve color used for the value text. */
    color?: string
    /** Value is stale (sampling stopped / no fresh point): rendered muted. */
    stale?: boolean
}

/**
 * key -> current value. Keys may be object paths (`objectPath`) or
 * DebugCurve-compatible curve keys (`${path}@${address.toString(16)}`); the
 * component checks both so `SourceLiveValueMap` from DebugCurve can be reused.
 */
export type ObjectLiveValueMap = Readonly<Record<string, ObjectLiveValue>>

/** Payload emitted for a field (parent maps it to a curve/watch entry). */
export interface ObjectNodeRef {
    /** Stable object path (`objectPath`). */
    key: string
    /** DebugCurve-compatible key (`${path}@${address.toString(16)}`). */
    curveKey: string
    path: string
    name: string
    fullName: string
    typeName: string
    address: number
    size: number
    sampleAddress: number
    locationText: string
    rootKey: string
    rootName: string
    depth: number
    sampleable: boolean
    liveSamplable: boolean
    fixedAddress: boolean
    pointer: boolean
    arrayLike: boolean
    childCount: number
    file?: string
    relativeFile?: string
    line?: number
    node: VariableNode
}

/** Build the serializable selection payload for one tree node. */
export function objectNodeRef(
    root: Pick<ObjectRoot, 'key' | 'name'> | null | undefined,
    node: VariableNode,
    path: string,
    depth = 0,
): ObjectNodeRef {
    const materialized = node.children?.length ?? 0
    return {
        key: path,
        curveKey: objectCurveKey(path, node),
        path,
        name: objectNodeLabel(node),
        fullName: String(node.name ?? ''),
        typeName: String(node.typeName ?? ''),
        address: toAddress(node.address),
        size: toFiniteNumber(node.size),
        sampleAddress: toAddress(node.sample?.address),
        locationText: objectNodeLocationText(node),
        rootKey: String(root?.key ?? ''),
        rootName: String(root?.name ?? ''),
        depth,
        sampleable: !!node.sampleable,
        liveSamplable: !!node.liveSamplable,
        fixedAddress: isFixedAddressNode(node),
        pointer: isPointerTypeName(node.typeName),
        arrayLike: materialized > 0 && isArrayLikeChildren(node),
        childCount: materialized,
        file: node.file,
        relativeFile: node.relativeFile,
        line: node.line,
        node,
    }
}

/** Tree node -> selection payload. */
export function objectTreeNodeRef(
    root: Pick<ObjectRoot, 'key' | 'name'> | null | undefined,
    treeNode: ObjectTreeNode,
): ObjectNodeRef {
    return objectNodeRef(root, treeNode.node, treeNode.path, treeNode.depth)
}

/**
 * Can this node be plotted/watched? True when the DWARF node has a sample leaf
 * (fixed address, pointer/live location, or a debug-frame expression).
 *
 * The parent page still decides whether the current sampling mode supports the
 * node (e.g. local sampling needs `liveSamplable`/`location`); the ref payload
 * carries `sampleable` / `liveSamplable` / `locationText` for that check.
 */
export function isObjectNodeCurveable(node: VariableNode | null | undefined): boolean {
    if (!node || !node.sampleable) return false
    return !!node.sample
}

export type ObjectBatchMode = 'active' | 'selected' | 'visible' | 'subtree'

export interface ObjectCollectOptions {
    /** Maximum refs emitted (default 512). */
    maxBatch?: number
    /** Only include nodes usable as curve/watch entries (default true). */
    curveableOnly?: boolean
    /** Maximum visited DWARF nodes while walking a subtree (default 20000). */
    maxVisits?: number
}

export interface ObjectBatchPayload {
    mode: ObjectBatchMode
    nodes: ObjectNodeRef[]
    /** Number of eligible nodes found before the `maxBatch` cap. */
    total: number
    /** `total > nodes.length` (cap hit). */
    truncated: boolean
}

/** `toggle-watch` payload: the field plus the requested new star state. */
export interface ObjectToggleWatchPayload extends ObjectNodeRef {
    /** true = star on (parent should watch it), false = star off. */
    watched: boolean
}

/** `jump-source` payload: DWARF file/line of the field (or of the root). */
export interface ObjectSourceJumpPayload {
    /** Preferred source path (relative DWARF path, else absolute). */
    path: string
    relativeFile?: string
    file?: string
    /** 1-based line. */
    line: number
    /** Executable/sample address when available (may be absent for pointer fields). */
    address?: number
    node: ObjectNodeRef
}

/** `save-preset` payload; persistence/localStorage is the parent page's job. */
export interface ObjectPresetSavePayload {
    name: string
    /** Stable root key (`name@address`). */
    rootKey: string
    /** Root object path (DWARF variable name); convenient for localStorage keys. */
    rootPath: string
    keys: string[]
}

function batchOptions(options: ObjectCollectOptions = {}): {
    maxBatch: number
    curveableOnly: boolean
    maxVisits: number
} {
    return {
        maxBatch: Math.max(1, Math.floor(toFiniteNumber(options.maxBatch ?? DEFAULT_OBJECT_BATCH_LIMIT))),
        curveableOnly: options.curveableOnly !== false,
        maxVisits: Math.max(1, Math.floor(toFiniteNumber(options.maxVisits ?? DEFAULT_OBJECT_BATCH_MAX_VISITS))),
    }
}

function isBatchCandidate(node: VariableNode, options: ObjectCollectOptions): boolean {
    if (options.curveableOnly === false) return true
    return isObjectNodeCurveable(node)
}

/**
 * Collect watch/curve batch refs from already materialised tree nodes.
 * `mode` is only echoed in the payload; the caller decides which rows to pass
 * (`selection` items, visible rows or a subtree root).
 */
export function collectObjectNodes(
    root: Pick<ObjectRoot, 'key' | 'name'> | null | undefined,
    trees: readonly ObjectTreeNode[],
    mode: ObjectBatchMode = 'visible',
    options: ObjectCollectOptions = {},
): ObjectBatchPayload {
    const limits = batchOptions(options)
    const nodes: ObjectNodeRef[] = []
    let total = 0
    if (mode === 'subtree') {
        for (const tree of trees) {
            const visited = new WeakSet<VariableNode>()
            const stack: Array<{node: VariableNode; path: string; depth: number}> = [{node: tree.node, path: tree.path, depth: tree.depth}]
            while (stack.length > 0) {
                if (total >= limits.maxVisits) break
                const current = stack.pop() as {node: VariableNode; path: string; depth: number}
                if (visited.has(current.node)) continue
                visited.add(current.node)
                if (isBatchCandidate(current.node, options)) {
                    total += 1
                    if (nodes.length < limits.maxBatch) {
                        nodes.push(objectNodeRef(root, current.node, current.path, current.depth))
                    }
                }
                const children = current.node.children ?? []
                for (let index = children.length - 1; index >= 0; index--) {
                    const child = children[index]
                    stack.push({node: child, path: objectChildKey(current.path, child), depth: current.depth + 1})
                }
            }
        }
        return {mode, nodes, total, truncated: total > nodes.length}
    }
    const seen = new Set<string>()
    for (const tree of trees) {
        if (seen.has(tree.path)) continue
        seen.add(tree.path)
        if (!isBatchCandidate(tree.node, options)) continue
        total += 1
        if (nodes.length < limits.maxBatch) nodes.push(objectTreeNodeRef(root, tree))
    }
    return {mode, nodes, total, truncated: total > nodes.length}
}

/** Convenience: collect a whole subtree of DWARF nodes (bounded). */
export function collectObjectSubtree(
    root: Pick<ObjectRoot, 'key' | 'name'> | null | undefined,
    treeNode: ObjectTreeNode,
    options: ObjectCollectOptions = {},
): ObjectBatchPayload {
    return collectObjectNodes(root, [treeNode], 'subtree', options)
}

// ---------------------------------------------------------------------------
// Virtual row window
// ---------------------------------------------------------------------------

export interface ObjectVirtualWindowParams {
    scrollTop: number
    viewportHeight: number
    rowHeight?: number
    rowCount: number
    overscan?: number
}

export interface ObjectVirtualWindow {
    start: number
    end: number
    offsetY: number
    totalHeight: number
    overscan: number
}

/** Virtual-list window math (only visible rows are rendered). */
export function computeObjectVirtualWindow(params: ObjectVirtualWindowParams): ObjectVirtualWindow {
    const rowHeight = toFiniteNumber(params.rowHeight) > 0 ? toFiniteNumber(params.rowHeight) : OBJECT_ROW_HEIGHT
    const rowCount = Math.max(0, Math.floor(toFiniteNumber(params.rowCount)))
    const overscan = Math.max(0, Math.floor(toFiniteNumber(params.overscan ?? OBJECT_OVERSCAN)))
    const totalHeight = rowCount * rowHeight
    if (rowCount === 0) return {start: 0, end: 0, offsetY: 0, totalHeight: 0, overscan}
    const scrollTop = Math.max(0, toFiniteNumber(params.scrollTop))
    const viewportHeight = Math.max(0, toFiniteNumber(params.viewportHeight))
    const visibleCount = Math.max(1, Math.ceil(viewportHeight / rowHeight))
    let start = Math.floor(scrollTop / rowHeight) - overscan
    if (start > rowCount - 1) start = rowCount - 1
    if (start < 0) start = 0
    const end = Math.min(rowCount, start + visibleCount + overscan * 2)
    return {start, end, offsetY: start * rowHeight, totalHeight, overscan}
}

// ---------------------------------------------------------------------------
// Presets (JSON text helpers; persistence is the parent page's job)
// ---------------------------------------------------------------------------

export interface ObjectPreset {
    id: string
    name: string
    /** Root object key (`objectRootKey`) the keys belong to. */
    rootKey: string
    /** Root object path (DWARF variable name; derived from `rootKey` when absent). */
    rootPath: string
    /** Field object paths (or legacy curve keys; `objectPathFromCurveKey` normalizes). */
    keys: string[]
    createdAt?: number
    updatedAt?: number
}

/**
 * Tolerant preset shape: DebugCurve's `utils/objectWatchPresets.ts` stores
 * `{rootPath, fields}` while this module stores `{rootKey, keys}`. Both are
 * accepted through `normalizeObjectPreset()` / `sanitizeObjectPreset()`.
 */
export interface ObjectPresetLike {
    id: string
    name: string
    rootKey?: string
    rootPath?: string
    keys?: readonly string[]
    fields?: readonly string[]
    createdAt?: number
    updatedAt?: number
}

export interface ObjectPresetInput {
    name: string
    rootKey: string
    keys: readonly string[]
    id?: string
    createdAt?: number
    updatedAt?: number
}

function normalizePresetKey(value: unknown): string {
    if (typeof value !== 'string') return ''
    return objectPathFromCurveKey(value.trim())
}

function presetRootFields(record: Record<string, unknown>): {rootKey: string; rootPath: string} {
    const rootKey = typeof record.rootKey === 'string' ? record.rootKey.trim() : ''
    const explicitPath = typeof record.rootPath === 'string' ? record.rootPath.trim() : ''
    const rootPath = explicitPath || objectPathFromCurveKey(rootKey)
    return {rootKey: rootKey || rootPath, rootPath}
}

function presetKeyList(record: Record<string, unknown>): string[] {
    const raw = Array.isArray(record.keys)
        ? record.keys
        : Array.isArray(record.fields)
            ? record.fields
            : []
    const keys: string[] = []
    const seen = new Set<string>()
    for (const item of raw) {
        const key = normalizePresetKey(item)
        if (!key || seen.has(key) || keys.length >= MAX_OBJECT_PRESET_KEYS) continue
        seen.add(key)
        keys.push(key)
    }
    return keys
}

/** Normalize a tolerant preset shape (parent props / localStorage record). */
export function normalizeObjectPreset(value: ObjectPresetLike | null | undefined): ObjectPreset | null {
    if (!value || typeof value !== 'object') return null
    return sanitizeObjectPreset(value)
}

/**
 * Sanitize a preset coming from localStorage / JSON: validates the shape,
 * accepts `{rootKey, keys}` or `{rootPath, fields}`, normalizes keys to object
 * paths, drops duplicates and enforces the size cap.
 * Returns `null` for invalid input (never throws).
 */
export function sanitizeObjectPreset(value: unknown): ObjectPreset | null {
    if (!value || typeof value !== 'object') return null
    const record = value as Record<string, unknown>
    const id = typeof record.id === 'string' ? record.id.trim() : ''
    if (!id) return null
    const name = typeof record.name === 'string' ? record.name.trim() : ''
    const {rootKey, rootPath} = presetRootFields(record)
    const keys = presetKeyList(record)
    const preset: ObjectPreset = {
        id,
        name: name || id,
        rootKey,
        rootPath,
        keys,
    }
    const createdAt = toFiniteNumber(record.createdAt)
    const updatedAt = toFiniteNumber(record.updatedAt)
    if (createdAt > 0) preset.createdAt = createdAt
    if (updatedAt > 0) preset.updatedAt = updatedAt
    return preset
}

/** Create a preset object (deterministic ids when `id` is supplied by the caller). */
export function createObjectPreset(input: ObjectPresetInput, now = Date.now()): ObjectPreset | null {
    const name = String(input?.name ?? '').trim()
    const rawKeys = Array.isArray(input?.keys) ? input.keys : []
    const keys: string[] = []
    const seen = new Set<string>()
    for (const item of rawKeys) {
        const key = normalizePresetKey(item)
        if (!key || seen.has(key) || keys.length >= MAX_OBJECT_PRESET_KEYS) continue
        seen.add(key)
        keys.push(key)
    }
    if (keys.length === 0) return null
    const rootKey = String(input?.rootKey ?? '').trim()
    return {
        id: String(input?.id ?? '').trim() || `object-preset-${now.toString(36)}`,
        name: name || `Object preset ${keys.length}`,
        rootKey,
        rootPath: objectPathFromCurveKey(rootKey),
        keys,
        createdAt: toFiniteNumber(input?.createdAt) || now,
        updatedAt: toFiniteNumber(input?.updatedAt) || now,
    }
}

/** Serialize a preset to pretty JSON text (parent copies/writes it). */
export function objectPresetToJson(preset: ObjectPreset | null | undefined): string {
    if (!preset) return ''
    return JSON.stringify({
        id: preset.id,
        name: preset.name,
        rootKey: preset.rootKey,
        rootPath: preset.rootPath,
        keys: preset.keys,
        createdAt: preset.createdAt,
        updatedAt: preset.updatedAt,
    }, null, 2)
}

/** Parse preset JSON text; returns `null` for empty/invalid/oversized input. */
export function parseObjectPresetJson(text: string | null | undefined): ObjectPreset | null {
    const raw = String(text ?? '').trim()
    if (!raw || raw.length > MAX_OBJECT_PRESET_JSON_LENGTH) return null
    let parsed: unknown
    try {
        parsed = JSON.parse(raw)
    } catch {
        return null
    }
    return sanitizeObjectPreset(parsed)
}
