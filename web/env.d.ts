/// <reference types="vite/client" />

/**
 * Build metadata injected by scripts/gen-env.mjs（docs/versioning.md §4.2）。
 */
interface ImportMetaEnv {
    /** `web/package.json` 的 version（构建必需） */
    readonly VITE_APP_WEB_VERSION: string
    /** 仓库根的 `git describe --tags --dirty --always`（溯源信息，可回退） */
    readonly VITE_APP_GIT_TAG: string
    /** 仓库根 HEAD 短哈希（溯源信息，可回退） */
    readonly VITE_APP_GIT_COMMIT: string
    /** 最近一次提交时间（沿用 About 页原有语义） */
    readonly VITE_APP_LAST_COMMIT: string
    /** `dev` / `prod`（沿用 D 的 .env.* 约定） */
    readonly VITE_APP_MODE: string
    /** 开发模式下连接的真实设备地址 */
    readonly VITE_DEVICE_HOST_NAME?: string
    /** `true` 时不在 App.vue 建立 WS（离线预览/截图用） */
    readonly VITE_DISABLE_CONNECTION?: string
    /** 构建期功能裁剪开关，缺省启用 */
    readonly VITE_ENABLE_UART?: string
    readonly VITE_ENABLE_DATA_FLOW?: string
}

interface ImportMeta {
    readonly env: ImportMetaEnv
}
