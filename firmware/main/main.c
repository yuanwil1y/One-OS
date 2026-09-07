#include "board.h"
#include "lvgl_port.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

void app_main(void)
{
    board_lcd_handles_t lcd = {0};
    ESP_ERROR_CHECK(board_lcd_init(&lcd));
    ESP_ERROR_CHECK(lvgl_port_init(&lcd));

    /*
     * app_main is the initial LVGL owner task. Keep all ordinary LVGL calls on
     * this task until an application deliberately introduces its own UI owner.
     */
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
