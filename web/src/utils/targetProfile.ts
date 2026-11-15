/**
 * Target detection + built-in MCU profile auto matching (task-36/48).
 *
 * The firmware only reports what SWD can actually observe: DBGMCU DEV_ID
 * (device group, 12 bit), FSIZE / flash capacity, CPUID core, UID and
 * DBGMCU IDCODE.  It can never read the package or the full order code
 * (e.g. the LQFP48 package of an STM32F103C8T6), so this module:
 *
 *  - parses `/api/target/info` v3 fields (`dev_id`, `flash_kib`,
 *    `model_candidates`, `core`, `detection_confidence`) with a fallback for
 *    older firmware that only reports `device` + `flash_size` (+ `idcode`);
 *  - derives plausible order-code candidates from the candidate group and the
 *    FSIZE density hint (STM32F103x8 etc.);
 *  - keeps a read-only list of common STM32F1/F4/G4/H7 order codes and
 *    automatically picks the best profile for the detected DEV_ID + FSIZE
 *    (there is exactly one profile per DEV_ID+FSIZE pair, so the match is
 *    deterministic; the profile is a best-effort representative, not proof);
 *  - emits a compact profile object for DebugCurve address validation plus a
 *    diagnostics JSON for issue reports.
 *
 * Everything here is a pure function/type module (no DOM/network access) so
 * it can be unit-tested from Node: `node scripts/test-target-profile.mjs`.
 */

/** Detection confidence reported by the firmware (`detection_confidence`). */
export type DetectionConfidence = 'unknown' | 'group' | 'flash-narrowed'

/** Inclusive RAM region: [start, start + size). */
export interface TargetRamRange {
    start: number
    size: number
}

/** Built-in MCU profile (read-only; users cannot edit profiles). */
export interface TargetProfile {
    /** Stable id, e.g. `stm32f103c8t6`. */
    id: string
    /** Display name (order code). */
    name: string
    /** MCU label (equal to the order code for built-in profiles). */
    mcu: string
    /** Order code, e.g. "STM32F103C8T6". */
    orderCode: string
    /** STM32 series label, e.g. "STM32F1". */
    series: string
    /** Package label, e.g. "LQFP48". */
    package: string
    /** Main flash size in KiB. */
    flashKib: number
    /** Expected DBGMCU DEV_ID. */
    devId: number
    /** Known main RAM ranges (partial; bus aliases/TCM aliases omitted). */
    ramRanges: TargetRamRange[]
    source: 'builtin'
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

/** Parsed detection view used by the UI/auto-match helpers. */
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

/** Match quality for one built-in profile. */
export type AutoMatchConfidence = 'exact' | 'flash'

/** One ranked built-in profile candidate for the detected target. */
export interface ProfileCandidateMatch {
    profile: TargetProfile
    /** exact = DEV_ID+FSIZE+order-code candidates agree; flash = DEV_ID+FSIZE only. */
    confidence: AutoMatchConfidence
    /** null = no candidate list available to check against. */
    candidateMatches: boolean | null
}

/** Best built-in profile for the detected target (null when undetermined). */
export interface AutoProfileMatch {
    profile: TargetProfile
    confidence: AutoMatchConfidence
    /** Other profiles with the same DEV_ID+FSIZE (normally empty). */
    alternatives: TargetProfile[]
    /** Order-code candidates that were checked (may be empty). */
    candidates: string[]
}

/** Compact profile description for DebugCurve address validation. */
export interface CompactTargetProfile {
    source: 'none' | 'detected' | 'auto'
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
}

export interface TargetDiagnosticsInput {
    detection: TargetDetectionInfo | null
    /** ISO timestamp injected by the caller so the helper stays pure. */
    generatedAt?: string
    appVersion?: string
}

/** Default STM32 main-flash base when the firmware does not report one. */
export const TARGET_FLASH_BASE_DEFAULT = 0x08000000

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

/**
 * Common STM32F1/F4/G4/H7 order codes used by development boards.
 *
 * Rules:
 *  - exactly one profile per (DEV_ID, FSIZE) pair keeps the automatic match
 *    deterministic (the package/order-code suffix is not observable over SWD,
 *    so e.g. F407VGT6 represents the 0x413 + 1 MiB combination);
 *  - `ramRanges` list the main regions only (bus/TCM aliases and backup SRAM
 *    are intentionally omitted).
 */
export const KNOWN_TARGET_PROFILES: readonly TargetProfile[] = Object.freeze([
    // STM32F1 -----------------------------------------------------------------
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
        source: 'builtin',
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
        source: 'builtin',
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
        source: 'builtin',
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
        source: 'builtin',
    },
    {
        id: 'stm32f107vct6',
        name: 'STM32F107VCT6',
        mcu: 'STM32F107VCT6',
        orderCode: 'STM32F107VCT6',
        series: 'STM32F1',
        package: 'LQFP100',
        flashKib: 256,
        devId: 0x418,
        ramRanges: [{start: 0x20000000, size: 64 * 1024}],
        source: 'builtin',
    },
    // STM32F4 -----------------------------------------------------------------
    {
        id: 'stm32f401ccu6',
        name: 'STM32F401CCU6',
        mcu: 'STM32F401CCU6',
        orderCode: 'STM32F401CCU6',
        series: 'STM32F4',
        package: 'UFQFPN48',
        flashKib: 256,
        devId: 0x423,
        ramRanges: [{start: 0x20000000, size: 64 * 1024}],
        source: 'builtin',
    },
    {
        id: 'stm32f411ceu6',
        name: 'STM32F411CEU6',
        mcu: 'STM32F411CEU6',
        orderCode: 'STM32F411CEU6',
        series: 'STM32F4',
        package: 'UFQFPN48',
        flashKib: 512,
        devId: 0x431,
        ramRanges: [{start: 0x20000000, size: 128 * 1024}],
        source: 'builtin',
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
        source: 'builtin',
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
        source: 'builtin',
    },
    {
        id: 'stm32f446ret6',
        name: 'STM32F446RET6',
        mcu: 'STM32F446RET6',
        orderCode: 'STM32F446RET6',
        series: 'STM32F4',
        package: 'LQFP64',
        flashKib: 512,
        devId: 0x421,
        ramRanges: [{start: 0x20000000, size: 128 * 1024}],
        source: 'builtin',
    },
    // STM32G4 -----------------------------------------------------------------
    {
        id: 'stm32g431cbt6',
        name: 'STM32G431CBT6',
        mcu: 'STM32G431CBT6',
        orderCode: 'STM32G431CBT6',
        series: 'STM32G4',
        package: 'LQFP48',
        flashKib: 128,
        devId: 0x468,
        ramRanges: [{start: 0x20000000, size: 32 * 1024}],
        source: 'builtin',
    },
    {
        id: 'stm32g474ret6',
        name: 'STM32G474RET6',
        mcu: 'STM32G474RET6',
        orderCode: 'STM32G474RET6',
        series: 'STM32G4',
        package: 'LQFP64',
        flashKib: 512,
        devId: 0x469,
        ramRanges: [{start: 0x20000000, size: 128 * 1024}],
        source: 'builtin',
    },
    // STM32H7 -----------------------------------------------------------------
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
        source: 'builtin',
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
        source: 'builtin',
    },
    {
        id: 'stm32h743vgt6',
        name: 'STM32H743VGT6',
        mcu: 'STM32H743VGT6',
        orderCode: 'STM32H743VGT6',
        series: 'STM32H7',
        package: 'LQFP100',
        flashKib: 1024,
        devId: 0x450,
        ramRanges: [
            {start: 0x20000000, size: 128 * 1024},
            {start: 0x24000000, size: 512 * 1024},
        ],
        source: 'builtin',
    },
    {
        id: 'stm32h750vbt6',
        name: 'STM32H750VBT6',
        mcu: 'STM32H750VBT6',
        orderCode: 'STM32H750VBT6',
        series: 'STM32H7',
        package: 'LQFP100',
        flashKib: 128,
        devId: 0x450,
        ramRanges: [
            {start: 0x20000000, size: 128 * 1024},
            {start: 0x24000000, size: 512 * 1024},
        ],
        source: 'builtin',
    },
])

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
    const kib = range.size / 1024
    const size = Number.isInteger(kib) ? `${kib} KiB` : `${range.size} B`
    return `${formatHex32(start)}-${formatHex32(end)} (${size})`
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

/** Parse a 32-bit hex/dec value (e.g. idcode "0x10036410"). */
function toU32(value: unknown): number | null {
    if (typeof value === 'number' && Number.isFinite(value)) {
        return Math.trunc(value) >>> 0
    }
    if (typeof value === 'string') {
        const text = value.trim()
        if (!text) {
            return null
        }
        const parsed = /^0x[0-9a-f]+$/i.test(text) ? Number.parseInt(text, 16) : Number(text)
        return Number.isFinite(parsed) ? Math.trunc(parsed) >>> 0 : null
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
    const devIdDirect = toFiniteNumber(info?.dev_id)
    // v2 firmware had no dev_id but did report idcode (DEV_ID in bits[11:0]):
    // derive it so automatic profile matching still works after an OTA lag.
    const idcodeValue = toU32(info?.idcode)
    const devId = devIdDirect !== null && devIdDirect > 0
        ? Math.trunc(devIdDirect) & 0xfff
        : (idcodeValue !== null ? idcodeValue & 0xfff : 0)
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
        if (devId > 0) {
            confidence = flashKib > 0 ? 'flash-narrowed' : 'group'
        } else if (group && !/^unknown/i.test(group)) {
            confidence = 'group'
        }
    }
    return {
        detected,
        devId,
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
// built-in profile auto matching (read-only; no user selection/editing)
// ---------------------------------------------------------------------------

/** Find a built-in profile by id. */
export function findBuiltinTargetProfile(id: string | null | undefined): TargetProfile | null {
    if (!id) {
        return null
    }
    return KNOWN_TARGET_PROFILES.find((profile) => profile.id === id) ?? null
}

/**
 * All built-in profiles (kept for DebugCurve task-49's temporary adapter; users
 * can no longer add/select profiles).  The optional extra list exists only for
 * API-compatibility with the former custom-profile flow.
 */
export function allTargetProfiles(extraProfiles: readonly TargetProfile[] = []): TargetProfile[] {
    return [...KNOWN_TARGET_PROFILES, ...extraProfiles]
}

function isDetectedTarget(value: unknown): value is DetectedTarget {
    return typeof value === 'object'
        && value !== null
        && 'devId' in value
        && 'candidates' in value
        && 'confidence' in value
}

function candidateMatchFor(profile: TargetProfile, candidates: string[]): boolean | null {
    if (candidates.length === 0) {
        return null
    }
    return candidates.some((candidate) => orderCodeMatchesCandidate(profile.orderCode, candidate))
}

/**
 * Rank the built-in profiles that match the detected DEV_ID + FSIZE.
 *
 * Both conditions are mandatory (DEV_ID alone identifies a device group and
 * FSIZE alone is meaningless); the order-code candidate check only refines the
 * ranking/quality.  Returns an empty array when nothing matches.
 */
export function rankTargetProfileMatches(
    infoOrDetected: TargetDetectionInfo | DetectedTarget | null | undefined,
    profiles: readonly TargetProfile[] = KNOWN_TARGET_PROFILES,
): ProfileCandidateMatch[] {
    const detected = isDetectedTarget(infoOrDetected) ? infoOrDetected : parseTargetDetection(infoOrDetected)
    if (!detected.detected || detected.devId === 0 || detected.flashKib === 0) {
        return []
    }
    const matches: ProfileCandidateMatch[] = []
    for (const profile of profiles) {
        if (profile.devId !== detected.devId || profile.flashKib !== detected.flashKib) {
            continue
        }
        const candidateMatches = candidateMatchFor(profile, detected.candidates)
        matches.push({
            profile,
            // 'exact' needs the order-code candidate check to confirm; with
            // DEV_ID+FSIZE only the match is a flash-capacity match.
            confidence: candidateMatches === true ? 'exact' : 'flash',
            candidateMatches,
        })
    }
    // exact candidates first, then "no candidate data", then flash-only; the
    // profile list order is the deterministic tie-breaker.
    return matches.sort((left, right) => scoreMatch(right) - scoreMatch(left))
}

function scoreMatch(match: ProfileCandidateMatch): number {
    if (match.candidateMatches === true) {
        return 2
    }
    return match.candidateMatches === null ? 1 : 0
}

/**
 * Best built-in profile for the detected DEV_ID + FSIZE.
 * Returns null when the detection lacks DEV_ID/FSIZE or no built-in profile
 * matches both (package/order-code suffix can never be verified over SWD).
 */
export function autoMatchTargetProfile(
    infoOrDetected: TargetDetectionInfo | DetectedTarget | null | undefined,
): AutoProfileMatch | null {
    const detected = isDetectedTarget(infoOrDetected) ? infoOrDetected : parseTargetDetection(infoOrDetected)
    const ranked = rankTargetProfileMatches(detected)
    if (ranked.length === 0) {
        return null
    }
    const [best, ...rest] = ranked
    return {
        profile: best.profile,
        confidence: best.confidence,
        alternatives: rest.map((match) => match.profile),
        candidates: detected.candidates,
    }
}

// ---------------------------------------------------------------------------
// compact API + diagnostics (for DebugCurve / copy support)
// ---------------------------------------------------------------------------

/**
 * Compact profile description for later DebugCurve consumption (address
 * validation, flash/RAM display).  The automatically matched built-in profile
 * wins for the exact order code; plain detection contributes DEV_ID/flash when
 * there is no match.
 */
export function compactTargetProfileApi(
    infoOrDetected: TargetDetectionInfo | DetectedTarget | null | undefined,
    /** Former user-selected profile (DebugCurve task-49 compatibility; ignored). */
    _legacySelectedProfile?: TargetProfile | null,
): CompactTargetProfile {
    const detected = isDetectedTarget(infoOrDetected) ? infoOrDetected : parseTargetDetection(infoOrDetected)
    const match = autoMatchTargetProfile(detected)
    const profile = match?.profile ?? null
    const flashKib = profile?.flashKib ?? detected.flashKib
    const flashBase = detected.flashBase || TARGET_FLASH_BASE_DEFAULT
    const source: CompactTargetProfile['source'] = profile ? 'auto' : (detected.detected ? 'detected' : 'none')
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
        : isTargetProfile(compactOrProfile)
            ? compactFromProfile(compactOrProfile)
            : compactTargetProfileApi(compactOrProfile)
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

function isTargetProfile(value: unknown): value is TargetProfile {
    return typeof value === 'object'
        && value !== null
        && 'orderCode' in value
        && 'ramRanges' in value
        && 'devId' in value
}

function compactFromProfile(profile: TargetProfile): CompactTargetProfile {
    return {
        source: 'auto',
        mcu: profile.mcu,
        orderCode: profile.orderCode,
        package: profile.package,
        series: profile.series,
        devId: profile.devId,
        flashKib: profile.flashKib,
        flashBase: TARGET_FLASH_BASE_DEFAULT,
        flashRange: flashRangeOf(profile.flashKib),
        ramRanges: profile.ramRanges,
    }
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
 * SWD really provides + the automatically matched built-in profile + compact
 * profile API.  `generatedAt` is injected by the caller to keep this function
 * pure.  There is no profile editor/selection state anymore.
 */
export function buildTargetDiagnostics(input: TargetDiagnosticsInput): Record<string, unknown> {
    const detected = parseTargetDetection(input.detection)
    const match = autoMatchTargetProfile(detected)
    const profile = match?.profile ?? null
    return {
        kind: 'wifi-dap-target-diagnostics',
        version: 2,
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
        autoMatch: match
            ? {
                matched: true,
                orderCode: profile?.orderCode ?? null,
                mcu: profile?.mcu ?? null,
                series: profile?.series ?? null,
                package: profile?.package ?? null,
                flashKib: profile?.flashKib ?? null,
                devId: profile?.devId ?? null,
                confidence: match.confidence,
                alternatives: match.alternatives.map((alternative) => alternative.orderCode),
                candidates: match.candidates,
            }
            : {matched: false, candidates: detected.candidates},
        profileApi: compactTargetProfileApi(detected),
    }
}

/** Serialize the diagnostics object as pretty JSON (copy-to-clipboard payload). */
export function buildTargetDiagnosticsJson(input: TargetDiagnosticsInput): string {
    try {
        return JSON.stringify(buildTargetDiagnostics(input), null, 2)
    } catch {
        return JSON.stringify({kind: 'wifi-dap-target-diagnostics', version: 2, error: 'serialization failed'})
    }
}
