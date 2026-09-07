#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-C6-Touch-LCD-1.9 board facts. */
#define BOARD_SPI_HOST          SPI2_HOST
#define BOARD_LCD_H_RES         170
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_X_GAP         35
#define BOARD_LCD_Y_GAP         0

#define BOARD_LCD_MOSI_GPIO     4
#define BOARD_LCD_SCLK_GPIO     5
#define BOARD_LCD_DC_GPIO       6
#define BOARD_LCD_CS_GPIO       7
#define BOARD_LCD_RST_GPIO      14
#define BOARD_LCD_BL_GPIO       15

#define BOARD_SD_MISO_GPIO      19
#define BOARD_SD_CS_GPIO        20
#define BOARD_SD_MOUNT_POINT    "/sdcard"

#define BOARD_I2C_SCL_GPIO      8
#define BOARD_I2C_SDA_GPIO      18
#define BOARD_CST816_ADDR       0x15

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
} board_lcd_handles_t;

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
} board_touch_sample_t;

/* Shared SPI2 bus used by LCD and SD. Safe to call more than once. */
esp_err_t board_spi2_init(void);

/* Bring up the 170x320 ST7789V2 panel and return native ESP-IDF handles. */
esp_err_t board_lcd_init(board_lcd_handles_t *out_handles);

/* Backlight is active-low through the board high-side switch. */
esp_err_t board_backlight_set(bool on);

/* Initialize I2C0 and attach the CST816 touch controller. */
esp_err_t board_i2c_init(void);
esp_err_t board_touch_init(void);
esp_err_t board_touch_read(board_touch_sample_t *out_sample);

/* Attach FATFS/SD over the already-owned SPI2 bus. */
esp_err_t board_sd_mount(bool format_if_mount_failed);
esp_err_t board_sd_unmount(void);
bool board_sd_is_mounted(void);
sdmmc_card_t *board_sd_card(void);
esp_err_t board_sd_format_fat(void);

#ifdef __cplusplus
}
#endif
