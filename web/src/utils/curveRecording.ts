/**
 * IndexedDB 录制存储（task-37）：保存一次显式录制会话的 series 元数据与原始帧。
 * 不自动录制、不保存变量选择；只有用户点击“保存录制”才写入。
 */
import type {AddressExpr, SampleLeaf} from './elfTypes'

export interface RecordingVariantData {
    pcLow: number
    pcHigh: number
    expr: AddressExpr
}

export interface RecordingSeriesData {
    name: string
    key: string
    typeName: string
    address: number
    size: number
    sample: SampleLeaf
    format: string
    color: string
    visible: boolean
    location?: AddressExpr
    register?: number
    pcLow?: number
    pcHigh?: number
    loclistMulti?: boolean
    variants?: RecordingVariantData[]
}

export interface RecordingRow {
    /** 相对录制开始的秒数（ESP32/浏览器时间轴）。 */
    time: number
    group: number
    /** 按录制时 series 顺序的原始 payload（每项 = 该 series 的 size 字节）。 */
    payloads: Uint8Array[]
    /** 设备帧 seq / 当前 PC（task-39/35；录制回放/导出诊断用）。 */
    seq?: number
    pc?: number
}

export type RecordingTriggerKind = 'periodic' | 'breakpoint' | 'watchpoint' | 'condition'

export interface CurveRecordingData {
    id: string
    name: string
    createdAt: number
    series: RecordingSeriesData[]
    rows: RecordingRow[]
    /** 录制会话触发方式与 watchpoint trigger seq（可选，兼容旧记录）。 */
    trigger?: RecordingTriggerKind
    triggerSeq?: number | null
    watchpoint?: {address: number; size: number; access: string} | null
}

export interface RecordingMeta {
    id: string
    name: string
    createdAt: number
    seriesCount: number
    rowCount: number
    duration: number
}

const DB_NAME = 'curve-recordings'
const STORE = 'recordings'
const DB_VERSION = 1

function openDb(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
        if (typeof indexedDB === 'undefined') {
            reject(new Error('IndexedDB unavailable'))
            return
        }
        const request = indexedDB.open(DB_NAME, DB_VERSION)
        request.onupgradeneeded = () => {
            const db = request.result
            if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, {keyPath: 'id'})
        }
        request.onsuccess = () => resolve(request.result)
        request.onerror = () => reject(request.error ?? new Error('IndexedDB open failed'))
    })
}

function requestToPromise<T>(request: IDBRequest<T>): Promise<T> {
    return new Promise((resolve, reject) => {
        request.onsuccess = () => resolve(request.result)
        request.onerror = () => reject(request.error ?? new Error('IndexedDB request failed'))
    })
}

export async function listRecordings(): Promise<RecordingMeta[]> {
    const db = await openDb()
    try {
        const store = db.transaction(STORE, 'readonly').objectStore(STORE)
        const all = await requestToPromise(store.getAll() as IDBRequest<CurveRecordingData[]>)
        return all.map((rec) => ({
            id: rec.id,
            name: rec.name,
            createdAt: rec.createdAt,
            seriesCount: rec.series.length,
            rowCount: rec.rows.length,
            duration: rec.rows.length > 0 ? rec.rows[rec.rows.length - 1].time : 0,
        })).sort((left, right) => right.createdAt - left.createdAt)
    } finally {
        db.close()
    }
}

export async function saveRecording(data: CurveRecordingData): Promise<void> {
    const db = await openDb()
    try {
        const store = db.transaction(STORE, 'readwrite').objectStore(STORE)
        await requestToPromise(store.put(data))
    } finally {
        db.close()
    }
}

export async function loadRecording(id: string): Promise<CurveRecordingData | null> {
    const db = await openDb()
    try {
        const store = db.transaction(STORE, 'readonly').objectStore(STORE)
        const result = await requestToPromise(store.get(id) as IDBRequest<CurveRecordingData | undefined>)
        return result ?? null
    } finally {
        db.close()
    }
}

export async function deleteRecording(id: string): Promise<void> {
    const db = await openDb()
    try {
        const store = db.transaction(STORE, 'readwrite').objectStore(STORE)
        await requestToPromise(store.delete(id))
    } finally {
        db.close()
    }
}

/** 浏览器内生成录制 id（不依赖 crypto.randomUUID 的旧环境兼容）。 */
export function newRecordingId(): string {
    return `rec-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`
}
