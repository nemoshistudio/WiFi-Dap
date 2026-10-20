import {computed, ref} from 'vue'
import {defineStore} from 'pinia'
import {
    type IOtaImageInfo,
    type IOtaInfo,
    type IOtaProgress,
    type IOtaUploadSuccess,
    OtaStatus,
    wt_ota_get_progress,
} from "@/api/apiOTA";

/** Update 页 UI 阶段（与固件状态机 docs/ota-local-upload.md §5 对应）。 */
export type OtaUploadPhase =
    | 'idle'
    | 'selected'
    | 'uploading'
    | 'verifying'
    | 'ready'
    | 'applying'
    | 'success'
    | 'error'

export interface IOtaSelectedFile {
    name: string;
    size: number;
    image: IOtaImageInfo;
}

/** 设备进度轮询间隔（契约 §4.2 建议 300–500ms） */
const PROGRESS_POLL_INTERVAL_MS = 400;

/**
 * OTA 状态：设备只读信息（GET_INFO/GET_PROGRESS）+ 浏览器上传 UI 状态。
 * 字段语义见 docs/versioning.md §6 与 docs/ota-local-upload.md §3–§5。
 */
export const useUpdateStore = defineStore('update', () => {
    /* ---- 设备信息（WS cmd 1 / cmd 3） ---- */
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

    /** NavBar 更新角标：本期无更新源（浏览器本地选文件），恒 false。 */
    const canUpdate = ref(false);

    /**
     * 固件是否实现 OTA(5) 命令（旧固件返回 code=4 Unsupported cmd）。
     * false 时 Update 隐藏上传区块，只保留协议/固件信息（优雅降级）。
     */
    const otaSupported = ref(true);

    /* ---- 上传 UI 状态 ---- */
    const phase = ref<OtaUploadPhase>('idle');
    const selectedFile = ref<IOtaSelectedFile | null>(null);
    /** 浏览器已上传字节/总字节（XHR upload.onprogress） */
    const browserLoaded = ref(0);
    const browserTotal = ref(0);
    /** 本地化的错误提示（UI 直接展示）与原始设备/HTTP err（调试用） */
    const errorMessage = ref('');
    const errorDetail = ref('');
    /** 设备回传的 sha256（上传响应或 GET_PROGRESS），供人工比对 */
    const deviceSha256 = ref('');
    /** 上传前运行版本，用于重启后用 GET_INFO.running_ver 判定升级是否生效 */
    const preUploadRunningVer = ref('');
    const uploadSuccess = ref<IOtaUploadSuccess | null>(null);

    const browserPercent = computed(() => {
        if (browserTotal.value <= 0) {
            return 0;
        }
        return Math.min(100, Math.round((browserLoaded.value / browserTotal.value) * 10000) / 100);
    });

    const devicePercent = computed(() => {
        if (progress.value.total <= 0) {
            return 0;
        }
        return Math.min(100, Math.round((progress.value.offset / progress.value.total) * 10000) / 100);
    });

    /** uploading/verifying/ready/applying 期间禁止重复开始 */
    const isBusy = computed(() => {
        return phase.value === 'uploading'
            || phase.value === 'verifying'
            || phase.value === 'ready'
            || phase.value === 'applying';
    });

    const hasSelectedFile = computed(() => selectedFile.value !== null);

    let progressIntervalID = -1;

    function setProgressInterval() {
        if (progressIntervalID < 0) {
            progressIntervalID = window.setInterval(() => {
                wt_ota_get_progress();
            }, PROGRESS_POLL_INTERVAL_MS);
        }
    }

    function clearProgressInterval() {
        if (progressIntervalID >= 0) {
            window.clearInterval(progressIntervalID);
            progressIntervalID = -1;
        }
    }

    /* ---- 设备响应 ---- */

    function applyInfo(next: IOtaInfo) {
        Object.assign(info.value, next);
        if (typeof next.sha256 === 'string' && next.sha256) {
            deviceSha256.value = next.sha256;
        }
        // 重启后设备已运行新槽：running_ver 与上传前不同 → 升级成功
        if ((phase.value === 'applying' || phase.value === 'ready')
            && preUploadRunningVer.value
            && typeof next.running_ver === 'string'
            && next.running_ver
            && next.running_ver !== preUploadRunningVer.value) {
            phase.value = 'success';
            errorMessage.value = '';
            errorDetail.value = '';
            clearProgressInterval();
        }
    }

    function applyProgress(next: IOtaProgress) {
        progress.value = {...next};
        if (typeof next.sha256 === 'string' && next.sha256) {
            deviceSha256.value = next.sha256;
        }
        switch (next.status) {
            case OtaStatus.RECEIVING:
                if (phase.value === 'uploading') {
                    progressBarStatus.value = '';
                }
                break;
            case OtaStatus.VERIFYING:
                phase.value = 'verifying';
                progressBarStatus.value = '';
                break;
            case OtaStatus.READY:
                phase.value = 'ready';
                progressBarStatus.value = 'success';
                break;
            case OtaStatus.APPLYING:
                phase.value = 'applying';
                progressBarStatus.value = 'success';
                break;
            case OtaStatus.ERROR:
                phase.value = 'error';
                progressBarStatus.value = 'exception';
                errorDetail.value = next.err ?? '';
                if (!errorMessage.value) {
                    errorMessage.value = next.err ?? '';
                }
                clearProgressInterval();
                break;
            case OtaStatus.IDLE:
            default:
                if (phase.value === 'uploading' || phase.value === 'verifying') {
                    // 上传会话被设备提前结束（例如 10s 空闲超时），保持等待 HTTP 结果
                    progressBarStatus.value = '';
                }
                break;
        }
    }

    /* ---- 上传流程（浏览器侧） ---- */

    function resetUpload() {
        phase.value = 'idle';
        selectedFile.value = null;
        browserLoaded.value = 0;
        browserTotal.value = 0;
        errorMessage.value = '';
        errorDetail.value = '';
        deviceSha256.value = '';
        uploadSuccess.value = null;
        preUploadRunningVer.value = '';
        progressBarStatus.value = '';
        clearProgressInterval();
    }

    function setSelectedFile(file: IOtaSelectedFile | null) {
        selectedFile.value = file;
        phase.value = file ? 'selected' : 'idle';
        errorMessage.value = '';
        errorDetail.value = '';
    }

    function beginUpload(preRunningVer: string) {
        phase.value = 'uploading';
        browserLoaded.value = 0;
        browserTotal.value = selectedFile.value?.size ?? 0;
        errorMessage.value = '';
        errorDetail.value = '';
        deviceSha256.value = '';
        uploadSuccess.value = null;
        progressBarStatus.value = '';
        preUploadRunningVer.value = preRunningVer;
        setProgressInterval();
    }

    function setBrowserProgress(loaded: number, total: number) {
        browserLoaded.value = loaded;
        browserTotal.value = total;
    }

    function onUploadSuccess(result: IOtaUploadSuccess) {
        uploadSuccess.value = result;
        if (typeof result.sha256 === 'string') {
            deviceSha256.value = result.sha256;
        }
        // HTTP 200 先发出、设备 1000ms 后重启（契约 §3.4/§5）
        phase.value = result.status === OtaStatus.READY ? 'ready' : 'applying';
        progressBarStatus.value = 'success';
        // 继续轮询 GET_PROGRESS/GET_INFO，等待重启后 running_ver 变化
        setProgressInterval();
    }

    function failUpload(message: string, detail = '') {
        phase.value = 'error';
        errorMessage.value = message;
        errorDetail.value = detail;
        progressBarStatus.value = 'exception';
        clearProgressInterval();
    }

    function markAborted() {
        phase.value = 'idle';
        browserLoaded.value = 0;
        browserTotal.value = 0;
        errorMessage.value = '';
        errorDetail.value = '';
        progressBarStatus.value = '';
        clearProgressInterval();
    }

    /** 旧固件不支持 OTA(5)：隐藏上传 UI，避免错误风暴。 */
    function markUnsupported() {
        otaSupported.value = false;
        clearProgressInterval();
    }

    function clearError() {
        if (phase.value === 'error') {
            phase.value = 'idle';
        }
        errorMessage.value = '';
        errorDetail.value = '';
        progressBarStatus.value = '';
    }

    return {
        info,
        progress,
        progressBarStatus,
        canUpdate,
        otaSupported,
        phase,
        selectedFile,
        browserLoaded,
        browserTotal,
        errorMessage,
        errorDetail,
        deviceSha256,
        preUploadRunningVer,
        uploadSuccess,
        browserPercent,
        devicePercent,
        isBusy,
        hasSelectedFile,
        setProgressInterval,
        clearProgressInterval,
        applyInfo,
        applyProgress,
        markUnsupported,
        resetUpload,
        setSelectedFile,
        beginUpload,
        setBrowserProgress,
        onUploadSuccess,
        failUpload,
        markAborted,
        clearError,
    }
})
