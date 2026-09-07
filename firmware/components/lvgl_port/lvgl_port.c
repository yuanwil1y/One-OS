#include "lvgl_port.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "lvgl.h"

#define LVGL_TICK_MS    5u
#define LVGL_DRAW_LINES 24u

#if LVGL_VERSION_MAJOR != 8
#error "One-OS board port is currently pinned to LVGL v8"
#endif

#if LV_COLOR_DEPTH != 16
#error "Waveshare ST7789V2 port requires LVGL RGB565 (16-bit)"
#endif

static esp_lcd_panel_handle_t s_panel;
static lv_color_t s_draw_pixels[BOARD_LCD_H_RES * LVGL_DRAW_LINES];
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static esp_timer_handle_t s_tick_timer;
static bool s_initialized;

static bool lcd_color_done(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *event_data,
                           void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    if (drv != NULL) {
        lv_disp_flush_ready(drv);
    }
    return false;
}

static void lvgl_flush(lv_disp_drv_t *drv,
                       const lv_area_t *area,
                       lv_color_t *color_map)
{
    if (drv == NULL || area == NULL || color_map == NULL || s_panel == NULL) {
        if (drv != NULL) {
            lv_disp_flush_ready(drv);
        }
        return;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel,
                                               area->x1,
                                               area->y1,
                                               area->x2 + 1,
                                               area->y2 + 1,
                                               color_map);
    if (err != ESP_OK) {
        lv_disp_flush_ready(drv);
    }
}

static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    if (data == NULL) {
        return;
    }

    data->state = LV_INDEV_STATE_REL;

    board_touch_sample_t sample = {0};
    if (board_touch_read(&sample) != ESP_OK || !sample.pressed) {
        return;
    }

    uint16_t x = sample.x;
    uint16_t y = sample.y;
    if (x >= BOARD_LCD_H_RES) {
        x = BOARD_LCD_H_RES - 1u;
    }
    if (y >= BOARD_LCD_V_RES) {
        y = BOARD_LCD_V_RES - 1u;
    }

    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state = LV_INDEV_STATE_PR;
}

static void lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

esp_err_t lvgl_port_init(const board_lcd_handles_t *lcd)
{
    if (lcd == NULL || lcd->io == NULL || lcd->panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = board_touch_init();
    if (err != ESP_OK) {
        return err;
    }

    lv_init();
    s_panel = lcd->panel;

    lv_disp_draw_buf_init(&s_draw_buf,
                          s_draw_pixels,
                          NULL,
                          sizeof(s_draw_pixels) / sizeof(s_draw_pixels[0]));

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BOARD_LCD_H_RES;
    s_disp_drv.ver_res = BOARD_LCD_V_RES;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.flush_cb = lvgl_flush;

    esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = lcd_color_done,
    };
    err = esp_lcd_panel_io_register_event_callbacks(lcd->io, &callbacks, &s_disp_drv);
    if (err != ESP_OK) {
        return err;
    }
    if (lv_disp_drv_register(&s_disp_drv) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = lvgl_touch_read;
    if (lv_indev_drv_register(&s_indev_drv) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = lvgl_tick,
        .name = "one_lv_tick",
    };
    err = esp_timer_create(&timer_args, &s_tick_timer);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_timer_start_periodic(s_tick_timer, LVGL_TICK_MS * 1000u);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    return board_backlight_set(true);
}
