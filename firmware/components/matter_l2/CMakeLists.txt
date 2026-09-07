idf_component_register(
    SRCS
        "matter_l2.cpp"
        "matter_l2_bounds.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_matter
        esp_matter_controller
        nvs_flash
        esp_timer
        freertos
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
