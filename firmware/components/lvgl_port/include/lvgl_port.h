#pragma once

#include "board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thin LVGL v8 binding for the board LCD + CST816 input. */
esp_err_t lvgl_port_init(const board_lcd_handles_t *lcd);

#ifdef __cplusplus
}
#endif
