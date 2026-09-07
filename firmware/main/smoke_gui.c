#include "smoke_gui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "smoke_tests.h"

#define WORKER_STACK_BYTES 8192u
#define WORKER_PRIORITY 5u

static QueueHandle_t s_command_queue;
static QueueHandle_t s_result_queue;
static lv_obj_t *s_buttons[SMOKE_TEST_COUNT];
static lv_obj_t *s_status_labels[SMOKE_TEST_COUNT];
static lv_obj_t *s_footer;
static lv_obj_t *s_format_modal;
static bool s_busy;

static const char *const s_test_names[SMOKE_TEST_COUNT] = {
    [SMOKE_TEST_WIFI] = "Wi-Fi",
    [SMOKE_TEST_BLE] = "BLE",
    [SMOKE_TEST_I154] = "802.15.4",
    [SMOKE_TEST_SD_RW] = "SD R/W",
    [SMOKE_TEST_SD_FORMAT] = "SD Format",
};

static void set_buttons_enabled(bool enabled)
{
    for (size_t i = 0; i < SMOKE_TEST_COUNT; ++i) {
        if (s_buttons[i] == NULL) {
            continue;
        }
        if (enabled) {
            lv_obj_clear_state(s_buttons[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_buttons[i], LV_STATE_DISABLED);
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        smoke_test_kind_t kind;
        if (xQueueReceive(s_command_queue, &kind, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        smoke_test_result_t result;
        (void)smoke_test_run(kind, &result);
        (void)xQueueSend(s_result_queue, &result, portMAX_DELAY);
    }
}

static void start_command(smoke_test_kind_t kind)
{
    if (s_busy || kind >= SMOKE_TEST_COUNT) {
        return;
    }

    if (xQueueSend(s_command_queue, &kind, 0) != pdTRUE) {
        lv_label_set_text(s_footer, "Queue error");
        return;
    }

    s_busy = true;
    set_buttons_enabled(false);
    lv_label_set_text(s_status_labels[kind], "RUNNING...");
    lv_label_set_text_fmt(s_footer, "Testing %s", s_test_names[kind]);
}

static void run_button_event(lv_event_t *event)
{
    const smoke_test_kind_t kind =
        (smoke_test_kind_t)(uintptr_t)lv_event_get_user_data(event);
    start_command(kind);
}

static void close_format_modal(bool reenable_buttons)
{
    if (s_format_modal != NULL) {
        lv_obj_del(s_format_modal);
        s_format_modal = NULL;
    }
    if (reenable_buttons && !s_busy) {
        set_buttons_enabled(true);
    }
}

static void format_cancel_event(lv_event_t *event)
{
    (void)event;
    close_format_modal(true);
    lv_label_set_text(s_footer, "Format cancelled");
}

static void format_confirm_event(lv_event_t *event)
{
    (void)event;
    close_format_modal(false);
    start_command(SMOKE_TEST_SD_FORMAT);
}

static void format_request_event(lv_event_t *event)
{
    (void)event;
    if (s_busy || s_format_modal != NULL) {
        return;
    }

    set_buttons_enabled(false);
    s_format_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_format_modal, 158, 156);
    lv_obj_center(s_format_modal);
    lv_obj_clear_flag(s_format_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_format_modal);
    lv_label_set_text(title, "FORMAT SD CARD?");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *warning = lv_label_create(s_format_modal);
    lv_obj_set_width(warning, 138);
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warning,
                      "This erases the FAT filesystem.\n"
                      "After formatting, One-OS will run a write/read verify.");
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *cancel = lv_btn_create(s_format_modal);
    lv_obj_set_size(cancel, 60, 34);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_event_cb(cancel, format_cancel_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "CANCEL");
    lv_obj_center(cancel_label);

    lv_obj_t *confirm = lv_btn_create(s_format_modal);
    lv_obj_set_size(confirm, 60, 34);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_add_event_cb(confirm, format_confirm_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_label = lv_label_create(confirm);
    lv_label_set_text(confirm_label, "FORMAT");
    lv_obj_center(confirm_label);
}

static void create_test_row(smoke_test_kind_t kind, int32_t y)
{
    lv_obj_t *row = lv_obj_create(lv_scr_act());
    lv_obj_set_size(row, 160, 48);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, s_test_names[kind]);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 2, 1);

    lv_obj_t *status = lv_label_create(row);
    lv_obj_set_width(status, 94);
    lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
    lv_label_set_text(status, "READY");
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    s_status_labels[kind] = status;

    lv_obj_t *button = lv_btn_create(row);
    lv_obj_set_size(button, 52, 34);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -2, 0);
    s_buttons[kind] = button;

    if (kind == SMOKE_TEST_SD_FORMAT) {
        lv_obj_add_event_cb(button, format_request_event, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(button,
                            run_button_event,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)kind);
    }

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, kind == SMOKE_TEST_SD_FORMAT ? "FMT" : "RUN");
    lv_obj_center(button_label);
}

esp_err_t smoke_gui_init(void)
{
    s_command_queue = xQueueCreate(1, sizeof(smoke_test_kind_t));
    s_result_queue = xQueueCreate(4, sizeof(smoke_test_result_t));
    if (s_command_queue == NULL || s_result_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(worker_task,
                    "smoke_worker",
                    WORKER_STACK_BYTES,
                    NULL,
                    WORKER_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_clean(lv_scr_act());

    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "One-OS Smoke  beta.1");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    for (size_t i = 0; i < SMOKE_TEST_COUNT; ++i) {
        create_test_row((smoke_test_kind_t)i, 28 + (int32_t)i * 52);
    }

    s_footer = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_footer, 160);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_footer, "Ready - tap RUN");
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -3);

    return ESP_OK;
}

void smoke_gui_poll(void)
{
    if (s_result_queue == NULL) {
        return;
    }

    smoke_test_result_t result;
    while (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        if (result.kind >= SMOKE_TEST_COUNT) {
            continue;
        }

        if (result.err == ESP_OK) {
            lv_label_set_text_fmt(s_status_labels[result.kind], "PASS %s", result.detail);
            lv_label_set_text_fmt(s_footer, "%s passed", s_test_names[result.kind]);
        } else {
            lv_label_set_text_fmt(s_status_labels[result.kind],
                                  "FAIL %s",
                                  esp_err_to_name(result.err));
            lv_label_set_text_fmt(s_footer,
                                  "%s failed: %s",
                                  s_test_names[result.kind],
                                  esp_err_to_name(result.err));
        }

        s_busy = false;
        set_buttons_enabled(true);
    }
}
