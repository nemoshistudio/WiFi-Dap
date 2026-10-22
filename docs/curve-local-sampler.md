# 曲线页设备本地采样（ESP32 闭环）— 接口与实现契约

状态：Phase 0/1 实施中（2026-10-22）。目标：把调试采样闭环从
`Web --WiFi--> ESP32 --SWD--> STM32` 改为 `ESP32 --SWD--> STM32`，Web 只下发
配置并批量接收样本，把每拍 4 次 WiFi 往返从关键路径移除。

## 1. 固件侧

新增组件 `components/curve_sampler`（或等价），复用 `components/target_flash` 的
本地 SWD 能力（`tf_swd_*` / `tf_dap_*` 与 DAP hand-off），不得改变现有
USBIP/WebSocket-DAP/target_flash 行为。采样任务固定使用专用任务/栈，不阻塞
httpd 任务。

### 1.1 HTTP 接口（同源，端口 80）

所有响应 JSON 至少包含 `{"ok":true|false}`；失败加 `"err":"..."`。

`POST /api/curve/selftest`（Phase 0）
- 无 body 或 `{}`。
- 动作：获取 DAP、SWD bring-up、执行 N=20 次
  `halt -> 读 r13/r7/r15 -> 读 2 个内存 word -> resume`，统计
  min/median/p90/max 单次耗时，然后恢复目标运行、释放 DAP。
- 返回：
```json
{"ok":true,"iterations":20,"min_us":..,"median_us":..,"p90_us":..,"max_us":..,
 "link_hz":2000000,"halt_us":..,"regs_us":..,"mem_us":..,"resume_us":..}
```
- 该端点用于 Phase 0 决策；可在 Phase 1 保留为诊断。

`POST /api/curve/start`
- Body JSON：
```json
{
  "interval_us": 20000,
  "halt": true,
  "series": [
    {"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}},
    {"id":1,"size":12,"expr":{"kind":"deref","inner":{"kind":"addr","address":603991984},"offset":220}}
  ]
}
```
- 限制：`series` 1..16，`size` 1..8，`interval_us` 5000..200000，
  `sum(size) <= 256`；表达式深度 <= 8。
- 表达式 schema 与 Web 端 `AddressExpr` 完全一致：
  `{"kind":"addr","address":u32}`、`{"kind":"reg","reg":u8,"offset":i32}`、
  `{"kind":"cfa","offset":i32}`、`{"kind":"add","inner":expr,"offset":i32}`、
  `{"kind":"deref","inner":expr,"offset":i32}`。
- 动作：调用 `tcp_server_kick_session()` 让出 DAP 会话；获取 DAP、SWD bring-up；
  创建采样任务；开始写 ring；返回：
```json
{"ok":true,"series_count":2,"sample_bytes":16,"interval_us":20000}
```
- 已在采样返回 `{"ok":false,"err":"busy"}`；目标未响应等返回对应错误。

`POST /api/curve/stop`
- 停止采样任务、恢复目标运行、释放 DAP；返回 `{"ok":true}`。可重复调用。

`GET /api/curve/status`
- 返回：
```json
{"ok":true,"running":true,"interval_us":20000,"frames":123,
 "overwritten":0,"invalid":0,"error":0,"sample_bytes":16}
```

`GET /api/curve/frames?since=<seq>&max=<n>`
- 二进制响应，`Content-Type: application/octet-stream`。`since` 为上次已收到的
  最后一个 `seq`（首次用 `0`，seq 从 1 开始）；`max` 1..64，默认 32。
- 无新帧返回 HTTP 204（空 body）。
- 格式（little-endian）：
```
magic       4B  "CVF1" (0x43 0x56 0x46 0x31)
version     u8  1
flags       u8  bit0=gap（since 已被覆盖，存在丢帧）
count       u16 帧数
first_seq   u32 第一帧 seq
frame_bytes u16 每帧 payload 字节数（= sum(series.size)）
reserved    u16 0
frames[count]:
  seq       u32
  timestamp_us u64   ESP32 本地时刻
  payload   frame_bytes 原始字节（按 series 顺序拼接）
```
- ring 建议 >=256 帧；覆盖时递增 `overwritten`，`frames` 返回仍保留的帧并置 gap。
- 若 `since` 为 0，表示从当前最旧帧开始。

### 1.2 采样器语义

- 每拍：`halt`（若请求 halt=true）→ 读取所需核心寄存器 → 计算表达式地址 →
  批量读内存 → `resume` → 写 ring。表达式求值失败/地址不可访问时该帧对应
  字节填 0 并递增 `invalid`，不得终止采样或让 DAP 进入 sticky FAULT。
- 地址过滤：STM32H7 只允许 DTCM `0x20000000..0x2001FFFF`、AXI SRAM
  `0x24000000..0x2407FFFF`、Flash `0x08000000..0x08FFFFFF`；其他地址不读。
- 发生 SWD/AP 错误：调用 `tf_swd_recover_bus()`，连续 5 次失败停止并记
  `error`；停止路径必须 `resume` + 释放 DAP。
- 时间戳使用 `esp_timer_get_time()`（微秒）。
- 采样任务不得长期持有 DAP 后不恢复目标；stop/异常都要恢复运行。

## 2. Web 侧

新增“设备本地采样”模式（Phase 1 可先只支持固定地址/简单表达式）：
- 不建立 WebSocket-DAP 会话；通过同源 `POST /api/curve/start` 下发当前选中系列
  的 `AddressExpr`（直接复用 DWARF 解析结果）。
- 每 20–50 ms 轮询 `GET /api/curve/frames?since=...&max=32`，按二进制格式解码，
  写入现有 chart ring（沿用现有 series/时间戳/CSV 逻辑）。
- 停止时 `POST /api/curve/stop`；页面卸载/错误也要 stop。
- 显示实际采样率、invalid、overwritten、halt 状态；错误提示可回退到 Web-DAP 模式。
- 现有 Web-DAP 实时/调试模式保持不变。

## 3. 验收

Phase 0：selftest 返回真实单次耗时；据此确定 Phase 1 默认采样间隔。
Phase 1：
- 固定地址变量本地采样 >=100 Hz，页面曲线正常，停止后目标恢复运行；
- 不改变现有 Web-DAP 模式和 target flash 行为；
- 长跑 60 s 无内存增长/无目标卡死；断线/停止可恢复；
- 构建/OTA 后 Web E2E 通过。

## 4. 真机结果（Phase 0/1，2026-10-22）

设备 `<device-ip>`，目标 reference `-O0` 调试固件，SWD 链路 1 MHz。

### Phase 0：`POST /api/curve/selftest`

20 次 `halt → 读 r13/r7/r15 → 读 2 个内存 word → resume`：

| 指标 | 值 |
|---|---|
| min / median / p90 / max | **1924 / 1926 / 1971 / 2065 µs** |
| 阶段平均 | halt 274 µs，regs 1246 µs，mem 301 µs，resume 116 µs |
| 链路 | 1 MHz，DPIDR 0x6BA02477，20/20 成功 |

结论：本地单拍约 1.9 ms，10 ms 间隔可跑 100 Hz（halt 占用约 19%），适合调试；
20 ms 间隔为 50 Hz（halt 占用约 10%）。Web 端可选 5/10/20/50 ms。

### Phase 1：真机 Web 端

- 固定地址 `xTickCount`：100–101 Hz、60 s / 5,966 帧、Invalid 0、
  Overwritten 0、heap 稳定 42.1 MB；停止后 Web-DAP 可正常重连，目标已恢复运行。
- 全局 `target_debug_handle->bmi088_.accl_ x/y/z`：100 Hz、约 450 帧/5 s、
  Invalid 0、Overwritten 0，数值正常（accel_z≈9.7 m/s²）。
- 栈帧变量 `delay`（`[r13+…]`）：100 Hz 采样，Invalid≈43/470（函数帧不在栈上时
  失效，符合预期），Overwritten 0。
- 长跑/停止：60 s 后 `POST /api/curve/stop`，后端 `running=false`，
  Web-DAP 实时采样回归正常（100 Hz 级）；页面无 JS error。
- 截图：`docs/images/web-curve-local-mode-real.png`。

### 已知限制

- `start` 会经 `tf_swd_link_init` 的 nRESET 复位一次目标；采样期间每拍 halt/resume。
- 采样期间不能建立 WS-DAP 会话；与 target flash 互斥目前是单向检查。
- 寄存器值变量（`DW_OP_reg` 无 location）暂不支持；`.debug_loc` 仍未解析。
- seq 每次 start 从 1 开始，Web 客户端 start 后重置 `since=0`（已实现）。


## 5. Phase 2（实施中）：断点触发采样

周期性 halt 对控制环仍有干扰，且栈帧变量只在函数执行时有效。Phase 2 增加
`trigger=breakpoint`：ESP32 在目标函数/PC 地址配置 FPB 硬件断点，目标命中后
读取当前帧/指针，再恢复运行；`interval_us` 变为两次采样之间的最小间隔
（节流，避免高频函数把目标反复停住）。

### 5.1 API 扩展

`POST /api/curve/start` 增加字段：
```json
{
  "trigger": "periodic",              // "periodic" | "breakpoint"
  "breakpoint_address": 134356037,    // 仅 breakpoint；Thumb 指令地址，bit0 忽略
  "interval_us": 20000,               // periodic=采样周期；breakpoint=最小采样间隔
  "halt": true,
  "series": [...]
}
```
- `trigger=breakpoint` 时 `halt` 字段忽略（命中时核心已被 FPB 暂停）。
- `breakpoint_address` 必须落在目标 Flash（0x08000000..0x08FFFFFF）且半字对齐；
  非法 → 400 `{"ok":false,"err":"bad breakpoint address"}`。
- `interval_us` 对 breakpoint 为节流下限，范围 5000..1000000 µs；默认 20000。
- 采样任务行为：
  1. 保存并配置 FPB comparator（FPB_CTRL/FPB_COMP0），使能断点；
  2. 等待 DHCSR.S_HALT（命中）；
  3. 读所需寄存器、求值表达式、读内存、写 ring（时间戳为命中时刻近旁）；
  4. 关闭 comparator，resume 目标；等待 `interval_us` 后再使能，等待下一次命中；
  5. stop/错误：关闭并恢复原 comparator，resume，释放 DAP。
- 命中时 PC 位于 `breakpoint_address` 之后/附近的指令；若选择函数入口，局部变量
  可能尚未初始化，建议把断点放在初始化之后的指令地址（用户可手工填地址）。

`GET /api/curve/status` 增加：
```json
{"trigger":"breakpoint","breakpoint_address":134356037,"hits":N,"sampled":N,
 "throttled":N}
```
`hits` 为 FPB 命中次数，`sampled` 为实际写入 ring 的次数，`throttled` 为节流丢弃次数
（当前实现命中后总是采样并节流等待，字段保留给后续多断点/跳过策略）。

### 5.2 Web 扩展

- 本地模式新增 `Trigger: Periodic / Breakpoint`；
- Breakpoint 模式显示十六进制地址输入（必填）、`Min interval`（5/10/20/50 ms）、
  hits/sampled 状态；Start 时带 `trigger`/`breakpoint_address`；
- 其余选变量/曲线/CSV/stop 逻辑复用；地址可从 ELF 符号/反汇编获得，UI 先提供
  手工输入，后续可加函数符号下拉。

### 5.3 真机验收

- 在 `Device::BMI088` 的某个高频函数上设断点，确认 hits/sampled 增长、帧时间戳
  符合函数调用频率且不超过节流上限；
- stop 后 comparator 关闭、目标恢复运行、FPB 原值恢复；
- 与 periodic 模式、Web-DAP 模式互不干扰；长跑无目标卡死。

## 6. Phase 2 真机结果（2026-10-22）

- 目标：reference `-O0` 调试固件，SWD 1 MHz，FPB comparator0。
- Breakpoint 地址：`Device::BMI088::PraseAccel` 入口 `0x08029244`
  （Thumb 地址 `0x08029245` 也可，固件按 bit0 掩码；status 保留原始值）。
- 固定地址 `xTickCount` + `trigger=breakpoint`、`interval_us=20000`：
  **Hits/Sampled 155/155**（约 3.4 s）、**45 Hz**、Invalid 0、Overwritten 0；
  帧时间戳间隔约 20–21 ms，符合节流下限。
- `reg13` 帧表达式（`{kind:'reg',reg:13,offset:0}`）breakpoint 模式：
  Hits/Sampled 135/135，Invalid 0，说明命中时寄存器/内存路径正常。
- 非法地址 `0x1234` → HTTP 400 `{"ok":false,"err":"bad breakpoint address"}`；
  Thumb bit 地址被接受。
- stop 后目标运行恢复，随后 Web-DAP debug 回归 16 Hz/Failures 0；
  periodic 本地无 halt 回归 200 Hz（5 ms）正常。
- 截图：`docs/images/web-curve-local-breakpoint.png`。

注意：入口处局部变量可能尚未初始化；实际使用时把断点放在初始化之后的指令地址，
并在 UI 中手动填写该地址；FPB 与其他调试器互斥。

## 7. Phase 3 快照采样真机结果（2026-10-22）

在 reference 固件中新增 BMI088 调试快照全局结构（`src/component/comp_debug_snapshot.*`，
`PraseAccel/PraseGyro` 末尾 seqlock 写入），固件编译并烧录后：

- 快照符号：`g_bmi088_debug_snapshot` @ **0x20002270**，60 B；
  字段 `raw_accel[3]/raw_gyro[3]/raw_temp/accel[3]/gyro[3]/temp/accel_count/gyro_count`。
- Web 上传 `reference-debug-O0-snapshot/firmware.elf`，本地模式 Periodic + 5 ms：
  **201 Hz、Invalid 0、Overwritten 0、Halt OFF**；
  选择 `raw_accel[0..2]` 时曲线持续更新（示例 -4912/-2292/648），无需 halt 目标。
- 目标固件运行正常（`g_assert_count=0`，`target_debug_handle` 初始化，tick 递增）。
- 产物：`firmware/reference-board/reference-debug-O0-snapshot/`（ELF sha256
  `c92df21b4dee763dd77af4f9cfe4b67c9de863f03b4fceacd20e0bbcc5706f32`，
  Windows `C:\Users\reference firmware\ozone-test\25mo​ni-debug-O0-snapshot\`）。
- 注意：快照源码在 reference firmware 工作树（未 commit）；构建用的 `-O0`/cap cutoff/
  timer stack 补丁只在手工固件中，原始构建配置已恢复。

### 7.1 1 ms 间隔说明

固件 API 允许 `halt=false` 的 periodic 采样最小 1000 µs（实测后端可达 ~1000 Hz），
但 Web 每 30 ms 轮询 HTTP 的吞吐/主线程在 1 kHz 下会落后：
3 路 1 ms 采样实测页面 ~288 Hz、overwritten 快速累积；5 ms（200 Hz）稳定且
overwritten=0。因此 Web UI 仍只暴露 5/10/20/50 ms 本地间隔，1 ms 仅供 API/后续
流式传输优化使用。

## 8. Phase 2.5 实现补充（2026-10-22）

- `POST /api/curve/start` 的每个 series 支持可选 `pc_low`/`pc_high`（Flash，
  半开区间 [low,high)，Thumb bit0 归一化）；仅表达式依赖寄存器时启用门控。
  当前 PC 不在范围 → 该 series 本拍填 0、`out_of_scope++`，不停止采样；
  `invalid` 仅表示地址/读取失败。
- 每拍只读一次 PC（periodic halt 后 / breakpoint 命中时）；no-halt 模式下
  带门控 series 一律 out_of_scope（不读过期栈）。
- `CURVE_MAX_SERIES` 提升到 32（sum(size)≤256）；ring 动态容量在 heap 不足时
  自动从 32 KiB budget 二分降到 ≥32 帧，避免 32 系列 start 失败。
- 状态新增 `out_of_scope`、`ring_frames`、`served_seq`；Web 端 PC 范围来自
  DWARF subprogram low_pc/high_pc，仅对栈帧/寄存器表达式下发。
- 真机：固定/全局 20 ms Halt OFF ≈50 Hz；栈帧变量 20 ms ≈50 Hz，
  不在栈时 out_of_scope 增长、invalid 0；32×4B 系列 ring_frames=117 可运行。

