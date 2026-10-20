import {type ApiJsonMsg, sendJsonMsg, WtModuleID} from '@/api'

export enum WtSytemCmd {
    WT_SYS_GET_FM_INFO      = 1,
    WT_SYS_REBOOT           = 2,
    WT_SYS_GET_SYS_INFO     = 3,
}

export interface ISysFmInfo extends ApiJsonMsg {
    fm_ver: string;
    upd_date: string;
    /** 固件协议版本（docs/protocol.md §2.1 / docs/versioning.md §5.2）；旧固件无此字段 */
    protocol_version?: number;
}

export interface ISysHwInfo extends ApiJsonMsg {
    hw_ver: string;
    mf_date: string;
}

export interface ISysInfo {
    sn: string;
    /** 硬件版本，构建注入，可为空字符串 */
    hw_ver?: string;
    /** 生产日期，构建注入，可为空字符串 */
    mf_date?: string;
}

export function wt_sys_get_fm_info() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.SYSTEM,
        cmd: WtSytemCmd.WT_SYS_GET_FM_INFO,
    };
    sendJsonMsg(msg);
}

export function wt_sys_reboot() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.SYSTEM,
        cmd: WtSytemCmd.WT_SYS_REBOOT,
    };
    sendJsonMsg(msg);
}

export function wt_sys_get_sys_info() {
    const msg: ApiJsonMsg = {
        module: WtModuleID.SYSTEM,
        cmd: WtSytemCmd.WT_SYS_GET_SYS_INFO,
    };
    sendJsonMsg(msg);
}
