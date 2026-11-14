# components/dap_tcp — CMSIS-DAP over TCP 后端（M3 骨架）

默认**关闭**（`CONFIG_BIRDIEBOT_DAP_TCP_ENABLE=n`）。开启后监听 TCP 3241，同一时间服务
一个调试器，帧格式与 OpenOCD `cmsis-dap backend tcp` 和 XRDAP 探针的 3241 端口一致：

```
"DAP\0"(4) + 长度(2, 小端) + 类型(1) + 保留(1) + CMSIS-DAP 包(长度)
```

| 文件 | 作用 |
|---|---|
| `include/dap_tcp_frame.h` / `src/dap_tcp_frame.c` | 纯 C 帧头编解码 + 流式解析器（无 ESP-IDF，host 可测） |
| `include/dap_tcp_server.h` / `src/dap_tcp_server.c` | lwIP 会话骨架：单客户端、keepalive、`TCP_NODELAY`、读帧、弱钩子；关闭时编译为 no-op 桩 |
| `test/host/run.sh` + `test/host/test_dap_tcp_frame.c` | host 回归（编解码 / 半包 / 粘包 / 非法帧 / 1024 上限 / 随机分块 fuzz） |
| `Kconfig` | `BIRDIEBOT_DAP_TCP_ENABLE`（默认 n）、端口 3241、任务核/栈/优先级 |

**骨架边界（重要）**：服务器不接入 DAP 执行器。每个请求帧交给弱钩子
`birdiebot_dap_tcp_execute()`；默认实现返回"不支持"，服务器记录告警并关闭连接。接入
DAP 引擎的位置与步骤见 [docs/dap-tcp-backend.md](../../docs/dap-tcp-backend.md) §7。

自测：

```bash
components/dap_tcp/test/host/run.sh          # 116 checks, 0 failure(s)
```
