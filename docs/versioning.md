# WiFi Dap 版本与协议握手契约（v1）

> 状态：**冻结草案 v1（2026-10-20，Wave 0）**。Wave 1/2 的固件、Web 前端与发布脚本以本文为准；变更需 Lead 统一修改并广播（每波最多 2 次契约变更）。
> 相关契约：`docs/protocol.md`（命令/字段总表）、`docs/ws-binary-router.md`（二进制路由）、`docs/ota-local-upload.md`（OTA 上传流程）、`docs/i18n-keys.md`（版本/握手文案 key）。
> 本文只定义"版本如何产生、如何注入、如何在运行时核对"，不重复定义 JSON/WS 命令字段（以 `protocol.md` 为准）。

---

## 0. 结论速览

| 版本 | 唯一来源 | 注入/暴露方式 | 运行时可见处 | 谁维护 |
|---|---|---|---|---|
| **固件版本（semver）** | 本地 git tag `vMAJOR.MINOR.PATCH` | CMake 计算 `PROJECT_VER` → `esp_app_desc.version` | 启动日志、`SYSTEM/GET_FM_INFO.fm_ver`、`OTA/GET_INFO.running_ver`、About 页 | WP-007（根 CMake）、WP-125（wt_system） |
| **协议版本（整数）** | `project_components/wt_common/wt_protocol_version.h` 的 `BIRDIEBOT_PROTOCOL_VERSION` | 编译期常量；启动日志打印；`GET_FM_INFO.protocol_version` 返回 | 启动日志、WS 握手、About 页 | WP-125（字段）、WP-131（前端常量与比较）、Lead（升版批准） |
| **Web UI 版本** | `web/package.json` 的 `version` | `web/scripts/gen-env.mjs` 注入 `VITE_APP_WEB_VERSION` 等 env | About 页、`window.__BIRDIEBOT_WEB__`、`web_version.txt` | WP-131（构建）、WP-143（发布脚本） |

**一句话规则**：一次发布 = 一个固件 semver（tag） + 一个协议整数 + 一个与之同步的 Web UI semver；三者只在**发布检查脚本**里互相校验，运行时不写回、不自动升级。

---

## 1. 设计原则与背景

1. **消灭"三套互不校验"**：D 仓库当前存在 `VITE_APP_GIT_TAG`、`esp_app_desc.version`、手工 `html/version.txt` 三套互不相干的版本（详见 `analysis/REPORT-2-esp32-tools.md` §1.4）。本契约固定为：**发布版本 = 固件 tag = Web UI 版本**，`git describe` 只作为构建溯源（provenance），不再作为第二个版本号展示。
2. **构建可复现**：同一 commit + 同一发布 tag 必须得到相同版本字符串；版本计算不得依赖网络、不得依赖完整 git 历史（浅克隆/无 `.git` 构建有确定回退）。
3. **失败可见**：版本解析失败不能产出静默空字符串；必须回退到已知版本并在构建日志中 WARNING，release 构建遇到 `-dirty` 直接失败。
4. **握手不阻塞业务**：协议版本不匹配时用 UI 提示 + 禁用危险操作（OTA 上传），Wi-Fi/UART/Widget 等基础功能仍可用（与 `protocol.md:141` 一致）。

---

## 2. 固件版本（semver，从 v1.0.0 起）

### 2.1 版本号规则

- 本地 **annotated tag**：`vMAJOR.MINOR.PATCH`，从 `v1.0.0` 开始；本项目约定**只做本地 tag，不 push**（INTEGRATION-PLAN §8.4）。
- 语义（本项目粒度）：
  - `MAJOR`：破坏性变更（分区布局/配置格式不兼容、需要重新烧录、协议版本升版）。
  - `MINOR`：向后兼容的新功能（新页面、新 JSON cmd、新 Kconfig 开关）。
  - `PATCH`：缺陷修复、文案/UI 修正、不改变接口的改动。
- 发布 tag 必须是**干净工作区**：`git status --porcelain` 为空；`git describe` 结果不得含 `-dirty`。
- 开发中间版本由 `git describe` 自动产生，格式与含义：

| `git describe --tags --dirty --always --match 'v[0-9]*'` 输出 | 含义 | 允许出现在 release 产物 |
|---|---|---|
| `v1.0.0` | 正好在 release tag 上 | ✅ |
| `v1.0.0-3-gabc1234` | tag 之后 3 个提交，commit `abc1234` | ❌（仅开发构建） |
| `v1.0.0-3-gabc1234-dirty` | 有未提交改动 | ❌ |
| `abc1234`（无匹配 tag） | 回退到 commit 短哈希 | ❌ |
| 失败/无 git | 回退（见 §2.2.3） | 仅允许 `v1.0.0` 回退 |

### 2.2 注入机制（CMake / ESP-IDF 5.2.1）

现状问题：集成分支 `CMakeLists.txt` 里的 `write_git_version_to_file()`（feature/birdiebot-v1）只执行 `git describe --tags`，失败时把**空字符串**写入 `version.txt`，且用正则剥掉 `-N-gsha` 后缀；在浅克隆/无 git 环境下会得到空 `PROJECT_VER`。**必须替换**为下述实现（WP-007 负责根 CMake；WP-143 负责发布脚本）。

#### 2.2.1 计算顺序（唯一权威）

新增 `cmake/version.cmake`，在 `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` **之前**被根 `CMakeLists.txt` include：

1. CMake cache 显式覆盖：`-DBIRDIEBOT_FW_VERSION=...`（CI/发布脚本用；最高优先级）。
2. `git describe --tags --dirty --always --match 'v[0-9]*'`（工作目录 = 源码根；成功即用）。
3. 源码根 `version.txt`（若存在且内容匹配版本正则；用于源码包/无 git 构建）。
4. 回退常量 `v1.0.0`（`BIRDIEBOT_FW_VERSION_FALLBACK` cache 变量，默认 `v1.0.0`），并 `message(WARNING "... using fallback ...")`。
5. 校验最终字符串匹配 `^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?(-dirty)?$`；不匹配直接 `message(FATAL_ERROR)`。

#### 2.2.2 参考实现

```cmake
# cmake/version.cmake（新增，WP-007；Lead 审阅）
set(BIRDIEBOT_FW_VERSION "" CACHE STRING "Override firmware version (e.g. v1.0.0)")
set(BIRDIEBOT_FW_VERSION_FALLBACK "v1.0.0" CACHE STRING "Fallback when git metadata is unavailable")

set(_ver "")
if(BIRDIEBOT_FW_VERSION)
  set(_ver "${BIRDIEBOT_FW_VERSION}")
  message(STATUS "Version: from BIRDIEBOT_FW_VERSION=${_ver}")
else()
  execute_process(
    COMMAND git describe --tags --dirty --always --match "v[0-9]*"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _git_out ERROR_QUIET RESULT_VARIABLE _git_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_git_rc EQUAL 0 AND _git_out MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+.*")
    set(_ver "${_git_out}")
    message(STATUS "Version: from git describe=${_ver}")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/version.txt")
    file(READ "${CMAKE_SOURCE_DIR}/version.txt" _ver)
    string(STRIP "${_ver}" _ver)
    message(WARNING "Version: git unavailable, using version.txt=${_ver}")
  else()
    set(_ver "${BIRDIEBOT_FW_VERSION_FALLBACK}")
    message(WARNING "Version: git metadata unavailable, using fallback=${_ver}")
  endif()
endif()

if(NOT _ver MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?(-dirty)?$")
  message(FATAL_ERROR "Invalid firmware version string: '${_ver}'")
endif()

set(PROJECT_VER "${_ver}" CACHE STRING "" FORCE)
set(BIRDIEBOT_FW_VERSION_RESOLVED "${_ver}" CACHE INTERNAL "")
```

根 `CMakeLists.txt` 中的调用位置（替换现有 `write_git_version_to_file`；示意如下，保留现有 `add_compile_options` 等无关配置）：

```cmake
cmake_minimum_required(VERSION 3.5)
include(cmake/version.cmake)          # ← 计算 PROJECT_VER
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
set(EXTRA_COMPONENT_DIRS project_components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(birdiebot_wifi_dap)           # ESP-IDF 会把 PROJECT_VER 写入 esp_app_desc.version
```

> ESP-IDF 5.2 的项目版本来源顺序为：CMake 变量 `PROJECT_VER` → 项目根 `version.txt` → 项目目录 `git describe` → `CONFIG_APP_PROJECT_VER_FROM_CONFIG`。本契约显式在 `project()` 前设置 `PROJECT_VER`，从而**不依赖** IDF 自己的 git 逻辑，浅克隆行为可控。

#### 2.2.3 浅克隆/无 git 回退矩阵

| 构建环境 | 期望行为 | 版本字符串 |
|---|---|---|
| 完整 clone + 有 tag（发布机） | `git describe` 成功 | `v1.0.0` / `v1.0.0-3-g…` |
| `--depth 1` 浅克隆且**未 fetch tag** | `git describe` 失败 → 读 `version.txt`（若发布归档带）→ 否则回退 | `version.txt` 内容 / `v1.0.0`（`WARNING`） |
| 源码 zip（无 `.git`） | 同上 | `version.txt` / `v1.0.0`（`WARNING`） |
| 有未提交改动 | `git describe` 带 `-dirty` | 开发构建允许；release 检查拒绝 |
| CI 显式指定 | 最高优先级 | `-DBIRDIEBOT_FW_VERSION=v1.0.0` |

**发布要求**：release 构建（`scripts/release_local.sh`，WP-143）必须先完整 clone 或显式传 `-DBIRDIEBOT_FW_VERSION`，并在构建日志中 grep 到 `Version: from git describe=`；禁止在 release 产物中出现 `-dirty` 或回退版本（发布脚本 `git describe --dirty` 校验失败即中止）。

### 2.3 构建日期与其他注入字段

`SYSTEM/GET_SYS_INFO`（`protocol.md:48`）需要 `hw_ver` / `mf_date` 两个"构建注入、可留空"的字段；`GET_FM_INFO.upd_date` 仍为构建日期。

- `upd_date`：保留 `project_components/wt_system/CMakeLists.txt` 现有 `FW_UPD_DATE` 注入（`git show -s --format=%cd --date=format:%Y-%m-%d`），但**增加失败回退**：git 失败时用 `date -u +%Y-%m-%d`，不得注入空串。
- `hw_ver`：`HW_VER` CMake cache 变量（默认 `""`，board 头可给默认值），`target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_HW_VER="${HW_VER}")`。
- `mf_date`：`MF_DATE` cache 变量（默认 `""`），同法注入 `FW_MF_DATE`。
- 以上三个值只在 `wt_system` 内部使用；**不要**把版本/日期写回 `version.txt` 后再读（避免二次源）。

### 2.4 运行时暴露

```c
/* project_components/wt_system/wt_system.c（WP-125） */
const esp_app_desc_t *desc = esp_app_get_description();
strncpy(fm_info->fm_ver, desc->version, sizeof(fm_info->fm_ver) - 1);  /* = PROJECT_VER */
```

- 固件版本唯一读取 API：`esp_app_get_description()->version`（勿自行维护第二份版本变量）。
- **启动日志（读取顺序第一条）**：`app_main()` 一开始、`wifi_manager_init()` 之前打印（避免被 WiFi 阻塞/日志刷掉）：

```text
I (312) BIRDIEBOT: WiFi Dap fw=v1.0.0 proto=1 board=BOARD_ZXR_DAP_S3 idf=5.2.1
```

  字段来源：`fw`=`esp_app_get_description()->version`、`proto`=`BIRDIEBOT_PROTOCOL_VERSION`、`board`=`CONFIG_BIRDIEBOT_BOARD_*` 字符串化、`idf`=`esp_get_idf_version()`。**该行不得包含任何 WiFi/AP 口令**；AP 信息单独一行（只允许 SSID + IP）。
- 固件版本同样经 `GET_FM_INFO`（`fm_ver`）返回给 Web UI；`fm_ver` 与 `GET_SYS_INFO`/About 页面显示必须同源。

### 2.5 固件版本验收

```bash
# 1) CMake 解析结果（配置阶段日志）
idf.py reconfigure 2>&1 | grep -E "Version: (from|git unavailable|git metadata unavailable)"
# 2) 产物内嵌版本（app_desc 位于镜像 0x20，version 位于 0x30；见 §6.4）
python3 - <<'PY'
import sys,struct
d=open('build/birdiebot_wifi_dap.bin','rb').read()
assert struct.unpack_from('<I',d,0x20)[0]==0xABCD5432, 'bad app_desc magic'
print(struct.unpack_from('<32s',d,0x30)[0].split(b'\0')[0].decode())
PY
# 3) 运行时：WS 发 SYSTEM/GET_FM_INFO，fm_ver 必须等于上面两项，且含 protocol_version
```

---

## 3. 协议版本 `protocol_version`（当前 = 1）

### 3.1 唯一来源

新增头文件 `project_components/wt_common/wt_protocol_version.h`（WP-125；`wt_common` 已是组件且 `INCLUDE_DIRS "."`）。引用方（`wt_system`、`wt_ota`、`main`）需要在各自 `idf_component_register` 的 `REQUIRES/PRIV_REQUIRES` 中声明 `wt_common`（`main` 是 IDF 特殊组件，自动依赖全部组件，可省略）：

```c
#ifndef WT_PROTOCOL_VERSION_H_GUARD
#define WT_PROTOCOL_VERSION_H_GUARD

/* WiFi Dap JSON/WS 协议版本。升版规则见 docs/versioning.md §3.3。 */
#define BIRDIEBOT_PROTOCOL_VERSION 1

#endif /* WT_PROTOCOL_VERSION_H_GUARD */
```

前端常量（唯一来源，不得散落魔法数字）：

```ts
/* web/src/api/protocol.ts（WP-131 新增） */
export const PROTOCOL_VERSION = 1
```

一致性由构建脚本校验（`scripts/check-protocol-version.sh`，§8.2）：从 C 头文件与 TS 文件各 grep 一个整数，不相等则构建失败。**禁止**在前端从固件响应动态"学习"协议版本后再当常量用（只做比较，不做降级协商）。

### 3.2 版本历史

| protocol_version | 日期 | 变更摘要 | 固件 tag | Web UI |
|---|---|---|---|---|
| 1 | 2026-10-20 | 初版冻结：SYSTEM(0)/WIFI(1)/DATA_FLOW(2)/UART(4)/OTA(5) 命令表、4B 二进制头、隐式 ACK、无请求 ID | v1.0.0 | 1.0.x |

### 3.3 升版规则

**必须 +1**（破坏性，旧前端不能正确工作）：
- 删除/重命名 JSON 请求或响应字段，或改变字段类型/单位；
- 改变既有 `cmd` 的语义、错误码含义；
- 改变 4 字节二进制头布局或 `data_type` 语义；
- 改变 WS 鉴权握手方式（首帧/query token）导致旧前端无法连接；
- 改变隐式 ACK/异步响应约定（例如引入请求 ID 且旧前端会错配）。

**不升版**（向后兼容，旧前端忽略即可）：新增 `cmd`、新增可选响应字段、新增模块（旧前端不注册即忽略）、新增错误码（旧前端按"失败"处理）、心跳周期微调（保持在 1.5s±0.5s 内）。

流程：改固件头 + 前端常量 + 本文 §3.2 表格 + `protocol.md` 相应章节，**同一合并窗口**由 Lead 批准；只改一端视为未完成。

### 3.4 启动打印与运行时读取

- 启动日志（§2.4）已包含 `proto=1`。
- `SYSTEM/GET_FM_INFO`（`protocol.md:46`）响应必须始终带 `protocol_version`（整型，非字符串）；旧固件无此字段时前端按 `0` 处理（§5.4）。
- 其他模块**不要**各自上报协议版本；唯一入口是 `GET_FM_INFO`（OTA 的 `GET_INFO` 只返回运行分区/版本，不重复协议版本）。

---

## 4. Web UI 版本与构建注入

### 4.1 版本来源

- **唯一来源**：`web/package.json` 的 `version`，semver 字符串（不带 `v` 前缀），从 `1.0.0` 起。
- 发布同步规则（release checklist 强制）：固件 tag `vX.Y.Z` ⇔ `web/package.json.version == "X.Y.Z"`；不一致时发布脚本中止（见 §8.3）。
- 开发期允许 `x.y.z-dev` 或领先的 patch，但合并到发布 tag 前必须对齐。

### 4.2 构建注入（扩展 D 的 `web/scripts/gen-env.mjs`）

D 现有脚本只注入 `VITE_APP_GIT_TAG` / `VITE_APP_LAST_COMMIT`，且 `git describe` 失败会让 `npm run build` 直接崩溃（D 的 `.env.local` 又不可复现）。契约要求改为：

| env 变量 | 值来源 | 回退 |
|---|---|---|
| `VITE_APP_WEB_VERSION` | `web/package.json` 的 `version` | 缺失则构建失败 |
| `VITE_APP_GIT_TAG` | 仓库根 `git describe --tags --dirty --always --match 'v[0-9]*'`（在 web/ 子目录执行会命中同一 .git） | `v1.0.0-unknown`（WARNING） |
| `VITE_APP_LAST_COMMIT` | `git log -1 --format=%cd` | `unknown` |
| `VITE_APP_PROTOCOL_VERSION` | 由 `check-protocol-version.sh` 校验后的常量（可选注入，供显示） | 构建失败 |
| `VITE_APP_MODE` | 沿用 D：`.env.development`=`dev` / `.env.production`=`prod` | — |
| `VITE_ENABLE_OTA` | 本项目固定 `true`（OTA 已纳入 Wave 2，见 INTEGRATION-PLAN §8.1） | — |

实现要求：
1. 所有 `execSync('git describe')` 包裹 try/catch，失败回退不抛异常。
2. 读取 `package.json` 的 version 并校验 `^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$`。
3. 构建结束后把构建信息写到 `web/dist/build-info.json`（供 About 页/调试读取，可选）。
4. 在 `main.ts` 暴露只读全局对象，便于问题定位与自动化验收：

```ts
/* web/src/main.ts（WP-131） */
window.__BIRDIEBOT_WEB__ = {
  webVersion: import.meta.env.VITE_APP_WEB_VERSION,
  gitTag: import.meta.env.VITE_APP_GIT_TAG,
  buildTime: import.meta.env.VITE_APP_LAST_COMMIT,
  protocolVersion: PROTOCOL_VERSION,
}
```

`web/src/env.d.ts` 增加 `vite/client` + `window.__BIRDIEBOT_WEB__` 的 `ImportMetaEnv` 类型声明（`VITE_APP_WEB_VERSION: string` 等），避免 TS 报错。

### 4.3 展示位置

| 位置 | 展示内容 | 数据来源 |
|---|---|---|
| About 页"网页版上位机" | 网页版本 + 构建 tag/时间 | `VITE_APP_WEB_VERSION` / `VITE_APP_GIT_TAG` / `VITE_APP_LAST_COMMIT` |
| About 页"调试器" | 固件版本 + 固件日期 + 协议版本 + SN + hw/mf | `GET_FM_INFO` / `GET_SYS_INFO` |
| About 页底部 | 绿色对勾/红色叹号：协议版本是否匹配 | §5 |
| Update 页 | 当前运行分区 + 运行版本 | `OTA/GET_INFO.running` / `.running_ver` |
| 浏览器 console | 启动时打印 `__BIRDIEBOT_WEB__` 一次（dev 模式） | 同上 |

文案 key 见 `docs/i18n-keys.md`（`protocol.*`、`common.*`、`ota.upload.*`），**禁止**在页面硬编码"版本/协议"中文。

### 4.4 产物元数据 `project_components/html/web_version.txt`

`web/build.sh`（WP-131 新增，`scripts/gen_web_assets.sh` 调用）在复制 gz 后写出（纯文本 key=value，一行一项，供发布审计/离线编译核对，不参与嵌入）：

```text
web_version=1.0.0
protocol_version=1
git_tag=v1.0.0
git_commit=abc1234
build_time=2026-10-20T14:30:00Z
index_gz_sha256=<sha256 of index.html.gz>
worker_gz_sha256=<sha256 of ws.sharedworker.js.gz>
```

- 该文件与两个 `.gz` 一起归档；release tag 时 gz 入库（INTEGRATION-PLAN §5.4），`web_version.txt` 必须同步提交。
- 开发分支不提交 gz / web_version.txt（`.gitignore` 已有 `web/dist/`；`project_components/html/*.gz` 按发布策略处理）。

---

## 5. WS 连接后的版本握手

### 5.1 时序

```text
浏览器                                  固件
  │  WS open → ws://<ip>/ws               │
  │◄──────────── ControlEvent.CONNECTED ──│（前端 websocketWrapper 触发）
  │  {"module":0,"cmd":1}  (GET_FM_INFO)  │
  │──────────────────────────────────────►│
  │◄─ {"module":0,"cmd":1,"fm_ver":"v1.0.0","upd_date":"2026-10-20","protocol_version":1}
  │  （可选）{"module":0,"cmd":3} GET_SYS_INFO → sn/hw_ver/mf_date
  │  比较 protocol_version ↔ PROTOCOL_VERSION（前端常量）
  │  match → About 绿标；mismatch/missing → 提示 + 禁用 OTA 上传
```

- 触发点：沿用 D 的 `useSystemModule`（`src/composables/useSystemModule.ts`）：`ControlEvent.CONNECTED` 时发 `GET_FM_INFO` + `GET_SYS_INFO`；不新增握手专用命令。
- 每次（重）连接都要重新握手；前端刷新/worker 重连后同样执行。
- 超时：发出 `GET_FM_INFO` 后 **5 s** 内无响应 → 视为"无法确认协议版本"（§5.4 第 4 行），只影响提示与 OTA 门禁，不断开 WS、不阻塞其他功能。
- 重试：自动重试 1 次（间隔 2 s），仍失败则放弃，等待下次 `CONNECTED`。

### 5.2 字段与示例

请求：`{"module":0,"cmd":1}`

成功响应（`protocol.md:46`，字段顺序无关）：

```json
{"module":0,"cmd":1,"fm_ver":"v1.0.0","upd_date":"2026-10-20","protocol_version":1}
```

- `protocol_version`：JSON number（非字符串），缺失 = 旧固件（前端按 0 处理）。
- `fm_ver`：`esp_app_desc.version` 原文，可含 `v` 前缀与 `-N-gsha`（前端只显示，不解析）。
- 错误响应：按 `protocol.md §1.1` 的错误格式；前端把 `GET_FM_INFO` 失败记为"未知版本"。

### 5.3 前端判定逻辑（参考实现）

```ts
/* web/src/composables/useSystemModule.ts 或 stores/useSystemStore.ts（WP-131） */
export type ProtocolState = 'ok' | 'mismatch' | 'legacy' | 'unknown'

function compareProtocol(fw: unknown): ProtocolState {
  if (typeof fw !== 'number') return 'legacy'            // 旧固件无字段
  if (fw !== PROTOCOL_VERSION) return 'mismatch'
  return 'ok'
}
```

- 状态存 Pinia（例如 `systemStore.protocolState` 与 `systemStore.fwProtocolVersion`），About 页与 Update 页共享。
- 文案：`protocol.mismatchTitle` / `protocol.mismatchDetail`（带 `{web}`/`{fw}` 参数）/ `protocol.mismatchLegacy` / `protocol.mismatchOtaDisabled` / `protocol.refreshHint`（见 i18n 契约）。
- 只提示一次（每个连接周期），避免心跳/重连刷屏；用户手动重连后允许再次提示。

### 5.4 不匹配行为矩阵

| 状态 | 条件 | UI 行为 | 业务影响 |
|---|---|---|---|
| `ok` | `protocol_version === PROTOCOL_VERSION` | About 显示"协议 v1 ✓" | 全部功能可用 |
| `mismatch` | 数字但不等 | `ElMessage.warning` 一次性提示 + About 红色标记 + 详情 | 基础功能可用；**OTA 上传按钮禁用**并显示 `protocol.mismatchOtaDisabled` |
| `legacy` | 响应无 `protocol_version` | 提示"固件未上报协议版本，可能是旧固件"（`protocol.mismatchLegacy`） | 同上（OTA 禁用，避免旧固件/新前端上传协议不一致） |
| `unknown` | 5 s 超时/请求失败 | 提示"无法确认固件协议版本"（可复用 `protocol.mismatchTitle` + 详情） | 基础功能可用；OTA 禁用 |
| 重连后 | 任意 | 重新握手并按上表更新 | 同上 |

**不阻塞**是硬性要求：禁止用弹窗/遮罩阻塞页面（`protocol.md:141`）。**OTA 门禁**是本契约唯一强约束，由 WP-207 在 Update 页实现。

### 5.5 与 OTA 页面的联动

- 版本不匹配时：Update 页仍可查看"当前运行版本/分区"（只读），但**上传/更新按钮 disabled**，并展示 `protocol.mismatchOtaDisabled`。
- 协议匹配后：按 `docs/ota-local-upload.md` 执行上传；上传结束后设备重启，页面重连 → 重新握手 → `GET_FM_INFO.fm_ver` 应为新版本（验证更新成功）。
- 前端判断"更新成功"的唯一依据 = 重启重连后 `fm_ver`/`running_ver` 变化 + `OTA/GET_INFO.status === "IDLE"`，**不能**仅凭上传进度 100%。

---

## 6. OTA 版本与运行分区（模块 5）

> 字段/命令总表以 `protocol.md §2.5` 为准；上传分片/校验/回执细节以 `docs/ota-local-upload.md` 为准。本节只固定"版本/分区信息"的语义与来源。

### 6.1 `OTA/GET_INFO`（cmd 1）字段语义

| 字段 | 类型 | 语义 | 固件来源 |
|---|---|---|---|
| `running` | string | 当前运行分区标签：`ota_0` / `ota_1`（本期无 factory） | `esp_ota_get_running_partition()->label` |
| `running_ver` | string | 当前运行分区固件版本（应等于 `GET_FM_INFO.fm_ver`） | `esp_app_get_description()->version` |
| `free` | number | 目标（空闲）OTA 槽可写字节数 | `esp_ota_get_next_update_partition(NULL)->size`（减去保留用量时须注明；默认 = 分区 size） |
| `slot_size` | number | 目标 OTA 槽总大小（用于前端上传前校验文件大小） | 同上 `partition->size` |
| `status` | string | 状态机当前状态（见 §6.3） | OTA 组件内存状态 |

> ❗ 与 D 旧前端字段不同：旧 `apiOTA.ts` 用 `fm_size/fm_ver/upd_date/upd_note`（URL 更新方案），新前端 WP-207 必须改用上表字段；旧字段不再返回。

### 6.2 `OTA/GET_PROGRESS`（cmd 3）

```json
{"module":5,"cmd":3,"offset":524288,"total":1572864,"status":"RECEIVING","err":""}
```

- `offset`：已接收/已写入字节；`total`：本次上传总字节（浏览器声明的 Content-Length 或文件大小）。
- `status`：`IDLE | RECEIVING | VERIFYING | READY | APPLYING | ERROR`（与 `protocol.md:110` 一致）。
- `err`：仅 `ERROR` 时非空（英文错误码/描述，前端展示原始信息 + 本地化 `common.failed`）。

### 6.3 状态机与版本变化

```text
IDLE ──开始上传──► RECEIVING ──收完──► VERIFYING ──通过──► READY
  ▲                    │                   │                │
  │                    └──失败/ABORT──► ERROR ◄──失败────────┘
  │                                       │
  └────────────── ABORT / 错误恢复 ───────┘
READY ──set_boot_partition + 重启──► APPLYING ──重启后──► IDLE（running 已切换）
```

- 重启后 `GET_INFO.running/running_ver` 反映新分区；若新固件首启失败且 rollback 开启（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`，INTEGRATION-PLAN §8.1），bootloader 回滚 → `running_ver` 回到旧版本。前端必须展示回滚后的真实 `running_ver`（不得显示"更新成功"缓存）。
- 固件侧必须在首启成功后调用 `esp_ota_mark_app_valid_cancel_rollback()`；否则下次重启回滚（实现属 WP-205，本文只固定对外语义）。
- 同一版本重刷：允许（恢复手段）；版本比较只用于 UI 提示，固件不拒绝。
- 降级：允许（本期无 anti-rollback），UI 用 `ota.upload.downgradeWarn` 二次确认。

### 6.4 版本展示与（可选）浏览器端镜像解析

- 上传前展示：文件名、大小、浏览器计算的 SHA-256（WebCrypto `crypto.subtle.digest`）、以及可选的镜像版本/编译日期。
- 可选增强（不在 v1 必须项，WP-207 视时间决定）：用 `File.slice(0, 0x100).arrayBuffer()` 解析 ESP 应用镜像头：

| 偏移 | 字段 | 说明 |
|---|---|---|
| `0x00` | `magic` = `0xE9` | 镜像 magic |
| `0x20` | `esp_app_desc.magic_word` = `0xABCD5432` | 小端 u32 |
| `0x30` | `version[32]` | 固件版本字符串（NUL 结尾） |
| `0x50` | `project_name[32]` | 项目名（应含 `birdiebot`） |
| `0x70` | `time[16]` / `0x80` `date[16]` | 编译时间/日期 |

  解析失败时退回"只显示文件名/大小"，不得阻止上传；**服务端校验仍以 OTA 组件为准**（前端解析仅供展示）。
- `running_ver` 与 `GET_FM_INFO.fm_ver` 不一致时（例如 OTA 后缓存/重连时序异常），以 `GET_FM_INFO` 为准并打印 warning，About 页显示同一值。

---

## 7. 兼容性矩阵

| 固件 | protocol_version | Web UI | 结果 |
|---|---|---|---|
| v1.0.x | 1 | 1.0.x | ✅ 完全兼容（本期唯一验收组合） |
| v1.x | 1 | 1.0.y（y≠x） | ✅ 兼容（协议未变） |
| v1.x | 1 | 1.y（y≥1） | ✅ 兼容（协议未变），提示"版本建议同步"非必须 |
| v2.0 | 2 | 1.0.x | ⚠️ 提示协议不匹配；基础功能尽力降级（未定义时按 §5.4 处理），OTA 禁用 |
| v1.0.x | 1 | 2.0.x（协议 2） | ⚠️ 同上反向 |
| 无协议字段（旧固件） | 0/缺失 | 1.0.x | ⚠️ `legacy` 提示，OTA 禁用 |

> 本期只承诺第一行；其余行为按 §5.4 保证"不崩溃、可提示、OTA 门禁"。

---

## 8. 检查脚本与构建接入

### 8.1 `scripts/check-version-string.sh`（WP-143）

```bash
#!/usr/bin/env bash
set -euo pipefail
ver="${1:-}"
[[ "$ver" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?$ ]] || { echo "bad version: $ver"; exit 1; }
[[ "$ver" != *-dirty ]] || { echo "release version must not be dirty: $ver"; exit 1; }
echo "version ok: $ver"
```

### 8.2 `scripts/check-protocol-version.sh`（WP-131 新增，构建前置）

```bash
#!/usr/bin/env bash
set -euo pipefail
hdr=$(grep -oE 'BIRDIEBOT_PROTOCOL_VERSION[[:space:]]+[0-9]+' \
        project_components/wt_common/wt_protocol_version.h | grep -oE '[0-9]+$')
ts=$(grep -oE 'PROTOCOL_VERSION[[:space:]]*=[[:space:]]*[0-9]+' \
        web/src/api/protocol.ts | grep -oE '[0-9]+$')
[[ -n "$hdr" && "$hdr" == "$ts" ]] || { echo "protocol version mismatch: C=$hdr TS=$ts"; exit 1; }
echo "protocol version ok: $hdr"
```

调用点：`scripts/gen_web_assets.sh`（WP-143）在 `web/build.sh` 之前执行；CI/本地构建同一脚本。**任何一端升版但另一端未改 → 构建失败**。

### 8.3 `scripts/check-release-versions.sh`（WP-143，release 前）

```bash
fw_tag=$(git describe --tags --exact-match --match 'v[0-9]*')   # 必须正好在 tag 上
web_ver=$(node -p "require('./web/package.json').version")
[[ "$fw_tag" == "v$web_ver" ]] || { echo "fw tag $fw_tag != web version $web_ver"; exit 1; }
bash scripts/check-protocol-version.sh
echo "release versions ok: $fw_tag / protocol 1 / web $web_ver"
```

### 8.4 运行时验收（WS 冒烟）

```js
// node scripts/ws-version-smoke.mjs <ip>   （Node 22+ 自带 WebSocket）
const ws = new WebSocket(`ws://${process.argv[2]}/ws`)
ws.onopen = () => ws.send(JSON.stringify({ module: 0, cmd: 1 }))
ws.onmessage = (e) => {
  const m = JSON.parse(e.data)
  if (m.module === 0 && m.cmd === 1) {
    console.log(m.fm_ver, m.protocol_version)
    process.exit(typeof m.protocol_version === 'number' && m.protocol_version === 1 ? 0 : 1)
  }
}
```

（构建完成后另有 `OTA/GET_INFO` 冒烟：断言返回 `running` 为 `ota_0`/`ota_1`、`running_ver === fm_ver`。）

---

## 9. 发布检查清单（release checklist，WP-143 固化）

- [ ] `git status --porcelain` 为空；当前 commit 正好在 annotated tag `vX.Y.Z` 上（`git describe --exact-match`）。
- [ ] `web/package.json.version === "X.Y.Z"`；`bash scripts/check-release-versions.sh` 通过。
- [ ] `bash scripts/check-protocol-version.sh` 通过（C 头 == TS 常量 == 文档 §3.2）。
- [ ] 固件构建日志出现 `Version: from git describe=vX.Y.Z`；无 `-dirty`、无回退 WARNING。
- [ ] `build/birdiebot_wifi_dap.bin` 的 `esp_app_desc.version` == tag（§2.5 第 2 条）。
- [ ] 启动串口第一条日志符合 §2.4 格式，`proto=1`。
- [ ] 烧录后 WS 冒烟：`fm_ver` == tag、`protocol_version` == 1（§8.4）。
- [ ] `project_components/html/index.html.gz` / `ws.sharedworker.js.gz` / `web_version.txt` 三者 hash/版本一致（§4.4）。
- [ ] About 页显示：网页版本 == 固件版本 == tag；协议 v1 ✓；OTA 页 `running`/`running_ver` 正确。
- [ ] release tag 提交 gz + web_version.txt（开发分支不提交），并记录 tag 与三个版本到发布说明。

---

## 10. 职责与变更流程

| 项 | 文件/位置 | 负责 WP | 验收 |
|---|---|---|---|
| 固件版本 CMake 注入 | `cmake/version.cmake`、根 `CMakeLists.txt` | WP-007（Lead 审阅） | §2.5 |
| `FW_UPD_DATE` / `HW_VER` / `MF_DATE` 注入 | `project_components/wt_system/CMakeLists.txt` | WP-125 | `GET_FM_INFO.upd_date`、`GET_SYS_INFO` 字段非空/可空 |
| `protocol_version` 头文件 + 字段 + 启动打印 | `project_components/wt_common/wt_protocol_version.h`、`wt_system_json_utils.c`、`main.c` | WP-125 / WP-007 | §3.4、§5.2 |
| 前端常量 + 握手比较 + About 展示 | `web/src/api/protocol.ts`、`web/src/stores/useSystemStore.ts`、`web/src/views/About.vue`、`web/src/main.ts` | WP-131 | §5.3/§5.4 |
| OTA 运行分区/版本 | `components/wt_ota/**`、`project_components/wt_system`（或 OTA 模块注册） | WP-205/WP-206 | §6.1、§6.3 |
| Update 页版本展示 + OTA 门禁 | `web/src/views/Update.vue`、`web/src/api/apiOTA.ts`、`web/src/stores/useUpdateStore.ts` | WP-207 | §5.5、§6.4 |
| Web 构建注入 + `web_version.txt` | `web/scripts/gen-env.mjs`、`web/build.sh`、`scripts/gen_web_assets.sh` | WP-131/WP-143 | §4.2、§4.4 |
| release 版本检查 | `scripts/check-release-versions.sh`、`scripts/release_local.sh` | WP-143 | §8.3、§9 |

**变更流程**：任何字段/格式变更 → 提 Lead → Lead 改本文 + `protocol.md` + 广播 → 相关 WP 在下一合并窗口同步。冻结后每波最多 2 次变更；只改一端视为未完成。

---

## 11. 与其他契约的关系 / 待 Lead 确认项

1. **`docs/protocol.md` 字段为准**：本文 §5.2/§6.1 的字段名与 `protocol.md:46,109,110` 完全一致；若两者冲突，以 `protocol.md` 为准并回头修本文（Lead 裁决）。
2. **OTA 上传细节**不在本文：分片大小、HTTP 端点、校验/回执、鉴权头以 `docs/ota-local-upload.md` 为准；本文固定"版本/分区信息"与"更新成功判定"。
3. **`protocol.md:17` 鉴权首帧**写作 `{"module":0,"cmd":0,"token":"..."}`，但 SYSTEM 命令表未定义 `cmd 0`。建议 Lead 二选一：(a) `protocol.md` 增加 SYSTEM `cmd 0 = AUTH`（返回 `protocol_version`），(b) 只保留 query `?token=` / HTTP `Authorization` 方式。本文的握手时序对两种方式都兼容（握手在连接建立后由 WS 消息完成）。
4. **INTEGRATION-PLAN 冲突**：`§0.2/§4` 旧文写"本期不做 OTA"，`§8.1` 改为"OTA 固件侧实现"；本文按 **§8.1** 与 `protocol.md §2.5` 执行。`§5.3` 旧文写"保留 zh/en/fr、trial"与 `§8.3`"仅中英双语"冲突；i18n 契约按 **§8.3**，trial 功能删除（`VITE_TRIAL_*` 与中文默认文案一并移除）。
5. **`upd_date` 格式**：统一 `YYYY-MM-DD`（`FW_UPD_DATE` 当前输出即此格式，`protocol.md:46` 同）；若未来改为完整 ISO8601，需协议升版。
6. **`esp_app_desc` 解析偏移**（§6.4）基于 ESP-IDF 5.2 `esp_app_format.h` 布局；若 IDF 升级改变布局，仅影响前端"上传前展示"的可选增强，不涉及协议。

---

## 附录 A：版本字符串正则

```text
固件/Web 发布 tag：  ^v[0-9]+\.[0-9]+\.[0-9]+$
开发版本：          ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+-g[0-9a-f]{7,})?(-dirty)?$
Web package.json：  ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$
protocol_version：  ^[1-9][0-9]*$
```

## 附录 B：启动日志样例（release）

```text
I (312) BIRDIEBOT: WiFi Dap fw=v1.0.0 proto=1 board=BOARD_ZXR_DAP_S3 idf=5.2.1
I (1450) wifi_manager: AP SSID=WiFi-Dap-A1B2 IP=192.168.1.1   # 不打印口令
I (3120) web_server: HTTP server started on port 80
I (3200) WS_REQ: ws open: 3
```

## 附录 C：字段来源速查（前端）

| 前端需要 | 来源 | 备注 |
|---|---|---|
| 固件版本/日期/协议 | `SYSTEM/GET_FM_INFO` | 连接后立即请求；重连重发 |
| 固件 SN/hw/mf | `SYSTEM/GET_SYS_INFO` | 可空字符串；About 显示 `-` 兜底 |
| Web UI 版本/构建 | `import.meta.env.VITE_APP_*` | 构建期注入，运行期不可变 |
| 运行分区/版本 | `OTA/GET_INFO` | 仅 Update 页；需 OTA 契约就绪 |
| OTA 进度 | `OTA/GET_PROGRESS` | 轮询 1s（D 现状）或 OTA 契约规定 |

*契约版本：v1，2026-10-20，Wave 0 冻结；维护：Lead；本文与 `protocol.md`、`ota-local-upload.md`、`i18n-keys.md` 同步变更。*
