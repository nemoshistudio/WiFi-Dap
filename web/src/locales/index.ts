import i18n from '@/i18n';
import zh from '@/locales/zh';

type NestedKeyOf<ObjectType extends object> = {
    [Key in keyof ObjectType & (string | number)]: ObjectType[Key] extends object
        ? `${Key}` | `${Key}.${NestedKeyOf<ObjectType[Key]>}`
        : `${Key}`
}[keyof ObjectType & (string | number)];

/**
 * 全量 i18n key（以 zh 为唯一结构源，docs/i18n-keys.md §2.3）。
 * 不允许再退化为 `| string`，否则缺 key / 拼错 key 无法在构建期发现。
 */
export type TranslationKeys = NestedKeyOf<typeof zh>;

/**
 * 翻译函数。
 *
 * @param key   `TranslationKeys` 字面量（禁止动态拼接，契约 §2.2）
 * @param named 可选命名插值参数，如 `{web: 1, fw: 2}`；占位符白名单见契约附录 B
 */
export function translate(key: TranslationKeys, named?: Record<string, unknown>): string {
    return named ? i18n.global.t(key, named) : i18n.global.t(key);
}

/**
 * 取**未插值**的原始文案（占位符保持 `{name}` 形式，随 locale 响应式更新）。
 *
 * 用于把文案交给不依赖 i18n 的组件、由其自行格式化占位符的场景：
 * SourceBrowser labels 由页面注入，组件侧 `formatSourceBrowserLabel()` 负责
 * `{name}/{count}/...` 替换。直接用 `translate()` 会被 vue-i18n 当作命名插值，
 * 未传参时占位符被替换为空串。
 */
export function translateRaw(key: TranslationKeys): string {
    const message = i18n.global.tm(key) as unknown;
    return typeof message === 'string' ? message : key;
}
