/*
 * UDP LCD Control Protocol — Packet Definitions and API
 *
 * Protocol version 0x01, little-endian byte order.
 * CRC-16-CCITT (POLY=0x1021, INIT=0xFFFF).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Protocol constants ---------- */

#define PROTO_VERSION           0x01
#define PROTO_HEADER_SIZE       9   /* VER(1)+SEQ(2)+FLAGS(1)+CMD(1)+FRAG_IDX(1)+FRAG_TOTAL(1)+LEN(2) */
#define PROTO_CRC_SIZE          2
#define PROTO_MAX_PAYLOAD       1400
#define PROTO_MAX_PACKET        (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE)

/* ---------- FLAGS bit definitions ---------- */

#define FLAG_ACK_REQ            (1 << 0)
#define FLAG_FRAG               (1 << 1)

/* ---------- Command codes ---------- */

typedef enum {
    CMD_LCD_INIT          = 0x01,
    CMD_LCD_SETBACKLIGHT  = 0x02,
    CMD_LCD_SETCONTRAST   = 0x03,
    CMD_LCD_SETBRIGHTNESS = 0x04,
    CMD_LCD_WRITEDATA     = 0x05,
    CMD_LCD_SETCURSOR     = 0x06,
    CMD_LCD_CUSTOMCHAR    = 0x07,
    CMD_LCD_WRITECMD      = 0x08,
    CMD_LCD_DE_INIT       = 0x0B,
    CMD_HEARTBEAT         = 0x0C,
    CMD_LCD_FULLFRAME     = 0x0D,
    CMD_ENTER_BOOT        = 0x19,
    CMD_ACK               = 0xFF,
} proto_cmd_t;

/* ---------- Heartbeat roles ---------- */

#define HB_ROLE_PC            0x01
#define HB_ROLE_DEVICE        0x02

/* ---------- Heartbeat timing ---------- */

#define HB_INTERVAL_MS        3000
#define HB_MISS_MAX           3

/* ---------- Parsed packet header ---------- */

typedef struct {
    uint8_t  ver;
    uint16_t seq;
    uint8_t  flags;
    uint8_t  cmd;
    uint8_t  frag_idx;
    uint8_t  frag_total;
    uint16_t len;           /* Payload byte count */
} proto_header_t;

/* ---------- Complete parsed packet ---------- */
/*
 * Zero-copy design: payload_ptr points directly into the caller's rx_buf.
 * Reduces struct size from ~1410B to ~24B and eliminates one memcpy.
 *
 * LIFETIME CONSTRAINT: payload_ptr is only valid while the original rx_buf
 * is alive and unmodified. Do NOT store proto_packet_t across recvfrom()
 * calls, enqueue it, or pass it to another task. All consumers must finish
 * within the same recv-loop iteration.
 *
 * NULL SAFETY: payload_ptr is NULL when header.len == 0. Callers must
 * check header.len > 0 before dereferencing.
 */
typedef struct {
    proto_header_t  header;
    const uint8_t  *payload_ptr;    /* Points into caller's rx_buf; NULL if len==0 */
    uint16_t        crc_received;
    uint16_t        crc_computed;
    bool            crc_valid;
} proto_packet_t;

/* ---------- Heartbeat payload (4 bytes) ---------- */

typedef struct __attribute__((packed)) {
    uint8_t  role;      /* HB_ROLE_PC or HB_ROLE_DEVICE */
    uint8_t  hb_seq;    /* 0-255 rolling */
    uint16_t uptime;    /* seconds since boot, little-endian */
} proto_heartbeat_payload_t;

/* ---------- Full-frame fixed header (first 4 bytes of payload) ---------- */

typedef struct __attribute__((packed)) {
    uint8_t contrast;
    uint8_t backlight;
    uint8_t brightness;
    uint8_t customchar_mask;
} proto_fullframe_header_t;

/* ---------- Fragment reassembly ---------- */

#define FRAG_MAX_FRAGMENTS          4
#define FRAG_MAX_SESSIONS           1
#define FRAG_REASSEMBLY_TIMEOUT_MS  2000

typedef struct {
    bool     active;
    uint16_t seq;
    uint8_t  cmd;
    uint8_t  frag_total;
    uint8_t  frags_received;
    uint32_t frag_bitmask;
    uint16_t frag_lengths[FRAG_MAX_FRAGMENTS];
    uint8_t  frag_data[FRAG_MAX_FRAGMENTS][PROTO_MAX_PAYLOAD];
    int64_t  start_time_ms;
} frag_session_t;

typedef struct {
    frag_session_t sessions[FRAG_MAX_SESSIONS];
} frag_reassembly_ctx_t;

/* ---------- Protocol API ---------- */

/**
 * Compute CRC-16-CCITT over a byte buffer.
 * POLY=0x1021, INIT=0xFFFF.
 */
uint16_t proto_crc16(const uint8_t *data, size_t len);

/**
 * Parse a raw UDP buffer into a proto_packet_t.
 * Validates version, size consistency, and CRC.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_CRC,
 *         or ESP_ERR_INVALID_VERSION on failure.
 */
esp_err_t proto_parse_packet(const uint8_t *buf, size_t buf_len, proto_packet_t *out);

/**
 * Build an ACK packet (7 bytes total).
 * @return Number of bytes written, or 0 on buffer too small.
 */
size_t proto_build_ack(uint8_t *buf, size_t buf_size, uint16_t seq, uint8_t status);

/**
 * Build a heartbeat packet (15 bytes total: 9B header + 4B payload + 2B CRC).
 * Uses ROLE=HB_ROLE_DEVICE, FLAGS=0, no ACK/FRAG.
 * @return Number of bytes written, or 0 on buffer too small.
 */
size_t proto_build_heartbeat(uint8_t *buf, size_t buf_size, uint16_t seq,
                             uint8_t hb_seq, uint16_t uptime);

/* ---------- Fragment reassembly API ---------- */

/** Initialize fragment reassembly context (clear all sessions). */
void frag_init(frag_reassembly_ctx_t *ctx);

/** Remove expired sessions (older than FRAG_REASSEMBLY_TIMEOUT_MS). */
void frag_cleanup_expired(frag_reassembly_ctx_t *ctx);

/**
 * Feed a fragment packet into the reassembly engine.
 * @return Pointer to a static internal buffer with the fully reassembled
 *         payload and sets *out_total_len, or NULL if more fragments needed.
 *         The pointer is valid until the next successful reassembly.
 */
uint8_t *frag_feed(frag_reassembly_ctx_t *ctx, const proto_packet_t *pkt,
                   size_t *out_total_len);

#ifdef __cplusplus
}
#endif
