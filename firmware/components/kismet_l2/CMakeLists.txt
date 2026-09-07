idf_component_register(
    SRCS
        "kismet_wifi_tracker.c"
        "kismet_wifi_decode.c"
        "kismet_wifi_session.c"
        "kismet_ble_tracker.c"
        "kismet_ble_session.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_timer nvs_flash bt freertos
)
