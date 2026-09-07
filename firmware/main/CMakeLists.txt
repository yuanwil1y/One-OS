idf_component_register(
    SRCS
        "main.c"
        "smoke_gui.c"
        "smoke_tests.c"
    INCLUDE_DIRS "."
    REQUIRES
        board
        lvgl_port
        lvgl
        freertos
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        bt
        ieee802154
)
