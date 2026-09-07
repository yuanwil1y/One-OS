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

#define COLOR_PAGE       lv_color_hex(0xF3F7F3)
#define COLOR_HEADER     lv_color_hex(0x1B5E20)
#define COLOR_GREEN      lv_color_hex(0x2E7D32)
#define COLOR_GREEN_DARK lv_color_hex(0x1B5E20)
#define COLOR_GREEN_SOFT lv_color_hex(0xA5D6A7)
#define COLOR_CARD       lv_color_hex(0xFFFFFF)
#define COLOR_TEXT       lv_color_hex(0x17351B)
#define COLOR_MUTED      lv_color_hex(0x607D63)
#define COLOR_BLUE       lv_color_hex(0x1565C0)
#define COLOR_RED        lv_color_hex(0xC62828)
#define COLOR_DISABLED   lv_color_hex(0x9E9E9E)

static QueueHandle_t s_command_queue;
static QueueHandle_t s_result_queue;
static lv_obj_t *s_buttons[SMOKE_TEST_COUNT];
static lv_obj_t *s_status_labels[SMOKE_TEST_COUNT];
static lv_obj_t *s_cards[SMOKE_TEST_COUNT];
static lv_obj_t *s_list;
static lv_obj_t *s_format_overlay;
static bool s_busy;

static const char *const s_test_names[SMOKE_TEST_COUNT] = {
    [SMOKE_TEST_WIFI] = "Wi-Fi Scan",
    [SMOKE_TEST_BLE] = "BLE Scan",
    [SMOKE_TEST_I154] = "802.15.4 Sweep",
    [SMOKE_TEST_SD_RW] = "SD Read / Write",
    [SMOKE_TEST_SD_FORMAT] = "SD Format",
};

static const char *const s_test_hints[SMOKE_TEST_COUNT] = {
    [SMOKE_TEST_WIFI] = "Active AP discovery",
    [SMOKE_TEST_BLE] = "3 second passive discovery",
    [SMOKE_TEST_I154] = "Channels 11-26",
    [SMOKE_TEST_SD_RW] = "Write, sync, read, compare",
    [SMOKE_TEST_SD_FORMAT] = "Destructive; confirmation required",
};

static void style_button(lv_obj_t *button, bool destructive)
{
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button,
                              destructive ? COLOR_RED : COLOR_GREEN,
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button,
                              destructive ? lv_color_hex(0x8E0000) : COLOR_GREEN_DARK,
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button,
                              COLOR_DISABLED,
                              LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_PART_MAIN);
}

static void set_buttons_enabled(bool enabled)
{
    for (size_t i = 0; i < SMOKE_TEST_COUNT; ++i) {
        if (s_buttons[i] == NULL) continue;
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
        if (xQueueReceive(s_command_queue, &kind, portMAX_DELAY) != pdTRUE) continue;

        smoke_test_result_t result;
        (void)smoke_test_run(kind, &result);
        (void)xQueueSend(s_result_queue, &result, portMAX_DELAY);
    }
}

static void start_command(smoke_test_kind_t kind)
{
    if (s_busy || kind >= SMOKE_TEST_COUNT) return;

    if (xQueueSend(s_command_queue, &kind, 0) != pdTRUE) {
        lv_label_set_text(s_status_labels[kind], "Queue error");
        lv_obj_set_style_text_color(s_status_labels[kind], COLOR_RED, LV_PART_MAIN);
        return;
    }

    s_busy = true;
    set_buttons_enabled(false);
    lv_label_set_text(s_status_labels[kind], "Running...");
    lv_obj_set_style_text_color(s_status_labels[kind], COLOR_BLUE, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cards[kind], COLOR_BLUE, LV_PART_MAIN);
    lv_obj_scroll_to_view(s_cards[kind], LV_ANIM_ON);
}

static void run_button_event(lv_event_t *event)
{
    smoke_test_kind_t kind =
        (smoke_test_kind_t)(uintptr_t)lv_event_get_user_data(event);
    start_command(kind);
}

static void close_format_overlay(bool reenable)
{
    if (s_format_overlay != NULL) {
        lv_obj_del(s_format_overlay);
        s_format_overlay = NULL;
    }
    if (reenable && !s_busy) set_buttons_enabled(true);
}

static void format_cancel_event(lv_event_t *event)
{
    (void)event;
    close_format_overlay(true);
    lv_label_set_text(s_status_labels[SMOKE_TEST_SD_FORMAT], "Cancelled - card unchanged");
    lv_obj_set_style_text_color(s_status_labels[SMOKE_TEST_SD_FORMAT], COLOR_MUTED, LV_PART_MAIN);
}

static void format_confirm_event(lv_event_t *event)
{
    (void)event;
    close_format_overlay(false);
    start_command(SMOKE_TEST_SD_FORMAT);
}

static void format_request_event(lv_event_t *event)
{
    (void)event;
    if (s_busy || s_format_overlay != NULL) return;

    set_buttons_enabled(false);

    s_format_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_format_overlay, 170, 320);
    lv_obj_center(s_format_overlay);
    lv_obj_clear_flag(s_format_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_format_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_format_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_format_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_format_overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_format_overlay, LV_OPA_60, LV_PART_MAIN);

    lv_obj_t *dialog = lv_obj_create(s_format_overlay);
    lv_obj_set_size(dialog, 154, 188);
    lv_obj_center(dialog);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dialog, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(dialog, COLOR_RED, LV_PART_MAIN);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dialog, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text(title, "Format SD card?");
    lv_obj_set_style_text_color(title, COLOR_RED, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *warning = lv_label_create(dialog);
    lv_obj_set_size(warning, 128, 76);
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    lv_label_set_text(warning,
                      "This erases the FAT filesystem.\n\n"
                      "After formatting, a write/read verify will run.");
    lv_obj_set_style_text_color(warning, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, 35);

    lv_obj_t *cancel = lv_btn_create(dialog);
    lv_obj_set_size(cancel, 58, 36);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 4, -6);
    lv_obj_add_event_cb(cancel, format_cancel_event, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(cancel, COLOR_DISABLED, LV_PART_MAIN);
    lv_obj_set_style_radius(cancel, 8, LV_PART_MAIN);
    lv_obj_t *cancel_text = lv_label_create(cancel);
    lv_label_set_text(cancel_text, "Cancel");
    lv_obj_center(cancel_text);

    lv_obj_t *confirm = lv_btn_create(dialog);
    lv_obj_set_size(confirm, 58, 36);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, -4, -6);
    lv_obj_add_event_cb(confirm, format_confirm_event, LV_EVENT_CLICKED, NULL);
    style_button(confirm, true);
    lv_obj_t *confirm_text = lv_label_create(confirm);
    lv_label_set_text(confirm_text, "Format");
    lv_obj_center(confirm_text);
}

static void create_test_card(smoke_test_kind_t kind)
{
    lv_obj_t *card = lv_obj_create(s_list);
    lv_obj_set_size(card, 154, 92);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COLOR_GREEN_SOFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    s_cards[kind] = card;

    lv_obj_t *name = lv_label_create(card);
    lv_obj_set_width(name, 86);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_label_set_text(name, s_test_names[kind]);
    lv_obj_set_style_text_color(name, COLOR_TEXT, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_width(hint, 86);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_label_set_text(hint, s_test_hints[kind]);
    lv_obj_set_style_text_color(hint, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 8, 29);

    lv_obj_t *button = lv_btn_create(card);
    lv_obj_set_size(button, 52, 36);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -7, 8);
    style_button(button, kind == SMOKE_TEST_SD_FORMAT);
    s_buttons[kind] = button;

    if (kind == SMOKE_TEST_SD_FORMAT) {
        lv_obj_add_event_cb(button, format_request_event, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(button,
                            run_button_event,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)kind);
    }

    lv_obj_t *button_text = lv_label_create(button);
    lv_label_set_text(button_text, kind == SMOKE_TEST_SD_FORMAT ? "FMT" : "RUN");
    lv_obj_center(button_text);

    lv_obj_t *status = lv_label_create(card);
    lv_obj_set_size(status, 136, 30);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status, "Ready");
    lv_obj_set_style_text_color(status, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 8, -5);
    s_status_labels[kind] = status;
}

esp_err_t smoke_gui_init(void)
{
    s_command_queue = xQueueCreate(1, sizeof(smoke_test_kind_t));
    s_result_queue = xQueueCreate(4, sizeof(smoke_test_result_t));
    if (s_command_queue == NULL || s_result_queue == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(worker_task,
                    "smoke_worker",
                    WORKER_STACK_BYTES,
                    NULL,
                    WORKER_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, COLOR_PAGE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, 170, 44);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "One-OS Smoke Test");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    lv_obj_t *subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "beta.2 - scroll list");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xC8E6C9), LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_MID, 0, -4);

    s_list = lv_obj_create(screen);
    lv_obj_set_size(s_list, 170, 276);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_list, COLOR_PAGE, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_list, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);

    for (size_t i = 0; i < SMOKE_TEST_COUNT; ++i) {
        create_test_card((smoke_test_kind_t)i);
    }

    return ESP_OK;
}

void smoke_gui_poll(void)
{
    if (s_result_queue == NULL) return;

    smoke_test_result_t result;
    while (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        if (result.kind >= SMOKE_TEST_COUNT) continue;

        if (result.err == ESP_OK) {
            lv_label_set_text_fmt(s_status_labels[result.kind], "PASS: %s", result.detail);
            lv_obj_set_style_text_color(s_status_labels[result.kind], COLOR_GREEN, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_cards[result.kind], COLOR_GREEN, LV_PART_MAIN);
        } else {
            lv_label_set_text_fmt(s_status_labels[result.kind],
                                  "FAIL: %s",
                                  esp_err_to_name(result.err));
            lv_obj_set_style_text_color(s_status_labels[result.kind], COLOR_RED, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_cards[result.kind], COLOR_RED, LV_PART_MAIN);
        }

        s_busy = false;
        set_buttons_enabled(true);
    }
}
