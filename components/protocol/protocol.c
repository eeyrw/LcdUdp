/*
 * Protocol core: CRC-16-CCITT, packet parsing, ACK and heartbeat builders.
 */

#include "protocol.h"
#include <string.h>

/* ---- CRC-16-CCITT (POLY=0x1021, INIT=0xFFFF) ---- */

uint16_t proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/* ---- Packet parser ---- */

esp_err_t proto_parse_packet(const uint8_t *buf, size_t buf_len, proto_packet_t *out)
{
    /* Minimum: header(9) + CRC(2) = 11 bytes */
    if (buf_len < PROTO_HEADER_SIZE + PROTO_CRC_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Parse header — little-endian */
    out->header.ver        = buf[0];
    out->header.seq        = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
    out->header.flags      = buf[3];
    out->header.cmd        = buf[4];
    out->header.frag_idx   = buf[5];
    out->header.frag_total = buf[6];
    out->header.len        = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);

    /* Version check */
    if (out->header.ver != PROTO_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    /* Payload length bounds */
    if (out->header.len > PROTO_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Total length consistency */
    size_t expected = PROTO_HEADER_SIZE + out->header.len + PROTO_CRC_SIZE;
    if (buf_len < expected) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Zero-copy: point directly into caller's buffer */
    out->payload_ptr = (out->header.len > 0) ? &buf[PROTO_HEADER_SIZE] : NULL;

    /* CRC verification */
    size_t crc_offset = PROTO_HEADER_SIZE + out->header.len;
    out->crc_received = (uint16_t)buf[crc_offset] | ((uint16_t)buf[crc_offset + 1] << 8);
    out->crc_computed = proto_crc16(buf, crc_offset);
    out->crc_valid    = (out->crc_received == out->crc_computed);

    if (!out->crc_valid) {
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

/* ---- ACK builder ---- */

size_t proto_build_ack(uint8_t *buf, size_t buf_size, uint16_t seq, uint8_t status)
{
    /* ACK packet: VER(1) + SEQ(2) + CMD(1) + STATUS(1) + CRC(2) = 7 bytes */
    if (buf_size < 7) {
        return 0;
    }

    buf[0] = PROTO_VERSION;
    buf[1] = (uint8_t)(seq & 0xFF);
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = CMD_ACK;
    buf[4] = status;

    uint16_t crc = proto_crc16(buf, 5);
    buf[5] = (uint8_t)(crc & 0xFF);
    buf[6] = (uint8_t)(crc >> 8);

    return 7;
}

/* ---- Heartbeat builder ---- */

size_t proto_build_heartbeat(uint8_t *buf, size_t buf_size, uint16_t seq,
                             uint8_t hb_seq, uint16_t uptime)
{
    /* Standard packet: header(9) + payload(4) + CRC(2) = 15 bytes */
    const size_t total = PROTO_HEADER_SIZE + 4 + PROTO_CRC_SIZE;
    if (buf_size < total) {
        return 0;
    }

    /* Header */
    buf[0] = PROTO_VERSION;
    buf[1] = (uint8_t)(seq & 0xFF);
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = 0x00;             /* FLAGS: no ACK_REQ, no FRAG */
    buf[4] = CMD_HEARTBEAT;
    buf[5] = 0;               /* FRAG_IDX  */
    buf[6] = 1;               /* FRAG_TOTAL */
    buf[7] = 4;               /* LEN low  */
    buf[8] = 0;               /* LEN high */

    /* Payload: ROLE + HB_SEQ + UPTIME(LE) */
    buf[9]  = HB_ROLE_DEVICE;
    buf[10] = hb_seq;
    buf[11] = (uint8_t)(uptime & 0xFF);
    buf[12] = (uint8_t)(uptime >> 8);

    /* CRC over VER..PAYLOAD */
    uint16_t crc = proto_crc16(buf, 13);
    buf[13] = (uint8_t)(crc & 0xFF);
    buf[14] = (uint8_t)(crc >> 8);

    return total;
}
