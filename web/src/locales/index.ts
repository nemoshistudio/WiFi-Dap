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
