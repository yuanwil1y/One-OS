idf_component_register(
    SRCS "esphome_ble_gatt.c" "esphome_ble_gatt_nimble.c" "esphome_api.c" "esphome_api_codec.c"
         "esphome_noise.c" "esphome_noise_crypto.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private" "."
    REQUIRES bt freertos lwip
)
