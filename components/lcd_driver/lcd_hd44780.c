/*
 * HD44780 LCD low-level driver via PCF8574A I2C expander.
 *
 * All functions in this file are intended to be called ONLY from lcd_task
 * (single-threaded access to I2C bus). No mutex protection needed.
 */

#include "lcd_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdatomic.h>

static const char *TAG = "lcd_hw";

/* ---- I2C handles ---- */

static i2c_master_bus_handle_t s_i2c_bus   = NULL;
static i2c_master_dev_handle_t s_i2c_dev   = NULL;

/* ---- LCD state (only modified from lcd_task) ---- */

static uint8_t  s_cols      = CONFIG_LCD_DEFAULT_COLS;
static uint8_t  s_rows      = CONFIG_LCD_DEFAULT_ROWS;
static uint8_t  s_backlight = LCD_PIN_BL;   /* On by default */
static bool     s_initialized = false;

/* Atomic copies for cross-task reads */
static _Atomic uint8_t s_cols_atomic = CONFIG_LCD_DEFAULT_COLS;
static _Atomic uint8_t s_rows_atomic = CONFIG_LCD_DEFAULT_ROWS;

/* HD44780 DDRAM row offsets (up to 4 rows) */
static const uint8_t s_row_offsets[4] = { 0x00, 0x40, 0x14, 0x54 };

/* ---------- I2C / PCF8574A primitives ---------- */

static esp_err_t pcf8574_write(uint8_t byte_val)
{
    return i2c_master_transmit(s_i2c_dev, &byte_val, 1, 50);
}

/* Write a 4-bit nibble with EN pulse.
 * data_nibble: value in bits 4-7 of PCF8574A output byte.
 * rs: 0 for command, LCD_PIN_RS for data. */
static esp_err_t lcd_write_nibble(uint8_t data_nibble, uint8_t rs)
{
    uint8_t out = data_nibble | rs | s_backlight;

    esp_err_t ret;
    /* EN high */
    ret = pcf8574_write(out | LCD_PIN_EN);
    if (ret != ESP_OK) return ret;
    esp_rom_delay_us(1);

    /* EN low — data latched on falling edge */
    ret = pcf8574_write(out & ~LCD_PIN_EN);
    if (ret != ESP_OK) return ret;
    esp_rom_delay_us(50);

    return ESP_OK;
}

/* Write a full byte in 4-bit mode (high nibble first, then low nibble). */
static esp_err_t lcd_write_byte(uint8_t byte, uint8_t rs)
{
    esp_err_t ret;
    /* High nibble: shift byte[7:4] into PCF8574A P4-P7 */
    ret = lcd_write_nibble(byte & 0xF0, rs);
    if (ret != ESP_OK) return ret;
    /* Low nibble: shift byte[3:0] into P4-P7 */
    ret = lcd_write_nibble((byte << 4) & 0xF0, rs);
    return ret;
}

/* ---- Public low-level helpers used by lcd_task ---- */

esp_err_t lcd_send_cmd(uint8_t cmd)
{
    return lcd_write_byte(cmd, 0);
}

esp_err_t lcd_send_data(uint8_t data)
{
    return lcd_write_byte(data, LCD_PIN_RS);
}

esp_err_t lcd_set_cursor_hw(uint8_t col, uint8_t row)
{
    if (row >= s_rows) row = s_rows - 1;
    if (col >= s_cols) col = s_cols - 1;
    uint8_t addr = s_row_offsets[row] + col;
    return lcd_send_cmd(HD44780_CMD_SET_DDRAM_ADDR | addr);
}

esp_err_t lcd_create_custom_char_hw(uint8_t index, const uint8_t font[8])
{
    if (index > 7) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = lcd_send_cmd(HD44780_CMD_SET_CGRAM_ADDR | (index << 3));
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < 8; i++) {
        ret = lcd_send_data(font[i]);
        if (ret != ESP_OK) return ret;
    }

    return ESP_OK;
}

esp_err_t lcd_set_backlight_hw(uint8_t on)
{
    s_backlight = on ? LCD_PIN_BL : 0;
    return pcf8574_write(s_backlight);
}

/* ---- HD44780 4-bit initialization sequence ---- */

esp_err_t lcd_hd44780_init(uint8_t cols, uint8_t rows)
{
    esp_err_t ret;

    s_cols = (cols > LCD_MAX_COLS) ? LCD_MAX_COLS : cols;
    s_rows = (rows > LCD_MAX_ROWS) ? LCD_MAX_ROWS : rows;
    atomic_store(&s_cols_atomic, s_cols);
    atomic_store(&s_rows_atomic, s_rows);

    /* Wait >40ms after power-on */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* PCF8574A initial state: all low + backlight */
    pcf8574_write(s_backlight);
    vTaskDelay(pdMS_TO_TICKS(10));

    /*
     * HD44780 reset sequence (datasheet Figure 24):
     * Send 0x03 three times in 8-bit-like mode to ensure sync,
     * then switch to 4-bit mode with 0x02.
     */

    /* Attempt 1: Function set 8-bit */
    ret = lcd_write_nibble(0x30, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(5));    /* Wait >4.1ms */

    /* Attempt 2 */
    ret = lcd_write_nibble(0x30, 0);
    if (ret != ESP_OK) return ret;
    esp_rom_delay_us(150);           /* Wait >100us */

    /* Attempt 3 */
    ret = lcd_write_nibble(0x30, 0);
    if (ret != ESP_OK) return ret;
    esp_rom_delay_us(150);

    /* Switch to 4-bit mode */
    ret = lcd_write_nibble(0x20, 0);
    if (ret != ESP_OK) return ret;
    esp_rom_delay_us(150);

    /* Function set: 4-bit, 2-line, 5×8 dots */
    uint8_t func = HD44780_CMD_FUNCTION_SET | HD44780_4BIT_MODE | HD44780_5x8DOTS;
    if (s_rows > 1) {
        func |= HD44780_2LINE;
    }
    ret = lcd_send_cmd(func);
    if (ret != ESP_OK) return ret;

    /* Display ON, cursor off, blink off */
    ret = lcd_send_cmd(HD44780_CMD_DISPLAY_CONTROL | HD44780_DISPLAY_ON
                       | HD44780_CURSOR_OFF | HD44780_BLINK_OFF);
    if (ret != ESP_OK) return ret;

    /* Clear display */
    ret = lcd_send_cmd(HD44780_CMD_CLEAR_DISPLAY);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(2));    /* Clear takes ~1.52ms */

    /* Entry mode: increment cursor, no display shift */
    ret = lcd_send_cmd(HD44780_CMD_ENTRY_MODE_SET | HD44780_ENTRY_INCREMENT
                       | HD44780_ENTRY_SHIFT_OFF);
    if (ret != ESP_OK) return ret;

    s_initialized = true;
    ESP_LOGI(TAG, "HD44780 initialized: %dx%d", s_cols, s_rows);
    return ESP_OK;
}

/* ---- I2C bus initialization ---- */

esp_err_t lcd_driver_i2c_init(void)
{
    /* Configure I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_LCD_I2C_SDA_PIN,
        .scl_io_num = CONFIG_LCD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add PCF8574A device */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_LCD_I2C_ADDR,
        .scl_speed_hz = CONFIG_LCD_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, addr=0x%02X)",
             CONFIG_LCD_I2C_SDA_PIN, CONFIG_LCD_I2C_SCL_PIN, CONFIG_LCD_I2C_ADDR);
    return ESP_OK;
}

esp_err_t lcd_driver_i2c_deinit(void)
{
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    return ESP_OK;
}

/* ---- Cross-task dimension query ---- */

void lcd_get_dimensions(uint8_t *cols, uint8_t *rows)
{
    if (cols) *cols = atomic_load(&s_cols_atomic);
    if (rows) *rows = atomic_load(&s_rows_atomic);
}

/* ---- Accessors for lcd_task.c ---- */

bool lcd_is_initialized(void)
{
    return s_initialized;
}

uint8_t lcd_get_cols(void) { return s_cols; }
uint8_t lcd_get_rows(void) { return s_rows; }
