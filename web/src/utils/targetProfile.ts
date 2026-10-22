/**
 * Target detection + MCU/board profile helpers (task-36, P2.16).
 *
 * The firmware only reports what SWD can actually observe: DBGMCU DEV_ID
 * (device group, 12 bit), FSIZE / flash capacity, CPUID core, UID and
 * DBGMCU IDCODE.  It can never read the package or the full order code
 * (e.g. the LQFP48 package of an STM32F103C8T6), so this module:
 *
 *  - parses `/api/target/info` v3 fields (`dev_id`, `flash_kib`,
 *    `model_candidates`, `core`, `detection_confidence`) with a fallback for
 *    older firmware that only reports `device` + `flash_size`;
 *  - derives the plausible order-code candidates from the candidate group and
 *    the FSIZE density hint (STM32F103x8 etc.);
 *  - maintains a list of known MCU/board profiles (order code, package,
 *    flashKib, ramRanges) and parses user-supplied custom profiles;
 *  - compares a *user-selected* profile against the detected group (the
 *    selection is never inferred automatically from detection);
 *  - persists the explicit selection and custom profiles in localStorage
 *    (tolerant: a missing/broken storage never breaks the page);
 *  - emits a compact profile object that DebugCurve can consume later for
 *    address validation, plus a diagnostics JSON for support/copy.
 *
 * Everything here is a pure function/type module (no DOM/network access) so
 * it can be unit-tested from Node: `node scripts/test-target-profile.mjs`.
 */

/** Detection confidence reported by the firmware (`detection_confidence`). */
export type DetectionConfidence = 'unknown' | 'group' | 'flash-narrowed'

/** Inclusive RAM/flash region: [start, start + size). */
export interface TargetRamRange {
    start: number
    size: number
}

/** Known or user-defined MCU/board profile (explicit user selection only). */
export interface TargetProfile {
    /** Stable id used for persistence; known profiles use `stm32f103c8t6`, custom ones `custom:<slug>`. */
    id: string
    /** Display name (known profiles: order code; custom: user label). */
    name: string
    /** MCU/order-code label, e.g. "STM32F103C8T6". */
    mcu: string
    /** Exact order code when known; equal to mcu for custom profiles. */
    orderCode: string
    /** STM32 series label, e.g. "STM32F1". */
    series: string
    /** Package label, e.g. "LQFP48"; empty string when unknown. */
    package: string
    /** Main flash size in KiB. */
    flashKib: number
    /** Expected DBGMCU DEV_ID (0 = not checked for custom profiles). */
    devId: number
    /** Known main RAM ranges (partial for custom profiles is fine). */
    ramRanges: TargetRamRange[]
    /** Where the profile came from. */
    source: 'known' | 'custom'
    /** Free-form English note (custom profiles only; UI shows it verbatim). */
    notes?: string
}

/** Minimal structural view of `/api/target/info` (old + v3 fields). */
export interface TargetDetectionInfo {
    detected?: boolean
    family?: string
    series?: string
    device?: string
    model_candidates?: string
    dev_id?: number
    flash_kib?: number
    flash_size?: number
    flash_base?: number
    core?: string
    detection_confidence?: DetectionConfidence | string
    supported?: boolean
    swd_clock_hz?: number
    cpuid?: number | string
    idcode?: number | string
    dpidr?: number | string
    uid?: string
    err?: string
    code?: number
    httpStatus?: number
}

/** Parsed detection view used by the UI/comparison helpers. */
export interface DetectedTarget {
    detected: boolean
    /** 12-bit DEV_ID (0 when unknown). */
    devId: number
    /** Flash size in KiB (0 when unknown). */
    flashKib: number
    /** Main flash base (0x08000000 unless the firmware reports otherwise). */
    flashBase: number
    /** Raw candidate string reported by the firmware. */
    group: string
    /** Candidate group without the trailing `[x8, 64 KiB]` hint. */
    baseGroup: string
    /** FSIZE hint code, e.g. "x8"; empty when unknown. */
    density: string
    /** Plausible order-code prefixes derived from the group + density. */
    candidates: string[]
    core: string
    series: string
    confidence: DetectionConfidence
    supported: boolean
}

/** Match level between a user-selected profile and the detected target. */
export type ProfileMatchLevel =
    | 'no-detection'
    | 'no-profile'
    | 'dev-id-mismatch'
    | 'flash-mismatch'
    | 'match'
    | 'unverified'

/** Machine-readable reasons/details behind a match level. */
export type ProfileMatchReason =
    | 'no-detection'
    | 'no-profile'
    | 'dev-id-unknown'
    | 'dev-id-mismatch'
    | 'flash-unknown'
    | 'flash-mismatch'
    | 'candidates-unavailable'
    | 'candidate-in-group'
    | 'candidate-not-in-group'
    | 'consistent'

export interface ProfileMatchResult {
    level: ProfileMatchLevel
    /** null = cannot be checked (missing data on either side). */
    devIdMatches: boolean | null
    flashMatches: boolean | null
    /** null = no candidate list available to check against. */
    candidateMatches: boolean | null
    reasons: ProfileMatchReason[]
}

/** Compact profile description intended for DebugCurve address validation. */
export interface CompactTargetProfile {
    source: 'none' | 'detected' | 'selected'
    mcu: string
    orderCode: string
    package: string
    series: string
    devId: number
    flashKib: number
    flashBase: number
    /** [start, end) of the main flash region, null when flashKib is unknown. */
    flashRange: [number, number] | null
    ramRanges: TargetRamRange[]
    confidence: DetectionConfidence
    supported: boolean
}

/** Detection limitations the UI must state explicitly (SWD cannot answer them). */
export type DetectionLimitationCode =
    | 'no-detection'
    | 'dev-id-unknown'
    | 'dev-id-group'
    | 'flash-size-unknown'
    | 'swd-no-package'
    | 'flash-unsupported'
    | 'profile-not-selected'
    | 'profile-dev-id-mismatch'
    | 'profile-flash-mismatch'
    | 'profile-order-code-mismatch'
    | 'profile-unverified'

/** Minimal localStorage-like interface (injectable for tests). */
export interface ProfileStorageLike {
    getItem(key: string): string | null
    setItem(key: string, value: string): void
    removeItem(key: string): void
}

export interface CustomProfileParseResult {
    ok: boolean
    profile: TargetProfile | null
    error?: string
}

export interface RestoredTargetProfiles {
    customProfiles: TargetProfile[]
    /** Profile restored from the last *explicit* user selection (never inferred). */
    selectedProfile: TargetProfile | null
    storageError?: string
}

export interface TargetDiagnosticsInput {
    detection: TargetDetectionInfo | null
    selectedProfile: TargetProfile | null
    /** ISO timestamp, injected by the caller so the helper stays pure. */
    generatedAt?: string
    appVersion?: string
    firmwareInfo?: Record<string, unknown> | null
}

/** Default STM32 main-flash base when the firmware does not report one. */
export const TARGET_FLASH_BASE_DEFAULT = 0x08000000

/** localStorage keys (v1: shape may evolve, so they are versioned). */
export const TARGET_PROFILE_STORAGE_KEYS = {
    selectedProfileId: 'birdiebot.target.selectedProfileId.v1',
    customProfiles: 'birdiebot.target.customProfiles.v1',
} as const

/** English labels for the confidence enum (temporary fallback until i18n keys land). */
export const CONFIDENCE_LABEL_EN: Record<DetectionConfidence, string> = {
    unknown: 'Unknown',
    group: 'Device group only',
    'flash-narrowed': 'Flash-size narrowed',
}

/** English fallback texts for the detection limitations (UI shows these verbatim). */
export const LIMITATION_TEXT_EN: Record<DetectionLimitationCode, string> = {
    'no-detection': 'No STM32 target answered on SWD.',
    'dev-id-unknown': 'DBGMCU DEV_ID is not in the known table; series/model cannot be resolved.',
    'dev-id-group': 'DEV_ID only resolves a device group; the exact order code needs confirmation.',
    'flash-size-unknown': 'FSIZE could not be read; only the DEV_ID device group is known.',
    'swd-no-package':
        'Package and full order code cannot be read over SWD (e.g. LQFP48 for STM32F103C8T6).',
    'flash-unsupported':
        'Detected series is identify/curve-only in this firmware; Web flashing supports STM32F4/H7 only.',
    'profile-not-selected':
        'No MCU profile selected: select a known/custom profile to label an exact order code (comparison only).',
    'profile-dev-id-mismatch': 'Selected profile DEV_ID does not match the detected DEV_ID.',
    'profile-flash-mismatch': 'Selected profile flash size does not match the detected FSIZE.',
    'profile-order-code-mismatch':
        'Selected profile order code is not a candidate of the detected device group.',
    'profile-unverified': 'Selected profile cannot be fully verified from SWD data alone.',
}

/** English fallback texts for the profile match levels/reasons. */
export const MATCH_TEXT_EN: Record<ProfileMatchLevel, string> = {
    'no-detection': 'Detect the target first to compare a profile.',
    'no-profile': 'No profile selected (detection never auto-selects one).',
    'dev-id-mismatch': 'Selected profile DEV_ID does not match the detected target.',
    'flash-mismatch': 'Selected profile flash size does not match the detected FSIZE.',
    match: 'Selected profile is consistent with the detected DEV_ID and flash size.',
    unverified: 'Selected profile is only partially consistent with the detection.',
}

/**
 * Known MCU/board profiles.  Values follow the ST datasheets for the classic
 * order codes below; `ramRanges` list the main regions only (bus aliases and
 * backup SRAM are intentionally omitted).
 */
export const KNOWN_TARGET_PROFILES: readonly TargetProfile[] = Object.freeze([
    {
        id: 'stm32f103c8t6',
        name: 'STM32F103C8T6',
        mcu: 'STM32F103C8T6',
        orderCode: 'STM32F103C8T6',
        series: 'STM32F1',
        package: 'LQFP48',
        flashKib: 64,
        devId: 0x410,
        ramRanges: [{start: 0x20000000, size: 20 * 1024}],
        source: 'known',
    },
    {
        id: 'stm32f103cbt6',
        name: 'STM32F103CBT6',
        mcu: 'STM32F103CBT6',
        orderCode: 'STM32F103CBT6',
        series: 'STM32F1',
        package: 'LQFP48',
        flashKib: 128,
        devId: 0x410,
        ramRanges: [{start: 0x20000000, size: 20 * 1024}],
        source: 'known',
    },
    {
        id: 'stm32f103rct6',
        name: 'STM32F103RCT6',
        mcu: 'STM32F103RCT6',
        orderCode: 'STM32F103RCT6',
        series: 'STM32F1',
        package: 'LQFP64',
        flashKib: 256,
        devId: 0x414,
        ramRanges: [{start: 0x20000000, size: 48 * 1024}],
        source: 'known',
    },
    {
        id: 'stm32f103zet6',
        name: 'STM32F103ZET6',
        mcu: 'STM32F103ZET6',
        orderCode: 'STM32F103ZET6',
        series: 'STM32F1',
        package: 'LQFP144',
        flashKib: 512,
        devId: 0x414,
        ramRanges: [{start: 0x20000000, size: 64 * 1024}],
        source: 'known',
    },
    {
        id: 'stm32f407vgt6',
        name: 'STM32F407VGT6',
        mcu: 'STM32F407VGT6',
        orderCode: 'STM32F407VGT6',
        series: 'STM32F4',
        package: 'LQFP100',
        flashKib: 1024,
        devId: 0x413,
        ramRanges: [
            {start: 0x20000000, size: 128 * 1024},
            {start: 0x10000000, size: 64 * 1024},
        ],
        source: 'known',
    },
    {
        id: 'stm32f429igt6',
        name: 'STM32F429IGT6',
        mcu: 'STM32F429IGT6',
        orderCode: 'STM32F429IGT6',
        series: 'STM32F4',
        package: 'LQFP176',
        flashKib: 1024,
        devId: 0x419,
        ramRanges: [
            {start: 0x20000000, size: 192 * 1024},
            {start: 0x10000000, size: 64 * 1024},
        ],
        source: 'known',
    },
    {
        id: 'stm32h723vgt6',
        name: 'STM32H723VGT6',
        mcu: 'STM32H723VGT6',
        orderCode: 'STM32H723VGT6',
        series: 'STM32H7',
        package: 'LQFP100',
        flashKib: 1024,
        devId: 0x483,
        ramRanges: [
            {start: 0x20000000, size: 128 * 1024},
            {start: 0x24000000, size: 320 * 1024},
        ],
        source: 'known',
    },
    {
        id: 'stm32h743vit6',
        name: 'STM32H743VIT6',
        mcu: 'STM32H743VIT6',
        orderCode: 'STM32H743VIT6',
        series: 'STM32H7',
        package: 'LQFP100',
        flashKib: 2048,
        devId: 0x450,
        ramRanges: [
            {start: 0x20000000, size: 128 * 1024},
            {start: 0x24000000, size: 512 * 1024},
        ],
        source: 'known',
    },
])

/** STM32 flash-density letters for the classic order-code convention. */
const FLASH_DENSITY_BY_KIB: Readonly<Record<number, string>> = Object.freeze({
    16: 'x4',
    32: 'x6',
    64: 'x8',
    128: 'xB',
    256: 'xC',
    384: 'xD',
    512: 'xE',
    768: 'xF',
    1024: 'xG',
    2048: 'xI',
})

const MAX_DERIVED_CANDIDATES = 32
const MAX_RAM_RANGES = 16
const MAX_PROFILE_FLASH_KIB = 16384

// ---------------------------------------------------------------------------
// small parsing/formatting helpers
// ---------------------------------------------------------------------------

/** Format a number as an 8-digit uppercase 0x hex string. */
export function formatHex32(value: number | null | undefined): string {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
        return '-'
    }
    return `0x${Math.trunc(value).toString(16).toUpperCase().padStart(8, '0')}`
}

/** Format a 12-bit DEV_ID as `0x410` (3 hex digits). */
export function formatDevId(devId: number | null | undefined): string {
    if (typeof devId !== 'number' || !Number.isInteger(devId) || devId <= 0) {
        return '-'
    }
    return `0x${devId.toString(16).toUpperCase().padStart(3, '0')}`
}

/** Human-readable flash size, e.g. `1024 KiB (1 MiB)`. */
export function formatFlashKib(flashKib: number | null | undefined): string {
    if (typeof flashKib !== 'number' || !Number.isFinite(flashKib) || flashKib <= 0) {
        return '-'
    }
    if (flashKib >= 1024 && flashKib % 1024 === 0) {
        return `${flashKib} KiB (${flashKib / 1024} MiB)`
    }
    return `${flashKib} KiB`
}

/** Format one RAM range as `0x20000000-0x20005000 (20 KiB)`. */
export function formatRamRange(range: TargetRamRange): string {
    const start = range.start >>> 0
    const end = (start + range.size) >>> 0
    return `${formatHex32(start)}-${formatHex32(end)} (${formatKib(range.size)})`
}

function formatKib(size: number): string {
    const kib = size / 1024
    return Number.isInteger(kib) ? `${kib} KiB` : `${size} B`
}

/** Format all RAM ranges of a profile on separate lines (empty string when none). */
export function formatProfileRamRanges(profile: TargetProfile | null | undefined): string[] {
    if (!profile || profile.ramRanges.length === 0) {
        return []
    }
    return profile.ramRanges.map(formatRamRange)
}

/** Flash range [start, end) from a KiB size, or null when the size is unknown. */
export function flashRangeOf(flashKib: number | null | undefined, flashBase = TARGET_FLASH_BASE_DEFAULT): [number, number] | null {
    if (typeof flashKib !== 'number' || !Number.isFinite(flashKib) || flashKib <= 0) {
        return null
    }
    const base = flashBase >>> 0
    return [base, (base + Math.trunc(flashKib) * 1024) >>> 0]
}

/** Density code ("x8", "xB", ...) for a flash size in KiB; empty when non-standard. */
export function flashDensityCode(flashKib: number | null | undefined): string {
    if (typeof flashKib !== 'number' || !Number.isInteger(flashKib)) {
        return ''
    }
    return FLASH_DENSITY_BY_KIB[flashKib] ?? ''
}

/** Tolerant `detection_confidence` parser (old firmware/unknown values -> 'unknown'). */
export function parseDetectionConfidence(value: unknown): DetectionConfidence {
    switch (typeof value === 'string' ? value.trim().toLowerCase() : value) {
        case 'flash-narrowed':
        case 'flash_narrowed':
        case 'flash':
        case 'narrowed':
            return 'flash-narrowed'
        case 'group':
        case 'device-group':
            return 'group'
        default:
            return 'unknown'
    }
}

function toFiniteNumber(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return value
    }
    if (typeof value === 'string' && value.trim() !== '') {
        const parsed = Number(value)
        return Number.isFinite(parsed) ? parsed : null
    }
    return null
}

function toPositiveInt(value: unknown, fallback = 0): number {
    const num = toFiniteNumber(value)
    if (num === null) {
        return fallback
    }
    const int = Math.trunc(num)
    return int > 0 ? int : fallback
}

// ---------------------------------------------------------------------------
// detection parsing: DEV_ID + FSIZE -> candidate group / order codes
// ---------------------------------------------------------------------------

function normalizeOrderCode(value: string): string {
    return value.trim().toUpperCase().replace(/[^A-Z0-9-]/g, '')
}

/** Split "STM32F100/F101/F102/F103 (MD)" into the model list and the note. */
function splitGroupBase(baseGroup: string): {models: string, note: string} {
    const match = /^(.*?)\s*\(([^()]*)\)\s*$/.exec(baseGroup)
    if (match) {
        return {models: match[1].trim(), note: match[2].trim()}
    }
    return {models: baseGroup.trim(), note: ''}
}

/** "F101" -> "STM32F101"; "STM32F100" stays as is. */
function toFullModel(token: string): string {
    const normalized = normalizeOrderCode(token)
    if (!normalized) {
        return ''
    }
    return normalized.startsWith('STM32') ? normalized : `STM32${normalized}`
}

/** Expand "STM32F476-F479" (or "STM32F476-STM32F479") into the full list. */
function expandRangeModel(fullModel: string): string[] {
    const dashIndex = fullModel.indexOf('-')
    if (dashIndex < 0) {
        return [fullModel]
    }
    const left = fullModel.slice(0, dashIndex)
    const right = fullModel.slice(dashIndex + 1)
    const leftMatch = /^(.*?)(\d+)$/.exec(left)
    const rightMatch = /^(.*?)(\d+)$/.exec(right)
    if (!leftMatch || !rightMatch) {
        return [fullModel]
    }
    const leftLetters = leftMatch[1]
    const start = Number(leftMatch[2])
    const rightLetters = rightMatch[1]
    const end = Number(rightMatch[2])
    if (!Number.isInteger(start) || !Number.isInteger(end) || end < start || end - start > MAX_DERIVED_CANDIDATES) {
        return [fullModel]
    }
    // The right side usually repeats the family prefix letter ("F476-F479") or
    // the full prefix ("STM32F476-STM32F479"); drop that duplication.
    let suffix = ''
    if (rightLetters !== leftLetters && rightLetters !== '' && !rightLetters.toUpperCase().endsWith(leftLetters.slice(-1).toUpperCase())) {
        suffix = rightLetters
    }
    const out: string[] = []
    for (let index = start; index <= end; index += 1) {
        out.push(`${leftLetters}${index}${suffix}`)
    }
    return out
}

/**
 * Derive plausible order-code prefixes from a candidate group and the FSIZE
 * density code.  Example: "STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]"
 * -> ["STM32F100x8", "STM32F101x8", "STM32F102x8", "STM32F103x8"].
 */
export function deriveOrderCodeCandidates(groupText: string | null | undefined, flashKib = 0): string[] {
    if (typeof groupText !== 'string' || groupText.trim() === '') {
        return []
    }
    const text = groupText.trim()
    const hintMatch = /\s*\[([^\]]*)\]\s*$/.exec(text)
    const hint = hintMatch ? hintMatch[1].trim() : ''
    const baseGroup = hintMatch ? text.slice(0, hintMatch.index).trim() : text
    const {models, note} = splitGroupBase(baseGroup)
    if (!models || /^unknown/i.test(models)) {
        return []
    }
    let density = ''
    for (const part of hint.split(',')) {
        const candidate = part.trim()
        if (/^x[0-9a-z]$/i.test(candidate)) {
            density = `x${candidate.slice(1).toUpperCase()}`
            break
        }
    }
    if (!density) {
        // Only fall back to the FSIZE-derived letter when the firmware did not
        // send a bracket hint at all; "[512 KiB]" deliberately omits the letter
        // for families where the order-code density convention does not apply.
        density = hintMatch ? '' : flashDensityCode(flashKib)
    }
    const out: string[] = []
    for (const token of models.split('/')) {
        const full = toFullModel(token)
        if (!full) {
            continue
        }
        for (const model of expandRangeModel(full)) {
            out.push(density ? `${model}${density}` : model)
            if (out.length >= MAX_DERIVED_CANDIDATES) {
                return out
            }
        }
    }
    if (out.length === 0 && note) {
        return []
    }
    return out
}

/**
 * True when `orderCode` (e.g. STM32F103C8T6) is compatible with a candidate
 * pattern (e.g. STM32F103x8, where 'x' matches one package letter).
 */
export function orderCodeMatchesCandidate(orderCode: string | null | undefined, candidate: string | null | undefined): boolean {
    if (typeof orderCode !== 'string' || typeof candidate !== 'string') {
        return false
    }
    const code = normalizeOrderCode(orderCode)
    const pattern = normalizeOrderCode(candidate)
    if (!code || !pattern) {
        return false
    }
    let regexSource = '^'
    for (const char of pattern) {
        if (char === 'X') {
            regexSource += '[A-Z]'
        } else if ('\\^$.|?*+()[]{}'.includes(char)) {
            regexSource += `\\${char}`
        } else {
            regexSource += char
        }
    }
    try {
        return new RegExp(regexSource).test(code)
    } catch {
        return false
    }
}

/** Parse `/api/target/info` (old and v3 fields) into the UI detection view. */
export function parseTargetDetection(info: TargetDetectionInfo | null | undefined): DetectedTarget {
    const detected = Boolean(info?.detected)
    const devIdRaw = toFiniteNumber(info?.dev_id)
    const flashKibRaw = toFiniteNumber(info?.flash_kib)
    const flashSize = toFiniteNumber(info?.flash_size)
    const flashKib = flashKibRaw !== null && flashKibRaw > 0
        ? Math.trunc(flashKibRaw)
        : (flashSize !== null && flashSize > 0 ? Math.round(flashSize / 1024) : 0)
    const group = typeof info?.model_candidates === 'string' && info.model_candidates.trim() !== ''
        ? info.model_candidates.trim()
        : (typeof info?.device === 'string' ? info.device.trim() : '')
    const hintMatch = /\s*\[([^\]]*)\]\s*$/.exec(group)
    const baseGroup = hintMatch ? group.slice(0, hintMatch.index).trim() : group
    const hint = hintMatch ? hintMatch[1] : ''
    const densityMatch = /\bx([0-9a-z])\b/i.exec(hint)
    const density = densityMatch ? `x${densityMatch[1].toUpperCase()}` : ''
    const flashBase = toPositiveInt(info?.flash_base, TARGET_FLASH_BASE_DEFAULT)
    const supported = typeof info?.supported === 'boolean' ? info.supported : false
    let confidence = parseDetectionConfidence(info?.detection_confidence)
    if (confidence === 'unknown' && detected) {
        // Older firmware (<v3) has no detection_confidence field: infer a
        // conservative level from the raw fields it does report.
        if (devIdRaw !== null && devIdRaw > 0) {
            confidence = flashKib > 0 ? 'flash-narrowed' : 'group'
        } else if (group && !/^unknown/i.test(group)) {
            confidence = 'group'
        }
    }
    return {
        detected,
        devId: devIdRaw !== null && devIdRaw > 0 ? Math.trunc(devIdRaw) & 0xfff : 0,
        flashKib,
        flashBase: flashBase > 0 ? flashBase : TARGET_FLASH_BASE_DEFAULT,
        group: detected ? group : '',
        baseGroup: detected ? baseGroup : '',
        density,
        candidates: detected ? deriveOrderCodeCandidates(group, flashKib) : [],
        core: typeof info?.core === 'string' && info.core.trim() !== '' ? info.core.trim() : 'unknown',
        series: typeof info?.series === 'string' && info.series.trim() !== '' ? info.series.trim() : 'unknown',
        confidence,
        supported,
    }
}

// ---------------------------------------------------------------------------
// profile lookup / comparison
// ---------------------------------------------------------------------------

/** Find a known profile by id. */
export function findKnownTargetProfile(id: string | null | undefined): TargetProfile | null {
    if (!id) {
        return null
    }
    return KNOWN_TARGET_PROFILES.find((profile) => profile.id === id) ?? null
}

/** All selectable profiles: known list first, then custom ones. */
export function allTargetProfiles(customProfiles: readonly TargetProfile[] = []): TargetProfile[] {
    return [...KNOWN_TARGET_PROFILES, ...customProfiles]
}

/**
 * Compare the detected target with a user-selected profile.
 * The profile is never inferred from the detection; it must be selected.
 */
export function compareDetectedToProfile(
    info: TargetDetectionInfo | DetectedTarget | null | undefined,
    profile: TargetProfile | null | undefined,
): ProfileMatchResult {
    const detected = isDetectedTarget(info) ? info : parseTargetDetection(info)
    const reasons: ProfileMatchReason[] = []
    if (!detected.detected) {
        reasons.push('no-detection')
        return {level: 'no-detection', devIdMatches: null, flashMatches: null, candidateMatches: null, reasons}
    }
    if (!profile) {
        reasons.push('no-profile')
        return {level: 'no-profile', devIdMatches: null, flashMatches: null, candidateMatches: null, reasons}
    }
    let devIdMatches: boolean | null = null
    if (detected.devId > 0 && profile.devId > 0) {
        devIdMatches = detected.devId === profile.devId
        reasons.push(devIdMatches ? 'consistent' : 'dev-id-mismatch')
        if (!devIdMatches) {
            return {level: 'dev-id-mismatch', devIdMatches, flashMatches: null, candidateMatches: null, reasons}
        }
    } else {
        reasons.push('dev-id-unknown')
    }
    let flashMatches: boolean | null = null
    if (detected.flashKib > 0 && profile.flashKib > 0) {
        flashMatches = detected.flashKib === profile.flashKib
        if (!flashMatches) {
            reasons.push('flash-mismatch')
            return {level: 'flash-mismatch', devIdMatches, flashMatches, candidateMatches: null, reasons}
        }
    } else {
        reasons.push('flash-unknown')
    }
    let candidateMatches: boolean | null = null
    const orderCode = profile.orderCode || profile.mcu
    if (detected.candidates.length > 0 && orderCode) {
        candidateMatches = detected.candidates.some((candidate) => orderCodeMatchesCandidate(orderCode, candidate))
        reasons.push(candidateMatches ? 'candidate-in-group' : 'candidate-not-in-group')
        if (!candidateMatches) {
            return {level: 'unverified', devIdMatches, flashMatches, candidateMatches, reasons}
        }
    } else {
        reasons.push('candidates-unavailable')
    }
    if (devIdMatches === true && flashMatches === true) {
        return {level: 'match', devIdMatches, flashMatches, candidateMatches, reasons}
    }
    return {level: 'unverified', devIdMatches, flashMatches, candidateMatches, reasons}
}

function isDetectedTarget(value: unknown): value is DetectedTarget {
    return typeof value === 'object'
        && value !== null
        && 'devId' in value
        && 'candidates' in value
        && 'confidence' in value
}

/** Limitations the UI must state explicitly (package/order code never verifiable). */
export function detectionLimitations(
    info: TargetDetectionInfo | DetectedTarget | null | undefined,
    profile: TargetProfile | null | undefined,
): DetectionLimitationCode[] {
    const detected = isDetectedTarget(info) ? info : parseTargetDetection(info)
    const out: DetectionLimitationCode[] = []
    if (!detected.detected) {
        return ['no-detection']
    }
    if (detected.devId === 0) {
        out.push('dev-id-unknown')
    } else if (detected.confidence === 'group') {
        out.push('dev-id-group')
    }
    if (detected.flashKib === 0) {
        out.push('flash-size-unknown')
    }
    // Package/order code are never observable over SWD, even with a profile.
    out.push('swd-no-package')
    if (!detected.supported) {
        out.push('flash-unsupported')
    }
    if (!profile) {
        out.push('profile-not-selected')
        return out
    }
    const match = compareDetectedToProfile(detected, profile)
    switch (match.level) {
        case 'dev-id-mismatch':
            out.push('profile-dev-id-mismatch')
            break
        case 'flash-mismatch':
            out.push('profile-flash-mismatch')
            break
        case 'unverified':
            if (match.candidateMatches === false) {
                out.push('profile-order-code-mismatch')
            } else {
                out.push('profile-unverified')
            }
            break
        case 'match':
        case 'no-detection':
        case 'no-profile':
        default:
            break
    }
    return out
}

// ---------------------------------------------------------------------------
// custom profile parsing / persistence
// ---------------------------------------------------------------------------

function parseAddressValue(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value)) {
        const addr = Math.trunc(value)
        return addr >= 0 ? addr : null
    }
    if (typeof value === 'string') {
        const text = value.trim()
        if (!text) {
            return null
        }
        const parsed = /^0x[0-9a-f]+$/i.test(text) ? Number.parseInt(text, 16) : Number(text)
        return Number.isFinite(parsed) && parsed >= 0 ? Math.trunc(parsed) : null
    }
    return null
}

/** Parse a size in bytes: number, "20480", "20KiB", "0x5000", "1MiB". */
function parseSizeValue(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value) && value > 0) {
        return Math.trunc(value)
    }
    if (typeof value !== 'string') {
        return null
    }
    const text = value.trim().replace(/[_ ]/g, '')
    const match = /^(0x[0-9a-f]+|[0-9]+(?:\.[0-9]+)?)(ki?b?|mi?b?|k|m|b)?$/i.exec(text)
    if (!match) {
        return null
    }
    const number = /^0x/i.test(match[1]) ? Number.parseInt(match[1], 16) : Number(match[1])
    if (!Number.isFinite(number) || number <= 0) {
        return null
    }
    const unit = (match[2] ?? '').toLowerCase()
    let multiplier = 1
    if (unit === 'k' || unit === 'kb' || unit === 'kib') {
        multiplier = 1024
    } else if (unit === 'm' || unit === 'mb' || unit === 'mib') {
        multiplier = 1024 * 1024
    }
    const size = Math.trunc(number * multiplier)
    return Number.isFinite(size) && size > 0 ? size : null
}

/**
 * Parse one RAM range.  Accepted forms:
 *  - {start: 0x20000000, size: 20480} / {start: "...", size: "20KiB"}
 *  - {start: 0x20000000, end: 0x20005000}  (end exclusive)
 *  - "0x20000000-0x20005000"               (end exclusive)
 *  - "0x20000000+20KiB"                    (start + size)
 */
export function parseRamRangeValue(value: unknown): TargetRamRange | null {
    if (typeof value === 'string') {
        const text = value.trim()
        if (!text) {
            return null
        }
        if (text.includes('+')) {
            const [startText, sizeText] = text.split('+', 2)
            const start = parseAddressValue(startText)
            const size = parseSizeValue(sizeText)
            return start !== null && size !== null ? {start, size} : null
        }
        const dashIndex = text.indexOf('-')
        if (dashIndex > 0) {
            const start = parseAddressValue(text.slice(0, dashIndex))
            const end = parseAddressValue(text.slice(dashIndex + 1))
            if (start === null || end === null || end <= start) {
                return null
            }
            return {start, size: end - start}
        }
        return null
    }
    if (typeof value === 'object' && value !== null) {
        const record = value as Record<string, unknown>
        const start = parseAddressValue(record.start)
        if (start === null || start === 0) {
            return null
        }
        if (record.size !== undefined && record.size !== null && record.size !== '') {
            const size = parseSizeValue(record.size)
            return size !== null ? {start, size} : null
        }
        const end = parseAddressValue(record.end)
        if (end === null || end <= start) {
            return null
        }
        return {start, size: end - start}
    }
    return null
}

function normalizeRamRanges(values: unknown): TargetRamRange[] | string {
    if (values === undefined || values === null || values === '') {
        return []
    }
    if (!Array.isArray(values)) {
        return 'ramRanges must be an array'
    }
    if (values.length > MAX_RAM_RANGES) {
        return `ramRanges must contain at most ${MAX_RAM_RANGES} ranges`
    }
    const ranges: TargetRamRange[] = []
    for (const value of values) {
        const range = parseRamRangeValue(value)
        if (!range) {
            return 'invalid RAM range (use "0x20000000-0x20005000" or {"start":"0x20000000","size":"20KiB"})'
        }
        if (range.start + range.size > 0x100000000) {
            return 'RAM range exceeds the 32-bit address space'
        }
        ranges.push(range)
    }
    ranges.sort((left, right) => left.start - right.start)
    for (let index = 1; index < ranges.length; index += 1) {
        const previous = ranges[index - 1]
        if (ranges[index].start < previous.start + previous.size) {
            return 'RAM ranges must not overlap'
        }
    }
    return ranges
}

/** Derive the series label ("STM32F1", "STM32H7", "STM32WB", ...) from an order code. */
export function deriveSeriesFromOrderCode(orderCode: string | null | undefined): string {
    if (typeof orderCode !== 'string') {
        return ''
    }
    const code = normalizeOrderCode(orderCode)
    const twoLetter = /^STM32([A-Z]{2})/.exec(code)
    if (twoLetter) {
        return `STM32${twoLetter[1]}`
    }
    const oneLetter = /^STM32([A-Z]\d)/.exec(code)
    if (oneLetter) {
        return `STM32${oneLetter[1]}`
    }
    return ''
}

function slugify(value: string): string {
    return value
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, '-')
        .replace(/^-+|-+$/g, '')
        .slice(0, 48)
}

function normalizeProfileObject(raw: unknown): CustomProfileParseResult {
    if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) {
        return {ok: false, profile: null, error: 'profile must be a JSON object'}
    }
    const record = raw as Record<string, unknown>
    const mcu = typeof record.mcu === 'string' ? record.mcu.trim() : ''
    const orderCodeRaw = typeof record.orderCode === 'string' ? record.orderCode.trim() : ''
    const nameRaw = typeof record.name === 'string' ? record.name.trim() : ''
    const mcuValue = mcu || orderCodeRaw || nameRaw
    if (!mcuValue) {
        return {ok: false, profile: null, error: 'mcu/order code is required'}
    }
    if (mcuValue.length > 64) {
        return {ok: false, profile: null, error: 'mcu must be at most 64 characters'}
    }
    const name = (nameRaw || mcuValue).slice(0, 64)
    const orderCode = (orderCodeRaw || mcuValue).toUpperCase().slice(0, 64)
    const packageValue = typeof record.package === 'string' ? record.package.trim().slice(0, 32) : ''
    const flashKib = toPositiveInt(record.flashKib ?? record.flash_kib, 0)
    if (flashKib <= 0 || flashKib > MAX_PROFILE_FLASH_KIB) {
        return {ok: false, profile: null, error: `flashKib must be 1..${MAX_PROFILE_FLASH_KIB}`}
    }
    const ramRanges = normalizeRamRanges(record.ramRanges ?? record.ram_ranges)
    if (typeof ramRanges === 'string') {
        return {ok: false, profile: null, error: ramRanges}
    }
    const devIdRaw = record.devId ?? record.dev_id
    let devId = 0
    if (devIdRaw !== undefined && devIdRaw !== null && devIdRaw !== '') {
        const parsed = parseAddressValue(devIdRaw)
        if (parsed === null || parsed > 0xfff) {
            return {ok: false, profile: null, error: 'devId must be a 12-bit value (0x000-0xFFF)'}
        }
        devId = parsed
    }
    const series = typeof record.series === 'string' && record.series.trim() !== ''
        ? record.series.trim().slice(0, 16)
        : deriveSeriesFromOrderCode(orderCode)
    const notes = typeof record.notes === 'string' ? record.notes.trim().slice(0, 200) : undefined
    const rawId = typeof record.id === 'string' ? record.id.trim() : ''
    const id = rawId.startsWith('custom:') && rawId.length > 7
        ? rawId.slice(0, 80)
        : `custom:${slugify(name) || slugify(mcuValue) || 'profile'}`
    return {
        ok: true,
        profile: {
            id,
            name,
            mcu: mcuValue,
            orderCode,
            series,
            package: packageValue,
            flashKib,
            devId,
            ramRanges,
            source: 'custom',
            ...(notes ? {notes} : {}),
        },
    }
}

/**
 * Parse the custom-profile JSON text entered by the user.
 * Required: mcu (or orderCode), flashKib.  Optional: name, package,
 * ramRanges, devId, series, notes.
 */
export function parseCustomTargetProfileJson(text: string | null | undefined): CustomProfileParseResult {
    if (typeof text !== 'string' || text.trim() === '') {
        return {ok: false, profile: null, error: 'profile JSON is empty'}
    }
    let raw: unknown
    try {
        raw = JSON.parse(text)
    } catch (error) {
        const message = error instanceof Error ? error.message : 'invalid JSON'
        return {ok: false, profile: null, error: `invalid JSON: ${message}`}
    }
    return normalizeProfileObject(raw)
}

/** Normalize a stored custom profile (tolerant; returns null when invalid). */
export function normalizeStoredTargetProfile(raw: unknown): TargetProfile | null {
    const parsed = normalizeProfileObject(raw)
    return parsed.ok ? parsed.profile : null
}

/** Insert or replace a custom profile (by id) in a copy of the list. */
export function upsertCustomProfile(list: readonly TargetProfile[], profile: TargetProfile): TargetProfile[] {
    const index = list.findIndex((item) => item.id === profile.id)
    if (index < 0) {
        return [...list, profile]
    }
    const next = [...list]
    next[index] = profile
    return next
}

/** Remove a custom profile (by id) in a copy of the list. */
export function removeCustomProfile(list: readonly TargetProfile[], id: string): TargetProfile[] {
    return list.filter((profile) => profile.id !== id)
}

function defaultStorage(): ProfileStorageLike | null {
    try {
        if (typeof globalThis === 'undefined' || !('localStorage' in globalThis)) {
            return null
        }
        const storage = (globalThis as unknown as {localStorage?: ProfileStorageLike}).localStorage
        return storage ?? null
    } catch {
        return null
    }
}

/** Load the persisted custom profiles (broken storage/JSON -> empty list). */
export function loadCustomProfiles(storage: ProfileStorageLike | null = defaultStorage()): TargetProfile[] {
    if (!storage) {
        return []
    }
    try {
        const text = storage.getItem(TARGET_PROFILE_STORAGE_KEYS.customProfiles)
        if (!text) {
            return []
        }
        const parsed: unknown = JSON.parse(text)
        if (!Array.isArray(parsed)) {
            return []
        }
        const profiles: TargetProfile[] = []
        for (const item of parsed) {
            const profile = normalizeStoredTargetProfile(item)
            if (profile && !profiles.some((known) => known.id === profile.id)) {
                profiles.push(profile)
            }
        }
        return profiles
    } catch {
        return []
    }
}

/** Persist the custom profiles; returns false when storage is unavailable. */
export function saveCustomProfiles(profiles: readonly TargetProfile[], storage: ProfileStorageLike | null = defaultStorage()): boolean {
    if (!storage) {
        return false
    }
    try {
        storage.setItem(TARGET_PROFILE_STORAGE_KEYS.customProfiles, JSON.stringify(profiles))
        return true
    } catch {
        return false
    }
}

/** Load the id of the last *explicitly selected* profile (never auto-inferred). */
export function loadSelectedProfileId(storage: ProfileStorageLike | null = defaultStorage()): string | null {
    if (!storage) {
        return null
    }
    try {
        const value = storage.getItem(TARGET_PROFILE_STORAGE_KEYS.selectedProfileId)
        return value && value.trim() !== '' ? value : null
    } catch {
        return null
    }
}

/** Persist (or clear, with null) the explicitly selected profile id. */
export function saveSelectedProfileId(id: string | null, storage: ProfileStorageLike | null = defaultStorage()): boolean {
    if (!storage) {
        return false
    }
    try {
        if (id && id.trim() !== '') {
            storage.setItem(TARGET_PROFILE_STORAGE_KEYS.selectedProfileId, id)
        } else {
            storage.removeItem(TARGET_PROFILE_STORAGE_KEYS.selectedProfileId)
        }
        return true
    } catch {
        return false
    }
}

/**
 * Restore custom profiles + the explicitly selected profile.  Detection never
 * chooses a profile: an unknown/stale selection simply resolves to null.
 */
export function resolveRestoredTargetProfiles(
    storage: ProfileStorageLike | null = defaultStorage(),
): RestoredTargetProfiles {
    const customProfiles = loadCustomProfiles(storage)
    const selectedId = loadSelectedProfileId(storage)
    let selectedProfile: TargetProfile | null = null
    if (selectedId) {
        selectedProfile = findKnownTargetProfile(selectedId)
            ?? customProfiles.find((profile) => profile.id === selectedId)
            ?? null
    }
    return {customProfiles, selectedProfile}
}

// ---------------------------------------------------------------------------
// compact API + diagnostics (for DebugCurve / copy support)
// ---------------------------------------------------------------------------

/**
 * Compact profile description for later DebugCurve consumption (address
 * validation, flash/RAM display).  The selected profile wins for the exact
 * order code; detection contributes DEV_ID/confidence when no profile is set.
 */
export function compactTargetProfileApi(
    info: TargetDetectionInfo | DetectedTarget | null | undefined,
    profile: TargetProfile | null | undefined,
): CompactTargetProfile {
    const detected = isDetectedTarget(info) ? info : parseTargetDetection(info)
    const flashKib = profile?.flashKib ?? detected.flashKib
    const flashBase = detected.flashBase || TARGET_FLASH_BASE_DEFAULT
    const source: CompactTargetProfile['source'] = profile ? 'selected' : (detected.detected ? 'detected' : 'none')
    return {
        source,
        mcu: profile?.mcu ?? (detected.detected ? detected.baseGroup : ''),
        orderCode: profile?.orderCode ?? '',
        package: profile?.package ?? '',
        series: profile?.series || detected.series,
        devId: profile?.devId || detected.devId,
        flashKib,
        flashBase,
        flashRange: flashRangeOf(flashKib, flashBase),
        ramRanges: profile?.ramRanges ?? [],
        confidence: detected.confidence,
        supported: detected.supported,
    }
}

/** Classify an address as main flash / RAM / outside the known regions. */
export function classifyTargetAddress(
    compactOrProfile: CompactTargetProfile | TargetProfile | null | undefined,
    address: number,
): 'flash' | 'ram' | 'none' {
    if (!compactOrProfile || typeof address !== 'number' || !Number.isFinite(address)) {
        return 'none'
    }
    const compact = isCompactTargetProfile(compactOrProfile)
        ? compactOrProfile
        : compactTargetProfileApi(null, compactOrProfile)
    const addr = Math.trunc(address) >>> 0
    if (compact.flashRange) {
        const [start, end] = compact.flashRange
        if (addr >= start && addr < end) {
            return 'flash'
        }
    }
    for (const range of compact.ramRanges) {
        const start = range.start >>> 0
        if (addr >= start && addr < ((start + range.size) >>> 0)) {
            return 'ram'
        }
    }
    return 'none'
}

function isCompactTargetProfile(value: unknown): value is CompactTargetProfile {
    return typeof value === 'object'
        && value !== null
        && 'flashRange' in value
        && 'ramRanges' in value
        && 'source' in value
}

/**
 * Diagnostics JSON for the "copy diagnostics" button: detection fields that
 * SWD really provides + the selected profile + limitations + compact API.
 * `generatedAt` is injected by the caller to keep this function pure.
 */
export function buildTargetDiagnostics(
    input: TargetDiagnosticsInput,
): Record<string, unknown> {
    const detected = parseTargetDetection(input.detection)
    const match = input.selectedProfile ? compareDetectedToProfile(detected, input.selectedProfile) : null
    const limitations = detectionLimitations(detected, input.selectedProfile ?? null)
    return {
        kind: 'birdiebot-target-diagnostics',
        version: 1,
        generatedAt: input.generatedAt ?? null,
        appVersion: input.appVersion ?? null,
        target: {
            detected: detected.detected,
            devId: detected.devId,
            devIdHex: formatDevId(detected.devId),
            flashKib: detected.flashKib,
            flashSizeBytes: detected.flashKib > 0 ? detected.flashKib * 1024 : 0,
            flashBase: formatHex32(detected.flashBase),
            modelCandidates: detected.group,
            baseGroup: detected.baseGroup,
            candidates: detected.candidates,
            core: detected.core,
            series: detected.series,
            confidence: detected.confidence,
            supported: detected.supported,
            raw: {
                idcode: input.detection?.idcode ?? null,
                cpuid: input.detection?.cpuid ?? null,
                dpidr: input.detection?.dpidr ?? null,
                uid: input.detection?.uid ?? null,
                family: input.detection?.family ?? null,
                device: input.detection?.device ?? null,
                swdClockHz: input.detection?.swd_clock_hz ?? null,
            },
        },
        selectedProfile: input.selectedProfile
            ? {
                id: input.selectedProfile.id,
                source: input.selectedProfile.source,
                name: input.selectedProfile.name,
                mcu: input.selectedProfile.mcu,
                orderCode: input.selectedProfile.orderCode,
                series: input.selectedProfile.series,
                package: input.selectedProfile.package,
                flashKib: input.selectedProfile.flashKib,
                devId: input.selectedProfile.devId,
                ramRanges: input.selectedProfile.ramRanges.map((range) => formatRamRange(range)),
            }
            : null,
        match,
        limitations: limitations.map((code) => ({code, text: LIMITATION_TEXT_EN[code]})),
        profileApi: compactTargetProfileApi(detected, input.selectedProfile),
        firmwareInfo: input.firmwareInfo ?? null,
    }
}

/** Serialize the diagnostics object as pretty JSON (copy-to-clipboard payload). */
export function buildTargetDiagnosticsJson(input: TargetDiagnosticsInput): string {
    try {
        return JSON.stringify(buildTargetDiagnostics(input), null, 2)
    } catch {
        return JSON.stringify({kind: 'birdiebot-target-diagnostics', version: 1, error: 'serialization failed'})
    }
}
