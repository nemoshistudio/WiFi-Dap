import { createI18n } from 'vue-i18n'
import zh from '@/locales/zh'
import en from '@/locales/en'

// Web UI 只支持中英双语（契约 v1，见 docs/i18n-keys.md §1）
export const availableLanguages = ['zh', 'en'] as const
export type Locale = (typeof availableLanguages)[number]

export const messages = { zh, en } as const

const LOCALE_STORAGE_KEY = 'wifi-dap.lang'

function isLocale(value: unknown): value is Locale {
    return value === 'zh' || value === 'en'
}

function readStoredLocale(): { present: boolean; locale: Locale | null } {
    try {
        const stored = localStorage.getItem(LOCALE_STORAGE_KEY)
        if (stored === null) return { present: false, locale: null }
        return { present: true, locale: isLocale(stored) ? stored : null }
    } catch {
        // localStorage 不可用（隐私模式/非浏览器环境）时忽略持久化
        return { present: false, locale: null }
    }
}

function detectLocale(): Locale {
    const stored = readStoredLocale()
    // 契约 §1.2：非法持久化语言码忽略并回退 zh
    if (stored.present) return stored.locale ?? 'zh'

    const lang = (typeof navigator !== 'undefined' && navigator.language ? navigator.language : 'zh').toLowerCase()
    if (lang.startsWith('zh')) return 'zh'
    if (lang.startsWith('en')) return 'en'
    return 'zh' // 缺省回退：中文
}

export const locale = detectLocale()

const i18n = createI18n({
    legacy: false,
    globalInjection: true,
    locale,
    fallbackLocale: 'zh', // 缺省回退 zh（必填）
    missingWarn: false, // 构建期已保证 key 对齐，运行期不刷缺失警告
    fallbackWarn: false,
    messages,
})

export function getFlagFromLang(lang: string) {
    if (lang === 'zh') {
        return '🇨🇳'
    } else if (lang === 'en') {
        return '🇺🇸'
    }
    return '🏳️'
}

export function setLang(lang: string): void {
    if (!isLocale(lang)) return
    i18n.global.locale.value = lang
    try {
        localStorage.setItem(LOCALE_STORAGE_KEY, lang)
    } catch {
        // 持久化失败不影响本次切换
    }
}

export function getLang() {
    return i18n.global.locale
}

export default i18n
