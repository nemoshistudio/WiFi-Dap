# scripts/ — 本地构建 / 校验 / 发布工具（Docker ESP-IDF + 构建锁）

本目录提供 WiFi Dap 项目的**本地**工程化工具（WP-141/142/143）。
所有构建、校验、打包都在本机完成：脚本不会 `git push`、不会切换分支、不会修改
remote、不会触发 GitHub Actions（INTEGRATION-PLAN v4 §8.4）。

| 文件 | 作用 | WP |
| --- | --- | --- |
| `idf-docker.sh` | 在 `espressif/idf:v5.2.1` 容器里运行 `idf.py`；整 worktree git 目录只读挂载；`--git-probe` 自检 | WP-002 |
| `build-lock.sh` | `flock` 构建信号量，最多 3 个并发构建，超限自动排队 | WP-002 |
| `build_all.sh` | 三目标（esp32s3/esp32c3/esp32）一键构建 + 版本/大小汇总 + 分区门禁 | WP-143 |
| `check_partitions.py` | 分区表冻结校验（含 docker 内 `gen_esp32part.py`）+ app 大小/版本上限 | WP-142 |
| `check_board_pins.py` | `components/board` 与 `docs/board-pins.md` 契约比对 + 4 板 host 语法检查 | WP-143 |
| `gen_web_assets.sh` | `web/dist-gz`/`web/dist` → 暂存 + hash + `web_version.txt`；`--install` 才显式写入 `project_components/html/` | WP-143 |
| `release_local.sh` | 本地发布包（app/bootloader/ptable/合并 bin + gz + SHA256SUMS），**不推送** | WP-143 |
| `check-version-string.sh` | 版本字符串校验（release 拒绝 `-dirty`/开发后缀） | WP-143 |
| `check-release-versions.sh` | 固件 tag / Web version / protocol / 镜像内嵌版本交叉校验 | WP-143 |
| `check-protocol-version.sh` | 协议版本三源一致性门禁：C 头 == `docs/protocol.md` == `web/src/api/index.ts` | WP-143/审计 B6 |
| `device_smoke.sh` | 设备就绪**只读**冒烟：USBIP/HTTP/WS 握手 + WS ping + OTA GET_INFO，聚合 PASS/FAIL | WP-305 |
| `ota_upload.py` | 真机 OTA：HTTP 直传 `/api/ota/upload` + WS 查模块 5 GET_INFO（纯标准库） | WP-205/305 |
| `README.md` | 本文档 | — |

## 前置条件

- Docker（当前用户可执行 `docker`，通常在 `docker` 组内）
- `bash`、`flock`（util-linux）、`realpath`（coreutils）、`python3`、`gcc`
- 首次使用会拉取 `espressif/idf:v5.2.1` 镜像（约数 GB，5–15 分钟）：

```bash
docker pull espressif/idf:v5.2.1
```

## 推荐入口：一键三目标构建

```bash
# 全量（S3 8MB + C3 4MB + ESP32 4MB；set-target + build + 分区门禁）
scripts/build_all.sh

# 干净重建 / 只构建 S3 / 指定编译并发
scripts/build_all.sh --clean
scripts/build_all.sh -t esp32s3 -j 8   # 编译并行度（容器内 CMAKE_BUILD_PARALLEL_LEVEL）

# 发布级构建（拒绝 -dirty 与回退版本；需要 vX.Y.Z tag）
scripts/build_all.sh -t esp32s3 --release
```

每个目标独立构建目录与独立 `sdkconfig`：
`build/<target>/`、`build/<target>/sdkconfig`，完整日志 `build/logs/<target>.log`。
构建结束后脚本会解析 `esp_app_desc.version`（必须非空，`docs/versioning.md`
§2.2/§2.5），打印版本与 .bin 大小汇总，并跑一次 `check_partitions.py --require-bin`。

> 注意：ESP-IDF 默认把每个 `-B` 目录的 sdkconfig 都写到工程根 `sdkconfig`，
> 三目标连续构建会互相覆盖。`build_all.sh` 显式传
> `-D SDKCONFIG=build/<target>/sdkconfig` 隔离；手工构建也请照做。

## idf-docker.sh

把工程目录挂载到容器 `/project`，以当前用户 uid/gid 运行，生成文件宿主可直接
读写。**linked worktree（`.git` 是文件）** 会额外把主仓 `.git` 与 worktree 的
gitdir 以只读方式挂到容器内**相同绝对路径**，保证容器内 `git describe` /
`git show`（PROJECT_VER、FW_UPD_DATE）可用；无 git 的源码包则回退 version.txt /
v1.0.0，不会产生空版本。

```bash
scripts/idf-docker.sh [-C <dir>] [-B <dir>] [--git-probe] <idf.py 参数...>
```

| 选项 | 说明 |
| --- | --- |
| `-C <dir>` | 工程根目录（默认：**当前工作目录**，可在任意 worktree 里直接运行） |
| `-B <dir>` | 构建目录，相对工程根（如 `build/esp32s3`），缺失时自动创建 |
| `--git-probe` | 打印容器内 git 版本/describe/commit/commit-date 后退出（验证 worktree 挂载） |
| `-h` | 帮助 |

### 常用示例

```bash
# 版本自检与 worktree git 自检
scripts/idf-docker.sh --version
scripts/idf-docker.sh -C . --git-probe

# 单目标编译（sdkconfig 建议放构建目录，避免多目标互相覆盖）
scripts/idf-docker.sh -B build/esp32s3 \
    -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 \
    -D SDKCONFIG=build/esp32s3/sdkconfig set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3 -D SDKCONFIG=build/esp32s3/sdkconfig build

# 推荐直接使用一键脚本
scripts/build_all.sh -t esp32c3
```

### 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `IDF_DOCKER_IMAGE` | `espressif/idf:v5.2.1` | 使用的镜像 |
| `IDF_CCACHE_DIR` | `~/.cache/birdiebot-ccache` | 宿主机 ccache 目录，容器内挂载为 `/ccache` |
| `IDF_DOCKER_EXTRA_ARGS` | 空 | 追加给 `docker run` 的参数，例如 `--device=/dev/ttyUSB0` |

ccache 默认开启（`IDF_CCACHE_ENABLE=1`、`CCACHE_DIR=/ccache`、`CCACHE_BASEDIR=/project`）。
> **烧录/串口监视**请在宿主机执行，或显式映射设备；
> 容器内同时运行多个构建时请配合 `build-lock.sh` 使用（`build_all.sh` 已自动套用）。

## build-lock.sh

`flock` 构建锁：本机最多 **3 个**并发构建，其余排队等待。锁目录
`/tmp/birdiebot-buildlock`，进程退出（包括被 kill）后自动释放。

```bash
scripts/build-lock.sh <命令...>
BUILD_LOCK_SLOTS=2 BUILD_LOCK_TIMEOUT=300 scripts/build-lock.sh make -j4
```

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `BUILD_LOCK_DIR` | `/tmp/birdiebot-buildlock` | 锁文件目录 |
| `BUILD_LOCK_SLOTS` | `3` | 最大并发构建数 |
| `BUILD_LOCK_TIMEOUT` | `1800` | 等锁超时秒数，`0` = 无限等待 |
| `BUILD_LOCK_POLL` | `2` | 等锁轮询间隔秒数 |

## 分区表校验：check_partitions.py

```bash
python3 scripts/check_partitions.py                   # 布局 + sdkconfig 接线 + gen_esp32part.py
python3 scripts/check_partitions.py --require-bin     # 额外要求已构建的 .bin（发布门禁）
python3 scripts/check_partitions.py --skip-layout     # 只查 app 大小/内嵌版本
```

校验内容（契约 `docs/ota-local-upload.md` §1）：

- `partitions/partitions_8mb_ota.csv`：nvs 24K / phy 4K / otadata 8K / ota_0 3M @0x20000 /
  ota_1 3M @0x320000 / wt_nvs 64K / coredump 64K，ptable @0x8000，末尾 0x640000~0x800000 预留；
- `partitions/partitions_4mb_single.csv`：nvs 24K / phy 4K / factory 3.75M / wt_nvs 64K /
  coredump 64K，**无 OTA**（C3/ESP32 只保证编译启动）；
- 冻结的名字/类型/偏移/大小逐项比对；ota_0 与 ota_1 等大、otadata ≥ 2 sector、
  app 分区 64K 对齐、无重叠、不越过 flash 容量；
- `sdkconfig.defaults.<target>` 的 flash 大小/分区文件名/`PARTITION_TABLE_OFFSET=0x8000` 接线；
- 在 `espressif/idf:v5.2.1` 容器内跑 `gen_esp32part.py` 解析 + 校验（也可在 IDF 容器内
  直接跑，脚本检测 `IDF_PATH` 后走 host python）；
- app 镜像上限：app 分区大小 − 128KiB 余量；并解析 `esp_app_desc.version` 必须非空。

## 板级契约校验：check_board_pins.py

```bash
python3 scripts/check_board_pins.py          # 引脚/LED/UART/默认值 + 4 板 gcc 语法检查
python3 scripts/check_board_pins.py -v       # 打印每个匹配到的宏
```

解析 `docs/board-pins.md` §3/§4 的契约表，用 `gcc -E -dM` 取
`components/board/boards/*.h` 实际宏值逐项比对（PIN_*、`BIRDIEBOT_BOARD_HAS_*_LED`、
UART 实例号、board/target 身份、1346/115200/1MHz 默认值），并复跑文档 §8.1 的
host 语法检查。只改 `components/board/**` 或文档表导致不一致时会失败。

## Web 资源：gen_web_assets.sh

Web UI 由 `web/build.sh`（WP-131）构建，输出两份目录：`web/dist/`（Vite 原始产物）与
`web/dist-gz/`（每个文件 `gzip -9 -n` 后同名 + `.gz`）。本脚本**只做转换/校验/hash**，
不跑 npm。**默认只写入 gitignore 的 `build/web-assets/` 暂存区，绝不静默覆盖已入库的
`project_components/html/`**（WP-122 合并进来的 gz 资产需显式 `--install` 才更新）。

```bash
# 1) 暂存 + 打印 hash manifest（默认读 web/dist-gz，回退 web/dist）
scripts/gen_web_assets.sh

# 2) 校验已入库 html 是否等于当前 Web UI 构建（不写任何文件）
scripts/gen_web_assets.sh --check

# 3) 显式安装（唯一会改 project_components/html/ 的模式；未管理文件不删除）
scripts/gen_web_assets.sh --install

# 其它：原始产物模式 / 指定目录 / 额外文件
scripts/gen_web_assets.sh --dist web/dist
scripts/gen_web_assets.sh --src web/dist-gz --out build/web-assets --install
```

- 输入优先级：`--src`（默认 `web/dist-gz`，预压缩、逐字节可复现）> `--dist`
  （默认 `web/dist`，脚本自行 `gzip -9 -n`）；
- 同时存在 `dist-gz` 与 `dist` 时自动交叉校验（dist-gz 陈旧会报错并要求重建 Web UI）；
- 必需资产：`index.html.gz`、`ws.sharedworker.js.gz`；存在 `build-info.json` 时自动包含
  `build-info.json.gz`；`--gzip FILE` 可追加其它原始文件；
- `web_version.txt` 按 `docs/versioning.md` §4.4 写 `web_version / protocol_version /
  git_tag / git_commit / build_time / index_gz_sha256 / worker_gz_sha256 (+ extras)`；
- `--check` 的**硬性条件**：gz sha256、`web_version`、`protocol_version` 必须一致；
  `build_time` 与 `git_tag`/`git_commit`（构建溯源，非内容身份）只告警不失败——
  仅改固件/脚本的后续提交不会让已入库 gz 失效（审计 B2 场景）；Web UI 变更后需
  `web/build.sh` → `--install` 更新。未管理 gz 只告警不删除。

## 设备就绪冒烟：device_smoke.sh

**只读**一键冒烟（不烧录、不复位、不重启）：设备上电且 WiFi/webserver 启动后，按顺序调用
`scripts/dap_smoke.py`（USBIP devlist + HTTP `GET /` + WS `GET_FM_INFO` 握手）与
`scripts/e2e_ws_client.py`（`ping` + `ota-info`），聚合 PASS/FAIL/SKIP 与耗时。

```bash
# 基本用法（AP 模式默认 192.168.1.1；STA 模式用路由器分配 IP / mDNS 名）
scripts/device_smoke.sh --host 192.168.1.1

# 开鉴权（BIRDIEBOT_WEB_AUTH=y）时带 pair code
scripts/device_smoke.sh --host 192.168.1.1 --token 123456

# 只测其中一类 / 机器可读 summary（stdout 只有 JSON，诊断在 stderr）
scripts/device_smoke.sh --host 192.168.1.1 --skip-dap --json
scripts/device_smoke.sh --host 192.168.1.1 --skip-ws
```

| 选项 | 说明 |
| --- | --- |
| `--host <ip>` | 设备地址（必填；裸 IP/主机名，不带 scheme） |
| `--port N` | HTTP/WebSocket 端口（默认 80；USBIP 探测仍用 3240） |
| `--token T` | 鉴权 pair code（`BIRDIEBOT_WEB_AUTH=y` 构建需要） |
| `--skip-dap` | 跳过 `dap_smoke.py`（USBIP/HTTP/WS 握手） |
| `--skip-ws` | 跳过两个 `e2e_ws_client.py` 步骤 |
| `--json` | 输出机器可读 summary：`steps[]`（name/status/exit_code/duration_ms/command/output_tail）、`summary`、`result`、`device_reachable`、未连接时 `hint` |
| `-h` | 帮助 |

退出码：`0` 全部 PASS；`1` 任一步失败**或设备不可达**；`2` 参数错误。
设备不可达时先做 3s TCP 预检并打印排查提示（设备未上电/未连 AP/IP 或端口错/防火墙/
鉴权），然后以 SKIP 记录各步骤并返回 1。

**分工与前置**

| 工具 | 用途 | 是否写设备 |
| --- | --- | --- |
| `scripts/device_smoke.sh` | 设备就绪后的只读冒烟（握手/WS/OTA 信息），适合烧录后 1 分钟自检 | 否 |
| `scripts/e2e_ws_client.py` | 深入单场景测试：`wifi-scan` / `uart-echo` / `dataflow-list` / `json`（按需） | 否（uart-echo 需回环跳线） |
| `scripts/ota_upload.py` | OTA 写路径：HTTP 上传 app bin（设备会切槽并重启） | **是**（仅 OTA，见 `docs/hardware-test-ota.md`） |

前置：设备已烧录当前固件并启动完成；PC 已连设备 AP/同 LAN；**DAP 代理端口 3240 是单连接串行**，
dap_smoke 的 USBIP 探测会打断正在进行的调试会话——调试时请勿运行（或用 `--skip-dap`）。
本目录若缺少 `dap_smoke.py`/`e2e_ws_client.py`（main 分支合并后才提供），对应步骤 FAIL 并提示
合并主分支；本地无设备自测可用 `DEVICE_SMOKE_SCRIPTS_DIR=<dir>` 指向 mock 工具目录（测试钩子）。

## OTA 真机上传/查询：ota_upload.py

Wave3 硬件验证工具（纯 Python 标准库，无需 pip）。文档见
[`docs/hardware-test-ota.md`](../docs/hardware-test-ota.md)。

```bash
# 上传 app 镜像（默认自动计算 SHA-256 并作为 X-SHA256 发送）
python3 scripts/ota_upload.py --host 192.168.1.1 --file build/esp32s3/birdiebot_wifi_dap.bin

# 上传后轮询 WS，等设备重启并打印新 running slot/version
python3 scripts/ota_upload.py --host 192.168.1.1 --file fw.bin --info

# 只查 OTA/GET_INFO（running/running_ver/free/slot_size/status/rollback/sha256）
python3 scripts/ota_upload.py --host 192.168.1.1 --info

# 鉴权开启（BIRDIEBOT_WEB_AUTH=y）/ 错误注入（错误 X-SHA256 期望 HTTP 422）
python3 scripts/ota_upload.py --host 192.168.1.1 --file fw.bin --token 12345678
python3 scripts/ota_upload.py --host 192.168.1.1 --file fw.bin --sha256 00…00
```

| 选项 | 说明 |
| --- | --- |
| `--host` | 设备地址：`IP` / `host:port` / `http://host[:port]`（WS 用同 host/port） |
| `--file` | 要上传的 app `.bin`（`build/esp32s3/birdiebot_wifi_dap.bin`，非 merge/zip） |
| `--sha256` | 可选 `X-SHA256`（64 hex）；默认=文件 hash，传错误值即 422 错误注入 |
| `--no-sha256` | 不发 `X-SHA256`（设备仍用 `esp_ota_end()` 校验） |
| `--token` | 鉴权 pair code：HTTP `Authorization: Bearer` / WS `?token=` |
| `--timeout` | socket/上传超时秒数（默认 300，契约会话上限） |
| `--info` | 只查 `GET_INFO`；与 `--file` 同用时上传后轮询重连验证新版本 |
| `--info-wait` | 上传后等待 GET_INFO 的秒数（默认 20，0=不等待） |

退出码：`0` 成功；`2` 参数/本地输入错误；`3` 网络/HTTP/WS 失败或设备返回非 200。
脚本只与设备通信，不构建、不烧录、不主动重启（设备成功上传后 ~1s 自行重启）。

## 本地发布包：release_local.sh

前置（审计 F3/R8）：发布机必须**先构建 Web UI**（`(cd web && bash build.sh)` → `web/dist/` + `web/dist-gz/`），
或显式传 `--web-src/--web-dist`；否则默认流程会给出清晰错误而不会静默打包旧资产
（`--skip-web` 可强制使用已入库资产，仅限本机调试）。

```bash
# 严格（要求干净工作区且 HEAD 正好在 vX.Y.Z tag 上；发布用）
scripts/release_local.sh

# 开发和本机验证（无 tag 也可打包，明确标注 local-dev）
scripts/release_local.sh --allow-dirty --skip-build --skip-web

# 指定 Web UI 构建产物（预压缩/原始）/ 不要合并镜像 / 不要 tar
scripts/release_local.sh --allow-dirty --web-src web/dist-gz --no-merge --no-tar
scripts/release_local.sh --allow-dirty --web-dist web/dist
```

产物默认在 **`build/release/`**（gitignore，绝不入库）：

```text
birdiebot-wifi-dap-<ver>/
├── firmware/birdiebot_wifi_dap.bin      # app（唯一可 OTA 上传的镜像）
├── firmware/bootloader.bin
├── firmware/partition-table.bin
├── firmware/birdiebot_wifi_dap_full.bin # merge_bin 首烧全量（不含 otadata，保持 0xFF）
├── firmware/flash.sh                    # 首次烧录：bootloader+ptable+ota_0
├── web/{index.html.gz,ws.sharedworker.js.gz,web_version.txt[,version.txt]}
├── SHA256SUMS
└── RELEASE-INFO.txt
```

严格模式会依次跑：干净工作区检查 → `vX.Y.Z` exact tag → **协议版本三源门禁**
（`check-protocol-version.sh`）→ `build_all.sh --release` → `check-release-versions.sh`
等价检查 → `check_partitions.py --require-bin` → Web gz hash 与 `web_version.txt`
一致性（严格模式**必须**存在 `project_components/html/web_version.txt`，审计 F4）→
`RELEASE-INFO.txt`（含 `web_version` / `web_protocol_version` / `web_metadata`）→
SHA256SUMS 自校验。
`--allow-dirty` 只放宽 tag/版本相等检查，仍做分区、大小、hash 与镜像内版本检查。

## 版本校验脚本

```bash
scripts/check-version-string.sh v1.0.0                       # release 字符串
scripts/check-version-string.sh --allow-dirty v1.0.0-3-gabc1234-dirty
scripts/check-release-versions.sh --allow-dev --bin build/esp32s3/birdiebot_wifi_dap.bin
scripts/check-release-versions.sh                            # 严格发布门禁
```

`check-release-versions.sh` 严格模式要求 `git describe --exact-match` 的 `vX.Y.Z`
tag、`web/package.json.version == X.Y.Z`、镜像内嵌版本 == tag，并自动调用
`scripts/check-protocol-version.sh`（本目录已提供；C 头==docs==Web UI，任一缺失/不一致 exit 1）。

## 协议版本门禁：check-protocol-version.sh

```bash
scripts/check-protocol-version.sh        # 0 三源一致 / 1 不一致或缺失 / 2 用法错误
```

三源：`project_components/wt_common/wt_protocol_version.h` 的
`BIRDIEBOT_PROTOCOL_VERSION`、`docs/protocol.md` 的 `protocol_version = N`、
`web/src/api/index.ts` 的 `export const PROTOCOL_VERSION = N`。
已作为前置接入 `scripts/build_all.sh`（失败即终止构建）与 `scripts/release_local.sh`
（release 模式失败退出，`--allow-dirty` 仅告警）。`docs/versioning.md` §8.2 里旧路径
`web/src/api/protocol.ts` 由文档角色同步为 `index.ts`。

## 自检 / 验证

```bash
# 1) 镜像 + IDF 版本 + worktree git 挂载
docker images espressif/idf
scripts/idf-docker.sh --version
scripts/idf-docker.sh -C . --git-probe

# 2) 协议版本三源 + 板级契约 + 分区表（无需构建）
scripts/check-protocol-version.sh
python3 scripts/check_board_pins.py
python3 scripts/check_partitions.py
python3 scripts/check_partitions.py --skip-layout

# 3) 一键三目标构建 + 版本/大小汇总 + 门禁
scripts/build_all.sh

# 4) Web 资产（web/build.sh 之后）：暂存 -> 校验 ->（按需显式安装）
scripts/gen_web_assets.sh
scripts/gen_web_assets.sh --check
```

## 注意事项

- 提交约束：只提交本 `scripts/` 目录内容（以及 WP 归属的根 CMakeLists /
  `cmake/` / `sdkconfig.defaults*` / `partitions/` / `.github/` / `components/board/`），
  **禁止** `git push` / 改 remote / 切分支 / merge / rebase。
- 脚本假设工程根包含 CMakeLists.txt 与 `sdkconfig.defaults*`；`-C` 指向的目录不对时
  `idf.py` 会自行报错。
- 所有产物写入 gitignore 的 `build*/`，不会污染工作区。
