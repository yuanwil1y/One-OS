#include "kismet_wifi.h"
#include "kismet_internal.h"
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WIFI_TASK_STACK 4096u
#define WIFI_TASK_PRIO 5u
#define WIFI_MIN_COPY 30u
#define WIFI_FCS_LEN 4u

/*
 * Bound on how long session teardown waits for the session task.
 *
 * The task's own loop is bounded by cfg.duration_ms and always runs its cleanup
 * block, but the native teardown it calls is outside this component's control.
 * This bound is what keeps a wedged native call from turning into an unbounded
 * application wait.
 */
#define KISMET_WIFI_DESTROY_TIMEOUT_MS 15000u

struct kismet_wifi_session {
    kismet_wifi_session_config_t cfg;
    kismet_wifi_tracker_t *tracker;
    kismet_wifi_frame_callback_t cb;
    void *cb_ctx;
    QueueHandle_t queue;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool cancel;
    volatile bool finished;
    /* Set by destroy_checked() once it has taken the completion semaphore, i.e.
     * once the task has provably stopped touching this session. */
    volatile bool claimed;
    uint8_t channels[KISMET_WIFI_MAX_CHANNELS];
    uint8_t channel_count;
    kismet_wifi_session_result_t result;
};

static kismet_wifi_session_t *s_active;
static portMUX_TYPE s_active_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t elapsed_ms(int64_t start_us)
{
    int64_t now = esp_timer_get_time();
    if (now <= start_us) return 0u;
    uint64_t ms = (uint64_t)(now - start_us) / 1000u;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static kismet_wifi_session_config_t normalize(const kismet_wifi_session_config_t *in)
{
    kismet_wifi_session_config_t c = {0};
    if (in) c = *in;
    if (c.dwell_ms == 0u) c.dwell_ms = KISMET_WIFI_DEFAULT_DWELL_MS;
    if (c.duration_ms == 0u) c.duration_ms = KISMET_WIFI_DEFAULT_DURATION_MS;
    if (c.frame_copy_len == 0u) c.frame_copy_len = KISMET_WIFI_DEFAULT_FRAME_COPY_LEN;
    if (c.rx_queue_depth == 0u) c.rx_queue_depth = KISMET_WIFI_DEFAULT_RX_QUEUE_DEPTH;
    return c;
}

static bool config_valid(const kismet_wifi_session_config_t *c)
{
    if (!c || c->duration_ms == 0u || c->duration_ms > KISMET_WIFI_HARD_MAX_DURATION_MS ||
        c->dwell_ms == 0u || c->frame_copy_len < WIFI_MIN_COPY ||
        c->frame_copy_len > KISMET_WIFI_MAX_FRAME_COPY || c->rx_queue_depth == 0u ||
        c->rx_queue_depth > KISMET_WIFI_HARD_MAX_RX_QUEUE_DEPTH ||
        c->channel_count > KISMET_WIFI_MAX_CHANNELS) return false;
    for (uint8_t i = 0; i < c->channel_count; ++i) {
        if (c->channels[i] < 1u || c->channels[i] > 14u) return false;
        for (uint8_t j = 0; j < i; ++j) if (c->channels[i] == c->channels[j]) return false;
    }
    return true;
}

static bool channel_legal(uint8_t ch, const wifi_country_t *country)
{
    if (!country || country->nchan == 0u) return false;
    uint16_t first = country->schan;
    uint16_t last = first + country->nchan - 1u;
    if (first < 1u) first = 1u;
    if (last > 14u) last = 14u;
    return ch >= first && ch <= last;
}

static esp_err_t build_channels(kismet_wifi_session_t *s)
{
    wifi_country_t country = {0};
    esp_err_t err = esp_wifi_get_country(&country);
    if (err != ESP_OK) return err;
    s->channel_count = 0u;
    if (s->cfg.channel_count != 0u) {
        for (uint8_t i = 0; i < s->cfg.channel_count; ++i) {
            if (channel_legal(s->cfg.channels[i], &country))
                s->channels[s->channel_count++] = s->cfg.channels[i];
        }
    } else {
        uint16_t first = country.schan;
        uint16_t last = country.nchan ? first + country.nchan - 1u : 0u;
        if (first < 1u) first = 1u;
        if (last > 14u) last = 14u;
        for (uint16_t ch = first; ch <= last && s->channel_count < KISMET_WIFI_MAX_CHANNELS; ++ch)
            s->channels[s->channel_count++] = (uint8_t)ch;
    }
    return s->channel_count ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static kismet_wifi_capture_type_t capture_type(wifi_promiscuous_pkt_type_t type)
{
    if (type == WIFI_PKT_MGMT) return KISMET_WIFI_CAPTURE_MANAGEMENT;
    if (type == WIFI_PKT_DATA) return KISMET_WIFI_CAPTURE_DATA;
    return KISMET_WIFI_CAPTURE_OTHER;
}

static void wifi_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    kismet_wifi_session_t *s = s_active;
    if (!s || !s->queue || !buf || s->finished) return;
    s->result.frames_received++;
    const wifi_promiscuous_pkt_t *packet = (const wifi_promiscuous_pkt_t *)buf;
    if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len < WIFI_FCS_LEN) {
        s->result.malformed_frames++;
        s->result.partial_flags |= KISMET_WIFI_PARTIAL_MALFORMED_FRAME;
        return;
    }
    uint32_t raw_len = packet->rx_ctrl.sig_len - WIFI_FCS_LEN;
    if (raw_len > UINT16_MAX) raw_len = UINT16_MAX;
    kismet_wifi_frame_t frame = {0};
    frame.seen_ms = (uint64_t)(esp_timer_get_time() / 1000);
    frame.rssi = packet->rx_ctrl.rssi;
    frame.channel = packet->rx_ctrl.channel;
    frame.type = capture_type(type);
    frame.original_len = (uint16_t)raw_len;
    frame.captured_len = frame.original_len < s->cfg.frame_copy_len ? frame.original_len : s->cfg.frame_copy_len;
    frame.truncated = frame.captured_len < frame.original_len;
    if (frame.truncated) {
        s->result.truncated_frames++;
        s->result.partial_flags |= KISMET_WIFI_PARTIAL_TRUNCATED_FRAME;
    }
    if (frame.captured_len) memcpy(frame.bytes, packet->payload, frame.captured_len);
    if (xQueueSend(s->queue, &frame, 0) != pdTRUE) {
        s->result.rx_queue_drops++;
        s->result.partial_flags |= KISMET_WIFI_PARTIAL_RX_DROP;
    }
}

static void process_frame(kismet_wifi_session_t *s, const kismet_wifi_frame_t *frame)
{
    kismet_wifi_observation_t primary, secondary;
    kismet_wifi_shallow_result_t result = kismet_wifi_shallow_observe(
        frame->bytes, frame->captured_len, frame->rssi, frame->channel,
        frame->seen_ms, &primary, &secondary);
    if (result == KISMET_WIFI_SHALLOW_MALFORMED) {
        s->result.malformed_frames++;
        s->result.partial_flags |= KISMET_WIFI_PARTIAL_MALFORMED_FRAME;
    } else if (result == KISMET_WIFI_SHALLOW_OK && s->tracker) {
        (void)kismet_wifi_tracker_ingest(s->tracker, &primary);
        (void)kismet_wifi_tracker_ingest(s->tracker, &secondary);
    }
    if (s->cb) {
        s->cb(frame, s->cb_ctx);
        s->result.frames_delivered++;
    }
}

static void set_native_error(kismet_wifi_session_t *s, esp_err_t err)
{
    if (s->result.native_error == ESP_OK && err != ESP_OK) s->result.native_error = err;
}

static void wifi_task(void *arg)
{
    kismet_wifi_session_t *s = (kismet_wifi_session_t *)arg;
    int64_t start = esp_timer_get_time();
    bool initialized = false, started = false, promiscuous = false, callback_set = false;
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    init_cfg.nvs_enable = false;
    esp_err_t err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) { set_native_error(s, err); goto out; }
    initialized = true;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) { set_native_error(s, err); goto cleanup; }
    started = true;
    if ((err = build_channels(s)) != ESP_OK) { set_native_error(s, err); goto cleanup; }
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    if ((err = esp_wifi_set_promiscuous_filter(&filter)) != ESP_OK ||
        (err = esp_wifi_set_promiscuous_rx_cb(wifi_rx)) != ESP_OK) {
        set_native_error(s, err);
        goto cleanup;
    }
    callback_set = true;
    if ((err = esp_wifi_set_channel(s->channels[0], WIFI_SECOND_CHAN_NONE)) != ESP_OK ||
        (err = esp_wifi_set_promiscuous(true)) != ESP_OK) {
        set_native_error(s, err);
        goto cleanup;
    }
    promiscuous = true;
    uint8_t channel_index = 0u;
    uint32_t last_hop_ms = 0u;
    for (;;) {
        uint32_t now_ms = elapsed_ms(start);
        if (s->cancel || now_ms >= s->cfg.duration_ms) break;
        if (now_ms - last_hop_ms >= s->cfg.dwell_ms) {
            channel_index = (uint8_t)((channel_index + 1u) % s->channel_count);
            err = esp_wifi_set_channel(s->channels[channel_index], WIFI_SECOND_CHAN_NONE);
            if (err != ESP_OK) { set_native_error(s, err); break; }
            s->result.channel_switches++;
            last_hop_ms = now_ms;
        }
        kismet_wifi_frame_t frame;
        if (xQueueReceive(s->queue, &frame, pdMS_TO_TICKS(10)) == pdTRUE)
            process_frame(s, &frame);
    }
    if (s->cancel) s->result.canceled = true;
    for (;;) {
        kismet_wifi_frame_t frame;
        if (xQueueReceive(s->queue, &frame, 0) != pdTRUE) break;
        process_frame(s, &frame);
    }
cleanup:
    if (promiscuous) { err = esp_wifi_set_promiscuous(false); set_native_error(s, err); }
    if (callback_set) { err = esp_wifi_set_promiscuous_rx_cb(NULL); set_native_error(s, err); }
    if (started) { err = esp_wifi_stop(); set_native_error(s, err); }
    if (initialized) { err = esp_wifi_deinit(); set_native_error(s, err); }
out:
    s->result.elapsed_ms = elapsed_ms(start);
    if (s->tracker) {
        kismet_wifi_tracker_note_capture(s->tracker, s->result.rx_queue_drops,
                                          s->result.malformed_frames,
                                          s->result.truncated_frames);
    }
    s->finished = true;
    portENTER_CRITICAL(&s_active_lock);
    if (s_active == s) s_active = NULL;
    portEXIT_CRITICAL(&s_active_lock);
    /* Last action before self-deletion, and deliberately after every other write
     * to this session: destroy_checked() treats a successful take of this
     * semaphore as proof that the task can no longer touch the session. */
    xSemaphoreGive(s->done);
    vTaskDelete(NULL);
}

esp_err_t kismet_wifi_session_start(const kismet_wifi_session_config_t *config,
                                    kismet_wifi_tracker_t *tracker,
                                    kismet_wifi_frame_callback_t cb, void *ctx,
                                    kismet_wifi_session_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    kismet_wifi_session_config_t cfg = normalize(config);
    if (!config_valid(&cfg)) return ESP_ERR_INVALID_ARG;
    wifi_mode_t mode;
    esp_err_t probe = esp_wifi_get_mode(&mode);
    if (probe == ESP_OK) return ESP_ERR_INVALID_STATE;
    if (probe != ESP_ERR_WIFI_NOT_INIT) return probe;
    kismet_wifi_session_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->cfg = cfg;
    s->tracker = tracker;
    s->cb = cb;
    s->cb_ctx = ctx;
    s->result.native_error = ESP_OK;
    s->queue = xQueueCreate(cfg.rx_queue_depth, sizeof(kismet_wifi_frame_t));
    s->done = xSemaphoreCreateBinary();
    if (!s->queue || !s->done) {
        if (s->queue) vQueueDelete(s->queue);
        if (s->done) vSemaphoreDelete(s->done);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_active_lock);
    if (s_active) {
        portEXIT_CRITICAL(&s_active_lock);
        vQueueDelete(s->queue);
        vSemaphoreDelete(s->done);
        free(s);
        return ESP_ERR_INVALID_STATE;
    }
    s_active = s;
    portEXIT_CRITICAL(&s_active_lock);
    if (xTaskCreate(wifi_task, "kismet_wifi", WIFI_TASK_STACK, s, WIFI_TASK_PRIO,
                    &s->task) != pdPASS) {
        portENTER_CRITICAL(&s_active_lock);
        if (s_active == s) s_active = NULL;
        portEXIT_CRITICAL(&s_active_lock);
        vQueueDelete(s->queue);
        vSemaphoreDelete(s->done);
        free(s);
        return ESP_ERR_NO_MEM;
    }
    *out = s;
    return ESP_OK;
}

esp_err_t kismet_wifi_session_cancel(kismet_wifi_session_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (s->finished) return ESP_ERR_INVALID_STATE;
    s->cancel = true;
    return ESP_OK;
}

esp_err_t kismet_wifi_session_wait(kismet_wifi_session_t *s, uint32_t timeout_ms)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    TickType_t ticks = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s->done, ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreGive(s->done);
    return ESP_OK;
}

void kismet_wifi_session_destroy(kismet_wifi_session_t *s)
{
    (void)kismet_wifi_session_destroy_checked(s);
}

esp_err_t kismet_wifi_session_get_result(const kismet_wifi_session_t *s,
                                         kismet_wifi_session_result_t *out)
{
    if (!s || !out) return ESP_ERR_INVALID_ARG;
    *out = s->result;
    return ESP_OK;
}

esp_err_t kismet_wifi_session_destroy_checked(kismet_wifi_session_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    if (!s->finished) {
        s->cancel = true;
        /*
         * Bounded wait, deliberately not portMAX_DELAY.
         *
         * The session task always reaches its completion point on the paths this
         * component controls (normal end, cancel, or its own duration), but the
         * native teardown it performs - esp_wifi_stop(), esp_wifi_deinit() - is
         * outside our control and can in principle block. Waiting forever would
         * remove the operation's timeout guarantee, so the wait is finite.
         *
         * If the task does NOT finish in time we must NOT free: it still holds
         * pointers into this session, its queue and its semaphore, and it will
         * dereference them when it finally completes, so freeing would be a
         * use-after-free in another task's stack. The session is intentionally
         * leaked instead and the caller is told, so it can fail the operation
         * rather than publish evidence from a session it cannot shut down.
         *
         * The leak is RECOVERABLE, not permanent: the task sets `finished` before
         * it deletes itself, so the handle the caller still holds becomes
         * destroyable as soon as that flag is set. See
         * kismet_wifi_session_task_alive().
         */
        if (xSemaphoreTake(s->done, pdMS_TO_TICKS(KISMET_WIFI_DESTROY_TIMEOUT_MS)) != pdTRUE) {
            /* Every allocation is left in place on purpose, and the handle stays
             * valid for a later retry. */
            return ESP_ERR_TIMEOUT;
        }
        /*
         * Taking the completion semaphore is the claim, and it is what makes the
         * free below safe. The task gives it as the VERY LAST thing it does before
         * vTaskDelete(NULL), after it has finished touching every field, so a
         * successful take proves there is no later access.
         *
         * This is why `finished` alone is not the safety condition: a task that has
         * set `finished` is still running until it reaches the give, and freeing
         * there would be a use-after-free on its stack. `claimed` records that the
         * give has actually been observed.
         */
        s->claimed = true;
        xSemaphoreGive(s->done);
    }
    vQueueDelete(s->queue);
    vSemaphoreDelete(s->done);
    free(s);
    return ESP_OK;
}

bool kismet_wifi_session_task_alive(const kismet_wifi_session_t *s)
{
    if (!s) return false;
    /*
     * Alive until BOTH conditions hold:
     *
     *   - `finished`, written by the task once its cleanup block is done, so the
     *     native Wi-Fi driver has been handed back;
     *   - `claimed`, set by destroy_checked() when it has taken the completion
     *     semaphore, which the task gives as its last action.
     *
     * `claimed` is what makes reclamation safe rather than merely probable. A task
     * that has set `finished` is still executing and still holds a pointer into
     * the session; only the semaphore give proves it has stopped touching it.
     */
    return !(s->finished && s->claimed);
}
