/*
 * Network component: Wi-Fi manager and UDP server.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Wi-Fi Manager ---- */

/**
 * Initialize Wi-Fi in STA mode.
 * - Tries NVS-stored credentials first.
 * - Falls back to SmartConfig (EspTouch) if no credentials or connect fails.
 * - Saves credentials to NVS on successful SmartConfig.
 * - Blocks until IP address is obtained.
 */
esp_err_t wifi_manager_init(void);

/** Returns true if Wi-Fi STA is connected with a valid IP. */
bool wifi_manager_is_connected(void);

/** Get the current IP address as a dotted-decimal string. */
esp_err_t wifi_manager_get_ip_str(char *buf, size_t buf_len);

/* ---- UDP Server ---- */

/**
 * Start the UDP server task.
 * Creates a UDP socket bound to CONFIG_UDP_LCD_PORT and spawns
 * the receive/dispatch task.
 */
esp_err_t udp_server_start(void);

/**
 * Send raw bytes to the last-known PC peer via UDP.
 * Thread-safe (internal mutex). Returns ESP_ERR_INVALID_STATE if
 * no peer address is known yet (no packet received from PC).
 */
esp_err_t udp_send_to_peer(const uint8_t *data, size_t len);

/**
 * Get the next TX sequence number (atomic increment, wraps at 65535).
 */
uint16_t udp_get_next_seq(void);

#ifdef __cplusplus
}
#endif
