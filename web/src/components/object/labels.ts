/**
 * ObjectBrowser.vue text labels.
 *
 * The component never imports vue-i18n: every visible string comes from this
 * default English map and the parent page can override any field through the
 * `labels` prop (DebugCurve passes translated strings; see
 * `docs/object-watch.md` "DebugCurve integration").
 *
 * `{placeholder}` tokens are replaced by `formatObjectBrowserLabel()`.
 */

export interface ObjectBrowserLabels {
    /** Panel title (used by the parent page header / a11y label). */
    title: string
    /** Section label above the root picker. */
    rootsLabel: string
    /** Placeholder of the root search input. */
    rootSearchPlaceholder: string
    /** Empty root-search result. */
    rootMenuEmpty: string
    /** Root list is truncated, `{shown}` `{total}`. */
    rootMenuMore: string
    /** Root count chip, `{count}`. */
    rootCount: string
    /** No root selected yet. */
    noRootSelected: string
    /** Preferred root badge (default `target_debug_handle`). */
    preferredBadge: string
    /** Pointer-to-object root badge. */
    pointerBadge: string
    /** Struct/class/union root badge. */
    objectBadge: string
    /** Array root badge. */
    arrayBadge: string
    /** Field/path search placeholder. */
    searchPlaceholder: string
    /** Only sampleable rows checkbox. */
    sampleableOnly: string
    /** Only fixed-address rows checkbox. */
    fixedAddressOnly: string
    /** Only rows with a live value checkbox. */
    withValueOnly: string
    /** Expand every branch of the current root. */
    expandAll: string
    /** Collapse every branch of the current root. */
    collapseAll: string
    /** Expand one branch (row caret tooltip). */
    expandField: string
    /** Collapse one branch (row caret tooltip). */
    collapseField: string
    /** Watch all fields below the active row. */
    watchSubtree: string
    /** Watch all currently visible rows. */
    watchVisible: string
    /** Watch all marked rows. */
    watchSelected: string
    /** Add every eligible visible field as a curve. */
    addCurves: string
    /** Add marked rows as curves. */
    addSelected: string
    /** Marked selection count, `{count}`. */
    selectedCount: string
    /** Preset section label. */
    presetsLabel: string
    /** Preset select placeholder. */
    presetSelect: string
    /** Preset name input placeholder. */
    presetNamePlaceholder: string
    /** Default preset name prefix when the input is empty, `{count}`. */
    presetDefaultName: string
    /** Save preset button. */
    presetSave: string
    /** Load preset button. */
    presetLoad: string
    /** Delete preset button. */
    presetDelete: string
    /** No presets available. */
    presetEmpty: string
    /** Table header: name. */
    colName: string
    /** Table header: type. */
    colType: string
    /** Table header: address. */
    colAddress: string
    /** Table header: current value. */
    colValue: string
    /** Row action: jump to source file:line. */
    sourceJump: string
    /** Row action disabled because the node has no DWARF file/line. */
    noSourceLocation: string
    /** Star tooltip when not watched. */
    watchAdd: string
    /** Star tooltip when watched. */
    watchRemove: string
    /** Default value placeholder. */
    noValue: string
    /** Stale live value hint. */
    staleValue: string
    /** Live-samplable badge. */
    liveBadge: string
    /** Requires halt (debug mode) badge. */
    haltBadge: string
    /** Address column placeholder for pointer-derived nodes. */
    derivedAddress: string
    /** No ELF variables parsed at all. */
    noVariablesTitle: string
    /** Hint for the no-variables state. */
    noVariablesBody: string
    /** Variables exist but no global object root was found. */
    noRootsTitle: string
    /** Hint for the no-roots state. */
    noRootsBody: string
    /** Root exists but has no materialised children. */
    noRootChildren: string
    /** Invalid rootKey prop, `{name}` `{fallback}`. */
    invalidRoot: string
    /** Invalid rootKey prop and no fallback root exists, `{name}`. */
    invalidRootFallback: string
    /** Large root hint, `{count}`. */
    largeRoot: string
    /** Array elements capped by the parser, `{shown}` `{total}`. */
    arrayCapped: string
    /** Children truncated by the tree limits, `{shown}` `{total}`. */
    treeLimited: string
    /** Pagination button, `{remaining}`. */
    showMoreChildren: string
    /** Filters/search removed every row. */
    noRowsFiltered: string
    /** Field search returned nothing. */
    noSearchResults: string
    /** Search result count, `{count}`. */
    searchResultsCount: string
    /** Multi-select hint. */
    multiSelectHint: string
    /** Batch action found no eligible field. */
    batchEmpty: string
    /** Batch action was capped, `{max}` `{total}`. */
    batchTruncated: string
    /** Footer row count, `{count}`. */
    visibleRows: string
    /** Footer hint when the visible row list is capped. */
    rowsLimited: string
    /** Tree aria-label. */
    treeAriaLabel: string
    /** Root picker aria-label. */
    rootAriaLabel: string
    /** Search results are flat: reveal a match in the tree. */
    revealInTree: string
}

/** All default labels are English and free of CJK (i18n strict contract). */
export const DEFAULT_OBJECT_BROWSER_LABELS: ObjectBrowserLabels = {
    title: 'Objects',
    rootsLabel: 'Object roots',
    rootSearchPlaceholder: 'Search object roots',
    rootMenuEmpty: 'No matching object roots',
    rootMenuMore: '{shown} of {total} roots shown; refine the search',
    rootCount: '{count} roots',
    noRootSelected: 'Select a root',
    preferredBadge: 'preferred',
    pointerBadge: 'pointer',
    objectBadge: 'object',
    arrayBadge: 'array',
    searchPlaceholder: 'Search fields, paths or addresses',
    sampleableOnly: 'Sampleable only',
    fixedAddressOnly: 'Fixed address only',
    withValueOnly: 'With value only',
    expandAll: 'Expand all',
    collapseAll: 'Collapse all',
    expandField: 'Expand',
    collapseField: 'Collapse',
    watchSubtree: 'Watch subtree',
    watchVisible: 'Watch visible',
    watchSelected: 'Watch selected',
    addCurves: 'Add curve',
    addSelected: 'Add selected',
    selectedCount: '{count} marked',
    presetsLabel: 'Presets',
    presetSelect: 'Select preset',
    presetNamePlaceholder: 'Preset name',
    presetDefaultName: 'Object preset {count}',
    presetSave: 'Save',
    presetLoad: 'Load',
    presetDelete: 'Delete',
    presetEmpty: 'No presets',
    colName: 'Name',
    colType: 'Type',
    colAddress: 'Address',
    colValue: 'Value',
    sourceJump: 'Jump to source',
    noSourceLocation: 'No DWARF file/line for this field',
    watchAdd: 'Watch',
    watchRemove: 'Unwatch',
    noValue: '\u2014',
    staleValue: 'Value is stale',
    liveBadge: 'live',
    haltBadge: 'halt',
    derivedAddress: 'derived',
    noVariablesTitle: 'No ELF variables parsed',
    noVariablesBody: 'Load a non-stripped ELF (with DWARF) in the Variables tab, then browse its global object roots here.',
    noRootsTitle: 'No global object roots found',
    noRootsBody: 'Only fixed-address struct/class objects, pointers to objects and arrays with readable children are listed. Local variables and scalars are excluded.',
    noRootChildren: 'This root has no expandable fields in the parsed DWARF tree.',
    invalidRoot: 'Object root "{name}" was not found; showing "{fallback}".',
    invalidRootFallback: 'Object root "{name}" was not found and no fallback root is available.',
    largeRoot: 'Large object: at least {count} fields; only expanded branches are materialised.',
    arrayCapped: '{shown} of {total} elements materialised by the ELF parser.',
    treeLimited: 'Showing the first {shown} of {total} fields.',
    showMoreChildren: 'Show more ({remaining})',
    noRowsFiltered: 'No rows match the current filters.',
    noSearchResults: 'No matching fields.',
    searchResultsCount: '{count} matches',
    multiSelectHint: 'Ctrl/Cmd+click to mark rows, Shift+click for a range.',
    batchEmpty: 'No eligible field to watch or plot.',
    batchTruncated: 'Only the first {max} of {total} fields were used.',
    visibleRows: '{count} rows',
    rowsLimited: 'Row list limited; narrow the search or collapse branches.',
    treeAriaLabel: 'Object fields',
    rootAriaLabel: 'Object root',
    revealInTree: 'Show in tree',
}

function lookupLabel(value: unknown): string {
    return typeof value === 'string' ? value : ''
}

/**
 * Merge parent-provided (translated) strings over the English defaults.
 * Empty strings are treated as "not provided" so a partial i18n mapping cannot
 * blank the UI.
 */
export function resolveObjectBrowserLabels(
    overrides: Partial<ObjectBrowserLabels> | null | undefined,
): ObjectBrowserLabels {
    if (!overrides) return DEFAULT_OBJECT_BROWSER_LABELS
    const resolved: ObjectBrowserLabels = {...DEFAULT_OBJECT_BROWSER_LABELS}
    for (const key of Object.keys(DEFAULT_OBJECT_BROWSER_LABELS) as Array<keyof ObjectBrowserLabels>) {
        const value = lookupLabel(overrides[key])
        if (value) resolved[key] = value
    }
    return resolved
}

/**
 * Replace `{name}` placeholders in a label. Unknown placeholders are left as-is
 * (keeps contract violations visible during integration).
 */
export function formatObjectBrowserLabel(
    template: string,
    values: Readonly<Record<string, string | number>> = {},
): string {
    return String(template ?? '').replace(/\{([a-zA-Z_][a-zA-Z0-9_]*)\}/g, (match, name: string) => {
        if (!Object.prototype.hasOwnProperty.call(values, name)) return match
        return String(values[name])
    })
}
