# UDP LCD Controller (ESP32-C3)

ESP32-C3 firmware that receives UDP packets and drives an HD44780 character LCD via I2C (PCF8574A expander). Built with ESP-IDF.

## Hardware

- **MCU:** ESP32-C3
- **LCD:** HD44780 compatible (default 20x4, configurable via protocol)
- **I2C Expander:** PCF8574A (standard backpack, addr 0x38-0x3F)
- **Pin Mapping (PCF8574A -> HD44780):**

| PCF8574A Pin | HD44780 Signal |
|:------------:|:--------------:|
| P0           | RS             |
| P1           | RW             |
| P2           | EN             |
| P3           | Backlight      |
| P4-P7        | D4-D7 (4-bit)  |

## Build

```bash
# Set target
idf.py set-target esp32c3

# (Optional) Customize configuration
idf.py menuconfig

# Build and flash
idf.py build flash monitor
```

Requires ESP-IDF v5.5+.

## Configuration (menuconfig)

All options are under `UDP LCD Controller Configuration`:

| Option                        | Default  | Range        | Description                              |
|-------------------------------|----------|--------------|------------------------------------------|
| `UDP_LCD_PORT`                | 5000     | 1024-65535   | UDP listening port                       |
| `LCD_I2C_SDA_PIN`            | 4        | 0-21         | I2C SDA GPIO                             |
| `LCD_I2C_SCL_PIN`            | 5        | 0-21         | I2C SCL GPIO                             |
| `LCD_I2C_ADDR`               | 0x3F     | 0x38-0x3F    | PCF8574A I2C address                     |
| `LCD_DEFAULT_COLS`           | 20       | —            | Default LCD columns                      |
| `LCD_DEFAULT_ROWS`           | 4        | 1-4          | Default LCD rows                         |
| `LCD_I2C_FREQ_HZ`           | 100000   | —            | I2C clock frequency (Hz)                 |
| `HEARTBEAT_LED_GPIO`        | 8        | -1 to 21     | Heartbeat indicator LED (-1 = disabled)  |
| `WIFI_LED_GPIO`             | 10       | -1 to 21     | Wi-Fi status LED (-1 = disabled)         |
| `WIFI_STA_MAX_RETRIES`      | 3        | 1-20         | STA retries before SmartConfig fallback  |
| `WIFI_STA_RETRY_INTERVAL_MS`| 2000     | 500-30000    | Delay between STA retries (ms)           |

## Project Structure

```
udp_lcd_ctrl/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── app_state.h
│   └── main.c
└── components/
    ├── protocol/           # Packet parsing, CRC, ACK/HB builders, fragment reassembly
    │   ├── include/protocol.h
    │   ├── protocol.c
    │   └── protocol_frag.c
    ├── lcd_driver/         # HD44780 4-bit I2C driver + FreeRTOS queue consumer
    │   ├── include/lcd_driver.h
    │   ├── lcd_hd44780.c
    │   └── lcd_task.c
    ├── network/            # Wi-Fi STA + SmartConfig + UDP server
    │   ├── include/network.h
    │   ├── wifi_manager.c
    │   └── udp_server.c
    └── heartbeat/          # Bidirectional heartbeat + LED indicator
        ├── include/heartbeat.h
        └── heartbeat.c
```

## FreeRTOS Tasks

| Task             | Stack  | Priority | Role                                      |
|------------------|--------|----------|-------------------------------------------|
| `udp_srv`        | 6144   | 6        | UDP recv loop, parse, dispatch, ACK, frag  |
| `lcd_task`       | 4096   | 5        | Serialize all HD44780 I2C operations       |
| `heartbeat`      | 3072   | 4        | Send heartbeat, track peer, toggle LED     |

## Protocol

- **Version:** 0x01, little-endian
- **CRC:** CRC-16-CCITT (POLY=0x1021, INIT=0xFFFF), covers header + payload
- **Header:** 9 bytes — `VER(1) + SEQ(2) + FLAGS(1) + CMD(1) + FRAG_IDX(1) + FRAG_TOTAL(1) + LEN(2)`
- **Max payload:** 1400 bytes
- **Flags:** `ACK_REQ` (bit 0), `FRAG` (bit 1)

### Commands

| Code | Command              | Payload                                        |
|------|----------------------|------------------------------------------------|
| 0x01 | LCD_INIT             | COLS(1) + ROWS(1)                              |
| 0x02 | LCD_SETBACKLIGHT     | VALUE(1)                                       |
| 0x03 | LCD_SETCONTRAST      | VALUE(1)                                       |
| 0x04 | LCD_SETBRIGHTNESS    | VALUE(1)                                       |
| 0x05 | LCD_WRITEDATA        | LEN(2, LE) + DATA(N)                           |
| 0x06 | LCD_SETCURSOR        | COL(1) + ROW(1)                                |
| 0x07 | LCD_CUSTOMCHAR       | INDEX(1) + FONT(8)                             |
| 0x08 | LCD_WRITECMD         | CMD(1)                                         |
| 0x0B | LCD_DE_INIT          | (none)                                         |
| 0x0C | HEARTBEAT            | ROLE(1) + HB_SEQ(1) + UPTIME(2, LE)            |
| 0x0D | LCD_FULLFRAME        | CONTRAST(1) + BL(1) + BR(1) + CC_MASK(1) + ... |
| 0x19 | ENTER_BOOT           | (stub, no action)                              |
| 0xFF | ACK                  | STATUS(1)                                      |

### Heartbeat

- Device role: 0x02, PC role: 0x01
- Interval: 3 seconds
- Miss threshold: 3 (9 seconds to detect disconnect)
- Independent of business commands

### Full Frame (0x0D)

Payload layout:

```
CONTRAST(1) + BACKLIGHT(1) + BRIGHTNESS(1) + CUSTOMCHAR_MASK(1)
+ [INDEX(1) + FONT(8)] × popcount(CUSTOMCHAR_MASK)
+ SCREEN_DATA(COL × ROW)
```

When `CUSTOMCHAR_MASK=0`, custom character data is omitted (normal frame optimization).

### Fragment Reassembly

- Up to 4 fragments per session, 1 concurrent session
- Reassembly timeout: 2 seconds
- Uses `FLAG_FRAG` bit and `FRAG_IDX`/`FRAG_TOTAL` header fields

## Wi-Fi Provisioning

1. On boot, attempt to connect with NVS-stored credentials
2. If no credentials or connection fails after configured retries (default 3, spaced 2s apart), fall back to SmartConfig (EspTouch)
3. On SmartConfig success, credentials are saved to NVS for subsequent boots

### LED Indicators

| LED            | State               | Behavior          |
|----------------|----------------------|-------------------|
| Wi-Fi LED      | Disconnected         | Off               |
| Wi-Fi LED      | SmartConfig active   | Fast blink (200ms)|
| Wi-Fi LED      | Connected            | Steady on         |
| Heartbeat LED  | System running       | Toggle every 3s   |

## LCD Status Display

When no PC is controlling the LCD (heartbeat lost), the display shows:

```
WiFi: Connected
IP:192.168.1.100
Port:5000
PC: Waiting...
```

Any UDP LCD command from the PC takes over the display. When the PC disconnects (heartbeat timeout), the status screen returns automatically.

## Data Flow

```
[PC] --UDP--> udp_server_task --parse/CRC/frag--> dispatch
  ├── CMD_HEARTBEAT --> heartbeat_on_peer_hb_received()
  ├── CMD_LCD_*     --> lcd_msg_t --> xQueueSend --> lcd_task --> I2C --> LCD
  └── ACK_REQ       --> proto_build_ack --> sendto(peer)

heartbeat_task --every 3s--> proto_build_heartbeat --> sendto(peer)
                          --> toggle LED
                          --> miss tracking --> lcd_show_status() on disconnect
```
