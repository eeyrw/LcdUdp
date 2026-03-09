idf_component_register(
    SRCS "heartbeat.c"
    INCLUDE_DIRS "include"
    REQUIRES protocol network lcd_driver freertos esp_timer log
)
