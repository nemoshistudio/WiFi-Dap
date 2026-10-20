/**
 * `vue-grid-layout-v3@3.1.2` 上游包没有发布类型声明（package.json 无 types 字段），
 * 这里提供 WP-136 所需的最小模块声明：WidgetPannel.vue 只用到两个具名组件。
 *
 * 若后续升级依赖并自带类型，可删除本文件。
 */
declare module 'vue-grid-layout-v3' {
    import type {DefineComponent} from 'vue'

    /** 网格容器：props 由调用方以 kebab-case 传入，事件/实例方法由组件内部维护。 */
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    export const GridLayout: DefineComponent<any, any, any>
    /** 网格单元：沿用 vue-grid-layout 的 x/y/w/h/i/layout 属性。 */
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    export const GridItem: DefineComponent<any, any, any>
}
