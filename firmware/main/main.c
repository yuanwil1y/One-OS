/*
 * One-OS application entry point.
 *
 * Startup order:
 *   1. board display/touch bring-up and the LVGL binding (the existing
 *      foundation, preserved);
 *   2. the headless application runtime: operation gate, worker task and
 *      request queue;
 *   3. the serial diagnostic console, which submits requests to that runtime.
 *
 * app_main remains the single LVGL owner task; the runtime and console run on
 * their own tasks, so no radio/protocol work ever blocks the display loop and
 * no LVGL call is made from a protocol callback.
 *
 * The GUI is deliberately not built here yet. The console exercises the same
 * runtime entry points the GUI will call, which is what makes the headless
 * workflow testable before any screen exists.
 */

#include "app_diag_console.h"
#include "app_runtime.h"
#include "board.h"
#include "lvgl_port.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app_main";

void app_main(void)
{
    board_lcd_handles_t lcd = {0};
    esp_err_t err;

    ESP_ERROR_CHECK(board_lcd_init(&lcd));
    ESP_ERROR_CHECK(lvgl_port_init(&lcd));

    /* Headless runtime first: the console needs it, and a runtime failure must
     * be visible rather than swallowed by the display loop below. */
    err = app_runtime_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "headless runtime failed to start: %s", esp_err_to_name(err));
    } else {
        err = app_diag_console_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "diagnostic console failed to start: %s",
                     esp_err_to_name(err));
        }
    }

    /* app_main is the single LVGL owner task in the foundation firmware. */
    for (;;) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 1u) {
            delay_ms = 1u;
        } else if (delay_ms > 20u) {
            delay_ms = 20u;
        }

        TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        if (delay_ticks == 0) {
            delay_ticks = 1;
        }
        vTaskDelay(delay_ticks);
    }
}
