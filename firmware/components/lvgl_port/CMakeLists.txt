idf_component_register(
    SRCS "lvgl_port.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd esp_timer lvgl
)
