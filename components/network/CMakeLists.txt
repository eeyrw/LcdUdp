idf_component_register(
    SRCS "wifi_manager.c" "udp_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_smartconfig nvs_flash
              lwip protocol lcd_driver heartbeat freertos esp_log
)
