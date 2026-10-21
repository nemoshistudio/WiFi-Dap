/**
 * Device-side DAP session takeover endpoint used by the curve page.
 *
 * 固件契约（真机验证 2026-10-21，WiFi DAP fw v1.2.0-19）：
 *   POST /api/dap/kick        （同源；无请求体 / 空 JSON）
 *     -> 200 {"module":7,"cmd":1,"ok":true,"kicked":true}
 *        kicked=true 表示确实断开了一个占用中的 USBIP/WS-DAP 会话，false 表示本来空闲。
 *     -> 4xx/5xx {"ok":false,"err":"<reason>"}      失败（占用/内部错误）
 *     -> 404                                        固件未实现该端点
 *
 * 成功语义：固件断开当前 USBIP（或 elaphureLink）DAP 会话的 TCP 连接，释放
 * DAP 任务与 ring buffer，使紧接着的 WS-DAP 连接可以接管。真机时序：kick 后
 * 等 ~300ms 再连 `ws://<host>:3240/`；USBIP 仍占用时 WS 握手会超时，因此页面
 * 侧会重试握手最多约 2s。前端在 kick 失败时仍展示“USBIP/DAP 会话占用中”
 * 提示并继续尝试 WS-DAP（用户可先手动断开 USBIP 后重试）。
 *
 * dev 模式（页面由 Vite dev server 提供）下没有同源设备，使用
 * VITE_DEVICE_HOST_NAME 直接请求设备 HTTP 端口；失败时按普通网络错误处理。
 */

export interface DapKickResult {
    ok: boolean
    /** 固件是否实现了该端点（404 => false）。 */
    supported: boolean
    httpStatus: number
    /** 是否真的断开了一个占用中的 DAP 会话（固件字段 kicked）。 */
    kicked: boolean
    /** 固件响应里的模块/命令号（module=7, cmd=1），用于诊断。 */
    module?: number
    cmd?: number
    err?: string
}

/** 设备 HTTP API 基地址：生产环境用同源相对路径；dev 模式指向真实设备。 */
function deviceApiUrl(path: string): string {
    if (import.meta.env.VITE_APP_MODE === 'dev') {
        const host = import.meta.env.VITE_DEVICE_HOST_NAME
        if (host) {
            const withoutScheme = host.replace(/^https?:\/\//, '')
            return `http://${withoutScheme}${path}`
        }
    }
    return path
}

/** POST /api/dap/kick：请求固件让出 DAP 会话，供 WS-DAP 曲线页接管。 */
export async function kickDapSession(): Promise<DapKickResult> {
    try {
        const response = await fetch(deviceApiUrl('/api/dap/kick'), {
            method: 'POST',
            cache: 'no-store',
        })
        let body: Record<string, unknown> | null = null
        try {
            const text = await response.text()
            if (text) {
                const parsed = JSON.parse(text)
                if (typeof parsed === 'object' && parsed !== null) {
                    body = parsed as Record<string, unknown>
                }
            }
        } catch {
            body = null
        }
        const err = typeof body?.err === 'string' && body.err ? (body.err as string) : undefined
        return {
            ok: response.ok && body?.ok !== false,
            supported: response.status !== 404,
            httpStatus: response.status,
            kicked: body?.kicked === true,
            module: typeof body?.module === 'number' ? (body.module as number) : undefined,
            cmd: typeof body?.cmd === 'number' ? (body.cmd as number) : undefined,
            err: err ?? (response.ok ? undefined : `HTTP ${response.status}`),
        }
    } catch (error) {
        return {
            ok: false,
            supported: true,
            httpStatus: 0,
            kicked: false,
            err: error instanceof Error ? error.message : 'network error',
        }
    }
}
