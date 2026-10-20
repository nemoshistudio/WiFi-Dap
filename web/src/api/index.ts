import {getWebsocketService} from "@/composables/websocket/websocketService";
import type {ApiBinaryMsg} from "@/api/binDataDef";

/**
 * JSON/WebSocket protocol version. This is the single front-end source of truth
 * and must match BIRDIEBOT_PROTOCOL_VERSION in the firmware (docs/versioning.md §3).
 */
export const PROTOCOL_VERSION = 1;

export interface ApiJsonMsg {
    module: number;
    cmd: number;
    /** Error message returned by the firmware. */
    err?: string;
    /** Error code returned by the firmware (docs/protocol.md §1.1). */
    code?: number;
}

export enum ControlMsgType {
    WS_EVENT = "WS_EVENT",
    WS_SET_HOST = "WS_SET_HOST",
    WS_GET_STATE = "WS_GET_STATE",
}

export enum ControlEvent {
    DISCONNECTED = "DISCONNECTED",
    LOADED = "LOADED",
    CONNECTED = "CONNECTED",
    CONNECTING = "CONNECTING",
    BROKEN = "BROKEN",
}

export interface ControlMsg {
    type: ControlMsgType,
    data: ControlEvent | string,
}

export interface ServerMsg {
    type: "json" | "binary"
    data: string | ArrayBuffer;
}

/** Web UI error codes (docs/protocol.md §1.1). */
export enum WtErrorCode {
    BAD_JSON_REQUEST = 2,
    INTERNAL_ERROR = 3,
    UNSUPPORTED_CMD = 4,
    PROPERTY_ERROR = 5,
    BUSY = 6,
}

/** JSON module ids (docs/protocol.md §2). 3 is reserved by the firmware. */
export enum WtModuleID {
    SYSTEM = 0,
    WIFI = 1,
    DATA_FLOW = 2,
    RESERVED = 3,
    UART = 4,
    OTA = 5,
}

/** SYSTEM(0) JSON commands (docs/protocol.md §2.1). */
export enum WtSystemCmd {
    GET_FM_INFO = 1,
    REBOOT = 2,
    GET_SYS_INFO = 3,
}

/** WIFI(1) JSON commands (docs/protocol.md §2.2). */
export enum WtWifiCmd {
    GET_STA_INFO = 1,
    CONNECT = 2,
    SCAN = 3,
    DISCONNECT = 4,
    GET_AP_INFO = 5,
    GET_MODE = 6,
    SET_MODE = 7,
    SET_AP_CRED = 8,
    GET_STATIC_IP = 9,
    SET_STATIC_IP = 10,
    GET_SAVED_LIST = 11,
    ADD_SAVED = 12,
    DEL_SAVED = 13,
    SET_AUTO_SAVE = 14,
}

/** DATA_FLOW(2) JSON commands (docs/protocol.md §2.3). */
export enum WtDataFlowCmd {
    GET_INS_LIST = 1,
    GET_CUR_INS = 2,
    GET_CUR_ATTACH_LIST = 3,
    GET_ATTACH_LIST = 4,
    ATTACH = 5,
    ATTACH_CUR_TO_RECVER = 6,
    ATTACH_CUR_TO_SENDER = 7,
    DETACH_SINGLE = 8,
    DETACH_CUR_FROM = 9,
    SET_DATA_TYPE = 10,
}

/** UART(4) JSON commands (docs/protocol.md §2.4). */
export enum WtUartCmd {
    GET_AVAILABLE_NUMS = 1,
    GET_BAUD = 4,
    SET_BAUD = 5,
    GET_CONFIG = 6,
    SET_CONFIG = 7,
    GET_FLOW_CTRL = 8,
    SET_FLOW_CTRL = 9,
    GET_PINS_NUM = 10,
    SET_PINS_NUM = 11,
    GET_MODE = 12,
    SET_MODE = 13,
    GET_STATUS = 20,
    SET_STATUS = 21,
    GET_DATA_TYPE = 22,
    SET_DATA_TYPE = 23,
    GET_DEFAULT_NUM = 24,
}

/** OTA(5) JSON commands (docs/protocol.md §2.5; local upload is WP-207). */
export enum WtOtaCmd {
    GET_INFO = 1,
    GET_PROGRESS = 3,
    ABORT = 4,
}

export function sendJsonMsg(apiJsonMsg: ApiJsonMsg) {
    const msg: ServerMsg = {
        type: "json",
        data: JSON.stringify(apiJsonMsg),
    };
    getWebsocketService().send(msg);
}

export function sendBinMsg(binMsg: ApiBinaryMsg) {
    const buffer = new Uint8Array(4 + binMsg.payload.length);
    buffer[0] = binMsg.data_type;
    buffer[1] = binMsg.module;
    buffer[2] = binMsg.sub_mod;
    buffer[3] = 0; // Reserved byte
    buffer.set(binMsg.payload, 4); // Append payload after header

    const msg: ServerMsg = {
        type: "binary",
        data: buffer.buffer as ArrayBuffer,
    };
    getWebsocketService().send(msg);
}
