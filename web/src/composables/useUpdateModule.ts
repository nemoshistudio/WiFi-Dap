import {registerModule} from "@/router/msgRouter";
import {type ApiJsonMsg, ControlEvent, type ControlMsg, ControlMsgType, WtModuleID, WtOtaCmd} from "@/api";
import {useUpdateStore} from "@/stores/useUpdateStore";
import {
    type IOtaInfo,
    type IOtaProgress,
    OtaStatus,
    wt_ota_get_info,
    wt_ota_get_progress,
} from "@/api/apiOTA";
import {isDevMode} from "@/composables/buildMode";
import {globalNotify} from "@/composables/notification";
import {translate} from "@/locales";

/**
 * OTA(5) WS 模块：GET_INFO / GET_PROGRESS 路由 + 设备进度轮询。
 *
 * 上传本体走 HTTP（`POST /api/ota/upload`，见 api/apiOTA.ts），本模块只负责
 * WS 通道的状态同步与 ABORT 回执（docs/ota-local-upload.md §4）。
 */
export function useUpdateModule() {
    const updateStore = useUpdateStore()

    function refreshDeviceState() {
        wt_ota_get_info();
        wt_ota_get_progress();
    }

    function syncPolling() {
        // 只有设备仍在处理上传时才需要 400ms 轮询；写入浏览器进度阶段也保持轮询，
        // 便于浏览器上传 % 与设备 offset/total 对比（契约 §7.2-3）。
        if (updateStore.isBusy || updateStore.progress.status === OtaStatus.RECEIVING) {
            updateStore.setProgressInterval();
        } else {
            updateStore.clearProgressInterval();
        }
    }

    function onClientCtrl(msg: ControlMsg) {
        if (msg.type !== ControlMsgType.WS_EVENT) {
            return
        }

        if (msg.data === ControlEvent.CONNECTED) {
            // 每次（重）连接都刷新 GET_INFO：设备重启后据此判定 running_ver 是否变化
            refreshDeviceState();
            syncPolling();
        } else if (msg.data === ControlEvent.DISCONNECTED) {
            updateStore.clearProgressInterval();
        }
    }

    function onClientMsg(msg: ApiJsonMsg) {
        // 旧固件不支持 OTA(5)（code=4）：隐藏上传 UI，不当作错误弹出
        if (msg.code === 4) {
            updateStore.markUnsupported();
            return;
        }
        // 鉴权失败（code=7）：msgRouter 已统一提示输入 pairing token，这里不重复报错
        if (msg.code === 7) {
            return;
        }
        switch (msg.cmd as WtOtaCmd) {
            case WtOtaCmd.GET_INFO: {
                updateStore.applyInfo(msg as IOtaInfo);
                syncPolling();
                break;
            }
            case WtOtaCmd.GET_PROGRESS: {
                updateStore.applyProgress(msg as IOtaProgress);
                syncPolling();
                break;
            }
            case WtOtaCmd.ABORT: {
                if (msg.err) {
                    // 仅在 RECEIVING 状态允许 ABORT，其它状态返回 code=5 invalid state
                    globalNotify(translate('ota.upload.failed', {err: msg.err}), 'error');
                } else {
                    updateStore.markAborted();
                }
                break;
            }
            default:
                break;
        }

        if (isDevMode()) {
            console.log(msg);
        }
    }

    registerModule(WtModuleID.OTA, {
        ctrlCallback: onClientCtrl,
        serverJsonMsgCallback: onClientMsg,
        serverBinMsgCallback: () => {},
    });
}
