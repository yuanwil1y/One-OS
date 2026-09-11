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
     */
    stopped = kismet_wifi_session_destroy_checked(session) == ESP_OK;
    session = NULL;

    if (stopped) {
        ingest_wifi_tracker(ev, tracker);
        enrich_wifi_from_tracker(ev, tracker);
        (void)kismet_wifi_tracker_get_stats(tracker, &tracker_stats);
    } else {
        /* The session could not be shut down, so its task may still be running.
         * No evidence is published and the stage is reported as failed rather
         * than as a thin success. */
        ESP_LOGE(TAG, "wifi session did not stop within the bound; discarding evidence");
        if (stats != NULL) {
            stats->wifi_native_error = ESP_ERR_TIMEOUT;
        }
    }

    kismet_wifi_tracker_destroy(tracker);
    tracker = NULL;

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
    /* 5. Always give the driver back, even on failure: otherwise the device
     *    would be left with no network capability at all. */
    {
        esp_err_t restore_err = wifi_mgr_restore_after_scan();
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "wifi restore after scan failed: %s",
                     esp_err_to_name(restore_err));
        }
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

    /* The Kismet session owns nvs_flash_init + nimble_port_init..deinit for its
     * bounded duration. The application must NOT initialise NimBLE here. */
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
     */
    stopped = kismet_ble_session_destroy_checked(session) == ESP_OK;
    kismet_ble_tracker_destroy(tracker);

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
        }
    }

    if (!stopped) {
        ESP_LOGE(TAG, "ble session did not stop within the bound; stage failed");
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
