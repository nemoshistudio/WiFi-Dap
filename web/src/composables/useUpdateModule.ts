import {registerModule} from "@/router/msgRouter";
import {type ApiJsonMsg, ControlEvent, type ControlMsg, ControlMsgType, WtModuleID, WtOtaCmd} from "@/api";
import {useUpdateStore} from "@/stores/useUpdateStore";
import {
    type IOtaInfo,
    type IOtaProgress,
    OtaStatus,
    wt_ota_get_info,
    wt_ota_get_progress,
    wt_ota_abort,
} from "@/api/apiOTA";
import {isDevMode} from "@/composables/buildMode";

/**
 * OTA(5) 模块：只读状态与进度轮询（Wave 1 预置）。
 * Wave 2 的 WP-207 打开 VITE_ENABLE_OTA 后在 App.vue 注册并接上传流程。
 */
export function useUpdateModule() {
    const updateStore = useUpdateStore()

    function onClientCtrl(msg: ControlMsg) {
        if (msg.type !== ControlMsgType.WS_EVENT) {
            return
        }

        if (msg.data === ControlEvent.CONNECTED) {
            wt_ota_get_info();
            wt_ota_get_progress();
        }
    }

    function onClientMsg(msg: ApiJsonMsg) {
        switch (msg.cmd as WtOtaCmd) {
            case WtOtaCmd.GET_INFO: {
                Object.assign(updateStore.info, msg as IOtaInfo);
                break;
            }
            case WtOtaCmd.GET_PROGRESS: {
                const progress = msg as IOtaProgress;
                Object.assign(updateStore.progress, progress);
                switch (progress.status) {
                    case OtaStatus.READY:
                    case OtaStatus.APPLYING:
                        updateStore.progressBarStatus = 'success';
                        updateStore.clearProgressInterval();
                        break;
                    case OtaStatus.ERROR:
                        updateStore.progressBarStatus = 'exception';
                        updateStore.clearProgressInterval();
                        break;
                    case OtaStatus.RECEIVING:
                        updateStore.progressBarStatus = '';
                        updateStore.setProgressInterval();
                        break;
                    default:
                        updateStore.progressBarStatus = '';
                        updateStore.clearProgressInterval();
                        break;
                }
                break;
            }
            case WtOtaCmd.ABORT:
                break;
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

    return { wt_ota_abort };
}
