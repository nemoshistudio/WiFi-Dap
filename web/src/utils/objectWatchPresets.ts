/**
 * Objects/Watch 预设骨架（task-53，等 task-52 ObjectBrowser API）。
 *
 * 只负责按“根对象路径”显式保存/加载/删除字段路径集合到 localStorage；
 * 不保存变量选择、不自动保存。ObjectBrowser 组件 API 落盘后由 DebugCurve
 * Objects tab 直接复用这些纯函数（不依赖组件实现）。
 */
export interface ObjectWatchPreset {
    id: string
    name: string
    rootPath: string
    fields: string[]
    createdAt: number
}

const KEY = 'curve.objectWatchPresets'

function readAll(): ObjectWatchPreset[] {
    try {
        const raw = typeof localStorage !== 'undefined' ? localStorage.getItem(KEY) : null
        if (!raw) return []
        const parsed = JSON.parse(raw) as ObjectWatchPreset[]
        return Array.isArray(parsed) ? parsed.filter((item) => item && typeof item.id === 'string'
            && typeof item.rootPath === 'string' && Array.isArray(item.fields)) : []
    } catch {
        return []
    }
}

function writeAll(presets: readonly ObjectWatchPreset[]): void {
    try {
        if (typeof localStorage !== 'undefined') localStorage.setItem(KEY, JSON.stringify(presets))
    } catch { /* storage unavailable */ }
}

/** 按 root 取预设（默认根对象由页面提供）。 */
export function listObjectWatchPresets(rootPath?: string): ObjectWatchPreset[] {
    const all = readAll()
    return rootPath ? all.filter((item) => item.rootPath === rootPath) : all
}

export function saveObjectWatchPreset(input: {name: string; rootPath: string; fields: readonly string[]}): ObjectWatchPreset {
    const preset: ObjectWatchPreset = {
        id: `obj-preset-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
        name: input.name.trim() || input.rootPath,
        rootPath: input.rootPath,
        fields: [...new Set(input.fields)],
        createdAt: Date.now(),
    }
    writeAll([preset, ...readAll().filter((item) => !(item.rootPath === preset.rootPath && item.name === preset.name))])
    return preset
}

export function deleteObjectWatchPreset(id: string): void {
    writeAll(readAll().filter((item) => item.id !== id))
}
