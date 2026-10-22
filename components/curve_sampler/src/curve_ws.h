/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - WebSocket frame push (P1.11), core state.
 *
 * The Web UI can subscribe on `GET /ws/curve` (WebSocket) after POST
 * /api/curve/start; the sampler then pushes CVF2 batches (same wire format as
 * GET /api/curve/frames) instead of the browser polling HTTP.  HTTP polling
 * keeps working unchanged.
 *
 * This header/level is pure C (no IDF/HTTP dependencies) so the subscription
 * table, per-client push planning and backpressure counters are covered by the
 * host regression test.  curve_ws.c is the thin IDF glue (WS URI handler +
 * httpd work-queue push).
 */
#ifndef CURVE_WS_H
#define CURVE_WS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A handful of browser tabs at most; every client costs one entry. */
#define CURVE_WS_MAX_CLIENTS 4U
/* Minimum time between two pushed batches per client (backpressure). */
#define CURVE_WS_PUSH_INTERVAL_MS 20U
/* Max frames per pushed batch (bounded by the subscriptions' request). */
#define CURVE_WS_MAX_BATCH_FRAMES 32U

typedef struct {
    int fd;                   /* -1 = free slot */
    bool subscribed;
    uint32_t next_seq;        /* first seq the client has not been sent yet (0=oldest) */
    uint32_t acked_seq;       /* client ack (monotonic, <= last sent) */
    uint32_t last_push_ms;    /* esp_timer ms of the last queued/sent batch */
    uint32_t dropped_batches; /* push skipped: httpd work queue full */
    uint32_t sent_batches;
    uint32_t sent_frames;
} curve_ws_client_t;

typedef struct {
    curve_ws_client_t clients[CURVE_WS_MAX_CLIENTS];
    uint16_t attached;        /* slots attached to a socket */
    uint16_t subscribed;      /* slots with an active subscription */
} curve_ws_state_t;

void curve_ws_state_init(curve_ws_state_t *state);

/** Attach a socket (idempotent per fd); returns the slot index or -1. */
int curve_ws_attach(curve_ws_state_t *state, int fd);

/** Detach one socket; returns true when it was present. */
bool curve_ws_detach(curve_ws_state_t *state, int fd);

/** Find one client slot by fd (NULL when absent). */
curve_ws_client_t *curve_ws_find(curve_ws_state_t *state, int fd);

/** Subscribe/unsubscribe one fd.  Subscribe restarts from the oldest ring frame. */
bool curve_ws_subscribe(curve_ws_state_t *state, int fd);
bool curve_ws_unsubscribe(curve_ws_state_t *state, int fd);

/** Record a client ack (monotonic; seq > last planned seq is clamped away). */
bool curve_ws_ack(curve_ws_state_t *state, int fd, uint32_t seq);

/**
 * @brief Plan one push for a subscribed client.
 *
 * Enforces the per-client push interval and returns the number of frames that
 * should be sent (0 = nothing to do now).  `since_out` is the HTTP-style
 * `since` cursor for curve_engine_alloc_frames(); `gap_out` mirrors the CVF2
 * gap flag meaning (client cursor older than the retained ring).
 *
 * @param oldest oldest retained seq, latest newest retained seq
 * @param max_frames client-requested batch cap (1..CURVE_WS_MAX_BATCH_FRAMES)
 * @param now_ms esp_timer /1000
 * @return frame count to push (0 = skip)
 */
uint32_t curve_ws_plan_push(curve_ws_state_t *state, int fd, uint32_t oldest,
                            uint32_t latest, uint32_t max_frames,
                            uint32_t now_ms, uint32_t *since_out,
                            uint32_t *first_seq_out, bool *gap_out);

/** Commit a planned push (updates next_seq / counters). */
void curve_ws_mark_pushed(curve_ws_state_t *state, int fd, uint32_t first_seq,
                          uint32_t count, uint32_t now_ms);

/** Count a skipped push (httpd work queue full / socket gone). */
void curve_ws_mark_dropped(curve_ws_state_t *state, int fd, uint32_t now_ms);

/** Called by the sampler after a frame was appended to the ring. */
void curve_ws_notify_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* CURVE_WS_H */
