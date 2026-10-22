#!/usr/bin/env node
/**
 * DebugCurve（变量曲线页）多变量性能基准脚本
 * ============================================
 *
 * 目标：给“设备本地采样 + 多变量曲线”建立可重复的页面性能基准，供优化前后对比。
 * 只通过 Playwright 驱动真实设备上的 Web 页面（不修改任何产品代码），输出 JSON + 可选 Markdown。
 *
 * 基准流程（每个场景）：
 *   1. 打开 http://<device>/#/debug-curve，确认采样来源=设备本地（ESP32 闭环）
 *   2. 上传调试 ELF，等待 DWARF 解析完成（固定地址/全局变量优先，避免 halt）
 *   3. 在变量树中勾选 N 路固定地址变量（N=40/128/256；选择顺序固定、可复现）
 *   4. 确认本地采样间隔 20 ms，设置 B（默认 drawCap=64）或 A（全部绘制）渲染模式
 *   5. Start，预热后测量 ~10 s 窗口：
 *      - 页面注入 rAF 计数 → 平均 FPS / 帧间隔分位 / 掉帧
 *      - PerformanceObserver(longtask / long-animation-frame) → 长任务数/总时长/LoAF invoker
 *      - window.__curveDebug()（页面自带 E2E 钩子）→ draws / lastDrawMs / avgDrawMs / drawn / visible
 *      - DOM：总节点数、变量树行、已选行、legend 项、canvas 尺寸
 *      - Performance.memory + CDP Performance.getMetrics（Task/Script/Layout/RecalcStyle）
 *      - fetch /api/curve/frames 轮询次数与耗时（Resource Timing）
 *      - CDP Profiler → 主线程热点函数（self time）
 *      - 设备 /api/curve/status（帧数、序列数、组数、halt、invalid、out_of_scope…）
 *   6. Stop（UI + API 双保险），确认 running=false、目标恢复运行
 *
 * 用法：
 *   cd web
 *   node scripts/bench-curve-perf.mjs                          # 默认 5 场景，输出 /tmp/curve-perf-before.json
 *   node scripts/bench-curve-perf.mjs --out /tmp/after.json --compare /tmp/curve-perf-before.json
 *   node scripts/bench-curve-perf.mjs --scenarios n40-b,n256-a --duration 10
 *
 * 常用参数：
 *   --url <url>            页面地址（默认 http://<device-ip>/#/debug-curve）
 *   --elf <path>           调试 ELF（默认 $HOME/reference firmware/firmware/reference-board/reference-debug-O0/firmware.elf）
 *   --out <json>           结果 JSON（默认 /tmp/curve-perf-before.json）
 *   --md <path>            额外输出 Markdown 摘要（可选）
 *   --compare <json>       与历史 JSON 对比，打印/写入 delta
 *   --label <text>         本次运行标签（默认由 --out 文件名推断，如 before/after）
 *   --scenarios <list>     逗号分隔场景 id，默认 n40-b,n128-b,n256-b,n128-a,n256-a
 *   --duration <sec>       测量窗口秒数（默认 10）
 *   --warmup <ms>          Start 后预热时间（默认 1500）
 *   --interval <ms>        本地采样间隔（默认 20）
 *   --headed               有头模式（调试用）
 *   --no-profile           关闭 CDP CPU Profiler（更轻量，但无热点函数）
 *   --keep-open            结束后保留浏览器（默认关闭）
 *   --playwright <path>    playwright 包路径（默认自动探测，见 resolvePlaywright）
 *   --selftest             只跑一个 40 路 / 3s 的冒烟场景（校验环境/设备）
 *
 * Playwright 依赖：本仓库 web/package.json 未内置 playwright。脚本会自动探测：
 *   $PLAYWRIGHT_MODULE → web/node_modules/playwright → ~/.npm/_npx/* /node_modules/playwright
 * 如果都没有，先在本机执行一次 `npx -y playwright@1.64.1 --version`（或 npm i -D playwright），
 * 浏览器可用 `npx playwright install chromium` 安装（本机 ~/.cache/ms-playwright 已有）。
 *
 * 安全边界：
 *   - 只 start/stop 本地采样；不会 OTA、不会改固件/页面、不会 commit。
 *   - 任何失败路径（异常/超时/Ctrl-C）都会尽力 POST /api/curve/stop 恢复目标运行。
 *   - 若设备上已有别的采样在跑，启动前会先 stop 并记录 warning（本任务独占设备）。
 *
 * 退出码：0=全部场景通过；1=有场景失败或 stop 校验失败；2=环境/参数错误。
 */

import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import crypto from 'node:crypto'
import {fileURLToPath, pathToFileURL} from 'node:url'

const SCRIPT_VERSION = '1.0.0'
const __dirname = path.dirname(fileURLToPath(import.meta.url))
const WEB_ROOT = path.resolve(__dirname, '..')

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const opts = {
    url: 'http://<device-ip>/#/debug-curve',
    elf: '$HOME/reference firmware/firmware/reference-board/reference-debug-O0/firmware.elf',
    out: '/tmp/curve-perf-before.json',
    md: '',
    compare: '',
    label: '',
    scenarios: '',
    duration: 10,
    warmup: 1500,
    interval: 20,
    headed: false,
    profile: true,
    keepOpen: false,
    playwright: '',
    selftest: false,
    fromJson: '',
    help: false,
  }
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]
    const next = () => {
      const value = argv[++i]
      if (value === undefined) throw new Error(`参数 ${arg} 缺少值`)
      return value
    }
    switch (arg) {
      case '--url': opts.url = next(); break
      case '--elf': opts.elf = next(); break
      case '--out': opts.out = next(); break
      case '--md': opts.md = next(); break
      case '--compare': opts.compare = next(); break
      case '--label': opts.label = next(); break
      case '--scenarios': opts.scenarios = next(); break
      case '--duration': opts.duration = Number(next()); break
      case '--warmup': opts.warmup = Number(next()); break
      case '--interval': opts.interval = Number(next()); break
      case '--playwright': opts.playwright = next(); break
      case '--headed': opts.headed = true; break
      case '--no-profile': opts.profile = false; break
      case '--keep-open': opts.keepOpen = true; break
      case '--selftest': opts.selftest = true; break
      case '--from-json': opts.fromJson = next(); break
      case '--help': case '-h': opts.help = true; break
      default: throw new Error(`未知参数 ${arg}（--help 查看用法）`)
    }
  }
  if (!opts.label) {
    const base = path.basename(opts.out).replace(/\.json$/i, '')
    opts.label = base.replace(/^curve-perf-/, '') || 'baseline'
  }
  if (opts.selftest) {
    opts.scenarios = 'n40-selftest'
    opts.duration = 3
    opts.warmup = 1000
    opts.out = opts.out.replace(/\.json$/i, '') + '-selftest.json'
  }
  return opts
}

const HELP = `DebugCurve 多变量性能基准

用法: node scripts/bench-curve-perf.mjs [选项]

选项:
  --url <url>          页面地址 (默认 http://<device-ip>/#/debug-curve)
  --elf <path>         调试 ELF 路径
  --out <json>         结果 JSON 路径 (默认 /tmp/curve-perf-before.json)
  --md <path>          额外输出 Markdown 摘要
  --compare <json>     与历史结果对比
  --label <text>       运行标签 (默认 from --out)
  --scenarios <list>   场景列表, 默认 n40-b,n128-b,n256-b,n128-a,n256-a
  --duration <sec>     测量窗口秒数 (默认 10)
  --warmup <ms>        Start 后预热 (默认 1500)
  --interval <ms>      本地采样间隔 (默认 20)
  --headed             有头浏览器
  --no-profile         关闭 CPU Profiler
  --keep-open          保留浏览器
  --playwright <path>  playwright 模块路径
  --from-json <json>   不跑基准，仅用既有 JSON 重新生成 Markdown/对比
  --selftest           冒烟测试 (40 路 / 3s)
  -h, --help           显示帮助
`

// 场景表：series=勾选曲线数（固定地址全局/静态变量），mode=B 默认绘制上限 / A 全部绘制
const SCENARIO_DEFS = [
  {id: 'n40-b', series: 40, mode: 'B', title: '40 series · B mode (default drawCap=64)'},
  {id: 'n128-b', series: 128, mode: 'B', title: '128 series · B mode (default drawCap=64)'},
  {id: 'n256-b', series: 256, mode: 'B', title: '256 series · B mode (default drawCap=64)'},
  {id: 'n128-a', series: 128, mode: 'A', title: '128 series · A mode (render all 128)'},
  {id: 'n256-a', series: 256, mode: 'A', title: '256 series · A mode (render all 256)'},
  {id: 'n40-selftest', series: 40, mode: 'B', title: '40 series · B mode (selftest)'},
]

// ---------------------------------------------------------------------------
// 通用工具
// ---------------------------------------------------------------------------
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))
const nowIso = () => new Date().toISOString()
const round = (value, digits = 2) => (Number.isFinite(value) ? Number(value.toFixed(digits)) : null)
const sha256 = (buffer) => crypto.createHash('sha256').update(buffer).digest('hex')

function fileSha256(file) {
  return sha256(fs.readFileSync(file))
}

function perfLog(...args) {
  console.log(`[${new Date().toISOString().slice(11, 23)}]`, ...args)
}

function pick(obj, keys) {
  const out = {}
  for (const key of keys) if (obj && key in obj) out[key] = obj[key]
  return out
}

// ---------------------------------------------------------------------------
// Playwright 定位（web/package.json 未内置；支持 npx 缓存/全局安装）
// ---------------------------------------------------------------------------
async function resolvePlaywright(explicitPath) {
  const candidates = []
  if (explicitPath) candidates.push(explicitPath)
  if (process.env.PLAYWRIGHT_MODULE) candidates.push(process.env.PLAYWRIGHT_MODULE)
  candidates.push('playwright')
  candidates.push(path.join(WEB_ROOT, 'node_modules/playwright/index.mjs'))
  const npxRoot = path.join(os.homedir(), '.npm', '_npx')
  try {
    const entries = fs.readdirSync(npxRoot)
      .map((name) => path.join(npxRoot, name, 'node_modules/playwright/index.mjs'))
      .filter((candidate) => fs.existsSync(candidate))
      .sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)
    candidates.push(...entries)
  } catch { /* ignore */ }
  for (const candidate of [
    '$HOME/deepseek-harness/node_modules/playwright/index.mjs',
    path.join(os.homedir(), '.npm-global/lib/node_modules/playwright/index.mjs'),
  ]) candidates.push(candidate)

  const errors = []
  for (const candidate of candidates) {
    try {
      const specifier = candidate === 'playwright' || !candidate.startsWith('/')
        ? candidate
        : pathToFileURL(candidate).href
      const mod = await import(specifier)
      if (mod && typeof mod.chromium?.launch === 'function') {
        return {module: mod, resolvedFrom: candidate}
      }
    } catch (error) {
      errors.push(`${candidate}: ${error.message}`)
    }
  }
  throw new Error(
    '无法加载 playwright。请在本机执行一次 `npx -y playwright@1.64.1 --version`，'
    + '或用 --playwright <路径> / PLAYWRIGHT_MODULE 指定。\n尝试过:\n' + errors.join('\n'),
  )
}

// ---------------------------------------------------------------------------
// 设备 HTTP
// ---------------------------------------------------------------------------
function deviceBase(url) {
  const parsed = new URL(url)
  return `${parsed.protocol}//${parsed.host}`
}

async function deviceFetch(base, pathname, options = {}) {
  const url = `${base}${pathname}`
  const response = await fetch(url, {cache: 'no-store', ...options})
  const text = await response.text()
  let json = null
  try { json = JSON.parse(text) } catch { /* not json */ }
  return {status: response.status, ok: response.ok, json, text}
}

async function getCurveStatus(base) {
  const res = await deviceFetch(base, '/api/curve/status')
  if (!res.ok || !res.json) throw new Error(`GET /api/curve/status 失败: HTTP ${res.status}`)
  return res.json
}

async function stopCurveSampling(base, reason) {
  let lastError = ''
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const res = await deviceFetch(base, '/api/curve/stop', {method: 'POST'})
      if (res.ok) {
        const status = await getCurveStatus(base)
        if (status.running !== true) return status
        lastError = 'stop 返回 ok 但 status.running 仍为 true'
      } else {
        lastError = `HTTP ${res.status} ${res.text.slice(0, 200)}`
      }
    } catch (error) {
      lastError = error.message
    }
    await sleep(300)
  }
  throw new Error(`停止本地采样失败(${reason}): ${lastError}`)
}

async function waitDeviceStopped(base, timeoutMs = 8000) {
  const deadline = Date.now() + timeoutMs
  let status = null
  while (Date.now() < deadline) {
    status = await getCurveStatus(base)
    if (status.running !== true) return status
    await sleep(250)
  }
  throw new Error(`等待设备停止采样超时: ${JSON.stringify(pick(status || {}, ['running', 'message', 'last_err']))}`)
}

async function fetchBundleInfo(base) {
  const started = Date.now()
  const response = await fetch(`${base}/`, {cache: 'no-store'})
  const buffer = Buffer.from(await response.arrayBuffer())
  return {
    httpStatus: response.status,
    loadMs: Date.now() - started,
    bytes: buffer.length,
    sha256: sha256(buffer),
  }
}

// ---------------------------------------------------------------------------
// 页面交互 helper
// ---------------------------------------------------------------------------
const TEXT = {
  start: /开始采样|Start sampling/i,
  stop: /停止采样|Stop sampling/i,
  uploadElf: /上传 ELF|Upload ELF/i,
  clearSelection: /清空选择|Clear selection/i,
  localSource: /设备本地|Device-local/i,
  localInterval: /本地采样间隔|Local interval/i,
  trigger: /触发方式|Trigger/i,
  periodic: /周期采样|Periodic/i,
}

async function pageReady(page) {
  await page.waitForFunction(() => !!document.body && /变量曲线|Data Curve/.test(document.body.innerText), null, {timeout: 30000})
  await page.waitForFunction(() => !!window.__curveDebug, null, {timeout: 30000})
  // 页面已完整挂载：Start 按钮存在
  await page.locator('button').filter({hasText: /开始采样|Start sampling/i}).first()
    .waitFor({state: 'visible', timeout: 30000})
}

async function innerTextOf(page) {
  return page.evaluate(() => document.body.innerText)
}

async function ensureLocalSource(page) {
  const sourceLabel = page.locator('label').filter({hasText: /采样来源|Sampling source/i}).first()
  const text = await sourceLabel.innerText()
  if (TEXT.localSource.test(text)) return 'already-local'
  await sourceLabel.locator('.el-select').click()
  await page.locator('.el-select-dropdown:visible .el-select-dropdown__item')
    .filter({hasText: TEXT.localSource}).first().click()
  await page.waitForFunction(() => /设备本地|Device-local/.test(document.body.innerText), null, {timeout: 10000})
  return 'switched-to-local'
}

async function ensureTriggerPeriodic(page) {
  const label = page.locator('label').filter({hasText: TEXT.trigger}).first()
  if ((await label.count()) === 0) return 'no-trigger-control'
  const text = await label.innerText()
  if (TEXT.periodic.test(text)) return 'already-periodic'
  await label.locator('.el-select').click()
  await page.locator('.el-select-dropdown:visible .el-select-dropdown__item')
    .filter({hasText: TEXT.periodic}).first().click()
  return 'switched-to-periodic'
}

async function ensureInterval(page, intervalMs) {
  const label = page.locator('label').filter({hasText: TEXT.localInterval}).first()
  if ((await label.count()) === 0) throw new Error('找不到“本地采样间隔”选择框')
  const text = await label.innerText()
  const wanted = `${intervalMs} ms`
  if (text.includes(wanted)) return {already: true, text: text.trim()}
  await label.locator('.el-select').click()
  await page.locator('.el-select-dropdown:visible .el-select-dropdown__item')
    .filter({hasText: new RegExp(`^\\s*${intervalMs}\\s*ms\\s*$`, 'i')}).first().click()
  await page.waitForFunction(
    (value) => document.body.innerText.includes(value),
    wanted,
    {timeout: 5000},
  )
  return {already: false, text: wanted}
}

async function uploadElf(page, elfPath) {
  const started = Date.now()
  const input = page.locator('input[type=file]').first()
  if ((await input.count()) === 0) throw new Error('页面上找不到 ELF 文件输入框')
  await input.setInputFiles(elfPath)
  await page.waitForFunction(() => {
    const rows = document.querySelectorAll('.var-row').length
    const alerts = [...document.querySelectorAll('.el-alert--error .el-alert__title')]
      .map((node) => node.textContent || '').join('|')
    return rows > 0 || alerts.length > 0
  }, null, {timeout: 180000})
  const bodyText = await innerTextOf(page)
  const errorMatch = bodyText.match(/ELF[^\n]{0,80}(失败|错误|无效|invalid)[^\n]{0,120}/i)
  if (errorMatch && (await page.locator('.var-row').count()) === 0) {
    throw new Error(`ELF 解析失败: ${errorMatch[0]}`)
  }
  const summaryMatch = bodyText.match(/DWARF[：:]\s*([\d,]+)\s*个变量（([\d,]+)\s*个含固定地址）/)
  const variablesMatch = bodyText.match(/变量列表[：:]\s*([\d,]+)/)
  return {
    parseMs: Date.now() - started,
    dwarfVariables: summaryMatch ? Number(summaryMatch[1].replace(/,/g, '')) : null,
    fixedAddressVariables: summaryMatch ? Number(summaryMatch[2].replace(/,/g, '')) : null,
    visibleVariables: variablesMatch ? Number(variablesMatch[1].replace(/,/g, '')) : null,
    rowsRendered: await page.locator('.var-row').count(),
  }
}

async function selectedCount(page) {
  return page.evaluate(() => {
    const match = document.body.innerText.match(/已选变量:\s*(\d+)\s*\/\s*(\d+)/)
    return match ? Number(match[1]) : -1
  })
}

async function clearSelection(page) {
  const count = await selectedCount(page)
  if (count <= 0) return
  const button = page.getByRole('button', {name: TEXT.clearSelection}).first()
  if ((await button.count()) === 0) throw new Error('找不到“清空选择”按钮')
  await button.click()
  const deadline = Date.now() + 5000
  while (Date.now() < deadline) {
    if ((await selectedCount(page)) === 0) return
    await sleep(100)
  }
  throw new Error('清空选择后已选数量未归零')
}

/** 固定地址变量行（地址列为 0x%08X 且带 checkbox = 可选择、无需 halt）。 */
async function fixedAddressRows(page) {
  return page.evaluate(() => {
    const rows = [...document.querySelectorAll('.var-row')]
    const result = []
    for (const row of rows) {
      const addr = (row.querySelector('.var-addr')?.textContent || '').trim()
      if (!/^0x[0-9a-f]{8}$/i.test(addr)) continue
      if (!row.querySelector('.el-checkbox')) continue
      const name = (row.querySelector('.var-name')?.textContent || '').trim().replace(/^[▾▸]\s*/, '')
      const sameAddressName = (row.querySelector('.var-name')?.getAttribute('title') || '').split('\n')[0]
      result.push({name: sameAddressName || name, addr})
    }
    return result
  })
}

async function showMoreVariables(page) {
  const button = page.locator('.show-more-row button').first()
  if ((await button.count()) === 0) return false
  if (!(await button.isVisible())) return false
  await button.click()
  await sleep(150)
  return true
}

async function selectFixedSeries(page, target, log) {
  await clearSelection(page)
  // 先确保变量树里至少有 target 个固定地址行；不足时点“显示更多变量”
  for (let attempt = 0; attempt < 20; attempt++) {
    const rows = await fixedAddressRows(page)
    if (rows.length >= target) break
    if (!(await showMoreVariables(page))) break
  }
  const rows = await fixedAddressRows(page)
  if (rows.length < target) {
    throw new Error(`固定地址变量不足: 需要 ${target}，页面可用 ${rows.length}（请确认只勾选固定地址/全局变量）`)
  }
  // 一次性点击前 target 个 checkbox（选择顺序固定 = 变量树顺序；跨轮次可复现）
  const clicked = await page.evaluate((count) => {
    const rows = [...document.querySelectorAll('.var-row')]
    let done = 0
    for (const row of rows) {
      if (done >= count) break
      const addr = (row.querySelector('.var-addr')?.textContent || '').trim()
      if (!/^0x[0-9a-f]{8}$/i.test(addr)) continue
      const checkbox = row.querySelector('.el-checkbox')
      if (!checkbox) continue
      checkbox.click()
      done += 1
    }
    return done
  }, target)
  if (clicked < target) {
    // 兜底：逐次补点（Vue 重渲染后重新查询）
    for (let attempt = 0; attempt < 10; attempt++) {
      const current = await selectedCount(page)
      if (current >= target) break
      await page.evaluate((need) => {
        const rows = [...document.querySelectorAll('.var-row')]
        let done = 0
        for (const row of rows) {
          if (done >= need) break
          const addr = (row.querySelector('.var-addr')?.textContent || '').trim()
          if (!/^0x[0-9a-f]{8}$/i.test(addr)) continue
          const checkbox = row.querySelector('.el-checkbox')
          if (!checkbox || row.classList.contains('selected')) continue
          checkbox.click()
          done += 1
        }
        return done
      }, target - current)
      await sleep(200)
    }
  }
  const deadline = Date.now() + 15000
  let count = -1
  while (Date.now() < deadline) {
    count = await selectedCount(page)
    if (count >= target) break
    await sleep(150)
  }
  if (count !== target) throw new Error(`勾选曲线数量不符: 期望 ${target}，实际 ${count}`)
  const picked = (await fixedAddressRows(page)).slice(0, target)
  log(`已勾选 ${target} 路固定地址变量（首: ${picked[0]?.name} @ ${picked[0]?.addr}；末: ${picked.at(-1)?.name} @ ${picked.at(-1)?.addr}）`)
  return {
    selected: target,
    first: picked.slice(0, 5),
    last: picked.slice(-5),
  }
}

async function setRenderMode(page, mode, log) {
  const wantAll = mode === 'A'
  const before = await page.evaluate(() => window.__curveDebug?.() || null)
  if (!before) throw new Error('window.__curveDebug 不可用，无法确认渲染模式')
  if (Boolean(before.renderAll) !== wantAll) {
    const toggle = page.locator('.render-bar .el-switch').first()
    if ((await toggle.count()) === 0) throw new Error('找不到 A/B 渲染模式开关')
    await toggle.click()
    await page.waitForFunction((want) => window.__curveDebug?.().renderAll === want, wantAll, {timeout: 5000})
  }
  const after = await page.evaluate(() => window.__curveDebug?.() || null)
  log(`渲染模式=${after.renderAll ? 'A(全部绘制)' : 'B(上限绘制)'} drawCap=${after.drawCap}`)
  return {renderAll: after.renderAll, drawCap: after.drawCap}
}

async function waitSamplingStarted(page, timeoutMs = 20000) {
  const started = Date.now()
  await page.waitForFunction(() => {
    const debug = window.__curveDebug?.()
    return !!debug && debug.ringCount > 0
  }, null, {timeout: timeoutMs})
  return Date.now() - started
}

async function clickStart(page) {
  const button = page.getByRole('button', {name: TEXT.start}).first()
  await button.waitFor({state: 'visible', timeout: 15000})
  if (await button.isDisabled()) {
    throw new Error('“开始采样”按钮不可用（ELF/变量选择未就绪，或本地模式校验失败）')
  }
  await button.click()
}

async function stopSamplingViaUi(page) {
  const button = page.getByRole('button', {name: TEXT.stop}).first()
  if ((await button.count()) === 0 || !(await button.isVisible())) return false
  await button.click()
  expectStartButton(page).catch(() => {})
  const deadline = Date.now() + 10000
  while (Date.now() < deadline) {
    const stopVisible = await page.getByRole('button', {name: TEXT.stop}).first().isVisible().catch(() => false)
    const startVisible = await page.getByRole('button', {name: TEXT.start}).first().isVisible().catch(() => false)
    if (!stopVisible && startVisible) return true
    await sleep(150)
  }
  return false
}

function expectStartButton(page) {
  return page.locator('button').filter({hasText: TEXT.start}).first().waitFor({state: 'visible', timeout: 10000})
}

/** 解析页面上容易读的本地采样统计文本（兜底/展示用）。 */
async function readUiStats(page) {
  return page.evaluate(() => {
    const text = document.body.innerText
    const grab = (re) => {
      const match = text.match(re)
      return match ? match[1].trim() : null
    }
    return {
      effectiveRate: grab(/实际采样率:\s*([\d.]+ Hz)/),
      points: grab(/采样点:\s*(\d+)/),
      localStatus: grab(/本地状态:\s*([^\n]+)/),
      halt: grab(/Halt:\s*(ON|OFF)/),
      frames: grab(/(?:^|\n)帧:\s*(\d+)/),
      invalid: grab(/无效:\s*(\d+)/),
      outOfScope: grab(/超出函数范围:\s*(\d+)/),
      overwritten: grab(/覆盖:\s*(\d+)/),
      series: grab(/曲线:\s*(\d+\/\d+)/),
      groups: grab(/(?:^|\n)组:\s*(\d+)/),
      currentGroup: grab(/当前组:\s*([^\n]+)/),
      groupRate: grab(/组刷新率:\s*([\d.]+ Hz)/),
    }
  })
}

async function readDomSnapshot(page) {
  return page.evaluate(() => {
    const debug = window.__curveDebug?.() || null
    const canvas = [...document.querySelectorAll('canvas')].map((node) => ({
      width: node.width,
      height: node.height,
      clientWidth: node.clientWidth,
      clientHeight: node.clientHeight,
    }))
    return {
      totalNodes: document.querySelectorAll('*').length,
      variableRowsRendered: document.querySelectorAll('.var-row').length,
      variableRowsTotal: debug ? debug.variableRows : null,
      selectedRowsRendered: document.querySelectorAll('.selected-row').length,
      legendItemsRendered: document.querySelectorAll('.legend-item').length,
      legendItemsComputed: debug ? debug.legendItems : null,
      legendMoreButtons: document.querySelectorAll('.legend-more').length,
      canvas,
      debug,
    }
  })
}

// ---------------------------------------------------------------------------
// 测量窗口（页面内注入）
// ---------------------------------------------------------------------------
/**
 * 注入 rAF 计数、longtask、long-animation-frame、MutationObserver，
 * 运行 durationMs 后返回原始数据（在页面内完成，避免 Node↔页面时钟对齐问题）。
 */
async function measureWindow(page, durationMs) {
  return page.evaluate((duration) => new Promise((resolve) => {
    const bench = {
      start: performance.now(),
      end: 0,
      longtasks: [],
      loaf: [],
      mutations: 0,
      raf: [],
      memory: null,
      framesPoll: {count: 0, totalMs: 0, maxMs: 0},
      longtaskSupported: false,
      loafSupported: false,
    }
    let longtaskObserver = null
    let loafObserver = null
    let mutationObserver = null
    try {
      longtaskObserver = new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) {
          bench.longtasks.push({start: entry.startTime, duration: entry.duration})
        }
      })
      longtaskObserver.observe({type: 'longtask'})
      bench.longtaskSupported = true
    } catch (error) { bench.longtaskError = String(error) }
    try {
      loafObserver = new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) {
          bench.loaf.push({
            start: entry.startTime,
            duration: entry.duration,
            blockingDuration: entry.blockingDuration,
            styleAndLayoutDuration: entry.styleAndLayoutDuration,
            renderStart: entry.renderStart,
            scripts: (entry.scripts || []).map((script) => ({
              name: script.sourceFunctionName,
              invoker: script.invoker,
              duration: script.duration,
              forcedStyleAndLayoutDuration: script.forcedStyleAndLayoutDuration,
              pauseDuration: script.pauseDuration,
            })),
          })
        }
      })
      loafObserver.observe({type: 'long-animation-frame'})
      bench.loafSupported = true
    } catch (error) { bench.loafError = String(error) }
    try {
      performance.setResourceTimingBufferSize?.(5000)
    } catch { /* ignore */ }
    try {
      mutationObserver = new MutationObserver((records) => { bench.mutations += records.length })
      mutationObserver.observe(document.body, {childList: true, subtree: true, attributes: true, characterData: true})
      bench.mutationSupported = true
    } catch (error) { bench.mutationError = String(error) }

    let finished = false
    const rafTimes = []
    function tick(timestamp) {
      if (finished) return
      rafTimes.push(timestamp)
      requestAnimationFrame(tick)
    }
    requestAnimationFrame(tick)

    const memorySamples = []
    const memoryTimer = setInterval(() => {
      if (performance.memory) {
        memorySamples.push({
          t: performance.now(),
          used: performance.memory.usedJSHeapSize,
          total: performance.memory.totalJSHeapSize,
        })
      }
    }, 500)

    setTimeout(() => {
      finished = true
      clearInterval(memoryTimer)
      bench.memorySamples = memorySamples
      bench.end = performance.now()
      bench.raf = rafTimes
      bench.memory = performance.memory
        ? {used: performance.memory.usedJSHeapSize, total: performance.memory.totalJSHeapSize}
        : null
      try {
        const resources = performance.getEntriesByType('resource')
          .filter((entry) => entry.name.includes('/api/curve/frames') && entry.startTime >= bench.start)
        bench.framesPoll.count = resources.length
        for (const entry of resources) {
          bench.framesPoll.totalMs += entry.duration
          if (entry.duration > bench.framesPoll.maxMs) bench.framesPoll.maxMs = entry.duration
        }
      } catch { /* ignore */ }
      try { longtaskObserver?.disconnect() } catch { /* ignore */ }
      try { loafObserver?.disconnect() } catch { /* ignore */ }
      try { mutationObserver?.disconnect() } catch { /* ignore */ }
      resolve(bench)
    }, duration)
  }), durationMs)
}

function summarizeRaf(raf) {
  if (!raf || raf.length < 2) return {frames: raf?.length || 0, fps: 0}
  const deltas = []
  for (let i = 1; i < raf.length; i++) deltas.push(raf[i] - raf[i - 1])
  deltas.sort((a, b) => a - b)
  const frames = raf.length
  const wall = (raf[raf.length - 1] - raf[0]) / 1000
  const percentile = (p) => deltas[Math.min(deltas.length - 1, Math.floor(deltas.length * p))] ?? null
  return {
    frames,
    wallSec: round(wall, 3),
    fps: wall > 0 ? round(frames / wall, 2) : 0,
    meanDeltaMs: round(deltas.reduce((sum, value) => sum + value, 0) / deltas.length, 2),
    p50Ms: round(percentile(0.5), 2),
    p95Ms: round(percentile(0.95), 2),
    p99Ms: round(percentile(0.99), 2),
    maxMs: round(deltas[deltas.length - 1], 2),
    droppedOver25ms: deltas.filter((value) => value > 25).length,
    droppedOver50ms: deltas.filter((value) => value > 50).length,
  }
}

function summarizeLongtasks(entries, windowMs) {
  const totalMs = entries.reduce((sum, entry) => sum + entry.duration, 0)
  const maxMs = entries.reduce((max, entry) => Math.max(max, entry.duration), 0)
  return {
    count: entries.length,
    totalMs: round(totalMs, 2),
    maxMs: round(maxMs, 2),
    meanMs: entries.length ? round(totalMs / entries.length, 2) : 0,
    perSec: round((entries.length / windowMs) * 1000, 2),
    mainThreadBlockedPct: round((totalMs / windowMs) * 100, 2),
    top: entries
      .slice()
      .sort((a, b) => b.duration - a.duration)
      .slice(0, 10)
      .map((entry) => ({startMs: round(entry.start, 1), durationMs: round(entry.duration, 2)})),
  }
}

function summarizeLoaf(entries, windowMs) {
  const totalMs = entries.reduce((sum, entry) => sum + entry.duration, 0)
  const blockingMs = entries.reduce((sum, entry) => sum + (entry.blockingDuration || 0), 0)
  const invokers = {}
  const scriptTotals = {}
  for (const entry of entries) {
    for (const script of entry.scripts || []) {
      const invoker = script.invoker || '(unknown)'
      invokers[invoker] = invokers[invoker] || {count: 0, ms: 0}
      invokers[invoker].count += 1
      invokers[invoker].ms += script.duration || 0
      const key = `${script.name || '(anonymous)'} @ ${invoker}`
      scriptTotals[key] = (scriptTotals[key] || 0) + (script.duration || 0)
    }
  }
  for (const value of Object.values(invokers)) value.ms = round(value.ms, 2)
  return {
    count: entries.length,
    totalMs: round(totalMs, 2),
    blockingMs: round(blockingMs, 2),
    maxMs: round(entries.reduce((max, entry) => Math.max(max, entry.duration), 0), 2),
    mainThreadBlockedPct: round((totalMs / windowMs) * 100, 2),
    invokers,
    topScripts: Object.entries(scriptTotals)
      .sort((a, b) => b[1] - a[1])
      .slice(0, 10)
      .map(([name, ms]) => ({name, ms: round(ms, 2)})),
  }
}

function summarizeProfile(profile, windowMs, intervalUs = 1000) {
  if (!profile || !profile.profile) return null
  const nodes = new Map()
  for (const node of profile.profile.nodes) nodes.set(node.id, node)
  const hitCounts = new Map()
  for (const id of profile.profile.samples || []) hitCounts.set(id, (hitCounts.get(id) || 0) + 1)
  const grouped = new Map()
  let totalSamples = 0
  for (const [id, hits] of hitCounts) {
    totalSamples += hits
    const frame = nodes.get(id)?.callFrame || {}
    const key = `${frame.functionName || '(anonymous)'}|${frame.url || ''}|${frame.lineNumber ?? -1}|${frame.columnNumber ?? -1}`
    const current = grouped.get(key) || {
      functionName: frame.functionName || '(anonymous)',
      url: frame.url || '',
      line: frame.lineNumber ?? -1,
      column: frame.columnNumber ?? -1,
      hits: 0,
    }
    current.hits += hits
    grouped.set(key, current)
  }
  const totalMs = (totalSamples * intervalUs) / 1000
  return {
    samplingIntervalUs: intervalUs,
    totalSamples,
    totalSelfMs: round(totalMs, 2),
    selfPctOfWindow: round((totalMs / windowMs) * 100, 2),
    top: [...grouped.values()]
      .sort((a, b) => b.hits - a.hits)
      .slice(0, 25)
      .map((entry) => ({
        ...entry,
        selfMs: round((entry.hits * intervalUs) / 1000, 2),
        selfPct: round(((entry.hits * intervalUs) / 1000 / windowMs) * 100, 2),
      })),
  }
}

function cdpMetricsToMap(metrics) {
  const map = {}
  for (const metric of metrics || []) map[metric.name] = metric.value
  return map
}

function diffCdpMetrics(before, after) {
  const names = [
    'TaskDuration', 'ScriptDuration', 'TaskOtherDuration', 'LayoutDuration',
    'RecalcStyleDuration', 'ThreadTime', 'ProcessTime',
    'JSHeapUsedSize', 'JSHeapTotalSize', 'Nodes', 'LayoutCount', 'RecalcStyleCount',
    'JSEventListeners',
  ]
  const out = {}
  for (const name of names) {
    if (before[name] === undefined || after[name] === undefined) continue
    const delta = after[name] - before[name]
    out[name] = name.endsWith('Size') || name === 'Nodes' || name === 'LayoutCount'
      || name === 'RecalcStyleCount' || name === 'JSEventListeners'
      ? Math.round(delta)
      : round(delta, 4)
  }
  return out
}

/** 强制 GC（CDP HeapProfiler）后再读内存，过滤“未回收垃圾”造成的巨大噪声。 */
async function forceGc(cdp) {
  try {
    await cdp.send('HeapProfiler.enable')
    await cdp.send('HeapProfiler.collectGarbage')
    return true
  } catch {
    return false
  }
}

// ---------------------------------------------------------------------------
// 单场景执行
// ---------------------------------------------------------------------------
async function runScenario({page, cdp, context, opts, scenario, base, elfInfo, browserInfo}) {
  const log = (...args) => perfLog(`[${scenario.id}]`, ...args)
  const result = {
    id: scenario.id,
    title: scenario.title,
    requestedSeries: scenario.series,
    requestedMode: scenario.mode,
    status: 'pending',
    timings: {},
    warnings: [],
    errors: [],
  }
  const startedAt = Date.now()
  let samplingStarted = false
  try {
    // 1) 重新加载页面，保证每个场景从干净状态开始（B/A 默认、内存、DOM 一致）
    const gotoStarted = Date.now()
    await page.goto(opts.url, {waitUntil: 'load', timeout: 60000})
    await pageReady(page)
    result.timings.pageLoadMs = Date.now() - gotoStarted
    result.pageMeta = await page.evaluate(() => ({
      webInfo: window.__BIRDIEBOT_WEB__ || null,
      title: document.title,
      viewport: {width: window.innerWidth, height: window.innerHeight, dpr: window.devicePixelRatio},
      location: location.href,
    }))

    const sourceState = await ensureLocalSource(page)
    if (sourceState !== 'already-local') result.warnings.push(`采样来源不是默认设备本地，已切换（${sourceState}）`)
    const triggerState = await ensureTriggerPeriodic(page)
    if (triggerState === 'switched-to-periodic') result.warnings.push('触发方式已从断点切换为周期采样')
    result.interval = await ensureInterval(page, opts.interval)

    // 2) 上传 ELF + 勾选固定地址变量
    result.elf = await uploadElf(page, opts.elf)
    log(`ELF 就绪 ${result.elf.parseMs}ms，DWARF 变量 ${result.elf.dwarfVariables}，固定地址 ${result.elf.fixedAddressVariables}`)
    const selectStarted = Date.now()
    result.selection = await selectFixedSeries(page, scenario.series, log)
    result.timings.selectionMs = Date.now() - selectStarted
    result.effectiveSeries = result.selection.selected

    // 3) 渲染模式（B=默认上限 / A=全部绘制）
    result.render = await setRenderMode(page, scenario.mode, log)

    // 4) 启动采样
    await clickStart(page)
    samplingStarted = true
    const firstDrawMs = await waitSamplingStarted(page)
    result.timings.firstDrawMs = firstDrawMs
    await sleep(opts.warmup)
    result.timings.warmupMs = opts.warmup

    // 5) 预热后、测量前快照（先强制 GC，内存增量才反映“窗口内真实新增/泄漏”而非 GC 时机噪声）
    const gcBefore = await forceGc(cdp)
    const deviceBefore = await getCurveStatus(base)
    const debugBefore = await page.evaluate(() => window.__curveDebug?.() || null)
    const domBefore = await readDomSnapshot(page)
    const memoryBefore = await page.evaluate(() => (performance.memory
      ? {used: performance.memory.usedJSHeapSize, total: performance.memory.totalJSHeapSize}
      : null))
    const uiBefore = await readUiStats(page)

    if (opts.profile) {
      await cdp.send('Profiler.enable')
      await cdp.send('Profiler.setSamplingInterval', {interval: 1000})
    }
    const cdpBefore = cdpMetricsToMap((await cdp.send('Performance.getMetrics')).metrics)
    if (opts.profile) await cdp.send('Profiler.start')

    // 6) 测量窗口（页面内 rAF/longtask/LoAF/mutation/resource；Node 侧同时等待）
    const windowStart = Date.now()
    const benchPromise = measureWindow(page, opts.duration * 1000)
    // 测量窗口期间取一次设备运行状态（帧数增量/实际速率）
    await sleep(Math.max(0, opts.duration * 1000 - 800))
    const deviceMid = await getCurveStatus(base)
    const bench = await benchPromise
    const windowMs = bench.end - bench.start
    const measurementWallMs = Date.now() - windowStart
    result.timings.windowMs = round(windowMs, 1)
    result.timings.measurementWallMs = measurementWallMs

    if (opts.profile) {
      try {
        result.profile = summarizeProfile(await cdp.send('Profiler.stop'), windowMs, 1000)
      } catch (error) {
        result.warnings.push(`Profiler.stop 失败: ${error.message}`)
      }
    }
    const gcAfter = await forceGc(cdp)
    const cdpAfter = cdpMetricsToMap((await cdp.send('Performance.getMetrics')).metrics)
    const debugAfter = await page.evaluate(() => window.__curveDebug?.() || null)
    const domAfter = await readDomSnapshot(page)
    const memoryAfter = await page.evaluate(() => (performance.memory
      ? {used: performance.memory.usedJSHeapSize, total: performance.memory.totalJSHeapSize}
      : null))
    const uiAfter = await readUiStats(page)
    const deviceAfterWindow = await getCurveStatus(base)

    // 7) 停采样（先 UI，后 API 双保险）
    const stopStarted = Date.now()
    const uiStopped = await stopSamplingViaUi(page)
    let deviceStopped
    try {
      deviceStopped = await stopCurveSampling(base, 'scenario-end')
    } catch (error) {
      result.errors.push(error.message)
      deviceStopped = await waitDeviceStopped(base).catch(() => null)
    }
    samplingStarted = false
    result.timings.stopMs = Date.now() - stopStarted
    result.stop = {
      uiStopped,
      deviceRunning: deviceStopped?.running ?? null,
      deviceMessage: deviceStopped?.message ?? null,
      lastErr: deviceStopped?.last_err ?? null,
    }

    // 8) 指标整理
    const rafSummary = summarizeRaf(bench.raf)
    // __curveDebug().draws / avgDrawMs 是“页面挂载以来累计值”，用 before 快照差分出测量窗口内的真实 draw 指标。
    const drawsDelta = debugAfter && debugBefore ? debugAfter.draws - debugBefore.draws : null
    const drawMsTotalBefore = debugBefore ? debugBefore.draws * debugBefore.avgDrawMs : 0
    const drawMsTotalAfter = debugAfter ? debugAfter.draws * debugAfter.avgDrawMs : 0
    const avgDrawMsWindow = drawsDelta && drawsDelta > 0
      ? (drawMsTotalAfter - drawMsTotalBefore) / drawsDelta
      : (debugAfter?.avgDrawMs ?? null)
    const drawTotalMsWindow = avgDrawMsWindow !== null && drawsDelta !== null ? avgDrawMsWindow * drawsDelta : 0
    const pollTotalMs = bench.framesPoll.totalMs
    const framesDelta = (deviceAfterWindow.frames ?? 0) - (deviceBefore.frames ?? 0)
    const windowSec = windowMs / 1000
    result.window = {
      durationMs: round(windowMs, 1),
      raf: rafSummary,
      longtasks: summarizeLongtasks(bench.longtasks, windowMs),
      loaf: summarizeLoaf(bench.loaf, windowMs),
      draw: {
        draws: debugAfter?.draws ?? null,
        drawsDelta,
        drawFps: drawsDelta !== null ? round(drawsDelta / windowSec, 2) : null,
        lastDrawMs: debugAfter?.lastDrawMs ?? null,
        avgDrawMs: avgDrawMsWindow !== null ? round(avgDrawMsWindow, 2) : null,
        avgDrawMsCumulative: debugAfter?.avgDrawMs ?? null,
        lastDrawMsBefore: debugBefore?.lastDrawMs ?? null,
        drawCpuSharePct: drawsDelta ? round((drawTotalMsWindow / windowMs) * 100, 2) : null,
        drawn: debugAfter?.drawn ?? null,
        visible: debugAfter?.visible ?? null,
        hiddenByCap: debugAfter && debugAfter.visible !== null && debugAfter.drawn !== null
          ? Math.max(0, debugAfter.visible - debugAfter.drawn)
          : null,
        selected: debugAfter?.selected ?? null,
        ringCount: debugAfter?.ringCount ?? null,
        hiddenPauses: debugAfter?.hiddenPauses ?? null,
      },
      dom: {
        totalNodes: domAfter.totalNodes,
        nodesDelta: domAfter.totalNodes - domBefore.totalNodes,
        variableRowsRendered: domAfter.variableRowsRendered,
        variableRowsTotal: domAfter.variableRowsTotal,
        selectedRowsRendered: domAfter.selectedRowsRendered,
        legendItemsRendered: domAfter.legendItemsRendered,
        legendItemsComputed: domAfter.legendItemsComputed,
        legendMoreButtons: domAfter.legendMoreButtons,
        canvas: domAfter.canvas,
        mutations: bench.mutations,
      },
      memory: {
        before: memoryBefore,
        after: memoryAfter,
        deltaUsedBytes: memoryBefore && memoryAfter ? memoryAfter.used - memoryBefore.used : null,
        windowEndUsed: bench.memory?.used ?? null,
        peakUsed: bench.memorySamples?.length
          ? Math.max(...bench.memorySamples.map((sample) => sample.used))
          : null,
        samples: bench.memorySamples?.length ?? 0,
        gcBefore,
        gcAfter,
        supported: Boolean(memoryBefore),
      },
      cdp: {
        windowMs: measurementWallMs,
        before: pick(cdpBefore, ['JSHeapUsedSize', 'Nodes', 'JSEventListeners']),
        after: pick(cdpAfter, ['JSHeapUsedSize', 'Nodes', 'JSEventListeners']),
        delta: diffCdpMetrics(cdpBefore, cdpAfter),
      },
      network: {
        framePolls: bench.framesPoll.count,
        framePollsPerSec: round(bench.framesPoll.count / windowSec, 2),
        framePollAvgMs: bench.framesPoll.count ? round(bench.framesPoll.totalMs / bench.framesPoll.count, 2) : null,
        framePollMaxMs: round(bench.framesPoll.maxMs, 2),
        framePollTotalMs: round(pollTotalMs, 2),
      },
      ui: {before: uiBefore, after: uiAfter},
    }
    result.device = {
      before: summarizeDeviceStatus(deviceBefore),
      mid: summarizeDeviceStatus(deviceMid),
      afterWindow: summarizeDeviceStatus(deviceAfterWindow),
      stopped: summarizeDeviceStatus(deviceStopped || {}),
      framesDelta,
      framesRate: round(framesDelta / windowSec, 2),
      seriesCount: deviceAfterWindow.series_count ?? null,
      groupCount: deviceAfterWindow.group_count ?? null,
      halt: deviceAfterWindow.halt ?? null,
      intervalUs: deviceAfterWindow.interval_us ?? null,
      linkHz: deviceAfterWindow.link_hz ?? null,
      invalid: deviceAfterWindow.invalid ?? null,
      outOfScope: deviceAfterWindow.out_of_scope ?? null,
      overwritten: deviceAfterWindow.overwritten ?? null,
      error: deviceAfterWindow.error ?? null,
      ringFrames: deviceAfterWindow.ring_frames ?? null,
      lastErr: deviceAfterWindow.last_err ?? null,
    }
    if (result.device.halt === true) {
      result.warnings.push('本次采样 halt=true（选中变量含寄存器/栈帧/variants），与“固定地址避免 halt”基准目标不符')
    }
    if (result.device.seriesCount !== null && result.device.seriesCount !== scenario.series) {
      result.warnings.push(`设备侧 series_count=${result.device.seriesCount}，与期望 ${scenario.series} 不符`)
    }
    if (result.device.groupCount && result.device.groupCount > 1) {
      result.note = `v2 多组轮转：${result.device.groupCount} 组 × 32 路，单组刷新率 ≈ 总帧率/${result.device.groupCount}`
    }
    if (rafSummary.fps < 30) result.warnings.push(`rAF FPS ${rafSummary.fps} < 30`)
    if (result.window.longtasks.count > 0) {
      result.warnings.push(`检测到 ${result.window.longtasks.count} 个 long task，总 ${result.window.longtasks.totalMs}ms`)
    }
    result.timings.totalMs = Date.now() - startedAt
    result.status = result.errors.length ? 'failed' : 'ok'
    log(`完成：rAF ${rafSummary.fps} FPS，longtask ${result.window.longtasks.count}/${result.window.longtasks.totalMs}ms，`
      + `draw ${result.window.draw.drawFps}fps avg ${result.window.draw.avgDrawMs}ms，DOM ${domAfter.totalNodes} 节点`)
  } catch (error) {
    result.status = 'failed'
    result.errors.push(error.stack || error.message)
    perfLog(`[${scenario.id}] FAILED:`, error.message)
    // 尽力停止设备侧采样（UI 可能已不可用）
    try {
      await stopCurveSampling(base, `scenario-failed:${scenario.id}`)
      samplingStarted = false
    } catch (stopError) {
      result.errors.push(`失败清理 stop 出错: ${stopError.message}`)
    }
    result.timings.totalMs = Date.now() - startedAt
  } finally {
    if (samplingStarted) {
      try { await stopCurveSampling(base, `scenario-finally:${scenario.id}`) } catch { /* best effort */ }
    }
  }
  return result
}

function summarizeDeviceStatus(status) {
  if (!status || typeof status !== 'object') return null
  return pick(status, [
    'ok', 'running', 'trigger', 'series_count', 'group_count', 'current_group',
    'interval_us', 'frames', 'invalid', 'out_of_scope', 'overwritten', 'error',
    'halt', 'link_hz', 'ring_frames', 'served_seq', 'hits', 'sampled', 'throttled',
    'consecutive_errors', 'last_err', 'message', 'sample_bytes',
  ])
}

// ---------------------------------------------------------------------------
// 对比 / Markdown
// ---------------------------------------------------------------------------
function loadPrevious(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (error) {
    throw new Error(`读取对比 JSON 失败 ${file}: ${error.message}`)
  }
}

function compareRuns(current, previous) {
  const prevById = new Map((previous.scenarios || []).map((scenario) => [scenario.id, scenario]))
  const rows = []
  for (const scenario of current.scenarios || []) {
    const prev = prevById.get(scenario.id)
    if (!prev || prev.status !== 'ok' || scenario.status !== 'ok') continue
    const metric = (pathFn) => {
      const a = pathFn(prev)
      const b = pathFn(scenario)
      if (a === null || a === undefined || b === null || b === undefined) return {previous: a, current: b, delta: null, deltaPct: null}
      return {previous: a, current: b, delta: round(b - a, 2), deltaPct: a === 0 ? null : round(((b - a) / a) * 100, 1)}
    }
    rows.push({
      scenario: scenario.id,
      fps: metric((s) => s.window?.raf?.fps),
      longtaskCount: metric((s) => s.window?.longtasks?.count),
      longtaskTotalMs: metric((s) => s.window?.longtasks?.totalMs),
      longtaskBlockedPct: metric((s) => s.window?.longtasks?.mainThreadBlockedPct),
      drawFps: metric((s) => s.window?.draw?.drawFps),
      avgDrawMs: metric((s) => s.window?.draw?.avgDrawMs),
      drawCpuPct: metric((s) => s.window?.draw?.drawCpuSharePct),
      nodes: metric((s) => s.window?.dom?.totalNodes),
      memoryDeltaMb: metric((s) => (s.window?.memory?.deltaUsedBytes === null ? null : s.window.memory.deltaUsedBytes / 1048576)),
      deviceFrames: metric((s) => s.device?.framesDelta),
    })
  }
  return rows
}

function buildMarkdown(result, comparison) {
  const lines = []
  const meta = result.meta
  lines.push(`# DebugCurve 多变量性能基准 — ${result.label}`, '')
  lines.push(`- 生成时间：${result.createdAt}`)
  lines.push(`- 页面：${result.config.url}`)
  lines.push(`- 设备：${meta.device.base}（bundle sha256 \`${otherHash(meta.device.bundle?.sha256)}\`，web ${meta.device.webInfo?.webVersion || '?'}）`)
  lines.push(`- ELF：\`${result.config.elf}\`（sha256 \`${otherHash(meta.elf?.sha256)}\`）`)
  lines.push(`- 浏览器：${meta.browser.name} ${meta.browser.version}（headless=${meta.browser.headless}，${meta.env.platform} ${meta.env.cpuModel}）`)
  lines.push(`- 配置：间隔 ${result.config.intervalMs} ms，测量窗口 ${result.config.durationSec}s，预热 ${result.config.warmupMs}ms，Playwright ${result.config.scriptVersion}`)
  lines.push('')
  lines.push('## 结果总表', '')
  lines.push('| 场景 | 曲线 | 模式 | rAF FPS | 帧 p95/p99 (ms) | long task | 阻塞占比 | draw FPS | avgDraw (ms) | draw CPU 占比 | DOM 节点 | 内存增量 | 设备帧 |')
  lines.push('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
  for (const scenario of result.scenarios) {
    if (scenario.status !== 'ok') {
      lines.push(`| ${scenario.id} | ${scenario.requestedSeries} | ${scenario.requestedMode} | FAILED | | | | | | | | | |`)
      continue
    }
    const w = scenario.window
    lines.push(`| ${scenario.id} | ${scenario.effectiveSeries} | ${scenario.requestedMode} | ${w.raf.fps} | ${w.raf.p95Ms}/${w.raf.p99Ms} | `
      + `${w.longtasks.count} / ${w.longtasks.totalMs}ms | ${w.longtasks.mainThreadBlockedPct}% | ${w.draw.drawFps} | ${w.draw.avgDrawMs} | `
      + `${w.draw.drawCpuSharePct}% | ${w.dom.totalNodes} | ${formatMb(w.memory.deltaUsedBytes)} | ${scenario.device.framesDelta} |`)
  }
  lines.push('')
  const failed = result.scenarios.filter((scenario) => scenario.status !== 'ok')
  if (failed.length) {
    lines.push('### 失败场景', '')
    for (const scenario of failed) lines.push(`- **${scenario.id}**：${scenario.errors.join(' | ')}`)
    lines.push('')
  }
  if (comparison && comparison.length) {
    lines.push('## 与基线对比', '')
    lines.push('| 场景 | FPS (前→后) | long task 总时长 | 阻塞占比 | avgDraw | draw CPU 占比 | 设备帧 |')
    lines.push('|---|---|---|---|---|---|---|')
    for (const row of comparison) {
      lines.push(`| ${row.scenario} | ${fmtDelta(row.fps)} | ${fmtDelta(row.longtaskTotalMs)} | ${fmtDelta(row.longtaskBlockedPct)} | `
        + `${fmtDelta(row.avgDrawMs)} | ${fmtDelta(row.drawCpuPct)} | ${fmtDelta(row.deviceFrames)} |`)
    }
    lines.push('')
  }
  lines.push('## 每场景详情', '')
  for (const scenario of result.scenarios) {
    lines.push(`### ${scenario.id}（${scenario.title}）`, '')
    if (scenario.status !== 'ok') {
      lines.push(`- 状态：**FAILED**`)
      for (const error of scenario.errors) lines.push(`- 错误：\`${String(error).split('\n')[0]}\``)
      lines.push('')
      continue
    }
    const w = scenario.window
    lines.push(`- 曲线：请求 ${scenario.requestedSeries} / 实际 ${scenario.effectiveSeries}；设备 series_count=${scenario.device.seriesCount}，group_count=${scenario.device.groupCount}，halt=${scenario.device.halt}`)
    lines.push(`- 渲染：mode=${scenario.render.renderAll ? 'A' : 'B'}，drawCap=${scenario.render.drawCap}，drawn=${w.draw.drawn}，visible=${w.draw.visible}，selected=${w.draw.selected}`)
    lines.push(`- rAF：${w.raf.fps} FPS（frames=${w.raf.frames}，mean=${w.raf.meanDeltaMs}ms，p50=${w.raf.p50Ms}，p95=${w.raf.p95Ms}，p99=${w.raf.p99Ms}，max=${w.raf.maxMs}，>25ms=${w.raf.droppedOver25ms}，>50ms=${w.raf.droppedOver50ms}）`)
    lines.push(`- long task：${w.longtasks.count} 个 / ${w.longtasks.totalMs}ms（max ${w.longtasks.maxMs}ms，占比 ${w.longtasks.mainThreadBlockedPct}%，${w.longtasks.perSec}/s）`)
    lines.push(`- LoAF：${w.loaf.count} 个 / ${w.loaf.totalMs}ms（blocking ${w.loaf.blockingMs}ms）；invoker=${JSON.stringify(w.loaf.invokers)}`)
    lines.push(`- 绘制：draws=${w.draw.draws}（${w.draw.drawFps} fps），avgDraw=${w.draw.avgDrawMs}ms，lastDraw=${w.draw.lastDrawMs}ms，draw CPU≈${w.draw.drawCpuSharePct}%`)
    lines.push(`- DOM：${w.dom.totalNodes} 节点（Δ${w.dom.nodesDelta}），变量树 ${w.dom.variableRowsRendered}/${w.dom.variableRowsTotal} 行，已选面板 ${w.dom.selectedRowsRendered} 行，legend ${w.dom.legendItemsRendered} 项，mutations=${w.dom.mutations}`)
    lines.push(`- CDP 增量：TaskDuration=${w.cdp.delta.TaskDuration}s，ScriptDuration=${w.cdp.delta.ScriptDuration}s，LayoutDuration=${w.cdp.delta.LayoutDuration}s，RecalcStyleDuration=${w.cdp.delta.RecalcStyleDuration}s，JSHeap ${formatMb(w.cdp.delta.JSHeapUsedSize)}`)
    lines.push(`- 内存（强制 GC 口径）：before=${formatMb(w.memory.before?.used)} / after=${formatMb(w.memory.after?.used)}（Δ${formatMb(w.memory.deltaUsedBytes)}），窗口内采样峰值 ${formatMb(w.memory.peakUsed)}（${w.memory.samples ?? 0} 个样本）`)
    lines.push(`- 轮询：/api/curve/frames ${w.network.framePolls} 次（${w.network.framePollsPerSec}/s，avg ${w.network.framePollAvgMs}ms，max ${w.network.framePollMaxMs}ms）`)
    lines.push(`- 设备：frames Δ${scenario.device.framesDelta}（${scenario.device.framesRate}/s），invalid=${scenario.device.invalid}，out_of_scope=${scenario.device.outOfScope}，overwritten=${scenario.device.overwritten}，ring_frames=${scenario.device.ringFrames}，link=${scenario.device.linkHz}Hz`)
    if (scenario.profile) {
      lines.push(`- 热点（self time top 8，采样窗口 ${w.durationMs}ms，profiler 覆盖 ${scenario.profile.selfPctOfWindow}%）：`)
      lines.push('')
      lines.push('  | 函数 | 位置 | self ms | self % |')
      lines.push('  |---|---|---|---|')
      for (const item of scenario.profile.top.slice(0, 8)) {
        lines.push(`  | \`${item.functionName}\` | ${item.url.replace(result.config.url.split('#')[0], '')}:${item.line + 1} | ${item.selfMs} | ${item.selfPct}% |`)
      }
      lines.push('')
    }
    if (scenario.warnings.length) {
      for (const warning of scenario.warnings) lines.push(`- ⚠️ ${warning}`)
    }
    lines.push('')
  }
  return lines.join('\n')
}

function otherHash(hash) {
  return hash ? String(hash).slice(0, 12) : '?'
}

function formatMb(bytes) {
  if (bytes === null || bytes === undefined) return '-'
  return `${round(bytes / 1048576, 2)} MB`
}

function fmtDelta(entry) {
  if (!entry || entry.delta === null || entry.delta === undefined) return '-'
  const sign = entry.delta > 0 ? '+' : ''
  const pct = entry.deltaPct === null ? '' : ` (${entry.deltaPct > 0 ? '+' : ''}${entry.deltaPct}%)`
  return `${entry.previous} → ${entry.current} (${sign}${entry.delta}${pct})`
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
async function main() {
  const opts = parseArgs(process.argv.slice(2))
  if (opts.help) {
    console.log(HELP)
    return 0
  }
  if (opts.fromJson) {
    const saved = loadPrevious(opts.fromJson)
    if (opts.compare) saved.comparison = compareRuns(saved, loadPrevious(opts.compare))
    if (!opts.md) throw new Error('--from-json 需要同时指定 --md <path>')
    fs.mkdirSync(path.dirname(path.resolve(opts.md)), {recursive: true})
    fs.writeFileSync(path.resolve(opts.md), buildMarkdown(saved, saved.comparison) + '\n')
    perfLog(`已从 ${opts.fromJson} 重新生成 Markdown: ${path.resolve(opts.md)}`)
    return 0
  }
  if (!fs.existsSync(opts.elf)) throw new Error(`ELF 不存在: ${opts.elf}`)
  const base = deviceBase(opts.url)
  perfLog(`DebugCurve 性能基准 v${SCRIPT_VERSION}`)
  perfLog(`页面 ${opts.url}`)
  perfLog(`ELF ${opts.elf} (${(fs.statSync(opts.elf).size / 1048576).toFixed(2)} MB, sha256 ${fileSha256(opts.elf).slice(0, 12)}…)`)

  // 场景列表
  const scenarioDefs = opts.scenarios
    ? opts.scenarios.split(',').map((id) => id.trim()).filter(Boolean).map((id) => {
      const def = SCENARIO_DEFS.find((item) => item.id === id)
      if (!def) throw new Error(`未知场景 id: ${id}（可选: ${SCENARIO_DEFS.map((item) => item.id).join(',')}）`)
      return def
    })
    : SCENARIO_DEFS.filter((def) => def.id !== 'n40-selftest')

  // 设备预检：确认可达 + 运行状态；若已有采样先 stop（本任务独占设备）
  const preflightWarnings = []
  let deviceBeforeStatus
  try {
    deviceBeforeStatus = await getCurveStatus(base)
  } catch (error) {
    throw new Error(`设备不可达 ${base}: ${error.message}`)
  }
  if (deviceBeforeStatus.running === true) {
    preflightWarnings.push('设备上已有本地采样在跑，基准开始前先 stop（可能是残留会话）')
    perfLog('设备已有采样运行，先停止…')
    deviceBeforeStatus = await stopCurveSampling(base, 'bench-preflight')
  }
  const bundle = await fetchBundleInfo(base).catch((error) => ({error: error.message}))
  perfLog(`设备 bundle: ${bundle.bytes} bytes, sha256 ${otherHash(bundle.sha256)}，预检 running=${deviceBeforeStatus.running}`)

  // Playwright
  const {module: playwright, resolvedFrom} = await resolvePlaywright(opts.playwright)
  perfLog(`Playwright 来自 ${resolvedFrom}`)
  const browser = await playwright.chromium.launch({
    headless: !opts.headed,
    args: [
      '--enable-precise-memory-info',
      '--disable-background-timer-throttling',
      '--disable-backgrounding-occluded-windows',
      '--disable-renderer-backgrounding',
      '--force-device-scale-factor=1',
      '--disable-features=CalculateNativeWinOcclusion',
    ],
  })
  const browserVersion = browser.version()
  const context = await browser.newContext({
    viewport: {width: 1600, height: 1000},
    locale: 'zh-CN',
    deviceScaleFactor: 1,
  })
  const page = await context.newPage()
  const pageErrors = []
  page.on('pageerror', (error) => pageErrors.push(String(error).slice(0, 500)))
  const consoleErrors = []
  page.on('console', (message) => {
    if (message.type() === 'error') consoleErrors.push(message.text().slice(0, 300))
  })
  const cdp = await context.newCDPSession(page)
  await cdp.send('Performance.enable')

  const result = {
    schemaVersion: 1,
    scriptVersion: SCRIPT_VERSION,
    label: opts.label,
    createdAt: nowIso(),
    config: {
      url: opts.url,
      elf: opts.elf,
      durationSec: opts.duration,
      warmupMs: opts.warmup,
      intervalMs: opts.interval,
      scenarios: scenarioDefs.map((def) => def.id),
      profile: opts.profile,
      headless: !opts.headed,
      viewport: {width: 1600, height: 1000},
      locale: 'zh-CN',
      scriptVersion: SCRIPT_VERSION,
    },
    meta: {
      device: {
        base,
        statusBefore: summarizeDeviceStatus(deviceBeforeStatus),
        bundle,
        webInfo: null,
      },
      elf: {path: opts.elf, sizeBytes: fs.statSync(opts.elf).size, sha256: fileSha256(opts.elf)},
      browser: {
        name: 'chromium (playwright)',
        version: browserVersion,
        headless: !opts.headed,
        playwrightSource: resolvedFrom,
      },
      env: {
        platform: `${os.platform()} ${os.release()} ${os.arch()}`,
        cpuModel: os.cpus()[0]?.model || 'unknown',
        cpuCount: os.cpus().length,
        totalMemMb: Math.round(os.totalmem() / 1048576),
        node: process.version,
        hostname: os.hostname(),
      },
    },
    preflightWarnings,
    scenarios: [],
    pageErrors,
    consoleErrors,
    exit: {stopVerified: false},
  }

  let hadFailure = false
  let previous = null
  if (opts.compare) {
    previous = loadPrevious(opts.compare)
    perfLog(`对比基线 ${opts.compare}（${previous.createdAt}，label=${previous.label}）`)
  }

  // Ctrl-C / 异常时尽力 stop
  let cleaningUp = false
  const cleanup = async (reason) => {
    if (cleaningUp) return
    cleaningUp = true
    perfLog(`清理（${reason}）：停止设备本地采样…`)
    try {
      const status = await stopCurveSampling(base, `bench-${reason}`)
      perfLog(`清理完成，设备 running=${status.running}`)
      return status
    } catch (error) {
      perfLog(`清理 stop 失败: ${error.message}`)
      return null
    }
  }
  const onSignal = async (signal) => {
    perfLog(`收到 ${signal}，终止并恢复目标…`)
    await cleanup(signal)
    await browser.close().catch(() => {})
    process.exit(130)
  }
  process.once('SIGINT', () => { void onSignal('SIGINT') })
  process.once('SIGTERM', () => { void onSignal('SIGTERM') })

  try {
    // 记录一次页面元信息（webInfo 在 .meta.device）
    await page.goto(opts.url, {waitUntil: 'load', timeout: 60000})
    await pageReady(page)
    result.meta.device.webInfo = await page.evaluate(() => window.__BIRDIEBOT_WEB__ || null)
    result.meta.device.title = await page.title()

    for (const scenario of scenarioDefs) {
      perfLog('='.repeat(78))
      perfLog(`场景 ${scenario.id}: ${scenario.title}`)
      const scenarioResult = await runScenario({page, cdp, context, opts, scenario, base, elfInfo: result.meta.elf, browserInfo: result.meta.browser})
      result.scenarios.push(scenarioResult)
      if (scenarioResult.status !== 'ok') hadFailure = true
      // 场景之间留一点恢复时间（stop 后目标已 resume；避免连续 start 撞 DAP）
      await sleep(500)
    }
  } catch (error) {
    hadFailure = true
    result.fatalError = error.stack || error.message
    perfLog('FATAL:', error.message)
  } finally {
    const finalStatus = await cleanup('finally')
    result.exit.stopVerified = finalStatus ? finalStatus.running !== true : false
    if (!result.exit.stopVerified) hadFailure = true
    result.meta.device.statusAfter = summarizeDeviceStatus(finalStatus || {})
    result.pageErrors = pageErrors
    result.consoleErrors = consoleErrors
    if (!opts.keepOpen) {
      await context.close().catch(() => {})
      await browser.close().catch(() => {})
    }
    result.finishedAt = nowIso()
    result.durationMs = Date.parse(result.finishedAt) - Date.parse(result.createdAt)
    // 汇总
    const okScenarios = result.scenarios.filter((scenario) => scenario.status === 'ok')
    result.summary = {
      total: result.scenarios.length,
      ok: okScenarios.length,
      failed: result.scenarios.length - okScenarios.length,
      stopVerified: result.exit.stopVerified,
      host: result.meta.env.hostname,
      device: base,
      bundleSha256: bundle.sha256 || null,
      elfSha256: result.meta.elf.sha256,
    }
    if (previous) {
      result.comparison = compareRuns(result, previous)
      perfLog('\n与基线对比:')
      for (const row of result.comparison) {
        perfLog(`  ${row.scenario}: FPS ${fmtDelta(row.fps)}；longtask ${fmtDelta(row.longtaskTotalMs)}；avgDraw ${fmtDelta(row.avgDrawMs)}；drawCPU ${fmtDelta(row.drawCpuPct)}`)
      }
    }
    fs.mkdirSync(path.dirname(path.resolve(opts.out)), {recursive: true})
    fs.writeFileSync(path.resolve(opts.out), JSON.stringify(result, null, 2) + '\n')
    perfLog(`结果已写入 ${path.resolve(opts.out)}`)
    if (opts.md) {
      const markdown = buildMarkdown(result, result.comparison)
      fs.mkdirSync(path.dirname(path.resolve(opts.md)), {recursive: true})
      fs.writeFileSync(path.resolve(opts.md), markdown + '\n')
      perfLog(`Markdown 已写入 ${path.resolve(opts.md)}`)
    }
    // 控制台摘要
    perfLog('\n=== 摘要 ===')
    for (const scenario of result.scenarios) {
      if (scenario.status !== 'ok') {
        perfLog(`  ${scenario.id}: FAILED — ${scenario.errors[0]?.split('\n')[0]}`)
        continue
      }
      const w = scenario.window
      perfLog(`  ${scenario.id}: rAF ${w.raf.fps} FPS | longtask ${w.longtasks.count}/${w.longtasks.totalMs}ms | `
        + `draw ${w.draw.drawFps}fps avg ${w.draw.avgDrawMs}ms | DOM ${w.dom.totalNodes} | mem ${formatMb(w.memory.deltaUsedBytes)} | `
        + `frames ${scenario.device.framesDelta} @ ${scenario.device.framesRate}/s`)
    }
    perfLog(`stopVerified=${result.exit.stopVerified}，失败场景=${result.summary.failed}，耗时 ${(result.durationMs / 1000).toFixed(1)}s`)
  }
  return hadFailure ? 1 : 0
}

main()
  .then((code) => { process.exitCode = code })
  .catch((error) => {
    console.error('FATAL:', error.stack || error.message)
    process.exitCode = 2
  })
