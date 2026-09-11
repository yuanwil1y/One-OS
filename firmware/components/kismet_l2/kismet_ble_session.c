#include "kismet_ble.h"
#include "kismet_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define BLE_TASK_STACK 4096u
#define BLE_TASK_PRIO 5u
#define BLE_SYNC_TIMEOUT_MS 5000u
#define BLE_STOP_GRACE_MS 2000u

/*
 * Bound on how long session teardown waits for the session task.
 *
 * Generous compared with the bounded scan loop, because nimble_port_stop() has to
 * let the host task unwind. This bound is what keeps a wedged native call from
 * becoming an unbounded application wait.
 */
#define KISMET_BLE_DESTROY_TIMEOUT_MS 15000u

struct kismet_ble_session {
    kismet_ble_session_config_t cfg;
    kismet_ble_tracker_t *tracker;
    kismet_ble_report_callback_t cb;
    void *cb_ctx;
    QueueHandle_t queue;
    SemaphoreHandle_t sync_sem;
    SemaphoreHandle_t scan_done;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool cancel;
    volatile bool finished;
    volatile bool sync_ok;
    uint8_t own_addr_type;
    kismet_ble_session_result_t result;
};

static kismet_ble_session_t *s_active;
static portMUX_TYPE s_active_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t elapsed_ms(int64_t start_us)
{
    int64_t now = esp_timer_get_time();
    if (now <= start_us) return 0u;
    uint64_t ms = (uint64_t)(now - start_us) / 1000u;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static kismet_ble_session_config_t normalize(const kismet_ble_session_config_t *in)
{
    kismet_ble_session_config_t c = {0};
    if (in) c = *in;
    if (!c.duration_ms) c.duration_ms = KISMET_BLE_DEFAULT_DURATION_MS;
    if (!c.report_copy_len) c.report_copy_len = KISMET_BLE_DEFAULT_REPORT_COPY_LEN;
    if (!c.report_queue_depth) c.report_queue_depth = KISMET_BLE_DEFAULT_REPORT_QUEUE_DEPTH;
    return c;
}

static bool config_valid(const kismet_ble_session_config_t *c)
{
    return c && c->duration_ms && c->duration_ms <= KISMET_BLE_HARD_MAX_DURATION_MS &&
           c->report_copy_len && c->report_copy_len <= KISMET_BLE_MAX_ADV_COPY &&
           c->report_queue_depth && c->report_queue_depth <= KISMET_BLE_HARD_MAX_REPORT_QUEUE_DEPTH;
}

static void set_native_error(kismet_ble_session_t *s, esp_err_t err)
{
    if (s->result.native_error == ESP_OK && err != ESP_OK) s->result.native_error = err;
}

static void ble_reset(int reason)
{
    (void)reason;
    kismet_ble_session_t *s = s_active;
    if (s) s->sync_ok = false;
}

static void ble_sync(void)
{
    kismet_ble_session_t *s = s_active;
    if (!s) return;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s->own_addr_type);
    s->sync_ok = rc == 0;
    if (s->sync_sem) xSemaphoreGive(s->sync_sem);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void queue_report(kismet_ble_session_t *s, const uint8_t addr[6],
                         uint8_t addr_type, int8_t rssi, bool connectable,
                         const uint8_t *data, uint16_t len)
{
    if (!s || !s->queue) return;
    s->result.reports_received++;
    kismet_ble_report_t report = {0};
    report.seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    memcpy(report.address, addr, 6);
    report.address_type = addr_type;
    report.rssi = rssi;
    report.connectable = connectable;
    report.original_len = len;
    report.captured_len = len < s->cfg.report_copy_len ? len : s->cfg.report_copy_len;
    report.truncated = report.captured_len < report.original_len;
    if (report.truncated) {
        s->result.truncated_reports++;
        s->result.partial_flags |= KISMET_BLE_PARTIAL_TRUNCATED_REPORT;
    }
    if (report.captured_len && data) memcpy(report.bytes, data, report.captured_len);
    if (xQueueSend(s->queue, &report, 0) != pdTRUE) {
        s->result.report_queue_drops++;
        s->result.partial_flags |= KISMET_BLE_PARTIAL_REPORT_DROP;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    kismet_ble_session_t *s = (kismet_ble_session_t *)arg;
    if (!s || !event) return 0;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *d = &event->disc;
        bool connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        queue_report(s, d->addr.val, d->addr.type, d->rssi, connectable,
                     d->data, d->length_data);
    }
#if MYNEWT_VAL(BLE_EXT_ADV)
    else if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
        bool connectable = (d->props & BLE_HCI_ADV_CONN_MASK) != 0;
        queue_report(s, d->addr.val, d->addr.type, d->rssi, connectable,
                     d->data, d->length_data);
    }
#endif
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s->scan_done) xSemaphoreGive(s->scan_done);
    }
    return 0;
}

static void process_report(kismet_ble_session_t *s, const kismet_ble_report_t *report)
{
    if (s->tracker) {
        kismet_ble_observation_t observation = {0};
        observation.seen_ms = report->seen_ms;
        memcpy(observation.address, report->address, 6);
        observation.address_type = report->address_type;
        observation.rssi = report->rssi;
        observation.connectable = report->connectable;
        (void)kismet_ble_tracker_ingest(s->tracker, &observation);
    }
    if (s->cb) {
        s->cb(report, s->cb_ctx);
        s->result.reports_delivered++;
    }
}

static void ble_task(void *arg)
{
    kismet_ble_session_t *s = (kismet_ble_session_t *)arg;
    int64_t start = esp_timer_get_time();
    bool nimble_initialized = false;
    bool scan_started = false;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        set_native_error(s, err);
        goto out;
    }
    err = nimble_port_init();
    if (err != ESP_OK) {
        set_native_error(s, err);
        goto out;
    }
    nimble_initialized = true;
    ble_hs_cfg.reset_cb = ble_reset;
    ble_hs_cfg.sync_cb = ble_sync;
    nimble_port_freertos_init(ble_host_task);
    if (xSemaphoreTake(s->sync_sem, pdMS_TO_TICKS(BLE_SYNC_TIMEOUT_MS)) != pdTRUE || !s->sync_ok) {
        set_native_error(s, ESP_ERR_TIMEOUT);
        goto stop;
    }

    int rc;
#if MYNEWT_VAL(BLE_EXT_ADV)
    struct ble_gap_ext_disc_params params = {0};
    params.passive = 1;
    uint32_t units = (s->cfg.duration_ms + 9u) / 10u;
    if (units > UINT16_MAX) units = UINT16_MAX;
    rc = ble_gap_ext_disc(s->own_addr_type, (uint16_t)units, 0, 0, 0, 0,
                          &params, &params, gap_event, s);
#else
    struct ble_gap_disc_params params = {0};
    params.passive = 1;
    params.filter_duplicates = 0;
    params.filter_policy = 0;
    params.limited = 0;
    rc = ble_gap_disc(s->own_addr_type, (int32_t)s->cfg.duration_ms,
                      &params, gap_event, s);
#endif
    if (rc != 0) {
        set_native_error(s, ESP_FAIL);
        goto stop;
    }
    scan_started = true;
    bool cancel_sent = false;
    for (;;) {
        if (s->cancel && !cancel_sent) {
            (void)ble_gap_disc_cancel();
            cancel_sent = true;
            s->result.canceled = true;
        }
        if (xSemaphoreTake(s->scan_done, 0) == pdTRUE) break;
        kismet_ble_report_t report;
        if (xQueueReceive(s->queue, &report, pdMS_TO_TICKS(10)) == pdTRUE) {
            process_report(s, &report);
        }
        if (elapsed_ms(start) > s->cfg.duration_ms + BLE_STOP_GRACE_MS) {
            set_native_error(s, ESP_ERR_TIMEOUT);
            if (!cancel_sent) (void)ble_gap_disc_cancel();
            break;
        }
    }
    for (;;) {
        kismet_ble_report_t report;
        if (xQueueReceive(s->queue, &report, 0) != pdTRUE) break;
        process_report(s, &report);
    }
    scan_started = false;

stop:
    if (scan_started) (void)ble_gap_disc_cancel();
    if (nimble_initialized) {
        rc = nimble_port_stop();
        if (rc != 0) {
            set_native_error(s, ESP_FAIL);
        } else {
            err = nimble_port_deinit();
            set_native_error(s, err);
        }
    }
out:
    s->result.elapsed_ms = elapsed_ms(start);
    if (s->tracker) {
        kismet_ble_tracker_note_capture(s->tracker, s->result.report_queue_drops,
                                        s->result.truncated_reports);
    }
    s->finished = true;
    portENTER_CRITICAL(&s_active_lock);
    if (s_active == s) s_active = NULL;
    portEXIT_CRITICAL(&s_active_lock);
    xSemaphoreGive(s->done);
    vTaskDelete(NULL);
}

static void free_session(kismet_ble_session_t *s)
{
    if (!s) return;
    if (s->queue) vQueueDelete(s->queue);
    if (s->sync_sem) vSemaphoreDelete(s->sync_sem);
    if (s->scan_done) vSemaphoreDelete(s->scan_done);
    if (s->done) vSemaphoreDelete(s->done);
    free(s);
}

esp_err_t kismet_ble_session_start(const kismet_ble_session_config_t *config,
                                   kismet_ble_tracker_t *tracker,
                                   kismet_ble_report_callback_t cb, void *ctx,
                                   kismet_ble_session_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    kismet_ble_session_config_t cfg = normalize(config);
    if (!config_valid(&cfg)) return ESP_ERR_INVALID_ARG;
    kismet_ble_session_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->cfg = cfg;
    s->tracker = tracker;
    s->cb = cb;
    s->cb_ctx = ctx;
    s->result.native_error = ESP_OK;
    s->queue = xQueueCreate(cfg.report_queue_depth, sizeof(kismet_ble_report_t));
    s->sync_sem = xSemaphoreCreateBinary();
    s->scan_done = xSemaphoreCreateBinary();
    s->done = xSemaphoreCreateBinary();
    if (!s->queue || !s->sync_sem || !s->scan_done || !s->done) {
        free_session(s);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_active_lock);
    if (s_active) {
        portEXIT_CRITICAL(&s_active_lock);
        free_session(s);
        return ESP_ERR_INVALID_STATE;
    }
    s_active = s;
    portEXIT_CRITICAL(&s_active_lock);
    if (xTaskCreate(ble_task, "kismet_ble", BLE_TASK_STACK, s, BLE_TASK_PRIO,
                    &s->task) != pdPASS) {
        portENTER_CRITICAL(&s_active_lock);
        if (s_active == s) s_active = NULL;
        portEXIT_CRITICAL(&s_active_lock);
        free_session(s);
        return ESP_ERR_NO_MEM;
    }
    *out = s;
    return ESP_OK;
}

esp_err_t kismet_ble_session_cancel(kismet_ble_session_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (s->finished) return ESP_ERR_INVALID_STATE;
    s->cancel = true;
    return ESP_OK;
}

esp_err_t kismet_ble_session_wait(kismet_ble_session_t *s, uint32_t timeout_ms)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s->done, ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreGive(s->done);
    return ESP_OK;
}

esp_err_t kismet_ble_session_get_result(const kismet_ble_session_t *s,
                                        kismet_ble_session_result_t *out)
{
    if (!s || !out) return ESP_ERR_INVALID_ARG;
    *out = s->result;
    return ESP_OK;
}

void kismet_ble_session_destroy(kismet_ble_session_t *s)
{
    (void)kismet_ble_session_destroy_checked(s);
}

esp_err_t kismet_ble_session_destroy_checked(kismet_ble_session_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (!s->finished) {
        s->cancel = true;
        /*
         * Bounded wait, deliberately not portMAX_DELAY.
         *
         * The task's own loop is bounded (scan duration plus a grace window) and
         * always reaches its cleanup block, but the native teardown it runs -
         * nimble_port_stop() waits for the host task, then nimble_port_deinit() -
         * is outside this component's control and can in principle block. Waiting
         * forever would remove the operation's timeout guarantee.
         *
         * If the task does not finish in time we must NOT free: it still holds
         * pointers into this session, its queue and its semaphores, so the session
         * is intentionally leaked and the caller is told so it can fail the
         * operation instead of publishing evidence from a session it could not
         * shut down.
         */
        if (xSemaphoreTake(s->done, pdMS_TO_TICKS(KISMET_BLE_DESTROY_TIMEOUT_MS)) != pdTRUE) {
            /* Every allocation is left in place on purpose. */
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s->done);
    }
    free_session(s);
    return ESP_OK;
}
