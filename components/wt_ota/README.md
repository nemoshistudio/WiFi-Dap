# components/wt_ota — OTA core (WP-205)

固件侧 OTA 核心，实现 `docs/ota-local-upload.md` 的会话状态机/写入/校验/回滚部分，
并自注册 JSON 模块 5（`docs/protocol.md` §2.5）。HTTP 上传端点
`POST /api/ota/upload` 属 **WP-206（web-dev，`project_components/web_server/uri_modules/uri_ota.c`）**，
本组件只提供线程安全的 C API。

> **Slim build (2026-10-26, A1/A2)**: on branch `slim/fw` the Web upload
> endpoint (`uri_ota.c`, `CONFIG_BIRDIEBOT_WEB_OTA_UPLOAD_ENABLE`) and the JSON
> module 5 registration (`CONFIG_BIRDIEBOT_OTA_ENABLE`) are both **off by
> default**; the state machine / C API in this component is kept (rollback
> helpers are still required by `main/`), and the unused upload code is removed
> by `--gc-sections`.  Restore recipe: `docs/feature-flags.md` A1/A2.

## 状态机（docs/ota-local-upload.md §5）

```text
IDLE --begin--> RECEIVING --finish--> VERIFYING --esp_ota_end+sha--> READY --apply--> APPLYING --reboot--> IDLE
  ^                 |                     |                             |
  |                 +--abort(4)----------->|                             |
  +--- ERROR <------+-- write/verify/timeout errors --------------------+
```

| 状态 | 触发 | GET_INFO/GET_PROGRESS `status` |
|---|---|---|
| `IDLE` | 无会话 / 客户端 ABORT 后 | `IDLE` |
| `RECEIVING` | `wt_ota_upload_begin()` 成功 | `RECEIVING` |
| `VERIFYING` | `wt_ota_upload_finish()` 内 `esp_ota_end()` 期间 | `VERIFYING` |
| `READY` | 校验 + `esp_ota_set_boot_partition()` 成功 | `READY` |
| `APPLYING` | `wt_ota_apply_and_reboot()`（HTTP 200 之后 1s 重启） | `APPLYING` |
| `ERROR` | 写入/校验/SHA/超时/断连失败 | `ERROR`（`err`/`last_error` 有值） |

- 目标槽：`esp_ota_get_next_update_partition(NULL)`（运行 `ota_0` 时写 `ota_1`，反之亦然）。
- 镜像有效性：`esp_ota_end()`；可选 `X-SHA256` 由本组件增量计算并比对，不匹配时**不切换启动槽**。
- 超时：`BIRDIEBOT_OTA_IDLE_TIMEOUT_MS`（默认 10s）/ `BIRDIEBOT_OTA_MAX_DURATION_MS`（默认 300s），
  在 `wt_ota_poll()`（GET_INFO/GET_PROGRESS 自动调用）里执行 `esp_ota_abort()` → ERROR。
- 4MB single-app 分区表（C3/ESP32）没有 OTA 槽：`begin` 返回 `ESP_ERR_NOT_SUPPORTED`，
  `GET_INFO` 的 `free/slot_size` 为 0，其余查询正常；仅做编译验收。

## C API（`include/wt_ota.h`，供 uri_ota.c 调用）

```c
esp_err_t wt_ota_upload_begin(uint32_t content_length);   /* 409 用 INVALID_STATE / 413 用 INVALID_SIZE */
esp_err_t wt_ota_upload_write(const void *data, size_t len);
esp_err_t wt_ota_upload_finish(const char *expected_sha256_hex); /* NULL = 不校验 X-SHA256 */
esp_err_t wt_ota_upload_abort(void);                      /* 客户端 ABORT；非 RECEIVING -> INVALID_STATE */
esp_err_t wt_ota_upload_fail(const char *reason);         /* 超时/断连 -> ERROR(reason) */
esp_err_t wt_ota_apply_and_reboot(uint32_t delay_ms);     /* READY -> APPLYING -> delay 后 esp_restart */

void wt_ota_get_info(wt_ota_info_t *info);                /* GET_INFO 快照 */
void wt_ota_get_progress(wt_ota_progress_t *progress);    /* GET_PROGRESS 快照 */
void wt_ota_poll(void);                                   /* 可选的超时检查（查询接口已自动调用） */
bool wt_ota_is_busy(void);                                /* 有活动会话 -> HTTP 409 */

bool wt_ota_rollback_enabled(void);
esp_err_t wt_ota_mark_app_valid(void);                    /* main/app 启动成功后调用（见下） */
esp_err_t wt_ota_mark_app_invalid(void);                  /* 致命初始化错误时显式回滚并重启 */
```

错误映射辅助（uri_ota.c 不应重复实现契约表）：
`wt_ota_err_to_http_status(err)` → 400/408/409/413/422/500/503；
`wt_ota_err_to_json_code(err)` → 3/5/6；`wt_ota_err_to_str(err)` → 英文诊断串。

### uri_ota.c 建议调用序列（WP-206）

```c
/* header 校验通过、httpd_req_async_handler_begin() 之后，在 OTA 任务里： */
err = wt_ota_upload_begin(content_length);          /* 失败 -> 409/413/503 等 */
while ((n = httpd_req_recv(req, buf, sizeof buf)) > 0) {
    if ((err = wt_ota_upload_write(buf, n)) != ESP_OK) break;  /* 失败 -> 500/413 */
}
if (err == ESP_OK) err = wt_ota_upload_finish(x_sha256_or_null); /* 422/500 */
if (err == ESP_OK) {
    /* 先回复 HTTP 200（rsp 字段见 upload 响应契约），再： */
    wt_ota_apply_and_reboot(1000);
} else {
    wt_ota_upload_fail(wt_ota_err_to_str(err));      /* 记录诊断（可选，ERROR 已由 core 设置） */
}
```

- 接收块大小用 `CONFIG_BIRDIEBOT_OTA_BUF_SIZE`（默认 4096）的**静态**缓冲；
  整个调用序列应在 OTA 专用任务里串行执行（契约 §3.2），不要阻塞 httpd 任务。
- `esp_ota_begin()` 内部擦除目标槽会在 `wt_ota_upload_begin()` 中阻塞数百 ms 到数秒；
  该函数已把状态先发布为 RECEIVING（offset=0），因此 GET_PROGRESS 仍可读取。
- 上传响应 JSON 所需字段：`running`=`info.running`、`running_ver`=`info.running_ver`、
  `slot`=`progress.slot`、`offset/total/status`=`progress`、`sha256`=`progress.sha256`。

## JSON 模块 5（自注册）

`src/wt_ota_json.c` 用 `API_JSON_MODULE_REGISTER` 注册 `module_id=5`，现有 ws/api 路由
自动分发，无需改 web_server：

| cmd | 名称 | 响应字段 |
|---|---|---|
| 1 | GET_INFO | `running, running_ver, free, slot_size, status` + 可选 `idle, rollback, sha256, last_error` |
| 3 | GET_PROGRESS | `offset, total, status, err` + 可选 `slot, sha256` |
| 4 | ABORT | 成功回显 `{module,cmd}`；非 RECEIVING 返回失败体 `{"module":5,"cmd":4,"err":"invalid state","code":5}`；WS 上 cmd 2 → code 4 |

契约允许的增量可选字段（`docs/ota-local-upload.md` §4.1/§4.2）已使用，字段名不变。
模块 5 的失败响应统一为 `docs/protocol.md` §1.1 的失败体
`{"module":5,"cmd":<cmd>,"err":"<msg>","code":<int>}`（HTTP/WS 同一 JSON，前端按 `err` 解析），
不再经过 ws 路由层的旧 `{"error":...}` 模板；ABORT 非法状态即 code 5。
`BIRDIEBOT_OTA_ENABLE=n` 时只关闭模块 5 注册，C API 仍可用。

## 回滚（docs/ota-local-upload.md §2.3）

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（三目标默认已开启）时，OTA 新镜像首启为
  `ESP_OTA_IMG_PENDING_VERIFY`，必须由应用确认，否则下次复位回滚到旧槽。
- **集成点（已落地，`main/main.c`）**：`app_main()` 在 WiFi、`start_webserver()`、DAP/UART
  任务都启动后注册一个 **10s 单次 `esp_timer`**，回调调用 `wt_ota_mark_app_valid()`；函数只在
  `ESP_OTA_IMG_PENDING_VERIFY` 时确认，否则安全返回并记日志。该钩子由
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 编译开关保护，未确认的 OTA 镜像会在下次复位时回滚。
- 过渡方案：`CONFIG_BIRDIEBOT_OTA_AUTO_MARK_VALID=y` 让组件自己在启动 10s 后确认
  （默认 n；启用时 `main/main.c` 的钩子编译排除，避免重复确认）。
- 回滚关闭（`=n`）时同上 API 为安全 no-op，`GET_INFO.rollback=false`。
- 回滚后 `GET_INFO`/`GET_PROGRESS` 反映实际运行的槽与版本；`last_error` 仅 RAM 级
  （重启后清空），符合契约 §2.3 第 5 条。

## 线程安全

- 所有公开函数用内部 mutex 串行化；上传 API 是单会话的（第二个 `begin` 返回
  `ESP_ERR_INVALID_STATE`）。
- `esp_ota_begin()` 的擦除在锁外执行，避免 GET_INFO/GET_PROGRESS（可能来自 ws 任务）
  被长时间阻塞；期间 `beginning=true`，`write/finish` 会被拒绝、`abort` 被latch。
- `esp_ota_write/end/set_boot_partition` 在锁内执行（毫秒级）。

## 构建与验证

```bash
# 在 dev-ota worktree（脚本来自主分支）
scripts/build_all.sh -t esp32s3
python3 scripts/check_partitions.py          # 8MB ota_0/ota_1 3M 等大，app ≤ 3M-128K
```

- 模块 5 的 WS 冒烟（真机，Wave3）：`{"module":5,"cmd":1}` 应返回
  `running=ota_0|ota_1`、`running_ver == SYSTEM/GET_FM_INFO.fm_ver`、`status=IDLE`。
- 真机 OTA 流程验证属 Wave3（WP-305 硬件验证清单）：上传合法 app.bin → 进度递增 →
  `GET_INFO.running_ver` 变化；rollback=y 时新镜像 10s 内复位会回滚，10s 后由
  `main/main.c` 钩子确认（`wt_ota_mark_app_valid()`），串口日志出现
  `OTA running image confirmed (rollback cancelled)`。
