# wireless-esp32-tools 网页用户界面

本项目是 [wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) 项目的网页用户界面。

`wireless-esp32-tools` 项目为多种ESP芯片提供了兼容CMSIS-DAP的无线调试工具，可将ESP芯片变为一个功能强大的无线调试器。

## 关于此网页界面

此 Web 应用程序提供了一套用于管理 `wireless-esp32-tools` 设备的内嵌管理面板，采用 Vue.js 3、Vite 和 Element Plus 构建，直接托管在 ESP32 上。**final slim 版本**：无 Web OTA/Update 页，无设备发现卡、首次使用引导与 Ozone 兼容；详细状态集中在 `/#/status` 设备状态页，其余页面顶部显示一行 compact 状态条。

### 主要功能

*   **Wi-Fi 配置**：终端模式(STA)与热点模式(AP)，支持静态/动态 IP 和 DNS 设置。
*   **目标烧录（`#/target`）**：浏览器端 STM32 检测/烧录（BIN/ELF/HEX/SREC），顶部 compact 状态条。
*   **设备状态（`#/status`）**：只读详情页（DAP/Wi-Fi/目标/链路融合/链路健康/错误摘要 + 诊断恢复）；其余页面顶部显示一行可点击的 compact 状态条。
*   **变量曲线（`#/debug-curve`）**：上传 ELF、选择变量、WS-DAP 最高 100 Hz 采样与实时曲线；设备本地采样上限 **64 系列**（C4）。
*   **实时数据可视化**：UART 数据查看器。
*   **嵌入式优先设计**：单文件 `.html` 部署，减少 http 连接数。
*   **适用于小屏幕**：响应式布局与触控友好的 UI。

## 界面截图

**UART 数据显示**

> 当前 final slim 的页面/状态条结构见 `docs/web-status-ui.md`；设备状态页截图可由设备实机或构建产物获取。

## 使用说明

### 最终用户

1.  将您的计算机或移动设备连接到运行 `wireless-esp32-tools` 的ESP32所承载的Wi-Fi网络。
2.  打开Web浏览器并导航到ESP32设备的IP地址（例如 `http://dap.local` 或 `http://192.168.1.1`(连接AP的情况下)）。

### 开发人员 (针对此Web UI)

请按照以下步骤设置项目以进行本地开发或构建以进行部署。

#### 环境准备

*   [Node.js](https://nodejs.org/) (v16 或更高版本)
*   [npm](https://www.npmjs.com/)

#### 本地开发

1.  **安装依赖：**
    ```sh
    npm install
    ```
2.  **运行开发服务器：**
    ```sh
    npm run dev
    ```
    这将在本地启动一个服务器，通常地址为 `http://localhost:5173`。

3.  **为移动/远程调试运行：**
    要从同一网络上的其他设备（如手机）访问开发服务器，请使用：
    ```sh
    npm run devh
    ```
    这将把服务器暴露给您的本地网络（例如 `http://192.168.X.X:5173`）。

#### 构建生产版本 (部署到ESP32)

1.  **构建项目：**
    ```sh
    npm run build
    ```
    此命令将编译和打包应用程序到 `dist/` 目录，生成 `index.html` 和 `ws.sharedworker.js`。

2.  **压缩输出文件：**
    进入 `dist/` 目录并使用gzip压缩构建产物。
    ```sh
    cd dist
    gzip *
    ```
    这将生成 `index.html.gz` 和 `ws.sharedworker.js.gz`。

3.  **部署到ESP32：**
    将压缩后的 `.gz` 文件复制到您的 `wireless-esp32-tools` ESP-IDF项目中的相应目录（例如 `project_components/html/`），然后将固件刷入您的设备。

### Web OTA UI 已移除（精简构建）

OTA 本地上传 UI（Update 页、导航项、OTA(5) WS 模块、`POST /api/ota/upload` 客户端）
已按契约**删除**，见仓库根目录 `docs/feature-flags.md` 的 **A1** 条。`VITE_ENABLE_OTA`
构建开关不再存在：精简固件关闭 OTA handler，Web 端仅保留固件/协议版本信息（About 页）。

如需恢复，可从 `backup/full-features-2026-10-26` tag 取回代码
（`git checkout backup/full-features-2026-10-26 -- web/src/views/Update.vue ...`），并
按 `docs/feature-flags.md` 恢复路由/导航/i18n key。

## 许可证

根据MIT许可证分发。更多信息请参见 `LICENCE.txt` 文件。 