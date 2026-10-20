import type {ControlMsg, ServerMsg} from "@/api";
import {ControlEvent, ControlMsgType} from "@/api";
import {WebsocketWrapper} from "@/composables/websocket/websocketWrapper";
import {toClient, toClientCtrl, toServer} from "@/composables/broadcastChannelDef";
import {isDevMode} from "@/composables/buildMode";

declare const self: SharedWorkerGlobalScope;

const websocket = new WebsocketWrapper();
let host = "";

function ctrlBroadcast(msg: ControlMsg) {
    toClientCtrl.postMessage(msg);
}

function msgBroadcast(msg: ServerMsg) {
    toClient.postMessage(msg);
}

// self.onconnect
self.onconnect = function(event) {
    const port = event.ports[0];
    port.onmessage = function (e: MessageEvent<ControlMsg>) {
        if (isDevMode()) {
            console.log('Received control message in SharedWorker:', e.data?.type);
        }
        if (e.data.type === ControlMsgType.WS_SET_HOST) {
            const nextHost = typeof e.data.data === 'string' ? e.data.data : '';
            // Keep the first host (multi-tab / multi-device safety)
            if (nextHost !== "" && (host === "" || host === nextHost)) {
                host = nextHost;
                websocket.init(host, msgBroadcast, ctrlBroadcast);
            }
        } else if (e.data.type === ControlMsgType.WS_GET_STATE) {
            websocket.getSocketStatus();
        }
    };
    const msg: ControlMsg = {
        type: ControlMsgType.WS_EVENT,
        data: ControlEvent.LOADED,
    }
    port.postMessage(msg);
};

toServer.onmessage = (ev: MessageEvent<ServerMsg>) => {
    websocket.send(ev.data);
};
