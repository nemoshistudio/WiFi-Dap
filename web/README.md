# Web UI for wireless-esp32-tools

This is the web user interface for the [wireless-esp32-tools](https://github.com/kerms/wireless-esp32-tools) project.

The `wireless-esp32-tools` project provides CMSIS-DAP compatible wireless debugging tools for various ESP chips, turning them into powerful wireless debug probes.

> The former Widget / network-editor dashboard page was removed on 2026-10-21; the UART page (terminal, data viewer, macros, loop send) remains and is the primary data-path UI.

## About This Web Interface

This web application provides a user-friendly dashboard to manage and interact with your `wireless-esp32-tools` device. It is built with Vue.js 3, Vite, and Element Plus, and is designed to be hosted directly on the ESP32.

From this interface you can monitor and interact with your target device.

### Key Features

*   **Wi-Fi Configuration**: Easily configure Wi-Fi settings including station mode (STA) and access point mode (AP), with support for static/dynamic IP addressing and DNS settings.
*   **Real-time Data Visualization**: Includes a UART data viewer for monitoring serial communication from your target device.
*   **Target Flash (`#/target`)**: Browser-side STM32 detect + flash (BIN/ELF/HEX/SREC); compact status strip on top.
*   **Device Status (`#/status`)**: Read-only detail page for DAP / Wi-Fi / target / link fusion / link health / errors + diagnostics/recovery. Every other page shows one clickable compact status strip.
*   **Variable Curves (`#/debug-curve`)**: Upload an ELF, select RAM/Flash variables, sample at up to 100 Hz over WS-DAP and plot live curves (CSV/PNG export); device-local sampling is capped at **64 series** (C4). See `docs/web-variable-curve.md`.
*   **Final slim**: no Web OTA/Update page, device-discovery card, first-run guide, online-manual entry or Ozone compatibility (see `docs/feature-flags.md` and the root README).
*   **Embedded-First Design**: Deploy in single .html file to reduce the number of http connection.
*   **Designed to be usable on small screen**: Responsive layout that adapts to mobile devices and tablets, with touch-friendly controls and optimized UI elements for small displays.

## Screenshots

**UART Data Viewer**

## Getting Started

### For End-Users

1.  Connect your computer or mobile device to the Wi-Fi network hosted by the ESP32 running `wireless-esp32-tools`.
2.  Open a web browser and navigate to the IP address of the ESP32 device (e.g., `http://dap.local` or `http://192.168.1.1`(when connected to the AP)).

### For Developers (of this Web UI)

Follow these steps to set up the project for local development or to build it for deployment.

#### Prerequisites

*   [Node.js](https://nodejs.org/) (v16 or later)
*   [npm](https://www.npmjs.com/)

#### Local Development

1.  **Install dependencies:**
    ```sh
    npm install
    ```
2.  **Run the development server:**
    ```sh
    npm run dev
    ```
    This will start a local server, typically at `http://localhost:5173`.

3.  **Run for mobile/remote debugging:**
    To access the development server from other devices on the same network (like a mobile phone), use:
    ```sh
    npm run devh
    ```
    This will expose the server to your local network (e.g., `http://192.168.X.X:5173`).

#### Building for Production (Deploying to ESP32)

1.  **Build the project:**
    ```sh
    npm run build
    ```
    This command compiles and bundles the application into the `dist/` directory, creating `index.html` and `ws.sharedworker.js`.

    There is some bugs on Firefox or Vue3 when embbeding sharedWorker in single file mode. So at the time, the sharedWorker are build separately.

2.  **Compress the output files:**
    Navigate into the `dist/` directory and compress the build artifacts using gzip.
    ```sh
    cd dist
    gzip *
    ```
    This will generate `index.html.gz` and `ws.sharedworker.js.gz`.

3.  **Deploy to ESP32:**
    Copy the compressed `.gz` files to the appropriate directory in your `wireless-esp32-tools` ESP-IDF project (e.g., `project_components/html/`) and flash the firmware to your device.

### Web OTA UI removed (slim build)

The OTA local-upload UI (Update page, nav entry, OTA(5) WS module, `POST /api/ota/upload`
client) was **removed** by contract — see `docs/feature-flags.md` item **A1** in the
repository root. `VITE_ENABLE_OTA` no longer exists: the slim firmware disables the OTA
handlers, and the Web UI only keeps the firmware/protocol version info (About page).

Restore the full feature from the `backup/full-features-2026-10-26` tag if needed
(`git checkout backup/full-features-2026-10-26 -- web/src/views/Update.vue ...`, then
re-add the route/nav/i18n keys documented in `docs/feature-flags.md`).

## License

Distributed under the MIT License. See `LICENCE.txt` for more information.
