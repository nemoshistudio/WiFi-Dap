# 曲线页设备本地采样（ESP32 闭环）— 接口与实现契约

状态：Phase 0/1/2 已完成；P1.8.x 调试增强（read/第二 comparator、函数快照、
UART trace/backtrace 原型、每变量刷新率评估）见 §10–§14（2026-10-24）。目标：把调试采样闭环从
`Web --WiFi--> ESP32 --SWD--> STM32` 改为 `ESP32 --SWD--> STM32`，Web 只下发
配置并批量接收样本，把每拍 4 次 WiFi 往返从关键路径移除。

> **精简构建（slim/fw，2026-10-26，C4）**：总系列上限 `CURVE_MAX_SERIES` 由 256 降为
> **64**（每组仍 ≤64、组数仍 ≤8、每组 payload ≤256 B 不变）；`curve_http.c` 里静态
> `curve_config_t` 的内部 BSS 因此减少 3 KiB（引擎 per-series tick 数组再减 192 B）。
> Web 前端同步把可选变量上限收到 64。本文 §1.1/§9/§13 中出现的 256/512 是完整功能
> 构建的历史数值；恢复方式见 [feature-flags.md](feature-flags.md) C4。

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


## 9. 多组轮转 + loclist variants（B/D 完整支持，协议 v2）

### 9.1 Start 请求扩展（向后兼容）

```json
{
  "interval_us": 20000,
  "halt": true,
  "groups": [
    {
      "id": 0,
      "series": [
        {"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}},
        {"id":1,"size":4,
         "variants":[
           {"pc_low":134386244,"pc_high":134386692,"expr":{"kind":"reg","reg":13,"offset":48}},
           {"pc_low":134386692,"pc_high":134387000,"expr":{"kind":"reg","reg":7,"offset":-32}}
         ]}
      ]
    },
    {"id":1,"series":[...]}
  ]
}
```
- 兼容旧客户端：`groups` 缺省时把根 `series` 当成单 group。
- 限制：groups 1..8；每组 series 1..32；每组 sum(size) ≤256；总 series ≤256。
- `variants` 可选、1..16，按 `pc_low/pc_high` 半开区间选择；有 variants 的 series 必须 halt=true；无匹配 → `out_of_scope++`、填 0。
- `interval_us` 是**每帧周期**；N 组时每组刷新率 = 1/(N×interval_us)，Web 需显示 group 刷新率。
- Start 响应增加 `group_count`、`series_count`、`sample_bytes`（各 group payload 的最大值）。

### 9.2 采样轮转

- 每个 tick 只采样当前 group，然后 `current_group=(current_group+1)%group_count`。
- PC 门控/variants/指针/地址过滤逻辑按该 group 的 series 执行。
- 连续错误/stop 行为不变。

### 9.3 CVF2 二进制帧

`GET /api/curve/frames` 返回（magic `CVF2`，version=2）：
```
magic       4B "CVF2"
version     u8 2
flags       u8 bit0=gap
count       u16
first_seq   u32
frame_bytes u16   // 所有 group 中最大 payload 字节
reserved    u16 0
frames[count]:
  seq       u32
  timestamp_us u64
  group     u8
  payload_len u16
  payload   payload_len bytes (按该 group 的 series 顺序)
```
- ring 槽按 `frame_bytes` 分配；`payload_len` 每帧实际长度；Web 按 group 的 series sizes 解码。
- `served_seq`/overwritten/gap 语义沿用。
- status 增加 `group_count`、`current_group`。
- 继续支持旧 CVF1（单 group）读取？新固件优先 CVF2；Web 需同时解码 v1/v2（向后兼容旧固件）。

### 9.4 loclist 多段

- Web 对多段变量发送 `variants`（来自 `locationVariants`，每个已有 pcLow/pcHigh/expr）；不再只发第一段。
- 单段/default 继续用 `expr`/`pc_low/pc_high`。
- 无 location 仍不可采样。

### 9.5 验收

- 多组（如 2 组）总 series >32（例如 40/64）可启动；页面按 group 映射；每组刷新率约 interval×组数。
- 多段 loclist 变量在覆盖当前 PC 的变体下采样值与调试器一致；无覆盖时 out_of_scope，不画假数据。
- 旧单组客户端行为不变；reference firmware 不改。

### 9.6 v3 compact expression / 内存

- Start 请求的 series/variants 可发 `expr_s`（紧凑前缀字符串）代替嵌套 `expr`：
  `A<addr8> R<reg2><off8> C<off8> +<off8><inner> D<off8><inner>`（固定宽度 hex）。
  Web 对每个表达式统一发送 `expr_s`；固件 v3 同时兼容旧 `expr`。
- 目的：40 路深指针请求从 ~7 KiB/数百个 cJSON 节点降到 ~2.9 KiB/几十个节点，
  避免 ESP32 堆碎片导致 start 解析失败；256 路也可控。
- 帧 ring 预算降为 **8 KiB**（`CURVE_RING_BYTES_BUDGET`，cap 128..512），
  大 payload 时容量自动降；start 前会释放上一次 ring 再解析请求，进一步降低堆压力。
- 真机：40+1 路/2 组 20 ms 启动成功，总 50 Hz、Groups 2、Group rate 25 Hz；
  单组固定变量仍 50 Hz Halt OFF。

### 9.7 ring 8 KiB 硬预算 / group weight

- `capacity = clamp(8192 / (15 + frame_bytes), 16, 512)`，ring bytes 恒 ≤8 KiB；
  16B→264 帧、64B→103、128B→57、256B→30 帧；malloc 仍失败时逐次减半到 8 帧。
- groups[] 可带 `weight` 1..8（默认1），采样调度表 sum(weight)≤64；全 1 等价旧轮转。
- status.ring_frames 为实值；host test 3343 checks。

## 10. P1.8.1：read watchpoint + 第二个 DWT comparator（已实现）

`trigger=watchpoint` 扩展（向后兼容：不带新字段的旧请求行为不变）：

```json
{"trigger":"watchpoint",
 "watch_address":536870912,"watch_size":4,"watch_access":"read",
 "watch2":{"address":603979776,"size":2,"access":"readwrite"},
 "interval_us":20000,
 "series":[{"id":0,"size":4,"expr":{"kind":"addr","address":536870916}}]}
```

- `watch_access` 增加 `"read"`：取值 `"write"`（默认）/`"read"`/`"readwrite"`，
  分别对应 `DWT_FUNCTIONn = 6/5/7`（load/store/load+store）。旧固件对
  `"read"` 返回 400，新固件接受；旧请求 `"write"`/`"readwrite"` 语义不变。
- `watch2`（可选对象）：第二路独立的 DWT comparator，字段
  `address`（必填，地址范围/自然对齐校验同第一路）、`size`（1/2/4，默认 4）、
  `access`（同上，默认 `"write"`）。与第一路完全相同（地址+size+access）会以
  400 `"watch2 duplicates the first watchpoint"` 拒绝；同地址不同 access
  （如 read + write 拆分）允许。非 `watchpoint` 模式带 `watch2` → 400
  `"watch2 requires trigger=\"watchpoint\""`。
- 资源分配/互斥：启动时读 `DWT_CTRL.NUMCOMP`，扫描 `DWT_FUNCTIONn` 取前 N 个
  空闲 comparator（第一路优先 comparator 0，保持旧行为；第二路用下一空闲）。
  目标少于 2 个 comparator 或空闲不足 → `start` 返回错误
  （`ESP_ERR_NOT_SUPPORTED` / `ESP_ERR_INVALID_STATE`，HTTP 500，`err`/message
  含 `DWT_CTRL` 实际值并说明原因），不会覆盖其他 comparator。
  部分写入失败会在返回前恢复已改动的 comparator。
- 恢复：stop/错误路径对本次分配的所有 comparator 恢复
  `COMPn/MASKn/FUNCTIONn` 并恢复 `DEMCR`；未分配的 comparator 与 FPB 不触碰。
- 命中归类：halt 后读所有已分配 `FUNCTIONn`，用 `MATCHED`(bit24) 判断是否本次
  触发；命中记 `watchpoint_hits`（总）与 `watchpoint2_hits`（第二路），
  `last_watch_index` = 1/2；两路同拍命中时记 1 且两个计数都 +1。
- status 新增：`watch_count`（本次配置的 watchpoint 数 1/2；实际 arming 看
  `watchpoint_armed`/`watchpoint2_armed`）、`watchpoint2_armed`、
  `watchpoint2_address`、`watchpoint2_hits`、`last_watch_index`。start 响应新增
  `watch_count`/`watchpoint2_address`/`watchpoint2_size`/`watchpoint2_access`。
- CVF2/触发标记不变：每个 watchpoint 帧仍是 trigger-event 帧（flags bit2 +
  `trigger_seq`）；backtrace（§12）可选开启。
- 真机未验证项：read watchpoint 的实际触发、双 comparator 同时/分别命中、
  第二路资源忙碌报错。需在 M7/M4 目标上用 §14 命令验证。

## 11. P1.8.2：函数快照（breakpoint 一次性前后帧，已实现）

目标场景：Web 里“选中函数 → 在函数内地址设断点 → start → 首次命中后冻结/标记
前后帧”。**默认 breakpoint 语义不变**：不带 `snapshot_after` 的请求仍为连续
断点采样（帧不标 trigger、不自动停止）。

```json
{"trigger":"breakpoint","breakpoint_address":134356037,"interval_us":20000,
 "snapshot_after":8,
 "series":[{"id":0,"size":4,"expr":{"kind":"reg","reg":13,"offset":244}}]}
```

- `snapshot_after`（0..32，字段**出现**即启用一次性快照，0 表示只保留触发帧）：
  1. 首次 FPB 命中 → 记录 trigger 帧（CVF2 flags bit2，`trigger_seq`），并锁存
     `snapshot_trigger_seq` / `snapshot_pre_frames`（命中时 ring 中已保留的
     触发前帧数；pre 帧就是 ring 里已有的历史，不额外分配缓存）。
  2. 关闭 FPB、resume 目标，用 periodic halt 采样在 `interval_us` 间隔上补采
     `snapshot_after` 帧（`snapshot_post_frames` 计数；这些帧不是 trigger 帧）。
  3. 采完后 session 自行停止：`running=false`、
     `snapshot_state="complete"`、message =
     `stopped: function snapshot complete (trigger seq=..., post=...)`。
     stop/错误提前结束 → `snapshot_state="aborted"`。
- ring 仍是唯一帧存储（8 KiB 预算）：停止后 `GET /api/curve/frames` 仍可读
  pre/trigger/post 帧。快照完成后的自动停止**不会**释放 ring，`POST /api/curve/stop`
  也不是必需的（session 已 stopped 时调用是幂等 no-op，不清 ring）；只有下一次
  `POST /api/curve/start` 才会在解析新请求前释放旧 ring。因此 Web 应先取完
  pre/trigger/post 帧，再启动下一次会话。启动时按 ring 实际容量
  clamp 生效的 `snapshot_after`（`status.snapshot_after` 为生效值，
  `ring_frames` 为容量），保证 trigger+post 不被立即覆盖。
- status 新增：`snapshot_state`（`off`/`armed`/`capturing`/`complete`/`aborted`）、
  `snapshot_after`、`snapshot_trigger_seq`、`snapshot_pre_frames`、
  `snapshot_post_frames`。start 响应带 `snapshot_state`/`snapshot_after`。
- Web 取帧建议：`since = max(1, snapshot_trigger_seq - snapshot_pre_frames)`，
  `max=64`，循环直到 `seq > snapshot_trigger_seq + snapshot_after`；用
  `flags.bit2` 定位 trigger 批次，`status.trigger_seq` 定位精确帧。
- 边界：一次 session 只快照首次命中；pre 历史受 ring 容量限制；快照模式多出
  `snapshot_after` 次 halt/resume（每次约 1-2 ms 量级 SWD 开销），默认关闭。
- 真机未验证项：首次命中触发→post 帧时间间隔、停止后 ring 可读、CVF2 bit2
  标记；需按 §14 验证。

## 12. 调试能力评估：SWO/UART trace、call stack、RTOS 任务感知

要求“只做可行原型/评估，明确边界”。本任务结论：

### 12.1 target-UART trace（原型已实现，走既有 UART 链路）

板载目标 UART 由 `components/uart_service` 单一持有（Web UART 页、TCP 1346
桥共用）。本组件作为额外 RX 订阅者旁路捕获最近 2 KiB 目标输出：

- `POST /api/curve/trace/start` → `{"ok":true,"active":true,"ring_bytes":2048}`
- `POST /api/curve/trace/stop` → `{"ok":true,"active":false}`
- `GET /api/curve/trace?since=<byte seq>` → `application/octet-stream` 原始字节；
  响应头：`X-Curve-Trace-Seq`（下次 since）、`X-Curve-Trace-Active`、
  `X-Curve-Trace-Gap`（since 已被覆盖为 1）、`X-Curve-Trace-Total`（总字节
  seq）、`X-Curve-Trace-Dropped`（被覆盖字节数）；无新数据 → HTTP 204。
- status 新增：`trace_active`/`trace_bytes`/`trace_dropped`。
- 边界：只有字节流（不做行切分/目标时间戳）；2 KiB ≈ 115200 波特下 0.17 s；
  与 Web UART/TCP 桥共享同一 UART 带宽与 RX 分发；本组件不重配波特率/校验
  （由 Web UART 模块控制）；`trace/start` 会 `uart_service_init()`（幂等）。
  若未来需要长时间/高速 trace，应改为独立 DMA/流式缓冲，而不是扩本组件
  2 KiB 静态环。
- 真机未验证项：与 Web UART 页/WS 二进制路由同时订阅、204/gap 语义、长跑。

### 12.2 SWO/ITM trace（不可行，未实现，不伪造）

本地 SWO 需要目标 SWO 引脚接到 ESP32 并有 UART/Manchester 接收路径；
当前 `components/DAP/DAP_config.h` 明确 `SWO_FUNCTION_ENABLE=0`，板级引脚契约
没有 SWO，本地采样器只拥有 SWD（AHB-AP）。USBIP 的 SWO 端点是给 PC 端调试器
的，不经过本地 sampler。因此不做本地 SWO trace，Web 侧也不应显示“SWO 可用”。

### 12.3 call stack / backtrace（原型已实现，帧指针展开）

start 可选 `"backtrace":true`（`trigger=breakpoint`/`watchpoint` 有效），每次
trigger 命中时在 halt 状态读 r7，经 AHB-AP 沿 AAPCS 帧指针链展开
（`[fp]`=上一 fp，`[fp+4]`=返回地址），保存最近一次命中最多 8 个 PC。
`GET /api/curve/backtrace` 返回：

```json
{"ok":true,"valid":true,"count":4,"truncated":false,
 "pcs":["0x08029A44","0x08029244","0x0801F310","0x08008A02"]}
```

边界：需要帧指针（`-O0` 或 `-fno-omit-frame-pointer`）；无 DWARF CFI /
`.eh_frame`、无 inline 展开、无中断/异常栈、无 RTOS 任务感知；只保留最后一次
命中；链合法性校验（Thumb bit、Flash 范围、fp 对齐且严格递增）失败即停止，
不会编造深栈。status 只给 `backtrace_valid`/`backtrace_count` 摘要。
真机未验证项：-O0 目标上的帧数/返回地址与调试器对比、错误链降级行为。

### 12.4 RTOS 任务感知（仅评估，未实现）

固件内没有 ELF 符号/类型信息，无法自行解析 `pxCurrentTCB`/TCB 成员偏移。
可行后续方案（需 Web 侧 ELF/DWARF 解析配合，超出本次范围）：
start 增加可选对象
`"rtos":{"current_tcb_address":u32,"name_offset":u16,"stack_offset":u16,`
`"end_stack_offset":u16,"priority_offset":u16,"name_len":u8}`；每次 halt 时读
TCB 指针→任务名/栈范围/优先级，写入 status/帧扩展。风险：FreeRTOS 版本与
config 相关、地址来自 ELF 可能过期、读 TCB 增加每拍 SWD 开销。本次明确不实现。

## 13. 每变量刷新率：groups/weights 评估（固件不改）

结论（2026-10-24 更新）：groups + weight 用于**组间**速率比例；P0-3 已在
**series 级**新增 `sample_every` 精确抽取（见 §15.3），二者叠加使用。
Web 侧权重策略仍按本节的 weight 公式；单系列绝对降频用 `sample_every`。

- 固件调度原语：每 `interval_us` 一个 tick 采一个 group；
  `schedule = 按 weight 展开的 group 序列`，`L = sum(weight)`。
  因此 group g 的实际刷新率 `rate_g = weight_g / (L * interval_us)`；
  每变量刷新率 = 其所属 group 的刷新率。
- 例：目标 50 / 25 / 12.5 Hz → weight 4/2/1（L=7），
  `interval_us = round(1e6 / 87.5) = 11429`，实际 50.003/25.002/12.501 Hz，
  误差 < 0.01%（受 `CURVE_MAX_GROUP_WEIGHT=8` 与 sum(weight)≤64 限制，比例
  1:8 内可表示；总量受 halt 模式最小 5 ms 限制 → 总 tick 率 ≤200 Hz）。
- 未设置时 Web 保持全 1 weight，行为与旧轮转完全一致。
- 若将来建议 `divisor`（每组每 N tick 采一次）：
  - 协议：`groups[].divisor`（u8 1..8，默认 1），JSON +约 14 B/group，
    8 组最多 +112 B；旧客户端不影响。
  - 静态内存：`curve_group_t` 当前 28 B（`series_first/count/sample_bytes`
    6 + bool + u8 + u8 + regs[16] = 26，尾部 2 B 对齐 padding），divisor 可
    放进 padding → **sizeof 不变**；`curve_config_t` 不变。
  - 引擎：调度需从"权重展开序列"改为"每 tick 判定 due 集合再选一个 group"，
    实现与测试复杂度增加，且不能比 weight 表达更多速率组合（任意目标速率
    比例最终仍等价于权重比），故本次不加。若确需"某组绝对降频而不影响其他组
    总 tick 率"，应作为 Web 侧分组/权重策略实现，而非扩固件协议。
- status 已报告 `group_count`/`current_group`，Web 可据 weight 展示每组实际
  刷新率（`rate_g`），不伪造精确 50 Hz（受 interval_us 整数取整影响）。

## 14. 测试、构建与真机建议（本任务交付）

- host 回归：`components/curve_sampler/test/host/run.sh` → **4299 checks
  通过**（新增 DWT helper/read/watch2 解析、snapshot/backtrace 解析、帧指针
  展开器、trace ring wrap/gap/eviction、status JSON 最坏长度）。
- 构建：`scripts/build_all.sh` 三目标（esp32s3/esp32c3/esp32）全绿；
  未 OTA、未 commit、未操作设备。
- 真机验收建议（需 Lead 在有 M7/M4 目标时执行）：
  1. read watchpoint：对已知只读高频变量设 `watch_access:"read"`，确认
     `watchpoint_hits`/`trigger_seq` 增长且帧值合理；
  2. 双 comparator：`xTickCount`（write）+ 另一全局量（read），确认
     `watch_count=2`、两路 hits 分别增长、stop 后 DWT 寄存器恢复
     （再次用其他调试器/DWT 无冲突）；再验证目标 comparator 忙碌时报错；
  3. 快照：选 `Device::BMI088::PraseAccel` 内某指令地址，`snapshot_after:8`，
     确认首帧 trigger、post 帧间隔≈`interval_us`、session 自动停止、
     停止后 `frames` 可读、`snapshot_state="complete"`；
  4. UART trace：Web UART 发送周期性文本，同时 `trace/start` + 轮询
     `GET /api/curve/trace`，确认 `X-Curve-Trace-Seq` 递增、gap=0；
  5. backtrace：`-O0` 目标 `backtrace:true`，对比调试器调用栈（至少 2-3 层），
     再用 `-O2` 确认降级为短链/`valid:false` 而不伪造；
  6. groups/weights 每变量刷新率：Web 设 50/25/12.5 Hz 三组，确认 group 计数
     与时间戳间隔符合 `rate_g` 公式，overwritten=0。


## 15. P0 固件增强（2026-10-24，task-60）

本节是 §1–§14 的增量契约；旧请求/旧解码器行为保持兼容。真机验证由 Lead
在有目标（STM32H723 等）时执行，本节标注“host 已验证”/“真机待验”。

### 15.1 请求体：`CURVE_JSON_BODY_MAX` 恢复 16 KiB + 流式二进制 `start-bin`

- `CURVE_JSON_BODY_MAX = 16384`（JSON v3 兼容路径，旧 Web 不变）。
  JSON 路径仍是**单块堆分配**；统一 NCM+WiFi 固件在碎片化后最大连续块可能
  <16 KiB，此时返回 **HTTP 503**，body 形如：
  `{"ok":false,"err":"no contiguous heap for 16384 B JSON body (largest N B); use POST /api/curve/start-bin","largest_free":N}`
  （不再静默失败/不回退整包占 heap）。
- 新增 `POST /api/curve/start-bin`，Content-Type 任意（按 octet-stream 发送），
  body 上限 `CURVE_BIN_BODY_MAX = 16384`，**流式解析**：httpd 任务用 256 B
  栈缓冲逐段 `httpd_req_recv`，直接填充静态 `curve_config_t`，不申请 16 KiB
  连续堆。成功响应与 `/api/curve/start` 相同（含 `group_count/series_count/
  sample_bytes/interval_us/sample_every_max/link_hz/dpidr/watchpoint*…`）；
  失败 400 且 `err` 为明确解析错误，413 表示 body 超限/截断，409 表示 busy。
- CBS1 二进制格式（全部 little-endian；完整定义同时写在
  `components/curve_sampler/src/curve_priv.h` 的 `CURVE_BIN_MAGIC` 注释）：
```
"CBS1" (4B) | version u8=1 | flags u8 | trigger u8 | watch_count u8 |
interval_us u32 | breakpoint_address u32 | cond_address u32 | cond_value i32 |
cond_size u8 | cond_op u8 | cond_mode u8 | cond_flags u8 |
snapshot_after u8 | group_count u8 | reserved u16=0
flags: bit0 halt, bit1 want_pc, bit2 backtrace, bit3 trigger_condition,
       bit4 snapshot_after
cond_flags: bit0 signed, bit1 stop_after
watch_count × {address u32, size u8, access u8}    (0..4, access 0/1/2)
group_count × {
  weight u8 | series_count u8 | reserved u16=0
  series_count × {
    size u8 | flags u8 (bit0 pc gate) | sample_every u8 | variant_count u8 |
    pc_low u32 | pc_high u32 |
    u16 expr_len | expr_len × ASCII expr_s（空=无单表达式）|
    variant_count × {pc_low u32, pc_high u32, u16 expr_len, expr bytes}
  }
}
```
- 说明：`expr_s` 与 JSON 紧凑表达式语法相同（`A/R/C/+/D`），因此 256 series
  的二进制请求约 5.9 KiB（host test 实测 5936 B），且解析全程无大块分配。
- 验收（host）：`test_bin_parse_256`（4×64 series 解析成功）、
  `test_bin_parse_errors`（坏 magic/版本/截断/空体）、
  `test_bin_parse_watchpoints`；JSON 256 series `test_compact_256_series_body`
  在 16 KiB 下通过。

### 15.2 启动预留 DAP ring + heap guard（P0-2，默认关闭）

- 实现：`dap_handle_prealloc_ringbuffers()`（components/dap_proxy）可从启动时
  的堆中预留两个 USBIP↔DAP ring 原始存储（约 41 KiB），DAP 任务用
  `xRingbufferCreateStatic()` 创建 wrapper，避免 `DELETE_HANDLE`→
  `RESET_HANDLE` 时为连续 20 KiB 块申请失败。
- **真机回归与默认值（2026-10-25）**：在统一 NCM+WiFi S3 固件上启用该预留
  后，USB-NCM（303a:4000）不再枚举，且内部堆降至约 17 KiB/最大 4.6 KiB
  （打开 Dashboard 后 ~7 KiB/1.6 KiB），Web smoke 超时。因此
  `BIRDIEBOT_DAP_RING_PREALLOC` 默认 **0**（`components/dap_proxy/DAP_handle.h`）：
  `app_main()` 不调用预留，DAP 任务恢复已知可用的懒分配
  `xRingbufferCreate()` 路径（DAP_Thread 保留 (re)create 重试循环，不再因一次
  瞬时失败杀掉 DAP 任务）。需要时可用
  `-DBIRDIEBOT_DAP_RING_PREALLOC=1` 重新编译启用并从状态字段验证。
- `/api/target/status` 仍暴露 `dap_ring_preallocated`（默认 false）、
  `dap_ring_active/request_bytes/response_bytes` 与 heap 快照，便于真机对比。
- heap guard（与预留无关，始终生效）：`/api/target/info` 在最大连续块
  <4096 B 时返回 503；curve JSON start 在 16 KiB 连续块不足时 503 并提示
  start-bin；frames 分配失败映射 503。
- `/api/target/info` 在 probing 前做 heap guard：最大连续内部块
  < 4096 B 时返回 **503**，`err="low heap: largest free block N B < 4096 B"`
  （不再让 SWD 静默超时）。
- `/api/curve/start`（JSON 路径）同样有 16 KiB 连续块 guard，返回 503 + 提示
  `start-bin`；`/api/curve/frames` 的响应分配失败映射为 503。
- `GET /api/curve/status` 新增 `heap_free`/`heap_largest`（真实快照）。
  启动日志新增 reserve 前后与网络初始化后的 free/largest 数值，便于现场诊断。

### 15.3 每系列精确刷新率 `sample_every`（P0-3）

- start 请求 series 对象新增 `sample_every`（u8 1..64，默认 1）；接受别名
  `divisor`（同义，两者都有时 `sample_every` 优先）。无 groups 的旧根
  `series` 同样支持。
- 语义：该 series 在**所属 group 的每 N 次 tick** 采一次（计数器每 tick
  前进、0 时采样）。与 groups/weight 叠加：weight 决定 group 调度次数，
  sample_every 决定组内抽取，二者独立。`curve_series_t.sample_every` 复用
  原 `reserved` 字节，结构体仍 16 B。
- 未采样 tick：该 series payload 字节写 0，并在 CVF2 stale footer 中标 0；
  status 计 `series_stale`；其余 series 正常采样（同一帧内混合 valid/stale）。
- 非法值（0/65）→ 400：`series[i].sample_every must be 1..64`。
- start 响应/status 新增 `sample_every_max`（本会话最大 N）。
- 有效刷新率 = group_rate / N（group_rate = weight_g/(L·interval_us)，见 §13）。
- host：`test_parse_sample_every`（含 divisor 别名/越界）、`test_sample_due`
  （due 判定）；`sizeof(curve_series_t)==16` 断言。

### 15.4 CVF2 per-series valid/stale footer（兼容旧解码）

- CVF2 **不升 magic/版本**（仍 `CVF2` version=2），header flags 新增
  bit4 = `SERIES_MASK (0x10)`。
- 帧布局完全不变（16 B header + 既有 15/19 B frame meta + payload）。
- 仅当本批至少一帧存在 stale series 时置 bit4；置位时在**所有帧记录之后**
  追加 `count × 8` 字节 footer：按帧顺序每帧一个 little-endian u64 bitmask，
  bit i = 该帧所属 group 内第 i 个 series（`cfg.series[group.series_first+i]`）
  本次 tick 是否有新鲜采样（1=有效；0=stale/未采样/该 tick 越界或读失败，
  payload 对应字节为 0）。只关心 64 series/group 上限。
- 旧解码器：按 header+count 帧解析后忽略尾部字节即可 → 完全兼容。
  新解码器：解析完最后一帧后若 `(flags & 0x10)` 再读 8×count 字节。
- `GET /api/curve/frames` 与 WS 曲线推送使用同一布局。
- host：`test_cvf2_series_mask`（bit4 组合、footer LE、旧 header 无 bit4）。

### 15.5 自动 SWD 时钟（P0-8）

- 自动探测顺序由 `2M→1M→500k` 扩为 **4M→2M→1M→500k**（target_flash
  `tf_probe_link()` 与 curve_sampler 本地采样 `curve_link_up()` 一致）；每个
  档位完整重跑 line reset/JTAG→SWD/DP power-up，保留首个识别成功的档位。
- 手动 API 不变：`GET/POST /api/target/swd-clock`（`hz=0` = auto；手动范围
  100 kHz..24 MHz，S3 M5 引擎；无引擎目标仍为 100 kHz..10 MHz），手动模式下不再走
  自动档位；S3 自动档位为 16/10/4/2/1/0.5 MHz（H723 16 MHz 稳定，20/24 MHz 边缘）。
- 实际时钟上报：`GET /api/target/info` 与 `/api/target/status` 新增
  `swd_clock_actual_hz`（最近一次成功 bring-up 实际使用值；无则省略）、
  `swd_clock_manual_hz`（0=auto）；curve 本地采样仍在 `status.link_hz` 报实际值。

### 15.6 链路质量指标（P0-8/E，`GET /api/target/status` 增量）

全部为真实测量，缺失字段**省略**（不伪造 null/RTT），HTTP 仍 200：
- Wi-Fi STA：`wifi_connected`（bool）；连接时 `wifi_rssi`(dBm)、
  `wifi_ssid`、`wifi_channel`。
- SWD：`swd_clock_actual_hz`/`swd_clock_manual_hz`（见 15.5）。
- DAP 事务延迟（设备侧，**不是网络 RTT**）：`dap_xfer_count`、
  `dap_xfer_avg_us`、`dap_xfer_max_us`、`dap_xfer_last_us` —— OUT URB 入队到
  对应 IN URB 应答的真实统计（原本仅编译期 `DAP_PROXY_STATS` 打印，现常开）。
- DAP ring 预留状态：`dap_ring_preallocated`、`dap_ring_active`、
  `dap_ring_request_bytes`、`dap_ring_response_bytes`。
- 既有：`heap_free`/`heap_largest`/`dap_session_active`/`wired_ncm_active`/
  `wired_dhcp_active`/`wired_dhcp_error`/`wired_dhcp_state`/`wired_ip`
  （NCM 构建）、`swd_clock_hz`（最近一次 probe 缓存）、`last_error`。
- 浏览器侧 RTT 由 Web 自己 `performance.now()` 往返测量，固件不提供/不冒充。

### 15.7 DWT comparator 2→4（P1-12）

- start 请求新增 canonical `"watchpoints":[{address,size?,access?}, …]`
  （1..4 项，access=`write`/`read`/`readwrite`）；旧 `watch_address/
  watch_size/watch_access`（slot 0）与 `watch2`（slot 1）保持兼容；新增
  `watch3`/`watch4` 便捷别名（必须按 slot 连续，缺前一档报错）。
- `watchpoints` 数组与旧对象形式**不能混用**；重复 address+access 报
  `duplicates`；同一 address 不同 access 合法（read/write 各占一个 comparator）。
- 引擎按 `DWT_CTRL.NUMCOMP` 分配空闲 comparator；资源不足时 start 返回明确
  错误：`watchpoints: N comparator(s) requested but DWT_CTRL=0x… reports M
  comparator(s)` 或 `… not enough free (DWT_CTRL=… FUNCTION0=… FUNCTION1=…
  FUNCTION2=… FUNCTION3=…)`，不做静默降级；stop/错误恢复保存的原寄存器。
- status：`watch_count` 0..4；保留 `watchpoint_armed/address/hits`（slot 0）、
  `watchpoint2_*`（slot 1），新增 `watchpoint3_armed/watchpoint3_address/
  watchpoint3_hits`、`watchpoint4_*`；`last_watch_index` 1..4。
- host：watchpoints 数组 4 路、watch3/watch4 连续别名、duplicate/混合/越界、
  `curve_dwt_alloc_comparators` 4 路分配、hit mask 4 位。

### 15.8 UART trace 时间戳/行缓冲/触发（P1-13）

- 旧 `GET /api/curve/trace?since=`（raw bytes + `X-Curve-Trace-*` 头）**保持
  不变**，旧 Web 兼容。
- 新增 `GET /api/curve/trace/lines?since=<line_seq>&max=<1..8>`（JSON）：
```json
{"ok":true,"active":false,"gap":false,"next_seq":12,
 "total_lines":11,"dropped_lines":0,"triggered":true,"trigger_seq":7,
 "lines":[{"seq":5,"t_us":12345678,"truncated":false,"text":"boot ok"}]}
```
  - `seq` 行序号从 1 起；`since` 为上次 `next_seq`；`gap=true` 表示 since
    早于 ring 最旧行（旧行已丢弃，Web 不应跳变补点）。
  - `t_us` = 该行**完成**（收到 `\n`）时的 ESP32 `esp_timer_get_time()`，
    单调 us；不是目标时间，固件不伪造目标时间戳。
  - 行切分：`\n` 结束；`\r` 丢弃；不可打印字节替换 `.`，TAB→空格；行文本
    ≤96 B，超长行分段落并 `truncated:true`（保留目标行边界语义，不伪造字
    节）。ring 保留最近 32 行，`dropped_lines` 为被覆盖行数。
- `POST /api/curve/trace/start` 可选小 body（≤128 B）：
  `{"trigger":"PATTERN","stop_after":true|false}`；首条包含 PATTERN 的行锁存
  `triggered/trigger_seq`，`stop_after` 在该行停止采集（预触发历史仍在两个
  ring 中，可 since 回取）。无 body / `{}` = 旧行为。
  body 过大返回 413；JSON 非法/字段类型错返回 400 明确错误。
- 边界：stop_after 由 RX 任务置 `active=false`，订阅清理发生在下一次
  trace/stop 调用；无 SWO/ITM，无目标时间戳；trace ring 内存为 .bss 固定
  预算（raw 2 KiB + line ~3.4 KiB），不占堆。
- host：`test_trace_line_ring`（时间戳/覆盖/gap/truncated/max clamp）与
  `curve_trace_line_match`（NULL/空模式/子串）。

### 15.9 构建/测试（本任务交付）

- host：`components/curve_sampler/test/host/run.sh` → **4441 checks 通过**。
- 构建：`scripts/build_all.sh` 三目标（esp32s3 统一 NCM+WiFi / esp32c3 /
  esp32）全绿；未 OTA、未 commit、未操作设备/reference firmware。
- 真机待验清单：start-bin（256 series）、sample_every + CVF2 footer、DWT4、
  4M auto clock、trace/lines + trigger、target/status 新字段（RSSI/DAP 延迟/
  ring 预留）、target/info 在碎片化堆下的 503 行为。
