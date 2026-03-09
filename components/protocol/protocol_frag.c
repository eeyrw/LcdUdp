/*
 * Fragment reassembly engine for the UDP LCD protocol.
 *
 * Manages up to FRAG_MAX_SESSIONS concurrent reassembly sessions.
 * Each session can hold up to FRAG_MAX_FRAGMENTS fragments.
 * Sessions expire after FRAG_REASSEMBLY_TIMEOUT_MS.
 */

#include "protocol.h"
#include "esp_timer.h"
#include <string.h>

/* Static reassembly output buffer — single-consumer (udp_server_task only) */
static uint8_t s_reassembly_buf[FRAG_MAX_FRAGMENTS * PROTO_MAX_PAYLOAD];

void frag_init(frag_reassembly_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

/* Find existing session with matching SEQ, or allocate a new one. */
static frag_session_t *find_or_create_session(frag_reassembly_ctx_t *ctx,
                                               uint16_t seq, uint8_t cmd,
                                               uint8_t frag_total)
{
    /* Search for existing */
    for (int i = 0; i < FRAG_MAX_SESSIONS; i++) {
        if (ctx->sessions[i].active && ctx->sessions[i].seq == seq) {
            return &ctx->sessions[i];
        }
    }

    /* Find an inactive slot */
    for (int i = 0; i < FRAG_MAX_SESSIONS; i++) {
        if (!ctx->sessions[i].active) {
            frag_session_t *s = &ctx->sessions[i];
            memset(s, 0, sizeof(*s));
            s->active     = true;
            s->seq        = seq;
            s->cmd        = cmd;
            s->frag_total = frag_total;
            s->start_time_ms = esp_timer_get_time() / 1000;
            return s;
        }
    }

    /* All slots occupied — evict the oldest */
    int oldest = 0;
    int64_t oldest_time = INT64_MAX;
    for (int i = 0; i < FRAG_MAX_SESSIONS; i++) {
        if (ctx->sessions[i].start_time_ms < oldest_time) {
            oldest_time = ctx->sessions[i].start_time_ms;
            oldest = i;
        }
    }
    frag_session_t *s = &ctx->sessions[oldest];
    memset(s, 0, sizeof(*s));
    s->active     = true;
    s->seq        = seq;
    s->cmd        = cmd;
    s->frag_total = frag_total;
    s->start_time_ms = esp_timer_get_time() / 1000;
    return s;
}

uint8_t *frag_feed(frag_reassembly_ctx_t *ctx, const proto_packet_t *pkt,
                   size_t *out_total_len)
{
    uint8_t idx   = pkt->header.frag_idx;
    uint8_t total = pkt->header.frag_total;

    /* Sanity checks */
    if (idx >= total || total > FRAG_MAX_FRAGMENTS || total == 0) {
        return NULL;
    }

    frag_session_t *s = find_or_create_session(ctx, pkt->header.seq,
                                                pkt->header.cmd, total);
    if (!s) {
        return NULL;
    }

    /* Store fragment (skip duplicates) */
    if (!(s->frag_bitmask & (1u << idx))) {
        if (pkt->header.len > PROTO_MAX_PAYLOAD) {
            return NULL;
        }
        if (pkt->header.len > 0 && pkt->payload_ptr != NULL) {
            memcpy(s->frag_data[idx], pkt->payload_ptr, pkt->header.len);
        }
        s->frag_lengths[idx] = pkt->header.len;
        s->frag_bitmask |= (1u << idx);
        s->frags_received++;
    }

    /* Check completeness */
    if (s->frags_received >= s->frag_total) {
        /* Reassemble in FRAG_IDX order */
        size_t total_len = 0;
        for (uint8_t i = 0; i < s->frag_total; i++) {
            memcpy(&s_reassembly_buf[total_len], s->frag_data[i], s->frag_lengths[i]);
            total_len += s->frag_lengths[i];
        }
        *out_total_len = total_len;
        s->active = false;
        return s_reassembly_buf;
    }

    return NULL;
}

void frag_cleanup_expired(frag_reassembly_ctx_t *ctx)
{
    int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < FRAG_MAX_SESSIONS; i++) {
        if (ctx->sessions[i].active &&
            (now - ctx->sessions[i].start_time_ms) > FRAG_REASSEMBLY_TIMEOUT_MS) {
            ctx->sessions[i].active = false;
        }
    }
}
