# scripts/ — 本地构建环境（Docker ESP-IDF + 构建锁）

本目录提供 WiFi Dap 项目的**本地**构建工具。所有构建都在本机完成，
脚本不会 `git push`、不会切换分支或修改 remote。

| 文件 | 作用 |
| --- | --- |
| `idf-docker.sh` | 在 `espressif/idf:v5.2.1` 容器里运行 `idf.py`，带 ccache 与用户映射 |
| `build-lock.sh` | `flock` 构建信号量，最多 3 个并发构建，超限自动排队 |
| `README.md` | 本文档 |

## 前置条件

- Docker（当前用户可执行 `docker`，通常在 `docker` 组内）
- `bash`、`flock`（util-linux）、`realpath`（coreutils）
- 首次使用会拉取 `espressif/idf:v5.2.1` 镜像（约数 GB，5–15 分钟）：

```bash
docker pull espressif/idf:v5.2.1
docker images espressif/idf          # 确认镜像存在
```

## idf-docker.sh

包装 `docker run`，把工程目录挂载到容器 `/project`，以当前用户 uid/gid 运行，
因此生成的 `build/`、`sdkconfig` 等文件宿主用户可直接读写，无需 `sudo chown`。

```bash
scripts/idf-docker.sh [options] <idf.py 参数...>
```

| 选项 | 说明 |
| --- | --- |
| `-C <dir>` | 工程根目录（默认：**当前工作目录**，可在任意 worktree 里直接运行） |
| `-B <dir>` | 构建目录，相对工程根（如 `build/esp32s3`）；会挂载保留、缺失时自动创建 |
| `-h` | 帮助 |

其余参数原样传给 `idf.py`。`-B` 也会转换成 `idf.py -B /project/<dir>`。

### 常用示例

```bash
# 版本自检
scripts/idf-docker.sh --version

# 编译 S3（在工程根运行；先 set-target 再 build）
scripts/idf-docker.sh -B build/esp32s3 -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 set-target esp32s3
scripts/idf-docker.sh -B build/esp32s3 build

# C3 / ESP32
scripts/idf-docker.sh -B build/esp32c3 -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32c3 set-target esp32c3
scripts/idf-docker.sh -B build/esp32c3 build
scripts/idf-docker.sh -B build/esp32   -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32   set-target esp32
scripts/idf-docker.sh -B build/esp32 build

# 在其它 worktree 里构建（脚本可任意位置调用，默认使用当前目录）
cd $HOME/birdiebot-wt/w0-foo
$HOME/birdiebot-wt/w0-env/scripts/idf-docker.sh -B build/esp32s3 build
```

### 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `IDF_DOCKER_IMAGE` | `espressif/idf:v5.2.1` | 使用的镜像 |
| `IDF_CCACHE_DIR` | `~/.cache/birdiebot-ccache` | 宿主机 ccache 目录，容器内挂载为 `/ccache` |
| `IDF_DOCKER_EXTRA_ARGS` | 空 | 追加给 `docker run` 的参数，例如 `--device=/dev/ttyUSB0 -v /dev:/dev` |

ccache 默认开启（`IDF_CCACHE_ENABLE=1`、`CCACHE_DIR=/ccache`、`CCACHE_BASEDIR=/project`），
第一次构建填充缓存，之后重复构建明显加速。清理缓存：`rm -rf ~/.cache/birdiebot-ccache`。

> **烧录/串口监视**：容器默认看不到串口设备，`flash`/`monitor` 请在宿主机执行，
> 或通过 `IDF_DOCKER_EXTRA_ARGS` 显式映射设备（如
> `IDF_DOCKER_EXTRA_ARGS="--device=/dev/ttyUSB0" scripts/idf-docker.sh -B build/esp32s3 flash`）。
> 容器内同时运行多个构建时请配合 `build-lock.sh` 使用。

## build-lock.sh

`flock` 构建锁：本机最多 **3 个**并发构建，其余排队等待，避免多个 worktree
同时编译把 CPU/内存打满。锁目录 `/tmp/birdiebot-buildlock`，进程退出（包括
被 kill）后 `flock` 自动释放，不会留下死锁。

```bash
scripts/build-lock.sh <命令...>
```

### 用法示例

```bash
# 推荐：构建一律套上构建锁
scripts/build-lock.sh scripts/idf-docker.sh -B build/esp32s3 build
scripts/build-lock.sh scripts/idf-docker.sh -B build/esp32c3 build
scripts/build-lock.sh make -j4

# 命令本身以 '-' 开头时用 -- 分隔
scripts/build-lock.sh -- some-tool --flag
```

### 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `BUILD_LOCK_DIR` | `/tmp/birdiebot-buildlock` | 锁文件目录 |
| `BUILD_LOCK_SLOTS` | `3` | 最大并发构建数 |
| `BUILD_LOCK_TIMEOUT` | `1800` | 等锁超时秒数，`0` = 无限等待 |
| `BUILD_LOCK_POLL` | `2` | 等锁轮询间隔秒数 |

### 退出码

| 码 | 含义 |
| --- | --- |
| 命令退出码 | 所运行命令自身的退出码 |
| `2` | 用法 / 参数错误 |
| `75` | 等锁超时（可用 `BUILD_LOCK_TIMEOUT` 调整） |
| `130` / `143` | 被 SIGINT / SIGTERM 中断 |

日志全部写 stderr（`[build-lock] ...`），不会污染 `idf.py` 的 stdout。

## 自检 / 验证

```bash
# 1) 镜像存在 + IDF 版本
docker images espressif/idf
scripts/idf-docker.sh --version

# 2) 构建锁：4 个 3 秒任务，第 4 个应排队到前 3 个之一结束后才启动
for i in 1 2 3 4; do
  scripts/build-lock.sh bash -c "echo START $i \$(date +%s); sleep 3; echo END $i \$(date +%s)" &
done
wait
# 期望：出现 "all 3 build slots busy; queued"；第 4 个 START 时间 >= 某个 END 时间

# 3) 锁状态（正在运行的槽位文件被 flock 占用）
ls -l /tmp/birdiebot-buildlock/
```

## 注意事项

- 提交约束：只提交本 `scripts/` 目录内容，**禁止** `git push` / 改 remote / 切分支 / merge / rebase。
- 脚本假设工程根包含 CMakeLists.txt 与 `sdkconfig.defaults*`；`-C` 指向的目录不对时
  `idf.py` 会自行报错，脚本只对缺少 `CMakeLists.txt` 给出 warning。
- `.dockerignore`、npm 缓存等属 WP-002 计划中的其它条目，不在本目录范围内
  （如需请由 Lead 统一新增，避免跨 WP 文件冲突）。
