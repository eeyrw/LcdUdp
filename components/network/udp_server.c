/*
 * UDP server: receive packets, parse protocol, dispatch to LCD/heartbeat.
 *
 * Thread safety: sendto() is protected by s_send_mutex (shared with
 * heartbeat task). Fragment reassembly is single-threaded (this task only).
 */

#include "network.h"
#include "protocol.h"
#include "lcd_driver.h"
#include "heartbeat.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "udp_srv";

#define UDP_TASK_STACK_SIZE     6144
#define UDP_TASK_PRIORITY       6
#define UDP_RX_BUF_SIZE         1500

/* ---- Module state ---- */

static int                s_sock = -1;
static struct sockaddr_in s_peer_addr;
static bool               s_peer_known = false;
static SemaphoreHandle_t  s_send_mutex = NULL;
static _Atomic uint16_t   s_tx_seq = 0;
static frag_reassembly_ctx_t s_frag_ctx;

/* ---- Forward declarations ---- */

static void udp_server_task(void *arg);
static void send_ack(uint16_t seq, uint8_t status);
static void dispatch_command(uint8_t cmd, const uint8_t *payload, size_t len);
static void dispatch_fullframe(const uint8_t *payload, size_t len);

/* ---- Public API ---- */

uint16_t udp_get_next_seq(void)
{
    return atomic_fetch_add(&s_tx_seq, 1);
}

esp_err_t udp_send_to_peer(const uint8_t *data, size_t len)
{
    if (!s_peer_known || s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_send_mutex, portMAX_DELAY);
    int ret = sendto(s_sock, data, len, 0,
                     (struct sockaddr *)&s_peer_addr, sizeof(s_peer_addr));
    xSemaphoreGive(s_send_mutex);
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t udp_server_start(void)
{
    s_send_mutex = xSemaphoreCreateMutex();
    if (!s_send_mutex) {
        ESP_LOGE(TAG, "Failed to create send mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(udp_server_task, "udp_srv",
                                  UDP_TASK_STACK_SIZE, NULL,
                                  UDP_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UDP server task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ---- UDP receive task ---- */

static void udp_server_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[UDP_RX_BUF_SIZE];

    /* Create UDP socket */
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Bind to configured port */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_UDP_LCD_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP server listening on port %d", CONFIG_UDP_LCD_PORT);

    frag_init(&s_frag_ctx);

    /* Receive loop */
    for (;;) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(s_sock, rx_buf, sizeof(rx_buf), 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Remember peer address for responses (ACK, heartbeat) */
        xSemaphoreTake(s_send_mutex, portMAX_DELAY);
        memcpy(&s_peer_addr, &source_addr, sizeof(s_peer_addr));
        s_peer_known = true;
        xSemaphoreGive(s_send_mutex);

        /* Parse packet */
        proto_packet_t pkt;
        esp_err_t err = proto_parse_packet(rx_buf, (size_t)len, &pkt);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Packet parse failed: %s (len=%d)",
                     esp_err_to_name(err), len);
            continue;
        }

        /* Heartbeat — handle immediately, bypass LCD queue */
        if (pkt.header.cmd == CMD_HEARTBEAT) {
            heartbeat_on_peer_hb_received(&pkt);
            if (pkt.header.flags & FLAG_ACK_REQ) {
                send_ack(pkt.header.seq, 0);
            }
            continue;
        }

        /* Determine effective payload (fragment reassembly if needed) */
        const uint8_t *payload = pkt.payload_ptr;
        size_t payload_len = pkt.header.len;

        if (pkt.header.flags & FLAG_FRAG) {
            size_t total_len = 0;
            uint8_t *reassembled = frag_feed(&s_frag_ctx, &pkt, &total_len);
            if (!reassembled) {
                /* Still waiting for more fragments */
                continue;
            }
            payload = reassembled;
            payload_len = total_len;
        }

        /* Dispatch command */
        dispatch_command(pkt.header.cmd, payload, payload_len);

        /* Send ACK if requested */
        if (pkt.header.flags & FLAG_ACK_REQ) {
            send_ack(pkt.header.seq, 0);
        }

        /* Periodic fragment cleanup */
        frag_cleanup_expired(&s_frag_ctx);
    }
}

/* ---- ACK sender ---- */

static void send_ack(uint16_t seq, uint8_t status)
{
    uint8_t buf[16];
    size_t len = proto_build_ack(buf, sizeof(buf), seq, status);
    if (len > 0) {
        udp_send_to_peer(buf, len);
    }
}

/* ---- Command dispatcher ---- */

static void dispatch_command(uint8_t cmd, const uint8_t *payload, size_t len)
{
    lcd_msg_t msg = {0};
    QueueHandle_t q = lcd_get_cmd_queue();
    if (!q) {
        ESP_LOGW(TAG, "LCD queue not ready");
        return;
    }

    switch (cmd) {
    case CMD_LCD_INIT:
        if (len >= 2) {
            msg.type = LCD_MSG_INIT;
            msg.data.init.cols = payload[0];
            msg.data.init.rows = payload[1];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_SETBACKLIGHT:
        if (len >= 1) {
            msg.type = LCD_MSG_SET_BACKLIGHT;
            msg.data.backlight.value = payload[0];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_SETCONTRAST:
        if (len >= 1) {
            msg.type = LCD_MSG_SET_CONTRAST;
            msg.data.contrast.value = payload[0];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_SETBRIGHTNESS:
        if (len >= 1) {
            msg.type = LCD_MSG_SET_BRIGHTNESS;
            msg.data.brightness.value = payload[0];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_WRITEDATA:
        if (len >= 2) {
            msg.type = LCD_MSG_WRITE_DATA;
            /* Payload: len(2B LE) + data(lenB) */
            uint16_t data_len = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            if (data_len > sizeof(msg.data.write_data.data)) {
                data_len = sizeof(msg.data.write_data.data);
            }
            if (len < 2 + data_len) {
                data_len = (uint16_t)(len - 2);
            }
            msg.data.write_data.len = data_len;
            memcpy(msg.data.write_data.data, &payload[2], data_len);
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_SETCURSOR:
        if (len >= 2) {
            msg.type = LCD_MSG_SET_CURSOR;
            msg.data.cursor.col = payload[0];
            msg.data.cursor.row = payload[1];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_CUSTOMCHAR:
        if (len >= 9) {
            msg.type = LCD_MSG_CUSTOM_CHAR;
            msg.data.custom_char.index = payload[0];
            memcpy(msg.data.custom_char.font, &payload[1], 8);
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_WRITECMD:
        if (len >= 1) {
            msg.type = LCD_MSG_WRITE_CMD;
            msg.data.write_cmd.cmd = payload[0];
            xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        }
        break;

    case CMD_LCD_DE_INIT:
        msg.type = LCD_MSG_DEINIT;
        xQueueSend(q, &msg, pdMS_TO_TICKS(100));
        break;

    case CMD_LCD_FULLFRAME:
        dispatch_fullframe(payload, len);
        break;

    case CMD_ENTER_BOOT:
        ESP_LOGW(TAG, "CMD_ENTER_BOOT received — stub, no action taken");
        break;

    default:
        ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
        break;
    }
}

/* ---- Full-frame parser ---- */

static void dispatch_fullframe(const uint8_t *payload, size_t len)
{
    if (len < 4) {
        ESP_LOGW(TAG, "FULLFRAME: payload too short (%d < 4)", (int)len);
        return;
    }

    lcd_msg_t msg = {0};
    msg.type = LCD_MSG_FULL_FRAME;
    msg.data.full_frame.contrast       = payload[0];
    msg.data.full_frame.backlight      = payload[1];
    msg.data.full_frame.brightness     = payload[2];
    msg.data.full_frame.customchar_mask = payload[3];

    size_t offset = 4;
    uint8_t mask = payload[3];
    uint8_t num_cc = 0;

    /* Parse custom character data based on mask bits */
    for (int i = 0; i < 8 && mask != 0; i++) {
        if (mask & (1 << i)) {
            if (offset + 9 > len) {
                ESP_LOGW(TAG, "FULLFRAME: truncated custom char data");
                return;
            }
            msg.data.full_frame.custom_char_indices[num_cc] = payload[offset];
            memcpy(msg.data.full_frame.custom_chars[num_cc],
                   &payload[offset + 1], 8);
            num_cc++;
            offset += 9;  /* INDEX(1) + FONT(8) */
        }
    }
    msg.data.full_frame.num_custom_chars = num_cc;

    /* Remaining bytes are screen data (COL × ROW, row-major) */
    size_t screen_len = len - offset;
    if (screen_len > sizeof(msg.data.full_frame.screen_data)) {
        screen_len = sizeof(msg.data.full_frame.screen_data);
    }
    memcpy(msg.data.full_frame.screen_data, &payload[offset], screen_len);
    msg.data.full_frame.screen_data_len = (uint16_t)screen_len;

    QueueHandle_t q = lcd_get_cmd_queue();
    if (q) {
        xQueueSend(q, &msg, pdMS_TO_TICKS(100));
    }
}
