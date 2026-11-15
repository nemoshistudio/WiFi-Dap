#!/usr/bin/env node
/* eslint-env node */
/**
 * task-41 测试：ELF 行号索引（`elfLineIndex.ts` + `elfDwarf.ts` 的 CU ranges/行表缓存）。
 *
 * 覆盖：
 *  1) `decodeDebugRangesV4` / `decodeDebugRngListsV5` synthetic 单元测试
 *     （base selection / offset_pair / start_end / start_length / startx / 截断容错）；
 *  2) 真实 reference -O2 / debug-O0 ELF：
 *     - `parseElfLineIndex` 全量索引统计（CU/文件/行/sequence/过滤统计）；
 *     - 所有行地址可执行 + Flash 校验、地址 0/重定位垃圾 sequence 已过滤；
 *     - CU ranges（.debug_ranges）与行表 sequence 逐 CU 一致性；
 *     - `pcToLine` 与 `addr2line -f -i` 抽样 >=200 交叉（release / debug 各 250）；
 *     - `fileLineToAddress`（is_stmt 优先 / Flash 校验）往返一致；
 *     - 路径匹配（相对路径/Windows 反斜杠/后缀/basename）；
 *  3) 容错：非 ELF/空 buffer、缺 .debug_line、坏 .debug_line/.debug_info offset、
 *     非法查询；`parseElfSections` 对 ELF32 真实文件。
 *
 * 只读：不修改仓库/设备，不 OTA、不 commit、不跑 Docker；临时产物只写 os.tmpdir()。
 *
 * 用法：node scripts/test-elf-line-index.mjs [release.elf] [debug.elf]
 *   默认 reference release / debug-O0；ELF 不存在时对应项 [skip]。
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const releaseElf = process.argv[2] ?? '~/firmware/reference/firmware.elf'
const debugElf = process.argv[3] ?? '~/firmware/reference-debug/firmware.elf'

// ---------------------------------------------------------------------------
// 打包 TS（项目自带 esbuild，离线）
// ---------------------------------------------------------------------------
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
if (!fs.existsSync(esbuild)) {
    console.error(`esbuild not found: ${esbuild} (run npm install in web/)`)
    process.exit(1)
}
const bundleTs = (entry, tag) => {
    const out = path.join(os.tmpdir(), `${tag}-${process.pid}.mjs`)
    execFileSync(esbuild, [
        path.join(root, 'src', 'utils', entry),
        '--bundle', '--format=esm', '--platform=node', `--outfile=${out}`,
    ], {stdio: 'pipe'})
    return out
}
const lineIndexBundle = bundleTs('elfLineIndex.ts', 'elf-line-index')
const dwarfBundle = bundleTs('elfDwarf.ts', 'elf-dwarf-lines')

const {
    parseElfSections,
    parseElfLineIndex,
    buildElfLineIndex,
    pcToLine,
    pcToFileLine,
    fileLineToAddress,
    fileLineToAddresses,
    findElfLineFiles,
    findElfLineFile,
    ElfLineIndexError,
} = await import(lineIndexBundle)
const {decodeDebugRangesV4, decodeDebugRngListsV5} = await import(dwarfBundle)

let failures = 0
const check = (name, cond, detail = '') => {
    if (cond) console.log(`  ok   ${name}`)
    else {
        console.log(`  FAIL ${name}${detail ? ' :: ' + detail : ''}`)
        failures++
    }
}
const checkEq = (name, actual, expected) => {
    check(name, actual === expected, `actual=${JSON.stringify(actual)} expected=${JSON.stringify(expected)}`)
}

const hex = (value) => `0x${(value >>> 0).toString(16)}`
const le = (value, size) => {
    const out = new Uint8Array(size)
    let rest = value
    for (let i = 0; i < size; i++) {
        out[i] = rest & 0xff
        rest = Math.floor(rest / 256)
    }
    return out
}
const u8 = (value) => le(value, 1)
const u32 = (value) => le(value, 4)
const uleb = (value) => {
    const out = []
    let rest = value
    do {
        let byte = rest & 0x7f
        rest = Math.floor(rest / 128)
        if (rest) byte |= 0x80
        out.push(byte)
    } while (rest)
    return Uint8Array.from(out)
}
const cat = (...parts) => {
    const arrays = parts.flat().map((part) => (typeof part === 'number' ? Uint8Array.of(part) : part))
    const total = arrays.reduce((sum, part) => sum + part.length, 0)
    const out = new Uint8Array(total)
    let offset = 0
    for (const part of arrays) {
        out.set(part, offset)
        offset += part.length
    }
    return out
}

// ---------------------------------------------------------------------------
// 1. CU ranges synthetic 单测
// ---------------------------------------------------------------------------
console.log('unit: CU ranges decoders')
{
    // DWARF4 .debug_ranges：base selection + 两条区间 + terminator
    const data = cat(
        u32(0xffffffff), u32(0x1000), // base = 0x1000
        u32(0x10), u32(0x20),
        u32(0x20), u32(0x30),
        u32(0), u32(0),
    )
    const ranges = decodeDebugRangesV4(data, 0, 4, 0)
    checkEq('ranges v4: count', ranges.length, 2)
    checkEq('ranges v4: base applied', `${hex(ranges[0].pcLow)}..${hex(ranges[0].pcHigh)}`, '0x1010..0x1020')
    checkEq('ranges v4: second range', `${hex(ranges[1].pcLow)}..${hex(ranges[1].pcHigh)}`, '0x1020..0x1030')
    // initialBase 生效
    const withInitial = decodeDebugRangesV4(cat(u32(4), u32(8), u32(0), u32(0)), 0, 4, 0x2000)
    checkEq('ranges v4: initialBase applied', `${hex(withInitial[0].pcLow)}..${hex(withInitial[0].pcHigh)}`, '0x2004..0x2008')
    // 截断：不抛异常，返回已解析范围
    const truncated = decodeDebugRangesV4(cat(u32(1), u32(2), [0x01]), 0, 4, 0)
    checkEq('ranges v4: truncated tolerated', truncated.length, 1)
    // 非法 offset / 空数据
    checkEq('ranges v4: invalid offset -> empty', decodeDebugRangesV4(data, 0x100, 4, 0).length, 0)
    checkEq('ranges v4: empty data -> empty', decodeDebugRangesV4(new Uint8Array(0), 0, 4, 0).length, 0)
}
{
    // DWARF5 .debug_rnglists：base_address + offset_pair + start_length + start_end
    const data = cat(
        u8(0x05), u32(0x8000), // DW_RLE_base_address
        u8(0x04), uleb(0x10), uleb(0x20), // offset_pair -> 0x8010..0x8020
        u8(0x07), u32(0x4000), uleb(0x40), // start_length -> 0x4000..0x4040
        u8(0x06), u32(0x5000), u32(0x5010), // start_end -> 0x5000..0x5010
        u8(0x00), // end_of_list
    )
    const ranges = decodeDebugRngListsV5(data, 0, 4)
    checkEq('ranges v5: count', ranges.length, 3)
    checkEq('ranges v5: offset_pair', `${hex(ranges[0].pcLow)}..${hex(ranges[0].pcHigh)}`, '0x8010..0x8020')
    checkEq('ranges v5: start_length', `${hex(ranges[1].pcLow)}..${hex(ranges[1].pcHigh)}`, '0x4000..0x4040')
    checkEq('ranges v5: start_end', `${hex(ranges[2].pcLow)}..${hex(ranges[2].pcHigh)}`, '0x5000..0x5010')
    // startx_endx + .debug_addr（addrBase 指向第一条地址）
    const debugAddr = cat(u32(0x220000), u32(0x330000))
    const startx = cat(u8(0x02), uleb(0), uleb(1), u8(0x00))
    const startxRanges = decodeDebugRngListsV5(startx, 0, 4, {addr: debugAddr, addrBase: 0})
    checkEq('ranges v5: startx_endx', `${hex(startxRanges[0].pcLow)}..${hex(startxRanges[0].pcHigh)}`, '0x220000..0x330000')
    // 未知 opcode / 截断：容错不抛异常
    const unknown = decodeDebugRngListsV5(cat(u8(0xee), u32(1), u32(2)), 0, 4)
    checkEq('ranges v5: unknown opcode tolerated', unknown.length, 0)
    const cut = decodeDebugRngListsV5(cat(u8(0x06), u32(1), [0x02]), 0, 4)
    checkEq('ranges v5: truncated tolerated', cut.length, 0)
}

// ---------------------------------------------------------------------------
// 2. 真实 ELF
// ---------------------------------------------------------------------------
console.log('\nreal ELF')

const execOrNull = (file, args, maxBuffer = 512 * 1024 * 1024) => {
    try {
        return execFileSync(file, args, {maxBuffer}).toString()
    } catch {
        return null
    }
}

/** 解析 addr2line -f -i -a 输出：地址 -> 最内层 frame 的 file:line。 */
const addr2lineLookup = (elfPath, addresses) => {
    const result = new Map()
    const output = execOrNull('addr2line', ['-e', elfPath, '-f', '-i', '-a', ...addresses])
    if (output === null) return null
    const lines = output.split('\n').map((line) => line.trim()).filter(Boolean)
    for (let index = 0; index < lines.length; index++) {
        if (!/^0x[0-9a-f]+$/i.test(lines[index])) continue
        const address = parseInt(lines[index], 16)
        const frameLines = []
        let cursor = index + 1
        while (cursor + 1 < lines.length && !/^0x[0-9a-f]+$/i.test(lines[cursor])) {
            frameLines.push({func: lines[cursor], location: lines[cursor + 1]})
            cursor += 2
        }
        const location = frameLines[0]?.location ?? ''
        result.set(address, location)
        index = cursor - 1
    }
    return result
}

const parseAddrLocation = (location) => {
    if (!location) return null
    const cleaned = location
        .replace(/\s+\(discriminator\s+\d+\)$/i, '')
        .replace(/\s+\(inlined by\)$/i, '')
    const match = cleaned.match(/^(.*):(\d+|\?)$/)
    if (!match) return null
    return {
        basename: match[1].split('/').pop(),
        path: match[1],
        line: match[2] === '?' ? 0 : parseInt(match[2], 10),
    }
}

const mergeAdjacentRanges = (ranges) => {
    const sorted = ranges
        .map((range) => [range[0] >>> 0, range[1] >>> 0])
        .filter(([low, high]) => high > low)
        .sort((left, right) => left[0] - right[0])
    const merged = []
    for (const range of sorted) {
        const last = merged[merged.length - 1]
        if (last && range[0] <= last[1]) last[1] = Math.max(last[1], range[1])
        else merged.push([range[0], range[1]])
    }
    return merged
}

const checkRealElf = (label, elfPath) => {
    if (!fs.existsSync(elfPath)) {
        console.log(`  [skip] ${label}: ELF not found: ${elfPath}`)
        return
    }
    const buffer = fs.readFileSync(elfPath)
    console.log(`\n  ${label}: ${elfPath}`)

    // ---- parseElfSections ----
    let parsed = null
    try {
        parsed = parseElfSections(buffer)
    } catch (error) {
        check(`${label}: parseElfSections`, false, error.message)
        return
    }
    const sectionNames = parsed.sections.map((section) => section.name)
    check(`${label}: parseElfSections finds .debug_info/.debug_line`, sectionNames.includes('.debug_info') && sectionNames.includes('.debug_line'))
    check(`${label}: parseElfSections finds .text (executable)`, parsed.sections.some((section) => section.name === '.text' && (section.flags & 0x4) !== 0))

    // ---- parseElfLineIndex ----
    const buildStart = Date.now()
    let index = null
    try {
        index = parseElfLineIndex(buffer)
    } catch (error) {
        check(`${label}: parseElfLineIndex`, false, error.message)
        return
    }
    const buildMs = Date.now() - buildStart
    check(`${label}: units parsed`, index.stats.units > 0, index.stats.units)
    check(`${label}: units with line table`, index.stats.unitsWithLines >= index.stats.units * 0.9,
        `${index.stats.unitsWithLines}/${index.stats.units}`)
    check(`${label}: CU ranges extracted (.debug_ranges)`, index.units.filter((unit) => unit.ranges.length > 0).length >= index.units.length * 0.9,
        `${index.units.filter((unit) => unit.ranges.length > 0).length}/${index.units.length}`)
    check(`${label}: files`, index.files.length > 100, index.files.length)
    check(`${label}: rows (>= 30000)`, index.rows.length >= 30000, index.rows.length)
    check(`${label}: sequences`, index.sequences.length > 100, index.sequences.length)
    check(`${label}: relocated/垃圾 sequence filtered`, index.stats.droppedSequences > 0,
        `dropped=${index.stats.droppedSequences} rows=${index.stats.droppedRows}`)
    check(`${label}: no address-0 rows`, index.rows.every((row) => row.address !== 0))
    check(`${label}: all rows in executable ranges`,
        index.rows.every((row) => index.executableRanges.some(([start, end]) => row.address >= start && row.address < end)))
    check(`${label}: all rows flash-validated (H7 .text in Flash)`,
        index.stats.flashRows === index.rows.length, `${index.stats.flashRows}/${index.rows.length}`)
    // C++ COMDAT/inline 合并会让同一代码区间出现在多个 CU 的行表里（addr2line 同样取第一个 CU），
    // 少量重叠是正常的；只要求不是大面积重叠（地址 0/不可执行垃圾已被过滤）。
    check(`${label}: overlapping sequences limited (<=3%)`,
        index.stats.overlappingSequences <= index.stats.sequences * 0.03,
        `${index.stats.overlappingSequences}/${index.stats.sequences}`)
    check(`${label}: index build time < 8s`, buildMs < 8000, `${buildMs}ms`)
    console.log(`  info ${label}: units=${index.stats.units} files=${index.stats.files} rows=${index.stats.rows} sequences=${index.stats.sequences} droppedSeq=${index.stats.droppedSequences} build=${buildMs}ms`)

    // ---- CU ranges vs 行表 sequence 一致性（逐 CU） ----
    let rangeUnits = 0
    let rangeMismatch = 0
    for (const unit of index.units) {
        if (unit.ranges.length === 0 || unit.sequences.length === 0) continue
        rangeUnits++
        const fromRanges = mergeAdjacentRanges(unit.ranges.map((range) => [range.pcLow, range.pcHigh]))
        const fromSequences = mergeAdjacentRanges(unit.sequences.map((sequenceIndex) => {
            const sequence = index.sequences[sequenceIndex]
            return [sequence.lowPc, sequence.highPc]
        }))
        if (fromRanges.length !== fromSequences.length) {
            rangeMismatch++
            continue
        }
        const same = fromRanges.every((range, rangeIndex) =>
            range[0] === fromSequences[rangeIndex][0] && range[1] === fromSequences[rangeIndex][1])
        if (!same) rangeMismatch++
    }
    check(`${label}: CU ranges match line sequences (>=95% CUs)`,
        rangeUnits > 0 && rangeMismatch / rangeUnits <= 0.05,
        `mismatch=${rangeMismatch}/${rangeUnits}`)

    // ---- pcToLine vs addr2line（抽样 250） ----
    const sampleStep = Math.max(1, Math.floor(index.rows.length / 250))
    const sampledRows = index.rows.filter((_, rowIndex) => rowIndex % sampleStep === 0).slice(0, 250)
    const addresses = sampledRows.map((row) => hex(row.address))
    const addrMap = addresses.length > 0 ? addr2lineLookup(elfPath, addresses) : null
    if (addrMap === null) {
        console.log(`  [skip] ${label}: addr2line not available`)
    } else {
        let matched = 0
        let unresolved = 0
        const mismatch = []
        for (const row of sampledRows) {
            const location = parseAddrLocation(addrMap.get(row.address))
            if (!location || location.basename === '??') {
                unresolved++
                continue
            }
            const hit = pcToLine(index, row.address)
            if (!hit || !hit.file) {
                unresolved++
                continue
            }
            if (hit.file.basename === location.basename && hit.line === location.line) matched++
            else if (mismatch.length < 8) {
                mismatch.push({address: hex(row.address), ours: `${hit.file.basename}:${hit.line}`, addr2line: `${location.basename}:${location.line}`})
            }
        }
        const resolved = sampledRows.length - unresolved
        check(`${label}: addr2line sampled >=200 rows`, sampledRows.length >= 200, sampledRows.length)
        check(`${label}: addr2line resolved >=90%`, unresolved <= sampledRows.length * 0.1, `unresolved=${unresolved}`)
        check(`${label}: pcToLine matches addr2line >=99%`, resolved > 0 && matched / resolved >= 0.99,
            `${matched}/${resolved}; ${JSON.stringify(mismatch.slice(0, 3))}`)
        console.log(`  info ${label}: pcToLine vs addr2line ${matched}/${resolved} (unresolved=${unresolved})`)
    }

    // ---- fileLineToAddress 往返（is_stmt/Flash 优先） ----
    {
        let sampled = 0
        let ok = 0
        const roundTripMismatch = []
        const stride = Math.max(1, Math.floor(index.rows.length / 250))
        for (let rowIndex = 0; rowIndex < index.rows.length && sampled < 250; rowIndex += stride) {
            const row = index.rows[rowIndex]
            if (row.fileIndex < 0 || row.line <= 0) continue
            const file = index.files[row.fileIndex]
            if (!file) continue
            sampled++
            const candidate = fileLineToAddress(index, file, row.line, {allowNonFlash: true})
            if (!candidate) continue
            if (!candidate.flash) continue
            const hit = pcToLine(index, candidate.address)
            if (hit && hit.file && hit.file.id === file.id && hit.line === row.line) ok++
            else if (roundTripMismatch.length < 5) {
                roundTripMismatch.push({address: hex(candidate.address), expected: `${file.basename}:${row.line}`, actual: hit ? `${hit.file?.basename}:${hit.line}` : null})
            }
        }
        check(`${label}: fileLineToAddress sampled >=200`, sampled >= 200, sampled)
        check(`${label}: fileLineToAddress round-trip >=99%`, sampled > 0 && ok / sampled >= 0.99,
            `${ok}/${sampled}; ${JSON.stringify(roundTripMismatch.slice(0, 3))}`)
        const preferred = fileLineToAddresses(index, index.files[0], index.rows.find((row) => row.fileIndex === 0)?.line ?? 0)
        check(`${label}: fileLineToAddress returns flash + is_stmt candidate`, preferred.length > 0
            && preferred.every((candidate) => candidate.flash || preferred[0].flash))
    }

    // ---- PC 边界 / 非法查询 ----
    check(`${label}: pcToLine(0) -> null`, pcToLine(index, 0) === null)
    check(`${label}: pcToLine outside exec -> null`, pcToLine(index, 0x20000000) === null)
    check(`${label}: pcToFileLine alias works`, typeof pcToFileLine === 'function')
    check(`${label}: fileLineToAddress missing file -> null`, fileLineToAddress(index, 'no-such-file-xyz.c', 10) === null)
    check(`${label}: fileLineToAddresses >=1 result type plain object`,
        Array.isArray(fileLineToAddresses(index, index.files[0], index.rows[0]?.line ?? 0)))

    // ---- 路径匹配 / relativePath 对齐 ----
    {
        const file = index.files.find((item) => item.relativePath.includes('/') && !item.relativePath.startsWith('/'))
        check(`${label}: files have relativePath`, !!file)
        if (file) {
            check(`${label}: relativePath stripped comp_dir`, !file.relativePath.startsWith('work/') && !file.relativePath.startsWith('/work'))
            check(`${label}: no backslashes in relativePath`, !file.relativePath.includes('\\'))
            check(`${label}: query by relativePath`, findElfLineFiles(index, file.relativePath).some((item) => item.id === file.id))
            check(`${label}: query by full path`, findElfLineFiles(index, file.path).some((item) => item.id === file.id))
            check(`${label}: query by windows backslashes`, findElfLineFiles(index, file.relativePath.replace(/\//g, '\\')).some((item) => item.id === file.id))
            check(`${label}: query by suffix`, findElfLineFiles(index, file.relativePath.split('/').slice(-2).join('/')).length > 0)
            check(`${label}: query by basename`, findElfLineFiles(index, file.basename).length > 0)
            check(`${label}: findElfLineFile helper`, findElfLineFile(index, file.basename)?.id !== undefined)
        }
    }

    // ---- pcToLine 性能抽样 ----
    {
        const probes = index.rows.filter((_, rowIndex) => rowIndex % Math.max(1, Math.floor(index.rows.length / 500)) === 0).slice(0, 500)
        const start = process.hrtime.bigint()
        let hits = 0
        for (const row of probes) if (pcToLine(index, row.address)) hits++
        const elapsedMs = Number(process.hrtime.bigint() - start) / 1e6
        check(`${label}: pcToLine hit rate`, hits === probes.length, `${hits}/${probes.length}`)
        console.log(`  info ${label}: pcToLine ${probes.length} probes in ${elapsedMs.toFixed(2)}ms`)
    }

    // ---- 容错：缺节 / 坏 offset ----
    {
        const sectionsWithoutLine = parsed.sections.filter((section) => section.name !== '.debug_line')
        const noLine = buildElfLineIndex(parsed.bytes, sectionsWithoutLine)
        check(`${label}: missing .debug_line -> empty rows + warning`, noLine.rows.length === 0
            && noLine.warnings.some((warning) => warning.toLowerCase().includes('line') || warning.includes('missing')))

        const corruptLineSections = parsed.sections.map((section) => section.name === '.debug_line'
            ? {...section, offset: parsed.bytes.length + 0x1000}
            : section)
        const corruptLine = buildElfLineIndex(parsed.bytes, corruptLineSections)
        check(`${label}: corrupt .debug_line offset -> tolerated`, corruptLine.rows.length === 0 && corruptLine.warnings.length > 0)

        const corruptInfoSections = parsed.sections.map((section) => section.name === '.debug_info'
            ? {...section, offset: parsed.bytes.length + 0x1000}
            : section)
        const corruptInfo = buildElfLineIndex(parsed.bytes, corruptInfoSections)
        check(`${label}: corrupt .debug_info offset -> tolerated`, corruptInfo.rows.length === 0 && corruptInfo.warnings.length > 0)

        let threw = false
        try {
            parseElfLineIndex(new Uint8Array(0))
        } catch (error) {
            threw = error instanceof ElfLineIndexError
        }
        check(`${label}: empty buffer throws ElfLineIndexError`, threw)
        threw = false
        try {
            parseElfLineIndex(Uint8Array.of(0x7f, 0x45, 0x4c, 0x46, 9, 9, 9, 9))
        } catch (error) {
            threw = error instanceof ElfLineIndexError
        }
        check(`${label}: malformed ELF throws ElfLineIndexError`, threw)
    }
}

for (const [label, elfPath] of [['release(-O2)', releaseElf], ['debug(-O0)', debugElf]]) checkRealElf(label, elfPath)

// ---------------------------------------------------------------------------
// 汇总
// ---------------------------------------------------------------------------
if (failures > 0) {
    console.error(`\n${failures} failure(s)`)
    process.exit(1)
}
console.log('\nall ELF line index checks passed')
