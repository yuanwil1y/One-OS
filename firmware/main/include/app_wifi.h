#pragma once

/*
 * Wi-Fi STA lifecycle owner.
 *
 * Resource contract (verified against the Kismet implementation):
 *
 *   kismet_wifi_session_start() calls esp_wifi_init() itself and refuses to
 *   start when esp_wifi_get_mode() succeeds, i.e. when the driver is already
 *   initialized. It always finishes with esp_wifi_stop() + esp_wifi_deinit()
 *   and its own frame counter, so it takes and returns the *entire* Wi-Fi
 *   driver.
 *
 * That makes STA connectivity and a Kismet passive scan mutually exclusive at
 * the driver level, not merely at the scheduling level. This module is the
 * single owner of that handover:
 *
 *   scan wants the radio -> wifi_mgr_release_for_scan()
 *       disconnect, stop, deinit; the netif and event loop stay alive
 *   scan finished        -> wifi_mgr_restore_after_scan()
 *       init, STA mode, reconnect using the stored credentials
 *
 * Credentials are application data (NVS namespace `nearby_wifi`), never part of
 * the recognition database and never printed by status output.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MGR_SSID_MAX 32u
#define WIFI_MGR_PASSWORD_MAX 64u
#define WIFI_MGR_SSID_STORE (WIFI_MGR_SSID_MAX + 1u)
#define WIFI_MGR_PASSWORD_STORE (WIFI_MGR_PASSWORD_MAX + 1u)
#define WIFI_MGR_IPV4_MAX 16u

typedef enum {
    WIFI_MGR_UNINITIALIZED = 0,
    WIFI_MGR_UNCONFIGURED,   /* no stored credentials: nothing to connect to */
    WIFI_MGR_DISCONNECTED,   /* configured, driver up, not connected */
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,      /* has an IP */
    WIFI_MGR_ERROR,          /* driver or connection failure, see last_error */
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    bool credentials_present;
    char ssid[WIFI_MGR_SSID_STORE];
    char ipv4[WIFI_MGR_IPV4_MAX];
    int8_t rssi;
    esp_err_t last_error;
    /* True while the driver is deliberately released for a Kismet scan. */
    bool released_for_scan;
} wifi_mgr_status_t;

const char *wifi_mgr_state_name(wifi_mgr_state_t state);

/*
 * Prepare netif/event handling and load stored credentials. Does not start the
 * radio. Returns ESP_OK even when no credentials exist: an unprovisioned device
 * is a normal state, not an error.
 */
esp_err_t wifi_mgr_init(void);

/*
 * Bring the STA up and connect when credentials exist. Safe to call repeatedly;
 * a no-op while the driver is released for a scan.
 */
esp_err_t wifi_mgr_start(void);

/*
 * Hand the Wi-Fi driver to a Kismet scan.
 *
 * Disconnects, stops and deinitializes the driver so
 * kismet_wifi_session_start() can own it. On success the returned state is
 * `was_connected`/`was_configured` so the caller knows what to restore. The
 * call fails (and leaves the driver untouched) if the handover cannot be
 * completed cleanly, so a caller never proceeds into a scan it cannot undo.
 */
esp_err_t wifi_mgr_release_for_scan(bool *out_was_started, bool *out_was_connected);

/*
 * Restore STA ownership after a scan and reconnect when credentials exist.
 *
 * Returns ESP_OK when the driver is usable again. A failed reconnect is
 * reported through wifi_mgr_get_status() (state ERROR/DISCONNECTED plus
 * last_error); it is never reported as connected.
 */
esp_err_t wifi_mgr_restore_after_scan(void);

/* Copy the current status. Safe from any task. */
void wifi_mgr_get_status(wifi_mgr_status_t *out);

/* True when the interface currently holds an IPv4 address. */
bool wifi_mgr_has_ip(void);

/*
 * Store credentials in NVS and start using them.
 * Passwords are never returned by any getter.
 */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password);

/* Remove stored credentials. */
esp_err_t wifi_mgr_clear_credentials(void);

#ifdef __cplusplus
}
#endif
