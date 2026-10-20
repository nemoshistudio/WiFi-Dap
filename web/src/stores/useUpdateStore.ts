import { ref } from 'vue'
import { defineStore } from 'pinia'
import {type IOtaInfo, type IOtaProgress, OtaStatus, wt_ota_get_progress} from "@/api/apiOTA";

/**
 * OTA 只读状态（Wave 1 预置；WP-207 在此基础上接上传流程）。
 * 字段语义见 docs/versioning.md §6。
 */
export const useUpdateStore = defineStore('update', () => {
    const info = ref<IOtaInfo>({
        module: 5,
        cmd: 1,
        running: "-",
        running_ver: "-",
        free: 0,
        slot_size: 0,
        status: OtaStatus.IDLE,
    });

    const progress = ref<IOtaProgress>({
        module: 5,
        cmd: 3,
        offset: 0,
        total: 0,
        status: OtaStatus.IDLE,
    });

    const progressBarStatus = ref('');
    /** 是否有可用更新：Wave 1 恒 false；WP-207 接入 OTA manifest 后实现。 */
    const canUpdate = ref(false);

    let progressIntervalID = -1;

    function setProgressInterval() {
        if (progressIntervalID < 0) {
            progressIntervalID = window.setInterval(() => {
                wt_ota_get_progress();
            }, 1000);
        }
    }

    function clearProgressInterval() {
        if (progressIntervalID >= 0) {
            window.clearInterval(progressIntervalID);
            progressIntervalID = -1;
        }
    }

    return {
        info,
        progress,
        canUpdate,
        progressBarStatus,
        setProgressInterval,
        clearProgressInterval,
    }
})
