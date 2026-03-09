/*
 * Heartbeat: periodic device heartbeat sender + PC peer miss tracking
 *            + LED heartbeat indicator.
 *
 * Design per protocol spec §12:
 * - Device sends CMD_HEARTBEAT (ROLE=0x02) every HB_INTERVAL_MS.
 * - On each send, miss_count increments.
 * - When a PC heartbeat (ROLE=0x01) is received, miss_count resets to 0.
 * - If miss_count >= HB_MISS_MAX, peer is considered disconnected.
 * - Heartbeats are independent of business commands.
 *
 * LED: toggles on each heartbeat cycle (CONFIG_HEARTBEAT_LED_GPIO).
 *      Set GPIO to -1 in menuconfig to disable.
 */

#include "heartbeat.h"
#include "protocol.h"
#include "network.h"
#include "lcd_driver.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdatomic.h>

static const char *TAG = "heartbeat";

#define HB_TASK_STACK_SIZE  3072
#define HB_TASK_PRIORITY    4

/* ---- State ---- */

static uint8_t         s_hb_seq = 0;          /* Rolling 0-255 */
static _Atomic uint8_t s_miss_count = 0;
static _Atomic bool    s_peer_connected = false;
static int64_t         s_boot_time_ms = 0;

/* ---- Notify LCD of connection state change ---- */

static void notify_lcd_status(bool pc_connected)
{
    char ip_str[16] = {0};
    bool wifi_ok = wifi_manager_is_connected();
    if (wifi_ok) {
        wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
    }
    lcd_show_status(ip_str, CONFIG_UDP_LCD_PORT, wifi_ok, pc_connected);
}

/* ---- Heartbeat task ---- */

static void heartbeat_task(void *arg)
{
    (void)arg;
    s_boot_time_ms = esp_timer_get_time() / 1000;

    /* Initialize heartbeat LED if configured */
    bool led_enabled = false;
    bool led_state = false;
#if CONFIG_HEARTBEAT_LED_GPIO >= 0
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_HEARTBEAT_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_set_level(CONFIG_HEARTBEAT_LED_GPIO, 0);
        led_enabled = true;
    }
#endif

    ESP_LOGI(TAG, "Heartbeat task started (interval=%dms, miss_max=%d)",
             HB_INTERVAL_MS, HB_MISS_MAX);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HB_INTERVAL_MS));

        /* Toggle heartbeat LED */
        if (led_enabled) {
            led_state = !led_state;
            gpio_set_level(CONFIG_HEARTBEAT_LED_GPIO, led_state ? 1 : 0);
        }

        /* Compute uptime in seconds */
        int64_t now_ms = esp_timer_get_time() / 1000;
        uint16_t uptime = (uint16_t)((now_ms - s_boot_time_ms) / 1000);

        /* Build and send heartbeat packet */
        uint8_t buf[32];
        uint16_t seq = udp_get_next_seq();
        size_t len = proto_build_heartbeat(buf, sizeof(buf), seq,
                                            s_hb_seq, uptime);
        if (len > 0) {
            udp_send_to_peer(buf, len);
        }
        s_hb_seq++;

        /* Increment miss counter */
        uint8_t miss = atomic_fetch_add(&s_miss_count, 1) + 1;

        if (miss >= HB_MISS_MAX) {
            bool was_connected = atomic_exchange(&s_peer_connected, false);
            if (was_connected) {
                ESP_LOGW(TAG, "PC heartbeat lost (miss=%d), peer disconnected", miss);
                notify_lcd_status(false);
            }
        }
    }
}

/* ---- Public API ---- */

esp_err_t heartbeat_start(void)
{
    BaseType_t ret = xTaskCreate(heartbeat_task, "heartbeat",
                                  HB_TASK_STACK_SIZE, NULL,
                                  HB_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void heartbeat_on_peer_hb_received(const proto_packet_t *pkt)
{
    /* Verify sender is PC (ROLE=0x01) */
    if (pkt->header.len < 1 || pkt->payload_ptr[0] != HB_ROLE_PC) {
        return;
    }

    bool was_disconnected = !atomic_exchange(&s_peer_connected, true);
    atomic_store(&s_miss_count, 0);

    if (was_disconnected) {
        ESP_LOGI(TAG, "PC heartbeat restored");
    }

    /* Debug: log peer info if payload is complete */
    if (pkt->header.len >= sizeof(proto_heartbeat_payload_t)) {
        const proto_heartbeat_payload_t *hb =
            (const proto_heartbeat_payload_t *)pkt->payload_ptr;
        ESP_LOGD(TAG, "PC HB: seq=%d, uptime=%ds", hb->hb_seq, hb->uptime);
    }
}

bool heartbeat_is_peer_connected(void)
{
    return atomic_load(&s_peer_connected);
}
