import type {ApiJsonMsg, ControlMsg, ServerMsg} from "@/api";
import {isDevMode} from "@/composables/buildMode";
import {type ApiBinaryMsg, decodeHeader} from "@/api/binDataDef";

export interface IModuleCallback {
    ctrlCallback: (msg: ControlMsg) => void;
    serverJsonMsgCallback: (msg: ApiJsonMsg) => void;
    serverBinMsgCallback: (msg: ApiBinaryMsg) => void;
}

const moduleMap = new Map<number, IModuleCallback>();

/**
 * 只读旁路监听器（additive observers）。
 *
 * 协议模块采用"一个模块一个 handler"的注册模型（`registerModule`），页面卸载时
 * 会 unregister 自己的 handler（例如 Wifi.vue）。Dashboard/诊断需要在任意页面
 * 旁路观察模块消息（WIFI 查询结果、连接事件），因此提供不占用模块槽位的监听器：
 * 消息仍先路由给原 handler，再广播给监听器，互不影响、可重复订阅/退订。
 */
type JsonMsgListener = (msg: ApiJsonMsg) => void;
type CtrlMsgListener = (msg: ControlMsg) => void;

const jsonMsgListeners = new Set<JsonMsgListener>();
const ctrlMsgListeners = new Set<CtrlMsgListener>();

/** 订阅所有 JSON 模块消息（返回退订函数；监听器异常不影响其它监听器）。 */
export function addJsonMsgListener(listener: JsonMsgListener): () => void {
    jsonMsgListeners.add(listener);
    return () => {
        jsonMsgListeners.delete(listener);
    };
}

/** 订阅 WebSocket 控制事件（连接/断开等）；返回退订函数。 */
export function addCtrlMsgListener(listener: CtrlMsgListener): () => void {
    ctrlMsgListeners.add(listener);
    return () => {
        ctrlMsgListeners.delete(listener);
    };
}

export function registerModule(moduleId: number, moduleCallback: IModuleCallback): boolean {
    if (moduleMap.has(moduleId)) {
        if (isDevMode()) {
            console.log("module ", moduleId, "already registered");
        }
        return false;
    }

    moduleMap.set(moduleId, moduleCallback);
    return true;
}

export function unregisterModule(moduleId: number) {
    moduleMap.delete(moduleId);
}

export function routeModuleServerMsg(msg: ServerMsg) {
    if (msg.type === "json") {
        let jsonMsg: ApiJsonMsg;
        try {
            jsonMsg = JSON.parse(msg.data as string) as ApiJsonMsg;
            if (jsonMsg.cmd === undefined ||
                jsonMsg.module === undefined
            ){
                console.log("Server msg has no cmd or module", msg.data);
                return;
            }
        } catch (e) {
            console.log(e);
            return;
        }

        const module = jsonMsg.module;
        const moduleHandler = moduleMap.get(module);
        if (moduleHandler) {
            moduleHandler.serverJsonMsgCallback(jsonMsg);
        } else {
            if (isDevMode()) {
                console.log("routeModuleServerMsg module not loaded", module);
            }
        }
        for (const listener of jsonMsgListeners) {
            try {
                listener(jsonMsg);
            } catch (error) {
                if (isDevMode()) {
                    console.warn("json message listener failed", error);
                }
            }
        }
    } else {
        const arr = msg.data as ArrayBuffer;
        if (arr.byteLength < 4) {
            if (isDevMode()) {
                console.log("binary message too short");
            }
            return;
        }

        const binaryMsg = decodeHeader(msg.data as ArrayBuffer);
        const moduleHandler = moduleMap.get(binaryMsg.module);
        if (moduleHandler) {
            moduleHandler.serverBinMsgCallback(binaryMsg);
        } else {
            if (isDevMode()) {
                console.log("routeModuleServerMsg ignored:", msg, binaryMsg);
            }
        }
    }
}

export function routeCtrlMsg(msg: ControlMsg) {
    for (const item of moduleMap) {
        item[1].ctrlCallback(msg);
    }
    for (const listener of ctrlMsgListeners) {
        try {
            listener(msg);
        } catch (error) {
            if (isDevMode()) {
                console.warn("control message listener failed", error);
            }
        }
    }
}
