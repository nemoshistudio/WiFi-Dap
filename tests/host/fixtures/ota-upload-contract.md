> **已归档（2026-10-26，final slim）**：浏览器本地 OTA / Update 页 / `POST /api/ota/upload` /
> OTA JSON module 5 已从默认构建移除（A1/A2）；本文仅作历史契约与恢复参考。
> 当前固件更新方式见 [README_CN.md §4.5/§6](../../../README_CN.md)（USB-JTAG / OpenOCD / UART0 下载）；
> 恢复见 docs/feature-flags.md。

# WiFi Dap：本地文件 OTA 上传契约（v1，冻结草案）

> 状态：**冻结草案 v1（2026-10-20，Wave 0，任务 WP-001b-2 / 对应实现 WP-205 / WP-206 / WP-207）**。
> 变更流程：任何修改需 Lead 统一编辑并广播；Wave 1/2 的固件与前端实现以本文为准。
> 相关契约：`docs/protocol.md`（JSON/WS 总协议、模块 5 命令表、错误码）、`docs/versioning.md`（版本号/握手）、`docs/i18n-keys.md`（前端文案 key）、`docs/ws-binary-router.md`（二进制路由，OTA 不使用）。
> 决策依据：`analysis/INTEGRATION-PLAN.md` v4 §8.1（固件侧实现浏览器本地上传，不做服务端/URL 更新/签名服务）、`analysis/PARALLEL-DEV-PLAN.md`（WP-142/205/206/207）。
>
> **精简构建（slim/fw，2026-10-26，A1/A2）**：本文描述**完整功能构建**的 OTA 契约。
> 精简构建默认移除 `POST /api/ota/upload` 端点（`CONFIG_BIRDIEBOT_WEB_OTA_UPLOAD_ENABLE=n`）
> 与 OTA module 5 的 JSON 查询（`CONFIG_BIRDIEBOT_OTA_ENABLE=n`）；OTA 双槽分区、
> `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 与 main 的 `wt_ota_mark_app_valid()`
> 回滚确认**保留**，回滚机制仍按 §2.3 工作。恢复方式/资源收益见
> feature-flags.md A1/A2。

---

## 0. 范围与非目标

### 0.1 本期实现

| 项 | 内容 |
|---|---|
| 上传方式 | Web UI 选择本地固件文件 → 浏览器 `POST /api/ota/upload` 流式上传 → 固件写入**空闲 OTA 槽** → 校验 → 设置下次启动分区 → 重启 |
| 分区 | ESP32-S3 8MB：`ota_0` + `ota_1` + `otadata`，双槽预留（§1） |
| 首烧 | **本期产线与首次烧录只写 `ota_0`**，`otadata` 留空；bootloader 回退扫描 `ota_0` 启动 |
| 回滚 | 默认 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`：升级镜像首启成功后由应用 `esp_ota_mark_app_valid_cancel_rollback()` 确认；未确认即复位则回滚（§2.3） |
| 命令 | OTA(5)：`GET_INFO=1`、`GET_PROGRESS=3`、`ABORT=4`（见 `docs/protocol.md` §2.5） |
| 校验 | 固件 `esp_ota_end()` 镜像校验（必选）+ 可选 `X-SHA256` 全文件 SHA-256 对比（§6） |
| 登录/鉴权 | 已移除（无 token）；`Host`/`Origin` 校验始终启用（§8） |

### 0.2 明确不做（本期）

- ❌ 服务端主动下载/URL 更新（`WT_OTA_DO_URL_UPDATE` 废弃）、manifest 服务、签名服务器、`3241` corsacOTA。
- ❌ 差分升级、压缩镜像、镜像加密/签名（secure boot 若后续启用，由 `esp_ota_end()` 自然校验签名）。
- ❌ 多客户端并发上传、断点续传（中断后只能重传整个文件）。
- ❌ Web UI 在 HTTP 明文环境下用 `SubtleCrypto` 强制算哈希（见 §6.3 的安全上下文限制，只能作为可选降级）。

---

## 1. 分区要求（8MB 双槽，WP-142 实现）

### 1.1 布局（`partitions/partitions_8mb_ota.csv`，S3 8MB 默认）

| Name | Type | SubType | Offset | Size | 说明 |
|---|---|---|---|---|---|
| `nvs` | data | nvs | `0x009000` | `0x006000` (24K) | 默认 NVS（WiFi/OTA 元数据可放这里） |
| `phy_init` | data | phy | `0x00F000` | `0x001000` (4K) | IDF PHY 校准数据 |
| `otadata` | data | ota | `0x010000` | `0x002000` (8K) | **必需**，2 个 sector；记录启动槽/回滚状态 |
| `ota_0` | app | ota_0 | `0x020000` | `0x300000` (3M) | 首次烧录槽（本期只烧这里） |
| `ota_1` | app | ota_1 | `0x320000` | `0x300000` (3M) | OTA 目标槽 |
| `wt_nvs` | data | nvs | `0x620000` | `0x010000` (64K) | 应用 NVS（沿用 C 的 `wt_nvs` 分区名） |
| `coredump` | data | coredump | `0x630000` | `0x010000` (64K) | 仅 debug 构建启用写入 |
| *（reserved）* | — | — | `0x640000`~`0x800000` | ~1.75M | 不分配，后续扩展 |

CSV 建议（WP-142 落地时逐字使用）：

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xF000,   0x1000,
otadata,  data, ota,     0x10000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x300000,
ota_1,    app,  ota_1,   0x320000, 0x300000,
wt_nvs,   data, nvs,     0x620000, 0x10000,
coredump, data, coredump,0x630000, 0x10000,
```

### 1.2 约束（验收项）

1. `sdkconfig`：`CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`、`CONFIG_PARTITION_TABLE_CUSTOM=y`、`CONFIG_PARTITION_TABLE_FILENAME="partitions/partitions_8mb_ota.csv"`、`CONFIG_PARTITION_TABLE_OFFSET=0x8000`（IDF 默认，bootloader 必须 < 32KB；S3/ C3/ESP32 均满足）。
2. app 分区必须 64KB 对齐（`0x20000`/`0x320000` 满足）；`ota_0` 与 `ota_1` **大小必须完全相等**（`esp_ota_get_next_update_partition()` 与回滚要求）。
3. `otadata` 大小必须 ≥ 2 个 flash sector（0x2000）；缺失时 `esp_ota_set_boot_partition()` 返回 `ESP_ERR_NOT_FOUND`（这正是 C 当前分区表的缺陷，本项目必须修正）。
4. 验收：`gen_esp32part.py partitions/partitions_8mb_ota.csv` 无错误；镜像 ≤ 3MB（WP-142 加 `scripts/check_partitions.py` 检查 app 大小余量 ≥ 128KB）。
5. 4MB 板降级选项：`partitions/4mb_single.csv`（`nvs` 24K + `phy_init` 4K + `factory` app + `wt_nvs` 64K + `coredump` 64K），**无 OTA**，仅保证编译/启动；文档标注“4MB 不支持 OTA”。

### 1.3 首次烧录（本期只写 ota_0）

- 烧录包 = `bootloader.bin @0x0` + `partition-table.bin @0x8000` + `birdiebot_wifi_dap.bin @0x20000`；**不要写 `otadata`**（保持 `0xFF`）。
- bootloader 行为（IDF 5.5.6 已核实）：`otadata` 两个 sector 均无效且**无 factory 分区**时，回退按 OTA 槽正向扫描，启动 `ota_0`，并把 `ota_0` 写成合法启动项；因此首烧 `ota_0` 可正常启动，无需 factory 槽。
- 首次启动成功后 `ota_0` 状态为 VALID（rollback 模式下也不会进入 PENDING_VERIFY）；之后的升级才会产生 PENDING_VERIFY（§2.3）。

示例（脚本会加构建锁，具体由 WP-143 实现）：

```bash
esptool.py --chip esp32s3 merge_bin -o build/birdiebot_wifi_dap_full.bin \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x20000 build/birdiebot_wifi_dap.bin
esptool.py --chip esp32s3 -p <PORT> write_flash 0x0 build/birdiebot_wifi_dap_full.bin
```

---

## 2. 槽位选择与回滚语义

### 2.1 目标槽选择

- 上传开始时：`target = esp_ota_get_next_update_partition(NULL)`，即**非当前运行槽**；`running` 槽名称（`ota_0`/`ota_1`）与 `target` 通过 `esp_ota_get_running_partition()` / `esp_ota_get_partition_description()` 获取。
- 运行 `ota_0` 时上传写 `ota_1`；升级重启后运行 `ota_1`，下一次上传写回 `ota_0`。两槽始终互为备份。
- 槽大小 3MB 同时用于 `GET_INFO.slot_size` / `free`，也是上传上限（§3.3）。

### 2.2 写入期间运行槽不变

`esp_ota_begin()` 只操作目标槽，运行中的镜像不受影响；上传失败/中止/断电都**不会**破坏当前系统，下次开机仍运行原槽。

### 2.3 回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）

| 阶段 | bootloader/应用行为 |
|---|---|
| 首次烧录 `ota_0`（otadata 空） | bootloader 回退扫描启动 `ota_0`，`set_actual_ota_seq` 写入合法 otadata，状态 VALID |
| OTA 写完后 `esp_ota_set_boot_partition(target)` | 写 otadata：目标槽状态 **NEW**，下次启动走新槽 |
| 新镜像首次启动（rollback=y） | bootloader 启动新槽并标记 **PENDING_VERIFY** |
| 应用确认 | 应用在系统服务就绪且稳定运行后调用 `esp_ota_mark_app_valid_cancel_rollback()` → VALID |
| 未确认即复位/崩溃 | 下次 bootloader 看到 PENDING_VERIFY → 标记 ABORTED 并**回滚到旧槽**（旧版本恢复运行） |

**契约要求（WP-205）**：

1. 默认 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（对齐 INTEGRATION-PLAN v4 §8.1）。
2. 应用必须调用 `esp_ota_get_state_partition()` 检查自身是否 `ESP_OTA_IMG_PENDING_VERIFY`；若是，在 `app_main` 完成基础初始化（WiFi 启动、`start_webserver()`、DAP/UART 任务创建）后启动 **10s 单次定时器**，到期调用 `esp_ota_mark_app_valid_cancel_rollback()`。10s 内崩溃/复位 → 自动回滚。
3. 初始化致命错误（如 NVS/分区异常）时可选调用 `esp_ota_mark_app_invalid_rollback_and_reboot()`，让本次升级显式回滚。
4. 若实机调试期需要关闭回滚：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n` 也可工作（新镜像直接启动、无自动回滚），但必须在 `docs/versioning.md`/README 明确当前构建使用哪种模式；**默认发布构建必须 y**。
5. 回滚后 `GET_INFO` 应显示回滚后实际运行的槽与版本，`last_error`（可选字段）保留上次升级失败的诊断信息（RAM 级即可，重启后丢失）。

---

## 3. HTTP 上传端点 `POST /api/ota/upload`

### 3.1 请求

| 项 | 约定 |
|---|---|
| Method / Path | `POST /api/ota/upload`（80 端口，`esp_http_server`；**不**开 3241） |
| `Content-Type` | 必须为 `application/octet-stream`（严格匹配前缀；否则 `415`） |
| `Content-Length` | **必须携带**（十进制字节数）；缺失/非法 → `411`；不支持 `Transfer-Encoding: chunked`（收到即 `411`） |
| Body | ESP-IDF **应用镜像**（`build/birdiebot_wifi_dap.bin`，从镜像头 `0xE9` 开始）；**不接受** merge_bin 全量包（含 bootloader/分区表） |
| `X-SHA256` | 可选；64 位十六进制（大小写均可），整个文件 SHA-256；非法格式 → `400` |
| 大小 | `4096 ≤ Content-Length ≤ slot_size`（3MB = `0x300000`）；越界 → `400`/`413` |
| 并发 | **单会话**：已有会话（RECEIVING/VERIFYING/READY/APPLYING）→ `409`；IDLE/ERROR 可开始新会话 |

### 3.2 处理模型（实现约束，WP-206）

1. 不能把 3MB 一次性缓存/整包读入：handler 校验头 → 释放 DAP ring（第 6 条）→
   创建 **OTA 专用任务**（S3 5,120 B 栈，堆不足时降级 4,096 B）→ 在 httpd 任务里
   用静态双缓冲分块 `httpd_req_recv()`，chunk 经静态队列交给 OTA 任务做
   flash erase/write。**收完整 body 之后**才调用
   `httpd_req_async_handler_begin()`（只延期响应），随后投递 EOS；OTA 任务在
   EOS 之后才取 `async_req` 发最终 JSON 并按需重启（原因见第 7 条）。
   上传期间 httpd 任务被 body 接收占用，其它 HTTP/WS 请求只能在上传分块间隙
   被服务（浏览器进度条由 XHR upload 事件驱动，不受影响）。
2. 接收缓冲用**静态分配**（`BIRDIEBOT_OTA_BUF_SIZE = 4096`），不占用 `memory_pool` 的 2048B 请求缓冲，也不做 3MB 动态分配。
3. 全局状态由 `wt_ota` 组件用一个 mutex 保护；会话结构体只允许 1 个实例。
4. `httpd_config.recv_wait_timeout` 需 ≥ 10s（或 OTA 线程用 socket 超时单独控制），确保慢网络下不误判超时。
5. 上传期间**不暂停** DAP/USBIP（3240）与 UART 桥；但 flash 擦写会短暂拉高延迟，契约不承诺上传期间的 DAP 时序。
6. **task-66b 内存交接（2026-10-25 补充）**：统一 Wi-Fi+USB-NCM 镜像在
   task-66 之后让两个 DAP ring buffer（20,480 + 20,672 B）常驻，实测
   `heap_largest=4,608 B`，低于 OTA 专用任务的 5,120 B 栈需求，导致本端点曾
   一律返回 503 `ota service unavailable`。现在 handler 在创建 OTA 任务 /
   `xTaskCreate()` **之前**调用
   `dap_handle_release_ringbuffers()` 显式归还 ~41 KiB（`DAP_handle.h` 的
   `RELEASE_HANDLE`），上传失败（无重启）时调用
   `dap_handle_restore_ringbuffers()` 恢复；上传成功则原本就会重启，启动时由
   DAP 任务重新创建 ring。因此**上传窗口内 DAP/USBIP 数据路径不可用**
   （`GET /api/target/status` 的 `dap_ring_active=false`），这改变了上面第 5 条
   的“不暂停 DAP”约定：OTA 期间不得同时进行 OpenOCD/USBIP 会话。
7. **IDF v5.5.6 async hand-off 顺序（2026-10-25 真机回归，S3 `75ef0b7` /
   C5 `807ffd8`）**：ESP-IDF v5.5.6 的
   `httpd_req_async_handler_begin()` 在成功尾部会把**原请求**的
   `aux->remaining_len` 清零（注释 "prevent the main thread from reading the
   rest of the request"）。若先 begin、再在 httpd 任务里 `httpd_req_recv()`，
   recv 会立即返回 0：transport 判成 `client disconnected`、不发响应，未消费
   的 body 随后被 httpd 当作“下一个请求”解析，客户端看到 HTTP 400
   `Bad request syntax`（1.7 MiB 镜像在 host 的 64 KiB socket buffer 处停住，
   ~3.8%）。IDF v5.2.1 没有该清零行为，所以旧顺序在升级前可用。
   修复顺序即上面第 1 条：**先收完整 body，再 async begin，EOS 后 OTA 任务取
   `async_req`**；同一根因在 C5 真机先定位（C5 用 3,584 B 栈），S3 移植后
   5,120/4,096 B 栈与 ring release/restore 保持不变。
   真机证据与诊断字段见 `docs/idf-5.5.6-upgrade.md` §5.4；transport 的
   `stage/body_*/task_stack/heap_*/last_error` 快照通过
   `GET /api/target/status` 的 `ota_upload_*` 字段读取
   （`project_components/web_server/include/ota_upload_diag.h`，只读诊断，
   不改变模块 5 协议/状态机）。

### 3.3 超时、中止与断连

| 场景 | 行为 |
|---|---|
| 空闲超时 | 连续 **10s** 未收到任何 body 字节 → `esp_ota_abort()`，状态 ERROR（`err="upload timeout"`），HTTP 返回 `408`（或连接已被关闭时仅记录） |
| 会话总时长 | 自 `esp_ota_begin()` 起 **300s** 上限，超时同上（防止慢速耗死设备） |
| 客户端断连/`AbortController` | `httpd_req_recv()` ≤0 → `esp_ota_abort()`，状态 ERROR（`err="client disconnected"`），不再回复 |
| WS `ABORT=4` | 仅 RECEIVING 状态有效 → `esp_ota_abort()`，状态回到 IDLE；其它状态返回 `code=5 err="invalid state"` |
| 写入错误 | `esp_ota_write` 失败 → 立即 abort → ERROR，HTTP `500`（若连接仍可写） |

超时/断连后目标槽内容不完整但**不影响启动**；下一次上传 `esp_ota_begin()` 会重新擦除目标槽。ERROR 状态保留最后一次 `offset/total/err` 供诊断，下一次上传开始时自动清为 IDLE。

> task-66b：上传失败/中止（无重启）后，handler/OTA 任务会调用
> `dap_handle_restore_ringbuffers()` 把 OTA 开始时释放的 DAP ring 重建回来，
> USBIP/OpenOCD 数据路径随即恢复（`GET /api/target/status` 的
> `dap_ring_active` 回到 `true`）。若重建因碎片堆失败（日志
> `DAP ring buffer restore failed`），DAP 数据路径在本轮启动内不可用，
> 重启设备即可恢复；不影响 OTA 槽状态与下一次上传。

### 3.4 成功与错误响应（HTTP + JSON）

HTTP 响应 `Content-Type: application/json`。**成功响应必须先发出，再延迟 1000ms 重启**，否则浏览器收不到结果。

成功（HTTP 200）：

```json
{
  "module": 5,
  "cmd": 2,
  "ok": true,
  "slot": "ota_1",
  "running": "ota_0",
  "running_ver": "v1.0.0",
  "offset": 3145728,
  "total": 3145728,
  "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
  "status": "APPLYING",
  "reboot_in_ms": 1000
}
```

- `cmd=2` 仅用于**上传响应关联**（对应 D 旧 `WT_OTA_DO_UPDATE=2` 的语义，被 HTTP 上传取代）；WS 线路收到 module 5 / cmd 2 必须回 `code=4 Unsupported cmd`。
- `sha256` 为设备端实际计算值（未提供 `X-SHA256` 时也返回，便于用户/前端人工比对）。
- 客户端收到 200 后视为成功，可等待设备重启并重新连接；连接提前断开也按成功处理（以 `GET_INFO.running_ver` 变化为准）。

失败：

```json
{ "module": 5, "cmd": 2, "ok": false, "err": "image verification failed", "code": 3 }
```

| HTTP | `err` 示例 | JSON `code` | 触发条件 |
|---|---|---|---|
| 400 | `invalid content length` / `bad sha256 header` / `image too small` | 5 | 头/字段非法 |
| 403 | `forbidden origin` | 5 | Origin/Host 校验失败（始终启用） |
| 409 | `ota session busy` | 6 | 已有会话/ABORT 状态不允许 |
| 411 | `content-length required` | 5 | 缺 Content-Length 或 chunked |
| 413 | `image too large` | 5 | 超过槽大小 |
| 415 | `unsupported content type` | 5 | 非 octet-stream |
| 422 | `image verification failed` / `sha256 mismatch` | 5 | `esp_ota_end()`/SHA-256 失败（目标槽不切换） |
| 408 | `upload timeout` | 6 | 空闲/总时长超时 |
| 500 | `ota begin failed` / `ota write failed` / `set boot partition failed` | 3 | 固件内部错误 |
| 503 | `ota service unavailable` | 6 | OTA 任务/内存不可用（task-66b 后：handler 已先释放 DAP ring；仍 503 说明 5,120 B 任务栈/async 请求仍分配失败，检查 `GET /api/target/status` 的 `heap_free/heap_largest`） |
| 503 | `out of memory` | 6 | `wt_ota_upload_begin()` 阶段堆不足（映射自 `ESP_ERR_NO_MEM`） |

> JSON `code` 使用 `docs/protocol.md` §1.1 的 2–6（鉴权已移除，旧 code 7 保留为保留值但 OTA 不再返回）。HTTP 状态码承担细分语义。`err` 字符串为英文、给开发者/日志使用，不参与 i18n。

---

## 4. WS 模块 5 命令（OTA）

统一走 `ws://<ip>/ws` 文本帧 JSON（与 `docs/protocol.md` §2.5 完全一致；本文补充字段语义与状态约束）。

### 4.1 `GET_INFO = 1`

请求：`{"module":5,"cmd":1}`

响应（`*` = protocol.md 冻结字段，其余为**允许的增量可选字段**）：

```json
{
  "module": 5, "cmd": 1,
  "running": "ota_0",          // * 当前运行槽：ota_0 / ota_1
  "running_ver": "v1.0.0",     // * 运行镜像版本（与 SYSTEM/GET_FM_INFO.fm_ver 同源，见 §9）
  "free": 3145728,             // * 空闲槽可写字节数 = slot_size（当前无文件系统占用）
  "slot_size": 3145728,        // * 每个 OTA 槽大小
  "status": "IDLE",            // * 见 §5
  "idle": "ota_1",             //  可选：下一上传目标槽
  "rollback": true,            //  可选：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  "last_error": ""             //  可选：上次失败 err（重启后清空）
}
```

### 4.2 `GET_PROGRESS = 3`

请求：`{"module":5,"cmd":3}`

响应（`*` 为冻结字段）：

```json
{
  "module": 5, "cmd": 3,
  "offset": 1048576,           // * 已写入字节数
  "total": 3145728,            // * 本次会话总字节数（IDLE 时为 0）
  "status": "RECEIVING",       // * 见 §5
  "err": "",                   // * 空串 = 无错误
  "slot": "ota_1",             //  可选：目标槽
  "sha256": ""                 //  可选：VERIFYING 完成后设备计算的 SHA-256
}
```

约束：前端轮询建议 **300–500ms**；IDLE 时返回 `offset=0,total=0,status=IDLE,err=""`；ERROR 时保留最后 `offset/total/err` 直到下次上传或 ABORT。

### 4.3 `ABORT = 4`

请求：`{"module":5,"cmd":4}`

- RECEIVING：`esp_ota_abort()` 后回显请求（`{"module":5,"cmd":4}`），状态 IDLE，已写数据丢弃。
- 其它状态：`{"module":5,"cmd":4,"err":"invalid state","code":5}`。
- 无会话时 `ABORT` 幂等返回 IDLE 亦可（实现二选一，需在 WP-206 注释说明；推荐回 `code=5 missing session` 以区分）。

---

## 5. 状态机（唯一权威定义）

```
                 POST /api/ota/upload（头校验通过）
        IDLE ────────────────────────────────► RECEIVING
          ▲                                        │ body 收完 / Content-Length 达成
          │ ABORT / 下次上传                        ▼
          │                                     VERIFYING
          │                                    │    │ esp_ota_end / SHA256 失败
          │                                    │    └──────────────► ERROR
          │                                    ▼ 成功
          │                                  READY（已 set_boot_partition）
          │                                    │
          │                                    ▼
          │                                  APPLYING（已回复 HTTP 200，1000ms 后 esp_restart）
          │
          └──────────────── ERROR（保留 offset/total/err；下次上传自动回 IDLE）
```

| 状态 | 含义 | `GET_INFO.status` / `GET_PROGRESS.status` | 允许操作 |
|---|---|---|---|
| `IDLE` | 无会话 | `IDLE` | POST 上传、GET_INFO、GET_PROGRESS |
| `RECEIVING` | HTTP body 接收中，`offset` 递增 | `RECEIVING` | GET_PROGRESS、ABORT、GET_INFO；第二个 POST → 409 |
| `VERIFYING` | body 收完，`esp_ota_end()` + SHA-256 校验 | `VERIFYING` | GET_PROGRESS/GET_INFO（ABORT 返回 5 invalid state） |
| `READY` | 镜像有效、`esp_ota_set_boot_partition()` 成功 | `READY` | GET_INFO/GET_PROGRESS（窗口极短，随后 APPLYING） |
| `APPLYING` | 已回复 200，等待重启 | `APPLYING` | GET_INFO/GET_PROGRESS（重启后回 IDLE） |
| `ERROR` | 失败，保留诊断 | `ERROR` | GET_INFO/GET_PROGRESS/下一次 POST；ABORT 回 IDLE |

- 状态仅存 RAM，重启后必为 IDLE。
- 状态转换由 OTA 任务串行执行，用 mutex 保护；HTTP handler / WS handler 只读写快照。
- `GET_PROGRESS` 是唯一进度来源；**不新增** WS 异步事件（保持 `docs/protocol.md` §1.1“OTA 走轮询”）。

---

## 6. 写入流程与校验

### 6.1 固件侧流程（WP-205 `components/wt_ota/**`）

```c
// 1) 选择目标槽
const esp_partition_t *running = esp_ota_get_running_partition();
const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);
// 2) 开始：已知 Content-Length 可传 image_size 以优化擦除；否则 OTA_SIZE_UNKNOWN
esp_ota_begin(target, image_size /* 或 OTA_SIZE_UNKNOWN */, &handle);
// 3) 接收循环（OTA 专用任务）
while ((n = httpd_req_recv(http_req, buf, sizeof(buf))) > 0) {
    if (sha256_enabled) mbedtls_sha256_update(...);   // 可选：增量计算
    esp_ota_write(handle, buf, n);                    // 失败 → abort/ERROR
    ota.offset += n;                                  // GET_PROGRESS 快照
}
// 4) 结束/校验（esp_ota_end 失败后 handle 不再可用，不要重复 abort）
esp_err_t err = esp_ota_end(handle);
if (err != ESP_OK) -> ERROR(err="image verification failed") -> HTTP 422
// 5) 可选 SHA-256 对比
if (x_sha256 && memcmp(digest, x_sha256)) -> ERROR(err="sha256 mismatch") -> HTTP 422（**不切换启动槽**）
// 6) 设置启动槽
esp_ota_set_boot_partition(target) != ESP_OK -> ERROR -> HTTP 500
// 7) READY → 发送 HTTP 200（§3.4）→ APPLYING → 延迟 1000ms esp_restart()
```

其它要求：

- `esp_ota_abort()` 只在 handle 仍有效（`esp_ota_begin()` 之后、`esp_ota_end()` 成功之前）调用。
- 上传成功后**不得**调用 `esp_ota_mark_app_valid_cancel_rollback()`（那是运行中镜像的确认，不是新镜像）。
- 回滚确认逻辑（§2.3）在 `app_main` 启动路径中实现，与 OTA 上传互斥：OTA 上传期间不允许重启确认流程干扰。
- 版本号字符串（`running_ver`）取 `esp_app_desc_t.version`，与 SYSTEM/GET_FM_INFO 同源，格式受 `docs/versioning.md` 约束。

### 6.2 镜像有效性（必选）

- `esp_ota_end()` 校验：镜像头 magic、段校验和、app descriptor；若后续启用 secure boot，还会校验签名。
- 上传内容必须是 **app 镜像**；误传 merge_bin 全量包通常会在 `esp_ota_end()` 阶段失败（返回 `422`），前端需提示“仅上传 `build/birdiebot_wifi_dap.bin`”。
- `esp_ota_begin()` 前检查 `Content-Length ≤ target->size`，否则直接 `413`（避免擦除后才发现越界）。

### 6.3 可选 SHA-256（`X-SHA256`）

1. 前端优先尝试浏览器 `crypto.subtle.digest("SHA-256", fileBytes)` 计算，随上传头 `X-SHA256` 发送；设备端增量计算并比对（大小写不敏感），不一致 → `422` 且**不切换启动槽**。
2. ⚠️ **安全上下文限制**：`crypto.subtle` 只在 secure context 可用；本设备 UI 通过 `http://<ip>` 提供、**不是** secure context（`localhost` 除外）。因此：
   - 前端必须 `if (window.isSecureContext && crypto.subtle)` 才走 SubtleCrypto；
   - 不可用时不发 `X-SHA256`（或让用户粘贴 `sha256sum` 结果），仍由 `esp_ota_end()` 保证镜像有效；
   - 设备端 `GET_PROGRESS.sha256` / 上传响应 `sha256` 始终返回，供用户与发布页哈希人工核对。
3. 设备端 SHA-256 使用 mbedtls（`mbedtls_sha256_init/update/finish`），可与接收循环共用，不额外分配大缓冲。
4. 失败处理：SHA 不一致时目标槽已写入但不切换启动；下一次上传会重新擦写，不需要额外清理。

---

## 7. 前端（D / `web/`）迁移契约（WP-207）

### 7.1 与 D 现有 `src/api/apiOTA.ts` 的映射

| D 现状（`WtOTACmd`） | 新契约 | 说明 |
|---|---|---|
| `WT_OTA_GET_UPDATE_INFO=1` → 字段 `total_size,ver` | module 5 `GET_INFO=1` → `slot_size/free/running/running_ver/status` | 字段名/语义改变，`IOTAFmInfo` 需重写 |
| `WT_OTA_DO_UPDATE=2`（WS 推送） | **删除**；改为 `POST /api/ota/upload` | HTTP 响应里 `cmd=2` 仅作关联 |
| `WT_OTA_GET_PROGRESS=3` → `progress,total_size,status` | `GET_PROGRESS=3` → `offset,total,status,err` | 字段重命名，状态枚举改变 |
| `WT_OTA_DO_URL_UPDATE=4`（URL 下载） | **删除并复用 `ABORT=4`** | 语义冲突，前端必须同步改 `WtOTACmd` 枚举 |
| `WtOTAProgressStatus = OK/IDLE/IN_PROGRESS/FAILED` | `IDLE/RECEIVING/VERIFYING/READY/APPLYING/ERROR` | i18n 映射见 §9.2 |

### 7.2 Update 页行为要求

1. 文件选择：`<input type="file" accept=".bin">`；展示文件名/大小；本地预检 `size ≤ 3MB` 且 `size ≥ 4096`；提示仅上传 app `.bin`。
2. 上传：`XMLHttpRequest`（或 `fetch`，但 body 必须是 `File`，由浏览器自动生成 `Content-Length`；**不得**用 `ReadableStream`/chunked，否则违反 §3.1）；`Content-Type: application/octet-stream`；`xhr.upload.onprogress` 显示浏览器上传进度。
3. 设备进度：上传同时每 400ms 轮询 WS `GET_PROGRESS`，以设备 `offset/total` 为准（浏览器上传 100% ≠ 设备写完）。
4. 中止：按钮 → WS `ABORT` + `xhr.abort()`；等待状态回 IDLE。
5. 完成：收到 HTTP 200 → 提示“升级完成，设备将在 1 秒后重启”；断线后每 3s 重连并 `GET_INFO` 校验 `running_ver` 已变化；未变化则提示失败并给出 `last_error`/设备日志建议。
6. `VITE_ENABLE_OTA=true`（v4 决策）；页面隐藏时仍可上传但应提示。
7. 页面必须给出“仅在可信局域网使用”的警告；无登录/鉴权（按需求移除）。

---

## 8. 安全边界（Host/Origin，无登录/鉴权）

| 模式 | 要求 |
|---|---|
| 所有模式 | 上传端点无 token；仍然校验 `Host`/`Origin`（浏览器跨源防护）、`Content-Type`/`Content-Length`/大小/SHA；文档/UI 显著提示仅可信 LAN |
| 浏览器来源 | 有 `Origin` 时必须同源或命中 `BIRDIEBOT_WEB_ALLOWED_ORIGIN`；违规 403 + code 5 |

威胁模型与说明（写入 README/WP-304 审查）：

- 无登录/鉴权 + 明文 HTTP：同 LAN 攻击者可用 `curl` 直接上传任意**有效 ESP-IDF 镜像**（固件不校验签名，除非启用 secure boot）→ 持久化控制。仅适用于可信实验室网络。
- 浏览器跨源 CSRF/CSWSH/DNS rebinding：由始终启用的 `Host`/`Origin` 校验防护；非浏览器客户端不受该限制（无鉴权）。
- 上传期间不响应重复上传（409），避免并发写 flash；`ABORT` 只能由当前会话客户端触发（无会话时无副作用）。
- 上传不校验镜像签名（本期不做签名服务）；secure boot 开启后 `esp_ota_end()` 会自动校验签名，届时上传非法签名镜像返回 422。

---

## 9. 与 versioning / i18n 契约的接口点

### 9.1 版本（`docs/versioning.md` 待冻结，本文只约定接口）

1. `GET_INFO.running_ver` 与 `SYSTEM/GET_FM_INFO.fm_ver` **必须同源**（`esp_app_desc_t.version`），格式 `vMAJOR.MINOR.PATCH`（如 `v1.0.0`），由 `docs/versioning.md` 冻结；浅克隆/无 tag 时的回退值也由该文档定义（INTEGRATION-PLAN v4 建议回退 `v1.0.0`）。
2. `protocol_version` 仍为 **1**；本契约只新增字段与 HTTP 端点，不升协议版本。前端在 `GET_FM_INFO.protocol_version != 1` 时提示版本失配（`docs/protocol.md` §4）。
3. OTA 成功后的版本可见性：重启后 `GET_FM_INFO.fm_ver` = 新镜像 `esp_app_desc.version`；前端不得自行拼接 OTA 文件名作为版本。
4. 发布/构建产生的 `version.txt`、`FW_UPD_DATE` 由 WP-141/143 注入；OTA 契约不要求前端展示 `upd_date`，但 `GET_INFO` 可增量返回 `running_ver` 对应的构建日期（可选）。

### 9.2 i18n（`docs/i18n-keys.md` 待冻结，本文只约定接口）

1. **设备返回的状态/错误均为稳定英文枚举或错误码**，不得本地化：`status ∈ {IDLE,RECEIVING,VERIFYING,READY,APPLYING,ERROR}`；`err` 为英文开发信息；HTTP 错误以状态码 + JSON `code` 区分。
2. 前端 i18n 按 `status` 枚举映射文案（建议 key 组 `update.status.*`，最终 key 名由 `docs/i18n-keys.md` 冻结）；按 HTTP 状态码/`code` 映射错误提示（建议 `update.error.*`）；未识别的枚举回退到通用文案并显示原始 `status`/`err`（便于调试）。
3. Update 页仅中英双语（INTEGRATION-PLAN v4 §8.3）；品牌串 `WiFi Dap` 不翻译。
4. 进度/大小数字本地化格式（MB/百分比）由前端 i18n 层处理，固件只给字节数。

---

## 10. 验收清单（WP-205 / WP-206 / WP-207）

- [ ] 分区表：`gen_esp32part.py` 校验通过；`ota_0/ota_1` 等大；`otadata` 8K；S3 8MB 配置；app 大小 ≤ 3MB-128K。
- [ ] 首烧只写 `ota_0` + bootloader + ptable（otadata 空），设备可启动并 `GET_INFO` 显示 `running=ota_0`、`status=IDLE`。
- [ ] 上传合法 app bin → 进度轮询 `RECEIVING` 递增 → `VERIFYING` → HTTP 200 → 重启后运行新槽、`running_ver` 变化；再传一次写回另一槽。
- [ ] `X-SHA256` 正确 → 200；错误 → 422 且重启后仍运行旧版本。
- [ ] 超过 3MB → 413；缺 Content-Length → 411；错 Content-Type → 415；重复上传 → 409；上传中 ABORT → 回 IDLE。
- [ ] 中途拔网/关闭浏览器 → 设备 ERROR（`client disconnected`）后可再次上传成功；设备不崩溃、当前运行版本不变。
- [ ] rollback=y：新镜像启动后 10s 内人为复位 → 回滚到旧槽（旧版本运行）；正常启动 10s 后确认，之后的普通重启仍在新版本。
- [ ] 无登录/鉴权：直接上传成功；非浏览器客户端同样可上传（仅限可信内网）。
- [ ] Update 页：本地文件选择、浏览器进度、设备进度、成功/失败提示、SHA 人工比对入口；HTTP 明文环境下不依赖 SubtleCrypto。
- [ ] host 测试（WP-210/302）：镜像大小/头校验、状态机转换、错误码映射（纯逻辑可单测）。

## 11. 遗留 / 待 Lead 决策

- 是否在 `GET_INFO` 增加 `rollback` 字段（本文列为可选）；建议保留，便于前端提示“已启用回滚”。
- OTA 期间是否暂停 WiFi 轮换/扫描（减少 flash/射频争用）：本文不强制；如需，以 WiFi 契约中的“暂停接口”实现，并在 `docs/wifi-credential-memory.md` §接口点登记。
- secure boot / flash encryption 是否在 v2 启用：启用后 `esp_ota_end()` 会校验签名，上传契约不变（仅错误提示需补）。
- `ABORT` 在无会话时的行为二选一（§4.3），WP-206 实现时定稿并在代码注释与本文件同步（Lead 广播）。

*契约版本：v1，2026-10-20，Wave 0（WP-001b-2）；实现归属：WP-205/206/207。*
