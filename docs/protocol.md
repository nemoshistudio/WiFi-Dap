# WiFi Dap JSON / WebSocket 协议契约（v1）

> 状态：**冻结草案 v1（2026-10-20，Wave 0）**。Wave 1/2 的前后端实现以本文为准；变更需 Lead 统一修改并广播。
> 相关契约：`docs/ws-binary-router.md`（二进制路由 API）、`docs/ota-local-upload.md`（OTA 上传）、`docs/wifi-credential-memory.md`（多 WiFi 凭据）、`docs/versioning.md`（版本/握手）、`docs/i18n-keys.md`（前端文案 key）。

---

## 1. 传输层

| 项 | 约定 |
|---|---|
| HTTP | 端口 80，`esp_http_server`；`GET /` 返回内嵌 UI（gzip）；`GET /ws.*` 返回 SharedWorker JS（gzip）；`GET /SSDP.xml` |
| WebSocket | 单条连接：`ws://<device-ip>/ws`；文本帧 = JSON 消息；二进制帧 = 4 字节头 + payload |
| 心跳 | 固件每 **1500ms** 向所有 WS 客户端广播一个**空 TEXT 帧**；前端收到任意消息即重置 2s 超时 |
| 帧大小 | 单帧 payload 上限 = `memory_pool` 可用容量（当前 2048 - 内部头 ≈ 1.8KB）；超限关闭连接（JSON）或回错误帧（二进制） |
| 并发 | 请求-响应无 ID（沿用 D/C 约定）；同一时刻同 cmd 只允许一个在途；慢命令（WiFi 扫描/连接、OTA 进度）用异步事件/轮询响应 |
| 登录/鉴权 | **已移除**（无 token、无首帧 AUTH、无 `Authorization`）；`Host`/`Origin` 校验始终启用（防浏览器 CSWSH/DNS-rebinding）；HTTP/WS 明文 |

### 1.1 JSON 请求/响应

请求：`{"module":<u8>,"cmd":<u16>, ...业务字段}`。
成功：回显请求字段 + 结果字段（无结果字段时仅回显请求，作为隐式 ACK）。
失败：`{"module":<u8>,"cmd":<u16>,"err":"<message>","code":<int>}`。
异步事件：`{"module":<u8>,"event":"<name>", ...}`（当前未使用；OTA/UART 走轮询/二进制）。

错误码（沿用 C 的 `api_json_req_status_e`）：

| code | 含义 |
|---|---|
| 2 | `Bad json request`（缺 module/cmd、JSON 解析失败） |
| 3 | `Internal error` |
| 4 | `Unsupported cmd` |
| 5 | `Property error`（字段缺失/类型错/取值非法） |
| 6 | `Busy`（内存池/请求队列满，稍后重试） |

---

## 2. 模块与命令总表

模块号：SYSTEM=0、WIFI=1、DATA_FLOW=2、RESERVED=3、UART=4、OTA=5。`protocol_version = 1`。

### 2.1 SYSTEM(0)

| cmd | 名称 | 请求 | 响应 | 说明 |
|---|---|---|---|---|
| 1 | GET_FM_INFO | – | `{"fm_ver":"v1.0.0","upd_date":"YYYY-MM-DD","protocol_version":1}` | 固件版本 + 协议版本；`upd_date` 由构建注入 |
| 2 | REBOOT | – | 回显请求 | 2 秒后重启 |
| 3 | GET_SYS_INFO | – | `{"sn":"XXXXXXXXXXXX","hw_ver":"...","mf_date":"YYYY-MM-DD"}` | SN 默认 = WiFi STA MAC 无分隔大写 12 hex；hw_ver/mf 由构建注入，可留空字符串 |
| 200 | DO_CRASH | – | 不返回 | **仅 debug 构建注册**，release 不实现 |

### 2.2 WIFI(1)

| cmd | 名称 | 关键请求字段 | 响应字段 | 说明 |
|---|---|---|---|---|
| 1 | GET_STA_INFO | – | `ssid,password,ip,gateway,netmask,dns_main,dns_backup,rssi,mac` | 沿用 C/D 现有 |
| 2 | CONNECT | `ssid,password[,save]` | 成功时复用 GET_STA_INFO 字段 | `save` 缺省跟随 `auto_save` 配置（默认 true）；`auto_save=false` 时缺省不保存；显式 `save=1` 仍强制保存（见 wifi-credential-memory.md） |
| 3 | SCAN | – | `scan_list:[{rssi,ssid,mac}]` | 异步；结果回一条 JSON |
| 4 | DISCONNECT | – | 回显 | 停止 STA 连接（AP 保持） |
| 5 | GET_AP_INFO | – | `ssid,password,ip,gateway,netmask` | ⚠️ 含 AP 口令（默认无鉴权下仅在可信 LAN 使用） |
| 6 | GET_MODE | – | `{"status":"<mode>","mode":<int>}` | 沿用 C 的 `wifi_apsta_mode_e` |
| 7 | SET_MODE | `mode` | 回显 | 含 Smart APSTA 模式 |
| 8 | SET_AP_CRED | `ssid,password` | 回显 | 空 ssid 表示恢复默认 `WiFi-Dap-XXXX` |
| 9 | GET_STATIC_IP | – | `{"enable":bool,"ip":"","gateway":"","netmask":"","dns_main":"","dns_backup":""}` | – |
| 10 | SET_STATIC_IP | `enable` + 上述字段 | 回显 | 默认 enable=false（DHCP） |
| **11** | **GET_SAVED_LIST** | – | `{"auto_save":bool,"count":<int>,"max":<int>,"list":[{"idx":0,"ssid":"...","last_success":<unix>,"priority":0}]}`（count/max 为增量可选） | 多 WiFi 记忆 |
| **12** | **ADD_SAVED** | `ssid,password[,priority]` | `{"idx":<int>}`（**不回显 password**，安全例外） | 已存在则更新口令/时间；前端据此只更新列表不展示口令 |
| **13** | **DEL_SAVED** | `idx` | 回显 | – |
| **14** | **SET_AUTO_SAVE** | `enable` | 回显 | 默认 true |

AP SSID 默认规则：`WiFi-Dap-XXXX`，`XXXX` = `esp_read_mac(ESP_MAC_WIFI_STA)` 末 2 字节的 4 位大写十六进制；NVS 用户自定义优先。

### 2.3 DATA_FLOW(2)（最小实现，D 的 Uart/Widget 仅依赖 attach）

| cmd | 名称 | 请求 | 响应 |
|---|---|---|---|
| 1 | GET_INS_LIST | – | `{"instances":[{"ins_idx":0,"mod_idx":0,"mod_type":130,"port_info":{"periph_num":0}}]}`，`mod_type=130`=UART |
| 2 | GET_CUR_INS | – | `{"ins_idx":0}` |
| 3 | GET_CUR_ATTACH_LIST | – | `{"attaches":[...]}` |
| 4 | GET_ATTACH_LIST | – | `{"attaches":[...]}` |
| 5 | ATTACH | `ins_idx, s_ins_idx, r_ins_idx` | 回显 |
| 6 | ATTACH_CUR_TO_RECVER | `{data_type}` | 回显 |
| 7 | ATTACH_CUR_TO_SENDER | `{data_type}` | 回显 |
| 8 | DETACH_SINGLE | `attach_idx` | 回显 |
| 9 | DETACH_CUR_FROM | `attach_idx` | 回显 |
| 10 | SET_DATA_TYPE | `ins_idx,data_type` | 回显 |

> 实现范围：实例 = 1 个 UART 实例（`periph_num` 为 UART 号）；`data_type` 使用二进制头中的 `RAW=0x20`；Web UI 的 attach 流程必须返回成功。

### 2.4 UART(4)

| cmd | 名称 | 请求 | 响应 |
|---|---|---|---|
| 1 | GET_AVAILABLE_NUMS | – | `{"num":1}` |
| 4 / 5 | GET_BAUD / SET_BAUD | `sub_mod, baud` | `{"baud":115200}` |
| 6 / 7 | GET_CONFIG / SET_CONFIG | `sub_mod, data_bits(5-8), parity(0-2), stop_bits(1/15/2)` | 同上 |
| 8 / 9 | GET_FLOW_CTRL / SET_FLOW_CTRL | `sub_mod, flow_ctrl` | `{"flow_ctrl":0}` |
| 10 / 11 | GET_PINS_NUM / SET_PINS_NUM | `sub_mod[,tx_pin,rx_pin]` | 未实现时回显 + `err:"unsupported"`（前端不阻塞） |
| 12 / 13 | GET_MODE / SET_MODE | `sub_mod[,mode]` | 未实现时同上 |
| 20 / 21 | GET_STATUS / SET_STATUS | `sub_mod[,enable]` | `{"enable":true}` |
| 22 / 23 | GET_DATA_TYPE / SET_DATA_TYPE | `sub_mod[,data_type]` | `{"data_type":32}`（RAW） |
| 24 | GET_DEFAULT_NUM | – | `{"num":0}` |

数据面：见 §3；UART 默认 115200-8N1，`sub_mod` = UART 序号（当前固定 0）。

### 2.5 OTA(5)（浏览器本地上传方案；详见 docs/ota-local-upload.md）

| cmd | 名称 | 请求 | 响应 |
|---|---|---|---|
| 1 | GET_INFO | – | `{"running":"ota_0","running_ver":"v1.0.0","free":<bytes>,"slot_size":<bytes>,"status":"IDLE"}` |
| 3 | GET_PROGRESS | – | `{"offset":<u32>,"total":<u32>,"status":"IDLE|RECEIVING|VERIFYING|READY|APPLYING|ERROR","err":""}` |
| 4 | ABORT | – | 回显；收尾 `esp_ota_abort` 并回到 IDLE |

上传端点：`POST /api/ota/upload`（见 OTA 契约文档）。

> ⚠️ 校验说明：设备通过 **HTTP 明文** 提供页面，浏览器处于非 secure context，`crypto.subtle`（SubtleCrypto）不可用 → 前端**不能**依赖浏览器计算 SHA-256。`X-SHA256` 头为**可选**（仅当客户端环境支持时）；设备在 `GET_INFO`/`GET_PROGRESS` 响应中回传已写入镜像的 sha256，供用户人工比对。镜像完整性以 `esp_ota_end()` 校验为准。


---


### 2.6 TARGET(6)（Web 端直烧 STM32，HTTP 接口）

浏览器上传 `.bin`，ESP32 固件通过 SWD 直接烧录目标 STM32（F4/H7），流式写入、无需 staging 分区。**module=6** 只用于 JSON 信封；实际传输为 HTTP，不走 WebSocket。详细流程/算法见 `docs/stm32-web-flash-plan.md`。

| 方法/路径 | 请求 | 响应 |
|---|---|---|
| `GET /api/target/info` | – | `{module:6,cmd:1,ok,detected,family,series,device,supported,swd_clock_hz,dpidr,cpuid,idcode,uid,flash_base,flash_size,max_image_size}`；未接目标时 `ok:false,code:5`（HTTP 200）；已识别但不可烧录的系列 `detected=true,supported=false` |
| `POST /api/target/flash` | Header `Content-Type: application/octet-stream`、`Content-Length`、`X-Target-SHA256`（64 hex）；Body 为镜像 | 成功 `{module:6,cmd:2,ok,family,idcode,uid,written,verified,sha256,state,message}`；错误 `{ok:false,err,code}` |
| `GET /api/target/status` | – | `{module:6,cmd:3,ok,state,total,written,verified,percent,err,message,busy}` |
| `POST /api/target/reset` | `{"run":true}`（可选） | `{module:6,cmd:4,ok,run}` |

- 支持目标：**识别全部 STM32 系列**（F0/F1/F2/F3/F4/F7/G0/G4/H5/H7/L0/L1/L4/L5/U5/WB/WL/C0 等，DEV_ID 表 + DBGMCU 候选基址 + SWD 2M/1M/500k 回退），`series`/`device` 给出系列与候选型号组；**烧录**目前仅 F4/H7（`supported=true`），其余识别后 `supported=false`。H7（H72x/H73x 1MB）已真机识别验证。
- 错误：`400/411/415` 请求头/类型错误，`409` 忙/单会话冲突，`413` 超过目标 flash 大小，`422` 探测失败/SHA 或回读校验失败，`408` 上传超时，`500/503` 内部/超时；JSON `code` 沿用 2/3/5/6。
- 烧录/读取期间**暂停无线串口**（UART TCP 桥 1346、UART 转发/广播），**但网络/WiFi/AP/HTTP/WS/USBIP 保持可用**，结束后自动恢复。
- 烧录期间 DAP 被独占，不能同时用 OpenOCD/USBIP 访问探针；完成后恢复。
- Host/Origin 校验始终启用；无登录/鉴权（实验室内网）。

## 3. 二进制帧协议（UART/数据面）

4 字节头（`wt_bin_data_hdr_t`）：

| offset | 字段 | 值 |
|---|---|---|
| 0 | `data_type` | `EVENT=0x02`、`RAW_BROADCAST=0x04`、`RAW=0x20`、`CMD=0x21`、`RESPONSE=0x22` |
| 1 | `module` | 模块号（UART=4 等） |
| 2 | `sub_mod` | 子实例（UART 序号） |
| 3 | reserved | 0 |

方向与语义：

- **client → device**：`RAW`（module=4, sub_mod=uart_idx）→ 写入 UART TX；`CMD` 保留（控制命令走 JSON）；其他 `data_type` 返回错误帧。
- **device → client**：UART RX → `RAW_BROADCAST`（module=4, sub_mod=uart_idx）广播给所有 WS 客户端；`EVENT` 保留给设备事件（JSON 优先）。
- 分片：入站 WS 二进制帧只在 **FIN=1** 时交给 router；不带分片聚合；单帧超限关闭/丢弃并回 `MSG_BINARY_ERROR`。
- 背压：UART RX 队列满时丢弃最旧数据并累加 `overflow` 计数（不阻塞 httpd）；`GET STATUS` 可返回该计数（可选）。
- 路由 API 见 `docs/ws-binary-router.md`；实现归属：WP-204（`components/dataflow/**` + `uri_bin_router.c`），挂接点 WP-123（`uri_ws.c`）。

---

## 4. 兼容性与版本

- 本协议在 D 的现有 JSON 命令（module 0/1/2/4/5 语义）基础上扩展：SYSTEM cmd 3、WIFI cmd 11–14、OTA 上传方式改为浏览器上传（原 D 的 URL 更新不用）、TARGET(6) Web 直烧 HTTP 接口。
- `protocol_version` 握手：WS 连接后前端发 `SYSTEM/GET_FM_INFO`，比较响应中的 `protocol_version` 与本端常量；不匹配则用 toast 提示“固件与网页版本不匹配，请刷新/升级”，但不阻塞使用。
- 前端请求字段名保持 D 的驼峰/下划线约定（以 D 的 `src/api/*.ts` 为准）；后端响应用 C 的下划线字段名。

---

## 5. 安全默认

- 无登录/鉴权（实验室模式）；`Host`/`Origin` 校验始终启用（浏览器跨源防护）；OTA 上传和 WiFi 口令读取仅在可信内网使用。
- WS-DAP(3240) 与 UART TCP 桥(1346) 默认开启，但各自 Kconfig 可关；release 文档必须提示“仅限可信局域网”。

*契约版本：v1，2026-10-20，Lead 维护。*
