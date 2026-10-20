import {defineStore} from "pinia";
import {ref} from "vue";

/**
 * 固件/网页协议版本握手状态（docs/versioning.md §5.3）。
 * - ok:       protocol_version 与前端 PROTOCOL_VERSION 一致
 * - mismatch: 固件上报了数字版本但不一致
 * - legacy:   固件响应没有 protocol_version（旧固件）
 * - unknown:  5s 内无法确认（超时/请求失败）
 */
export type ProtocolState = 'ok' | 'mismatch' | 'legacy' | 'unknown'

export const useSystemStore = defineStore('system', () => {

    const curFmInfo = ref({
        ver: "-",
        date: "-",
    });

    const hwInfo = ref({
        ver: "-",
        date: "-",
    })

    const sys_info = ref({
        sn: "-",
    });

    const rebootInProgress = ref(false);

    /* ---- 协议握手（WP-131 / docs/versioning.md §5） ---- */
    const protocolState = ref<ProtocolState>('unknown');
    /** 固件上报的 protocol_version；null = 尚未确认/旧固件 */
    const fwProtocolVersion = ref<number | null>(null);

    return {
        curFmInfo,
        hwInfo,
        sysInfo: sys_info,
        rebootInProgress,
        protocolState,
        fwProtocolVersion,
    }
});
