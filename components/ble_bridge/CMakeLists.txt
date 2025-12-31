cmake_minimum_required(VERSION 3.22)

idf_component_register(
    SRCS
        "ble_bridge.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        bt nvs_flash
    PRIV_REQUIRES
        osd
)
