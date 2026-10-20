import type {ControlMsg, ServerMsg} from "@/api";
import {ControlEvent, ControlMsgType} from "@/api";
import {WebsocketWrapper} from "@/composables/websocket/websocketWrapper";
import {toClient, toClientCtrl, toServer} from "@/composables/broadcastChannelDef";
import {isDevMode} from "@/composables/buildMode";
import {normalizeAuthToken} from "@/composables/websocket/auth";

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
            console.log('Received message in SharedWorker:', e.data);
        }
        if (e.data.type === ControlMsgType.WS_SET_HOST) {
            const nextHost = typeof e.data.data === 'string' ? e.data.data : '';
            const token = normalizeAuthToken(e.data.token);
            // Keep the first host (multi-tab / multi-device safety) but allow a
            // later tab to supply/refresh the auth token for the same device.
            if (nextHost !== "" && (host === "" || host === nextHost)) {
                host = nextHost;
                websocket.init(host, msgBroadcast, ctrlBroadcast, token);
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
