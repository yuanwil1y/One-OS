#include "smoke_tests.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_ieee802154.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#define BLE_SCAN_MS 3000u
#define I154_DWELL_MS 80u
#define SD_TEST_PATH BOARD_SD_MOUNT_POINT "/one_os_smoke.txt"
#define SD_TEST_PAYLOAD "One-OS v0.1.0-beta.1 smoke test\n"

static bool s_nvs_initialized;
static bool s_netif_initialized;
static bool s_event_loop_ready;

static SemaphoreHandle_t s_ble_sync_sem;
static SemaphoreHandle_t s_ble_done_sem;
static volatile bool s_ble_sync_ok;
static volatile uint32_t s_ble_report_count;
static uint8_t s_ble_own_addr_type;

static volatile uint32_t s_i154_frame_count;
static bool s_i154_callback_registered;

static esp_err_t ensure_nvs(void)
{
    if (s_nvs_initialized) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        s_nvs_initialized = true;
    }
    return err;
}

static esp_err_t ensure_wifi_platform(void)
{
    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_netif_initialized) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_netif_initialized = true;
    }

    if (!s_event_loop_ready) {
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_event_loop_ready = true;
    }

    return ESP_OK;
}

static esp_err_t run_wifi(smoke_test_result_t *result)
{
    esp_err_t err = ensure_wifi_platform();
    if (err != ESP_OK) {
        return err;
    }

    bool initialized = false;
    bool started = false;
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }
    initialized = true;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        goto cleanup;
    }
    started = true;

    const wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        goto cleanup;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err == ESP_OK) {
        result->primary = ap_count;
        (void)snprintf(result->detail, sizeof(result->detail), "%u APs", (unsigned)ap_count);
    }

cleanup:
    if (started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (err == ESP_OK && stop_err != ESP_OK) {
            err = stop_err;
        }
    }
    if (initialized) {
        esp_err_t deinit_err = esp_wifi_deinit();
        if (err == ESP_OK && deinit_err != ESP_OK) {
            err = deinit_err;
        }
    }
    return err;
}

static void ble_on_reset(int reason)
{
    (void)reason;
    s_ble_sync_ok = false;
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    }
    s_ble_sync_ok = rc == 0;
    if (s_ble_sync_sem != NULL) {
        xSemaphoreGive(s_ble_sync_sem);
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event == NULL) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC) {
        ++s_ble_report_count;
    }
#if MYNEWT_VAL(BLE_EXT_ADV)
    else if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        ++s_ble_report_count;
    }
#endif
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (s_ble_done_sem != NULL) {
            xSemaphoreGive(s_ble_done_sem);
        }
    }
    return 0;
}

static esp_err_t run_ble(smoke_test_result_t *result)
{
    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) {
        return err;
    }

    s_ble_sync_sem = xSemaphoreCreateBinary();
    s_ble_done_sem = xSemaphoreCreateBinary();
    if (s_ble_sync_sem == NULL || s_ble_done_sem == NULL) {
        if (s_ble_sync_sem != NULL) {
            vSemaphoreDelete(s_ble_sync_sem);
            s_ble_sync_sem = NULL;
        }
        if (s_ble_done_sem != NULL) {
            vSemaphoreDelete(s_ble_done_sem);
            s_ble_done_sem = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    s_ble_sync_ok = false;
    s_ble_report_count = 0;

    err = nimble_port_init();
    if (err != ESP_OK) {
        goto cleanup_sems;
    }

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    if (xSemaphoreTake(s_ble_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE || !s_ble_sync_ok) {
        err = ESP_ERR_TIMEOUT;
        goto stop_host;
    }

    int rc;
#if MYNEWT_VAL(BLE_EXT_ADV)
    struct ble_gap_ext_disc_params params = {0};
    params.passive = 1;
    rc = ble_gap_ext_disc(s_ble_own_addr_type,
                          (uint16_t)(BLE_SCAN_MS / 10u),
                          0,
                          0,
                          0,
                          0,
                          &params,
                          &params,
                          ble_gap_event,
                          NULL);
#else
    struct ble_gap_disc_params params = {0};
    params.passive = 1;
    params.filter_duplicates = 0;
    params.filter_policy = 0;
    params.limited = 0;
    rc = ble_gap_disc(s_ble_own_addr_type,
                      (int32_t)BLE_SCAN_MS,
                      &params,
                      ble_gap_event,
                      NULL);
#endif
    if (rc != 0) {
        err = ESP_FAIL;
        goto stop_host;
    }

    if (xSemaphoreTake(s_ble_done_sem, pdMS_TO_TICKS(BLE_SCAN_MS + 2000u)) != pdTRUE) {
        (void)ble_gap_disc_cancel();
        err = ESP_ERR_TIMEOUT;
        goto stop_host;
    }

    result->primary = s_ble_report_count;
    (void)snprintf(result->detail,
                   sizeof(result->detail),
                   "%lu reports",
                   (unsigned long)s_ble_report_count);
    err = ESP_OK;

stop_host: {
        int stop_rc = nimble_port_stop();
        if (err == ESP_OK && stop_rc != 0) {
            err = ESP_FAIL;
        }
        int deinit_rc = nimble_port_deinit();
        if (err == ESP_OK && deinit_rc != ESP_OK) {
            err = deinit_rc;
        }
    }

cleanup_sems:
    if (s_ble_sync_sem != NULL) {
        vSemaphoreDelete(s_ble_sync_sem);
        s_ble_sync_sem = NULL;
    }
    if (s_ble_done_sem != NULL) {
        vSemaphoreDelete(s_ble_done_sem);
        s_ble_done_sem = NULL;
    }
    return err;
}

static void IRAM_ATTR i154_rx_done(uint8_t *frame,
                                   esp_ieee802154_frame_info_t *frame_info)
{
    (void)frame_info;
    if (frame == NULL) {
        return;
    }

    ++s_i154_frame_count;
    (void)esp_ieee802154_receive_handle_done(frame);
}

static esp_err_t ensure_i154_callback(void)
{
    if (s_i154_callback_registered) {
        return ESP_OK;
    }

    const esp_ieee802154_event_cb_list_t callbacks = {
        .rx_done_cb = i154_rx_done,
    };
    esp_err_t err = esp_ieee802154_event_callback_list_register(callbacks);
    if (err == ESP_OK) {
        s_i154_callback_registered = true;
    }
    return err;
}

static esp_err_t run_i154(smoke_test_result_t *result)
{
    esp_err_t err = ensure_i154_callback();
    if (err != ESP_OK) {
        return err;
    }

    s_i154_frame_count = 0;
    uint32_t channels_completed = 0;

    for (uint8_t channel = 11; channel <= 26; ++channel) {
        bool enabled = false;
        bool receiving = false;
        bool promiscuous = false;

        err = esp_ieee802154_enable();
        if (err != ESP_OK) {
            break;
        }
        enabled = true;

        err = esp_ieee802154_set_channel(channel);
        if (err == ESP_OK) {
            err = esp_ieee802154_set_promiscuous(true);
            if (err == ESP_OK) {
                promiscuous = true;
            }
        }
        if (err == ESP_OK) {
            err = esp_ieee802154_receive();
            if (err == ESP_OK) {
                receiving = true;
                vTaskDelay(pdMS_TO_TICKS(I154_DWELL_MS));
            }
        }

        if (promiscuous) {
            esp_err_t prom_err = esp_ieee802154_set_promiscuous(false);
            if (err == ESP_OK && prom_err != ESP_OK) {
                err = prom_err;
            }
        }
        if (receiving) {
            esp_err_t sleep_err = esp_ieee802154_sleep();
            if (err == ESP_OK && sleep_err != ESP_OK) {
                err = sleep_err;
            }
        }
        if (enabled) {
            esp_err_t disable_err = esp_ieee802154_disable();
            if (err == ESP_OK && disable_err != ESP_OK) {
                err = disable_err;
            }
        }

        if (err != ESP_OK) {
            break;
        }
        ++channels_completed;
    }

    result->primary = channels_completed;
    result->secondary = s_i154_frame_count;
    if (err == ESP_OK) {
        (void)snprintf(result->detail,
                       sizeof(result->detail),
                       "%lu ch, %lu frames",
                       (unsigned long)channels_completed,
                       (unsigned long)s_i154_frame_count);
    }
    return err;
}

static esp_err_t sd_rw_on_mounted(void)
{
    static const char payload[] = SD_TEST_PAYLOAD;
    char readback[sizeof(payload)] = {0};

    FILE *fp = fopen(SD_TEST_PATH, "wb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    const size_t payload_len = sizeof(payload) - 1u;
    if (fwrite(payload, 1, payload_len, fp) != payload_len ||
        fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        (void)fclose(fp);
        (void)remove(SD_TEST_PATH);
        return ESP_FAIL;
    }
    if (fclose(fp) != 0) {
        (void)remove(SD_TEST_PATH);
        return ESP_FAIL;
    }

    fp = fopen(SD_TEST_PATH, "rb");
    if (fp == NULL) {
        (void)remove(SD_TEST_PATH);
        return ESP_FAIL;
    }

    const size_t read_len = fread(readback, 1, payload_len, fp);
    const int close_rc = fclose(fp);
    const bool matches = read_len == payload_len &&
                         memcmp(readback, payload, payload_len) == 0;
    (void)remove(SD_TEST_PATH);

    return matches && close_rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t run_sd_rw(smoke_test_result_t *result)
{
    esp_err_t err = board_sd_mount(false);
    if (err != ESP_OK) {
        return err;
    }

    err = sd_rw_on_mounted();
    esp_err_t unmount_err = board_sd_unmount();
    if (err == ESP_OK && unmount_err != ESP_OK) {
        err = unmount_err;
    }

    if (err == ESP_OK) {
        (void)snprintf(result->detail, sizeof(result->detail), "write/read verified");
    }
    return err;
}

static esp_err_t run_sd_format(smoke_test_result_t *result)
{
    esp_err_t err = board_sd_mount(false);
    if (err == ESP_OK) {
        err = board_sd_format_fat();
    } else {
        /* User confirmed destructive formatting; allow ESP-IDF to create FAT. */
        err = board_sd_mount(true);
    }

    if (err == ESP_OK) {
        err = sd_rw_on_mounted();
    }

    esp_err_t unmount_err = board_sd_unmount();
    if (err == ESP_OK && unmount_err != ESP_OK) {
        err = unmount_err;
    }

    if (err == ESP_OK) {
        (void)snprintf(result->detail, sizeof(result->detail), "format + R/W verified");
    }
    return err;
}

esp_err_t smoke_test_run(smoke_test_kind_t kind, smoke_test_result_t *out_result)
{
    if (out_result == NULL || kind >= SMOKE_TEST_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->kind = kind;

    esp_err_t err;
    switch (kind) {
    case SMOKE_TEST_WIFI:
        err = run_wifi(out_result);
        break;
    case SMOKE_TEST_BLE:
        err = run_ble(out_result);
        break;
    case SMOKE_TEST_I154:
        err = run_i154(out_result);
        break;
    case SMOKE_TEST_SD_RW:
        err = run_sd_rw(out_result);
        break;
    case SMOKE_TEST_SD_FORMAT:
        err = run_sd_format(out_result);
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    out_result->err = err;
    return err;
}
