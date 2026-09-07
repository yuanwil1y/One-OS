#include "board.h"

#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"

#define SD_MAX_FILES        6
#define SD_ALLOC_UNIT_BYTES 4096

static sdmmc_card_t *s_card;

static sdspi_device_config_t sd_device_config(void)
{
    sdspi_device_config_t config = SDSPI_DEVICE_CONFIG_DEFAULT();
    config.host_id = BOARD_SPI_HOST;
    config.gpio_cs = BOARD_SD_CS_GPIO;
    return config;
}

esp_err_t board_sd_mount(bool format_if_mount_failed)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    esp_err_t err = board_spi2_init();
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    sdspi_device_config_t slot_config = sd_device_config();
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT_BYTES,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT,
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
    }
    return err;
}

esp_err_t board_sd_unmount(void)
{
    if (s_card == NULL) {
        return ESP_OK;
    }

    sdmmc_card_t *card = s_card;
    s_card = NULL;
    return esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, card);
}

bool board_sd_is_mounted(void)
{
    return s_card != NULL;
}

sdmmc_card_t *board_sd_card(void)
{
    return s_card;
}

esp_err_t board_sd_format_fat(void)
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_sdcard_format(BOARD_SD_MOUNT_POINT, s_card);
}
