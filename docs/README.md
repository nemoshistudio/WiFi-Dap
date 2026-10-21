# WiFi Dap 文档索引

本目录是项目的**文档地图与维护规则入口**。用户手册在仓库根目录（[README.md](../README.md) / [README_CN.md](../README_CN.md)）；本索引说明每份文档的用途、哪些是冻结契约、哪些还在产出中，以及文档变更流程。

> 维护者：文档负责人（Wave 3）。冻结契约（见 [§2](#2-冻结契约lead-维护)）只允许 **Lead** 修改并广播；其余文档由对应 WP/负责人维护。
> 链接约定：本仓库内全部使用**相对路径**；引用仓库外材料（`analysis/`）只写文件名、不做超链接。

---

## 1. 文档地图（按读者）

| 读者/目的 | 文档 | 说明 |
|---|---|---|
| 新用户 / 烧录与使用 | [README_CN.md](../README_CN.md) / [README.md](../README.md) | 项目定位、功能矩阵、引脚、构建烧录、Web UI、调试接入、安全默认、故障排查 |
| 部署加固 / 安全自查 | [security.md](security.md) | 面向部署者的精简安全指南：默认实验室模式、开鉴权/改口令/关传输、端口与发现面清单、OTA 限制、Accepted risks 与 N1–N9 修复状态 |
| 许可证 / 合规署名 | [NOTICE](../NOTICE) / [LICENSES/](../LICENSES/) | 混合许可总览（本项目原创 Apache-2.0 + 第三方 MIT/Apache-2.0 组件）与许可证全文 |
| 工具链 / 构建发布 | [scripts/README.md](../scripts/README.md) | Docker ESP-IDF 5.2.1 构建、分区/板级校验、Web 资产、发布包、OTA/E2E 测试脚本 |
| Web UI 前端开发 | [web/README.md](../web/README.md) / [web/README.cn.md](../web/README.cn.md) | Web UI 功能、开发与构建方式（Vue 3 + Vite） |
| OTA 固件实现 | [components/wt_ota/README.md](../components/wt_ota/README.md) | OTA 核心状态机、C API、模块 5 注册、回滚与线程安全 |
| 协议/接口开发 | [protocol.md](protocol.md)、[ws-binary-router.md](ws-binary-router.md) | JSON/WS 总协议与二进制数据路由（冻结契约） |
| 契约背景与合并决策 | [merge-map.md](merge-map.md) | W0 三方合并快照、A→C 路径映射与差异统计（冻结契约，不面向最终用户） |

---

## 2. 冻结契约（Lead 维护）

以下 8 个文件是 **v1 冻结草案**，是前后端/固件/脚本的共同依据。变更规则：

- 只允许 **Lead 统一修改并广播**；每个 Wave 最多 2 次契约变更（各文件头部有同样声明）。
- 冻结内容不因实现细节调整而改；实现与契约冲突时，先记录差异并走契约变更流程，不得单方面改契约。
- 引用方（固件/前端/脚本）只能按契约实现；新增可选字段需契约明确允许。

| 契约 | 范围 | 主要实现/消费方 |
|---|---|---|
| [protocol.md](protocol.md) | 传输层（HTTP/WS/心跳/帧大小/并发/鉴权）、JSON 请求-响应与错误码、模块 0/1/2/4/5 命令总表、二进制帧格式、版本兼容 | `project_components/web_server/`、`project_components/api_router/`、`web/src/api/`、`components/dataflow/`、`components/uart_service/` |
| [ws-binary-router.md](ws-binary-router.md) | WebSocket 二进制数据路由 C API（`wt_ws_router_*`，冻结头文件 `components/ws_router/include/wt_ws_router.h`）、UART RAW/BROADCAST 数据面、队列与线程约束 | `components/ws_router/`、`project_components/web_server/uri_modules/uri_bin_router.c`、`components/dataflow/` |
| [ota-local-upload.md](ota-local-upload.md) | 浏览器本地 OTA：8 MB 双槽分区契约、`POST /api/ota/upload`、模块 5 命令、状态机、超时/中止、回滚、鉴权 | `components/wt_ota/`、`project_components/web_server/uri_modules/uri_ota.c`、`main/main.c`（回滚确认钩子）、`web/src/views/Update.vue` |
| [wifi-credential-memory.md](wifi-credential-memory.md) | Wi-Fi 多凭据记忆：NVS schema（`wt_nvs/wifi_cred`）、轮换算法、AP SSID 规则、JSON cmd 11–14 | `project_components/wifi_manager/`（`wifi_credential.*`、`wifi_api_json.c`）、`web/src/views/Wifi.vue` |
| [versioning.md](versioning.md) | 固件 semver / 协议版本 / Web UI 版本的唯一来源、注入机制、运行时暴露、握手与发布门禁 | `cmake/version.cmake`、`project_components/wt_common/wt_protocol_version.h`、`web/scripts/gen-env.mjs`、`scripts/check-release-versions.sh` |
| [i18n-keys.md](i18n-keys.md) | Web UI i18n key 契约（zh/en，冻结后 314 key）、命名规范、禁止硬编码文案、检查脚本 | `web/src/locales/**`、`web/scripts/check-i18n.mjs`、`web/build.sh` |
| [board-pins.md](board-pins.md) | 板级引脚与硬件默认值唯一契约（4 块板、LED 缺失约定、UART 桥/默认时钟/端口、板选择方式） | `components/board/`、`components/DAP/`、`components/uart_tcp_bridge/`、`scripts/check_board_pins.py` |
| [merge-map.md](merge-map.md) | W0 参考快照清单、三方合并基准（base/A/C/D）、A→C 路径映射、关键差异与统计、合并/对比命令 | 合并审查与 Wave-1 任务拆分（历史依据；日常开发不需要读） |

> 说明：契约文件头部的 "相关契约" 与 `analysis/...` 引用是 **W0 决策依据**，`analysis/` 不随仓库发布（见 [§5](#5-计划与分析材料不入库)），因此这些引用不是可点击链接。

---

## 3. 测试与验证文档

> 状态：**测试文档已全部入库**（DAP / OTA / UART-DataFlow / WiFi 四类硬件清单 + host 测试报告 + DAP 边界 host 测试选题）。

| 文档/工具 | 类型 | 内容 | 状态 |
|---|---|---|---|
| [hardware-test-ota.md](hardware-test-ota.md) | 硬件验收清单 | 8 MB S3 真机 OTA：首次烧录/otadata、Web UI 与 HTTP 直传、错误注入矩阵、回滚验证、通过标准与记录模板 | ✅ 已入库（Wave 3） |
| [hardware-test-uart-dataflow.md](hardware-test-uart-dataflow.md) | 硬件验收清单 | UART/DataFlow/WS 二进制 E2E：T1 连通/版本、T2 UART 配置、T3 浏览器终端、T4 二进制回环、T5 TCP 桥、T6 attach、T7 双客户端广播、T8 OTA 信息 | ✅ 已入库（Wave 2/3） |
| [hardware-test-dap.md](hardware-test-dap.md) | 硬件验收清单 | DAP/USBIP/elaphureLink/WS-DAP 真机验证（Keil/OpenOCD/pyOCD）、R2/R9 P0 项、逻辑分析仪判据 | ✅ 已入库（dap-dev） |
| [hardware-test-wifi.md](hardware-test-wifi.md) | 硬件验收清单 | Wi-Fi 多凭据记忆/AP SSID：T0 默认 AP、T1–T5 cmd 11–14、轮换与持久化、错误码；含只读自动脚本说明 | ✅ 已入库（wifi-dev） |
| [host-test-report.md](host-test-report.md) | host 测试报告 | Wave 3 / WP-302 纯逻辑与契约测试：C 125 checks + Python 60 tests（ASan/UBSan），2 个 suite 全部 PASS；含 F1/F2 findings 与覆盖缺口 | ✅ 已入库（QA） |
| [dap-boundary-host-tests.md](dap-boundary-host-tests.md) | host 测试选题（只读走查） | DAP ↔ 传输层边界契约速查与 host 可测候选（HT-01…），供 QA 选题与后续测试扩展 | ✅ 已入库（Wave 3） |
| [scripts/e2e_ws_client.py](../scripts/e2e_ws_client.py) | 自动化脚本 | 纯标准库 WS 客户端：`ping` / `wifi-scan` / `uart-echo` / `dataflow-list` / `ota-info` / `json`，供上面的清单直接引用 | ✅ 已入库 |
| [scripts/ota_upload.py](../scripts/ota_upload.py) | 自动化脚本 | 真机 OTA：HTTP 直传 `/api/ota/upload` + WS 模块 5 `GET_INFO` 轮询，支持 `--token`/错误注入 | ✅ 已入库 |
| [merge-and-verification-2026-10-21.md](merge-and-verification-2026-10-21.md) | 合并 + 全量验证报告 | 本次全量合并清单（含 C1）、构建矩阵、host 测试、真机 WiFi/USB-NCM/USBIP/DAP profiling 验证数据与回滚方式 | ✅ 已入库（2026-10-21） |
| [c1-ncm-status.md](c1-ncm-status.md) | C1 有线 DAP 状态 | USB-NCM 双会话根因/修复、NCM 与 WiFi 验收数据、主机侧注意事项、残余风险 | ✅ 已入库（2026-10-21） |
| [c1-second-session-analysis.md](c1-second-session-analysis.md) | C1 根因分析附录 | 第二会话 `CMD_INFO failed` 的完整证据链、修复独立评审与加固建议（含补丁） | ✅ 已入库（2026-10-21） |
| [c1-ncm-flash-hang-analysis.md](c1-ncm-flash-hang-analysis.md) | C1 烧录卡死走查 | NCM 烧录卡死的代码走查与根因假设（历史分析，w7/c1review） | ✅ 已入库（2026-10-21） |
| [h7-flash-speedup.md](h7-flash-speedup.md) | 速度优化报告 | H7 烧录/调试 A/B 实测（sysprogs 分析、OpenOCD 候选、时钟扫描、增量烧录、C4 负结果） | ✅ 已入库（2026-10-21） |
| [scripts/dap_stats_query.py](../scripts/dap_stats_query.py) | 自动化脚本 | stats 构建的 Vendor9 profiling 计数器读取（pyusb 原始 CMSIS-DAP，无需串口） | ✅ 已入库（2026-10-21） |
| [web-variable-curve.md](web-variable-curve.md) | Web 功能说明 | 浏览器变量曲线页：ELF 选变量、WS-DAP 采样、`/api/dap/kick` 接管、复位/限速/导出、限制与验收证据 | ✅ 新增（2026-10-21） |
| [ozone-timeline-hss-feasibility.md](ozone-timeline-hss-feasibility.md) | 可行性报告 | Ozone Timeline(HSS) 接入 WiFi DAP：官方支持边界、代理 DLL 方案、ABI、风险与 Phase-0 门槛 | ✅ 新增（2026-10-21） |

测试文档中的 **【需真机】** / **【本地】** / **【需 4MB 硬件/可选】** 标记约定见各文档开头；C3/ESP32 仅为编译验收目标。
host 测试报告引用的 `tests/host/**`、`scripts/test_host.sh`、`scripts/dap_smoke.py`、`scripts/test_wifi_*` 等随对应 Wave 3 分支合入，本索引只索引文档、不直接链接这些脚本产物。

---

## 4. 安全审查

- **安全审查报告**：[security-review-web.md](security-review-web.md)（Web/OTA/WiFi 面，Wave3；P0 现状 0 / 条件性 P0 1 / P1 4 / P2 10）；[security-review-dap.md](security-review-dap.md)（DAP 传输面，N1–N9；P0×2 / P1×3 / P2×4）。
- **面向部署者的加固指南**：[security.md](security.md)（默认实验室模式、开鉴权/改 AP 口令/关 3240+1346、OTA 限制、端口与发现面清单）。
- **风险处置**：用户已决定的安全范围（P1-1/P1-3/P1-4/条件性 P0 按工作室内网接受；N1–N9 修复状态；P3 忽略）见 [security.md](security.md#accepted-risks-studio-lan)。
- 已入库文档中的安全相关内容可直接引用：
  - [protocol.md §5](protocol.md)（默认无鉴权、鉴权开启后的 token/Origin 要求、3240/1346 开关提示）；
  - [ota-local-upload.md §8](ota-local-upload.md)（OTA 上传端点鉴权要求）；
  - [README §9](../README_CN.md)（面向部署者的安全默认与加固清单，含 Known security limitations）；
  - [project_components/web_server/Kconfig](../project_components/web_server/Kconfig)（`BIRDIEBOT_WEB_AUTH` / `BIRDIEBOT_WEB_TOKEN` / `BIRDIEBOT_WEB_ALLOWED_ORIGIN` 选项说明）。
- 审查报告中的**条件性 P0** 指 WP-206 `uri_ota.c`（`POST /api/ota/upload`）合入时若未强制鉴权/校验则即时生效；合入后应通知审查人按报告 §4.3 复审一次。未修复项应同时登记到报告与对应契约的“已知限制”。

---

## 5. 计划与分析材料（不入库）

W0–W3 的设计与决策材料放在工作区的 `analysis/` 目录，**不进入本仓库**（当前仓库树内不存在该目录）。契约与文档中出现的 `analysis/xxx.md` 引用仅作历史决策依据，不可点击。

被文档引用过的代表性文件（仅供参考，均不在仓库内）：

- `analysis/INTEGRATION-PLAN.md`（集成计划 v4：需求、端口/功能边界、P7.1 安全约束）
- `analysis/PARALLEL-DEV-PLAN.md`（WP 拆分与并行开发计划）
- `analysis/REPORT.md`、`analysis/REPORT-2-esp32-tools.md`（A/C 源码审查与已知缺陷）
- `analysis/01-dap-core.md`、`analysis/02-network-transport.md`、`analysis/03-fork-build.md`（DAP 核心 / 网络传输 / fork 构建分析）

如需公开这些材料，应由 Lead 决定是否脱敏后迁入 `docs/`，而不是直接链接外部路径。

---

## 6. 构建、发布与 CI 文档

- [scripts/README.md](../scripts/README.md)：本地 Docker 构建（`idf-docker.sh`）、构建锁（`build-lock.sh`）、三目标一键构建（`build_all.sh`）、分区/板级/Web 资产校验、发布包（`release_local.sh`）、OTA/E2E 脚本用法。**构建与发布以该文档为准。**
- [.github/workflows/main.yml](../.github/workflows/main.yml)：**本地 CI 模板**（`workflow_dispatch` 手动触发、不 push、不发布）；与 `scripts/` 中的本地脚本等价，当前项目不依赖 GitHub Actions。
- [partitions/](../partitions/)：分区表契约文件（`partitions_8mb_ota.csv` / `partitions_4mb_single.csv`），语义见 [ota-local-upload.md §1](ota-local-upload.md)。

---

## 7. 文档维护规则

1. **相对链接**：仓库内文档互链必须使用相对路径；不要链接 `analysis/`、构建产物 `build/`、或本机绝对路径。
2. **中英同步**：`README.md` 与 `README_CN.md` 内容保持一致，修改一侧时必须同步另一侧（结构、命令、链接）。
3. **冻结契约**：见 [§2](#2-冻结契约lead-维护)。文档负责人不修改冻结契约；发现不一致时，在交付说明/任务中报给 Lead。
4. **测试清单**：硬件测试文档由对应模块负责人维护；新增脚本或改变设备行为时同步更新清单。
5. **i18n**：新增/修改前端文案走 [i18n-keys.md](i18n-keys.md) 流程（`web/scripts/check-i18n.mjs` 为构建前置检查）。
6. **状态先行**：新增文档先在本索引登记（用途 + 状态），未产出的文档标 **待产出**，不写死链接。
7. **安全限制同步**：`security-review-web.md` 的 P0/P1 结论若有变化（尤其条件性 P0 / WP-206 合入），同步更新根 README「Known security limitations」与 [security.md](security.md)。
8. **版本与发布**：修改版本注入/发布流程时，同步 [versioning.md](versioning.md) 与 [scripts/README.md](../scripts/README.md)。

---

## 8. 文档状态总览

| 文档 | 负责人/来源 | 状态 |
|---|---|---|
| 根 `README.md` / `README_CN.md`（WiFi Dap 完整文档） | 文档（Wave 3） | ✅ 已重写（Known security limitations + Accepted risks + 许可证口径修正） |
| `docs/README.md`（本索引） | 文档（Wave 3） | ✅ 本次更新 |
| [security.md](security.md)（部署加固指南） | 文档（Wave 3） | ✅ 本次更新（Accepted risks + N1–N9 状态 + 128-bit token 口径） |
| [NOTICE](../NOTICE) | 文档/合规（Wave 3） | ✅ 本次新增（版权与第三方来源汇总） |
| [LICENSES/](../LICENSES/)（`Apache-2.0.txt` / `MIT.txt`） | 文档/合规（Wave 3） | ✅ 本次新增（许可证全文） |
| 8 份冻结契约（[§2](#2-冻结契约lead-维护)） | Lead | ✅ 冻结 v1（2026-10-20） |
| [hardware-test-ota.md](hardware-test-ota.md) | OTA/硬件验证（Wave 3） | ✅ 已入库 |
| [hardware-test-uart-dataflow.md](hardware-test-uart-dataflow.md) | UART/DataFlow（Wave 2/3） | ✅ 已入库 |
| [hardware-test-dap.md](hardware-test-dap.md) | dap-dev | ✅ 已入库 |
| [hardware-test-wifi.md](hardware-test-wifi.md) | wifi-dev | ✅ 已入库 |
| [host-test-report.md](host-test-report.md) | QA（WP-302） | ✅ 已入库 |
| [dap-boundary-host-tests.md](dap-boundary-host-tests.md) | Wave 3（只读走查） | ✅ 已入库 |
| [security-review-web.md](security-review-web.md) | webhost-analyst（WP-304） | ✅ 已入库 |
| [security-review-dap.md](security-review-dap.md) | dap-core-analyst（sec review A） | ✅ 已入库 |
| [scripts/README.md](../scripts/README.md) | 构建/发布 | ✅ 已入库 |
| [web/README.md](../web/README.md) / [web/README.cn.md](../web/README.cn.md) | Web UI | ✅ 已入库（上游来源说明） |
| [components/wt_ota/README.md](../components/wt_ota/README.md) | OTA 固件 | ✅ 已入库 |
