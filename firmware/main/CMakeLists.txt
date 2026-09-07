idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board lvgl_port lvgl freertos matter_l2
)
