/*
 * Native side of the provisioning portal: the two operations that touch hardware
 * directly and therefore cannot live in app_portal.c or app_http_portal.c.
 *
 *   app_portal_native_wifi_scan()  a bounded active scan for the portal's network list
 *   app_portal_native_status_fill() the parts of /api/status that come from the runtime
 *
 * WHY THE SCAN IS SEPARATE FROM THE APPLICATION SCAN
 *
 * The application scan (app_scan_native.c) is a passive RF survey that hands the Wi-Fi
 * driver to Kismet and back. The portal needs something different: a short active scan
 * that lists nearby access points so the operator can pick one. Using esp_wifi_scan_start
 * means the driver must be in STA mode and owned by the station manager, which is only
 * true while the portal is NOT holding the radio for its AP.
 *
 * So this function refuses to run while the portal's AP is up, and the portal reports an
 * empty or unavailable list rather than a wrong one. That is a real product limitation,
 * stated here rather than papered over: the operator can scan before starting the portal
 * or after the station is connected, and while the AP is up the list is unavailable.
 *
 * Platform dependent (ESP-IDF Wi-Fi). The request/response shapes it fills are not.
 */

#include "app_portal.h"

#include <stdlib.h>
#include <string.h>

#include "app_runtime.h"
#include "app_str.h"
#include "app_wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "app_portal_native";

/* Hard bound on the driver's own result list, independent of the caller's capacity:
 * the driver allocates the array, so an unbounded request would be an allocation the
 * caller cannot see. */
#define PORTAL_SCAN_MAX_AP 32u
/* Short and bounded: this is a list for a human to pick from, not a survey. */
#define PORTAL_SCAN_DWELL_MS 120u
#define PORTAL_SCAN_TIMEOUT_MS 4000u

bool app_portal_native_wifi_scan(app_portal_ap_t *out, size_t capacity,
                                 size_t *out_count)
{
    wifi_scan_config_t config;
    wifi_ap_record_t *records;
    uint16_t found = 0u;
    size_t copy;

    if (out == NULL || out_count == NULL || capacity == 0u) {
        return false;
    }
    *out_count = 0u;

    /*
     * Refused while the portal's own AP holds the radio. Scanning there would either
     * fail or, worse, disturb the AP an operator is connected to.
     */
    if (wifi_mgr_ap_is_up()) {
        ESP_LOGW(TAG, "Wi-Fi scan unavailable while the provisioning AP is up");
        return false;
    }
    /*
     * The station must already own the driver. Checking the reported state rather than
     * starting the driver here keeps the single Wi-Fi lifecycle owner intact: this
     * function never calls esp_wifi_init(), and a scan that needs the driver started is a
     * scan the portal cannot serve yet - which it says, instead of returning an empty
     * list the operator would read as "no networks in range".
     */
    if (!wifi_mgr_state_is_scannable()) {
        ESP_LOGW(TAG, "Wi-Fi scan unavailable: the station does not own the driver");
        return false;
    }

    memset(&config, 0, sizeof(config));
    config.show_hidden = true;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = PORTAL_SCAN_DWELL_MS;
    config.scan_time.active.max = PORTAL_SCAN_DWELL_MS;

    if (esp_wifi_scan_start(&config, true) != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed");
        return false;
    }
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) {
        return false;
    }

    if (found == 0u) {
        /* A genuinely empty result. Success with zero entries, not an error: "no
         * networks in range" is an answer. */
        (void)esp_wifi_clear_ap_list();
        return true;
    }
    if (found > PORTAL_SCAN_MAX_AP) {
        found = PORTAL_SCAN_MAX_AP;
    }

    /* The driver allocates this; it must be freed even on the error paths below. */
    records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * (size_t)found);
    if (records == NULL) {
        (void)esp_wifi_clear_ap_list();
        return false;
    }
    if (esp_wifi_scan_get_ap_records(&found, records) != ESP_OK) {
        free(records);
        (void)esp_wifi_clear_ap_list();
        return false;
    }

    copy = found < capacity ? found : capacity;
    for (size_t i = 0u; i < copy; ++i) {
        size_t len = strnlen((const char *)records[i].ssid, sizeof(records[i].ssid));

        /*
         * The SSID is copied as BYTES plus a length, never as a C string: an SSID is a
         * byte string that may be empty (hidden) and may not be printable, and the
         * response builder hex-encodes it so it cannot break the JSON document.
         */
        if (len > APP_PORTAL_SSID_MAX) {
            len = APP_PORTAL_SSID_MAX;
        }
        memcpy(out[i].ssid, records[i].ssid, len);
        out[i].ssid_len = (uint8_t)len;
        out[i].rssi = records[i].rssi;
        out[i].channel = records[i].primary;
        out[i].auth_mode = (uint8_t)records[i].authmode;
        out[i].hidden = len == 0u;
    }

    free(records);
    /* Releases the driver's internal list as well as the records we copied. */
    (void)esp_wifi_clear_ap_list();

    *out_count = copy;
    ESP_LOGI(TAG, "portal scan found %u networks, reported %u", (unsigned)found,
             (unsigned)copy);
    return true;
}

void app_portal_native_status_fill(void *ctx, app_portal_status_t *out)
{
    /* Static, because app_portal_status_t keeps a POINTER to the firmware string and the
     * structure outlives this call. A stack buffer here would leave the status document
     * reading freed memory. */
    static char firmware[64];
    static bool firmware_filled;

    wifi_mgr_status_t status;
    app_db_state_t db_state;
    uint32_t db_version = 0u;
    uint32_t db_profiles = 0u;

    (void)ctx;
    if (out == NULL) {
        return;
    }

    if (!firmware_filled) {
        if (app_runtime_version(firmware, sizeof(firmware)) != ESP_OK) {
            (void)app_strlcpy(firmware, "unknown", sizeof(firmware));
        }
        firmware_filled = true;
    }
    out->firmware = firmware;

    wifi_mgr_get_status(&status);
    out->sta_state = wifi_mgr_state_name(status.state);
    (void)app_strlcpy(out->sta_ssid, status.ssid, sizeof(out->sta_ssid));
    (void)app_strlcpy(out->sta_ipv4, status.ipv4, sizeof(out->sta_ipv4));

    /* The AP address comes from the transport that owns the interface, not from the
     * session's own record of what it asked for. */
    if (wifi_mgr_ap_ipv4(out->ap_ipv4, sizeof(out->ap_ipv4)) != ESP_OK) {
        out->ap_ipv4[0] = '\0';
    }

    /*
     * The recognition database state, from the runtime's own accessors - the same ones
     * the serial console uses. Two independent answers to "what is the database doing"
     * would eventually disagree, and the operator would have no way to tell which one to
     * believe.
     */
    db_state = app_runtime_db_state();
    out->db_state = app_db_state_name(db_state);
    (void)app_runtime_db_info(&db_version, &db_profiles);
    out->db_version = db_version;
    out->db_profiles = db_profiles;
}
