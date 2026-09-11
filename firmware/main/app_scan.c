/*
 * Application scan policy and bounded evidence store.
 *
 * Platform independent: compiled into the ESP32-C6 firmware and into the host
 * regression tests. No ESP-IDF/FreeRTOS/LVGL headers may be included here.
 */

#include "app_scan.h"
#include "app_str.h"

#include <string.h>

/* ---------------- helpers ---------------- */

static bool mac_equal(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool ipv4_equal(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* Oldest evidence is the eviction candidate: recognition-independent, so an
 * unknown device is never treated as less valuable than a known one. */
static size_t oldest_wifi_index(const app_scan_evidence_t *ev)
{
    size_t oldest = 0u;
    for (size_t i = 1u; i < ev->wifi_count; ++i) {
        if (ev->wifi[i].last_seen_ms < ev->wifi[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    return oldest;
}

static size_t oldest_ble_index(const app_scan_evidence_t *ev)
{
    size_t oldest = 0u;
    for (size_t i = 1u; i < ev->ble_count; ++i) {
        if (ev->ble[i].last_seen_ms < ev->ble[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    return oldest;
}

static size_t oldest_lan_index(const app_scan_evidence_t *ev)
{
    size_t oldest = 0u;
    for (size_t i = 1u; i < ev->lan_count; ++i) {
        if (ev->lan[i].last_seen_ms < ev->lan[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    return oldest;
}

/* ---------------- RF stage verdict ---------------- */

app_scan_rf_verdict_t app_scan_evaluate_rf_stage(const app_scan_rf_outcome_t *outcome)
{
    app_scan_rf_verdict_t verdict;

    memset(&verdict, 0, sizeof(verdict));

    if (outcome == NULL) {
        verdict.terminal_state = APP_STAGE_STATE_FAILED;
        verdict.evidence_usable = false;
        verdict.reason = "no_outcome";
        return verdict;
    }

    /*
     * Teardown first. If the session could not be shut down, its task may still
     * be writing the evidence store, so nothing about this stage can be trusted -
     * neither its evidence nor a claim that it covered the protocol.
     */
    if (!outcome->stop_confirmed) {
        verdict.terminal_state = APP_STAGE_STATE_FAILED;
        verdict.evidence_usable = false;
        verdict.reason = "session_stop_unconfirmed";
        return verdict;
    }

    /* The session is genuinely gone, so whatever it collected is safe to use. */
    verdict.evidence_usable = true;

    if (outcome->canceled) {
        /* Cancellation is a normal bounded outcome, and it means coverage was
         * truncated: the stage records that rather than pretending DONE. */
        verdict.terminal_state = APP_STAGE_STATE_CANCELED;
        verdict.reason = "canceled";
        return verdict;
    }

    if (outcome->native_error != ESP_OK) {
        /* Something was collected but the stage did not complete cleanly. With no
         * evidence at all this is a failure rather than a thin result. */
        verdict.terminal_state = outcome->collected > 0u ? APP_STAGE_STATE_PARTIAL
                                                        : APP_STAGE_STATE_FAILED;
        verdict.reason = "native_error";
        return verdict;
    }

    if (outcome->timed_out) {
        /* The stage ended on its own deadline. It may have collected plenty, but
         * coverage is incomplete, so PARTIAL - never DONE. */
        verdict.terminal_state = outcome->collected > 0u ? APP_STAGE_STATE_PARTIAL
                                                        : APP_STAGE_STATE_FAILED;
        verdict.reason = "stage_timeout";
        return verdict;
    }

    verdict.terminal_state = APP_STAGE_STATE_DONE;
    verdict.reason = NULL;
    return verdict;
}

bool app_scan_state_covers_protocol(app_stage_state_t state)
{
    /* Only a stage that ran to completion can support the claim "if it were
     * there, we would have seen it". PARTIAL, FAILED, SKIPPED and CANCELED all
     * leave open the possibility that the protocol was not fully observed. */
    return state == APP_STAGE_STATE_DONE;
}

/* ---------------- lifecycle ---------------- */

void app_scan_evidence_reset(app_scan_evidence_t *ev, uint32_t generation)
{
    if (ev == NULL) {
        return;
    }
    memset(ev, 0, sizeof(*ev));
    ev->generation = generation;
}

/* ---------------- stage planning ---------------- */

app_scan_stage_plan_t app_scan_plan_stage(const app_scan_inputs_t *inputs,
                                          app_scan_stage_t stage)
{
    app_scan_stage_plan_t plan;

    memset(&plan, 0, sizeof(plan));

    if (inputs == NULL) {
        plan.action = APP_STAGE_ACTION_REJECTED;
        plan.terminal_state = APP_STAGE_STATE_FAILED;
        plan.reason = "no_inputs";
        return plan;
    }

    if (inputs->canceled) {
        plan.action = APP_STAGE_ACTION_SKIP;
        plan.terminal_state = APP_STAGE_STATE_CANCELED;
        plan.reason = "canceled";
        return plan;
    }

    switch (stage) {
    case APP_STAGE_WIFI_RF:
        if (!inputs->wifi_driver_acquired) {
            /* The Kismet session refused to take the driver, or the handover
             * failed. Nothing was listened to, so this must not look "done". */
            plan.action = APP_STAGE_ACTION_REJECTED;
            plan.terminal_state = APP_STAGE_STATE_FAILED;
            plan.reason = "wifi_driver_unavailable";
            return plan;
        }
        plan.action = APP_STAGE_ACTION_RUN;
        plan.terminal_state = APP_STAGE_STATE_PENDING;
        plan.reason = NULL;
        return plan;

    case APP_STAGE_BLE_RF:
        if (!inputs->ble_available) {
            plan.action = APP_STAGE_ACTION_UNAVAILABLE;
            plan.terminal_state = APP_STAGE_STATE_SKIPPED;
            plan.reason = "ble_backend_unavailable";
            return plan;
        }
        plan.action = APP_STAGE_ACTION_RUN;
        plan.terminal_state = APP_STAGE_STATE_PENDING;
        plan.reason = NULL;
        return plan;

    case APP_STAGE_MDNS:
    case APP_STAGE_SSDP:
    case APP_STAGE_LAN_HOSTS:
    case APP_STAGE_LAN_SERVICES:
        if (!inputs->has_ip) {
            /* No IPv4 address: LAN discovery is not applicable. RF results from
             * earlier stages stay valid; only these stages are skipped. */
            plan.action = APP_STAGE_ACTION_SKIP;
            plan.terminal_state = APP_STAGE_STATE_SKIPPED;
            plan.reason = inputs->wifi_configured ? "no_ip" : "wifi_unconfigured";
            return plan;
        }
        plan.action = APP_STAGE_ACTION_RUN;
        plan.terminal_state = APP_STAGE_STATE_PENDING;
        plan.reason = NULL;
        return plan;

    case APP_STAGE_THREAD:
    case APP_STAGE_ZIGBEE:
        /* Native backends are not wired yet. Report them as skipped so a scan
         * never implies these protocols were covered. */
        plan.action = APP_STAGE_ACTION_UNAVAILABLE;
        plan.terminal_state = APP_STAGE_STATE_SKIPPED;
        plan.reason = (stage == APP_STAGE_THREAD) ? "thread_backend_unavailable"
                                                  : "zigbee_backend_unavailable";
        return plan;

    case APP_STAGE_ENRICHMENT:
    case APP_STAGE_MATERIALIZE:
        /* These stages are always applicable: they work on whatever evidence
         * was collected, including "nothing". */
        plan.action = APP_STAGE_ACTION_RUN;
        plan.terminal_state = APP_STAGE_STATE_PENDING;
        plan.reason = NULL;
        return plan;

    default:
        plan.action = APP_STAGE_ACTION_SKIP;
        plan.terminal_state = APP_STAGE_STATE_SKIPPED;
        plan.reason = "unknown_stage";
        return plan;
    }
}

/* ---------------- Wi-Fi evidence ---------------- */

app_scan_wifi_t *app_scan_find_wifi(app_scan_evidence_t *ev, const uint8_t bssid[6])
{
    if (ev == NULL || bssid == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < ev->wifi_count; ++i) {
        if (mac_equal(ev->wifi[i].bssid, bssid)) {
            return &ev->wifi[i];
        }
    }
    return NULL;
}

bool app_scan_ingest_wifi(app_scan_evidence_t *ev, const app_scan_wifi_t *obs)
{
    app_scan_wifi_t *existing;

    if (ev == NULL || obs == NULL) {
        return false;
    }
    if (obs->ssid_len > APP_SCAN_MAX_SSID) {
        ev->drops.rej_malformed++;
        return false;
    }

    existing = app_scan_find_wifi(ev, obs->bssid);
    if (existing != NULL) {
        /* Same BSSID: merge, never duplicate. Keep the richest SSID. */
        existing->rssi_last = obs->rssi_last;
        existing->channel = obs->channel;
        existing->role |= obs->role;
        existing->channel_bitmap |= obs->channel_bitmap;
        if (obs->last_seen_ms > existing->last_seen_ms) {
            existing->last_seen_ms = obs->last_seen_ms;
        }
        if (obs->first_seen_ms != 0u &&
            (existing->first_seen_ms == 0u || obs->first_seen_ms < existing->first_seen_ms)) {
            existing->first_seen_ms = obs->first_seen_ms;
        }
        if (obs->seen_count > existing->seen_count) {
            existing->seen_count = obs->seen_count;
        }
        if (obs->has_ssid && !existing->has_ssid) {
            existing->has_ssid = true;
            existing->ssid_hidden = obs->ssid_hidden;
            existing->ssid_len = obs->ssid_len;
            memcpy(existing->ssid, obs->ssid, obs->ssid_len);
        }
        return true;
    }

    if (ev->wifi_count >= APP_SCAN_MAX_WIFI) {
        size_t victim = oldest_wifi_index(ev);
        ev->wifi[victim] = *obs;
        ev->drops.device_evictions++;
        return true;
    }

    ev->wifi[ev->wifi_count++] = *obs;
    return true;
}

/* ---------------- BLE evidence ---------------- */

app_scan_ble_t *app_scan_find_ble(app_scan_evidence_t *ev, const uint8_t address[6],
                                  uint8_t address_type)
{
    if (ev == NULL || address == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < ev->ble_count; ++i) {
        /* Address *and* address type identify a BLE peer: the same bytes with a
         * different type are different devices. */
        if (ev->ble[i].address_type == address_type &&
            mac_equal(ev->ble[i].address, address)) {
            return &ev->ble[i];
        }
    }
    return NULL;
}

bool app_scan_ingest_ble(app_scan_evidence_t *ev, const app_scan_ble_t *obs)
{
    app_scan_ble_t *existing;

    if (ev == NULL || obs == NULL) {
        return false;
    }
    if (obs->captured_len > APP_SCAN_MAX_ADV_BYTES) {
        ev->drops.rej_malformed++;
        return false;
    }

    existing = app_scan_find_ble(ev, obs->address, obs->address_type);
    if (existing != NULL) {
        existing->rssi_last = obs->rssi_last;
        existing->connectable = obs->connectable;
        if (obs->last_seen_ms > existing->last_seen_ms) {
            existing->last_seen_ms = obs->last_seen_ms;
        }
        if (obs->first_seen_ms != 0u &&
            (existing->first_seen_ms == 0u || obs->first_seen_ms < existing->first_seen_ms)) {
            existing->first_seen_ms = obs->first_seen_ms;
        }
        existing->seen_count += obs->seen_count > 0u ? obs->seen_count : 1u;
        /* A later advertisement may carry the parsed structures the first one
         * lacked (scan responses usually do). Prefer a parsed copy. */
        if (!existing->has_parsed_adv && obs->has_parsed_adv) {
            existing->has_parsed_adv = true;
            existing->adv = obs->adv;
        }
        if (obs->original_len > existing->original_len) {
            existing->original_len = obs->original_len;
        }
        if (obs->captured_len > existing->captured_len) {
            existing->captured_len = obs->captured_len;
        }
        existing->truncated = obs->truncated;
        return true;
    }

    if (ev->ble_count >= APP_SCAN_MAX_BLE) {
        size_t victim = oldest_ble_index(ev);
        ev->ble[victim] = *obs;
        ev->drops.device_evictions++;
        return true;
    }

    ev->ble[ev->ble_count++] = *obs;
    return true;
}

bool app_scan_store_ble_adv(app_scan_ble_adv_t *slots, size_t capacity,
                            size_t *used, const app_scan_ble_adv_t *adv,
                            uint32_t *out_evictions)
{
    if (slots == NULL || used == NULL || adv == NULL || capacity == 0u) {
        return false;
    }
    if (*used >= capacity) {
        if (out_evictions != NULL) {
            (*out_evictions)++;
        }
        return false;
    }
    slots[*used] = *adv;
    (*used)++;
    return true;
}

/* ---------------- LAN evidence ---------------- */

app_scan_lan_t *app_scan_find_lan(app_scan_evidence_t *ev, const char *ipv4)
{
    if (ev == NULL || ipv4 == NULL || ipv4[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0u; i < ev->lan_count; ++i) {
        if (ipv4_equal(ev->lan[i].ipv4, ipv4)) {
            return &ev->lan[i];
        }
    }
    return NULL;
}

bool app_scan_ingest_lan(app_scan_evidence_t *ev, const app_scan_lan_t *obs)
{
    app_scan_lan_t *existing;

    if (ev == NULL || obs == NULL) {
        return false;
    }
    if (obs->ipv4[0] == '\0') {
        /* Without an address there is no LAN device to key on. An mDNS/SSDP
         * result with no address is not silently merged into an arbitrary
         * host: it is rejected and counted. */
        ev->drops.rej_malformed++;
        return false;
    }

    existing = app_scan_find_lan(ev, obs->ipv4);
    if (existing != NULL) {
        /* Merging is by IP only within one scan. IP is not a stable physical
         * identity across scans, so it is never used to merge across
         * generations; the Device layer keeps that rule. */
        existing->from_mdns = existing->from_mdns || obs->from_mdns;
        existing->from_ssdp = existing->from_ssdp || obs->from_ssdp;
        existing->from_nmap = existing->from_nmap || obs->from_nmap;
        existing->up = existing->up || obs->up;
        if (obs->hostname[0] != '\0' && existing->hostname[0] == '\0') {
            (void)app_strlcpy(existing->hostname, obs->hostname, sizeof(existing->hostname));
        }
        if (obs->service[0] != '\0' && existing->service[0] == '\0') {
            (void)app_strlcpy(existing->service, obs->service, sizeof(existing->service));
        }
        if (obs->service_count > 0u) {
            existing->service_count += obs->service_count;
        }
        if (obs->last_seen_ms > existing->last_seen_ms) {
            existing->last_seen_ms = obs->last_seen_ms;
        }
        if (obs->first_seen_ms != 0u &&
            (existing->first_seen_ms == 0u || obs->first_seen_ms < existing->first_seen_ms)) {
            existing->first_seen_ms = obs->first_seen_ms;
        }
        return true;
    }

    if (ev->lan_count >= APP_SCAN_MAX_LAN) {
        size_t victim = oldest_lan_index(ev);
        ev->lan[victim] = *obs;
        ev->drops.lan_evictions++;
        return true;
    }

    ev->lan[ev->lan_count++] = *obs;
    return true;
}

bool app_scan_evidence_truncated(const app_scan_evidence_t *ev)
{
    if (ev == NULL) {
        return false;
    }
    return ev->drops.device_evictions > 0u || ev->drops.ble_adv_evictions > 0u ||
           ev->drops.lan_evictions > 0u || ev->drops.rej_truncated > 0u;
}
