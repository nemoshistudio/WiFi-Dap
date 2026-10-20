import {useSystemStore, type ProtocolState} from "@/stores/useSystemStore";
import {registerModule} from "@/router/msgRouter";
import {
    type ApiJsonMsg,
    ControlEvent,
    type ControlMsg,
    ControlMsgType,
    PROTOCOL_VERSION,
    WtModuleID
} from "@/api";
import {type ISysFmInfo, type ISysInfo, wt_sys_get_fm_info, wt_sys_get_sys_info, WtSytemCmd} from "@/api/apiSystem";
import {isDevMode} from "@/composables/buildMode";
import {globalNotify} from "@/composables/notification";
import {translate} from "@/locales";

/** 握手超时/重试参数（docs/versioning.md §5.1） */
const HANDSHAKE_TIMEOUT_MS = 5000;
const HANDSHAKE_RETRY_MS = 2000;

/**
 * SYSTEM(0) 模块 + 协议版本握手。
 *
 * 连接后发 GET_FM_INFO（cmd 1）比较 protocol_version；超时 5s 视为 unknown，
 * 2s 未响应自动重试 1 次。握手只影响提示与 OTA 门禁，不阻塞其他功能。
 */
export function useSystemModule() {
    const sysStore = useSystemStore()

    let handshakeTimeoutId = 0;
    let handshakeRetryId = 0;
    /** 每个连接周期只提示一次，避免心跳/重连刷屏（docs/versioning.md §5.3） */
    let handshakeNotified = false;

    function clearHandshakeTimers() {
        if (handshakeTimeoutId) {
            window.clearTimeout(handshakeTimeoutId);
            handshakeTimeoutId = 0;
        }
        if (handshakeRetryId) {
            window.clearTimeout(handshakeRetryId);
            handshakeRetryId = 0;
        }
    }

    type NotifyType = 'error' | 'warning' | 'info' | 'success';

    function notifyHandshakeOnce(message: string, type: NotifyType = 'warning') {
        if (handshakeNotified) {
            return;
        }
        handshakeNotified = true;
        globalNotify(message, type);
    }

    function compareProtocol(fw: unknown): ProtocolState {
        if (typeof fw !== 'number') {
            return 'legacy';
        }
        return fw === PROTOCOL_VERSION ? 'ok' : 'mismatch';
    }

    function reportProtocolState(state: ProtocolState, fwVersion: number | null) {
        switch (state) {
            case 'ok':
                break;
            case 'mismatch':
                notifyHandshakeOnce(
                    `${translate('protocol.mismatchTitle')} ` +
                    translate('protocol.mismatchDetail', {
                        web: PROTOCOL_VERSION,
                        fw: fwVersion ?? '?',
                    }) + ' ' + translate('protocol.refreshHint')
                );
                break;
            case 'legacy':
                notifyHandshakeOnce(translate('protocol.mismatchLegacy'));
                break;
            case 'unknown':
                notifyHandshakeOnce(translate('protocol.unknownVersion'));
                break;
        }
    }

    function startHandshake() {
        clearHandshakeTimers();
        handshakeNotified = false;
        sysStore.protocolState = 'unknown';
        sysStore.fwProtocolVersion = null;
        sysStore.handshakeSettled = false;

        wt_sys_get_fm_info();

        handshakeRetryId = window.setTimeout(() => {
            handshakeRetryId = 0;
            if (sysStore.protocolState === 'unknown') {
                wt_sys_get_fm_info();
            }
        }, HANDSHAKE_RETRY_MS);

        handshakeTimeoutId = window.setTimeout(() => {
            handshakeTimeoutId = 0;
            if (!sysStore.handshakeSettled) {
                sysStore.handshakeSettled = true;
                sysStore.protocolState = 'unknown';
                reportProtocolState('unknown', null);
            }
        }, HANDSHAKE_TIMEOUT_MS);
    }

    function onClientCtrl(msg: ControlMsg) {
        if (msg.type !== ControlMsgType.WS_EVENT) {
            return
        }

        if (msg.data === ControlEvent.CONNECTED) {
            sysStore.rebootInProgress = false;
            // 每次（重）连接都重新握手（docs/versioning.md §5.1）
            startHandshake();
            wt_sys_get_sys_info();
        } else if (msg.data === ControlEvent.DISCONNECTED) {
            clearHandshakeTimers();
            sysStore.handshakeSettled = false;
            sysStore.protocolState = 'unknown';
            sysStore.fwProtocolVersion = null;
        }
    }

    function onClientMsg(msg: ApiJsonMsg) {
        // 鉴权失败（code=7）由 auth session 统一提示；不当作固件/协议错误处理
        if (msg.err !== undefined && msg.code === 7) {
            clearHandshakeTimers();
            return;
        }
        switch (msg.cmd as WtSytemCmd) {
            case WtSytemCmd.WT_SYS_REBOOT:
                sysStore.rebootInProgress = true;
                break;
            case WtSytemCmd.WT_SYS_GET_FM_INFO: {
                clearHandshakeTimers();
                sysStore.handshakeSettled = true;
                if (msg.err !== undefined) {
                    // 请求失败（非鉴权类）：视为无法确认版本，提示但不阻塞业务
                    sysStore.protocolState = 'unknown';
                    sysStore.fwProtocolVersion = null;
                    reportProtocolState('unknown', null);
                    break;
                }
                const fm_info = msg as ISysFmInfo;
                sysStore.curFmInfo.date = fm_info.upd_date;
                sysStore.curFmInfo.ver = fm_info.fm_ver;
                const fwVersion = typeof fm_info.protocol_version === 'number'
                    ? fm_info.protocol_version
                    : null;
                const state = compareProtocol(fm_info.protocol_version);
                sysStore.fwProtocolVersion = fwVersion;
                sysStore.protocolState = state;
                reportProtocolState(state, fwVersion);
                break;
            }
            case WtSytemCmd.WT_SYS_GET_SYS_INFO: {
                const sysInfo: ISysInfo = msg as ISysInfo & ApiJsonMsg;
                Object.assign(sysStore.sysInfo, sysInfo);
                if (typeof sysInfo.hw_ver === 'string') {
                    sysStore.hwInfo.ver = sysInfo.hw_ver;
                }
                if (typeof sysInfo.mf_date === 'string') {
                    sysStore.hwInfo.date = sysInfo.mf_date;
                }
                break;
            }
        }
        if (isDevMode()) {
            console.log(msg);
        }
    }

    registerModule(WtModuleID.SYSTEM, {
        ctrlCallback: onClientCtrl,
        serverJsonMsgCallback: onClientMsg,
        serverBinMsgCallback: () => {},
    });
}
