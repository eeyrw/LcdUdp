idf_component_register(
    SRCS "protocol.c" "protocol_frag.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
