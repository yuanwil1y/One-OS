#include "board.h"
#include "lvgl_port.h"
#include "smoke_gui.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

void app_main(void)
{
    board_lcd_handles_t lcd = {0};
    ESP_ERROR_CHECK(board_lcd_init(&lcd));
    ESP_ERROR_CHECK(lvgl_port_init(&lcd));
    ESP_ERROR_CHECK(smoke_gui_init());

    /*
     * app_main is the LVGL owner task. Radio and SD smoke tests run on a worker
     * task and return results through a queue; only this task touches LVGL.
     */
    for (;;) {
        smoke_gui_poll();

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
