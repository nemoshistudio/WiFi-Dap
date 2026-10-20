/**
 * Web 鉴权会话（WP-209f，docs/protocol.md §1/§5）。
 *
 * - 唯一 token 来源：`?token=`（首次进入自动持久化）或 localStorage `birdiebot.token`
 *   （Wave 1 auth.ts 已固化的 storage key，本文件沿用，不迁移）。
 * - NavBar/AuthDialog/App banner/Update/Wifi 共用本模块的响应式状态。
 * - token 变更后 `epoch` 自增；App.vue 监听后重连 WS（URL ?token= + 首帧 SYSTEM/0/AUTH）。
 * - 默认无 token（实验室模式）不打扰用户：不弹窗、不显示 banner；仅手动点锁图标
 *   或收到 401/403/JSON code=7 时提示。
 */
import {computed, reactive} from 'vue'
import {AUTH_TOKEN_STORAGE_KEY, consumeUrlAuthToken, normalizeAuthToken, resolveAuthToken} from '@/composables/websocket/auth'
import {globalNotify} from '@/composables/notification'
import {translate} from '@/locales'

interface AuthSessionState {
    /** 当前生效的 pairing token（空串 = 实验室模式/未登录） */
    token: string
    /** token 变更计数（App.vue watch 后重连 WS） */
    epoch: number
    /** 设备返回未授权（JSON code=7 / HTTP 401/403）后置位，直到 token 变更 */
    unauthorized: boolean
    dialogVisible: boolean
    /** 弹窗输入草稿 */
    draftToken: string
    /** URL 曾携带 ?token= 且已被清除（App.vue 挂载后提示“token 已保存”一次） */
    urlTokenConsumed: boolean
}

/** 启动时消费 URL token：清理地址栏并持久化（安全审查 B，见 consumeUrlAuthToken） */
const urlToken = consumeUrlAuthToken()

export const authState = reactive<AuthSessionState>({
    token: urlToken || resolveAuthToken(),
    epoch: 0,
    unauthorized: false,
    dialogVisible: false,
    draftToken: '',
    urlTokenConsumed: urlToken.length > 0,
})

/** 是否已配置 pairing token（响应式，供 NavBar/Update/Wifi 使用）。 */
export const hasToken = computed(() => authState.token.length > 0)

/** 未授权提示节流（重连/多命令时不刷屏） */
const UNAUTHORIZED_NOTIFY_INTERVAL_MS = 5000
let lastUnauthorizedNotifyAt = 0

function persistToken(token: string) {
    try {
        if (token) {
            localStorage.setItem(AUTH_TOKEN_STORAGE_KEY, token)
        } else {
            localStorage.removeItem(AUTH_TOKEN_STORAGE_KEY)
        }
    } catch {
        // localStorage 不可用（隐私模式）：token 仅本次页面会话有效，不阻塞使用
    }
}

export function openAuthDialog() {
    authState.draftToken = authState.token
    authState.dialogVisible = true
}

export function closeAuthDialog() {
    authState.dialogVisible = false
}

/**
 * 保存 token 并触发重连。
 * @returns 是否保存成功（空/非法 token 返回 false 并提示 auth.invalidToken）
 */
export function saveAuthToken(raw: string): boolean {
    const token = normalizeAuthToken(raw)
    if (!token) {
        globalNotify(translate('auth.invalidToken'), 'warning')
        return false
    }
    persistToken(token)
    authState.token = token
    authState.unauthorized = false
    authState.dialogVisible = false
    authState.epoch += 1
    globalNotify(translate('common.success'), 'success')
    return true
}

/** 清除 token（退出鉴权）并触发重连；之后回到实验室模式。 */
export function clearAuthToken() {
    persistToken('')
    authState.token = ''
    authState.unauthorized = false
    authState.dialogVisible = false
    authState.epoch += 1
    globalNotify(translate('common.success'), 'success')
}

/**
 * 设备报告未授权：置位 + 节流提示 + 打开 token 输入框（不阻塞页面）。
 * JSON code=7（msgRouter）与 HTTP 401/403（OTA 上传）共用。
 */
export function markUnauthorized() {
    authState.unauthorized = true
    const now = Date.now()
    if (now - lastUnauthorizedNotifyAt > UNAUTHORIZED_NOTIFY_INTERVAL_MS) {
        lastUnauthorizedNotifyAt = now
        globalNotify(translate('auth.required'), 'warning')
    }
    if (!authState.dialogVisible) {
        openAuthDialog()
    }
}

/** HTTP 状态码是否为鉴权失败（驱动 token 输入提示）。 */
export function isHttpAuthFailure(status: number): boolean {
    return status === 401 || status === 403
}
