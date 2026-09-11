/*
 * Headless application runtime.
 *
 * Owns the application worker task, the request queue and the operation gate.
 * Every product operation - scan, cancel, status, device/entity enumeration and
 * control - is executed here, by one owner task, through the L2 family APIs.
 *
 * The serial console is only transport. The future GUI will submit the same
 * requests, so no business logic may live in either front end.
 *
 * Threading contract:
 *   - the worker task is the only mutator of application state;
 *   - callers block in app_runtime_submit() until their own request completes;
 *   - callbacks must never touch HA state or LVGL directly.
 */

#include "app_runtime.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_runtime";

#define APP_RUNTIME_QUEUE_DEPTH 4u
#define APP_RUNTIME_WORKER_STACK 6144u
#define APP_RUNTIME_WORKER_PRIO 5u
#define APP_RUNTIME_DEFAULT_TIMEOUT_MS 30000u

/* One queued unit of work. `result` lives in the submitter's stack frame and
 * `done` is signalled by the worker after writing it, so the submitter stays
 * blocked for exactly as long as that frame is alive. No response buffer is
 * ever copied through the queue. */
typedef struct {
    app_diag_request_t request;
    app_diag_response_t *result;
    SemaphoreHandle_t done;
} app_job_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_lock;
static app_ops_t s_ops;
static app_ops_t s_ops_mirror;
static uint32_t s_queue_drops;
static bool s_started;

/* Set by app_diag_console so the resource report can include its stack. */
static TaskHandle_t s_console_task;

void app_runtime_set_console_task(TaskHandle_t task);

/* ---------------- helpers ---------------- */

static void lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t app_runtime_version(char *out, size_t out_size)
{
    const esp_app_desc_t *desc;
    int written;

    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }

    desc = esp_app_get_description();
    if (desc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    written = snprintf(out, out_size, "%s %s", desc->project_name, desc->version);
    if (written < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)written >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

app_op_state_t app_runtime_op_state(void)
{
    app_op_state_t state;

    lock();
    state = app_ops_state(&s_ops_mirror);
    unlock();
    return state;
}

esp_err_t app_runtime_get_resources(app_runtime_resources_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    out->free_heap_bytes = (uint32_t)esp_get_free_heap_size();
    out->min_free_heap_bytes = (uint32_t)esp_get_minimum_free_heap_size();
    out->largest_free_block_bytes =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if (s_worker != NULL) {
        out->worker_stack_high_water_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(s_worker) * sizeof(StackType_t);
    }
    if (s_console_task != NULL) {
        out->console_stack_high_water_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(s_console_task) * sizeof(StackType_t);
    }

    lock();
    out->queue_drops = s_queue_drops;
    out->generation = s_ops_mirror.scan.generation;
    out->stage_completed = app_ops_stage_completed_count(&s_ops_mirror.scan);
    out->stage_total = (uint32_t)APP_STAGE_COUNT;
    out->op_state = app_ops_state_name(app_ops_state(&s_ops_mirror));
    unlock();

    return ESP_OK;
}

void app_runtime_set_console_task(TaskHandle_t task)
{
    s_console_task = task;
}

/* ---------------- device / entity enumeration ---------------- */

/*
 * These helpers read ha_core, which is documented as not thread safe and is
 * owned by one task. They are therefore only called from the worker.
 */

esp_err_t app_runtime_write_devices(char *out, size_t out_size,
                                    size_t *out_written, bool *out_truncated)
{
    size_t used = 0u;
    size_t count;
    bool truncated = false;

    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    count = ha_core_device_count();

    for (size_t i = 0u; i < count; ++i) {
        const ha_device_t *device = ha_core_device_at(i);
        char line[288];
        int written;

        if (device == NULL) {
            continue;
        }

        written = snprintf(line, sizeof(line),
                           "device id=%s name=%s manufacturer=%s model=%s entities=%u\n",
                           device->id,
                           device->name[0] != '\0' ? device->name : "-",
                           device->manufacturer[0] != '\0' ? device->manufacturer : "-",
                           device->model[0] != '\0' ? device->model : "-",
                           (unsigned)ha_core_entity_count_for_device(device->id));
        if (written < 0 || (size_t)written >= sizeof(line)) {
            truncated = true;
            continue;
        }
        if (used + (size_t)written + 1u > out_size) {
            truncated = true;
            break;
        }
        memcpy(out + used, line, (size_t)written);
        used += (size_t)written;
        out[used] = '\0';
    }

    if (count == 0u) {
        static const char none[] = "no devices\n";
        if (sizeof(none) <= out_size) {
            memcpy(out, none, sizeof(none));
            used = sizeof(none) - 1u;
        } else {
            truncated = true;
        }
    }

    if (out_written != NULL) {
        *out_written = used;
    }
    if (out_truncated != NULL) {
        *out_truncated = truncated;
    }
    return ESP_OK;
}

static size_t append_entity(char *out, size_t out_size, size_t used,
                            const ha_entity_t *entity, const char *device_id,
                            bool *truncated)
{
    const ha_state_t *state;
    char line[288];
    int written;

    state = ha_core_state_get(entity->entity_id);
    written = snprintf(line, sizeof(line),
                       "entity id=%s device=%s state=%s unit=%s available=%u\n",
                       entity->entity_id, device_id,
                       state != NULL ? state->state : "unknown",
                       entity->unit_of_measurement[0] != '\0'
                           ? entity->unit_of_measurement
                           : "-",
                       (unsigned)(entity->available ? 1u : 0u));
    if (written < 0 || (size_t)written >= sizeof(line)) {
        *truncated = true;
        return used;
    }
    if (used + (size_t)written + 1u > out_size) {
        *truncated = true;
        return used;
    }
    memcpy(out + used, line, (size_t)written);
    used += (size_t)written;
    out[used] = '\0';
    return used;
}

esp_err_t app_runtime_write_entities(const char *device_id, char *out,
                                     size_t out_size, size_t *out_written,
                                     bool *out_truncated)
{
    size_t used = 0u;
    bool truncated = false;

    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (device_id == NULL || device_id[0] == '\0') {
        size_t devices = ha_core_device_count();

        for (size_t d = 0u; d < devices; ++d) {
            const ha_device_t *device = ha_core_device_at(d);
            size_t entities;

            if (device == NULL) {
                continue;
            }
            entities = ha_core_entity_count_for_device(device->id);
            for (size_t e = 0u; e < entities; ++e) {
                const ha_entity_t *entity =
                    ha_core_entity_at_for_device(device->id, e);
                if (entity == NULL) {
                    continue;
                }
                used = append_entity(out, out_size, used, entity, device->id,
                                     &truncated);
            }
        }
    } else {
        size_t entities = ha_core_entity_count_for_device(device_id);

        for (size_t e = 0u; e < entities; ++e) {
            const ha_entity_t *entity =
                ha_core_entity_at_for_device(device_id, e);
            if (entity == NULL) {
                continue;
            }
            used = append_entity(out, out_size, used, entity, device_id,
                                 &truncated);
        }
    }

    if (out_written != NULL) {
        *out_written = used;
    }
    if (out_truncated != NULL) {
        *out_truncated = truncated;
    }
    return ESP_OK;
}

/* ---------------- request execution ---------------- */

static void publish_state(void)
{
    lock();
    s_ops_mirror = s_ops;
    unlock();
}

static app_diag_response_t make_response(const app_diag_request_t *request,
                                         app_diag_error_t error)
{
    app_diag_response_t response;

    memset(&response, 0, sizeof(response));
    response.request_id = request->request_id;
    response.command = request->command;
    response.error = error;
    response.stage = APP_DIAG_STAGE_NONE;
    response.stage_state = APP_DIAG_STAGE_STATE_IDLE;
    return response;
}

static app_diag_stage_t diag_stage_of(app_scan_stage_t stage)
{
    switch (stage) {
    case APP_STAGE_WIFI_RF:      return APP_DIAG_STAGE_WIFI_RF;
    case APP_STAGE_BLE_RF:       return APP_DIAG_STAGE_BLE_RF;
    case APP_STAGE_THREAD:       return APP_DIAG_STAGE_THREAD;
    case APP_STAGE_ZIGBEE:       return APP_DIAG_STAGE_ZIGBEE;
    case APP_STAGE_MDNS:         return APP_DIAG_STAGE_MDNS;
    case APP_STAGE_SSDP:         return APP_DIAG_STAGE_SSDP;
    case APP_STAGE_LAN_HOSTS:    return APP_DIAG_STAGE_LAN_HOSTS;
    case APP_STAGE_LAN_SERVICES: return APP_DIAG_STAGE_LAN_SERVICES;
    case APP_STAGE_ENRICHMENT:   return APP_DIAG_STAGE_ENRICHMENT;
    case APP_STAGE_MATERIALIZE:  return APP_DIAG_STAGE_MATERIALIZE;
    default:                     return APP_DIAG_STAGE_NONE;
    }
}

static app_diag_stage_state_t diag_state_of(app_stage_state_t state)
{
    switch (state) {
    case APP_STAGE_STATE_PENDING:  return APP_DIAG_STAGE_STATE_PENDING;
    case APP_STAGE_STATE_RUNNING:  return APP_DIAG_STAGE_STATE_RUNNING;
    case APP_STAGE_STATE_DONE:     return APP_DIAG_STAGE_STATE_DONE;
    case APP_STAGE_STATE_PARTIAL:  return APP_DIAG_STAGE_STATE_PARTIAL;
    case APP_STAGE_STATE_FAILED:   return APP_DIAG_STAGE_STATE_FAILED;
    case APP_STAGE_STATE_SKIPPED:  return APP_DIAG_STAGE_STATE_SKIPPED;
    case APP_STAGE_STATE_CANCELED: return APP_DIAG_STAGE_STATE_SKIPPED;
    default:                       return APP_DIAG_STAGE_STATE_IDLE;
    }
}

/*
 * Run one scan stage.
 *
 * B0 wires the lifecycle only; the RF/LAN stage bodies arrive with B1..B3.
 * A stage whose body is not implemented is recorded as SKIPPED and reported as
 * NOT_IMPLEMENTED, so an unfinished scan can never be mistaken for a completed
 * one.
 */
static app_diag_error_t run_scan_stage(app_scan_stage_t stage)
{
    if (app_ops_stage_begin(&s_ops, stage) != APP_OPS_OK) {
        return APP_DIAG_ERR_CANCELED;
    }

    (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_SKIPPED);
    return APP_DIAG_ERR_NOT_IMPLEMENTED;
}

static app_diag_response_t execute_scan(const app_diag_request_t *request)
{
    app_diag_response_t response = make_response(request, APP_DIAG_OK);
    app_scan_status_t report;
    app_diag_error_t first_error = APP_DIAG_OK;
    uint32_t budget_ms;
    uint32_t start_ms;
    int last_stage = -1;

    if (app_ops_scan_begin(&s_ops, request->request_id, &report) != APP_OPS_OK) {
        response.error = APP_DIAG_ERR_BUSY;
        response.detail = "operation_active";
        return response;
    }
    publish_state();

    budget_ms = request->timeout_ms != 0u ? request->timeout_ms
                                          : APP_RUNTIME_DEFAULT_TIMEOUT_MS;
    start_ms = now_ms();

    for (int i = 0; i < (int)APP_STAGE_COUNT; ++i) {
        app_diag_error_t stage_error;

        if (app_ops_scan_is_canceled(&s_ops)) {
            break;
        }
        /* The budget bounds the whole scan; the first stage always runs so a
         * scan cannot report success without doing anything. */
        if (i > 0 && (uint32_t)(now_ms() - start_ms) >= budget_ms) {
            response.detail = "scan_budget_exceeded";
            break;
        }

        stage_error = run_scan_stage((app_scan_stage_t)i);
        last_stage = i;
        if (stage_error != APP_DIAG_OK && first_error == APP_DIAG_OK) {
            first_error = stage_error;
        }
        publish_state();
    }

    if (app_ops_scan_finish(&s_ops, &report) != APP_OPS_OK) {
        response.error = APP_DIAG_ERR_INTERNAL;
        publish_state();
        return response;
    }
    publish_state();

    response.error = first_error;
    response.stage = last_stage >= 0 ? diag_stage_of((app_scan_stage_t)last_stage)
                                     : APP_DIAG_STAGE_NONE;
    response.stage_state = last_stage >= 0
                               ? diag_state_of(report.states[last_stage])
                               : APP_DIAG_STAGE_STATE_IDLE;

    if (report.canceled) {
        response.error = APP_DIAG_ERR_CANCELED;
        response.stage_state = APP_DIAG_STAGE_STATE_SKIPPED;
    }

    response.partial = report.partial || (first_error != APP_DIAG_OK);
    response.truncated = report.truncated;
    return response;
}

static app_diag_response_t execute_status(const app_diag_request_t *request)
{
    app_diag_response_t response = make_response(request, APP_DIAG_OK);
    app_scan_status_t scan;

    lock();
    if (app_ops_scan_snapshot(&s_ops_mirror, &scan)) {
        response.stage = diag_stage_of(scan.current);
        response.stage_state = diag_state_of(scan.states[scan.current]);
        response.partial = scan.partial;
        response.truncated = scan.truncated;
    }
    unlock();

    return response;
}

static app_diag_response_t execute_request(const app_diag_request_t *request)
{
    app_diag_response_t response;

    switch (request->command) {
    case APP_DIAG_CMD_PING:
    case APP_DIAG_CMD_VERSION:
    case APP_DIAG_CMD_RESOURCES:
    case APP_DIAG_CMD_HELP:
        return make_response(request, APP_DIAG_OK);

    case APP_DIAG_CMD_STATUS:
        return execute_status(request);

    case APP_DIAG_CMD_SCAN:
        return execute_scan(request);

    case APP_DIAG_CMD_CANCEL:
        response = make_response(request, APP_DIAG_OK);
        if (app_ops_scan_cancel(&s_ops) != APP_OPS_OK) {
            response.error = APP_DIAG_ERR_NOT_ACTIVE;
            response.detail = "no_active_scan";
        }
        publish_state();
        return response;

    case APP_DIAG_CMD_DEVICES:
    case APP_DIAG_CMD_ENTITIES:
        /* Enumeration is wired to ha_core in the device/state task (B3). */
        return make_response(request, APP_DIAG_ERR_NOT_IMPLEMENTED);

    case APP_DIAG_CMD_CONTROL:
        /* Control never silently succeeds: an unimplemented backend reports
         * NOT_IMPLEMENTED so no caller can mistake it for a state change. */
        return make_response(request, APP_DIAG_ERR_NOT_IMPLEMENTED);

    default:
        return make_response(request, APP_DIAG_ERR_UNKNOWN_COMMAND);
    }
}

/* ---------------- worker ---------------- */

static void app_worker_task(void *arg)
{
    app_job_t job;

    (void)arg;

    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* The result buffer belongs to the still-blocked submitter. */
        if (job.result != NULL) {
            *job.result = execute_request(&job.request);
        }

        if (job.done != NULL) {
            (void)xSemaphoreGive(job.done);
        }
    }
}

esp_err_t app_runtime_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(APP_RUNTIME_QUEUE_DEPTH, sizeof(app_job_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    app_ops_init(&s_ops);
    app_ops_init(&s_ops_mirror);

    if (xTaskCreate(app_worker_task, "app_worker", APP_RUNTIME_WORKER_STACK, NULL,
                    APP_RUNTIME_WORKER_PRIO, &s_worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "headless runtime started");
    return ESP_OK;
}

esp_err_t app_runtime_submit(const app_diag_request_t *request,
                             app_diag_response_t *out_response,
                             uint32_t timeout_ms)
{
    app_job_t job;
    SemaphoreHandle_t done;

    if (!s_started || request == NULL || out_response == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * `timeout_ms` is intentionally not used to abandon the request.
     *
     * The worker writes the response into this stack frame and then signals
     * `done`. Abandoning the wait would leave the worker holding a pointer to
     * a dead frame, so this function always waits for ownership of the result.
     * Every request is bounded internally instead (each scan stage and backend
     * call has its own deadline), which is what actually guarantees the worker
     * cannot block forever.
     */
    (void)timeout_ms;

    done = xSemaphoreCreateBinary();
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&job, 0, sizeof(job));
    job.request = *request;
    job.result = out_response;
    job.done = done;

    if (xQueueSend(s_queue, &job, 0) != pdTRUE) {
        vSemaphoreDelete(done);
        lock();
        s_queue_drops++;
        unlock();
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(done, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(done);
    return ESP_OK;
}

esp_err_t app_runtime_request_scan(uint32_t request_id,
                                   app_diag_scan_kind_t kind,
                                   uint32_t timeout_ms,
                                   app_diag_response_t *out_response)
{
    app_diag_request_t request;

    memset(&request, 0, sizeof(request));
    request.command = APP_DIAG_CMD_SCAN;
    request.scan_kind = kind;
    request.request_id = request_id;
    request.timeout_ms = timeout_ms;
    return app_runtime_submit(&request, out_response, timeout_ms);
}

esp_err_t app_runtime_request_cancel(uint32_t target_request_id,
                                     app_diag_response_t *out_response)
{
    app_diag_request_t request;

    memset(&request, 0, sizeof(request));
    request.command = APP_DIAG_CMD_CANCEL;
    request.request_id = target_request_id;
    return app_runtime_submit(&request, out_response, 0u);
}

esp_err_t app_runtime_request_status(uint32_t request_id,
                                     app_diag_response_t *out_response)
{
    app_diag_request_t request;

    memset(&request, 0, sizeof(request));
    request.command = APP_DIAG_CMD_STATUS;
    request.request_id = request_id;
    return app_runtime_submit(&request, out_response, 0u);
}

esp_err_t app_runtime_request_control(uint32_t request_id,
                                      const char *entity_id,
                                      const char *action,
                                      const char *value,
                                      app_diag_response_t *out_response)
{
    app_diag_request_t request;

    if (entity_id == NULL || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&request, 0, sizeof(request));
    request.command = APP_DIAG_CMD_CONTROL;
    request.request_id = request_id;
    (void)strlcpy(request.target, entity_id, sizeof(request.target));
    (void)strlcpy(request.action, action, sizeof(request.action));
    if (value != NULL) {
        (void)strlcpy(request.value, value, sizeof(request.value));
    }
    return app_runtime_submit(&request, out_response, 0u);
}
