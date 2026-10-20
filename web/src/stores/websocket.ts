import {defineStore} from "pinia";

import {ControlEvent} from "@/api";

export const useWsStore = defineStore('websocket', {
    state: () => {
        return {
            state: ControlEvent.DISCONNECTED as ControlEvent,
        };
    },
});
