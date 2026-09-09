idf_component_register(
    SRCS
        "matter_l2.cpp"
        "matter_l2_bounds.c"
    INCLUDE_DIRS "include"
    REQUIRES
        chip
        mbedtls
        nvs_flash
        esp_timer
        esp_event
        freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
