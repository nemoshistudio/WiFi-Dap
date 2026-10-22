#!/usr/bin/env node
/**
 * WP-132 · Web UI i18n 契约检查（docs/i18n-keys.md v1）
 *
 * 默认（report 模式）：
 *   - 硬编码 UI CJK、页面中的 fr 残留、动态拼接 key 只输出报告，不阻塞；
 *   - 语言文件 / i18n.ts 的结构性违约（key 缺失、集合不一致、fr.ts 存在、
 *     fallback 配置错误、占位符不一致等）仍然失败。
 *   - 原因：Wave 1 页面由 WP-131/133/... 并行整改，WP-132 不修改页面代码。
 *
 * CI 模式：`node scripts/check-i18n.mjs --strict`
 *   - 上述页面级报告项一并转为失败，供页面整改完成后接入构建。
 *
 * 可选：`--baseline=path/to/i18n-baseline.json`
 *   - 默认读取 web/scripts/i18n-baseline.json（不存在则使用脚本内嵌的 148 key 基线）。
 *
 * 退出码：0 = 通过；1 = 契约检查失败；2 = 无法解析 locale 文件。
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const SRC = path.join(root, 'src')
const LOCALE_DIR = path.join(SRC, 'locales')
const LOCALES = ['zh', 'en']
const EXPECTED_TOTAL = 433
const EXPECTED_BASELINE = 148
const EXPECTED_ADDED = 285

const errors = []
const warnings = []
const notes = []
const args = process.argv.slice(2)
const strict = args.includes('--strict')
const quiet = args.includes('--quiet')

if (args.includes('--help') || args.includes('-h')) {
    console.log(`用法: node scripts/check-i18n.mjs [--strict] [--quiet] [--baseline=<json>]

默认: 语言文件结构性检查失败 -> exit 1；页面硬编码/fr/动态 key 仅报告。
--strict: 页面级报告项也作为失败（接入 CI 时使用）。
--quiet:  仅输出汇总与错误。`)
    process.exit(0)
}

// ---------------------------------------------------------------------------
// 契约 key 清单（与 docs/i18n-keys.md 同步；脚本启动时自检数量）
// ---------------------------------------------------------------------------
const keys = (prefix, names) => names.map((name) => (prefix ? `${prefix}.${name}` : name))

const BASELINE_KEYS = [
    // 基线平铺 key（11：已按需求删除作者/联系方式相关 12 个）
    ...keys('', [
        'emoji.flag', 'disconnected', 'connected', 'connecting', 'use',
        'version', 'releaseTime', 'credit', 'aboutWebHost',
        'aboutDebugger', 'serialNumber',
    ]),
    ...keys('common', ['debuggerConnected', 'ok']),
    ...keys('ws', ['disconnected', 'connected', 'connecting']),
    ...keys('page', ['home', 'wifi', 'about', 'uart', 'close', 'update', 'fullscreen', 'windowed']),
    ...keys('wifi', [
        'settings', 'setFailed', 'setSuccess', 'connection', 'scanning', 'scan', 'scanDone',
        'warnWifiName', 'password', 'connectInfoHTML', 'connect', 'mode', 'save', 'station',
        'intelligent', 'APOnly', 'disconnected', 'modeTipsHtml', 'enabled', 'disabled',
        'stationInfo', 'hotspotInfo', 'signalStrength', 'gateway', 'netmask', 'primaryDNS',
        'backupDNS', 'IPmode', 'DNSmode', 'internalAddress', 'autoIP', 'staticIP', 'autoDNS',
        'staticDNS', 'APauto_STA', 'APonly', 'AP_STA', 'connectionSuccess', 'enterAPName',
        'debuggerNotConnected',
    ]),
    ...keys('uart', [
        'port', 'startCommunication', 'stopCommunication', 'commonlyUsed', 'baudrate',
        'customBaud', 'use', 'actual', 'dataBits', 'stopBits', 'parity', 'parityNone',
        'parityOdd', 'parityEven', 'flowControl', 'send', 'clear', 'clearTooltip',
        'updateTooltip', 'autoUpdateTooltip', 'receive', 'displayOptions', 'display', 'show',
        'text', 'timestamp', 'enable', 'lineWrap', 'highlight', 'frameBreakStrategy',
        'priority', 'rule', 'ruleTips', 'value', 'timeout', 'match', 'byte', 'begin', 'end',
        'other', 'decodeAnsiEscapeCodes', 'ansiTooltips', 'filter', 'textAndEscape',
        'autoUpdateNewData', 'updateFrequency', 'updateFrequencyTooltip', 'addHeader',
        'addFooter', 'passthrough', 'proxy', 'serverPort', 'connectedClient', 'refresh',
        'interface', 'noClientConnected', 'import', 'export', 'reset', 'resetTooltip',
        'saveToLocal', 'saveToLocalTooltip', 'add', 'edit', 'drag', 'ipChangeAlert',
        'layout', 'landscape', 'portrait', 'responsive', 'configPannel', 'displayPannel',
        'macroPannel', 'autoScrollToBottom', 'clearScreen', 'autoUpdate',
        'tempDisplayTooltip', 'loopSend', 'loopSendTooltip', 'sendFormat', 'cachedFrame', 'format',
    ]),
    ...keys('navbar', ['navigationSidebar', 'getSomeFries']),
]

const ADDED_KEYS = [
    ...keys('common', [
        'cancel', 'confirm', 'close', 'save', 'delete', 'edit', 'back', 'retry', 'refresh',
        'reset', 'apply', 'loading', 'success', 'failed', 'error', 'warning', 'unknown',
        'yes', 'no', 'copy', 'copied', 'notConnected', 'unsupported', 'details', 'required', 'actions',
        'select',
    ]),
    ...keys('protocol', [
        'title', 'version', 'webVersion', 'firmwareVersion', 'buildTag', 'buildTime', 'match',
        'mismatchTitle', 'mismatchDetail', 'mismatchLegacy', 'mismatchOtaDisabled', 'refreshHint',
        'unknownVersion',
    ]),
    ...keys('wifi.saved', [
        'title', 'description', 'empty', 'autoSave', 'autoSaveHint', 'priority', 'lastSuccess',
        'current', 'connect', 'connecting', 'forget', 'forgetConfirm', 'forgetConfirmTitle',
        'deleted', 'add', 'addTitle', 'added', 'updated', 'invalidPassword', 'maxReached',
        'connectFailed', 'apDefault', 'apCustom', 'apDefaultHint', 'listFull', 'evictedHint',
    ]),
    ...keys('wifi.ap', ['resetDefault', 'resetConfirm']),
    ...keys('ota.upload', [
        'title', 'description', 'selectFile', 'dragHint', 'fileName', 'fileSize', 'fileVersion',
        'fileDate', 'fileInvalid', 'tooLarge', 'sha256', 'verifyingFile', 'upload', 'uploading',
        'cancel', 'cancelConfirm', 'progress', 'speed', 'eta', 'verifying', 'ready',
        'rebootNote', 'success', 'failed', 'aborted', 'sameVersionWarn', 'downgradeWarn',
        'current', 'slotSize', 'slotFree',
    ]),
    ...keys('ota.status', ['idle', 'receiving', 'verifying', 'ready', 'applying', 'error']),
    ...keys('about', ['hardwareVersion', 'manufactureDate', 'board', 'copyInfo']),
    ...keys('system', ['reboot', 'rebooting']),
    ...keys('uart', [
        'instance', 'connectFailed', 'overflowWarning', 'binaryError', 'remoteUnsupported',
        'invalidHex', 'hexMode', 'hexdumpMode', 'highlightRx', 'highlightTx',
        'sendShortcut', 'noResponse',
    ]),
    ...keys('target', [
        'title', 'description', 'detect', 'detecting', 'detectFailed', 'notDetected',
        'notSupported', 'family', 'model', 'unsupportedFamily', 'cpuid', 'idcode', 'uid', 'flashBase', 'flashSize',
        'running', 'securityHint', 'selectFile', 'fileName', 'fileSize', 'sha256',
        'hashComputing', 'invalidFile', 'emptyFile', 'fileTooLarge', 'start',
        'confirmFlashTitle', 'confirmFlash', 'noTargetHint', 'selectFirst', 'flashing',
        'flashingHint', 'uploadProgress', 'deviceStatus', 'success', 'failed', 'result',
        'written', 'reset', 'resetting', 'resetSuccess', 'resetFailed',
        // 目标烧录多格式支持（BIN/ELF/HEX/SREC 前端转换）
        'sourceFile', 'fileFormat', 'convertedImage', 'imageSize', 'flashUsage', 'segments',
        'formatBin', 'formatElf', 'formatHex', 'formatSrec', 'ignoredSegments', 'filledGaps',
        'debugInfoIgnored', 'convertComputing', 'convertFailed',
    ]),
    // 变量曲线页面（WS-DAP 采样 + ELF 选变量 + canvas 实时图）新增 41 个 + page.debugCurve
    ...keys('curve', [
        'takeoverHint', 'takeoverConfirmTitle', 'kickFailed', 'connectFailed', 'reconnectHint',
        'usbipReattachHint', 'connect', 'disconnect', 'start', 'stop', 'clear',
        'statusIdle', 'statusConnecting', 'statusConnected', 'statusSampling', 'statusStopped',
        'statusError', 'targetHalted', 'resumeTarget', 'resetTarget', 'sampleRate', 'effectiveRate', 'points',
        'failedCount', 'droppedCount', 'format', 'elfUpload', 'elfHint', 'elfParsed', 'elfInvalid',
        'searchPlaceholder', 'variables', 'selected', 'noSymbols', 'manualAddress',
        'manualAddressHint', 'addAddress', 'invalidAddress', 'maxSeries', 'exportCsv', 'exportPng',
        'noData',
        // DWARF 类型浏览器增量
        'colName', 'colType', 'colAddress', 'colSize', 'colSource', 'dwarfParsed',
        'dwarfFallback', 'symtabNotice', 'expandAll', 'collapseAll', 'wideHint', 'unknownType',
        // 采样管线 / Auto 采样率
        'sampleRateAuto', 'autoRate', 'samplingHint',
        // DWARF 源码名 / 不可采样变量 / 分组
        'onlySampleable', 'unsampleable', 'unsampleableHint', 'groupBy', 'groupNone',
        'groupFile', 'groupNamespace', 'debugInfoHint',
        // 调试采样模式（暂停读帧/寄存器/指针）
        'samplingMode', 'samplingModeLive', 'samplingModeDebug', 'debugInterval', 'debugModeHint',
        'debugSamplable', 'pointerSamplable', 'haltSamplableHint', 'pointerExpandedHint', 'debugSampleFailed', 'haltSeriesRemoved',
        // 设备本地采样（ESP32 闭环，Phase 1）
        'samplingSource', 'samplingSourceWeb', 'samplingSourceLocal', 'localInterval', 'localModeHint',
        'localRegisterUnsupported', 'localRegisterShort', 'localStartFailed', 'localStopFailed', 'localPollFailed',
        'localFrames', 'localInvalid', 'localOverwritten', 'localStatus', 'localRunning', 'localStopped',
        'localSeriesRemoved', 'localSeriesLocked', 'localSeriesLimit',
        // 设备本地 Phase 2：断点触发
        'localTrigger', 'localTriggerPeriodic', 'localTriggerBreakpoint', 'localMinInterval',
        'localBreakpoint', 'localBreakpointHint', 'localBreakpointRequired', 'localBreakpointInvalid',
        'localHits', 'localSampled',
    ]),
    ...keys('page', ['debugCurve']),
]

const EXPECTED_KEYS = [...BASELINE_KEYS, ...ADDED_KEYS]
const EXPECTED_SET = new Set(EXPECTED_KEYS)
const BASELINE_SET = new Set(BASELINE_KEYS)
const ADDED_SET_RE = /^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9]*)+$/
const ALLOWED_PLACEHOLDERS = new Set(['err', 'max', 'size', 'code', 'web', 'fw', 'count', 'ssid', 'fixed'])

function selfCheckContract() {
    const problems = []
    if (BASELINE_KEYS.length !== EXPECTED_BASELINE) {
        problems.push(`脚本内嵌基线 key 数 ${BASELINE_KEYS.length} != ${EXPECTED_BASELINE}`)
    }
    if (ADDED_KEYS.length !== EXPECTED_ADDED) {
        problems.push(`脚本内嵌新增 key 数 ${ADDED_KEYS.length} != ${EXPECTED_ADDED}`)
    }
    if (EXPECTED_SET.size !== EXPECTED_TOTAL) {
        problems.push(`脚本内嵌契约 key 总数 ${EXPECTED_SET.size} != ${EXPECTED_TOTAL}`)
    }
    for (const [name, list] of [['baseline', BASELINE_KEYS], ['added', ADDED_KEYS]]) {
        if (new Set(list).size !== list.length) problems.push(`脚本内嵌 ${name} 清单存在重复 key`)
    }
    for (const key of BASELINE_KEYS) {
        if (ADDED_SET_RE.test(key) === false) {
            // 基线允许历史命名（大写开头/单段），仅确认不在新增清单里
        }
        if (ADDED_KEYS.includes(key)) problems.push(`key 同时出现在基线与新增清单: ${key}`)
    }
    if (problems.length) {
        console.error('[fatal] check-i18n 内嵌契约清单自检失败:\n' + problems.join('\n'))
        process.exit(2)
    }
}

selfCheckContract()

// ---------------------------------------------------------------------------
// locale 文件解析：优先 TypeScript AST；无依赖时回退 Node 原生 TS 导入
// ---------------------------------------------------------------------------
let tsPromise
function loadTypeScript() {
    if (!tsPromise) {
        tsPromise = import('typescript')
            .then((mod) => mod.default ?? mod)
            .catch(() => null)
    }
    return tsPromise
}

function parseWithTypeScript(ts, file) {
    const text = fs.readFileSync(file, 'utf8')
    const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true, ts.ScriptKind.TS)
    const exportDefault = source.statements.find((stmt) => ts.isExportAssignment(stmt))
    if (!exportDefault) throw new Error('未找到 export default')

    const collected = []
    const values = new Map()
    const duplicates = []
    const unsupported = []

    const stringValue = (node) => {
        if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) return node.text
        if (ts.isBinaryExpression(node) && node.operatorToken.kind === ts.SyntaxKind.PlusToken) {
            const left = stringValue(node.left)
            const right = stringValue(node.right)
            return left !== null && right !== null ? left + right : null
        }
        return null
    }

    const walk = (node, prefix) => {
        if (!ts.isObjectLiteralExpression(node)) return
        for (const prop of node.properties) {
            if (!ts.isPropertyAssignment(prop)) {
                unsupported.push(prop.getText(source).slice(0, 80))
                continue
            }
            const name = ts.isIdentifier(prop.name) || ts.isStringLiteral(prop.name) || ts.isNumericLiteral(prop.name)
                ? prop.name.text
                : null
            if (!name) {
                unsupported.push(prop.getText(source).slice(0, 80))
                continue
            }
            const key = prefix ? `${prefix}.${name}` : name
            if (ts.isObjectLiteralExpression(prop.initializer)) {
                walk(prop.initializer, key)
            } else {
                collected.push(key)
                values.set(key, stringValue(prop.initializer))
            }
        }
    }

    walk(exportDefault.expression, '')
    const seen = new Set()
    for (const key of collected) {
        if (seen.has(key)) duplicates.push(key)
        seen.add(key)
    }
    return { parser: 'typescript', keys: collected, values, duplicates, unsupported }
}

function flattenLocale(object, prefix, out) {
    for (const [name, value] of Object.entries(object)) {
        const key = prefix ? `${prefix}.${name}` : name
        if (value !== null && typeof value === 'object' && !Array.isArray(value)) {
            flattenLocale(value, key, out)
        } else {
            out.push([key, value])
        }
    }
}

async function parseWithNativeImport(file) {
    const mod = await import(pathToFileURL(file).href)
    const out = []
    flattenLocale(mod.default, '', out)
    const keys = out.map(([key]) => key)
    const values = new Map(out)
    const seen = new Set()
    const duplicates = []
    for (const key of keys) {
        if (seen.has(key)) duplicates.push(key)
        seen.add(key)
    }
    return { parser: 'node-native', keys, values, duplicates, unsupported: [] }
}

async function collectLocale(lang) {
    const file = path.join(LOCALE_DIR, `${lang}.ts`)
    if (!fs.existsSync(file)) {
        errors.push(`缺少 locale 文件: src/locales/${lang}.ts`)
        return null
    }
    const ts = await loadTypeScript()
    if (ts) return parseWithTypeScript(ts, file)
    notes.push('未找到 typescript 依赖，回退到 Node 原生 TS 导入（CI 请先 npm ci）')
    try {
        return await parseWithNativeImport(file)
    } catch (error) {
        console.error(`[fatal] 无法解析 src/locales/${lang}.ts：${error.message}`)
        console.error('请先在 web/ 下安装依赖（npm ci / npm install）后重试。')
        process.exit(2)
    }
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
function rel(file) {
    return path.relative(root, file).split(path.sep).join('/')
}

function formatList(list, limit = 24) {
    if (list.length <= limit) return list.join(', ')
    return `${list.slice(0, limit).join(', ')} …（共 ${list.length} 个）`
}

function placeholders(value) {
    if (typeof value !== 'string') return []
    return [...value.matchAll(/\{([a-zA-Z_][a-zA-Z0-9_]*)\}/g)].map((match) => match[1]).sort()
}

function walkFiles(dir, out = []) {
    const EXCLUDED = new Set(['locales', 'node_modules', 'dist', '.git', 'assets', 'constants'])
    for (const name of fs.readdirSync(dir)) {
        const full = path.join(dir, name)
        const stat = fs.statSync(full)
        if (stat.isDirectory()) {
            if (!EXCLUDED.has(name)) walkFiles(full, out)
        } else if (/\.(ts|tsx|js|mjs|vue)$/.test(name)) {
            out.push(full)
        }
    }
    return out
}

function stripComments(line, state) {
    let code = line
    if (state.inBlock) {
        const end = code.indexOf('*/')
        if (end === -1) return ''
        code = code.slice(end + 2)
        state.inBlock = false
    }
    if (state.inHtml) {
        const end = code.indexOf('-->')
        if (end === -1) return ''
        code = code.slice(end + 3)
        state.inHtml = false
    }
    for (;;) {
        const start = code.indexOf('/*')
        if (start === -1) break
        const end = code.indexOf('*/', start + 2)
        if (end === -1) {
            code = code.slice(0, start)
            state.inBlock = true
            break
        }
        code = code.slice(0, start) + code.slice(end + 2)
    }
    for (;;) {
        const start = code.indexOf('<!--')
        if (start === -1) break
        const end = code.indexOf('-->', start + 4)
        if (end === -1) {
            code = code.slice(0, start)
            state.inHtml = true
            break
        }
        code = code.slice(0, start) + code.slice(end + 3)
    }
    // 行注释；避免把 https:// 当成注释
    code = code.replace(/(^|\s)\/\/.*$/, '$1')
    return code
}

// ---------------------------------------------------------------------------
// 0. 可选基线清单
// ---------------------------------------------------------------------------
let baselineKeys = BASELINE_KEYS
let baselineSource = '脚本内嵌 180 key'
const baselineArg = args.find((arg) => arg.startsWith('--baseline='))
const baselineFile = baselineArg
    ? path.resolve(root, baselineArg.slice('--baseline='.length))
    : path.join(root, 'scripts', 'i18n-baseline.json')

if (fs.existsSync(baselineFile)) {
    try {
        const data = JSON.parse(fs.readFileSync(baselineFile, 'utf8'))
        const list = Array.isArray(data) ? data : Array.isArray(data?.keys) ? data.keys : null
        if (!list) {
            errors.push(`${rel(baselineFile)}: 期望 key 字符串数组（或 { keys: [] }）`)
        } else {
            const cleaned = list.filter((key) => typeof key === 'string')
            if (cleaned.length !== list.length) errors.push(`${rel(baselineFile)}: 含非字符串项`)
            if (cleaned.length !== EXPECTED_BASELINE) {
                warnings.push(`${rel(baselineFile)}: 基线 key 数 ${cleaned.length} != 契约 ${EXPECTED_BASELINE}`)
            }
            const notInContract = cleaned.filter((key) => !BASELINE_SET.has(key))
            if (notInContract.length) {
                warnings.push(`${rel(baselineFile)}: 含契约基线外 key: ${formatList(notInContract)}`)
            }
            baselineKeys = cleaned
            baselineSource = rel(baselineFile)
        }
    } catch (error) {
        errors.push(`${rel(baselineFile)}: JSON 解析失败 (${error.message})`)
    }
} else if (baselineArg) {
    errors.push(`--baseline 指定的文件不存在: ${baselineFile}`)
} else {
    notes.push('未提供 web/scripts/i18n-baseline.json，使用 check-i18n.mjs 内嵌的 148 key 基线')
}

// ---------------------------------------------------------------------------
// 1. locale key 集合
// ---------------------------------------------------------------------------
const parsed = {}
for (const lang of LOCALES) {
    parsed[lang] = await collectLocale(lang)
}

const localeSets = {}
for (const lang of LOCALES) {
    const data = parsed[lang]
    if (!data) continue
    localeSets[lang] = new Set(data.keys)
    if (data.duplicates.length) {
        errors.push(`src/locales/${lang}.ts: 存在重复 key: ${formatList([...new Set(data.duplicates)])}`)
    }
    if (data.unsupported.length) {
        errors.push(`src/locales/${lang}.ts: 存在非静态属性（无法提取 key）: ${formatList(data.unsupported)}`)
    }
    const expectedMissing = [...EXPECTED_SET].filter((key) => !localeSets[lang].has(key))
    const unexpected = data.keys.filter((key) => !EXPECTED_SET.has(key))
    if (expectedMissing.length) {
        errors.push(`src/locales/${lang}.ts: 缺少契约 key ${expectedMissing.length} 个: ${formatList(expectedMissing)}`)
    }
    if (unexpected.length) {
        errors.push(`src/locales/${lang}.ts: 含契约外 key ${unexpected.length} 个: ${formatList([...new Set(unexpected)])}`)
    }
    if (localeSets[lang].size !== EXPECTED_TOTAL) {
        errors.push(`src/locales/${lang}.ts: key 数 ${localeSets[lang].size} != 契约 ${EXPECTED_TOTAL}`)
    }
    const lost = baselineKeys.filter((key) => !localeSets[lang].has(key))
    if (lost.length) {
        errors.push(`src/locales/${lang}.ts: 基线 key 被删除/改名 ${lost.length} 个: ${formatList(lost)}`)
    }
}

if (localeSets.zh && localeSets.en) {
    const onlyZh = [...localeSets.zh].filter((key) => !localeSets.en.has(key))
    const onlyEn = [...localeSets.en].filter((key) => !localeSets.zh.has(key))
    if (onlyZh.length) errors.push(`en.ts 缺少 zh 中的 key: ${formatList(onlyZh)}`)
    if (onlyEn.length) errors.push(`zh.ts 缺少 en 中的 key: ${formatList(onlyEn)}`)
}

// ---------------------------------------------------------------------------
// 2. 值检查：空值/空白、占位符、新增 key 规则
// ---------------------------------------------------------------------------
if (parsed.zh && parsed.en) {
    for (const lang of LOCALES) {
        const values = parsed[lang].values
        for (const key of EXPECTED_SET) {
            if (!values.has(key)) continue
            const value = values.get(key)
            if (typeof value !== 'string') {
                errors.push(`src/locales/${lang}.ts: ${key} 的值不是静态字符串`)
                continue
            }
            if (value.length === 0) errors.push(`src/locales/${lang}.ts: ${key} 的值为空`)
            if (value !== value.trim()) errors.push(`src/locales/${lang}.ts: ${key} 首尾含空白`)
            const tokens = placeholders(value)
            for (const token of tokens) {
                if (!ALLOWED_PLACEHOLDERS.has(token)) {
                    errors.push(`src/locales/${lang}.ts: ${key} 使用了契约外占位符 {${token}}`)
                }
            }
            if (ADDED_SET_RE.test(key) === false && ADDED_KEYS.includes(key)) {
                errors.push(`src/locales/${lang}.ts: 新增 key 命名不符合规范: ${key}`)
            }
            if (ADDED_KEYS.includes(key)) {
                if (/<[a-zA-Z/!]/.test(value)) errors.push(`src/locales/${lang}.ts: 新增 key ${key} 不得含 HTML`)
                if (value.includes('\n')) errors.push(`src/locales/${lang}.ts: 新增 key ${key} 不得含换行`)
            }
        }
    }

    for (const key of EXPECTED_SET) {
        const zhTokens = placeholders(parsed.zh.values.get(key))
        const enTokens = placeholders(parsed.en.values.get(key))
        if (zhTokens.join(',') !== enTokens.join(',')) {
            errors.push(`占位符不一致: ${key} (zh={${zhTokens.join('},{')}} / en={${enTokens.join('},{')}}`)
        }
    }

    if (!quiet) {
        for (const lang of LOCALES) {
            const cjk = [...parsed[lang].values.entries()].filter(([, value]) => /[\u4e00-\u9fff]/.test(String(value)))
            if (lang === 'en' && cjk.length) {
                warnings.push(`en.ts 有 ${cjk.length} 个值含 CJK 字符: ${formatList(cjk.map(([key]) => key))}`)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 3. fr 彻底移除 + i18n.ts 配置
// ---------------------------------------------------------------------------
const frFile = path.join(LOCALE_DIR, 'fr.ts')
if (fs.existsSync(frFile)) errors.push('src/locales/fr.ts 必须删除')
for (const name of fs.existsSync(LOCALE_DIR) ? fs.readdirSync(LOCALE_DIR) : []) {
    if (!['zh.ts', 'en.ts', 'index.ts'].includes(name) && /\.(ts|js)$/.test(name)) {
        warnings.push(`src/locales/ 下存在非契约语言文件: ${name}`)
    }
}

const i18nPath = path.join(SRC, 'i18n.ts')
const i18nText = fs.existsSync(i18nPath) ? fs.readFileSync(i18nPath, 'utf8') : ''
if (!i18nText) {
    errors.push('缺少 src/i18n.ts')
} else {
    if (/\bfr\b|Français|fr\.ts/.test(i18nText)) errors.push('src/i18n.ts 仍引用 fr/法文')
    if (/\bconsole\.(log|debug|info)\s*\(/.test(i18nText)) errors.push('src/i18n.ts 不得保留生产 console 日志')
    const fallback = i18nText.match(/fallbackLocale\s*:\s*['"]([^'"]+)['"]/)
    if (!fallback || fallback[1] !== 'zh') errors.push('src/i18n.ts: fallbackLocale 必须为 zh')
    const available = i18nText.match(/availableLanguages\s*=\s*\[([\s\S]*?)\]/)
    if (!available) {
        errors.push('src/i18n.ts: 未找到 availableLanguages 声明')
    } else {
        const langs = [...available[1].matchAll(/['"]([^'"]+)['"]/g)].map((match) => match[1])
        if (langs.join(',') !== 'zh,en') {
            errors.push(`src/i18n.ts: availableLanguages 必须为 ['zh', 'en']，实际 [${langs.join(', ')}]`)
        }
    }
    if (!/startsWith\(\s*['"]zh['"]\s*\)/.test(i18nText)) errors.push("src/i18n.ts: 缺少 zh* -> zh 的语言检测")
    if (!/startsWith\(\s*['"]en['"]\s*\)/.test(i18nText)) errors.push("src/i18n.ts: 缺少 en* -> en 的语言检测")
    const messagesDecl = i18nText.match(/messages\s*=\s*\{([\s\S]*?)\}/)
    if (!messagesDecl || !/\bzh\b/.test(messagesDecl[1]) || !/\ben\b/.test(messagesDecl[1])) {
        errors.push('src/i18n.ts: messages 必须包含 zh 与 en')
    }
}

// ---------------------------------------------------------------------------
// 4. 页面/配置扫描：硬编码 CJK、fr 残留、动态 key
// ---------------------------------------------------------------------------
const sourceFiles = fs.existsSync(SRC) ? walkFiles(SRC) : []
const cjkHits = []
const frHits = []
const dynamicHits = []
const CJK_RE = /[\u4e00-\u9fff]/
const FR_RE = /\bfr\b|Français|fr-FR/

for (const file of sourceFiles) {
    const relative = rel(file)
    const text = fs.readFileSync(file, 'utf8')
    const lines = text.split('\n')

    // 硬编码 CJK（跳过注释；locales/constants/assets 已在 walkFiles 排除）
    const state = { inBlock: false, inHtml: false }
    lines.forEach((line, index) => {
        const code = stripComments(line, state)
        if (CJK_RE.test(code)) {
            cjkHits.push({ file: relative, line: index + 1, text: line.trim().slice(0, 140) })
        }
    })

    // 页面中的 fr / 法文残留（i18n.ts 已单独作为错误处理）
    if (relative !== 'src/i18n.ts') {
        lines.forEach((line, index) => {
            if (FR_RE.test(line)) {
                frHits.push({ file: relative, line: index + 1, text: line.trim().slice(0, 140) })
            }
        })
    }

    // 动态拼接 / 变量 key（契约 §2.2，报告项）
    const dynState = { inBlock: false, inHtml: false }
    lines.forEach((line, index) => {
        const code = stripComments(line, dynState)
        if (/function\s+translate|const\s+translate\s*=/.test(code)) return
        const nonLiteral = /\b(?:translate|\$t)\(\s*(?!['"`])/.test(code)
        const concatenated = /\b(?:translate|\$t)\(\s*['"][^'"]*['"]\s*\+/.test(code)
        const template = /\b(?:translate|\$t)\(\s*`[^`]*\$\{/.test(code)
        if (nonLiteral || concatenated || template) {
            dynamicHits.push({ file: relative, line: index + 1, text: line.trim().slice(0, 140) })
        }
    })
}

// locales/index.ts 的类型收紧状态（WP-131 合并前不阻塞，--strict 才失败）
const localeIndexPath = path.join(LOCALE_DIR, 'index.ts')
if (fs.existsSync(localeIndexPath)) {
    const text = fs.readFileSync(localeIndexPath, 'utf8')
    if (!/NestedKeyOf<\s*typeof\s+zh\s*>/.test(text)) {
        warnings.push('locales/index.ts: TranslationKeys 必须从 zh 推导（NestedKeyOf<typeof zh>）')
    }
    if (/TranslationKeys\s*\|\s*string/.test(text)) {
        warnings.push('locales/index.ts: translate() 仍接受 string；契约 §2.3 要求在消费方同步后收紧为 TranslationKeys')
    }
}

// ---------------------------------------------------------------------------
// 5. 报告输出
// ---------------------------------------------------------------------------
if (!quiet) {
    const byFile = new Map()
    for (const hit of cjkHits) byFile.set(hit.file, (byFile.get(hit.file) || 0) + 1)

    console.log('')
    console.log(`[report] 硬编码 UI CJK：${cjkHits.length} 行 / ${byFile.size} 个文件`)
    console.log('         默认仅报告（WP-132 不改页面代码）；--strict 时失败。按文件：')
    console.log('         ' + [...byFile.entries()].map(([file, count]) => `${file} (${count})`).join(', '))
    if (cjkHits.length) {
        console.log('         清单（文件名:行号: 内容）：')
        for (const hit of cjkHits) console.log(`         ${hit.file}:${hit.line}: ${hit.text}`)
    }

    console.log('')
    console.log(`[report] 页面 fr / 法文残留：${frHits.length} 行（${new Set(frHits.map((hit) => hit.file)).size} 个文件）`)
    for (const hit of frHits) console.log(`         ${hit.file}:${hit.line}: ${hit.text}`)

    console.log('')
    console.log(`[report] 动态/拼接 key 调用：${dynamicHits.length} 处（契约 §2.2 禁止；建议改显式映射）`)
    for (const hit of dynamicHits) console.log(`         ${hit.file}:${hit.line}: ${hit.text}`)
}

// --strict 将页面级报告项转为失败
if (strict) {
    if (cjkHits.length) errors.push(`--strict: 硬编码 UI CJK ${cjkHits.length} 行`)
    if (frHits.length) errors.push(`--strict: 页面 fr/法文残留 ${frHits.length} 行`)
    if (dynamicHits.length) errors.push(`--strict: 动态/拼接 key 调用 ${dynamicHits.length} 处`)
    for (const warning of warnings) errors.push(`--strict: ${warning}`)
    warnings.length = 0
}

console.log('')
for (const note of notes) console.log(`[note] ${note}`)
for (const warning of warnings) console.log(`[warn] ${warning}`)
for (const error of errors) console.error(`[error] ${error}`)

const zhCount = localeSets.zh ? localeSets.zh.size : 0
const enCount = localeSets.en ? localeSets.en.size : 0
console.log('')
console.log(`locales: zh=${zhCount} en=${enCount}; contract=${EXPECTED_TOTAL} (baseline=${baselineKeys.length} via ${baselineSource}, added=${ADDED_KEYS.length})`)
console.log(`fr.ts: ${fs.existsSync(frFile) ? '存在（失败）' : '已删除'}; hardcoded CJK=${cjkHits.length}; page-fr=${frHits.length}; dynamic-key=${dynamicHits.length}`)
if (errors.length) {
    console.error(`i18n check FAILED: ${errors.length} error(s)` + (strict ? ' (strict)' : ''))
    process.exitCode = 1
} else {
    console.log('i18n check passed' + (cjkHits.length || frHits.length || dynamicHits.length ? ' (页面级报告项未阻塞；CI 请用 --strict)' : ''))
}
