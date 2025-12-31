idf_component_register(
    SRCS "ra_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES ble_bridge esp_timer
)
