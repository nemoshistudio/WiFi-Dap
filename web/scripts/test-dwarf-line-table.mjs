#!/usr/bin/env node
/* eslint-env node */
/**
 * task-34 测试：DWARF `.debug_line` 行号表独立解析模块（web/src/utils/dwarfLineTable.ts）。
 *
 * 覆盖：
 *  1) synthetic v2/v3/v4/v5 line table：头部字段、v2–v4 字符串目录/文件表、
 *     v5 DW_LNCT_* + line_strp/strp/strx/string/data16(MD5) form、standard opcodes、
 *     special opcodes、v4 max_ops_per_inst(op_index)、extended opcodes
 *     (end_sequence/set_address/define_file/set_discriminator/未知 opcode)、
 *     DWARF64 格式、坏长度/截断/坏版本、line_range=0；
 *  2) 路径规范化/拼接（Unix/Windows）、addressToLine、file:line -> 地址、
 *     文件路径查询/前缀映射、buildFileLineIndex；
 *  3) 真实 ELF：只读提取 reference -O2 与 debug-O0 ELF 的 `.debug_line`，
 *     顺序枚举全部 contribution，与 `readelf -W --debug-dump=decodedline`
 *     逐行交叉核对；并用 `addr2line -f -i` 抽样 >=200 行验证 file:line。
 *
 * 只读：不修改仓库/设备，不 OTA、不 commit、不跑 Docker。临时产物只写 os.tmpdir()。
 *
 * 用法：node scripts/test-dwarf-line-table.mjs [release.elf] [debug.elf]
 *   默认：
 *     $HOME/reference firmware/firmware/reference-board/reference/firmware.elf
 *     $HOME/reference firmware/firmware/reference-board/reference-debug-O0/firmware.elf
 *   ELF 不存在时对应项 [skip]；readelf/addr2line 不可用时对应交叉验证 [skip]。
 */
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {execFileSync} from 'node:child_process'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const releaseElf = process.argv[2] ?? '$HOME/reference firmware/firmware/reference-board/reference/firmware.elf'
const debugElf = process.argv[3] ?? '$HOME/reference firmware/firmware/reference-board/reference-debug-O0/firmware.elf'

// ---------------------------------------------------------------------------
// 打包 TS 模块（项目自带 esbuild，离线）
// ---------------------------------------------------------------------------
const esbuild = path.join(root, 'node_modules', '.bin', 'esbuild')
if (!fs.existsSync(esbuild)) {
    console.error(`esbuild not found: ${esbuild} (run npm install in web/)`)
    process.exit(1)
}
const bundleFile = path.join(os.tmpdir(), `dwarf-line-table-${process.pid}.mjs`)
execFileSync(esbuild, [
    path.join(root, 'src', 'utils', 'dwarfLineTable.ts'),
    '--bundle', '--format=esm', '--platform=node', `--outfile=${bundleFile}`,
], {stdio: 'pipe'})

const {
    parseLineTable,
    addressToLine,
    pcToLine,
    findLineTableFile,
    resolveLineTableFiles,
    fileLineToAddresses,
    findFilesByPathPrefix,
    buildFileLineIndex,
    queryFileLineIndex,
    normalizeSourcePath,
    isAbsoluteSourcePath,
    sourceFileBasename,
    LineTableError,
    DW_LNCT_PATH,
    DW_LNCT_DIRECTORY_INDEX,
    DW_LNCT_TIMESTAMP,
    DW_LNCT_SIZE,
    DW_LNCT_MD5,
} = await import(bundleFile)

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

// ---------------------------------------------------------------------------
// 合成字节工具
// ---------------------------------------------------------------------------
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
const u16 = (value) => le(value, 2)
const u32 = (value) => le(value, 4)
const sbyte = (value) => Uint8Array.of(value & 0xff)
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
const sleb = (value) => {
    const out = []
    let rest = value
    for (;;) {
        let byte = rest & 0x7f
        rest >>= 7
        const sign = (byte & 0x40) !== 0
        if ((rest === 0 && !sign) || (rest === -1 && sign)) {
            out.push(byte)
            break
        }
        out.push(byte | 0x80)
    }
    return Uint8Array.from(out)
}
const cstring = (text) => {
    const bytes = new TextEncoder().encode(text)
    return Uint8Array.from([...bytes, 0])
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

// 字符串池：v5 line_strp/strp/strx form 测试用，返回字节 + 字符串偏移。
const makeStringPool = (strings) => {
    const offsets = new Map()
    const parts = []
    let offset = 0
    for (const text of strings) {
        offsets.set(text, offset)
        const bytes = cstring(text)
        parts.push(bytes)
        offset += bytes.length
    }
    return {bytes: cat(...parts), offsetOf: (text) => offsets.get(text) ?? 0}
}

// ---- 行号程序 opcode 构造 ----
const DW_LNS = {
    copy: 0x01,
    advancePc: 0x02,
    advanceLine: 0x03,
    setFile: 0x04,
    setColumn: 0x05,
    negateStmt: 0x06,
    setBasicBlock: 0x07,
    constAddPc: 0x08,
    fixedAdvancePc: 0x09,
    setPrologueEnd: 0x0a,
    setEpilogueBegin: 0x0b,
    setIsa: 0x0c,
}
const DW_LNE = {
    endSequence: 0x01,
    setAddress: 0x02,
    defineFile: 0x03,
    setDiscriminator: 0x04,
}
const op = {
    copy: () => Uint8Array.of(DW_LNS.copy),
    advancePc: (value) => cat([DW_LNS.advancePc], uleb(value)),
    advanceLine: (value) => cat([DW_LNS.advanceLine], sleb(value)),
    setFile: (value) => cat([DW_LNS.setFile], uleb(value)),
    setColumn: (value) => cat([DW_LNS.setColumn], uleb(value)),
    negateStmt: () => Uint8Array.of(DW_LNS.negateStmt),
    setBasicBlock: () => Uint8Array.of(DW_LNS.setBasicBlock),
    constAddPc: () => Uint8Array.of(DW_LNS.constAddPc),
    fixedAdvancePc: (value) => cat([DW_LNS.fixedAdvancePc], u16(value)),
    setPrologueEnd: () => Uint8Array.of(DW_LNS.setPrologueEnd),
    setEpilogueBegin: () => Uint8Array.of(DW_LNS.setEpilogueBegin),
    setIsa: (value) => cat([DW_LNS.setIsa], uleb(value)),
    special: (value) => Uint8Array.of(value),
    endSequence: () => cat([0], uleb(1), [DW_LNE.endSequence]),
    setAddress: (value, addressSize = 4, segmentSelectorSize = 0) =>
        cat([0], uleb(1 + segmentSelectorSize + addressSize), [DW_LNE.setAddress], le(0, segmentSelectorSize), le(value, addressSize)),
    defineFile: (name, directoryIndex = 1, mtime = 0, size = 0) => {
        const body = cat(cstring(name), uleb(directoryIndex), uleb(mtime), uleb(size))
        return cat([0], uleb(1 + body.length), [DW_LNE.defineFile], body)
    },
    setDiscriminator: (value) => cat([0], uleb(1 + uleb(value).length), [DW_LNE.setDiscriminator], uleb(value)),
    unknownExtended: (subOpcode, payload = Uint8Array.of()) => cat([0], uleb(1 + payload.length), [subOpcode], payload),
}

/** 标准 opcode 长度表（opcode_base=13 时对应 DW_LNS 1..12）。 */
const DEFAULT_STANDARD_LENGTHS = [0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1]

// ---- v5 目录/文件表 form 写入 ----
const writeLineFormValue = (form, value, pools, offsetSize) => {
    switch (form) {
        case 0x08: // DW_FORM_string
            return cstring(String(value))
        case 0x1f: // DW_FORM_line_strp
            return le(pools.lineStr.offsetOf(String(value)), offsetSize)
        case 0x0e: // DW_FORM_strp
            return le(pools.str.offsetOf(String(value)), offsetSize)
        case 0x1a: // DW_FORM_strx
            return uleb(Number(value))
        case 0x25: // DW_FORM_strx1
            return u8(Number(value))
        case 0x28: // DW_FORM_strx4
            return u32(Number(value))
        case 0x1e: // DW_FORM_data16
            return Uint8Array.from(value)
        case 0x0b:
            return u8(Number(value))
        case 0x05:
            return u16(Number(value))
        case 0x06:
            return u32(Number(value))
        case 0x0f:
            return uleb(Number(value))
        default:
            throw new Error(`test builder: unsupported form 0x${form.toString(16)}`)
    }
}

const v5EntryBytes = (formats, values, pools, offsetSize) => {
    const parts = []
    for (const format of formats) {
        let value
        switch (format.content) {
            case DW_LNCT_PATH:
                value = values.path
                break
            case DW_LNCT_DIRECTORY_INDEX:
                value = values.directoryIndex ?? 0
                break
            case DW_LNCT_TIMESTAMP:
                value = values.mtime ?? 0
                break
            case DW_LNCT_SIZE:
                value = values.size ?? 0
                break
            case DW_LNCT_MD5:
                value = values.md5 ?? new Array(16).fill(0)
                break
            default:
                throw new Error(`test builder: unknown DW_LNCT_* ${format.content}`)
        }
        parts.push(writeLineFormValue(format.form, value, pools, offsetSize))
    }
    return cat(...parts)
}

/**
 * 构造一个 .debug_line contribution。
 * v2–v4：directories:string[]，files:[{name,directoryIndex,mtime,size}]
 * v5：v5.directoryFormats/fileFormats + directories:[{path}] + files:[{path,directoryIndex,mtime,size,md5}]
 */
const buildLineTable = ({
    version = 4,
    dwarf64 = false,
    addressSize = 4,
    segmentSelectorSize = 0,
    minInst = 1,
    maxOps = 1,
    defaultIsStmt = 1,
    lineBase = -5,
    lineRange = 14,
    opcodeBase = 13,
    standardOpcodeLengths = null,
    directories = [],
    files = [],
    v5 = null,
    program = Uint8Array.of(),
    lineStr = null,
    str = null,
}) => {
    const offsetSize = dwarf64 ? 8 : 4
    let header = cat(u8(minInst))
    if (version >= 4) header = cat(header, u8(maxOps))
    header = cat(header, u8(defaultIsStmt), sbyte(lineBase), u8(lineRange), u8(opcodeBase))
    const lengths = standardOpcodeLengths ?? DEFAULT_STANDARD_LENGTHS.slice(0, opcodeBase - 1)
    for (const length of lengths) header = cat(header, uleb(length))
    if (version >= 5) {
        if (!v5) throw new Error('v5 builder requires v5 formats')
        header = cat(header, u8(v5.directoryFormats.length))
        for (const format of v5.directoryFormats) header = cat(header, uleb(format.content), uleb(format.form))
        header = cat(header, uleb(directories.length))
        for (const directory of directories) {
            header = cat(header, v5EntryBytes(v5.directoryFormats, directory, {lineStr, str}, offsetSize))
        }
        header = cat(header, u8(v5.fileFormats.length))
        for (const format of v5.fileFormats) header = cat(header, uleb(format.content), uleb(format.form))
        header = cat(header, uleb(files.length))
        for (const file of files) {
            header = cat(header, v5EntryBytes(v5.fileFormats, file, {lineStr, str}, offsetSize))
        }
    } else {
        for (const directory of directories) header = cat(header, cstring(directory))
        header = cat(header, cstring(''))
        for (const file of files) {
            header = cat(
                header,
                cstring(file.name),
                uleb(file.directoryIndex ?? 1),
                uleb(file.mtime ?? 0),
                uleb(file.size ?? 0),
            )
        }
        header = cat(header, cstring(''))
    }
    const prefix = cat(u16(version), version >= 5 ? cat(u8(addressSize), u8(segmentSelectorSize)) : Uint8Array.of(), le(header.length, offsetSize))
    const body = cat(prefix, header, program)
    if (dwarf64) return cat(u32(0xffffffff), le(body.length, 8), body)
    return cat(u32(body.length), body)
}

const rowSummary = (row) => ({
    address: row.address,
    line: row.line,
    file: row.file ? row.file.name : null,
    end: row.endSequence,
    stmt: row.isStmt,
    opIndex: row.opIndex,
})
const rowsSummary = (table) => table.rows.map(rowSummary)

// ---------------------------------------------------------------------------
// 1. synthetic：v4 基础 + v2/v3 兼容
// ---------------------------------------------------------------------------
console.log('unit: synthetic .debug_line v2/v3/v4')
{
    const program = cat(
        op.setAddress(0x1000),
        op.special(20), // adjusted=7 -> line +2, address +0 -> line 3
        op.advanceLine(1),
        op.copy(), // line 4 @0x1000
        op.advancePc(3),
        op.special(60), // adjusted=47 -> address +3, line +0 -> 0x1003 line 4
        op.setDiscriminator(7),
        op.setColumn(12),
        op.advanceLine(-2),
        op.copy(), // line 2 @0x1003 column 12 discriminator 7
        op.endSequence(), // end @0x1003
    )
    const data = buildLineTable({
        version: 4,
        directories: ['/work'],
        files: [{name: 'src/main.c', directoryIndex: 1}],
        program,
    })
    const table = parseLineTable({data, offset: 0, addressSize: 4})
    checkEq('v4: table version', table.version, 4)
    checkEq('v4: row count (incl. end_sequence row)', table.rows.length, 5)
    checkEq('v4: sequence count', table.sequences.length, 1)
    const expectedRows = [
        {address: 0x1000, line: 3, file: 'src/main.c', end: false, stmt: true, opIndex: 0},
        {address: 0x1000, line: 4, file: 'src/main.c', end: false, stmt: true, opIndex: 0},
        {address: 0x1006, line: 4, file: 'src/main.c', end: false, stmt: true, opIndex: 0},
        {address: 0x1006, line: 2, file: 'src/main.c', end: false, stmt: true, opIndex: 0},
        {address: 0x1006, line: 2, file: 'src/main.c', end: true, stmt: true, opIndex: 0},
    ]
    checkEq('v4: rows (address/line/file/end)', JSON.stringify(rowsSummary(table)), JSON.stringify(expectedRows))
    checkEq('v4: sequence low', table.sequences[0].lowPc, 0x1000)
    checkEq('v4: sequence high (end_sequence exclusive)', table.sequences[0].highPc, 0x1006)
    check('v4: sequence terminated', table.sequences[0].terminated)
    checkEq('v4: file path joined with comp_dir', table.files[0].path, '/work/src/main.c')
    checkEq('v4: file dwarfIndex', table.files[0].dwarfIndex, 1)
    checkEq('v4: file directory index', table.files[0].directoryIndex, 1)
    checkEq('v4: endOffset = contribution end', table.endOffset, data.length)
    checkEq('v4: no warnings', table.warnings.join('|'), '')

    // 同一 program 的 v2/v3 表：除 version 外应一致
    for (const version of [2, 3]) {
        const legacy = parseLineTable({
            data: buildLineTable({
                version,
                directories: ['/work'],
                files: [{name: 'src/main.c', directoryIndex: 1}],
                program,
            }),
            offset: 0,
            addressSize: 4,
        })
        checkEq(`v${version}: version`, legacy.version, version)
        checkEq(`v${version}: rows identical to v4`, JSON.stringify(rowsSummary(legacy)), JSON.stringify(rowsSummary(table)))
        checkEq(`v${version}: files identical`, JSON.stringify(legacy.files.map((file) => file.path)), JSON.stringify(table.files.map((file) => file.path)))
    }
}
{
    // 多 sequence：地址不连续、切换 sequence 后 state 重置
    const program = cat(
        op.setAddress(0x1000),
        op.advanceLine(9),
        op.copy(),
        op.advancePc(0x10),
        op.advanceLine(1),
        op.copy(),
        op.advancePc(0x10),
        op.endSequence(),
        op.setAddress(0x2000),
        op.advanceLine(4),
        op.copy(),
        op.advancePc(0x10),
        op.endSequence(),
    )
    const table = parseLineTable({
        data: buildLineTable({version: 4, directories: ['/work'], files: [{name: 'a.c', directoryIndex: 1}], program}),
        offset: 0,
        addressSize: 4,
    })
    checkEq('sequences: count', table.sequences.length, 2)
    checkEq('sequences: seq0 rows', `${table.sequences[0].startRow}..${table.sequences[0].endRow}`, '0..2')
    checkEq('sequences: seq0 range', `${hex(table.sequences[0].lowPc)}..${hex(table.sequences[0].highPc)}`, '0x1000..0x1020')
    checkEq('sequences: seq1 range', `${hex(table.sequences[1].lowPc)}..${hex(table.sequences[1].highPc)}`, '0x2000..0x2010')
    checkEq('sequences: row count', table.rows.length, 5)
    checkEq('multi-seq: state reset line', table.rows[3].line, 5)
    checkEq('multi-seq: seq index', table.rows[3].sequence, 1)
}
{
    // v4 max_ops_per_inst=2：advance_pc 的 op_index/地址推进；special opcode 推进
    const program = cat(
        op.setAddress(0),
        op.advanceLine(1),
        op.copy(), // 0/0
        op.advancePc(1),
        op.copy(), // 0/1
        op.advancePc(1),
        op.copy(), // 4/0
        op.advancePc(3),
        op.copy(), // 8/1
        op.special(13 + 14), // adjusted=14 -> op advance 1, line -5, emits row 12/0
        op.endSequence(),
    )
    const table = parseLineTable({
        data: buildLineTable({
            version: 4,
            minInst: 4,
            maxOps: 2,
            directories: ['/work'],
            files: [{name: 'vliw.c', directoryIndex: 1}],
            program,
        }),
        offset: 0,
        addressSize: 4,
    })
    checkEq('max_ops: maxOpsPerInstruction', table.maxOpsPerInstruction, 2)
    checkEq(
        'max_ops: address/op_index progression',
        table.rows.filter((row) => !row.endSequence).map((row) => `${row.address}/${row.opIndex}`).join(' '),
        '0/0 0/1 4/0 8/1 12/0',
    )
    checkEq('max_ops: advance_pc op_index reset by set_address', table.rows[0].opIndex, 0)
}

// ---------------------------------------------------------------------------
// 2. synthetic：DWARF5 header / forms / file 语义
// ---------------------------------------------------------------------------
console.log('\nunit: synthetic .debug_line v5')
{
    const lineStr = makeStringPool(['/project', '/project/src', 'main.c', 'util.h'])
    const program = cat(
        op.setAddress(0x4000),
        op.advanceLine(4),
        op.copy(), // 初始 file=1 -> files[1] (util.h)
        op.setFile(0),
        op.copy(), // files[0] (main.c @ dir 0)
        op.advancePc(4),
        op.endSequence(),
    )
    const data = buildLineTable({
        version: 5,
        addressSize: 4,
        v5: {
            directoryFormats: [{content: DW_LNCT_PATH, form: 0x1f}],
            fileFormats: [
                {content: DW_LNCT_PATH, form: 0x1f},
                {content: DW_LNCT_DIRECTORY_INDEX, form: 0x0f},
                {content: DW_LNCT_TIMESTAMP, form: 0x06},
                {content: DW_LNCT_SIZE, form: 0x0f},
                {content: DW_LNCT_MD5, form: 0x1e},
            ],
        },
        directories: [{path: '/project'}, {path: '/project/src'}],
        files: [
            {path: 'main.c', directoryIndex: 0, mtime: 11, size: 22, md5: new Array(16).fill(0xab)},
            {path: 'util.h', directoryIndex: 1, mtime: 33, size: 44, md5: new Array(16).fill(0xcd)},
        ],
        lineStr,
        program,
    })
    const table = parseLineTable({data, offset: 0, lineStr: lineStr.bytes, compDir: '/project'})
    checkEq('v5: version', table.version, 5)
    checkEq('v5: address size from header', table.addressSize, 4)
    checkEq('v5: directory table', JSON.stringify(table.directories), JSON.stringify(['/project', '/project/src']))
    checkEq('v5: file count', table.files.length, 2)
    checkEq('v5: file0 name', table.files[0].name, 'main.c')
    checkEq('v5: file0 path (dir index 0 = comp_dir)', table.files[0].path, '/project/main.c')
    checkEq('v5: file1 path', table.files[1].path, '/project/src/util.h')
    checkEq('v5: file metadata', `${table.files[1].mtime}/${table.files[1].size}`, '33/44')
    check('v5: file md5 captured', table.files[1].md5 && table.files[1].md5.length === 16 && table.files[1].md5[0] === 0xcd)
    checkEq('v5: initial file register = 1', table.rows[0].fileIndex, 1)
    checkEq('v5: row file=1 -> util.h', table.rows[0].file?.name, 'util.h')
    checkEq('v5: set_file(0) -> main.c', table.rows[1].file?.name, 'main.c')
    checkEq('v5: rows count', table.rows.length, 3)
    check('v5: terminated sequence', table.sequences[0]?.terminated === true)
}
{
    // v5：FORM_string / FORM_strp / FORM_data4 / data16(MD5)
    const lineStr = makeStringPool([])
    const str = makeStringPool(['inc', 'feature.c'])
    const data = buildLineTable({
        version: 5,
        addressSize: 4,
        v5: {
            directoryFormats: [{content: DW_LNCT_PATH, form: 0x08}],
            fileFormats: [
                {content: DW_LNCT_PATH, form: 0x0e},
                {content: DW_LNCT_DIRECTORY_INDEX, form: 0x0b},
                {content: DW_LNCT_TIMESTAMP, form: 0x06},
                {content: DW_LNCT_SIZE, form: 0x0f},
                {content: DW_LNCT_MD5, form: 0x1e},
            ],
        },
        directories: [{path: 'inc'}],
        files: [{path: 'feature.c', directoryIndex: 0, mtime: 0x12345678, size: 99, md5: new Array(16).fill(0x11)}],
        lineStr,
        str,
        program: cat(op.setAddress(0x100), op.copy(), op.endSequence()),
    })
    const table = parseLineTable({data, offset: 0, lineStr: lineStr.bytes, str: str.bytes, compDir: '/project'})
    checkEq('v5 forms: file path via strp', table.files[0].path, '/project/inc/feature.c')
    checkEq('v5 forms: directory via string', table.directories[0], '/project/inc')
    checkEq('v5 forms: mtime data4', table.files[0].mtime, 0x12345678)
}
{
    // v5：DW_FORM_strx + .debug_str_offsets（base=0，4 字节条目）
    const str = makeStringPool(['offsets.c'])
    const strOffsets = cat(le(str.offsetOf('offsets.c'), 4))
    const data = buildLineTable({
        version: 5,
        addressSize: 4,
        v5: {
            directoryFormats: [{content: DW_LNCT_PATH, form: 0x08}],
            fileFormats: [
                {content: DW_LNCT_PATH, form: 0x1a},
                {content: DW_LNCT_DIRECTORY_INDEX, form: 0x0f},
            ],
        },
        directories: [{path: '/base'}],
        files: [{path: 0, directoryIndex: 0}],
        str,
        program: cat(op.setAddress(0x200), op.copy(), op.endSequence()),
    })
    const table = parseLineTable({data, offset: 0, str: str.bytes, strOffsets, strOffsetsBase: 0, compDir: '/base'})
    checkEq('v5 strx: file path resolved', table.files[0].path, '/base/offsets.c')
}
{
    // v5：DW_FORM_strx1 / DW_FORM_strx4 索引形式
    for (const [form, label] of [[0x25, 'strx1'], [0x28, 'strx4']]) {
        const str = makeStringPool(['indexed.c'])
        const strOffsets = cat(le(str.offsetOf('indexed.c'), 4))
        const data = buildLineTable({
            version: 5,
            addressSize: 4,
            v5: {
                directoryFormats: [{content: DW_LNCT_PATH, form: 0x08}],
                fileFormats: [
                    {content: DW_LNCT_PATH, form},
                    {content: DW_LNCT_DIRECTORY_INDEX, form: 0x0f},
                ],
            },
            directories: [{path: '/base'}],
            files: [{path: 0, directoryIndex: 0}],
            str,
            program: cat(op.setAddress(0x300), op.copy(), op.endSequence()),
        })
        const table = parseLineTable({data, offset: 0, str: str.bytes, strOffsets, strOffsetsBase: 0, compDir: '/base'})
        checkEq(`v5 ${label}: file path resolved`, table.files[0].path, '/base/indexed.c')
    }
}
{
    // v4 DWARF64 格式（unit_length 0xffffffff + 8 字节 header_length）
    const table = parseLineTable({
        data: buildLineTable({
            version: 4,
            dwarf64: true,
            directories: ['/work'],
            files: [{name: 'w.c', directoryIndex: 1}],
            program: cat(op.setAddress(0x40), op.copy(), op.endSequence()),
        }),
        offset: 0,
        addressSize: 4,
    })
    check('v4 DWARF64: dwarf64 detected', table.dwarf64 === true)
    checkEq('v4 DWARF64: row parsed', table.rows[0].address, 0x40)
    checkEq('v4 DWARF64: file path', table.files[0].path, '/work/w.c')
}
{
    // DWARF64 line table：unit_length 0xffffffff + 8 字节 header_length / line_strp 偏移
    const lineStr = makeStringPool(['/wide', 'wide.c'])
    const data = buildLineTable({
        version: 5,
        dwarf64: true,
        addressSize: 8,
        v5: {
            directoryFormats: [{content: DW_LNCT_PATH, form: 0x1f}],
            fileFormats: [
                {content: DW_LNCT_PATH, form: 0x1f},
                {content: DW_LNCT_DIRECTORY_INDEX, form: 0x0f},
            ],
        },
        directories: [{path: '/wide'}],
        files: [{path: 'wide.c', directoryIndex: 0}],
        lineStr,
        program: cat(op.setAddress(0x100000000, 8), op.copy(), op.endSequence()),
    })
    const table = parseLineTable({data, offset: 0, lineStr: lineStr.bytes, compDir: '/wide'})
    check('DWARF64: dwarf64 detected', table.dwarf64 === true)
    checkEq('DWARF64: file path via 8-byte line_strp', table.files[0].path, '/wide/wide.c')
    checkEq('DWARF64: 8-byte address row', table.rows[0].address, 0x100000000)
}
{
    // extended opcodes：define_file / set_discriminator / 未知扩展 opcode
    const program = cat(
        op.setAddress(0x500),
        op.setBasicBlock(),
        op.setPrologueEnd(),
        op.setEpilogueBegin(),
        op.setIsa(3),
        op.setDiscriminator(9),
        op.copy(),
        op.defineFile('extra.c', 1),
        op.setFile(2), // v4: dwarf file number 2 -> 第二个文件（define_file 追加）
        op.copy(),
        op.unknownExtended(0x80, Uint8Array.of(1, 2, 3)),
        op.endSequence(),
    )
    const table = parseLineTable({
        data: buildLineTable({version: 4, directories: ['/work'], files: [{name: 'base.c', directoryIndex: 1}], program}),
        offset: 0,
        addressSize: 4,
    })
    const first = table.rows[0]
    check('extended: define_file appends file entry', table.files.length === 2 && table.files[1].name === 'extra.c')
    checkEq('extended: second row file = defined file', table.rows[1].file?.name, 'extra.c')
    check('extended: set_* flags recorded', first.basicBlock && first.prologueEnd && first.epilogueBegin)
    checkEq('extended: isa recorded', first.isa, 3)
    checkEq('extended: discriminator recorded', first.discriminator, 9)
    check('extended: unknown extended opcode skipped with warning', table.warnings.some((warning) => warning.includes('unknown extended')))
    checkEq('extended: sequence terminated', table.sequences[0].terminated, true)
}
{
    // 未知 standard opcode（opcode_base=14，第 13 个标准 opcode 有 1 个操作数）
    const standardLengths = [...DEFAULT_STANDARD_LENGTHS, 1]
    const program = cat(op.setAddress(0x100), [13], uleb(0x123), op.copy(), op.endSequence())
    const table = parseLineTable({
        data: buildLineTable({
            version: 4,
            opcodeBase: 14,
            standardOpcodeLengths: standardLengths,
            directories: ['/work'],
            files: [{name: 'a.c', directoryIndex: 1}],
            program,
        }),
        offset: 0,
        addressSize: 4,
    })
    checkEq('unknown standard opcode: rows parsed', table.rows.length, 2)
    check('unknown standard opcode: warning', table.warnings.some((warning) => warning.includes('unknown standard')))
}
{
    // line_range=0（非法）容忍 + const_add_pc/fixed_advance_pc/negate_stmt
    const program = cat(
        op.setAddress(0x1000),
        op.negateStmt(),
        op.constAddPc(),
        op.fixedAdvancePc(0x20),
        op.copy(),
        op.endSequence(),
    )
    const table = parseLineTable({
        data: buildLineTable({
            version: 4,
            lineRange: 0,
            directories: ['/work'],
            files: [{name: 'a.c', directoryIndex: 1}],
            program,
        }),
        offset: 0,
        addressSize: 4,
    })
    check('line_range 0: tolerated with warning', table.warnings.some((warning) => warning.includes('line_range')))
    checkEq('line_range 0: lineRange normalized 1', table.lineRange, 1)
    // const_add_pc: ((255-13)/14)=17 -> address += 17；fixed +0x20 -> 0x1000+17+32 = 0x1041
    checkEq('const_add_pc + fixed_advance_pc address', table.rows[0].address, 0x1000 + 242 + 0x20)
    check('negate_stmt toggled', table.rows[0].isStmt === false)
}
{
    // 头部/程序错误
    let error = null
    try {
        const valid = buildLineTable({
            version: 5,
            addressSize: 4,
            v5: {directoryFormats: [{content: DW_LNCT_PATH, form: 0x08}], fileFormats: [{content: DW_LNCT_PATH, form: 0x08}]},
            directories: [],
            files: [],
            program: Uint8Array.of(),
        })
        const unsupported = new Uint8Array(valid)
        unsupported[4] = 6 // version (u16 LE) 改成 6
        unsupported[5] = 0
        parseLineTable({data: unsupported, addrSize: 4})
    } catch (caught) {
        error = caught
    }
    check('errors: unsupported version throws LineTableError', error instanceof LineTableError)
    error = null
    try {
        const good = buildLineTable({version: 4, directories: [], files: [], program: Uint8Array.of()})
        // 把 header_length 改成超大值（offset 0: unit_length(4) version(2) header_length(4)）
        const broken = new Uint8Array(good)
        broken[6] = 0xff
        broken[7] = 0xff
        broken[8] = 0xff
        broken[9] = 0x7f
        parseLineTable({data: broken, addressSize: 4})
    } catch (caught) {
        error = caught
    }
    check('errors: header_length beyond unit throws LineTableError', error instanceof LineTableError)
    error = null
    try {
        parseLineTable({data: Uint8Array.of(1, 2), addressSize: 4})
    } catch (caught) {
        error = caught
    }
    check('errors: truncated contribution throws LineTableError', error instanceof LineTableError)

    // 程序截断：copy 后缺 end_sequence
    const truncated = parseLineTable({
        data: buildLineTable({
            version: 4,
            directories: ['/work'],
            files: [{name: 'a.c', directoryIndex: 1}],
            program: cat(op.setAddress(0x100), op.copy()),
        }),
        offset: 0,
        addressSize: 4,
    })
    check('errors: missing end_sequence -> warning', truncated.warnings.some((warning) => warning.includes('without DW_LNE_end_sequence')))
    check('errors: missing end_sequence sequence marked un-terminated', truncated.sequences[0]?.terminated === false)
    checkEq('errors: partial rows kept', truncated.rows.length, 1)
}
{
    // 从贡献中间 offset 解析（.debug_line 多 CU 连续存放）
    const first = buildLineTable({version: 4, directories: ['/work'], files: [{name: 'a.c', directoryIndex: 1}], program: cat(op.setAddress(0x10), op.copy(), op.endSequence())})
    const second = buildLineTable({version: 4, directories: ['/work'], files: [{name: 'b.c', directoryIndex: 1}], program: cat(op.setAddress(0x20), op.copy(), op.endSequence())})
    const data = cat(first, second)
    const table = parseLineTable({data, offset: first.length, addressSize: 4})
    checkEq('multi-contribution: offset parse file', table.files[0].name, 'b.c')
    checkEq('multi-contribution: offset parse address', table.rows[0].address, 0x20)
    checkEq('multi-contribution: endOffset', table.endOffset, data.length)
}

// ---------------------------------------------------------------------------
// 3. 路径 / 查询纯函数
// ---------------------------------------------------------------------------
console.log('\nunit: paths & lookups')
{
    checkEq('path: normalize windows', normalizeSourcePath('C:\\work\\src\\..\\b\\file.c'), 'C:/work/b/file.c')
    checkEq('path: normalize unix', normalizeSourcePath('//a///b/./c/../d'), '/a/b/d')
    checkEq('path: normalize relative', normalizeSourcePath('src/./main.c'), 'src/main.c')
    checkEq('path: basename', sourceFileBasename('C:/work/a/b.c'), 'b.c')
    check('path: absolute detection', isAbsoluteSourcePath('/a/b') && isAbsoluteSourcePath('C:\\a') && !isAbsoluteSourcePath('a/b'))
}
{
    const program = cat(
        op.setAddress(0x1000),
        op.advanceLine(9),
        op.copy(), // line 10 @0x1000 (a.c)
        op.setFile(2), // -> b.c
        op.copy(), // line 10 @0x1000 (b.c)
        op.advancePc(0x10),
        op.advanceLine(5),
        op.copy(), // line 15 @0x1010 (b.c)
        op.advancePc(0x10),
        op.endSequence(),
        op.setAddress(0x3000),
        op.advanceLine(4),
        op.copy(), // line 5 @0x3000 (a.c)
        op.advancePc(0x10),
        op.endSequence(),
    )
    const table = parseLineTable({
        data: buildLineTable({
            version: 4,
            directories: ['/work', '/work/inc'],
            files: [
                {name: 'src/a.c', directoryIndex: 1},
                {name: 'b.h', directoryIndex: 2},
            ],
            program,
        }),
        offset: 0,
        addressSize: 4,
    })
    checkEq('lookup: file count', table.files.length, 2)
    checkEq('lookup: second file path', table.files[1].path, '/work/inc/b.h')

    const hit = addressToLine(table, 0x1004)
    checkEq('pcToLine: hit address', hit?.address, 0x1000)
    checkEq('pcToLine: hit line', hit?.line, 10)
    checkEq('addr2line: between rows keeps previous row', addressToLine(table, 0x100f)?.address, 0x1000)
    checkEq('addr2line: next row wins', addressToLine(table, 0x1010)?.line, 15)
    checkEq('addr2line: pc at sequence end is not covered', addressToLine(table, 0x1020), null)
    checkEq('addr2line: second sequence hit', addressToLine(table, 0x3005)?.line, 5)
    checkEq('pcToLine alias works', pcToLine(table, 0x3005)?.line, 5)

    checkEq('lookup: exact path match', findLineTableFile(table, '/work/src/a.c')?.name, 'src/a.c')
    checkEq('lookup: suffix match', findLineTableFile(table, 'inc/b.h')?.name, 'b.h')
    checkEq('lookup: basename match', findLineTableFile(table, 'a.c')?.name, 'src/a.c')
    checkEq('lookup: no match', findLineTableFile(table, 'missing.c'), null)
    checkEq('lookup: resolve returns all suffix matches', resolveLineTableFiles(table, 'b.h').length, 1)
    checkEq('lookup: file prefix listing', JSON.stringify(findFilesByPathPrefix(table, '/work/inc').map((file) => file.name)), JSON.stringify(['b.h']))

    const addresses = fileLineToAddresses(table, 'a.c', 10)
    checkEq('file:line -> address count', addresses.length, 1)
    checkEq('file:line -> address value', addresses[0].address, 0x1000)
    checkEq('file:line -> multiple sequences', fileLineToAddresses(table, 'a.c', 5).map((item) => hex(item.address)).join(','), '0x3000')
    checkEq('file:line -> no rows for end line', fileLineToAddresses(table, 'a.c', 10)[0].isStmt, true)

    const index = buildFileLineIndex(table)
    const indexed = queryFileLineIndex(table, index, 'a.c', 10)
    checkEq('file:line index parity', JSON.stringify(indexed), JSON.stringify(addresses))
    check('file:line index has both files', index.size === 2)
}

// ---------------------------------------------------------------------------
// 4. 真实 ELF 交叉验证
// ---------------------------------------------------------------------------
console.log('\nreal ELF')

/** 最小只读 ELF section 表解析（支持 ELF32/64 LE）。 */
const readElfSections = (file) => {
    const raw = fs.readFileSync(file)
    if (raw.length < 0x40 || raw[0] !== 0x7f || raw[1] !== 0x45 || raw[2] !== 0x4c || raw[3] !== 0x46) {
        throw new Error('not an ELF file')
    }
    const is64 = raw[4] === 2
    if (raw[5] !== 1) throw new Error('only little-endian ELF supported')
    const u16 = (offset) => raw[offset] | (raw[offset + 1] << 8)
    const u32 = (offset) => raw.readUInt32LE(offset)
    const u64 = (offset) => Number(raw.readBigUInt64LE(offset))
    const eShoff = is64 ? u64(0x28) : u32(0x20)
    const eShentsize = is64 ? u16(0x3a) : u16(0x2e)
    const eShnum = is64 ? u16(0x3c) : u16(0x30)
    const eShstrndx = is64 ? u16(0x3e) : u16(0x32)
    const sections = []
    for (let index = 0; index < eShnum; index++) {
        const base = eShoff + index * eShentsize
        sections.push({
            nameOffset: u32(base),
            type: u32(base + 4),
            flags: is64 ? u64(base + 0x8) : u32(base + 0x8),
            addr: is64 ? u64(base + 0x10) : u32(base + 0x0c),
            offset: is64 ? u64(base + 0x18) : u32(base + 0x10),
            size: is64 ? u64(base + 0x20) : u32(base + 0x14),
        })
    }
    const strtab = sections[eShstrndx]
    const readName = (offset) => {
        const start = strtab.offset + offset
        const end = raw.indexOf(0, start)
        return raw.toString('utf8', start, end)
    }
    const byName = new Map()
    for (const section of sections) byName.set(readName(section.nameOffset), section)
    // SHF_EXECINSTR = 0x4：用于过滤“重定位为 0/小地址”的行表行（addr2line 无法解析）。
    const executableRanges = sections
        .filter((section) => (section.flags & 0x4) !== 0 && section.addr > 0 && section.size > 0)
        .map((section) => [section.addr, section.addr + section.size])
    return {raw, is64, sections: byName, executableRanges}
}

const sectionBytesOf = (elf, name) => {
    const section = elf.sections.get(name)
    if (!section || section.offset + section.size > elf.raw.length) return null
    return elf.raw.subarray(section.offset, section.offset + section.size)
}

/** 顺序枚举 .debug_line 中的全部 contribution（与 readelf 顺序一致）。 */
const enumerateLineTables = (lineSection, options) => {
    const tables = []
    let offset = 0
    let guard = 0
    while (offset < lineSection.length && guard++ < 100000) {
        try {
            const table = parseLineTable({data: lineSection, offset, ...options})
            tables.push(table)
            if (table.endOffset <= offset) break
            offset = table.endOffset
        } catch (error) {
            tables.push({error, offset})
            break
        }
    }
    return tables
}

const execOrNull = (file, args, maxBuffer = 512 * 1024 * 1024) => {
    try {
        return execFileSync(file, args, {maxBuffer}).toString()
    } catch {
        return null
    }
}

/** 解析 readelf -W --debug-dump=decodedline 输出（.debug_line 顺序 = contribution 顺序）。 */
const parseReadelfDecodedLine = (text) => {
    const blocks = []
    const unmatched = []
    let current = null
    const rowPattern = /^(.*?)\s{2,}(-|\d+)\s{2,}(0x[0-9a-f]+|0)(?:\[(\d+)\])?(?:\s{2,}(\d+))?(\s+x|\s*)$/
    for (const rawLine of text.split('\n')) {
        const line = rawLine.replace(/\s+$/, '')
        if (!line) continue
        if (line.startsWith('Contents of') || line.startsWith('File name')) continue
        if (line.startsWith('CU: ')) {
            if (line.endsWith(':')) {
                current = {name: line.slice(4, -1), rows: []}
                blocks.push(current)
            }
            // "CU: No directory table" / "CU: Empty file name table" 等诊断行跳过
            continue
        }
        if (!current) continue
        const match = line.match(rowPattern)
        if (!match) {
            // readelf 在 DW_LNS_set_file / set_isa / 未知 opcode 时会打印诊断行（不以 CU 开头）
            const diagnostic = line.endsWith(':') || /(^\[Use file )|(^<over large )|(^\s+Set ISA to )|(^\s+Unknown opcode )|(^UNKNOWN \()/.test(line)
            if (!diagnostic && unmatched.length < 20) unmatched.push(line)
            continue
        }
        current.rows.push({
            file: match[1].trim(),
            line: match[2] === '-' ? null : parseInt(match[2], 10),
            address: parseInt(match[3], match[3] === '0' ? 10 : 16),
            opIndex: match[4] === undefined ? 0 : parseInt(match[4], 10),
            view: match[5] === undefined ? 0 : parseInt(match[5], 10),
            isStmt: match[6].trim() === 'x',
        })
    }
    return {blocks, unmatched}
}

/** 用 addr2line -f -i -a 抽样验证 file:line。 */
const makeExecutableFilter = (ranges) => (address) =>
    ranges.some(([start, end]) => address >= start && address < end)

const addr2lineCheck = (elfPath, tables, expectedSamples, isExecutable) => {
    const samples = []
    for (const table of tables) {
        if (table.error) continue
        for (const row of table.rows) {
            if (row.endSequence || !row.file) continue
            if (isExecutable && !isExecutable(row.address)) continue
            samples.push({table, row})
        }
    }
    if (samples.length === 0) return {sampled: 0, matched: 0, mismatch: []}
    const step = Math.max(1, Math.floor(samples.length / expectedSamples))
    const selected = samples.filter((_, index) => index % step === 0).slice(0, expectedSamples)
    const addresses = selected.map((sample) => hex(sample.row.address))
    const output = execOrNull('addr2line', ['-e', elfPath, '-f', '-i', '-a', ...addresses])
    if (output === null) return null
    const lines = output.split('\n').map((line) => line.trim()).filter(Boolean)
    const resolved = new Map()
    for (let index = 0; index < lines.length; index++) {
        const line = lines[index]
        if (!/^0x[0-9a-f]+$/i.test(line)) continue
        const address = parseInt(line, 16)
        const frames = []
        let cursor = index + 1
        while (cursor + 1 < lines.length && !/^0x[0-9a-f]+$/i.test(lines[cursor])) {
            frames.push({func: lines[cursor], location: lines[cursor + 1]})
            cursor += 2
        }
        resolved.set(address, frames)
        index = cursor - 1
    }
    let matched = 0
    let unresolved = 0
    let unparsable = 0
    const unparsableSamples = []
    const unresolvedSamples = []
    const mismatch = []
    for (const sample of selected) {
        const frames = resolved.get(sample.row.address)
        if (!frames || frames.length === 0) {
            unresolved++
            if (unresolvedSamples.length < 8) unresolvedSamples.push({address: hex(sample.row.address), expected: `${sample.row.file.name}:${sample.row.line}`})
            continue
        }
        const rawLocation = frames[0].location
        const location = rawLocation.replace(/\s+\(discriminator\s+\d+\)$/i, '').replace(/\s+\(inlined by\)$/i, '')
        // DWARF line 0（未知行）在 addr2line 输出为 `file:?`，等价于行号 0。
        const match = location.match(/^(.*):(\d+|\?)$/)
        if (!match) {
            unparsable++
            if (unparsableSamples.length < 8) unparsableSamples.push({address: hex(sample.row.address), rawLocation})
            continue
        }
        const expectedLine = sample.row.line
        const expectedBase = sample.row.file.name.split('/').pop()
        const actualLine = match[2] === '?' ? 0 : parseInt(match[2], 10)
        const actualBase = match[1].split('/').pop()
        if (actualLine === expectedLine && actualBase === expectedBase) matched++
        else if (mismatch.length < 10) {
            mismatch.push({address: hex(sample.row.address), expected: `${expectedBase}:${expectedLine}`, addr2line: `${actualBase}:${actualLine}`})
        }
    }
    return {sampled: selected.length, matched, unresolved, unresolvedSamples, unparsable, unparsableSamples, mismatch}
}

const checkRealElf = (label, elfPath) => {
    if (!fs.existsSync(elfPath)) {
        console.log(`  [skip] ${label}: ELF not found: ${elfPath}`)
        return
    }
    let elf = null
    let lineSection = null
    try {
        elf = readElfSections(elfPath)
        lineSection = sectionBytesOf(elf, '.debug_line')
    } catch (error) {
        console.log(`  [skip] ${label}: cannot read ELF: ${error.message}`)
        return
    }
    check(`${label}: .debug_line section present`, lineSection !== null && lineSection.length > 0, lineSection ? hex(lineSection.length) : 'missing')
    if (!lineSection || lineSection.length === 0) return
    const addressSize = elf.is64 ? 8 : 4
    const lineStr = sectionBytesOf(elf, '.debug_line_str')
    const str = sectionBytesOf(elf, '.debug_str')
    const tables = enumerateLineTables(lineSection, {addressSize, compDir: '', lineStr, str})
    check(`${label}: all contributions parsed without header errors`, tables.every((table) => !table.error), `${tables.filter((table) => table.error).length} errors`)
    const goodTables = tables.filter((table) => !table.error)
    check(`${label}: contributions found`, goodTables.length > 0, goodTables.length)
    const totalRows = goodTables.reduce((sum, table) => sum + table.rows.length, 0)
    check(`${label}: rows parsed (>=200 for cross-check)`, totalRows >= 200, totalRows)
    check(`${label}: versions covered`, goodTables.every((table) => table.version >= 2 && table.version <= 5), [...new Set(goodTables.map((table) => table.version))].join(','))
    check(`${label}: tables cover multiple sequences`, goodTables.reduce((sum, table) => sum + table.sequences.length, 0) > goodTables.length)

    // readelf 交叉核对
    const decoded = execOrNull('readelf', ['-W', '--debug-dump=decodedline', elfPath])
    if (decoded === null) {
        console.log(`  [skip] ${label}: readelf not available`)
    } else {
        const {blocks, unmatched} = parseReadelfDecodedLine(decoded)
        check(`${label}: readelf block count == contribution count`, blocks.length === goodTables.length, `readelf=${blocks.length} ours=${goodTables.length}`)
        check(`${label}: readelf rows fully parsed`, unmatched.length === 0, unmatched.slice(0, 3).join(' | '))
        let rowMismatches = 0
        let fileMismatches = 0
        let comparedRows = 0
        const samples = []
        for (let index = 0; index < Math.min(blocks.length, goodTables.length); index++) {
            const block = blocks[index]
            const table = goodTables[index]
            if (block.rows.length !== table.rows.length) {
                rowMismatches++
                if (samples.length < 5) samples.push({block: index, reason: `row count ${block.rows.length} != ${table.rows.length}`})
                continue
            }
            for (let rowIndex = 0; rowIndex < block.rows.length; rowIndex++) {
                const expected = block.rows[rowIndex]
                const row = table.rows[rowIndex]
                comparedRows++
                const isEnd = expected.line === null
                const fieldsMatch = isEnd
                    ? row.endSequence && expected.address === row.address
                    : !row.endSequence && expected.address === row.address && expected.isStmt === row.isStmt
                        && expected.line === row.line && expected.opIndex === row.opIndex
                if (!fieldsMatch) {
                    rowMismatches++
                    if (samples.length < 5) {
                        samples.push({
                            block: index,
                            row: rowIndex,
                            expected: {...expected, end: isEnd},
                            actual: {...rowSummary(row)},
                        })
                    }
                }
                const expectedFile = expected.file
                const actualFile = row.file ? row.file.name : null
                if (actualFile !== expectedFile) {
                    fileMismatches++
                    if (samples.length < 5) samples.push({block: index, row: rowIndex, expectedFile, actualFile})
                }
            }
        }
        check(`${label}: readelf rows matched exactly`, rowMismatches === 0, `${rowMismatches} mismatches; ${JSON.stringify(samples.slice(0, 2))}`)
        check(`${label}: readelf row file names matched`, fileMismatches === 0, `${fileMismatches} mismatches`)
        check(`${label}: compared >=200 rows with readelf`, comparedRows >= 200, comparedRows)
        console.log(`  info ${label}: contributions=${goodTables.length} rows=${totalRows} compared=${comparedRows} versions=${[...new Set(goodTables.map((table) => table.version))].join(',')}`)
    }

    // addr2line 抽样
    const sampleResult = addr2lineCheck(elfPath, goodTables, 300, makeExecutableFilter(elf.executableRanges))
    if (sampleResult === null) {
        console.log(`  [skip] ${label}: addr2line not available`)
    } else {
        check(`${label}: addr2line sampled >=200 rows`, sampleResult.sampled >= 200, sampleResult.sampled)
        const resolvedCount = sampleResult.sampled - sampleResult.unresolved - sampleResult.unparsable
        if (sampleResult.unparsable > 0) console.log(`  info ${label}: addr2line unparsable locations=${sampleResult.unparsable} ${JSON.stringify(sampleResult.unparsableSamples)}`)
        const resolvedRatio = resolvedCount ? sampleResult.matched / resolvedCount : 1
        check(`${label}: addr2line resolved >=90%`, sampleResult.unresolved <= sampleResult.sampled * 0.1, `unresolved=${sampleResult.unresolved}; ${JSON.stringify(sampleResult.unresolvedSamples)}`)
        check(`${label}: addr2line file:line match >=99% (resolved only)`, resolvedCount >= 200 && resolvedRatio >= 0.99, `${sampleResult.matched}/${resolvedCount}; ${JSON.stringify(sampleResult.mismatch.slice(0, 5))}`)
        console.log(`  info ${label}: addr2line matched ${sampleResult.matched}/${sampleResult.sampled}`)
    }

    // PC -> 行 / 反向查询自洽性（用抽样行验证 addressToLine 至少能命中）
    let pcHit = 0
    let pcSampled = 0
    let pcMiss = 0
    for (const table of goodTables) {
        for (const row of table.rows) {
            if (row.endSequence || !row.file || row.address === 0) continue
            pcSampled++
            if (pcSampled % 97 !== 0) continue
            const hit = addressToLine(table, row.address)
            // 同一表内多个“重定位为 0”的 sequence 会地址重叠（readelf 同样显示），
            // 此时 first-match 可能命中更早的 sequence；只要求能命中地址 <= pc 的行。
            if (hit && hit.address <= row.address) pcHit++
            else {
                pcMiss++
                if (process.env.DBG_LINE === '1' && pcMiss <= 12) {
                    console.log(`  [debug] pc miss table=${table.offset} row=${row.rowIndex} addr=${hex(row.address)} line=${row.line} seq=${row.sequence} seqs=${JSON.stringify(table.sequences.filter((seq) => seq.startRow <= row.rowIndex && seq.endRow >= row.rowIndex))}`)
                }
            }
        }
    }
    const pcExpected = Math.floor(pcSampled / 97)
    check(`${label}: addressToLine self-consistency (>=95%)`, pcHit > 0 && pcHit / Math.max(1, pcExpected) >= 0.95, `${pcHit}/${pcExpected} hits`)
}

for (const [label, elfPath] of [['release(-O2)', releaseElf], ['debug(-O0)', debugElf]]) checkRealElf(label, elfPath)

// ---------------------------------------------------------------------------
// 汇总
// ---------------------------------------------------------------------------
if (failures > 0) {
    console.error(`\n${failures} failure(s)`)
    process.exit(1)
}
console.log('\nall DWARF .debug_line checks passed')
