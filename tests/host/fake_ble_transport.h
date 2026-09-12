/*
 * A scripted NimBLE-level backend for the esphome_l2 GATT transport, for host
 * tests.
 *
 * The real backend (esphome_ble_gatt_nimble.c) is a thin translation of this
 * exact ops table onto NimBLE, and the transport above it - esphome_ble_gatt.c -
 * is compiled unchanged in this test. So a test that drives the transport through
 * this backend exercises the real framing, the real epoch guard, the real
 * subscription table and the real radio suspend/resume points, and only replaces
 * the radio itself.
 *
 * The backend never decides policy. It reports what it is told to report, and it
 * counts what it was asked to do, so a test can assert on the calls as well as on
 * the results.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esphome_ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* The config the transport handed to init: this is where the adapter's radio
     * pair must appear, and where a test can see the timeouts it passed. */
    esphome_ble_gatt_config_t config;
    /* Set by the transport at init. */
    void *session;

    /* Result the next call returns, per operation. */
    esp_err_t next_init;
    esp_err_t next_connect;
    esp_err_t next_discover;
    esp_err_t next_read;
    esp_err_t next_write;
    esp_err_t next_set_notify;
    esp_err_t next_cancel;
    esp_err_t next_disconnect;

    /* Native error the next failing call reports, as a real backend would. */
    int native_error;

    /* State the transport and the tests share. */
    bool connected;
    /* Mirrors the last set_notify(on) so a test can see the CCCD state without
     * reading the transport's private subscription table. */
    bool notify_active;

    /* Value the next successful read copies out, then reports the same length
     * every time, so a test can tell a real read from a stale buffer. */
    uint8_t read_value[8];
    size_t read_value_len;
    /* Length the backend claims to have written, which is what the transport
     * reports to its caller. */
    size_t read_reply_len;

    /* Last write the transport passed down, so a test can assert the bytes. */
    uint8_t written[8];
    size_t written_len;
    uint16_t written_handle;
    bool written_response;

    uint16_t last_cccd_handle;
    bool last_notify_on;
    bool last_indications;
    uint16_t last_notify_value_handle;

    uint32_t connect_timeout_seen;
    uint32_t disconnect_timeout_seen;

    /* Re-entrancy hook: when set, the read calls the session's cancel before
     * returning, which is how a test reproduces "another task cancels while a
     * read is outstanding" through the real transport and the real adapter. */
    void *cancel_session;

    int init_calls;
    int deinit_calls;
    int connect_calls;
    int discover_calls;
    int read_calls;
    int write_calls;
    int set_notify_calls;
    int cancel_calls;
    int disconnect_calls;
    int connected_queries;
} fake_ble_backend_t;

/* The ops table to hand esphome_ble_gatt_init_with_backend(). */
extern const struct esphome_ble_gatt_backend_ops fake_ble_backend_ops;

/* The live instance, which lives inside the transport's own storage. NULL before
 * the transport has been initialised and after it has been deinitialised. */
fake_ble_backend_t *fake_ble_backend_state(void);

/* Clear the counters and the last-write record, keeping the configured results
 * and the connection state. */
void fake_ble_backend_reset_counts(void);

/* Deliver a notification for `value_handle` through whatever callback the
 * transport registered, which is how a test drives the subscription path. */
void fake_ble_backend_notify(uint16_t value_handle, const uint8_t *data, size_t len);
void fake_ble_backend_notify_truncated(uint16_t value_handle, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
