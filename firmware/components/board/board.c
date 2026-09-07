#include "board.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_lcd_panel_st7789.h"

#define LCD_PIXEL_CLOCK_HZ      (20 * 1000 * 1000)
#define LCD_SPI_QUEUE_DEPTH     10
#define LCD_MAX_TRANSFER_BYTES  (BOARD_LCD_H_RES * 40 * sizeof(uint16_t))

static bool s_spi2_initialized;

/*
 * Espressif's public ST7789 driver handles the generic panel lifecycle.
 * These porch/power/gamma values are retained from the proven Waveshare
 * ESP-IDF board sequence for this exact display module.
 */
static esp_err_t board_lcd_apply_vendor_init(esp_lcd_panel_io_handle_t io)
{
    static const uint8_t b2[] = {0x0c, 0x0c, 0x00, 0x33, 0x33};
    static const uint8_t b7[] = {0x35};
    static const uint8_t bb[] = {0x13};
    static const uint8_t c0[] = {0x2c};
    static const uint8_t c2[] = {0x01};
    static const uint8_t c3[] = {0x0b};
    static const uint8_t c4[] = {0x20};
    static const uint8_t c6[] = {0x0f};
    static const uint8_t d0[] = {0xa4, 0xa1};
    static const uint8_t d6[] = {0xa1};
    static const uint8_t e0[] = {
        0x00, 0x03, 0x07, 0x08, 0x07, 0x15, 0x2a,
        0x44, 0x42, 0x0a, 0x17, 0x18, 0x25, 0x27,
    };
    static const uint8_t e1[] = {
        0x00, 0x03, 0x08, 0x07, 0x07, 0x23, 0x2a,
        0x43, 0x42, 0x09, 0x18, 0x17, 0x25, 0x27,
    };

    struct lcd_init_step {
        int command;
        const uint8_t *data;
        size_t data_size;
    };

    static const struct lcd_init_step init_steps[] = {
        {0xb2, b2, sizeof(b2)},
        {0xb7, b7, sizeof(b7)},
        {0xbb, bb, sizeof(bb)},
        {0xc0, c0, sizeof(c0)},
        {0xc2, c2, sizeof(c2)},
        {0xc3, c3, sizeof(c3)},
        {0xc4, c4, sizeof(c4)},
        {0xc6, c6, sizeof(c6)},
        {0xd0, d0, sizeof(d0)},
        {0xd6, d6, sizeof(d6)},
        {0xe0, e0, sizeof(e0)},
        {0xe1, e1, sizeof(e1)},
    };

    for (size_t i = 0; i < sizeof(init_steps) / sizeof(init_steps[0]); ++i) {
        esp_err_t err = esp_lcd_panel_io_tx_param(io,
                                                  init_steps[i].command,
                                                  init_steps[i].data,
                                                  init_steps[i].data_size);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t board_spi2_init(void)
{
    if (s_spi2_initialized) {
        return ESP_OK;
    }

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_LCD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER_BYTES,
    };

    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    s_spi2_initialized = true;
    return ESP_OK;
}

esp_err_t board_backlight_set(bool on)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    return gpio_set_level(BOARD_LCD_BL_GPIO, on ? 0 : 1);
}

esp_err_t board_lcd_init(board_lcd_handles_t *out_handles)
{
    if (out_handles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    out_handles->io = NULL;
    out_handles->panel = NULL;

    esp_err_t err = board_backlight_set(false);
    if (err != ESP_OK) {
        return err;
    }

    err = board_spi2_init();
    if (err != ESP_OK) {
        return err;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = LCD_SPI_QUEUE_DEPTH,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST,
                                   &io_cfg,
                                   &out_handles->io);
    if (err != ESP_OK) {
        return err;
    }

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(out_handles->io, &panel_cfg, &out_handles->panel);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_reset(out_handles->panel);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_init(out_handles->panel);
    if (err != ESP_OK) {
        return err;
    }

    err = board_lcd_apply_vendor_init(out_handles->io);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_set_gap(out_handles->panel, BOARD_LCD_X_GAP, BOARD_LCD_Y_GAP);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_invert_color(out_handles->panel, true);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_lcd_panel_disp_on_off(out_handles->panel, true);
    if (err != ESP_OK) {
        return err;
    }

    /* Keep the panel dark until the LVGL display/input binding is ready. */
    return ESP_OK;
}
