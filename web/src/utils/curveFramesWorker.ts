/**
 * frames 取帧/解码 Worker（task-37）：把 fetch + CVF2 解码从主线程移出。
 * 主线程只接收已解码的 CurveLocalFramesPage（结构化克隆）；失败时主线程 fallback。
 */
import {decodeCurveFrames, emptyCurveFramesPage} from '@/api/curveLocal'

interface WorkerRequest {
    id: number
    url: string
}

const scope = self as unknown as {
    onmessage: ((event: MessageEvent<WorkerRequest>) => void) | null
    postMessage: (message: unknown) => void
}

scope.onmessage = (event: MessageEvent<WorkerRequest>) => {
    const {id, url} = event.data
    void (async () => {
        try {
            const response = await fetch(url, {cache: 'no-store'})
            if (response.status === 204) {
                scope.postMessage({id, page: emptyCurveFramesPage()})
                return
            }
            if (!response.ok) {
                scope.postMessage({id, error: `HTTP ${response.status}`})
                return
            }
            const buffer = await response.arrayBuffer()
            const page = decodeCurveFrames(buffer)
            // page 内 payload 为 Uint8Array，可结构化克隆。
            scope.postMessage({id, page})
        } catch (error) {
            scope.postMessage({id, error: error instanceof Error ? error.message : String(error)})
        }
    })()
}
