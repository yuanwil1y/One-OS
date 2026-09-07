idf_component_register(
    SRCS "board.c" "touch.c" "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_driver_sdspi fatfs sdmmc
)
