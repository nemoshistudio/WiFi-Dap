/*
 * SPDX-License-Identifier: MIT
 *
 * Curve page device-local sampler - WebSocket push subscription state.
 *
 * Pure C so the subscription table, per-client cursor/interval planning and the
 * backpressure/drop accounting are host-testable (no IDF/HTTP dependency).
 * See curve_ws.h for the protocol overview.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "curve_ws.h"

static void client_reset(curve_ws_client_t *client)
{
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

void curve_ws_state_init(curve_ws_state_t *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    for (size_t i = 0U; i < CURVE_WS_MAX_CLIENTS; i++) {
        client_reset(&state->clients[i]);
    }
}

curve_ws_client_t *curve_ws_find(curve_ws_state_t *state, int fd)
{
    if (state == NULL || fd < 0) {
        return NULL;
    }
    for (size_t i = 0U; i < CURVE_WS_MAX_CLIENTS; i++) {
        if (state->clients[i].fd == fd) {
            return &state->clients[i];
        }
    }
    return NULL;
}

int curve_ws_attach(curve_ws_state_t *state, int fd)
{
    if (state == NULL || fd < 0) {
        return -1;
    }
    for (size_t i = 0U; i < CURVE_WS_MAX_CLIENTS; i++) {
        if (state->clients[i].fd == fd) {
            return (int)i; /* already attached (duplicate handshake) */
        }
    }
    for (size_t i = 0U; i < CURVE_WS_MAX_CLIENTS; i++) {
        if (state->clients[i].fd < 0) {
            client_reset(&state->clients[i]);
            state->clients[i].fd = fd;
            state->attached++;
            return (int)i;
        }
    }
    return -1; /* table full */
}

bool curve_ws_detach(curve_ws_state_t *state, int fd)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL) {
        return false;
    }
    if (client->subscribed) {
        state->subscribed--;
    }
    if (state->attached > 0U) {
        state->attached--;
    }
    client_reset(client);
    return true;
}

bool curve_ws_subscribe(curve_ws_state_t *state, int fd)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL) {
        return false;
    }
    if (!client->subscribed) {
        state->subscribed++;
    }
    client->subscribed = true;
    client->next_seq = 0U;   /* 0 = start from the oldest retained frame */
    client->acked_seq = 0U;
    client->last_push_ms = 0U;
    return true;
}

bool curve_ws_unsubscribe(curve_ws_state_t *state, int fd)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL) {
        return false;
    }
    if (client->subscribed) {
        state->subscribed--;
    }
    client->subscribed = false;
    return true;
}

bool curve_ws_ack(curve_ws_state_t *state, int fd, uint32_t seq)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL) {
        return false;
    }
    /* Clamp to what was actually planned/sent: the ack can never run ahead of
     * next_seq-1 (next_seq==0 => nothing planned yet). */
    if (client->next_seq == 0U) {
        return true;
    }
    if (seq >= client->next_seq) {
        seq = client->next_seq - 1U;
    }
    if (seq > client->acked_seq) {
        client->acked_seq = seq;
    }
    return true;
}

uint32_t curve_ws_plan_push(curve_ws_state_t *state, int fd, uint32_t oldest,
                            uint32_t latest, uint32_t max_frames,
                            uint32_t now_ms, uint32_t *since_out,
                            uint32_t *first_seq_out, bool *gap_out)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);
    uint32_t since;
    uint32_t first;
    uint32_t count;
    bool gap = false;

    if (since_out != NULL) {
        *since_out = 0U;
    }
    if (first_seq_out != NULL) {
        *first_seq_out = 0U;
    }
    if (gap_out != NULL) {
        *gap_out = false;
    }
    if (client == NULL || !client->subscribed || latest == 0U || oldest == 0U) {
        return 0U;
    }
    if (max_frames == 0U) {
        return 0U;
    }
    if (max_frames > CURVE_WS_MAX_BATCH_FRAMES) {
        max_frames = CURVE_WS_MAX_BATCH_FRAMES;
    }
    /* Per-client push interval (backpressure).  A backwards clock (uint32 wrap)
     * is treated as "interval elapsed" instead of blocking forever. */
    if (client->last_push_ms != 0U && now_ms >= client->last_push_ms &&
        (now_ms - client->last_push_ms) < CURVE_WS_PUSH_INTERVAL_MS) {
        return 0U;
    }

    since = (client->next_seq == 0U) ? 0U : (client->next_seq - 1U);
    if (since == 0U) {
        first = oldest; /* start from the oldest retained frame (HTTP since=0) */
    } else {
        if (since >= latest) {
            return 0U; /* client is up to date */
        }
        first = since + 1U;
        if (first < oldest) {
            first = oldest;
            gap = true;
        }
    }
    if (first > latest) {
        return 0U;
    }
    count = latest - first + 1U;
    if (count > max_frames) {
        count = max_frames;
    }
    if (since_out != NULL) {
        *since_out = since;
    }
    if (first_seq_out != NULL) {
        *first_seq_out = first;
    }
    if (gap_out != NULL) {
        *gap_out = gap;
    }
    return count;
}

void curve_ws_mark_pushed(curve_ws_state_t *state, int fd, uint32_t first_seq,
                          uint32_t count, uint32_t now_ms)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL || count == 0U) {
        return;
    }
    client->next_seq = first_seq + count;
    client->last_push_ms = (now_ms == 0U) ? 1U : now_ms;
    client->sent_batches++;
    client->sent_frames += count;
}

void curve_ws_mark_dropped(curve_ws_state_t *state, int fd, uint32_t now_ms)
{
    curve_ws_client_t *client = curve_ws_find(state, fd);

    if (client == NULL) {
        return;
    }
    client->dropped_batches++;
    /* Back off one push interval so a failing/slow client is not hammered. */
    client->last_push_ms = (now_ms == 0U) ? 1U : now_ms;
}
