/*
 * Native implementation of the B2 scan stages.
 *
 * Every callback in this file runs on a Kismet session task. The session's own
 * bounded copy structures are the ONLY thing callbacks read: no pointer from a
 * callback is retained beyond the call, and every value that must survive is
 * copied into the application evidence store before the callback returns.
 */

#include "app_scan_native.h"
#include "app_str.h"

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "ha_discovery.h"
#include "kismet_ble.h"
#include "kismet_wifi.h"
#include "nmap_l2.h"
#include "wireshark_l2.h"

static const char *TAG = "app_scan";

#define SCAN_WIFI_DEFAULT_DURATION_MS 4000u
#define SCAN_BLE_DEFAULT_DURATION_MS 4000u
#define SCAN_MDNS_DEFAULT_TIMEOUT_MS 3000u
#define SCAN_SSDP_DEFAULT_TIMEOUT_MS 2500u
#define SCAN_NMAP_DEFAULT_TIMEOUT_MS 6000u
#define SCAN_WIFI_DEFAULT_DEVICES 32u
#define SCAN_BLE_DEFAULT_DEVICES 32u

/* Bounded staging buffers. Deliberately small: this is a working set, not the
 * recognition corpus, which lives on SD. */
#define SCAN_MDNS_MAX_SERVICES 12u
#define SCAN_SSDP_MAX_SERVICES 12u
#define SCAN_NMAP_MAX_HOSTS 16u
#define SCAN_NMAP_MAX_PORTS 8u
#define SCAN_NMAP_MAX_ENDPOINTS 8u
/* Bytes read from a service before giving up. Bounded so a chatty or hostile
 * peer cannot make the prober read indefinitely. */
#define SCAN_SERVICE_CAPTURE_BYTES 256u

static volatile bool s_cancel;

void app_scan_native_request_cancel(bool canceled)
{
    s_cancel = canceled;
}

bool app_scan_native_cancel_requested(void)
{
    return s_cancel;
}

/*
 * A session whose teardown timed out.
 *
 * destroy_checked() deliberately does not free a session whose task refused to
 * exit, because that task still dereferences it - and, more importantly on this
 * board, because that task still owns the radio: the Kismet Wi-Fi session calls
 * esp_wifi_stop()/esp_wifi_deinit() in its own cleanup, and the BLE session owns
 * nvs_flash and the NimBLE host. So a timed-out session is not just a leak, it is
 * a live owner of hardware the application must not touch.
 *
 * Holding the handle here is what turns "leaked" into "recoverable":
 *
 *   - the radio stays IN QUARANTINE while the task is alive. `wifi_rf` refuses to
 *     start, so the application never calls esp_wifi_init() underneath a task that
 *     is about to call esp_wifi_deinit(), which is the one ordering that could
 *     tear down a driver another session just brought up;
 *   - release_quarantined_sessions() retries the destroy as soon as the task has
 *     finished, so the session and its queue, semaphores and tracker are reclaimed
 *     rather than accumulating one per failed teardown;
 *   - the stage that hit it is FAILED with unusable evidence, so a scan is never
 *     reported as complete on data from a session that could not be stopped.
 *
 * At most one Wi-Fi and one BLE session can be quarantined at a time, because a
 * second session of the same family cannot start while the first is alive. The
 * count is therefore bounded by construction, not by a retry budget.
 */
typedef struct {
    kismet_wifi_session_t *wifi;
    kismet_ble_session_t *ble;
} quarantined_sessions_t;

static quarantined_sessions_t s_quarantine;
/* The BLE tracker a quarantined BLE session still writes into. Held for exactly
 * as long as that session, never freed while its task can still call back. */
static kismet_ble_tracker_t *s_ble_tracker_in_quarantine;

/* Set true when a session had to be quarantined and is still alive. */
static app_scan_rf_quarantine_t s_wifi_quarantine_state;
static app_scan_rf_quarantine_t s_ble_quarantine_state;

const char *app_scan_rf_quarantine_name(app_scan_rf_quarantine_t state)
{
    switch (state) {
    case APP_SCAN_RF_QUARANTINE_NONE:      return "none";
    case APP_SCAN_RF_QUARANTINE_HELD:      return "held";
    case APP_SCAN_RF_QUARANTINE_RECLAIMED: return "reclaimed";
    default:                               return "invalid";
    }
}

app_scan_rf_quarantine_t app_scan_native_wifi_quarantine(void)
{
    return s_wifi_quarantine_state;
}

app_scan_rf_quarantine_t app_scan_native_ble_quarantine(void)
{
    return s_ble_quarantine_state;
}

/*
 * Reclaim everything a previous failed teardown left behind.
 *
 * Called at the start of every RF stage, before anything else. Returns the number
 * of sessions still alive afterwards: a non-zero result means the radio is still
 * owned by a task that refused to exit, and the stage must not start.
 */
static size_t release_quarantined_sessions(void)
{
    size_t alive = 0u;

    if (s_quarantine.wifi != NULL) {
        if (kismet_wifi_session_task_alive(s_quarantine.wifi)) {
            alive++;
        } else if (kismet_wifi_session_destroy_checked(s_quarantine.wifi) == ESP_OK) {
            ESP_LOGW(TAG, "a quarantined wifi session finally exited and was reclaimed");
            s_quarantine.wifi = NULL;
            s_wifi_quarantine_state = APP_SCAN_RF_QUARANTINE_RECLAIMED;
            /*
             * The driver has been handed back by the task that owned it, so the
             * STA owner may take it again. Reconnecting here rather than at the
             * end of the failed scan is what keeps the network outage limited to
             * the time the task actually took.
             */
            (void)wifi_mgr_release_quarantine();
        } else {
            /* Still not destroyable. Treated exactly like "still alive": the radio
             * is not ours to take. */
            alive++;
        }
    }

    if (s_quarantine.ble != NULL) {
        if (kismet_ble_session_task_alive(s_quarantine.ble)) {
            alive++;
        } else if (kismet_ble_session_destroy_checked(s_quarantine.ble) == ESP_OK) {
            ESP_LOGW(TAG, "a quarantined ble session finally exited and was reclaimed");
            s_quarantine.ble = NULL;
            s_ble_quarantine_state = APP_SCAN_RF_QUARANTINE_RECLAIMED;
            /* The task can no longer write to its tracker, so this is the first
             * point at which freeing it is safe. */
            kismet_ble_tracker_destroy(s_ble_tracker_in_quarantine);
            s_ble_tracker_in_quarantine = NULL;
        } else {
            alive++;
        }
    }

    return alive;
}

bool app_scan_native_radio_available(void)
{
    return release_quarantined_sessions() == 0u;
}

void app_scan_native_config_default(app_scan_native_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->wifi_duration_ms = SCAN_WIFI_DEFAULT_DURATION_MS;
    out->ble_duration_ms = SCAN_BLE_DEFAULT_DURATION_MS;
    out->mdns_timeout_ms = SCAN_MDNS_DEFAULT_TIMEOUT_MS;
    out->ssdp_timeout_ms = SCAN_SSDP_DEFAULT_TIMEOUT_MS;
    out->nmap_timeout_ms = SCAN_NMAP_DEFAULT_TIMEOUT_MS;
    out->wifi_max_devices = SCAN_WIFI_DEFAULT_DEVICES;
    out->ble_max_devices = SCAN_BLE_DEFAULT_DEVICES;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static bool cancel_cb(void *ctx)
{
    (void)ctx;
    return s_cancel;
}

/* ================= Wi-Fi RF ================= */

/* Frames between the session callback and the tracker are copied by the session
 * before delivery; this callback converts the bounded copy into application
 * evidence and parses the management frame with the Wireshark family. */
static void wifi_frame_cb(const kismet_wifi_frame_t *frame, void *ctx)
{
    app_scan_evidence_t *ev = (app_scan_evidence_t *)ctx;
    wireshark_wifi_mgmt_t mgmt;
    app_scan_wifi_t obs;

    if (ev == NULL || frame == NULL) {
        return;
    }

    memset(&mgmt, 0, sizeof(mgmt));
    if (wireshark_wifi_mgmt_parse(frame->bytes, frame->captured_len, &mgmt) !=
        WIRESHARK_PARSE_OK) {
        /* Parsing failures are counted by the caller through the session result;
         * nothing is invented for a frame we could not read. */
        return;
    }
    if (mgmt.diag.malformed) {
        return;
    }

    /* Only beacon/probe-response style frames carry a BSSID role we can use. */
    memset(&obs, 0, sizeof(obs));
    obs.generation = ev->generation;
    obs.role = 1u; /* AP-ish: advertised */
    memcpy(obs.bssid, mgmt.bssid, 6);
    obs.rssi_last = frame->rssi;
    obs.channel = frame->channel != 0u ? frame->channel : mgmt.ies.channel;
    obs.last_seen_ms = frame->seen_ms;
    obs.first_seen_ms = frame->seen_ms;
    obs.seen_count = 1u;
    if (obs.channel != 0u) {
        obs.channel_bitmap = (uint16_t)(1u << (obs.channel & 0x0Fu));
    }

    if (mgmt.ies.ssid_present) {
        obs.has_ssid = true;
        obs.ssid_hidden = mgmt.ies.ssid_hidden;
        obs.ssid_len = mgmt.ies.ssid_len <= APP_SCAN_MAX_SSID
                           ? mgmt.ies.ssid_len
                           : APP_SCAN_MAX_SSID;
        memcpy(obs.ssid, mgmt.ies.ssid, obs.ssid_len);
    }

    (void)app_scan_ingest_wifi(ev, &obs);
}

/*
 * Fill in SSIDs from the tracker for devices the frame parser could not name.
 * The tracker joins devices to SSIDs separately, so a device materialised from
 * a probe request would otherwise stay nameless.
 */
static void enrich_wifi_from_tracker(app_scan_evidence_t *ev,
                                     const kismet_wifi_tracker_t *tracker)
{
    for (size_t i = 0u; i < ev->wifi_count; ++i) {
        app_scan_wifi_t *entry = &ev->wifi[i];
        uint8_t ssid[KISMET_WIFI_MAX_SSID_LEN];
        uint8_t ssid_len = 0u;
        bool hidden = false;

        if (entry->has_ssid && entry->ssid_len > 0u) {
            continue;
        }
        if (kismet_wifi_tracker_get_device_ssid(tracker, entry->bssid, ssid,
                                                sizeof(ssid), &ssid_len,
                                                &hidden) != ESP_OK) {
            continue;
        }
        if (hidden) {
            entry->ssid_hidden = true;
            continue;
        }
        if (ssid_len == 0u || ssid_len > APP_SCAN_MAX_SSID) {
            continue;
        }
        entry->has_ssid = true;
        entry->ssid_hidden = false;
        entry->ssid_len = ssid_len;
        memcpy(entry->ssid, ssid, ssid_len);
    }
}

/* Copy the tracker's bounded device table into the evidence store. This is what
 * makes tracker-only sightings (an AP seen before the first frame copy) visible
 * rather than silently missing. */
static void ingest_wifi_tracker(app_scan_evidence_t *ev,
                                const kismet_wifi_tracker_t *tracker)
{
    size_t count = kismet_wifi_tracker_device_count(tracker);

    for (size_t i = 0u; i < count; ++i) {
        kismet_wifi_device_t device;
        app_scan_wifi_t obs;

        if (kismet_wifi_tracker_get_device(tracker, i, &device) != ESP_OK) {
            continue;
        }

        memset(&obs, 0, sizeof(obs));
        obs.generation = ev->generation;
        memcpy(obs.bssid, device.mac, 6);
        obs.role = (uint8_t)device.role;
        obs.rssi_last = device.rssi_last;
        obs.channel = device.last_channel;
        obs.channel_bitmap = device.channel_bitmap;
        obs.first_seen_ms = device.first_seen_ms;
        obs.last_seen_ms = device.last_seen_ms;
        obs.seen_count = device.seen_count;

        (void)app_scan_ingest_wifi(ev, &obs);
    }
}

esp_err_t app_scan_native_wifi_rf(app_scan_evidence_t *ev,
                                  const app_scan_native_config_t *config,
                                  app_scan_native_stats_t *stats)
{
    app_scan_native_config_t cfg;
    kismet_wifi_tracker_t *tracker = NULL;
    kismet_wifi_session_t *session = NULL;
    kismet_wifi_session_config_t session_cfg;
    kismet_wifi_tracker_config_t tracker_cfg;
    kismet_wifi_session_result_t result;
    kismet_wifi_tracker_stats_t tracker_stats;
    bool was_started = false;
    bool was_connected = false;
    esp_err_t err;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 0. Reclaim anything a previous failed teardown left behind, and refuse to
     *    take the radio while another task still owns it.
     *
     * `wifi_mgr_release_for_scan()` tears the STA driver down and the Kismet
     * session builds its own. If a previous session never exited, that task may
     * call esp_wifi_deinit() at any moment - after this scan has initialised the
     * driver. Refusing here is the only ordering that cannot corrupt the driver's
     * state; the stage is recorded FAILED so the scan is not presented as complete.
     */
    if (!app_scan_native_radio_available()) {
        ESP_LOGE(TAG, "wifi stage refused: a previous session still owns the radio");
        if (stats != NULL) {
            stats->wifi_native_error = ESP_ERR_NOT_FINISHED;
            stats->wifi_quarantine = s_wifi_quarantine_state;
            stats->wifi_quarantine_set = true;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    if (config != NULL) {
        cfg = *config;
    } else {
        app_scan_native_config_default(&cfg);
    }
    if (cfg.wifi_duration_ms == 0u) {
        cfg.wifi_duration_ms = SCAN_WIFI_DEFAULT_DURATION_MS;
    }
    if (cfg.wifi_max_devices == 0u) {
        cfg.wifi_max_devices = SCAN_WIFI_DEFAULT_DEVICES;
    }
    if (stats != NULL) {
        stats->wifi_queue_drops = 0u;
        stats->wifi_canceled = false;
        stats->wifi_native_error = ESP_OK;
    }

    /* 1. Take the Wi-Fi driver from the STA owner. The Kismet session will call
     *    esp_wifi_init() itself and refuses to start if the driver is live. */
    err = wifi_mgr_release_for_scan(&was_started, &was_connected);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi release for scan failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Tracker + session. */
    memset(&tracker_cfg, 0, sizeof(tracker_cfg));
    tracker_cfg.max_devices = cfg.wifi_max_devices;
    tracker_cfg.max_ssids = 24u;
    tracker_cfg.max_relations = 24u;
    tracker_cfg.max_ssid_links = 32u;

    err = kismet_wifi_tracker_create(&tracker_cfg, &tracker);
    if (err != ESP_OK) {
        goto restore;
    }

    memset(&session_cfg, 0, sizeof(session_cfg));
    session_cfg.duration_ms = cfg.wifi_duration_ms;
    session_cfg.frame_copy_len = 256u;

    err = kismet_wifi_session_start(&session_cfg, tracker, wifi_frame_cb, ev,
                                    &session);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "kismet wifi session start failed: %s", esp_err_to_name(err));
        goto restore;
    }

    if (s_cancel) {
        (void)kismet_wifi_session_cancel(session);
    }

    /*
     * 3. Wait for the bounded duration.
     *
     * A wait timeout is not by itself an error: the session owns its own
     * duration and cancel path. What matters is that we only touch the tracker
     * once the session task can no longer be running, because the tracker is
     * mutated from that task's callbacks. `stopped` records whether we actually
     * observed completion.
     */
    bool stopped = false;
    err = kismet_wifi_session_wait(session, cfg.wifi_duration_ms + 3000u);
    if (err == ESP_OK) {
        stopped = true;
    } else {
        (void)kismet_wifi_session_cancel(session);
        err = kismet_wifi_session_wait(session, 3000u);
        if (err == ESP_OK) {
            stopped = true;
        } else {
            ESP_LOGE(TAG, "kismet wifi session did not stop in time");
        }
    }

    memset(&result, 0, sizeof(result));
    (void)kismet_wifi_session_get_result(session, &result);

    if (stats != NULL) {
        stats->wifi_queue_drops = result.rx_queue_drops;
        stats->wifi_canceled = result.canceled;
        stats->wifi_native_error = result.native_error;
        stats->frames_malformed += result.malformed_frames;
        stats->frames_truncated += result.truncated_frames;
        stats->frames_parsed += result.frames_delivered;
    }

    /*
     * 4. Tear the session down BEFORE reading the tracker.
     *
     * destroy_checked() waits a bounded time for the session task to exit and
     * reports whether it actually did. Only a confirmed stop makes the tracker
     * and the evidence safe to read: that task is their only writer, so reading
     * them after an unconfirmed stop is a data race.
     *
     * When it did NOT stop, three things follow, and all three are handled here
     * rather than being left to the next scan:
     *
     *   - the session is NOT freed and is NOT forgotten. It is moved to quarantine
     *     with the tracker it still owns, because the tracker must outlive the
     *     callbacks that write into it. Freeing it here would be the
     *     use-after-free the bounded wait exists to avoid;
     *   - no evidence is published and the stage is FAILED;
     *   - the radio is reported as held, so the next Wi-Fi stage refuses to start
     *     until that task has exited. The driver is handed back to the STA owner
     *     only when it is ours to hand back.
     */
    stopped = kismet_wifi_session_destroy_checked(session) == ESP_OK;

    if (stopped) {
        session = NULL;
        ingest_wifi_tracker(ev, tracker);
        enrich_wifi_from_tracker(ev, tracker);
        (void)kismet_wifi_tracker_get_stats(tracker, &tracker_stats);
        kismet_wifi_tracker_destroy(tracker);
        tracker = NULL;
    } else {
        ESP_LOGE(TAG, "wifi session did not stop within the bound; evidence discarded "
                      "and the radio is quarantined");
        if (stats != NULL) {
            stats->wifi_native_error = ESP_ERR_TIMEOUT;
        }
        /*
         * A second quarantine would mean the invariant broke: a new session cannot
         * start while the first is alive, so this is a defect, not a state to
         * absorb. Reported rather than silently overwriting the older handle,
         * which would leak the tracker the older task still writes to.
         */
        if (s_quarantine.wifi != NULL) {
            ESP_LOGE(TAG, "wifi quarantine already held; keeping the older session");
            session = NULL;
            tracker = NULL;
        } else {
            s_quarantine.wifi = session;
            s_wifi_quarantine_state = APP_SCAN_RF_QUARANTINE_HELD;
            session = NULL;
            tracker = NULL; /* owned by the quarantined session from here on */
        }
    }

    /*
     * Reduce the outcome to a stage verdict through the host-tested policy, and
     * hand it to the caller. A stage that timed out or could not be shut down must
     * never be recorded as DONE, so the verdict travels with the result instead of
     * the caller inferring "success" from a return code.
     */
    {
        app_scan_rf_outcome_t outcome;

        memset(&outcome, 0, sizeof(outcome));
        outcome.stop_confirmed = stopped;
        outcome.canceled = result.canceled;
        outcome.timed_out = !stopped;
        outcome.native_error = result.native_error;
        outcome.collected = (uint32_t)ev->wifi_count;
        if (stats != NULL) {
            stats->wifi_verdict = app_scan_evaluate_rf_stage(&outcome);
            stats->wifi_verdict_set = true;
        }
    }

    if (result.native_error != ESP_OK) {
        err = result.native_error;
        goto restore;
    }
    err = stopped ? ESP_OK : ESP_ERR_TIMEOUT;

restore:
    /*
     * 5. Give the driver back to the STA owner - but only when it is ours to give.
     *
     * `stopped` is false in two different situations, and they need opposite
     * treatment:
     *
     *   - a session was started and its task refused to exit. That task still owns
     *     the Wi-Fi driver and will call esp_wifi_stop()/esp_wifi_deinit() when it
     *     finally does, so restoring STA here would initialise the driver
     *     underneath it and let the old task tear the new one down. The radio is
     *     quarantined instead: the STA owner is told not to touch it, and the next
     *     Wi-Fi stage refuses to start until the task has exited.
     *   - no session ever started (tracker creation, session start or the
     *     release-for-scan itself failed). Nobody holds the driver, so it is
     *     restored exactly as before. Quarantining here would strand the network
     *     for a failure that never took the radio.
     *
     * Quarantining is not a lost capability: the reconnect happens in
     * release_quarantined_sessions() the moment the task is gone, which is the
     * earliest point at which it is safe.
     */
    if (stopped || s_quarantine.wifi == NULL) {
        esp_err_t restore_err = wifi_mgr_restore_after_scan();
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "wifi restore after scan failed: %s",
                     esp_err_to_name(restore_err));
        }
    } else {
        (void)wifi_mgr_quarantine();
    }

    if (session != NULL) {
        kismet_wifi_session_destroy(session);
    }
    if (tracker != NULL) {
        kismet_wifi_tracker_destroy(tracker);
    }
    return err;
}

/* ================= BLE RF ================= */

static void ble_report_cb(const kismet_ble_report_t *report, void *ctx)
{
    app_scan_evidence_t *ev = (app_scan_evidence_t *)ctx;
    wireshark_ble_adv_t parsed;
    app_scan_ble_t obs;

    if (ev == NULL || report == NULL) {
        return;
    }

    memset(&obs, 0, sizeof(obs));
    obs.generation = ev->generation;
    memcpy(obs.address, report->address, 6);
    obs.address_type = report->address_type;
    obs.rssi_last = report->rssi;
    obs.connectable = report->connectable;
    obs.first_seen_ms = report->seen_ms;
    obs.last_seen_ms = report->seen_ms;
    obs.seen_count = 1u;
    obs.original_len = report->original_len;
    obs.captured_len = report->captured_len;
    obs.truncated = report->truncated;

    memset(&parsed, 0, sizeof(parsed));
    if (report->captured_len > 0u &&
        wireshark_ble_adv_parse(report->bytes, report->captured_len, &parsed) ==
            WIRESHARK_PARSE_OK) {
        obs.has_parsed_adv = true;
        obs.adv.valid = true;
        memcpy(obs.adv.address, report->address, 6);
        obs.adv.address_type = report->address_type;
        obs.adv.truncated = parsed.diag.truncated || parsed.name_truncated;

        if (parsed.name_present) {
            size_t len = parsed.name_len;
            if (len >= sizeof(obs.adv.name)) {
                len = sizeof(obs.adv.name) - 1u;
            }
            obs.adv.name_present = true;
            memcpy(obs.adv.name, parsed.name, len);
            obs.adv.name[len] = '\0';
        }
        if (parsed.flags_present) {
            obs.adv.flags_present = true;
            obs.adv.flags = parsed.flags;
        }
        if (parsed.tx_power_present) {
            obs.adv.tx_power_present = true;
            obs.adv.tx_power_dbm = parsed.tx_power_dbm;
        }
        if (parsed.appearance_present) {
            obs.adv.appearance_present = true;
            obs.adv.appearance = parsed.appearance;
        }
        if (parsed.uuid16_count > 0u) {
            uint8_t n = parsed.uuid16_count;
            if (n > APP_SCAN_MAX_UUID16) {
                n = APP_SCAN_MAX_UUID16;
            }
            obs.adv.uuid16_count = n;
            memcpy(obs.adv.uuid16, parsed.uuid16, (size_t)n * sizeof(uint16_t));
        }
        obs.adv.service_data_count = parsed.service_data_count;
        obs.adv.manufacturer_data_count = parsed.manufacturer_data_count;
        if (parsed.manufacturer_data_count > 0u) {
            obs.adv.first_company_id = parsed.manufacturer_data[0].company_id;
        }
    }

    (void)app_scan_ingest_ble(ev, &obs);
}

esp_err_t app_scan_native_ble_rf(app_scan_evidence_t *ev,
                                 const app_scan_native_config_t *config,
                                 app_scan_native_stats_t *stats)
{
    app_scan_native_config_t cfg;
    kismet_ble_tracker_t *tracker = NULL;
    kismet_ble_session_t *session = NULL;
    kismet_ble_tracker_config_t tracker_cfg;
    kismet_ble_session_config_t session_cfg;
    kismet_ble_session_result_t result;
    esp_err_t err;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Reclaim anything a previous failed teardown left behind, and refuse to take
     * the NimBLE host while another task still owns it. A quarantined BLE session
     * will call nvs_flash and nimble_port_deinit() when it finally exits, so a new
     * session started now would have its host torn down underneath it.
     */
    if (!app_scan_native_radio_available()) {
        ESP_LOGE(TAG, "ble stage refused: a previous session still owns the radio");
        if (stats != NULL) {
            stats->ble_native_error = ESP_ERR_NOT_FINISHED;
            stats->ble_quarantine = s_ble_quarantine_state;
            stats->ble_quarantine_set = true;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    if (config != NULL) {
        cfg = *config;
    } else {
        app_scan_native_config_default(&cfg);
    }
    if (cfg.ble_duration_ms == 0u) {
        cfg.ble_duration_ms = SCAN_BLE_DEFAULT_DURATION_MS;
    }
    if (cfg.ble_max_devices == 0u) {
        cfg.ble_max_devices = SCAN_BLE_DEFAULT_DEVICES;
    }
    if (stats != NULL) {
        stats->ble_queue_drops = 0u;
        stats->ble_canceled = false;
        stats->ble_native_error = ESP_OK;
    }

    memset(&tracker_cfg, 0, sizeof(tracker_cfg));
    tracker_cfg.max_devices = cfg.ble_max_devices;

    /*
     * The Kismet session owns nvs_flash_init + nimble_port_init..deinit for its
     * bounded duration. The application must NOT initialise NimBLE here - and the
     * radio guard above is what makes that true even after a failed teardown: a
     * quarantined session still owns the NimBLE host and will deinit it.
     */
    err = kismet_ble_tracker_create(&tracker_cfg, &tracker);
    if (err != ESP_OK) {
        return err;
    }

    memset(&session_cfg, 0, sizeof(session_cfg));
    session_cfg.duration_ms = cfg.ble_duration_ms;
    session_cfg.report_copy_len = 255u;

    err = kismet_ble_session_start(&session_cfg, tracker, ble_report_cb, ev, &session);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "kismet ble session start failed: %s", esp_err_to_name(err));
        kismet_ble_tracker_destroy(tracker);
        return err;
    }

    if (s_cancel) {
        (void)kismet_ble_session_cancel(session);
    }

    bool stopped = false;
    err = kismet_ble_session_wait(session, cfg.ble_duration_ms + 5000u);
    if (err == ESP_OK) {
        stopped = true;
    } else {
        (void)kismet_ble_session_cancel(session);
        err = kismet_ble_session_wait(session, 5000u);
        if (err == ESP_OK) {
            stopped = true;
        } else {
            ESP_LOGE(TAG, "kismet ble session did not stop in time");
        }
    }

    memset(&result, 0, sizeof(result));
    (void)kismet_ble_session_get_result(session, &result);

    if (stats != NULL) {
        stats->ble_queue_drops = result.report_queue_drops;
        stats->ble_canceled = result.canceled;
        stats->ble_native_error = result.native_error;
        stats->frames_truncated += result.truncated_reports;
    }

    /*
     * Tear down before publishing anything.
     *
     * destroy_checked() waits a bounded time for the session task to exit and
     * reports whether it did. The report callback and the tracker are written from
     * that task, so only a confirmed stop makes the evidence safe to use; without
     * it the stage is FAILED with unusable evidence rather than a thin success.
     *
     * The tracker must NOT be destroyed when the stop is unconfirmed. It is the
     * object the still-running task writes into: freeing it here would be a
     * use-after-free in another task's stack, and it is exactly the corruption the
     * bounded wait exists to avoid. Instead the session and its tracker move to
     * quarantine together, are reclaimed once the task is gone, and the next BLE
     * stage refuses to start until then - because that task also owns the NimBLE
     * host, which it will tear down after any new session has brought it up.
     */
    stopped = kismet_ble_session_destroy_checked(session) == ESP_OK;

    if (stopped) {
        kismet_ble_tracker_destroy(tracker);
        tracker = NULL;
        session = NULL;
    } else if (s_quarantine.ble != NULL) {
        /* Two quarantined sessions would break the single-owner invariant. Report
         * it and keep the older handle rather than overwriting it, which would
         * leak the tracker that older task still writes to. */
        ESP_LOGE(TAG, "ble quarantine already held; keeping the older session");
        session = NULL;
        tracker = NULL;
    } else {
        s_quarantine.ble = session;
        s_ble_tracker_in_quarantine = tracker;
        s_ble_quarantine_state = APP_SCAN_RF_QUARANTINE_HELD;
        session = NULL;
        tracker = NULL; /* both owned by the quarantine from here on */
    }

    {
        app_scan_rf_outcome_t outcome;

        memset(&outcome, 0, sizeof(outcome));
        outcome.stop_confirmed = stopped;
        outcome.canceled = result.canceled;
        outcome.timed_out = !stopped;
        outcome.native_error = result.native_error;
        outcome.collected = (uint32_t)ev->ble_count;
        if (stats != NULL) {
            stats->ble_verdict = app_scan_evaluate_rf_stage(&outcome);
            stats->ble_verdict_set = true;
            stats->ble_quarantine = s_ble_quarantine_state;
            stats->ble_quarantine_set = true;
        }
    }

    if (!stopped) {
        ESP_LOGE(TAG, "ble session did not stop within the bound; stage failed, "
                      "the NimBLE host stays quarantined");
        return ESP_ERR_TIMEOUT;
    }
    return result.native_error;
}

/* ================= LAN stages ================= */

/*
 * Caller-owned collection for the port scan.
 *
 * The Nmap port callback runs on the Nmap scan task, so the endpoints array must
 * be owned by a frame that outlives the scan. The worker blocks in
 * nmap_port_scan_wait() and only reads the collection after the scan has
 * finished, which is the same ordering discipline used for the RF stages.
 */
typedef struct {
    nmap_service_endpoint_t *endpoints;
    uint16_t capacity;
    uint16_t count;
    uint16_t overflows;
} nmap_port_collect_t;

static void nmap_port_cb(const nmap_port_result_t *result, void *ctx)
{
    nmap_port_collect_t *collect = (nmap_port_collect_t *)ctx;

    if (collect == NULL || result == NULL) {
        return;
    }
    /* Only open ports are worth probing, and only ports the service prober can
     * actually identify. */
    if (result->state != NMAP_PORT_OPEN) {
        return;
    }
    if (collect->count >= collect->capacity) {
        collect->overflows++;
        return;
    }
    collect->endpoints[collect->count].target = result->target;
    collect->endpoints[collect->count].port = result->port;
    /* PASSIVE avoids sending anything beyond connecting; it is enough to classify
     * a service that greets first (SSH, SMTP, HTTP banners). */
    collect->endpoints[collect->count].probe_profile = NMAP_SERVICE_PROBE_PASSIVE;
    collect->count++;
}

static void nmap_service_cb(const nmap_service_result_t *result, void *ctx)
{
    app_scan_evidence_t *ev = (app_scan_evidence_t *)ctx;
    app_scan_lan_t obs;
    char ipv4[APP_SCAN_MAX_IPV4];
    uint32_t addr;

    if (ev == NULL || result == NULL) {
        return;
    }
    addr = result->target.addr_be;
    (void)snprintf(ipv4, sizeof(ipv4), "%u.%u.%u.%u",
                   (unsigned)((addr >> 24) & 0xFFu), (unsigned)((addr >> 16) & 0xFFu),
                   (unsigned)((addr >> 8) & 0xFFu), (unsigned)(addr & 0xFFu));

    memset(&obs, 0, sizeof(obs));
    obs.generation = ev->generation;
    (void)app_strlcpy(obs.ipv4, ipv4, sizeof(obs.ipv4));
    obs.from_nmap = true;
    obs.up = true;
    obs.service_count = 1u;
    /* A service name is only recorded when the prober identified one. An
     * unidentified open port still counts as a service, but no name is invented
     * for it. */
    if (result->service_name[0] != '\0') {
        (void)app_strlcpy(obs.service, result->service_name, sizeof(obs.service));
    }
    obs.first_seen_ms = now_ms();
    obs.last_seen_ms = obs.first_seen_ms;
    (void)app_scan_ingest_lan(ev, &obs);
}

static void lan_note_mdns(app_scan_evidence_t *ev, const ha_mdns_service_t *service)
{
    app_scan_lan_t obs;

    if (service->address_count == 0u) {
        /* No address: cannot key a LAN device. Counted as rejected by the
         * evidence store rather than guessed onto another host. */
        (void)app_scan_ingest_lan(ev, &(app_scan_lan_t){0});
        return;
    }

    for (uint8_t i = 0u; i < service->address_count; ++i) {
        const char *addr = service->addresses[i];
        /* Only IPv4 results can be matched against the LAN evidence tables. */
        bool ipv4 = true;
        int dots = 0;
        for (const char *p = addr; *p != '\0'; ++p) {
            if (*p == '.') {
                dots++;
            } else if (*p < '0' || *p > '9') {
                ipv4 = false;
                break;
            }
        }
        if (!ipv4 || dots != 3) {
            continue;
        }

        memset(&obs, 0, sizeof(obs));
        obs.generation = ev->generation;
        (void)app_strlcpy(obs.ipv4, addr, sizeof(obs.ipv4));
        obs.from_mdns = true;
        obs.up = true;
        obs.service_count = 1u;
        (void)app_strlcpy(obs.hostname, service->hostname, sizeof(obs.hostname));
        (void)app_strlcpy(obs.service, service->service_type, sizeof(obs.service));
        obs.first_seen_ms = now_ms();
        obs.last_seen_ms = obs.first_seen_ms;
        (void)app_scan_ingest_lan(ev, &obs);
    }
}

esp_err_t app_scan_native_mdns(app_scan_evidence_t *ev,
                               const app_scan_native_config_t *config,
                               app_scan_native_stats_t *stats)
{
    ha_mdns_discover_options_t options;
    ha_mdns_service_t services[SCAN_MDNS_MAX_SERVICES];
    size_t count = 0u;
    ha_discovery_status_t status;
    uint32_t timeout;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_mgr_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }

    timeout = (config != NULL && config->mdns_timeout_ms != 0u)
                  ? config->mdns_timeout_ms
                  : SCAN_MDNS_DEFAULT_TIMEOUT_MS;

    memset(&options, 0, sizeof(options));
    options.timeout_ms = timeout;
    options.cancelled = cancel_cb;
    options.cancel_context = NULL;

    memset(services, 0, sizeof(services));
    status = ha_mdns_discover_once(&options, services,
                                   sizeof(services) / sizeof(services[0]), &count);
    if (status == HA_DISCOVERY_NETWORK_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == HA_DISCOVERY_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0u; i < count; ++i) {
        lan_note_mdns(ev, &services[i]);
    }
    if (stats != NULL) {
        stats->lan_services += (uint32_t)count;
    }
    return ESP_OK;
}

esp_err_t app_scan_native_ssdp(app_scan_evidence_t *ev,
                               const app_scan_native_config_t *config,
                               app_scan_native_stats_t *stats)
{
    ha_ssdp_discover_options_t options;
    ha_ssdp_service_t services[SCAN_SSDP_MAX_SERVICES];
    size_t count = 0u;
    ha_discovery_status_t status;
    uint32_t timeout;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_mgr_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }

    timeout = (config != NULL && config->ssdp_timeout_ms != 0u)
                  ? config->ssdp_timeout_ms
                  : SCAN_SSDP_DEFAULT_TIMEOUT_MS;

    memset(&options, 0, sizeof(options));
    options.timeout_ms = timeout;
    options.cancelled = cancel_cb;
    options.cancel_context = NULL;

    memset(services, 0, sizeof(services));
    status = ha_ssdp_discover_once(&options, services,
                                   sizeof(services) / sizeof(services[0]), &count);
    if (status == HA_DISCOVERY_NETWORK_ERROR) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == HA_DISCOVERY_CANCELLED) {
        return ESP_ERR_INVALID_STATE;
    }

    /* SSDP answers carry LOCATION (a URL), not a bare address. The LOCATION host
     * is the only address we can key on; when it has no parseable IPv4 host the
     * result is retained as a service count only and no device is invented. */
    if (stats != NULL) {
        stats->lan_services += (uint32_t)count;
    }
    return ESP_OK;
}

/* Nmap host results arrive on the Nmap scan task. */
static void nmap_host_cb(const nmap_host_result_t *result, void *ctx)
{
    app_scan_evidence_t *ev = (app_scan_evidence_t *)ctx;
    app_scan_lan_t obs;
    char ipv4[APP_SCAN_MAX_IPV4];
    uint32_t addr;

    if (ev == NULL || result == NULL) {
        return;
    }
    if (result->state != NMAP_HOST_UP) {
        return;
    }
    addr = result->target.addr_be;

    (void)snprintf(ipv4, sizeof(ipv4), "%u.%u.%u.%u",
                   (unsigned)((addr >> 24) & 0xFFu), (unsigned)((addr >> 16) & 0xFFu),
                   (unsigned)((addr >> 8) & 0xFFu), (unsigned)(addr & 0xFFu));

    memset(&obs, 0, sizeof(obs));
    obs.generation = ev->generation;
    (void)app_strlcpy(obs.ipv4, ipv4, sizeof(obs.ipv4));
    obs.from_nmap = true;
    obs.up = true;
    obs.first_seen_ms = now_ms();
    obs.last_seen_ms = obs.first_seen_ms;
    (void)app_scan_ingest_lan(ev, &obs);
}

esp_err_t app_scan_native_lan_hosts(app_scan_evidence_t *ev,
                                    const app_scan_native_config_t *config,
                                    app_scan_native_stats_t *stats)
{
    nmap_discovery_config_t cfg;
    nmap_scan_handle_t scan = NULL;
    nmap_scan_summary_t summary;
    uint32_t timeout;
    esp_err_t err;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_mgr_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }

    timeout = (config != NULL && config->nmap_timeout_ms != 0u)
                  ? config->nmap_timeout_ms
                  : SCAN_NMAP_DEFAULT_TIMEOUT_MS;

    memset(&cfg, 0, sizeof(cfg));
    cfg.use_local_subnet = true;
    cfg.enable_icmp = false; /* avoid requiring raw-ICMP privileges */
    cfg.timing = nmap_timing_policy_default();
    cfg.timing.scan_timeout_ms = timeout;
    cfg.result_cb = nmap_host_cb;
    cfg.user_ctx = ev;

    err = nmap_discovery_start(&cfg, &scan);
    if (err != ESP_OK) {
        return err;
    }

    if (s_cancel) {
        (void)nmap_discovery_cancel(scan);
    }

    err = nmap_discovery_wait(scan, timeout + 2000u);
    memset(&summary, 0, sizeof(summary));
    (void)nmap_scan_get_summary(scan, &summary);

    if (stats != NULL) {
        stats->lan_hosts += summary.completed_jobs;
    }

    if (err == ESP_ERR_TIMEOUT) {
        (void)nmap_discovery_cancel(scan);
        (void)nmap_discovery_wait(scan, 2000u);
        return ESP_ERR_TIMEOUT;
    }
    return summary.terminal_error;
}

esp_err_t app_scan_native_lan_services(app_scan_evidence_t *ev,
                                       const app_scan_native_config_t *config,
                                       app_scan_native_stats_t *stats)
{
    /*
     * Service discovery for hosts the previous stage already found.
     *
     * WHAT THIS ACTUALLY DOES ON THE NETWORK - stated precisely, because
     * "passive" is a term of art and this stage is not passive in the RF sense:
     *
     *   - it performs an ACTIVE TCP connect() to each selected port. That is
     *     observable traffic: the peer sees an inbound connection attempt, and so
     *     does anything monitoring the path. It is therefore a "bounded local
     *     host/service probe" under the product rules, not a passive observation
     *     and not comparable to the Kismet RF stages.
     *   - it does NOT send any protocol payload. The nmap component's
     *     NMAP_SERVICE_PROBE_PASSIVE profile reads a bounded banner and never
     *     calls send(); only NMAP_SERVICE_PROBE_HTTP_HEAD would, and that profile
     *     is deliberately not requested here. So there is no HTTP request, no
     *     credential attempt and no protocol-specific probe payload.
     *   - the identification is therefore best-effort: a service that speaks only
     *     after being prompted will stay unidentified, and that is accepted
     *     rather than escalating to an active probe.
     *
     * Every bound the product requires is applied here:
     *
     *   - targets come only from hosts LAN_HOSTS already observed as up, capped at
     *     SCAN_NMAP_MAX_HOSTS, with addresses parsed by the address parser rather
     *     than by a printf format;
     *   - a fixed, short list of well-known management/service ports, capped at
     *     SCAN_NMAP_MAX_PORTS;
     *   - a bounded capture per service, so a chatty peer cannot make us read
     *     indefinitely;
     *   - one shared deadline across the port and service halves, so the whole
     *     stage is finite, with cancellation checked between the halves;
     *   - a host with no open port is not an error, and having nothing to probe
     *     returns success.
     */
    static const uint16_t k_probe_ports[SCAN_NMAP_MAX_PORTS] = {
        80u,   /* HTTP */
        443u,  /* HTTPS */
        22u,   /* SSH */
        23u,   /* Telnet */
        6053u, /* ESPHome Native API */
        1883u, /* MQTT */
        554u,  /* RTSP */
        9100u, /* raw printing */
    };

    nmap_ipv4_target_t targets[SCAN_NMAP_MAX_HOSTS];
    nmap_service_endpoint_t endpoints[SCAN_NMAP_MAX_ENDPOINTS];
    nmap_port_scan_config_t port_cfg;
    nmap_service_scan_config_t service_cfg;
    nmap_scan_handle_t scan = NULL;
    nmap_scan_summary_t summary;
    uint32_t timeout;
    uint32_t deadline;
    uint16_t target_count = 0u;
    uint16_t endpoint_count = 0u;
    esp_err_t err;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_mgr_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ev->lan_count == 0u) {
        /* Nothing to probe. Not a failure: LAN_HOSTS may simply have found no
         * hosts, or may not have run at all. */
        return ESP_OK;
    }

    timeout = (config != NULL && config->nmap_timeout_ms != 0u)
                  ? config->nmap_timeout_ms
                  : SCAN_NMAP_DEFAULT_TIMEOUT_MS;
    deadline = now_ms() + timeout;

    /* Collect bounded targets from the evidence already gathered.
     *
     * The address is parsed with esp_netif_str_to_ip4() rather than sscanf: it
     * accepts exactly dotted-quad IPv4 and rejects anything else, reports its own
     * error instead of relying on a count, and avoids the uint32_t vs unsigned int
     * mismatch that a "%u" scanf has. */
    for (size_t i = 0u; i < ev->lan_count && target_count < SCAN_NMAP_MAX_HOSTS; ++i) {
        esp_ip4_addr_t parsed;

        if (esp_netif_str_to_ip4(ev->lan[i].ipv4, &parsed) != ESP_OK) {
            continue;
        }
        if (parsed.addr == 0u) {
            /* 0.0.0.0 is not a probe target. */
            continue;
        }
        targets[target_count].addr_be = parsed.addr;
        ++target_count;
    }
    if (target_count == 0u) {
        return ESP_OK;
    }

    /* ---- 1. which of the well-known ports are actually open ------------ */
    {
        nmap_port_collect_t collect;

        memset(&collect, 0, sizeof(collect));
        collect.endpoints = endpoints;
        collect.capacity = SCAN_NMAP_MAX_ENDPOINTS;

        memset(&port_cfg, 0, sizeof(port_cfg));
        port_cfg.targets = targets;
        port_cfg.target_count = target_count;
        port_cfg.ports = k_probe_ports;
        port_cfg.port_count = (uint16_t)(sizeof(k_probe_ports) / sizeof(k_probe_ports[0]));
        port_cfg.timing = nmap_timing_policy_default();
        port_cfg.timing.scan_timeout_ms = timeout / 2u;
        port_cfg.result_cb = nmap_port_cb;
        port_cfg.user_ctx = &collect;

        err = nmap_port_scan_start(&port_cfg, &scan);
        if (err != ESP_OK) {
            return err;
        }
        if (s_cancel) {
            (void)nmap_port_scan_cancel(scan);
        }
        err = nmap_port_scan_wait(scan, timeout / 2u + 2000u);
        memset(&summary, 0, sizeof(summary));
        (void)nmap_scan_get_summary(scan, &summary);

        if (err == ESP_ERR_TIMEOUT) {
            (void)nmap_port_scan_cancel(scan);
            (void)nmap_port_scan_wait(scan, 2000u);
            return ESP_ERR_TIMEOUT;
        }
        if (summary.terminal_error != ESP_OK) {
            return summary.terminal_error;
        }

        endpoint_count = collect.count;
    }

    if (endpoint_count == 0u) {
        /* No open well-known ports anywhere: nothing further to probe. */
        return ESP_OK;
    }
    if (s_cancel || now_ms() >= deadline) {
        return ESP_ERR_TIMEOUT;
    }

    /* ---- 2. bounded service identification on those endpoints ---------- */
    memset(&service_cfg, 0, sizeof(service_cfg));
    service_cfg.endpoints = endpoints;
    service_cfg.endpoint_count = endpoint_count;
    service_cfg.capture_bytes = SCAN_SERVICE_CAPTURE_BYTES;
    service_cfg.timing = nmap_timing_policy_default();
    service_cfg.timing.scan_timeout_ms = timeout / 2u;
    service_cfg.result_cb = nmap_service_cb;
    service_cfg.user_ctx = ev;

    err = nmap_service_scan_start(&service_cfg, &scan);
    if (err != ESP_OK) {
        return err;
    }
    if (s_cancel) {
        (void)nmap_service_scan_cancel(scan);
    }
    err = nmap_service_scan_wait(scan, timeout / 2u + 2000u);
    memset(&summary, 0, sizeof(summary));
    (void)nmap_scan_get_summary(scan, &summary);

    if (err == ESP_ERR_TIMEOUT) {
        (void)nmap_service_scan_cancel(scan);
        (void)nmap_service_scan_wait(scan, 2000u);
        return ESP_ERR_TIMEOUT;
    }
    return summary.terminal_error;
}
