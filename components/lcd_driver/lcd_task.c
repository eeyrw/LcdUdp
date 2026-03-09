/*
 * LCD command queue consumer task.
 *
 * Receives lcd_msg_t messages from the FreeRTOS queue and dispatches
 * them to the HD44780 hardware driver. All I2C access is serialized
 * through this single task — no external locking required.
 *
 * Status display: when PC is not controlling the LCD (no active heartbeat),
 * the LCD shows device info (Wi-Fi status, IP, port). Any UDP LCD command
 * sets s_pc_controlling=true; LCD_MSG_SHOW_STATUS sets it back to false.
 */

#include "lcd_driver.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "lcd_task";

#define LCD_TASK_STACK_SIZE   4096
#define LCD_TASK_PRIORITY     5
#define LCD_QUEUE_DEPTH       16

static QueueHandle_t s_lcd_queue = NULL;

/* PC control tracking: true = LCD owned by PC commands, false = show status */
static bool s_pc_controlling = false;

/* ---- Extern functions from lcd_hd44780.c ---- */

extern esp_err_t lcd_hd44780_init(uint8_t cols, uint8_t rows);
extern esp_err_t lcd_send_cmd(uint8_t cmd);
extern esp_err_t lcd_send_data(uint8_t data);
extern esp_err_t lcd_set_cursor_hw(uint8_t col, uint8_t row);
extern esp_err_t lcd_create_custom_char_hw(uint8_t index, const uint8_t font[8]);
extern esp_err_t lcd_set_backlight_hw(uint8_t on);
extern bool      lcd_is_initialized(void);
extern uint8_t   lcd_get_cols(void);
extern uint8_t   lcd_get_rows(void);

/* ---- Stored state for contrast/brightness (no HW support on HD44780) ---- */

static uint8_t s_contrast   = 0;
static uint8_t s_brightness = 0;

/* ---- Helper: write a string to LCD at current cursor, pad to width ---- */

static void lcd_write_str_padded(const char *str, uint8_t width)
{
    uint8_t i = 0;
    while (str[i] && i < width) {
        lcd_send_data((uint8_t)str[i]);
        i++;
    }
    /* Pad remaining with spaces */
    while (i < width) {
        lcd_send_data(' ');
        i++;
    }
}

/* ---- Status display handler ---- */

static void handle_show_status(const lcd_msg_t *msg)
{
    if (!lcd_is_initialized()) return;

    s_pc_controlling = false;
    lcd_set_backlight_hw(1);

    uint8_t cols = lcd_get_cols();
    uint8_t rows = lcd_get_rows();
    const typeof(msg->data.status) *st = &msg->data.status;

    char line[LCD_MAX_COLS + 1];

    /* Row 0: Wi-Fi status */
    lcd_set_cursor_hw(0, 0);
    if (st->wifi_connected) {
        snprintf(line, sizeof(line), "WiFi: Connected");
    } else {
        snprintf(line, sizeof(line), "WiFi: Disconnected");
    }
    lcd_write_str_padded(line, cols);

    /* Row 1: IP address */
    if (rows >= 2) {
        lcd_set_cursor_hw(0, 1);
        snprintf(line, sizeof(line), "IP:%s", st->ip_str);
        lcd_write_str_padded(line, cols);
    }

    /* Row 2: UDP port */
    if (rows >= 3) {
        lcd_set_cursor_hw(0, 2);
        snprintf(line, sizeof(line), "Port:%u", st->port);
        lcd_write_str_padded(line, cols);
    }

    /* Row 3: PC connection status */
    if (rows >= 4) {
        lcd_set_cursor_hw(0, 3);
        if (st->pc_connected) {
            snprintf(line, sizeof(line), "PC: Connected");
        } else {
            snprintf(line, sizeof(line), "PC: Waiting...");
        }
        lcd_write_str_padded(line, cols);
    }
}

/* ---- Full-frame handler ---- */

static void handle_full_frame(const lcd_msg_t *msg)
{
    if (!lcd_is_initialized()) {
        ESP_LOGW(TAG, "FULLFRAME: LCD not initialized, dropping");
        return;
    }

    const typeof(msg->data.full_frame) *ff = &msg->data.full_frame;

    /* Apply backlight */
    lcd_set_backlight_hw(ff->backlight);

    /* Store contrast and brightness (no direct HD44780 control) */
    s_contrast   = ff->contrast;
    s_brightness = ff->brightness;

    /* Write custom characters if mask is non-zero */
    if (ff->customchar_mask != 0) {
        for (uint8_t i = 0; i < ff->num_custom_chars; i++) {
            lcd_create_custom_char_hw(ff->custom_char_indices[i],
                                      ff->custom_chars[i]);
        }
    }

    /* Write screen data row by row (atomic full-screen update) */
    uint8_t cols = lcd_get_cols();
    uint8_t rows = lcd_get_rows();
    uint16_t expected_len = (uint16_t)cols * rows;

    if (ff->screen_data_len < expected_len) {
        ESP_LOGW(TAG, "FULLFRAME: screen data too short (%d < %d)",
                 ff->screen_data_len, expected_len);
        expected_len = ff->screen_data_len;
    }

    for (uint8_t r = 0; r < rows; r++) {
        lcd_set_cursor_hw(0, r);
        for (uint8_t c = 0; c < cols; c++) {
            uint16_t idx = (uint16_t)r * cols + c;
            if (idx < expected_len) {
                lcd_send_data(ff->screen_data[idx]);
            }
        }
    }
}

/* ---- LCD consumer task ---- */

static void lcd_task(void *arg)
{
    lcd_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_lcd_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (msg.type) {
        case LCD_MSG_SHOW_STATUS:
            handle_show_status(&msg);
            break;

        case LCD_MSG_INIT:
            ESP_LOGI(TAG, "INIT: %dx%d", msg.data.init.cols, msg.data.init.rows);
            lcd_hd44780_init(msg.data.init.cols, msg.data.init.rows);
            s_pc_controlling = true;
            break;

        case LCD_MSG_DEINIT:
            ESP_LOGI(TAG, "DEINIT");
            lcd_send_cmd(HD44780_CMD_CLEAR_DISPLAY);
            lcd_set_backlight_hw(0);
            s_pc_controlling = false;
            break;

        case LCD_MSG_SET_BACKLIGHT:
            s_pc_controlling = true;
            lcd_set_backlight_hw(msg.data.backlight.value);
            break;

        case LCD_MSG_SET_CONTRAST:
            s_pc_controlling = true;
            s_contrast = msg.data.contrast.value;
            ESP_LOGD(TAG, "Contrast stored: %d", s_contrast);
            break;

        case LCD_MSG_SET_BRIGHTNESS:
            s_pc_controlling = true;
            s_brightness = msg.data.brightness.value;
            ESP_LOGD(TAG, "Brightness stored: %d", s_brightness);
            break;

        case LCD_MSG_WRITE_DATA:
            if (!lcd_is_initialized()) break;
            s_pc_controlling = true;
            for (uint16_t i = 0; i < msg.data.write_data.len; i++) {
                lcd_send_data(msg.data.write_data.data[i]);
            }
            break;

        case LCD_MSG_SET_CURSOR:
            if (!lcd_is_initialized()) break;
            s_pc_controlling = true;
            lcd_set_cursor_hw(msg.data.cursor.col, msg.data.cursor.row);
            break;

        case LCD_MSG_CUSTOM_CHAR:
            if (!lcd_is_initialized()) break;
            s_pc_controlling = true;
            lcd_create_custom_char_hw(msg.data.custom_char.index,
                                      msg.data.custom_char.font);
            break;

        case LCD_MSG_WRITE_CMD:
            if (!lcd_is_initialized()) break;
            s_pc_controlling = true;
            lcd_send_cmd(msg.data.write_cmd.cmd);
            break;

        case LCD_MSG_FULL_FRAME:
            s_pc_controlling = true;
            handle_full_frame(&msg);
            break;

        default:
            ESP_LOGW(TAG, "Unknown message type: %d", msg.type);
            break;
        }
    }
}

/* ---- Public API ---- */

QueueHandle_t lcd_get_cmd_queue(void)
{
    return s_lcd_queue;
}

esp_err_t lcd_task_start(void)
{
    s_lcd_queue = xQueueCreate(LCD_QUEUE_DEPTH, sizeof(lcd_msg_t));
    if (!s_lcd_queue) {
        ESP_LOGE(TAG, "Failed to create LCD queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(lcd_task, "lcd_task",
                                  LCD_TASK_STACK_SIZE, NULL,
                                  LCD_TASK_PRIORITY, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LCD task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LCD task started (queue depth=%d)", LCD_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t lcd_show_status(const char *ip_str, uint16_t port,
                          bool wifi_connected, bool pc_connected)
{
    QueueHandle_t q = lcd_get_cmd_queue();
    if (!q) return ESP_ERR_INVALID_STATE;

    lcd_msg_t msg = {0};
    msg.type = LCD_MSG_SHOW_STATUS;
    strncpy(msg.data.status.ip_str, ip_str ? ip_str : "0.0.0.0",
            sizeof(msg.data.status.ip_str) - 1);
    msg.data.status.port = port;
    msg.data.status.wifi_connected = wifi_connected ? 1 : 0;
    msg.data.status.pc_connected = pc_connected ? 1 : 0;

    return (xQueueSend(q, &msg, pdMS_TO_TICKS(100)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}
