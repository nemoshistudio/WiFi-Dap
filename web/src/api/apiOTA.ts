import {type ApiJsonMsg, sendJsonMsg, WtModuleID, WtOtaCmd} from '@/api'

/**
 * OTA(5) 与固件本地上传协议（docs/protocol.md §2.5 / docs/ota-local-upload.md）。
 *
 * Wave 1 只预置 JSON 命令与状态模型；浏览器上传（POST /api/ota/upload）与
 * Update 页交互属 Wave 2 的 WP-207，不在本 WP 实现。
 */
export enum OtaStatus {
    IDLE = "IDLE",
    RECEIVING = "RECEIVING",
    VERIFYING = "VERIFYING",
    READY = "READY",
    APPLYING = "APPLYING",
    ERROR = "ERROR",
}

/** OTA/GET_INFO（cmd 1）响应（docs/versioning.md §6.1）。 */
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
    /** 设备回传已写入镜像的 sha256，供人工比对（前端不依赖 SubtleCrypto） */
    sha256?: string;
}

/** OTA/GET_PROGRESS（cmd 3）响应（docs/versioning.md §6.2）。 */
export interface IOtaProgress extends ApiJsonMsg {
    offset: number;
    total: number;
    status: OtaStatus;
    /** 仅 ERROR 时非空（英文错误码/描述） */
    err?: string;
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
