import {type ApiJsonMsg, sendJsonMsg, WtModuleID, WtOtaCmd} from '@/api'

/**
 * OTA(5) 与固件本地上传协议（docs/protocol.md §2.5 / docs/ota-local-upload.md）。
 *
 * - WS JSON：GET_INFO=1 / GET_PROGRESS=3 / ABORT=4
 * - HTTP：POST /api/ota/upload（Body = app .bin 原始镜像，Content-Length 由浏览器对
 *   Blob/File 自动生成；**绝不**用 ReadableStream/chunked，见契约 §3.1/§7.2）
 *
 * 设备以 HTTP 明文提供页面（非 secure context），前端**不依赖** SubtleCrypto 校验；
 * 设备在 HTTP 响应与 GET_PROGRESS 中回传 sha256 供人工比对。
 */
export const OTA_UPLOAD_PATH = '/api/ota/upload'
/** 最小合法 ESP 应用镜像（契约 §3.1：4096 ≤ Content-Length ≤ slot_size） */
export const OTA_IMAGE_MIN_SIZE = 4096
/** 未知槽大小时的默认上限（8MB 分区表 ota 槽 3MB，见 ota-local-upload §1.1） */
export const OTA_DEFAULT_SLOT_SIZE = 0x300000

export enum OtaStatus {
    IDLE = "IDLE",
    RECEIVING = "RECEIVING",
    VERIFYING = "VERIFYING",
    READY = "READY",
    APPLYING = "APPLYING",
    ERROR = "ERROR",
}

/** OTA/GET_INFO（cmd 1）响应（docs/versioning.md §6.1 / ota-local-upload §4.1）。 */
export interface IOtaInfo extends ApiJsonMsg {
    /** 当前运行分区标签：ota_0 / ota_1 */
    running: string;
    /** 当前运行分区固件版本（应等于 GET_FM_INFO.fm_ver） */
    running_ver: string;
    /** 目标槽可写字节数 */
    free: number;
    /** 目标槽总大小（前端选文件时校验大小） */
    slot_size: number;
    /** 状态机当前状态 */
    status: OtaStatus;
    /** 可选增量字段（ota-local-upload §4.1） */
    idle?: string;
    rollback?: boolean;
    last_error?: string;
    /** 设备回传已写入镜像的 sha256，供人工比对 */
    sha256?: string;
}

/** OTA/GET_PROGRESS（cmd 3）响应（docs/versioning.md §6.2 / ota-local-upload §4.2）。 */
export interface IOtaProgress extends ApiJsonMsg {
    offset: number;
    total: number;
    status: OtaStatus;
    /** 仅 ERROR 时非空（英文错误码/描述，不参与 i18n） */
    err?: string;
    slot?: string;
    sha256?: string;
}

/** POST /api/ota/upload 成功响应（HTTP 200；cmd=2 仅用于上传关联，见契约 §3.4）。 */
export interface IOtaUploadSuccess extends ApiJsonMsg {
    ok: boolean;
    slot?: string;
    running?: string;
    running_ver?: string;
    offset?: number;
    total?: number;
    sha256?: string;
    status?: OtaStatus;
    reboot_in_ms?: number;
}

/** HTTP 上传失败信息（HTTP 状态码承担细分语义，JSON code 仅 2–6，契约 §3.4）。 */
export interface IOtaHttpError {
    httpStatus: number;
    code?: number;
    err?: string;
}

export interface IOtaUploadHandlers {
    /** 浏览器上传进度（XHR upload.onprogress） */
    onUploadProgress?: (loaded: number, total: number) => void;
    onSuccess: (result: IOtaUploadSuccess) => void;
    onFailure: (failure: IOtaHttpError) => void;
}

export interface IOtaUploadHandle {
    abort(): void;
}

/** 从 app 镜像头解析出的展示信息（契约 §6.4；解析失败不阻止上传）。 */
export interface IOtaImageInfo {
    valid: boolean;
    /** 镜像头 magic（0xE9） */
    magicOk: boolean;
    /** esp_app_desc.magic_word（0xABCD5432） */
    descOk: boolean;
    version: string;
    project: string;
    buildDate: string;
    buildTime: string;
}

function readCString(bytes: Uint8Array, offset: number, length: number): string {
    const slice = bytes.subarray(offset, offset + length)
    const nul = slice.indexOf(0)
    const end = nul === -1 ? slice.length : nul
    return new TextDecoder().decode(slice.subarray(0, end)).trim()
}

/**
 * 解析 ESP-IDF 应用镜像头（只读前 0xD0 字节，不读取整个文件）。
 *
 * 布局（esp_app_desc_t，ESP-IDF 5.x）：0x20 magic_word、0x30 version[32]、
 * 0x50 project_name[32]、0x70 time[16]、0x80 date[16]。
 */
export async function parseEspAppImage(file: File): Promise<IOtaImageInfo> {
    const empty: IOtaImageInfo = {
        valid: false,
        magicOk: false,
        descOk: false,
        version: '',
        project: '',
        buildDate: '',
        buildTime: '',
    }
    try {
        const buffer = await file.slice(0, 0xd0).arrayBuffer()
        const bytes = new Uint8Array(buffer)
        if (bytes.length < 0x90) {
            return empty
        }
        const magicOk = bytes[0] === 0xe9
        const magicWord = (bytes[0x20] | (bytes[0x21] << 8) | (bytes[0x22] << 16) | (bytes[0x23] << 24)) >>> 0
        const descOk = magicWord === 0xabcd5432
        const version = readCString(bytes, 0x30, 32)
        const project = readCString(bytes, 0x50, 32)
        const buildTime = readCString(bytes, 0x70, 16)
        const buildDate = readCString(bytes, 0x80, 16)
        return {
            // app 镜像必须同时满足 0xE9 头与 esp_app_desc magic
            valid: magicOk && descOk,
            magicOk,
            descOk,
            version,
            project,
            buildDate,
            buildTime,
        }
    } catch {
        return empty
    }
}

function parseJsonSafe(text: string): Record<string, unknown> | null {
    try {
        const parsed = JSON.parse(text)
        return typeof parsed === 'object' && parsed !== null ? parsed as Record<string, unknown> : null
    } catch {
        return null
    }
}

/**
 * 浏览器本地固件上传（XHR，用于获取 upload.onprogress）。
 *
 * - Body 直接传 File：浏览器自动生成 Content-Length，避免 chunked（契约 §3.1）
 * - 不计算/不发送 X-SHA256（HTTP 明文非 secure context，契约 §6.3 允许降级）
 */
export function wt_ota_upload(file: File, handlers: IOtaUploadHandlers): IOtaUploadHandle {
    const xhr = new XMLHttpRequest()
    let finished = false

    function settleFailure(failure: IOtaHttpError) {
        if (finished) {
            return
        }
        finished = true
        handlers.onFailure(failure)
    }

    xhr.open('POST', OTA_UPLOAD_PATH, true)
    xhr.setRequestHeader('Content-Type', 'application/octet-stream')

    xhr.upload.onprogress = (event: ProgressEvent) => {
        if (event.lengthComputable && handlers.onUploadProgress) {
            handlers.onUploadProgress(event.loaded, event.total)
        }
    }

    xhr.onload = () => {
        if (finished) {
            return
        }
        const parsed = parseJsonSafe(xhr.responseText || '')
        const ok = xhr.status >= 200 && xhr.status < 300 && parsed?.ok !== false
        if (ok) {
            finished = true
            handlers.onSuccess((parsed ?? {module: 5, cmd: 2, ok: true}) as unknown as IOtaUploadSuccess)
        } else {
            settleFailure({
                httpStatus: xhr.status,
                code: typeof parsed?.code === 'number' ? parsed.code as number : undefined,
                err: typeof parsed?.err === 'string' ? parsed.err as string : undefined,
            })
        }
    }
    xhr.onerror = () => settleFailure({httpStatus: 0, err: 'network error'})
    xhr.ontimeout = () => settleFailure({httpStatus: 0, err: 'upload timeout'})
    xhr.onabort = () => settleFailure({httpStatus: 0, err: 'aborted'})

    xhr.send(file)

    return {
        abort() {
            if (!finished) {
                finished = true
                xhr.abort()
            }
        },
    }
}

export function wt_ota_get_info() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.OTA,
        cmd: WtOtaCmd.GET_INFO,
    };
    sendJsonMsg(msg);
}

export function wt_ota_get_progress() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.OTA,
        cmd: WtOtaCmd.GET_PROGRESS,
    };
    sendJsonMsg(msg);
}

export function wt_ota_abort() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.OTA,
        cmd: WtOtaCmd.ABORT,
    };
    sendJsonMsg(msg);
}
