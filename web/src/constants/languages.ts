/**
 * 语言选项（docs/i18n-keys.md §1.1 / §6.3）。
 *
 * 语言名以母语显示、不随当前 locale 翻译，属于契约允许的硬编码例外，
 * 因此固定放在本受控白名单文件中；Web UI 只允许 zh / en 两种语言。
 */
export type LanguageCode = 'zh' | 'en'

export interface LanguageOption {
    code: LanguageCode
    /** 母语名（不翻译） */
    label: string
    /** 国旗 emoji，仅用于下拉展示 */
    flag: string
}

export const LANGUAGES: readonly LanguageOption[] = [
    { code: 'zh', label: '中文', flag: '🇨🇳' },
    { code: 'en', label: 'English', flag: '🇺🇸' },
] as const

export function getLanguageOption(code: string): LanguageOption | undefined {
    return LANGUAGES.find((language) => language.code === code)
}
