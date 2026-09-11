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
    /*
     * The radio is not ours to take.
     *
     * A Kismet scan whose teardown timed out still owns the Wi-Fi driver: its task
     * has not reached esp_wifi_deinit() yet, and it will. Initialising the driver
     * before that happens would let the old task deinitialise the driver a new
     * session is using, so no Wi-Fi operation may start until the task exits.
     *
     * This is a distinct state rather than ERROR because it is not a failure of
     * this module and it clears by itself: the scan side reclaims the session once
     * the task is gone and calls wifi_mgr_release_quarantine().
     */
    WIFI_MGR_QUARANTINED,
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
    /* True while the radio is held by a scan session that did not release it. */
    bool quarantined;
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
 *
 * Refuses with ESP_ERR_INVALID_STATE while the radio is quarantined, so a caller
 * that ignored the scan-side guard cannot cause esp_wifi_init() to run underneath
 * a task that is about to call esp_wifi_deinit().
 */
esp_err_t wifi_mgr_restore_after_scan(void);

/*
 * Mark the radio as held by a scan session that could not be stopped.
 *
 * Called by the scan path when a Kismet Wi-Fi session's teardown timed out. Until
 * wifi_mgr_release_quarantine() is called, every operation that would initialise
 * the driver returns ESP_ERR_INVALID_STATE and the reported state is
 * WIFI_MGR_QUARANTINED. Idempotent.
 */
esp_err_t wifi_mgr_quarantine(void);

/*
 * The quarantined session's task has exited and the radio is free again.
 *
 * Clears the quarantine and reconnects through the normal path, so the reported
 * state is never "connected" unless an address really exists. Returns
 * ESP_ERR_INVALID_STATE when no quarantine was held.
 */
esp_err_t wifi_mgr_release_quarantine(void);

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

/*
 * Temporary provisioning access point.
 *
 * The portal needs an AP for the length of one session. It is offered here rather
 * than by the portal so there is still exactly one Wi-Fi lifecycle owner: the portal
 * asks for an AP and this module decides how the driver gets there.
 *
 * Ordering contract, enforced by app_provision.c:
 *
 *   wifi_mgr_release_for_scan()   - orderly handover, driver down
 *     wifi_mgr_ap_start()         - driver up in AP mode with the given credentials
 *     wifi_mgr_ap_stop()          - driver down again
 *   wifi_mgr_restore_after_scan() - reconnect the station
 *
 * The handover is what makes this safe: the driver is not initialised when the AP
 * starts, and the station is not brought up until the AP is down. There is therefore
 * never a moment where two owners both believe they hold the driver, and the AP
 * never reconfigures a live station out from under it.
 *
 * ap_start refuses while the radio is quarantined, for the same reason every other
 * entry point does: a session whose teardown timed out still owns the driver and will
 * deinitialise it.
 */
esp_err_t wifi_mgr_ap_start(const char *ssid, const char *password);
esp_err_t wifi_mgr_ap_stop(void);

/* The soft-AP interface's IPv4 address as a string, or an empty string when the AP is
 * not up. This is what /api/status reports so the operator knows where to point a
 * browser, which is the only thing the AP address is for. */
esp_err_t wifi_mgr_ap_ipv4(char *out, size_t out_size);

/* True while the soft AP is running. */
bool wifi_mgr_ap_is_up(void);

/*
 * May an active scan be started right now?
 *
 * True only while the station owns the driver: initialised, not deliberately released
 * for a scan or for the portal's AP, and not quarantined. The provisioning portal's
 * network list uses this, because an active scan needs the driver in station mode and the
 * portal must never take it from the AP it is serving.
 */
bool wifi_mgr_state_is_scannable(void);

#ifdef __cplusplus
}
#endif
