#!/usr/bin/env node
/**
 * Pure-function tests for web/src/utils/targetProfile.ts (task-36).
 *
 * Covers:
 *  - known MCU/board profile list (order code/package/flashKib/ramRanges);
 *  - FSIZE density codes + candidate derivation (STM32F103x8 etc.);
 *  - /api/target/info v3 parsing (+ legacy v2 fallback);
 *  - detected-vs-selected-profile comparison levels;
 *  - limitation codes (SWD cannot read package / flash boundary);
 *  - custom profile JSON parsing/validation (ramRanges forms);
 *  - localStorage persistence with an injected fake storage;
 *  - compact profile API + address classification (DebugCurve hand-off);
 *  - diagnostics JSON.
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
    idcode: 0x10036410,
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

function fakeStorage(initial = {}) {
    const map = new Map(Object.entries(initial))
    return {
        getItem: (key) => (map.has(key) ? map.get(key) : null),
        setItem: (key, value) => {
            map.set(key, String(value))
        },
        removeItem: (key) => {
            map.delete(key)
        },
        dump: () => Object.fromEntries(map),
    }
}

// 1. known profiles ----------------------------------------------------------
section('known profiles')
check('8 known profiles', tp.KNOWN_TARGET_PROFILES.length === 8, `${tp.KNOWN_TARGET_PROFILES.length}`)
const ids = tp.KNOWN_TARGET_PROFILES.map((profile) => profile.id)
check('profile ids unique', new Set(ids).size === ids.length)
const f103c8 = tp.findKnownTargetProfile('stm32f103c8t6')
check('F103C8T6 profile exists', Boolean(f103c8))
eq('F103C8T6 orderCode', f103c8.orderCode, 'STM32F103C8T6')
eq('F103C8T6 package', f103c8.package, 'LQFP48')
eq('F103C8T6 flashKib', f103c8.flashKib, 64)
eq('F103C8T6 devId', f103c8.devId, 0x410)
eq('F103C8T6 series', f103c8.series, 'STM32F1')
check('F103C8T6 RAM range', f103c8.ramRanges.length === 1
    && f103c8.ramRanges[0].start === 0x20000000
    && f103c8.ramRanges[0].size === 20 * 1024)
const f103cb = tp.findKnownTargetProfile('stm32f103cbt6')
eq('F103CBT6 flashKib', f103cb.flashKib, 128)
const h723 = tp.findKnownTargetProfile('stm32h723vgt6')
eq('H723VGT6 flashKib', h723.flashKib, 1024)
eq('H723VGT6 devId', h723.devId, 0x483)
check('H723VGT6 AXI RAM', h723.ramRanges.some((range) => range.start === 0x24000000 && range.size === 320 * 1024))
const h743 = tp.findKnownTargetProfile('stm32h743vit6')
eq('H743VIT6 flashKib', h743.flashKib, 2048)
eq('H743VIT6 devId', h743.devId, 0x450)
check('F407VGT6 present', Boolean(tp.findKnownTargetProfile('stm32f407vgt6')))
check('F429IGT6 present', Boolean(tp.findKnownTargetProfile('stm32f429igt6')))
check('unknown profile id -> null', tp.findKnownTargetProfile('nope') === null)
check('allTargetProfiles appends custom', tp.allTargetProfiles([{id: 'custom:x'}]).length === 9)

// 2. confidence + density ----------------------------------------------------
section('confidence/density helpers')
eq('confidence flash-narrowed', tp.parseDetectionConfidence('flash-narrowed'), 'flash-narrowed')
eq('confidence group', tp.parseDetectionConfidence('GROUP'), 'group')
eq('confidence unknown fallback', tp.parseDetectionConfidence('something-else'), 'unknown')
eq('confidence null fallback', tp.parseDetectionConfidence(null), 'unknown')
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
check('WB candidates count', wbCandidates.length === 6)
const fallbackDensity = tp.deriveOrderCodeCandidates('STM32F100/F101/F102/F103 (MD)', 64)
check('density derived from flashKib when hint missing', fallbackDensity.includes('STM32F103x8'))
check('unknown group -> no candidates', tp.deriveOrderCodeCandidates('unknown (DEV_ID=0x410)', 0).length === 0)
check('empty group -> no candidates', tp.deriveOrderCodeCandidates('', 64).length === 0)

// 4. order-code matching -----------------------------------------------------
section('order-code matching')
check('F103C8T6 matches F103x8', tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F103x8'))
check('F103C8T6 does not match F100x8', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F100x8'))
check('F103C8T6 does not match F103xB', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F103xB'))
check('H743VIT6 matches H743xI', tp.orderCodeMatchesCandidate('STM32H743VIT6', 'STM32H743xI'))
check('empty pattern', !tp.orderCodeMatchesCandidate('STM32F103C8T6', ''))
check('regex chars escaped', !tp.orderCodeMatchesCandidate('STM32F103C8T6', 'STM32F1.3x8'))

// 5. detection parsing -------------------------------------------------------
section('detection parsing')
const detectedF103 = tp.parseTargetDetection(F103C8_DETECTION)
eq('F103 devId', detectedF103.devId, 0x410)
eq('F103 flashKib', detectedF103.flashKib, 64)
eq('F103 density', detectedF103.density, 'x8')
eq('F103 baseGroup', detectedF103.baseGroup, 'STM32F100/F101/F102/F103 (MD)')
eq('F103 confidence', detectedF103.confidence, 'flash-narrowed')
eq('F103 core', detectedF103.core, 'Cortex-M3')
eq('F103 supported false', detectedF103.supported, false)
check('F103 candidates', detectedF103.candidates.includes('STM32F103x8'))
const detectedLegacy = tp.parseTargetDetection({
    detected: true,
    device: 'STM32H723/H725/H730/H733/H735',
    flash_size: 1048576,
    supported: true,
})
eq('legacy flashKib from flash_size', detectedLegacy.flashKib, 1024)
eq('legacy inferred confidence', detectedLegacy.confidence, 'group')
const notDetected = tp.parseTargetDetection(null)
eq('null detection -> detected false', notDetected.detected, false)
eq('null detection -> confidence unknown', notDetected.confidence, 'unknown')

// 6. profile comparison ------------------------------------------------------
section('profile comparison')
const match = tp.compareDetectedToProfile(F103C8_DETECTION, f103c8)
eq('F103C8T6 profile match level', match.level, 'match')
eq('F103 devId matches', match.devIdMatches, true)
eq('F103 flash matches', match.flashMatches, true)
eq('F103 candidate matches', match.candidateMatches, true)
const flashMismatch = tp.compareDetectedToProfile(F103C8_DETECTION, f103cb)
eq('F103CBT6 vs 64KiB -> flash-mismatch', flashMismatch.level, 'flash-mismatch')
const devMismatch = tp.compareDetectedToProfile(F103C8_DETECTION, tp.findKnownTargetProfile('stm32f407vgt6'))
eq('F407 vs F1 -> dev-id-mismatch', devMismatch.level, 'dev-id-mismatch')
eq('no detection', tp.compareDetectedToProfile(null, f103c8).level, 'no-detection')
eq('no profile', tp.compareDetectedToProfile(F103C8_DETECTION, null).level, 'no-profile')
const unknownCompare = tp.compareDetectedToProfile(UNKNOWN_DEVICE_DETECTION, {id: 'custom:x', devId: 0, flashKib: 0})
eq('unknown detection -> unverified', unknownCompare.level, 'unverified')
eq('H723 profile match', tp.compareDetectedToProfile(H723_DETECTION, h723).level, 'match')

// 7. limitations -------------------------------------------------------------
section('limitations')
const f103Limits = tp.detectionLimitations(F103C8_DETECTION, null)
check('no-detection limitation first', tp.detectionLimitations(null, null)[0] === 'no-detection')
check('F103: no package from SWD', f103Limits.includes('swd-no-package'))
check('F103: flash unsupported', f103Limits.includes('flash-unsupported'))
check('F103: profile not selected', f103Limits.includes('profile-not-selected'))
check('F103: flash-narrowed has no group limitation', !f103Limits.includes('dev-id-group'))
const f103MatchedLimits = tp.detectionLimitations(F103C8_DETECTION, f103c8)
check('matched profile clears profile-not-selected', !f103MatchedLimits.includes('profile-not-selected'))
check('matched profile still cannot verify package', f103MatchedLimits.includes('swd-no-package'))
const groupLimits = tp.detectionLimitations({
    detected: true,
    dev_id: 0x410,
    flash_kib: 0,
    model_candidates: 'STM32F100/F101/F102/F103 (MD)',
    detection_confidence: 'group',
    supported: false,
}, null)
check('group confidence -> dev-id-group', groupLimits.includes('dev-id-group'))
check('group confidence -> flash-size-unknown', groupLimits.includes('flash-size-unknown'))
check('flash mismatch limitation', tp.detectionLimitations(F103C8_DETECTION, f103cb).includes('profile-flash-mismatch'))
check('order-code mismatch limitation', tp.detectionLimitations(H723_DETECTION, f103c8).includes('profile-dev-id-mismatch'))
check('limitation texts are English strings', typeof tp.LIMITATION_TEXT_EN['swd-no-package'] === 'string'
    && tp.LIMITATION_TEXT_EN['swd-no-package'].includes('SWD'))

// 8. custom profiles ---------------------------------------------------------
section('custom profiles')
const customText = JSON.stringify({
    name: 'My F103 board',
    mcu: 'STM32F103C8T6',
    package: 'LQFP48',
    flashKib: 64,
    ramRanges: ['0x20000000-0x20005000', {start: '0x10000000', size: '1KiB'}],
    devId: '0x410',
})
const customParsed = tp.parseCustomTargetProfileJson(customText)
check('custom profile parsed', customParsed.ok, customParsed.error)
eq('custom profile mcu', customParsed.profile.mcu, 'STM32F103C8T6')
eq('custom profile series derived', customParsed.profile.series, 'STM32F1')
eq('custom profile source', customParsed.profile.source, 'custom')
check('custom profile id prefixed', customParsed.profile.id.startsWith('custom:'))
eq('custom profile RAM ranges', customParsed.profile.ramRanges.length, 2)
check('custom profile RAM size', customParsed.profile.ramRanges.some((range) => range.size === 20 * 1024))
eq('custom profile devId parsed', customParsed.profile.devId, 0x410)
check('custom profile id is stable', tp.parseCustomTargetProfileJson(customText).profile.id === customParsed.profile.id)
const rangeForms = tp.parseCustomTargetProfileJson(JSON.stringify({
    mcu: 'STM32F103C8T6',
    flashKib: 64,
    ramRanges: [{start: 0x20000000, end: 0x20005000}, '0x10000000+1KiB'],
}))
check('RAM range object/end + start+size forms', rangeForms.ok
    && rangeForms.profile.ramRanges.length === 2
    && rangeForms.profile.ramRanges.some((range) => range.start === 0x20000000 && range.size === 20 * 1024)
    && rangeForms.profile.ramRanges.some((range) => range.start === 0x10000000 && range.size === 1024))
check('empty JSON rejected', !tp.parseCustomTargetProfileJson('').ok)
check('invalid JSON rejected', !tp.parseCustomTargetProfileJson('{nope').ok)
check('missing mcu/flash rejected', !tp.parseCustomTargetProfileJson('{"name":"x"}').ok)
check('flashKib=0 rejected', !tp.parseCustomTargetProfileJson('{"mcu":"STM32F103C8T6","flashKib":0}').ok)
check('bad devId rejected', !tp.parseCustomTargetProfileJson('{"mcu":"STM32F103C8T6","flashKib":64,"devId":"0x1234"}').ok)
check('overlapping RAM rejected', !tp.parseCustomTargetProfileJson(JSON.stringify({
    mcu: 'STM32F103C8T6',
    flashKib: 64,
    ramRanges: [{start: 0x20000000, size: 0x1000}, {start: 0x20000800, size: 0x1000}],
})).ok)
check('stored profile normalizes', tp.normalizeStoredTargetProfile(JSON.parse(JSON.stringify(customParsed.profile))) !== null)
check('invalid stored profile -> null', tp.normalizeStoredTargetProfile({name: 'x'}) === null)

// 9. persistence -------------------------------------------------------------
section('persistence')
const storage = fakeStorage()
check('storage count matches', tp.KNOWN_TARGET_PROFILES.length === ids.length)
eq('load custom profiles empty', tp.loadCustomProfiles(storage).length, 0)
check('save custom profiles', tp.saveCustomProfiles([customParsed.profile], storage))
eq('load custom profiles roundtrip', tp.loadCustomProfiles(storage).length, 1)
eq('load custom profile mcu', tp.loadCustomProfiles(storage)[0].mcu, 'STM32F103C8T6')
eq('load selected id empty', tp.loadSelectedProfileId(storage), null)
check('save selected id', tp.saveSelectedProfileId('stm32f103c8t6', storage))
eq('load selected id', tp.loadSelectedProfileId(storage), 'stm32f103c8t6')
const restoredKnown = tp.resolveRestoredTargetProfiles(storage)
eq('restore known selection', restoredKnown.selectedProfile?.id, 'stm32f103c8t6')
eq('restore custom list too', restoredKnown.customProfiles.length, 1)
tp.saveSelectedProfileId(customParsed.profile.id, storage)
eq('restore custom selection', tp.resolveRestoredTargetProfiles(storage).selectedProfile?.mcu, 'STM32F103C8T6')
tp.saveSelectedProfileId(null, storage)
eq('clear selected id', tp.loadSelectedProfileId(storage), null)
eq('unknown selected id -> null', tp.resolveRestoredTargetProfiles(fakeStorage({
    [tp.TARGET_PROFILE_STORAGE_KEYS.selectedProfileId]: 'gone',
})).selectedProfile, null)
eq('broken custom JSON -> empty', tp.loadCustomProfiles(fakeStorage({
    [tp.TARGET_PROFILE_STORAGE_KEYS.customProfiles]: '{bad',
})).length, 0)
eq('null storage custom', tp.loadCustomProfiles(null).length, 0)
eq('null storage selected', tp.loadSelectedProfileId(null), null)
check('null storage save false', tp.saveCustomProfiles([], null) === false)
check('upsert replaces by id', tp.upsertCustomProfile([customParsed.profile], {...customParsed.profile, name: 'renamed'})[0].name === 'renamed')
check('remove custom profile', tp.removeCustomProfile([customParsed.profile], customParsed.profile.id).length === 0)

// 10. compact API + address classification ----------------------------------
section('compact API / address validation')
const compactSelected = tp.compactTargetProfileApi(F103C8_DETECTION, f103c8)
eq('compact source selected', compactSelected.source, 'selected')
eq('compact orderCode', compactSelected.orderCode, 'STM32F103C8T6')
eq('compact flashKib', compactSelected.flashKib, 64)
check('compact flashRange', JSON.stringify(compactSelected.flashRange) === JSON.stringify([0x08000000, 0x08010000]), JSON.stringify(compactSelected.flashRange))
eq('compact RAM ranges', compactSelected.ramRanges.length, 1)
const compactDetected = tp.compactTargetProfileApi(F103C8_DETECTION, null)
eq('compact source detected', compactDetected.source, 'detected')
eq('compact detected devId', compactDetected.devId, 0x410)
eq('compact no detection source', tp.compactTargetProfileApi(null, null).source, 'none')
eq('flash base address', tp.classifyTargetAddress(compactSelected, 0x08000000), 'flash')
eq('flash last byte', tp.classifyTargetAddress(compactSelected, 0x0800ffff), 'flash')
eq('flash end exclusive', tp.classifyTargetAddress(compactSelected, 0x08010000), 'none')
eq('RAM start', tp.classifyTargetAddress(compactSelected, 0x20000000), 'ram')
eq('RAM end', tp.classifyTargetAddress(compactSelected, 0x20004fff), 'ram')
eq('RAM end exclusive', tp.classifyTargetAddress(compactSelected, 0x20005000), 'none')
eq('outside regions', tp.classifyTargetAddress(compactSelected, 0x12345678), 'none')
eq('profile-only classification', tp.classifyTargetAddress(f103c8, 0x08000010), 'flash')
eq('null profile classification', tp.classifyTargetAddress(null, 0x08000000), 'none')

// 11. diagnostics JSON -------------------------------------------------------
section('diagnostics')
const diagnosticsText = tp.buildTargetDiagnosticsJson({
    detection: F103C8_DETECTION,
    selectedProfile: f103c8,
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
eq('diagnostics generatedAt', diagnostics.generatedAt, '2026-01-02T03:04:05.000Z')
eq('diagnostics devIdHex', diagnostics.target.devIdHex, '0x410')
eq('diagnostics confidence', diagnostics.target.confidence, 'flash-narrowed')
check('diagnostics candidates', Array.isArray(diagnostics.target.candidates) && diagnostics.target.candidates.includes('STM32F103x8'))
eq('diagnostics selected orderCode', diagnostics.selectedProfile.orderCode, 'STM32F103C8T6')
eq('diagnostics match level', diagnostics.match.level, 'match')
check('diagnostics limitations include swd-no-package',
    diagnostics.limitations.some((entry) => entry.code === 'swd-no-package'))
check('diagnostics limitations have text',
    diagnostics.limitations.every((entry) => typeof entry.text === 'string' && entry.text.length > 0))
check('diagnostics profileApi flashRange', JSON.stringify(diagnostics.profileApi.flashRange) === JSON.stringify([0x08000000, 0x08010000]))
const noProfileDiagnostics = JSON.parse(tp.buildTargetDiagnosticsJson({
    detection: H723_DETECTION,
    selectedProfile: null,
}))
eq('diagnostics no profile', noProfileDiagnostics.selectedProfile, null)
eq('diagnostics no profile match', noProfileDiagnostics.match, null)
check('diagnostics no profile limitation',
    noProfileDiagnostics.limitations.some((entry) => entry.code === 'profile-not-selected'))

// summary --------------------------------------------------------------------
console.log(`\n${checks} checks, ${failures} failure(s)`)
if (failures > 0) {
    process.exitCode = 1
}
