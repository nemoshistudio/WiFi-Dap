#!/usr/bin/env node
/**
 * Pure-function tests for web/src/utils/targetProfile.ts (task-36/48).
 *
 * Covers:
 *  - built-in STM32F1/F4/G4/H7 profile list invariants (one profile per
 *    DEV_ID+FSIZE pair, order code/package/flashKib/ramRanges);
 *  - FSIZE density codes + candidate derivation (STM32F103x8 etc.);
 *  - /api/target/info v3 parsing + legacy fallback (device/flash/idcode);
 *  - automatic best-profile matching / multi-candidate ranking;
 *  - compact profile API + address classification (DebugCurve hand-off);
 *  - copy-diagnostics JSON (no profile editor state).
 *
 * Usage: node scripts/test-target-profile.mjs
 */
import {execFileSync} from 'node:child_process'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
const bundleOut = path.join(os.tmpdir(), `target-profile-test-${process.pid}.mjs`)

execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'targetProfile.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${bundleOut}`,
], {stdio: 'pipe'})

const tp = await import(bundleOut)

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
const eq = (name, actual, expected) => check(name, Object.is(actual, expected), `got ${JSON.stringify(actual)} expected ${JSON.stringify(expected)}`)
const section = (title) => console.log(`\n[${title}]`)

// Detection fixtures ---------------------------------------------------------
const F103C8_DETECTION = {
    detected: true,
    family: 'stm32f1',
    series: 'STM32F1',
    device: 'STM32F100/F101/F102/F103 (MD)',
    model_candidates: 'STM32F100/F101/F102/F103 (MD) [x8, 64 KiB]',
    dev_id: 0x410,
    flash_kib: 64,
    flash_size: 65536,
    flash_base: 0x08000000,
    core: 'Cortex-M3',
    detection_confidence: 'flash-narrowed',
    supported: false,
    cpuid: 0x410fc231,
    idcode: '0x10036410',
    uid: '00112233445566778899aabb',
    swd_clock_hz: 2000000,
}
const H723_DETECTION = {
    detected: true,
    series: 'STM32H7',
    device: 'STM32H723/H725/H730/H733/H735',
    model_candidates: 'STM32H723/H725/H730/H733/H735 [xG, 1024 KiB]',
    dev_id: 0x483,
    flash_kib: 1024,
    flash_size: 1048576,
    core: 'Cortex-M7',
    detection_confidence: 'flash-narrowed',
    supported: true,
}
const UNKNOWN_DEVICE_DETECTION = {
    detected: true,
    series: 'unknown',
    device: 'unknown (DEV_ID=0x123)',
    model_candidates: 'unknown (DEV_ID=0x123)',
    dev_id: 0x123,
    flash_kib: 0,
    flash_size: 0,
    core: 'Cortex-M4',
    detection_confidence: 'unknown',
    supported: false,
}

function cloneProfile(base, overrides) {
    return {...base, ramRanges: base.ramRanges.map((range) => ({...range})), ...overrides}
}

// 1. built-in profile list ---------------------------------------------------
section('built-in profiles')
const profiles = tp.KNOWN_TARGET_PROFILES
check('16 built-in profiles', profiles.length === 16, `${profiles.length}`)
const ids = profiles.map((profile) => profile.id)
check('profile ids unique', new Set(ids).size === ids.length)
const pairKeys = profiles.map((profile) => `${profile.devId}/${profile.flashKib}`)
check('one profile per DEV_ID+FSIZE pair', new Set(pairKeys).size === pairKeys.length, pairKeys.join(','))
check('all profiles are builtin', profiles.every((profile) => profile.source === 'builtin'))
check('order code/mcu/name consistent', profiles.every((profile) => profile.orderCode === profile.mcu && profile.name === profile.mcu))
check('series limited to F1/F4/G4/H7', profiles.every((profile) => ['STM32F1', 'STM32F4', 'STM32G4', 'STM32H7'].includes(profile.series)))
check('packages present', profiles.every((profile) => typeof profile.package === 'string' && profile.package.length > 0))
check('flash sizes positive', profiles.every((profile) => profile.flashKib > 0 && Number.isInteger(profile.flashKib)))
check('dev ids are 12-bit', profiles.every((profile) => profile.devId > 0 && profile.devId <= 0xfff))
check('ram ranges valid', profiles.every((profile) => profile.ramRanges.length > 0 && profile.ramRanges.every((range) => range.start > 0 && range.size > 0)))
const seriesSet = new Set(profiles.map((profile) => profile.series))
check('F1/F4/G4/H7 coverage', ['STM32F1', 'STM32F4', 'STM32G4', 'STM32H7'].every((series) => seriesSet.has(series)), [...seriesSet].join(','))
const expectedOrderCodes = [
    'STM32F103C8T6', 'STM32F103CBT6', 'STM32F103RCT6', 'STM32F103ZET6', 'STM32F107VCT6',
    'STM32F401CCU6', 'STM32F411CEU6', 'STM32F407VGT6', 'STM32F429IGT6', 'STM32F446RET6',
    'STM32G431CBT6', 'STM32G474RET6', 'STM32H723VGT6', 'STM32H743VIT6', 'STM32H743VGT6', 'STM32H750VBT6',
]
check('expected common order codes present', expectedOrderCodes.every((code) => profiles.some((profile) => profile.orderCode === code)))
const f103c8 = profiles.find((profile) => profile.orderCode === 'STM32F103C8T6')
eq('F103C8T6 package', f103c8.package, 'LQFP48')
eq('F103C8T6 flashKib', f103c8.flashKib, 64)
eq('F103C8T6 devId', f103c8.devId, 0x410)
check('F103C8T6 RAM range', f103c8.ramRanges.length === 1
    && f103c8.ramRanges[0].start === 0x20000000
    && f103c8.ramRanges[0].size === 20 * 1024)
check('findBuiltinTargetProfile', tp.findBuiltinTargetProfile('stm32f103c8t6')?.orderCode === 'STM32F103C8T6')
check('findBuiltinTargetProfile miss', tp.findBuiltinTargetProfile('nope') === null)
eq('allTargetProfiles compat length', tp.allTargetProfiles().length, 16)

// 2. helper functions --------------------------------------------------------
section('helpers')
eq('confidence flash-narrowed', tp.parseDetectionConfidence('flash-narrowed'), 'flash-narrowed')
eq('confidence group', tp.parseDetectionConfidence('GROUP'), 'group')
eq('confidence unknown fallback', tp.parseDetectionConfidence('nope'), 'unknown')
eq('density 64K', tp.flashDensityCode(64), 'x8')
eq('density 128K', tp.flashDensityCode(128), 'xB')
eq('density 1024K', tp.flashDensityCode(1024), 'xG')
eq('density 2048K', tp.flashDensityCode(2048), 'xI')
eq('density nonstandard', tp.flashDensityCode(100), '')
eq('formatDevId', tp.formatDevId(0x410), '0x410')
eq('formatHex32', tp.formatHex32(0x08000000), '0x08000000')
eq('formatFlashKib 64', tp.formatFlashKib(64), '64 KiB')
eq('formatFlashKib 1024', tp.formatFlashKib(1024), '1024 KiB (1 MiB)')
eq('formatRamRange', tp.formatRamRange({start: 0x20000000, size: 20 * 1024}), '0x20000000-0x20005000 (20 KiB)')
check('flashRangeOf', JSON.stringify(tp.flashRangeOf(64)) === JSON.stringify([0x08000000, 0x08010000]))
eq('flashRangeOf zero', tp.flashRangeOf(0), null)

// 3. candidate derivation ----------------------------------------------------
section('candidate derivation')
const f103Candidates = tp.deriveOrderCodeCandidates(F103C8_DETECTION.model_candidates, 64)
check('F103 group -> 4 candidates', f103Candidates.length === 4, JSON.stringify(f103Candidates))
check('F103 candidates contain STM32F103x8', f103Candidates.includes('STM32F103x8'))
check('F103 candidates contain STM32F100x8', f103Candidates.includes('STM32F100x8'))
const h7Candidates = tp.deriveOrderCodeCandidates(H723_DETECTION.model_candidates, 1024)
check('H7 group -> 5 candidates', h7Candidates.length === 5, JSON.stringify(h7Candidates))
check('H7 candidates contain STM32H723xG', h7Candidates.includes('STM32H723xG'))
const ranged = tp.deriveOrderCodeCandidates('STM32F465/F466/F467/F468/F469/F476-F479 [xG, 1024 KiB]', 1024)
check('range token expanded (9 candidates)', ranged.length === 9, JSON.stringify(ranged))
check('range expansion contains STM32F479xG', ranged.includes('STM32F479xG'))
const wbCandidates = tp.deriveOrderCodeCandidates('STM32WB10/WB15/WB20/WB25/WB30/WB35 [512 KiB]', 512)
check('WB candidates have no density letter', wbCandidates.every((item) => !item.includes('x')), JSON.stringify(wbCandidates))
check('density derived from flashKib when hint missing', tp.deriveOrderCodeCandidates('STM32F100/F101/F102/F103 (MD)', 64).includes('STM32F103x8'))
check('unknown group -> no candidates', tp.deriveOrderCodeCandidates('unknown (DEV_ID=0x410)', 0).length === 0)
check('empty group -> no candidates', tp.deriveOrderCodeCandidates('', 64).length === 0)

// 4. order-code matching -----------------------------------------------------
section('order-code matching')
check('F103C8T6 matches F103x8', tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F103x8'))
check('F103C8T6 does not match F100x8', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F100x8'))
check('F103C8T6 does not match F103xB', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F103xB'))
check('H743VIT6 matches H743xI', tp.orderCodeMatchesCandidate('STM32H743VIT6', 'STM32H743xI'))
check('empty pattern rejected', !tp.orderCodeMatchesCandidate('STM32F103C8T6', ''))
check('regex chars escaped', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F1.3x8'))

// 5. detection parsing -------------------------------------------------------
section('detection parsing')
const detectedF103 = tp.parseTargetDetection(F103C8_DETECTION)
eq('F103 devId', detectedF103.devId, 0x410)
eq('F103 flashKib', detectedF103.flashKib, 64)
eq('F103 density', detectedF103.density, 'x8')
eq('F103 baseGroup', detectedF103.baseGroup, 'STM32F100/F101/F102/F103 (MD)')
eq('F103 core', detectedF103.core, 'Cortex-M3')
eq('F103 supported false', detectedF103.supported, false)
check('F103 candidates', detectedF103.candidates.includes('STM32F103x8'))
const detectedLegacy = tp.parseTargetDetection({
    detected: true,
    device: 'STM32H723/H725/H730/H733/H735',
    flash_size: 1048576,
    idcode: '0x10036483',
    supported: true,
})
eq('legacy flashKib from flash_size', detectedLegacy.flashKib, 1024)
eq('legacy devId derived from idcode', detectedLegacy.devId, 0x483)
eq('null detection -> detected false', tp.parseTargetDetection(null).detected, false)
check('unknown-device detection parses', tp.parseTargetDetection(UNKNOWN_DEVICE_DETECTION).devId === 0x123)

// 6. automatic profile matching ---------------------------------------------
section('automatic profile matching')
const f103Match = tp.autoMatchTargetProfile(F103C8_DETECTION)
check('F103C8T6 detection auto-matches', f103Match?.profile.orderCode === 'STM32F103C8T6', JSON.stringify(f103Match?.profile?.orderCode))
eq('F103C8T6 match confidence', f103Match.confidence, 'exact')
eq('F103C8T6 alternatives empty', f103Match.alternatives.length, 0)
const cbDetection = {...F103C8_DETECTION, model_candidates: 'STM32F100/F101/F102/F103 (MD) [xB, 128 KiB]', flash_kib: 128, flash_size: 131072}
check('F103CB detection auto-matches F103CBT6', tp.autoMatchTargetProfile(cbDetection)?.profile.orderCode === 'STM32F103CBT6')
eq('0x410 + 32KiB (no builtin) -> null', tp.autoMatchTargetProfile({...F103C8_DETECTION, flash_kib: 32, flash_size: 32768}), null)
check('F407VGT6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x413, flash_kib: 1024,
    model_candidates: 'STM32F405/F407/F415/F417 [xG, 1024 KiB]',
})?.profile.orderCode === 'STM32F407VGT6')
check('H723VGT6 auto-matches', tp.autoMatchTargetProfile(H723_DETECTION)?.profile.orderCode === 'STM32H723VGT6')
check('H743VIT6 auto-matches (2 MiB)', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x450, flash_kib: 2048,
    model_candidates: 'STM32H743/H745/H747/H750/H755/H757 [xI, 2048 KiB]',
})?.profile.orderCode === 'STM32H743VIT6')
check('H743VGT6 auto-matches (1 MiB)', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x450, flash_kib: 1024,
    model_candidates: 'STM32H743/H745/H747/H750/H755/H757',
})?.profile.orderCode === 'STM32H743VGT6')
check('H750VBT6 auto-matches (128 KiB)', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x450, flash_kib: 128,
    model_candidates: 'STM32H750 [xB, 128 KiB]',
})?.profile.orderCode === 'STM32H750VBT6')
check('F401CCU6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x423, flash_kib: 256,
    model_candidates: 'STM32F401xB/F401xC [xC, 256 KiB]',
})?.profile.orderCode === 'STM32F401CCU6')
check('F411CEU6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x431, flash_kib: 512,
    model_candidates: 'STM32F411 [xE, 512 KiB]',
})?.profile.orderCode === 'STM32F411CEU6')
check('F446RET6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x421, flash_kib: 512,
    model_candidates: 'STM32F446',
})?.profile.orderCode === 'STM32F446RET6')
check('F107VCT6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x418, flash_kib: 256,
    model_candidates: 'STM32F105/F107',
})?.profile.orderCode === 'STM32F107VCT6')
check('G431CBT6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x468, flash_kib: 128,
    model_candidates: 'STM32G431/G441 [xB, 128 KiB]',
})?.profile.orderCode === 'STM32G431CBT6')
check('G474RET6 auto-matches', tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x469, flash_kib: 512,
    model_candidates: 'STM32G474/G484 [xE, 512 KiB]',
})?.profile.orderCode === 'STM32G474RET6')
check('unknown DEV_ID -> null', tp.autoMatchTargetProfile(UNKNOWN_DEVICE_DETECTION) === null)
check('DEV_ID without FSIZE -> null', tp.autoMatchTargetProfile({detected: true, dev_id: 0x410, flash_kib: 0}) === null)
check('not detected -> null', tp.autoMatchTargetProfile(null) === null)
check('legacy idcode detection auto-matches', tp.autoMatchTargetProfile({
    detected: true, device: 'STM32H723/H725/H730/H733/H735', flash_size: 1048576, idcode: '0x10036483',
})?.profile.orderCode === 'STM32H723VGT6')
const noCandidatesMatch = tp.autoMatchTargetProfile({
    detected: true, dev_id: 0x410, flash_kib: 64, model_candidates: 'unknown (DEV_ID=0x410)',
})
check('no candidate list still matches via DEV_ID+FSIZE', noCandidatesMatch?.profile.orderCode === 'STM32F103C8T6')
eq('no candidate list -> flash confidence', noCandidatesMatch.confidence, 'flash')

// multi-candidate ranking (synthetic duplicate DEV_ID+FSIZE profiles)
const syntheticProfiles = [
    cloneProfile(f103c8, {id: 'synthetic-flash-only', orderCode: 'STM32F199C8T6', name: 'STM32F199C8T6', mcu: 'STM32F199C8T6'}),
    cloneProfile(f103c8, {id: 'synthetic-exact', orderCode: 'STM32F103C8T6'}),
    cloneProfile(f103c8, {id: 'synthetic-exact-2', orderCode: 'STM32F101C8T6', name: 'STM32F101C8T6', mcu: 'STM32F101C8T6'}),
]
const ranked = tp.rankTargetProfileMatches(F103C8_DETECTION, syntheticProfiles)
eq('ranking keeps all DEV_ID+FSIZE matches', ranked.length, 3)
eq('ranking puts exact candidate matches first', ranked[0].confidence, 'exact')
check('ranking exact entries ordered by profile list', ranked[0].profile.id === 'synthetic-exact')
check('ranking flash-only last', ranked[2].profile.id === 'synthetic-flash-only')
check('ranked top equals auto match', tp.autoMatchTargetProfile(F103C8_DETECTION)?.profile.orderCode === 'STM32F103C8T6')
const nullCandidateRanked = tp.rankTargetProfileMatches(
    {...detectedF103, candidates: []},
    [cloneProfile(f103c8, {id: 'synthetic-flash-only', orderCode: 'STM32F199C8T6'})],
)
eq('empty candidate list ranks flash', nullCandidateRanked[0].confidence, 'flash')
eq('empty candidate list candidateMatches null', nullCandidateRanked[0].candidateMatches, null)

// 7. compact API + address classification -----------------------------------
section('compact API / address validation')
const compact = tp.compactTargetProfileApi(F103C8_DETECTION)
eq('compact source auto', compact.source, 'auto')
eq('compact order code', compact.orderCode, 'STM32F103C8T6')
eq('compact flashKib', compact.flashKib, 64)
eq('compact RAM ranges', compact.ramRanges.length, 1)
check('compact flashRange', JSON.stringify(compact.flashRange) === JSON.stringify([0x08000000, 0x08010000]), JSON.stringify(compact.flashRange))
eq('compact devId', compact.devId, 0x410)
const compactUnknown = tp.compactTargetProfileApi({
    detected: true, dev_id: 0x123, flash_kib: 512, device: 'unknown (DEV_ID=0x123)',
})
eq('compact source detected when unmatched', compactUnknown.source, 'detected')
eq('compact no order code when unmatched', compactUnknown.orderCode, '')
eq('compact none', tp.compactTargetProfileApi(null).source, 'none')
eq('compat compact with null profile param', tp.compactTargetProfileApi(F103C8_DETECTION, null).orderCode, 'STM32F103C8T6')
eq('flash base address', tp.classifyTargetAddress(compact, 0x08000000), 'flash')
eq('flash last byte', tp.classifyTargetAddress(compact, 0x0800ffff), 'flash')
eq('flash end exclusive', tp.classifyTargetAddress(compact, 0x08010000), 'none')
eq('RAM start', tp.classifyTargetAddress(compact, 0x20000000), 'ram')
eq('RAM end', tp.classifyTargetAddress(compact, 0x20004fff), 'ram')
eq('RAM end exclusive', tp.classifyTargetAddress(compact, 0x20005000), 'none')
eq('outside regions', tp.classifyTargetAddress(compact, 0x12345678), 'none')
eq('profile-only classification', tp.classifyTargetAddress(f103c8, 0x08000010), 'flash')
eq('null classification', tp.classifyTargetAddress(null, 0x08000000), 'none')

// 8. diagnostics JSON --------------------------------------------------------
section('diagnostics')
const diagnosticsText = tp.buildTargetDiagnosticsJson({
    detection: F103C8_DETECTION,
    generatedAt: '2026-01-02T03:04:05.000Z',
    appVersion: '1.2.0-test',
})
let diagnostics = null
try {
    diagnostics = JSON.parse(diagnosticsText)
} catch {
    diagnostics = null
}
check('diagnostics JSON parses', diagnostics !== null)
eq('diagnostics kind', diagnostics.kind, 'birdiebot-target-diagnostics')
eq('diagnostics version', diagnostics.version, 2)
eq('diagnostics generatedAt', diagnostics.generatedAt, '2026-01-02T03:04:05.000Z')
eq('diagnostics appVersion', diagnostics.appVersion, '1.2.0-test')
eq('diagnostics devIdHex', diagnostics.target.devIdHex, '0x410')
eq('diagnostics flashKib', diagnostics.target.flashKib, 64)
eq('diagnostics flashBase', diagnostics.target.flashBase, '0x08000000')
check('diagnostics candidates', diagnostics.target.candidates.includes('STM32F103x8'))
check('diagnostics has no profile editor state', !('selectedProfile' in diagnostics) && !('match' in diagnostics) && !('limitations' in diagnostics))
eq('diagnostics autoMatch matched', diagnostics.autoMatch.matched, true)
eq('diagnostics autoMatch orderCode', diagnostics.autoMatch.orderCode, 'STM32F103C8T6')
eq('diagnostics autoMatch confidence', diagnostics.autoMatch.confidence, 'exact')
eq('diagnostics autoMatch alternatives', diagnostics.autoMatch.alternatives.length, 0)
check('diagnostics profileApi flashRange', JSON.stringify(diagnostics.profileApi.flashRange) === JSON.stringify([0x08000000, 0x08010000]))
const unmatchedDiagnostics = JSON.parse(tp.buildTargetDiagnosticsJson({detection: UNKNOWN_DEVICE_DETECTION}))
eq('diagnostics unmatched autoMatch', unmatchedDiagnostics.autoMatch.matched, false)
eq('diagnostics unmatched profileApi source', unmatchedDiagnostics.profileApi.source, 'detected')
const noTargetDiagnostics = JSON.parse(tp.buildTargetDiagnosticsJson({detection: null}))
eq('diagnostics null detection source', noTargetDiagnostics.profileApi.source, 'none')
eq('diagnostics null detection autoMatch', noTargetDiagnostics.autoMatch.matched, false)

// summary --------------------------------------------------------------------
console.log(`\n${checks} checks, ${failures} failure(s)`)
if (failures > 0) {
    process.exitCode = 1
}
