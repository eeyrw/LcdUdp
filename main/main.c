/*
 * UDP LCD Controller — Application Entry Point
 *
 * Startup sequence:
 *   1. Initialize NVS (for Wi-Fi credential storage)
 *   2. Initialize I2C bus and PCF8574A device
 *   3. Start LCD command consumer task
 *   4. Initialize Wi-Fi (blocks until connected via NVS creds or SmartConfig)
 *   5. Start UDP server (recv + dispatch task)
 *   6. Start heartbeat task
 *
 * After initialization, app_main returns and FreeRTOS tasks take over.
 */

#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "network.h"
#include "lcd_driver.h"
#include "heartbeat.h"
#include "app_state.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== UDP LCD Controller starting ===");

    /* 1. Initialize NVS — required for Wi-Fi credential persistence */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing and re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Initialize I2C bus and add PCF8574A device */
    ESP_LOGI(TAG, "Initializing I2C (SDA=%d, SCL=%d, addr=0x%02X)",
             CONFIG_LCD_I2C_SDA_PIN, CONFIG_LCD_I2C_SCL_PIN, CONFIG_LCD_I2C_ADDR);
    ESP_ERROR_CHECK(lcd_driver_i2c_init());

    /* 3. Start LCD command queue consumer task */
    ESP_ERROR_CHECK(lcd_task_start());

    /* 3a. Initialize LCD hardware with default dimensions and show boot message */
    {
        lcd_msg_t msg = {0};
        msg.type = LCD_MSG_INIT;
        msg.data.init.cols = CONFIG_LCD_DEFAULT_COLS;
        msg.data.init.rows = CONFIG_LCD_DEFAULT_ROWS;
        xQueueSend(lcd_get_cmd_queue(), &msg, portMAX_DELAY);
    }
    /* Show "connecting" status while Wi-Fi is starting */
    lcd_show_status("0.0.0.0", CONFIG_UDP_LCD_PORT, false, false);

    /* 4. Initialize Wi-Fi — blocks until IP address obtained */
    app_set_state(APP_STATE_WIFI_CONNECTING);
    ESP_LOGI(TAG, "Initializing Wi-Fi (STA + SmartConfig fallback)...");
    ESP_ERROR_CHECK(wifi_manager_init());
    app_set_state(APP_STATE_WIFI_CONNECTED);

    char ip_str[16];
    wifi_manager_get_ip_str(ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Wi-Fi connected, IP: %s", ip_str);

    /* 5. Start UDP server */
    ESP_ERROR_CHECK(udp_server_start());
    app_set_state(APP_STATE_RUNNING);
    ESP_LOGI(TAG, "UDP server listening on port %d", CONFIG_UDP_LCD_PORT);

    /* 6. Start heartbeat task */
    ESP_ERROR_CHECK(heartbeat_start());

    /* 7. Show device status on LCD (Wi-Fi up, waiting for PC) */
    lcd_show_status(ip_str, CONFIG_UDP_LCD_PORT, true, false);

    ESP_LOGI(TAG, "=== System ready ===");
    /* app_main returns; FreeRTOS scheduler continues running all tasks */
}
