/*
 * Heartbeat module: bidirectional heartbeat for link status detection.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the heartbeat task.
 * Sends CMD_HEARTBEAT every HB_INTERVAL_MS (3s) to the last-known peer.
 * Tracks consecutive misses and reports peer disconnect after HB_MISS_MAX.
 */
esp_err_t heartbeat_start(void);

/**
 * Called by the UDP server task when a CMD_HEARTBEAT packet from the PC
 * (ROLE=0x01) is received. Resets the miss counter.
 */
void heartbeat_on_peer_hb_received(const proto_packet_t *pkt);

/**
 * Returns true if the PC peer is considered connected (heartbeats arriving).
 */
bool heartbeat_is_peer_connected(void);

#ifdef __cplusplus
}
#endif
