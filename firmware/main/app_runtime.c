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
#include "app_str.h"

#include <stdio.h>
#include <string.h>

#include "app_device.h"
#include "app_db_import.h"
#include "app_device_db.h"
#include "app_device_db_sd.h"
#include "app_http_portal.h"
#include "app_provision.h"
#include "app_scan.h"
#include "app_scan_native.h"
#include "app_wifi.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "app_runtime";

#define APP_RUNTIME_QUEUE_DEPTH 4u
#define APP_RUNTIME_WORKER_STACK 8192u
#define APP_RUNTIME_WORKER_PRIO 5u
/* Whole-scan budget. Individual stages have their own shorter timeouts; this is
 * the outer bound that keeps one request from occupying the worker forever. */
#define APP_RUNTIME_DEFAULT_SCAN_BUDGET_MS 45000u

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

/*
 * Scan working state. Only the worker task touches these, and the worker is the
 * only task that runs a scan, so no lock is needed. The evidence store is large
 * enough to justify keeping it off the worker's stack.
 */
static app_scan_evidence_t s_evidence;
static app_scan_native_stats_t s_scan_stats;
static app_scan_inputs_t s_scan_inputs;

/*
 * The recognition source.
 *
 * A reference, not an owned object: the SD-backed database lives in its own
 * translation unit and is opened here at start-up. Passing NULL is valid and
 * means "no recognition", which must still produce generic read-only Devices.
 */
static app_recognizer_ref_t s_recognizer;

/*
 * The SD corpus and its reader.
 *
 * The index is the reader's only variable-size buffer, and it is static so the
 * budget is fixed and knowable: APP_DB_INDEX_BUDGET bytes, no heap, no growth with
 * corpus size. The reader itself holds the header plus one record at a time.
 *
 * Both live here rather than in a platform-independent file because opening a
 * database is an I/O operation, and this worker is the single owner of I/O.
 */
#define APP_DB_INDEX_BUDGET (256u * DEVICE_DB_INDEX_BUCKET_SIZE)

static app_device_db_t s_db;
static app_device_db_sd_t s_db_sd;
static uint8_t s_db_index[APP_DB_INDEX_BUDGET];

/*
 * Recognition results for the current generation, filled by the enrichment stage
 * and consumed by materialisation. Static for the same reason as the evidence
 * store: it is too large for the worker's stack and has exactly one owner.
 */
static app_recognition_table_t s_recognition;

/* Whether the corpus was usable when the current scan started, for the report. */
static app_db_state_t s_db_state_at_scan = APP_DB_STATE_CLOSED;

/* Set by app_diag_console so the resource report can include its stack. */
static TaskHandle_t s_console_task;

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

    out->db_state = app_db_state_name(s_db.state);
    out->db_path = s_db_sd.path;

    return ESP_OK;
}

void app_runtime_set_console_task(TaskHandle_t task)
{
    s_console_task = task;
}

/* ---------------- recognition database ---------------- */

/*
 * Progress hook for the streaming body-checksum pass.
 *
 * Runs on the worker task, which owns the operation gate, so it reads the cancel
 * flag directly. This is what keeps a cancel during database validation bounded:
 * without it a scan could sit hashing a corpus for seconds after the user asked
 * it to stop.
 */
static bool db_progress_cancel_requested(void *ctx)
{
    (void)ctx;
    return app_ops_scan_is_canceled(&s_ops);
}

/*
 * Open (or reopen) the corpus.
 *
 * Called once at start-up and again before each scan, because a card that was
 * absent when the device booted must not require a reboot once it is inserted.
 * Never fatal: every outcome is recorded in `app_db_state_name()` terms and the
 * application continues with generic Devices.
 */
static void open_recognition_database(void)
{
    app_db_storage_ops_t ops = app_device_db_sd_ops();

    app_device_db_close(&s_db);
    app_device_db_set_progress(&s_db, db_progress_cancel_requested, NULL,
                               APP_DB_PROGRESS_INTERVAL_BYTES);
    app_device_db_open(&s_db, &ops, &s_db_sd, s_db_index,
                       (uint32_t)sizeof(s_db_index));

    if (s_db.state == APP_DB_STATE_READY) {
        /*
         * A newly opened corpus is validated, so match against it. When it is not
         * ready the reference is left pointing at the closed handle: the recognizer
         * then reports "could not run" instead of "nothing matched", which is the
         * distinction the device table reports to the user.
         */
        s_recognizer.ops = app_device_db_recognizer_ops();
        s_recognizer.ctx = &s_db;
        ESP_LOGI(TAG, "recognition database ready: %s v%lu profiles=%lu",
                 s_db_sd.path, (unsigned long)s_db.content_version,
                 (unsigned long)s_db.profile_count);
    } else {
        ESP_LOGW(TAG, "recognition unavailable: %s", app_db_state_name(s_db.state));
    }
}

app_db_state_t app_runtime_db_state(void)
{
    return s_db.state;
}

esp_err_t app_runtime_db_describe(char *out, size_t out_size)
{
    const app_recognizer_ops_t *ops = app_device_db_recognizer_ops();

    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ops == NULL || ops->describe == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ops->describe(&s_db, out, out_size);
    return ESP_OK;
}

const char *app_runtime_db_path(void)
{
    return s_db_sd.path;
}

bool app_runtime_db_info(uint32_t *out_content_version, uint32_t *out_profile_count)
{
    /*
     * Zeroed first, always. A caller that ignores the return value then reports "no
     * version" rather than the previous corpus's version - which would be a status
     * document describing a database the card no longer holds.
     */
    if (out_content_version != NULL) {
        *out_content_version = 0u;
    }
    if (out_profile_count != NULL) {
        *out_profile_count = 0u;
    }
    if (s_db.state != APP_DB_STATE_READY) {
        return false;
    }
    if (out_content_version != NULL) {
        *out_content_version = s_db.content_version;
    }
    if (out_profile_count != NULL) {
        *out_profile_count = s_db.profile_count;
    }
    return true;
}

/* ---------------- provisioning portal ---------------- */

/*
 * The portal session and its two transports.
 *
 * The session is owned by the worker, because only the worker handles a `portal` request
 * - and the worker is also the only thing that runs a scan, which is what makes the
 * operation gate meaningful. The HTTP server runs its own task, but its handlers read the
 * same session object and go through the wire layer, so there is still one business path.
 */
static app_provision_t s_portal;
static app_http_portal_t s_http_portal;
static app_db_import_t s_portal_import;

/*
 * The password and the token are shown once, to the local console, and never again.
 *
 * This flag is what makes "once" true: the operator reads them off the screen and types
 * them, and a second `portal status` afterwards prints where to find them instead of
 * repeating them into a scrollback buffer that may be captured. It is exactly the
 * discipline the product rules ask for - the secret is presented locally, once, and does
 * not become a value that lives in logs.
 */
static bool s_portal_credentials_shown;

static void portal_db_close(void *ctx)
{
    (void)ctx;
    /*
     * Closing the reader is what makes the replace window harmless. Between the two
     * renames the corpus path holds nothing, so a reader mid-seek would be reading a file
     * that no longer has a name - and could answer a match out of a corpus being retired.
     */
    app_device_db_close(&s_db);
}

static esp_err_t portal_db_reopen(void *ctx)
{
    (void)ctx;
    open_recognition_database();
    /*
     * A reopen that finds no usable corpus is a normal state, not an error: the reader is
     * back either way and the reported database state says why recognition is off. Failing
     * here would turn "the card is empty" into "the portal broke".
     */
    return ESP_OK;
}

static uint32_t portal_now_ms(void *ctx)
{
    (void)ctx;
    return now_ms();
}

/*
 * Hand the radio to the portal's access point.
 *
 * The session calls this and checks the result, so a release that fails must be reported
 * rather than swallowed: starting an AP on a driver the station still owns is exactly the
 * state the session's rollback exists to prevent, and it cannot roll back something it was
 * told succeeded.
 */
static esp_err_t portal_sta_release(void *ctx)
{
    bool was_started = false;
    bool was_connected = false;

    (void)ctx;
    return wifi_mgr_release_for_scan(&was_started, &was_connected);
}

static esp_err_t portal_sta_restore(void *ctx)
{
    (void)ctx;
    return wifi_mgr_restore_after_scan();
}

static esp_err_t portal_ap_start(void *ctx, const char *ssid, const char *password)
{
    (void)ctx;
    return wifi_mgr_ap_start(ssid, password);
}

static esp_err_t portal_ap_stop(void *ctx)
{
    (void)ctx;
    return wifi_mgr_ap_stop();
}

static esp_err_t portal_server_start(void *ctx, uint16_t port)
{
    (void)ctx;
    s_http_portal.port = port;
    return app_http_portal_start(&s_http_portal, NULL);
}

static esp_err_t portal_server_stop(void *ctx)
{
    (void)ctx;
    return app_http_portal_stop(&s_http_portal);
}

static void portal_status_fill(void *ctx, app_portal_status_t *out);
static size_t portal_device_count(void *ctx);
static size_t portal_entity_count(void *ctx);

static void portal_status_fill(void *ctx, app_portal_status_t *out)
{
    (void)ctx;
    app_portal_native_status_fill(NULL, out);
}

static size_t portal_device_count(void *ctx)
{
    (void)ctx;
    return app_device_count();
}

static size_t portal_entity_count(void *ctx)
{
    (void)ctx;
    return app_entity_count();
}

static const app_http_portal_hooks_t s_portal_hooks = {
    .status_fill = portal_status_fill,
    .device_count = portal_device_count,
    .entity_count = portal_entity_count,
};

/* Every one of these is required: a partially wired session would silently skip a
 * rollback step, which is exactly the failure the session exists to prevent. */
static app_provision_ops_t portal_ops(void)
{
    app_provision_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.ap_start = portal_ap_start;
    ops.ap_stop = portal_ap_stop;
    ops.sta_restore = portal_sta_restore;
    ops.sta_release = portal_sta_release;
    ops.server_start = portal_server_start;
    ops.server_stop = portal_server_stop;
    ops.db_close = portal_db_close;
    ops.db_reopen = portal_db_reopen;
    ops.now_ms = portal_now_ms;
    ops.random_u32 = esp_random;
    return ops;
}

/*
 * Prepare the portal session.
 *
 * Called once at start-up. Building the session does NOT start it: the portal is an
 * operator action, never a boot action, because an access point that came up on its own
 * would take the station down at every reboot.
 */
static void portal_setup(void)
{
    app_provision_ops_t ops = portal_ops();
    app_db_import_io_t io = app_device_db_sd_ops();
    app_provision_config_t config;
    char path[APP_DB_IMPORT_PATH_MAX];

    app_provision_config_default(&config);

    if (app_device_db_sd_path(&s_db_sd, path, sizeof(path))) {
        (void)app_db_import_init(&s_portal_import, &io, &s_db_sd, path, 0u);
    }

    memset(&s_http_portal, 0, sizeof(s_http_portal));
    s_http_portal.session = &s_portal;
    s_http_portal.import = &s_portal_import;
    s_http_portal.hooks = &s_portal_hooks;
    s_http_portal.hooks_ctx = NULL;

    if (app_provision_init(&s_portal, &ops, NULL, &s_ops, &config) != ESP_OK) {
        ESP_LOGE(TAG, "portal session could not be initialised");
    }
}

/*
 * `request <id> portal <start|stop|status>`
 *
 * Runs on the worker, like every other request, so the operation gate it takes is the
 * same one a scan takes and the two cannot overlap.
 */
static app_diag_error_t execute_portal(const app_diag_request_t *request,
                                       app_diag_response_t *response,
                                       char *payload, size_t payload_size)
{
    const char *verb = request->action;

    if (strcmp(verb, "start") == 0) {
        app_provision_status_t status = app_provision_start(&s_portal);

        if (status != APP_PROVISION_OK) {
            response->detail = app_provision_status_name(status);
            return (status == APP_PROVISION_ERR_BUSY) ? APP_DIAG_ERR_BUSY
                                                      : APP_DIAG_ERR_INTERNAL;
        }
        /*
         * The credentials are NOT assembled here. Starting the session is this worker's
         * job; presenting the password is the local console's, through
         * app_runtime_portal_present(). Keeping them apart is what stops the secret from
         * travelling through a response structure that other transports also use.
         */
        s_portal_credentials_shown = false;
        return APP_DIAG_OK;
    }

    if (strcmp(verb, "stop") == 0) {
        app_provision_status_t status = app_provision_stop(&s_portal);

        s_portal_credentials_shown = false;
        return (status == APP_PROVISION_OK) ? APP_DIAG_OK : APP_DIAG_ERR_INTERNAL;
    }

    if (strcmp(verb, "status") == 0) {
        /*
         * Status never contains the password or the token, whether or not they have been
         * shown. It reports that the session is up and whether the credentials were
         * presented - the same discipline /api/status follows, applied to the console as
         * well so nobody is tempted to treat the serial port as an exception.
         */
        (void)payload;
        (void)payload_size;
        return APP_DIAG_OK;
    }

    response->detail = "portal_verb_unknown";
    return APP_DIAG_ERR_INVALID_ARGUMENT;
}

/*
 * Present the session credentials, once, to the caller - which must be the local console.
 *
 * The name says "present" rather than "get" because that is the contract: these values are
 * shown to the operator and are not readable again afterwards. A second call reports
 * where they went rather than repeating them, so a terminal scrollback captured later does
 * not contain the password.
 *
 * Returns false when no session is active.
 */
bool app_runtime_portal_present(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return false;
    }
    out[0] = '\0';

    if (!app_provision_is_active(&s_portal)) {
        return false;
    }
    if (s_portal_credentials_shown) {
        (void)snprintf(out, out_size,
                       "portal=active credentials=shown_once already");
        return true;
    }

    {
        char ap_ipv4[WIFI_MGR_IPV4_MAX];

        if (wifi_mgr_ap_ipv4(ap_ipv4, sizeof(ap_ipv4)) != ESP_OK) {
            (void)app_strlcpy(ap_ipv4, "-", sizeof(ap_ipv4));
        }
        (void)snprintf(out, out_size,
                       "portal=active\n"
                       "ssid=%s\n"
                       "password=%s\n"
                       "token=%s\n"
                       "ap_ipv4=%s\n"
                       "note: shown once on this console; never repeated in a status "
                       "response or a log",
                       s_portal.ap_ssid, s_portal.ap_password, s_portal.token, ap_ipv4);
    }
    s_portal_credentials_shown = true;
    return true;
}

/* One-line portal report for `portal status`: phase, whether the AP is really up, and
 * whether the credentials were presented. Never the credentials themselves. */
esp_err_t app_runtime_portal_status(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(out, out_size,
                   "portal=%s ap_active=%u credentials=%s upload=%s",
                   app_provision_phase_name(s_portal.phase),
                   (unsigned)(wifi_mgr_ap_is_up() ? 1u : 0u),
                   s_portal_credentials_shown ? "presented" : "not_presented",
                   app_db_import_phase_name(s_portal_import.phase));
    return ESP_OK;
}

/*
 * The enrichment stage body.
 *
 * One recognition pass over all accumulated evidence, before anything is written
 * to the device tables. The order is the product's: match and refine everything,
 * then materialise. Doing it the other way would mean matching per protocol while
 * the device table is being written, which is several passes over the same corpus
 * and no single place where "what did recognition decide" can be observed.
 *
 * The results go into `s_recognition`, and materialisation consumes that table
 * through a recognizer view - so the database is touched exactly here.
 */
static app_diag_error_t run_enrichment_stage(app_scan_evidence_t *ev,
                                             bool *out_truncated)
{
    size_t observations;

    *out_truncated = false;

    /*
     * Reopen first: the card may have been inserted or replaced since the last
     * scan, and a scan is the natural point at which to notice.
     */
    open_recognition_database();
    s_db_state_at_scan = s_db.state;

    app_scan_native_request_cancel(app_ops_scan_is_canceled(&s_ops));
    observations = app_recognition_enrich(ev, &s_recognizer, &s_recognition);

    if (app_recognition_table_truncated(&s_recognition)) {
        /* Capacity, not a recognition failure: reported so the scan is partial
         * rather than presented as fully covered. */
        *out_truncated = true;
    }

    if (!app_db_state_is_usable(s_db.state)) {
        /*
         * Recognition could not run. The stage is SKIPPED with the database's own
         * reason rather than FAILED: a missing card is a normal deployment state,
         * not a scan error, and the scan report already carries the distinction
         * between "no database" and "nothing matched".
         */
        ESP_LOGW(TAG, "enrichment skipped: recognition %s",
                 app_db_state_name(s_db.state));
        return APP_DIAG_ERR_NOT_IMPLEMENTED;
    }

    if (app_ops_scan_is_canceled(&s_ops)) {
        return APP_DIAG_ERR_CANCELED;
    }

    ESP_LOGI(TAG, "enrichment: %lu observations against %s",
             (unsigned long)observations, s_db_sd.path);
    return APP_DIAG_OK;
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

    count = app_device_count();

    for (size_t i = 0u; i < count; ++i) {
        const app_device_binding_t *binding = app_device_at(i);
        const ha_device_t *device;
        char line[288];
        int written;

        if (binding == NULL) {
            continue;
        }
        device = ha_core_device_get(binding->ha_device_id);

        written = snprintf(line, sizeof(line),
                           "device id=%s name=%s protocol=%s recognition=%s availability=%s"
                           " sources=%u entities=%u signal=%d generation=%lu\n",
                           binding->device_id,
                           (device != NULL && device->name[0] != '\0') ? device->name : "-",
                           binding->protocol_label[0] != '\0' ? binding->protocol_label : "-",
                           app_recognition_name(binding->recognition),
                           app_availability_name(binding->availability),
                           (unsigned)binding->sources,
                           (unsigned)app_entity_count_for_device(binding->device_id),
                           binding->has_signal ? (int)binding->signal_dbm : 0,
                           (unsigned long)binding->last_generation);
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
                            const app_entity_binding_t *binding, bool *truncated)
{
    const ha_state_t *state;
    const app_device_binding_t *device;
    char line[288];
    int written;

    state = ha_core_state_get(binding->entity_id);
    device = app_device_find(binding->device_id);

    written = snprintf(line, sizeof(line),
                       "entity id=%s device=%s state=%s unit=%s writable=%u available=%u\n",
                       binding->entity_id, binding->device_id,
                       state != NULL ? state->state : "unknown",
                       binding->unit[0] != '\0' ? binding->unit : "-",
                       (unsigned)(binding->writable ? 1u : 0u),
                       (unsigned)((device != NULL &&
                                   device->availability == APP_AVAILABILITY_ONLINE)
                                      ? 1u
                                      : 0u));
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
    size_t count;

    if (out == NULL || out_size == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    count = app_entity_count();
    for (size_t i = 0u; i < count; ++i) {
        const app_entity_binding_t *binding = app_entity_at(i);

        if (binding == NULL) {
            continue;
        }
        if (device_id != NULL && device_id[0] != '\0' &&
            strcmp(binding->device_id, device_id) != 0) {
            continue;
        }
        used = append_entity(out, out_size, used, binding, &truncated);
    }

    if (used == 0u) {
        static const char none[] = "no entities\n";
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
 * The stage policy decides first whether the stage is applicable at all (no IP,
 * missing credentials, protocol backend not wired). Only then is the native body
 * invoked. Anything that did not actually run is recorded as SKIPPED or FAILED,
 * never as DONE, so a partial scan cannot be mistaken for a complete one.
 *
 * `s_scan_inputs` is refreshed from live state before the stage runs.
 */
static app_diag_error_t run_scan_stage(app_scan_stage_t stage,
                                       app_scan_evidence_t *ev,
                                       const app_scan_native_config_t *cfg,
                                       app_scan_native_stats_t *stats)
{
    app_scan_stage_plan_t plan;
    esp_err_t err;

    plan = app_scan_plan_stage(&s_scan_inputs, stage);
    if (plan.action != APP_STAGE_ACTION_RUN) {
        /* Not applicable now. Record the reason and move on: a skipped stage
         * must not be reported as an error the caller has to guess at. */
        if (app_ops_stage_begin(&s_ops, stage) == APP_OPS_OK) {
            (void)app_ops_stage_end(&s_ops, stage, plan.terminal_state);
        }
        if (plan.action == APP_STAGE_ACTION_REJECTED) {
            return APP_DIAG_ERR_INTERNAL;
        }
        return APP_DIAG_ERR_NOT_IMPLEMENTED;
    }

    if (app_ops_stage_begin(&s_ops, stage) != APP_OPS_OK) {
        return APP_DIAG_ERR_CANCELED;
    }

    app_scan_native_request_cancel(app_ops_scan_is_canceled(&s_ops));

    switch (stage) {
    case APP_STAGE_WIFI_RF:
        err = app_scan_native_wifi_rf(ev, cfg, stats);
        /* LAN applicability changed: the STA handover result decides it. */
        s_scan_inputs.has_ip = wifi_mgr_has_ip();
        s_scan_inputs.wifi_connected = s_scan_inputs.has_ip;
        break;
    case APP_STAGE_BLE_RF:
        err = app_scan_native_ble_rf(ev, cfg, stats);
        break;
    case APP_STAGE_MDNS:
        err = app_scan_native_mdns(ev, cfg, stats);
        break;
    case APP_STAGE_SSDP:
        err = app_scan_native_ssdp(ev, cfg, stats);
        break;
    case APP_STAGE_LAN_HOSTS:
        err = app_scan_native_lan_hosts(ev, cfg, stats);
        break;
    case APP_STAGE_LAN_SERVICES:
        err = app_scan_native_lan_services(ev, cfg, stats);
        break;
    case APP_STAGE_ENRICHMENT: {
        bool trunc4 = false;
        app_diag_error_t enrich_error = run_enrichment_stage(ev, &trunc4);

        if (trunc4) {
            app_ops_scan_mark_truncated(&s_ops);
        }
        if (enrich_error == APP_DIAG_OK) {
            (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_DONE);
            return APP_DIAG_OK;
        }
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_SKIPPED);
        return enrich_error;
    }
    case APP_STAGE_MATERIALIZE: {
        bool truncated = false;
        /*
         * Materialisation consumes the recognition table, not the database. A
         * device with no entry there is reported as "recognition unavailable",
         * which is exactly what happens when the enrichment stage was skipped, so
         * a missing database can never turn into a silent "unknown".
         */
        app_recognizer_ref_t ref = app_recognition_table_recognizer(&s_recognition);

        (void)app_device_materialize(ev, &ref, &truncated);
        if (truncated) {
            app_ops_scan_mark_truncated(&s_ops);
        }
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_DONE);
        return APP_DIAG_OK;
    }
    default:
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_SKIPPED);
        return APP_DIAG_ERR_NOT_IMPLEMENTED;
    }

    /*
     * The RF stages decide their own recorded state.
     *
     * A stage can return ESP_OK while having ended on its own deadline, or having
     * failed to shut its session down; deriving DONE from the return code alone
     * would let a partial scan look complete. The verdict computed by
     * app_scan_evaluate_rf_stage() is therefore authoritative for those stages.
     */
    if (stage == APP_STAGE_WIFI_RF || stage == APP_STAGE_BLE_RF) {
        const bool is_wifi = stage == APP_STAGE_WIFI_RF;
        const bool verdict_set = is_wifi ? stats->wifi_verdict_set
                                         : stats->ble_verdict_set;
        const app_scan_rf_verdict_t verdict = is_wifi ? stats->wifi_verdict
                                                      : stats->ble_verdict;

        if (verdict_set) {
            (void)app_ops_stage_end(&s_ops, stage, verdict.terminal_state);
            switch (verdict.terminal_state) {
            case APP_STAGE_STATE_DONE:
            case APP_STAGE_STATE_PARTIAL:
                return APP_DIAG_OK;
            case APP_STAGE_STATE_CANCELED:
                return APP_DIAG_ERR_CANCELED;
            default:
                return APP_DIAG_ERR_INTERNAL;
            }
        }
    }

    if (err == ESP_OK) {
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_DONE);
        return APP_DIAG_OK;
    }

    /* A cancelled stage is a normal bounded outcome, not a failure. */
    if (app_ops_scan_is_canceled(&s_ops)) {
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_CANCELED);
        return APP_DIAG_ERR_CANCELED;
    }

    if (err == ESP_ERR_NOT_SUPPORTED) {
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_SKIPPED);
        return APP_DIAG_ERR_NOT_IMPLEMENTED;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* Dependency vanished mid-scan (for example the IP was lost). */
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_SKIPPED);
        return APP_DIAG_ERR_UNSUPPORTED;
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        /*
         * The radio is still owned by a previous session whose teardown timed out.
         *
         * FAILED, not SKIPPED: this is not "the protocol is not wired up", it is
         * "this scan could not use the radio at all". Reporting it as skipped would
         * present a scan that saw nothing as a normal, complete outcome.
         */
        ESP_LOGE(TAG, "scan stage %s failed: the radio is held by a previous session",
                 app_scan_stage_name(stage));
        (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_FAILED);
        return APP_DIAG_ERR_INTERNAL;
    }

    ESP_LOGW(TAG, "scan stage %s failed: %s", app_scan_stage_name(stage),
             esp_err_to_name(err));
    (void)app_ops_stage_end(&s_ops, stage, APP_STAGE_STATE_FAILED);
    return APP_DIAG_ERR_INTERNAL;
}

static app_diag_response_t execute_scan(const app_diag_request_t *request)
{
    app_diag_response_t response = make_response(request, APP_DIAG_OK);
    app_scan_status_t report;
    app_diag_error_t first_error = APP_DIAG_OK;
    uint32_t budget_ms;
    uint32_t start_ms;
    int last_stage = -1;
    app_scan_native_config_t native_cfg;

    if (app_ops_scan_begin(&s_ops, request->request_id, &report) != APP_OPS_OK) {
        response.error = APP_DIAG_ERR_BUSY;
        response.detail = "operation_active";
        return response;
    }
    publish_state();

    /* New generation: clear evidence, advance the application generation and
     * mark every ephemeral binding as not-yet-seen. */
    app_scan_evidence_reset(&s_evidence, report.generation);
    app_device_generation_begin(report.generation);

    app_scan_native_config_default(&native_cfg);
    memset(&s_scan_stats, 0, sizeof(s_scan_stats));
    app_scan_native_request_cancel(false);

    wifi_mgr_status_t wifi_status;
    wifi_mgr_get_status(&wifi_status);
    memset(&s_scan_inputs, 0, sizeof(s_scan_inputs));
    s_scan_inputs.wifi_configured = wifi_status.credentials_present;
    s_scan_inputs.has_ip = wifi_mgr_has_ip();
    s_scan_inputs.wifi_connected = s_scan_inputs.has_ip;
    s_scan_inputs.wifi_driver_acquired = true; /* the handover decides below */
    s_scan_inputs.ble_available = true;
    s_scan_inputs.canceled = false;

    budget_ms = request->timeout_ms != 0u ? request->timeout_ms
                                          : APP_RUNTIME_DEFAULT_SCAN_BUDGET_MS;
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

        s_scan_inputs.canceled = app_ops_scan_is_canceled(&s_ops);
        stage_error = run_scan_stage((app_scan_stage_t)i, &s_evidence,
                                     &native_cfg, &s_scan_stats);
        last_stage = i;

        /* The first non-OK stage outcome is reported. A skipped stage still
         * contributes NOT_IMPLEMENTED so the caller can tell that this scan did
         * not cover everything; the per-stage state carries the detail. */
        if (first_error == APP_DIAG_OK && stage_error != APP_DIAG_OK) {
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

    /* Sweep only what a protocol that actually ran failed to re-observe. A
     * skipped or failed stage must not make its devices look disappeared. */
    app_device_generation_finish(&report);

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

    /* Evidence capacity overflow is a real truncation of the reported result. */
    if (app_scan_evidence_truncated(&s_evidence)) {
        response.truncated = true;
    }

    response.partial = report.partial || (first_error != APP_DIAG_OK);
    response.truncated = response.truncated || report.truncated;
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
            /* Nothing was running: say so rather than reporting a successful
             * cancellation of a scan that never existed. */
            response.error = APP_DIAG_ERR_NOT_ACTIVE;
            response.detail = "no_active_scan";
        }
        publish_state();
        return response;

    case APP_DIAG_CMD_DEVICES:
    case APP_DIAG_CMD_ENTITIES:
        /* Enumeration is served from the application binding tables, which are
         * the same state the future GUI will read. */
        return make_response(request, APP_DIAG_OK);

    case APP_DIAG_CMD_CONTROL:
        /* Control never silently succeeds: an unimplemented backend reports
         * NOT_IMPLEMENTED so no caller can mistake it for a state change. */
        return make_response(request, APP_DIAG_ERR_NOT_IMPLEMENTED);

    case APP_DIAG_CMD_PORTAL: {
        app_diag_error_t err = execute_portal(request, &response, NULL, 0u);

        return (err == APP_DIAG_OK) ? make_response(request, APP_DIAG_OK)
                                    : make_response(request, err);
    }

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
    app_scan_evidence_reset(&s_evidence, 0u);
    app_recognition_table_reset(&s_recognition);
    app_device_table_reset();

    /*
     * Bind the SD adapter and take a first look at the corpus.
     *
     * "At boot: attempt device_db_open(); continue application startup regardless
     * of recognition result." A missing card, a missing file, a corrupt corpus and
     * an unsupported version are all recorded states, not start-up failures, and a
     * card inserted later is picked up by the reopen before the next scan.
     */
    {
        app_device_db_sd_config_t sd_config;

        memset(&sd_config, 0, sizeof(sd_config));
        sd_config.auto_mount = true;
        sd_config.format_if_mount_failed = false;
        app_device_db_sd_init(&s_db_sd, &sd_config);
        open_recognition_database();
    }

    /* Prepare STA ownership and load stored credentials. An unprovisioned
     * device is a normal state; it must not stop the runtime from starting. */
    (void)wifi_mgr_init();

    /*
     * Build the portal session.
     *
     * Built, not started: an access point that came up on its own would take the station
     * down at every reboot. Starting one is an operator action, through the local console.
     */
    portal_setup();

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
    (void)app_strlcpy(request.target, entity_id, sizeof(request.target));
    (void)app_strlcpy(request.action, action, sizeof(request.action));
    if (value != NULL) {
        (void)app_strlcpy(request.value, value, sizeof(request.value));
    }
    return app_runtime_submit(&request, out_response, 0u);
}
