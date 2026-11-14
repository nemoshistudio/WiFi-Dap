# WiFi Dap 文档索引

本目录是项目的**文档地图与维护规则入口**。用户手册在仓库根目录（[README.md](../README.md) / [README_CN.md](../README_CN.md)）；本索引说明每份文档的用途、哪些是冻结契约、哪些是历史归档，以及文档变更流程。

> 维护者：文档负责人。冻结契约（见 [§2](#2-冻结契约lead-维护)）只允许 **Lead** 修改并广播；其余文档由对应负责人维护。
> 链接约定：本仓库内全部使用**相对路径**；引用仓库外材料只写文件名、不做超链接。

> **当前产品状态：final slim（2026-10-26 起）。** 默认固件/Web UI 已移除 Web OTA、`/api/discover`、SSDP、设备发现卡、首次使用引导、在线手册入口与 Ozone 兼容；新增独立 `/#/status` 设备状态页 + 全站 compact 状态条。逐项移除/恢复/资源收益见 [feature-flags.md](feature-flags.md)，当前 Web UI 结构见 [web-status-ui.md](web-status-ui.md)；已移除功能的历史文档在 [archive/removed-features/](archive/removed-features/README.md)（**不是当前功能说明**）。

---

## 1. 文档地图（按读者）

| 读者/目的 | 文档 | 说明 |
|---|---|---|
| **新维护者 / 交接** | [PROJECT-HANDOFF.md](PROJECT-HANDOFF.md) | **先读这份**：项目现状、设备/固件、构建/烧录/HIL、已知问题、下一步与禁止事项 |
| 新用户 / 烧录与使用 | [README_CN.md](../README_CN.md) / [README.md](../README.md) | 项目定位、功能矩阵、引脚、构建烧录、Web UI、调试接入、安全默认、故障排查（已按 final slim 更新） |
| **离线 HTML 手册** | [manual/index.html](manual/index.html)（快速上手）/ [manual/manual.html](manual/manual.html)（完整手册）/ [manual/developer.html](manual/developer.html)（开发者附录） | 三本独立离线静态手册，浏览器直接打开；**不随固件发布、Web UI 无入口**（B5 已移除在线手册入口） |
| **当前功能状态 / 恢复** | [feature-flags.md](feature-flags.md) | final slim 移除项（A1/A2/H1/H3/J1/M1/C4、B2/B3/B5）、Kconfig 开关、备份 tag `backup/full-features-2026-10-26`、逐项恢复步骤、资源收益、验证记录 |
| **Web UI 当前结构** | [web-status-ui.md](web-status-ui.md) | 页面地图（target/status/curve/uart/wifi/about）、`/#/status` 详情页、compact 状态条、数据来源与 HIL 断言 |
| **历史归档** | [archive/README.md](archive/README.md) / [archive/removed-features/README.md](archive/removed-features/README.md) | 已移除功能的历史契约/清单/旧任务简报（OTA、discover、SSDP、Ozone、引导等）与按日期归档的验证报告；**仅作证据，不是当前功能** |
| XRDAP 借鉴项落地（M1–M4） | [xrdap-learnings.md](xrdap-learnings.md) | 探针唯一身份/CMSIS-DAP caps 合规/mDNS TXT、本地 CI 门禁、TCP 3241 骨架、S3 高速 SWD 引擎：默认状态、验证结果、HIL 待办与回退（2026-11-14，非冻结契约） |
| SWD 高速引擎 | [swd-23mhz-engine.md](swd-23mhz-engine.md) | ESP32-S3 dedicated-GPIO 20/24 MHz 档位、时序预算、`DAP_SWJ_Clock` 接线、Kconfig、HIL 清单与风险 |
| CMSIS-DAP over TCP 骨架 | [dap-tcp-backend.md](dap-tcp-backend.md) | 3241 端口 `DAP\0` 帧格式、流解析、单客户端服务器、DAP 执行器接线计划、HIL H1–H14（默认关闭） |
| 部署加固 / 安全自查 | [security.md](security.md) | 面向部署者的精简安全指南：默认实验室模式、开鉴权/改口令/关传输、端口与发现面清单、final slim 无 OTA 端点、Host/Origin guard 默认关闭（M1）、Accepted risks 与 N1–N9 修复状态 |
| 许可证 / 合规署名 | [NOTICE](../NOTICE) / [LICENSES/](../LICENSES/) | 混合许可总览（本项目原创 Apache-2.0 + 第三方 MIT/Apache-2.0 组件）与许可证全文 |
| 工具链 / 构建发布 | [scripts/README.md](../scripts/README.md) | Docker ESP-IDF 5.5.6 构建、分区/板级校验、Web 资产、发布包、E2E/HIL 脚本（OTA 脚本仅 full 构建适用） |
| IDF 基线 / 升级记录 | [idf-5.5.6-upgrade.md](idf-5.5.6-upgrade.md) | S3/C3/ESP32/C5 统一升级到 ESP-IDF v5.5.6 的迁移点、构建命令、验证矩阵、风险/回滚（历史迁移记录） |
| Web UI 前端开发 | [web/README.md](../web/README.md) / [web/README.cn.md](../web/README.cn.md) | Web UI 功能、开发与构建方式（Vue 3 + Vite） |
| 协议/接口开发 | [protocol.md](protocol.md)、[ws-binary-router.md](ws-binary-router.md) | JSON/WS 总协议与二进制数据路由（冻结契约；OTA §2.5 已标注 slim 移除） |
| 契约背景与合并决策 | [merge-map.md](merge-map.md) | W0 三方合并快照、A→C 路径映射与差异统计（冻结契约，不面向最终用户） |

---

## 2. 冻结契约（Lead 维护）

以下文件是 **v1 冻结草案**，是前后端/固件/脚本的共同依据。变更规则：

- 只允许 **Lead 统一修改并广播**；每个 Wave 最多 2 次契约变更（各文件头部有同样声明）。
- 冻结内容不因实现细节调整而改；实现与契约冲突时，先记录差异并走契约变更流程，不得单方面改契约。
- 引用方（固件/前端/脚本）只能按契约实现；新增可选字段需契约明确允许。
- **final slim 差异**：`protocol.md`/`versioning.md`/`wifi-credential-memory.md` 中标注“精简构建移除”的 OTA 章节仍保留为**协议编号/历史契约**（端点已不存在）；移除项以 [feature-flags.md](feature-flags.md) 为准。

| 契约 | 范围 | 主要实现/消费方 |
|---|---|---|
| [protocol.md](protocol.md) | 传输层（HTTP/WS/心跳/帧大小/并发/鉴权）、JSON 请求-响应与错误码、模块 0/1/2/4/5 命令总表、二进制帧格式、版本兼容 | `project_components/web_server/`、`project_components/api_router/`、`web/src/api/`、`components/dataflow/` |
| [ws-binary-router.md](ws-binary-router.md) | WebSocket 二进制数据路由 C API（`wt_ws_router_*`，冻结头文件 `components/ws_router/include/wt_ws_router.h`）、UART RAW/BROADCAST 数据面、队列与线程约束 | `components/ws_router/`、`project_components/web_server/uri_modules/uri_bin_router.c`、`components/dataflow/` |
| [wifi-credential-memory.md](wifi-credential-memory.md) | Wi-Fi 多凭据记忆：NVS schema（`wt_nvs/wifi_cred`）、轮换算法、AP SSID 规则、JSON cmd 11–14 | `project_components/wifi_manager/`（`wifi_credential.*`、`wifi_api_json.c`）、`web/src/views/Wifi.vue` |
| [versioning.md](versioning.md) | 固件 semver / 协议版本 / Web UI 版本的唯一来源、注入机制、运行时暴露、握手与发布门禁 | `cmake/version.cmake`、`project_components/wt_common/wt_protocol_version.h`、`web/scripts/gen-env.mjs`、`scripts/check-release-versions.sh` |
| [i18n-keys.md](i18n-keys.md) | Web UI i18n key 契约（zh/en 当前 947 key）、命名规范、禁止硬编码文案、检查脚本 | `web/src/locales/**`、`web/scripts/check-i18n.mjs`、`web/build.sh` |
| [board-pins.md](board-pins.md) | 板级引脚与硬件默认值唯一契约（5 块板、LED 缺失约定、UART 桥/默认时钟/端口、板选择方式；C5 UART1 TX=GPIO25/RX=GPIO26） | `components/board/`、`components/DAP/`、`components/uart_tcp_bridge/`、`scripts/check_board_pins.py` |
| [merge-map.md](merge-map.md) | W0 参考快照清单、三方合并基准（base/A/C/D）、A→C 路径映射、关键差异与统计、合并/对比命令 | 合并审查与 Wave-1 任务拆分（历史依据；日常开发不需要读） |

> **已移出冻结契约表**：[ota-local-upload.md](archive/removed-features/ota-local-upload.md)
> （浏览器 OTA 契约）随 A1/A2 移除，归档到 `docs/archive/removed-features/`，仍需从备份 tag
> 恢复 full 构建时使用；当前更新方式见根 README §4.5/§6。
> [feature-flags.md](feature-flags.md) 是**当前状态台账**而非冻结契约：记录每个移除项、Kconfig
> 开关、备份 tag 与恢复步骤，随产品状态更新。

> 说明：契约文件头部的 "相关契约" 与 `analysis/...` 引用是 **W0 决策依据**，除
> `analysis/slim-hil/`（final slim HIL 证据，已入库）外，`analysis/` 不随仓库发布
> （见 [§5](#5-计划与分析材料)），因此这些引用不是可点击链接。

---

## 3. 测试与验证文档

> 状态：**测试文档已全部入库**。当前验收以 final slim HIL 为准；OTA/discover 相关清单仅适用于
> 从 `backup/full-features-2026-10-26` 恢复的 full 构建。

| 文档/工具 | 类型 | 内容 | 状态 |
|---|---|---|---|
| [hil-gate.md](hil-gate.md) | **HIL 门禁/性能验证（当前）** | §1–§10 full 基线 + **§11 final slim profile**：`--profile auto` 识别、OTA/discover 移除断言、6 路由/状态页/compact strip 页面断言、WS/mDNS TXT、SWD 时钟矩阵、soak（S3-F407 600 s clean / C5-H7 180 s clean）、USBIP smoke | ✅ final slim 已验证（2026-10-26，`analysis/slim-hil/`） |
| [hardware-test-uart-dataflow.md](hardware-test-uart-dataflow.md) | 硬件验收清单 | UART/DataFlow/WS 二进制 E2E：T1–T8（连通/配置/终端/回环/TCP 桥/attach/广播；T8 OTA 信息仅 full 构建） | ✅ 已入库（Wave 2/3） |
| [hardware-test-dap.md](hardware-test-dap.md) | 硬件验收清单 | DAP/USBIP/elaphureLink/WS-DAP 真机验证（Keil/OpenOCD/pyOCD）、R2/R9 P0 项、逻辑分析仪判据 | ✅ 已入库（dap-dev） |
| [hardware-test-wifi.md](hardware-test-wifi.md) | 硬件验收清单 | Wi-Fi 多凭据记忆/AP SSID：T0 默认 AP、T1–T5 cmd 11–14、轮换与持久化、错误码 | ✅ 已入库（wifi-dev） |
| [host-test-report.md](host-test-report.md) | host 测试报告 | Wave 3 纯逻辑/契约测试：C 125 checks + Python 60 tests（ASan/UBSan）；F1 OTA 代码映射属 full 构建历史 | ✅ 已入库（QA，历史报告） |
| [dap-boundary-host-tests.md](dap-boundary-host-tests.md) | host 测试选题（只读走查） | DAP ↔ 传输层边界契约速查与 host 可测候选（HT-01…） | ✅ 已入库（Wave 3） |
| [hil-extended.md](hil-extended.md) | 扩展 HIL 脚本说明 | `hil_extended.py` 的 Web Flash/OTA 半包/USBIP 阶段；**OTA/扩展阶段仅适用于 full 构建**，slim 下按 usage error/SKIP 处理 | ✅ 已入库（历史脚本，保留） |
| [scripts/e2e_ws_client.py](../scripts/e2e_ws_client.py) | 自动化脚本 | 纯标准库 WS 客户端：`ping` / `wifi-scan` / `uart-echo` / `dataflow-list` / `json`（`ota-info` 仅 full 构建） | ✅ 已入库 |
| [scripts/hil_gate.py](../scripts/hil_gate.py) | 自动化脚本 | 只读门禁 + `--profile auto|slim`（final slim 断言见 [hil-gate.md §11](hil-gate.md)） | ✅ final slim 已扩展 |
| [analysis/slim-hil/README.md](../analysis/slim-hil/README.md) | **final slim HIL 证据** | S3(F407) + C5(H7) 真机报告 JSON/时间戳：gate/SWD 矩阵/soak/USBIP；S3 600 s、C5 180 s clean | ✅ 已入库（commit `hil/slim`） |
| ~~hardware-test-ota.md~~ | 历史清单 | OTA 硬件验收（首烧/上传/错误注入/回滚） | 📦 已归档：[archive/removed-features/hardware-test-ota.md](archive/removed-features/hardware-test-ota.md) |
| ~~ota_upload.py / ota-info~~ | 历史脚本/子命令 | OTA 上传与 module 5 轮询 | 脚本保留，仅 full 构建适用（见 [feature-flags.md](feature-flags.md)） |
| [merge-and-verification-2026-10-21.md](merge-and-verification-2026-10-21.md) | 合并 + 全量验证报告 | 2026-10-21 全功能合并清单、构建矩阵、host 测试、真机验证数据（历史） | ✅ 已入库（历史） |
| [c1-ncm-status.md](c1-ncm-status.md) / [c1-second-session-analysis.md](c1-second-session-analysis.md) | C1 有线 DAP 状态/根因 | USB-NCM 双会话根因/修复、第二会话 `CMD_INFO` 证据链（历史） | ✅ 已入库（2026-10-21） |
| [h7-flash-speedup.md](h7-flash-speedup.md) | 速度优化报告 | H7 烧录/调试 A/B 实测（历史） | ✅ 已入库（2026-10-21） |
| [web-variable-curve.md](web-variable-curve.md) / [target-flash-formats.md](target-flash-formats.md) | Web 功能说明 | 变量曲线（含 C4 ≤64 说明）、目标烧录多格式与 SHA 契约 | ✅ 已入库 |

测试文档中的 **【需真机】** / **【本地】** / **【需 4MB 硬件/可选】** 标记约定见各文档开头；C3/ESP32 仅为编译验收目标。

---

## 4. 安全审查

- **面向部署者的加固指南（当前）**：[security.md](security.md)（默认实验室模式、开鉴权/改 AP 口令/关 3240+1346、端口与发现面清单、final slim 无 OTA 端点、Host/Origin guard 默认关闭（M1）、Accepted risks 与 N1–N9 状态）。
- **历史安全审查报告**（full 构建基线，OTA/discover 端点现已移除、Host/Origin guard 默认关闭）：[security-review-web.md](security-review-web.md)（P0 现状 0 / 条件性 P0 1 / P1 4 / P2 10）、[security-review-dap.md](security-review-dap.md)（N1–N9）。
- **风险处置**：用户已决定的安全范围（P1-1/P1-3/P1-4/条件性 P0 按工作室内网接受；N1–N9 修复状态；P3 忽略；M1 Host/Origin guard 默认关闭）见 [security.md](security.md#accepted-risks-studio-lan)。
- 安全相关契约：[protocol.md §5](protocol.md)、[README §9](../README_CN.md#9-安全默认与加固)、`project_components/web_server/Kconfig`（`BIRDIEBOT_WEB_AUTH` / `BIRDIEBOT_WEB_TOKEN` / `BIRDIEBOT_WEB_ALLOWED_ORIGIN` / `BIRDIEBOT_WEB_HOST_GUARD`）。
- 历史 OTA 审查条目（条件性 P0 `uri_ota.c`）已随 A1 移除；从备份 tag 恢复 full 构建时应重新复审。

---

## 5. 计划与分析材料

- `analysis/slim-hil/`：**final slim HIL 证据**（gate/SWD/soak/USBIP 报告 JSON、时间戳、README），已 force-add 入库（`.gitignore` 忽略 `analysis/` 的规则对该目录例外）——[索引](../analysis/slim-hil/README.md)。
- 其余 W0–W3 设计与决策材料放在工作区 `analysis/`，**不进入本仓库**；契约与文档中出现的 `analysis/xxx.md` 引用仅作历史决策依据，不可点击。
- 如需公开这些材料，应由 Lead 决定是否脱敏后迁入 `docs/`，而不是直接链接外部路径。

---

## 6. 构建、发布与 CI 文档

- [scripts/README.md](../scripts/README.md)：本地 Docker 构建（`idf-docker.sh`）、构建锁（`build-lock.sh`）、多目标构建（`build_all.sh`）、分区/板级/Web 资产校验、发布包（`release_local.sh`）、E2E/HIL 脚本。**构建与发布以该文档为准。**
- [.github/workflows/main.yml](../.github/workflows/main.yml)：**本地 CI 模板**（`workflow_dispatch` 手动触发、不 push、不发布）；与 `scripts/` 中的本地脚本等价，当前项目不依赖 GitHub Actions。
- [partitions/](../partitions/)：分区表契约文件（`partitions_8mb_ota.csv` / `partitions_4mb_single.csv`）。final slim 保留双槽布局与回滚确认，但**不再暴露 Web OTA**；分区/回滚语义的历史契约见 [archive/removed-features/ota-local-upload.md](archive/removed-features/ota-local-upload.md)。
- 固件更新（slim）：UART0 或内置 USB Serial/JTAG（S3：BOOT+RESET ROM 下载；C5：OpenOCD + USB-JTAG），见根 README §4.5/§6 与 [esp32-c5-port.md](esp32-c5-port.md)（C5，由 c5-hw 维护）。

---

## 7. 文档维护规则

1. **相对链接**：仓库内文档互链必须使用相对路径；不要链接 `analysis/`（`analysis/slim-hil/` 已入库例外）、构建产物 `build/`、或本机绝对路径。提交前跑一遍相对链接扫描（无内置 `check_docs` 脚本）。
2. **中英同步**：`README.md` 与 `README_CN.md` 内容保持一致，修改一侧时必须同步另一侧（结构、命令、链接）。
3. **冻结契约**：见 [§2](#2-冻结契约lead-维护)。文档负责人不修改冻结契约；发现不一致时，在交付说明/任务中报给 Lead。
4. **当前 vs 历史**：描述**当前功能**的文档不得把已移除功能写成可用；历史材料放 [archive/](archive/README.md) 并在文件头标注“已归档”。引用归档内容时必须写“历史归档”。
5. **测试清单**：硬件测试文档由对应模块负责人维护；新增脚本或改变设备行为时同步更新清单。
6. **i18n**：新增/修改前端文案走 [i18n-keys.md](i18n-keys.md) 流程（`web/scripts/check-i18n.mjs` 为构建前置检查）。
7. **状态先行**：新增文档先在本索引登记（用途 + 状态），未产出的文档标 **待产出**，不写死链接。
8. **安全限制同步**：`security-review-web.md` 的 P0/P1 结论若有变化，同步更新根 README「Known security limitations」与 [security.md](security.md)。
9. **版本与发布**：修改版本注入/发布流程时，同步 [versioning.md](versioning.md) 与 [scripts/README.md](../scripts/README.md)。

---

## 8. 文档状态总览

| 文档 | 负责人/来源 | 状态 |
|---|---|---|
| 根 `README.md` / `README_CN.md` | 文档 | ✅ final slim 已更新（功能矩阵/Web UI/安全/故障排查/链接） |
| [manual/index.html](manual/index.html) / [manual/manual.html](manual/manual.html) / [manual/developer.html](manual/developer.html)（离线 HTML 手册） | 文档 | ✅ 已入库（三本独立静态手册；按 final slim 更新，Web UI 无入口） |
| `docs/README.md`（本索引） | 文档 | ✅ 本次更新（归档/新文档/历史标记） |
| [feature-flags.md](feature-flags.md) | 固件/前端/文档 | ✅ 本次补 final 状态、备份 tag、归档路径 |
| [web-status-ui.md](web-status-ui.md) | 前端/文档 | ✅ 本次新增（/status + compact strip） |
| [hil-gate.md](hil-gate.md) §11 | HIL/测试 | ✅ final slim 验证记录 |
| [archive/README.md](archive/README.md) / [archive/removed-features/README.md](archive/removed-features/README.md) | 文档 | ✅ 本次新增（归档索引） |
| [security.md](security.md) | 文档 | ✅ final slim 差异（无 OTA 端点、Host/Origin guard 默认关闭） |
| [NOTICE](../NOTICE) / [LICENSES/](../LICENSES/) | 文档/合规 | ✅ 已入库 |
| 冻结契约（[§2](#2-冻结契约lead-维护)） | Lead | ✅ v1（2026-10-20）；slim 差异见 feature-flags |
| [hardware-test-uart-dataflow.md](hardware-test-uart-dataflow.md) / [hardware-test-dap.md](hardware-test-dap.md) / [hardware-test-wifi.md](hardware-test-wifi.md) | 各模块负责人 | ✅ 已入库（OTA 清单已归档） |
| [security-review-web.md](security-review-web.md) / [security-review-dap.md](security-review-dap.md) | 安全审查 | ✅ 已入库（full 构建历史基线） |
| [scripts/README.md](../scripts/README.md) | 构建/发布 | ✅ 已入库 |
| [web/README.md](../web/README.md) / [web/README.cn.md](../web/README.cn.md) | Web UI | ✅ 已入库（含 OTA 移除说明） |
