/*
 * Wi-Fi STA lifecycle owner.
 *
 * See app_wifi.h for the resource contract with the Kismet Wi-Fi session. This
 * file is the only place in the application that calls esp_wifi_init/stop/deinit
 * for STA connectivity.
 */

#include "app_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_wifi";

#define WIFI_NVS_NAMESPACE "nearby_wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "password"
#define WIFI_NVS_KEY_CONFIGURED "configured"

/*
 * Bounded connect wait. The station must not hold the worker forever: a scan
 * request that is waiting for the handover has to get an answer even when the
 * access point is absent.
 */
#define WIFI_CONNECT_TIMEOUT_MS 15000u
#define WIFI_DISCONNECT_TIMEOUT_MS 5000u

typedef struct {
    wifi_mgr_state_t state;
    bool credentials_present;
    bool netif_ready;
    bool event_ready;
    bool driver_up;          /* esp_wifi_init() succeeded */
    bool want_connection;    /* credentials exist and STA should hold an IP */
    bool released_for_scan;
    char ssid[WIFI_MGR_SSID_STORE];
    char ipv4[WIFI_MGR_IPV4_MAX];
    int8_t rssi;
    esp_err_t last_error;
} wifi_mgr_ctx_t;

static wifi_mgr_ctx_t s_ctx;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_events;   /* signalled on connect/disconnect/got-ip */
static esp_netif_t *s_netif;
static bool s_started_api;

/* ---------------- locking ---------------- */

static void lock_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

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

const char *wifi_mgr_state_name(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_UNINITIALIZED: return "uninitialized";
    case WIFI_MGR_UNCONFIGURED:  return "unconfigured";
    case WIFI_MGR_DISCONNECTED:  return "disconnected";
    case WIFI_MGR_CONNECTING:    return "connecting";
    case WIFI_MGR_CONNECTED:     return "connected";
    case WIFI_MGR_ERROR:         return "error";
    default:                     return "invalid";
    }
}

/* ---------------- credential storage ---------------- */

static esp_err_t load_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    size_t ssid_len = sizeof(s_ctx.ssid);
    uint8_t configured = 0;
    size_t configured_len = sizeof(configured);

    s_ctx.credentials_present = false;
    s_ctx.ssid[0] = '\0';

    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* never provisioned: normal state */
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(handle, WIFI_NVS_KEY_CONFIGURED, &configured);
    if (err == ESP_OK && configured == 1u) {
        err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, s_ctx.ssid, &ssid_len);
        if (err == ESP_OK && s_ctx.ssid[0] != '\0') {
            s_ctx.credentials_present = true;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    nvs_close(handle);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t read_stored_secret(char *out, size_t out_size)
{
    nvs_handle_t handle;
    esp_err_t err;

    out[0] = '\0';
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(handle, WIFI_NVS_KEY_PASSWORD, out, &out_size);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    bool handle_open = false;
    esp_err_t err;

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > WIFI_MGR_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }
    if (strlen(password) > WIFI_MGR_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_init();
    lock();

    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        handle_open = true;
        err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, WIFI_NVS_KEY_CONFIGURED, 1u);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle_open) {
        nvs_close(handle);
    }

    if (err == ESP_OK) {
        (void)strlcpy(s_ctx.ssid, ssid, sizeof(s_ctx.ssid));
        s_ctx.credentials_present = true;
        s_ctx.want_connection = true;
    }

    unlock();
    return err;
}

esp_err_t wifi_mgr_clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    lock_init();
    lock();

    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, WIFI_NVS_KEY_SSID);
        (void)nvs_erase_key(handle, WIFI_NVS_KEY_PASSWORD);
        (void)nvs_erase_key(handle, WIFI_NVS_KEY_CONFIGURED);
        err = nvs_commit(handle);
        nvs_close(handle);
    }

    if (err == ESP_OK) {
        s_ctx.credentials_present = false;
        s_ctx.want_connection = false;
        s_ctx.ssid[0] = '\0';
        s_ctx.ipv4[0] = '\0';
        s_ctx.state = WIFI_MGR_UNCONFIGURED;
    }

    unlock();
    return err;
}

/* ---------------- event handling ---------------- */

static void clear_ip(void)
{
    s_ctx.ipv4[0] = '\0';
    s_ctx.rssi = 0;
}

static void record_ip(esp_netif_ip_info_t *info, esp_netif_t *netif)
{
    if (info == NULL || netif == NULL) {
        return;
    }
    (void)snprintf(s_ctx.ipv4, sizeof(s_ctx.ipv4), IPSTR, IP2STR(&info->ip));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base != WIFI_EVENT) {
        if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            lock();
            record_ip((esp_netif_ip_info_t *)data, s_netif);
            s_ctx.state = WIFI_MGR_CONNECTED;
            s_ctx.last_error = ESP_OK;
            if (s_netif != NULL) {
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    s_ctx.rssi = ap.rssi;
                }
            }
            unlock();
            if (s_events != NULL) {
                (void)xSemaphoreGive(s_events);
            }
        }
        return;
    }

    switch (id) {
    case WIFI_EVENT_STA_START:
        lock();
        if (s_ctx.state != WIFI_MGR_CONNECTED) {
            s_ctx.state = WIFI_MGR_DISCONNECTED;
        }
        unlock();
        break;

    case WIFI_EVENT_STA_CONNECTED:
        lock();
        s_ctx.state = WIFI_MGR_CONNECTING;
        unlock();
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        lock();
        clear_ip();
        /* Without credentials a disconnect is the expected steady state. */
        s_ctx.state = s_ctx.credentials_present ? WIFI_MGR_DISCONNECTED
                                                : WIFI_MGR_UNCONFIGURED;
        unlock();
        if (s_events != NULL) {
            (void)xSemaphoreGive(s_events);
        }
        break;

    default:
        break;
    }
}

/* ---------------- driver lifecycle ---------------- */

static esp_err_t ensure_event_and_netif(void)
{
    esp_err_t err;

    if (!s_ctx.event_ready) {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &on_wifi_event, NULL);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         &on_wifi_event, NULL);
        if (err != ESP_OK) {
            return err;
        }
        s_ctx.event_ready = true;
    }

    if (!s_ctx.netif_ready) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == NULL) {
            return ESP_FAIL;
        }
        s_ctx.netif_ready = true;
    }

    return ESP_OK;
}

static esp_err_t driver_up(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_ctx.driver_up) {
        return ESP_OK;
    }

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        /* Roll back a partially started driver so the radio is not left in an
         * unknown state that would block a later scan handover. */
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        return err;
    }

    s_ctx.driver_up = true;
    return ESP_OK;
}

static void driver_down(void)
{
    if (!s_ctx.driver_up) {
        return;
    }
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    s_ctx.driver_up = false;
}

static esp_err_t apply_credentials_and_connect(void)
{
    wifi_config_t config;
    char password[WIFI_MGR_PASSWORD_STORE];
    esp_err_t err;

    memset(&config, 0, sizeof(config));
    (void)strlcpy((char *)config.sta.ssid, s_ctx.ssid, sizeof(config.sta.ssid));

    err = read_stored_secret(password, sizeof(password));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    (void)strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    /* Wipe the local copy as soon as the driver has taken its own. */
    memset(password, 0, sizeof(password));

    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    lock();
    s_ctx.state = WIFI_MGR_CONNECTING;
    unlock();
    return ESP_OK;
}

esp_err_t wifi_mgr_init(void)
{
    esp_err_t err;

    lock_init();
    lock();

    err = ensure_event_and_netif();
    if (err == ESP_OK) {
        err = load_credentials();
    }

    if (err == ESP_OK) {
        s_ctx.want_connection = s_ctx.credentials_present;
        s_ctx.state = s_ctx.credentials_present ? WIFI_MGR_DISCONNECTED
                                                : WIFI_MGR_UNCONFIGURED;
        s_ctx.last_error = ESP_OK;
        s_started_api = true;
    } else {
        s_ctx.state = WIFI_MGR_ERROR;
        s_ctx.last_error = err;
    }

    unlock();
    return err;
}

esp_err_t wifi_mgr_start(void)
{
    esp_err_t err = ESP_OK;

    lock_init();
    lock();

    if (!s_started_api) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.released_for_scan) {
        /* The radio belongs to a scan; do not fight it for the driver. */
        unlock();
        return ESP_OK;
    }
    if (!s_ctx.credentials_present) {
        s_ctx.state = WIFI_MGR_UNCONFIGURED;
        unlock();
        return ESP_OK;
    }

    err = ensure_event_and_netif();
    if (err == ESP_OK) {
        err = driver_up();
    }
    if (err == ESP_OK) {
        if (s_events == NULL) {
            s_events = xSemaphoreCreateBinary();
        }
        if (s_events == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        err = apply_credentials_and_connect();
    }

    if (err != ESP_OK) {
        s_ctx.state = WIFI_MGR_ERROR;
        s_ctx.last_error = err;
        unlock();
        return err;
    }

    s_ctx.want_connection = true;
    unlock();

    /* Wait outside the lock: the event handler needs it. */
    if (s_events != NULL) {
        (void)xSemaphoreTake(s_events, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    }

    lock();
    if (s_ctx.state != WIFI_MGR_CONNECTED) {
        /* Report the honest outcome instead of assuming success. */
        s_ctx.last_error = ESP_ERR_TIMEOUT;
        if (s_ctx.state != WIFI_MGR_ERROR) {
            s_ctx.state = WIFI_MGR_DISCONNECTED;
        }
    }
    err = (s_ctx.state == WIFI_MGR_CONNECTED) ? ESP_OK : ESP_ERR_TIMEOUT;
    unlock();

    return err;
}

esp_err_t wifi_mgr_release_for_scan(bool *out_was_started, bool *out_was_connected)
{
    bool was_started = false;
    bool was_connected = false;

    if (out_was_started != NULL) {
        *out_was_started = false;
    }
    if (out_was_connected != NULL) {
        *out_was_connected = false;
    }

    lock_init();
    lock();

    if (!s_started_api) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.released_for_scan) {
        /* Already released: idempotent so a retried scan does not corrupt the
         * remembered restore intent. */
        was_started = s_ctx.driver_up;
        was_connected = s_ctx.state == WIFI_MGR_CONNECTED;
        unlock();
        if (out_was_started != NULL) {
            *out_was_started = was_started;
        }
        if (out_was_connected != NULL) {
            *out_was_connected = was_connected;
        }
        return ESP_OK;
    }

    was_started = s_ctx.driver_up;
    was_connected = s_ctx.state == WIFI_MGR_CONNECTED;

    if (s_events != NULL) {
        /* Drain a stale signal so the next wait observes a fresh event. */
        (void)xSemaphoreTake(s_events, 0);
    }

    driver_down();
    clear_ip();
    s_ctx.released_for_scan = true;
    s_ctx.state = s_ctx.credentials_present ? WIFI_MGR_DISCONNECTED
                                            : WIFI_MGR_UNCONFIGURED;
    unlock();

    if (out_was_started != NULL) {
        *out_was_started = was_started;
    }
    if (out_was_connected != NULL) {
        *out_was_connected = was_connected;
    }

    ESP_LOGI(TAG, "wifi released for scan (was_started=%d was_connected=%d)",
             (int)was_started, (int)was_connected);
    return ESP_OK;
}

esp_err_t wifi_mgr_restore_after_scan(void)
{
    esp_err_t err;

    lock_init();
    lock();
    if (!s_started_api) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.released_for_scan = false;
    unlock();

    if (!s_ctx.credentials_present) {
        lock();
        s_ctx.state = WIFI_MGR_UNCONFIGURED;
        unlock();
        return ESP_OK;
    }

    /* Reconnect through the normal start path so a failed reconnect is reported
     * exactly like any other connection failure. */
    err = wifi_mgr_start();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA restore after scan failed: %s", esp_err_to_name(err));
        lock();
        /* Restore failed: the radio is usable but connectivity is not back.
         * Record it; never claim CONNECTED. */
        s_ctx.last_error = err;
        if (s_ctx.state != WIFI_MGR_ERROR) {
            s_ctx.state = WIFI_MGR_DISCONNECTED;
        }
        unlock();
    } else {
        ESP_LOGI(TAG, "wifi STA restored after scan");
    }
    return err;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    if (out == NULL) {
        return;
    }
    lock_init();
    lock();
    memset(out, 0, sizeof(*out));
    out->state = s_ctx.state;
    out->credentials_present = s_ctx.credentials_present;
    (void)strlcpy(out->ssid, s_ctx.ssid, sizeof(out->ssid));
    (void)strlcpy(out->ipv4, s_ctx.ipv4, sizeof(out->ipv4));
    out->rssi = s_ctx.rssi;
    out->last_error = s_ctx.last_error;
    out->released_for_scan = s_ctx.released_for_scan;
    unlock();
}

bool wifi_mgr_has_ip(void)
{
    bool has_ip;

    lock_init();
    lock();
    has_ip = s_ctx.state == WIFI_MGR_CONNECTED && s_ctx.ipv4[0] != '\0';
    unlock();
    return has_ip;
}
