/**
 * 不翻译的专有名词（docs/i18n-keys.md §6.1 受控白名单）。
 *
 * 本文件只放品牌名、作者名等固定名称；禁止放 UI 句子/提示语。
 * check-i18n.mjs 会跳过 `src/constants/**`，因此这里允许出现中文专有名词。
 */
export const BRAND_NAME = 'WiFi Dap'
export const BRAND_SHORT_NAME = 'WiFi Dap'
export const AUTHOR_NAME = '空空(kerms)'
export const AUTHOR_HOMEPAGE = 'https://yunsi.studio/'
export const PROJECT_HOMEPAGE = 'https://yunsi.studio/wireless-debugger'
