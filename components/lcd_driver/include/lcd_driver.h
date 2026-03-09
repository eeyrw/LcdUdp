/*
 * HD44780 Character LCD Driver via I2C (PCF8574A expander)
 *
 * Supports 4-bit mode, configurable dimensions, custom characters,
 * and backlight control through the PCF8574A P3 pin.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PCF8574A → HD44780 pin mapping (standard I2C backpack) ---- */

#define LCD_PIN_RS    (1 << 0)   /* P0 → Register Select */
#define LCD_PIN_RW    (1 << 1)   /* P1 → Read/Write      */
#define LCD_PIN_EN    (1 << 2)   /* P2 → Enable           */
#define LCD_PIN_BL    (1 << 3)   /* P3 → Backlight        */
#define LCD_PIN_D4    (1 << 4)   /* P4 → Data bit 4       */
#define LCD_PIN_D5    (1 << 5)   /* P5 → Data bit 5       */
#define LCD_PIN_D6    (1 << 6)   /* P6 → Data bit 6       */
#define LCD_PIN_D7    (1 << 7)   /* P7 → Data bit 7       */

/* ---- HD44780 instruction set ---- */

#define HD44780_CMD_CLEAR_DISPLAY    0x01
#define HD44780_CMD_RETURN_HOME      0x02
#define HD44780_CMD_ENTRY_MODE_SET   0x04
#define HD44780_CMD_DISPLAY_CONTROL  0x08
#define HD44780_CMD_CURSOR_SHIFT     0x10
#define HD44780_CMD_FUNCTION_SET     0x20
#define HD44780_CMD_SET_CGRAM_ADDR   0x40
#define HD44780_CMD_SET_DDRAM_ADDR   0x80

/* Entry mode bits */
#define HD44780_ENTRY_INCREMENT      0x02
#define HD44780_ENTRY_SHIFT_OFF      0x00

/* Display control bits */
#define HD44780_DISPLAY_ON           0x04
#define HD44780_CURSOR_OFF           0x00
#define HD44780_BLINK_OFF            0x00

/* Function set bits */
#define HD44780_4BIT_MODE            0x00
#define HD44780_2LINE                0x08
#define HD44780_5x8DOTS             0x00

/* ---- Maximum screen dimensions ---- */

#define LCD_MAX_COLS    40
#define LCD_MAX_ROWS    4

/* ---- LCD command queue message types ---- */

typedef enum {
    LCD_MSG_INIT,
    LCD_MSG_DEINIT,
    LCD_MSG_SET_BACKLIGHT,
    LCD_MSG_SET_CONTRAST,
    LCD_MSG_SET_BRIGHTNESS,
    LCD_MSG_WRITE_DATA,
    LCD_MSG_SET_CURSOR,
    LCD_MSG_CUSTOM_CHAR,
    LCD_MSG_WRITE_CMD,
    LCD_MSG_FULL_FRAME,
    LCD_MSG_SHOW_STATUS,        /* Display device status info on LCD */
} lcd_msg_type_t;

/* ---- LCD command message (posted to the queue) ---- */

typedef struct {
    lcd_msg_type_t type;
    union {
        struct { uint8_t cols; uint8_t rows; } init;
        struct { uint8_t value; } backlight;
        struct { uint8_t value; } contrast;
        struct { uint8_t value; } brightness;
        struct { uint16_t len; uint8_t data[LCD_MAX_COLS * LCD_MAX_ROWS]; } write_data;
        struct { uint8_t col; uint8_t row; } cursor;
        struct { uint8_t index; uint8_t font[8]; } custom_char;
        struct { uint8_t cmd; } write_cmd;
        struct {
            uint8_t  contrast;
            uint8_t  backlight;
            uint8_t  brightness;
            uint8_t  customchar_mask;
            uint8_t  custom_char_indices[8];
            uint8_t  custom_chars[8][8];
            uint8_t  num_custom_chars;
            uint8_t  screen_data[LCD_MAX_COLS * LCD_MAX_ROWS];
            uint16_t screen_data_len;
        } full_frame;
        struct {
            char    ip_str[16];     /* Dotted-decimal IP, e.g. "192.168.1.100" */
            uint16_t port;          /* UDP listening port */
            uint8_t  wifi_connected; /* 1=connected, 0=disconnected */
            uint8_t  pc_connected;   /* 1=PC peer active, 0=not connected */
        } status;
    } data;
} lcd_msg_t;

/* ---- Public API ---- */

/**
 * Initialize I2C bus and add the PCF8574A device.
 * Must be called once before lcd_task_start().
 */
esp_err_t lcd_driver_i2c_init(void);

/**
 * De-initialize the I2C bus.
 */
esp_err_t lcd_driver_i2c_deinit(void);

/**
 * Get the LCD command queue handle.
 * Other tasks use this to post lcd_msg_t messages.
 */
QueueHandle_t lcd_get_cmd_queue(void);

/**
 * Start the LCD consumer task.
 * Creates the FreeRTOS queue and spawns the task that processes lcd_msg_t.
 */
esp_err_t lcd_task_start(void);

/**
 * Get current LCD dimensions. Thread-safe (atomic reads).
 * Returns the values set by the most recent CMD_LCD_INIT.
 */
void lcd_get_dimensions(uint8_t *cols, uint8_t *rows);

/**
 * Send a status display message to the LCD queue.
 * Shows Wi-Fi status, IP address, UDP port, and PC connection state.
 * Call from any task — thread-safe via queue.
 */
esp_err_t lcd_show_status(const char *ip_str, uint16_t port,
                          bool wifi_connected, bool pc_connected);

#ifdef __cplusplus
}
#endif
