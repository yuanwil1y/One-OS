/*
 * Firmware binding: app_ctl_ble -> app_ble_gatt. See the header for why the
 * characteristic resolution lives here and why every refusal returns false.
 *
 * The adapter is a pure translator with counters. It makes no control decision: it
 * never confirms anything, never touches the observed state, and never decides that a
 * failure is recoverable - the backend and the loop own all of that.
 */

#include "app_ctl_ble_gatt.h"

#include <string.h>

#include "app_ble_addr.h"
#include "app_str.h"

/* Bluetooth GATT specification, Characteristic Properties bit field. The transport
 * copies NimBLE's ble_gatt_chr::properties verbatim (esphome_ble_gatt_nimble.c), so
 * these are the spec's bits and not an invention. */
#define GATT_PROP_WRITE 0x08u
#define GATT_PROP_WRITE_NO_RESPONSE 0x04u

bool app_ctl_ble_gatt_property_is_writable(uint8_t properties)
{
    /*
     * Either write form counts. A characteristic that offers only Write Without
     * Response is still one the device accepts commands on - refusing it would leave
     * a working device uncontrollable, and the difference between the two forms is
     * the transport's business (app_ble_gatt_session_write takes the response flag).
     */
    return (properties & (GATT_PROP_WRITE | GATT_PROP_WRITE_NO_RESPONSE)) != 0u;
}

bool app_ctl_ble_gatt_resolve(void *ctx, const app_entity_binding_t *entity,
                              const app_device_binding_t *device, uint16_t *out_value_handle)
{
    app_ctl_ble_gatt_t *self = (app_ctl_ble_gatt_t *)ctx;
    const app_ble_db_t *db;
    uint16_t handle = 0u;
    uint32_t index;

    if (self == NULL || self->session == NULL || entity == NULL || device == NULL ||
        out_value_handle == NULL) {
        return false;
    }
    *out_value_handle = 0u;

    /*
     * Is there a link at all? The session's own phase is the only honest answer: a
     * session that is connecting or failed has a database but nothing to write over.
     */
    if (self->session->phase != APP_BLE_PHASE_READY || !self->session->connected) {
        self->resolve_refusals_not_open++;
        return false;
    }

    /*
     * Is this control for the peer we are connected to?
     *
     * Both sides are display order (see app_ble_addr.h): the session's peer came from
     * scan evidence, and the device binding's address is the display-order string the
     * device id was built from. Comparing them in one order is only safe because the
     * convention is a single one; comparing a controller-order address here would
     * match a *different* device whose bytes happen to be the reverse.
     */
    {
        uint8_t from_device[6];
        const char *id = device->device_id;
        size_t id_len;
        const char *hex;

        if (id == NULL) {
            self->resolve_refusals_wrong_peer++;
            return false;
        }
        id_len = strlen(id);
        /*
         * The device id is "ble_<type><12 hex digits>" - 16 characters of prefix,
         * which is how app_device_identity_of_ble() builds it. Anything shorter is not
         * a BLE device id and must not resolve: refusing beats reading past the end of
         * a string, and beats matching a device id from another family.
         */
        if (id_len < 12u || strncmp(id, "ble_", 4u) != 0) {
            self->resolve_refusals_wrong_peer++;
            return false;
        }
        hex = id + (id_len - 12u);

        for (size_t i = 0u; i < 6u; ++i) {
            unsigned value = 0u;

            for (size_t d = 0u; d < 2u; ++d) {
                char c = hex[i * 2u + d];

                if (c >= '0' && c <= '9') {
                    value = value * 16u + (unsigned)(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    value = value * 16u + (unsigned)(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    value = value * 16u + (unsigned)(c - 'A' + 10);
                } else {
                    self->resolve_refusals_wrong_peer++;
                    return false;
                }
            }
            from_device[i] = (uint8_t)value;
        }

        /*
         * Compared order-SENSITIVELY, and deliberately not with
         * app_ble_addr_equal().
         *
         * That helper is order-insensitive on purpose - it exists so a caller that
         * forgot to convert an address, or converted it twice, still recognises one
         * device instead of producing two records for it. That is the right tolerance
         * for deduplication and the WRONG one here: a peer whose address is the
         * connected one read backwards is a different device, and treating it as the
         * same would resolve a handle against the wrong peripheral's database. Both
         * sides are display order by convention (app_ble_addr.h), so an exact compare
         * is the honest test.
         */
        if (memcmp(from_device, self->session->peer.address, 6u) != 0) {
            self->resolve_refusals_wrong_peer++;
            return false;
        }
    }

    /*
     * The recipe's characteristic index, resolved over the DISCOVERED database. This
     * is a lookup, not a second discovery: the session ran discovery when it opened,
     * and a handle from any other source would be a guess.
     */
    index = entity->write_target_id;
    if (index == DEVICE_DB_NO_INDEX) {
        self->resolve_refusals_bad_index++;
        return false;
    }

    db = &self->session->db;
    if (app_ble_gatt_resolve(db, index, &handle, NULL) != APP_BLE_OK || handle == 0u) {
        self->resolve_refusals_bad_index++;
        return false;
    }

    /* Writable at all? Checked here so the caller learns why, instead of the radio
     * refusing an attribute the device exposes as read-only. */
    if (index >= (uint32_t)db->characteristic_count ||
        !app_ctl_ble_gatt_property_is_writable(db->characteristics[index].properties)) {
        self->resolve_refusals_not_writable++;
        return false;
    }

    *out_value_handle = handle;
    return true;
}

/* ------------------------------------------------------------------ */
/* the session calls                                                   */
/* ------------------------------------------------------------------ */

static int ctl_write(void *ctx, uint16_t handle, const uint8_t *data, size_t len)
{
    app_ctl_ble_gatt_t *self = (app_ctl_ble_gatt_t *)ctx;
    app_ble_status_t status;

    if (self == NULL || self->session == NULL) {
        return (int)APP_BLE_ERR_NOT_READY;
    }
    self->writes_attempted++;

    /*
     * With response. A control is a command the device must acknowledge at the ATT
     * layer: a write without response returns as soon as the packet is queued, so a
     * "successful" send would mean even less than usual. The observed state still
     * only moves on the device's own report - this is about knowing the write
     * arrived at all.
     */
    status = app_ble_gatt_session_write(self->session, handle, data, len, true);
    return (int)status;
}

static bool ctl_is_ready(void *ctx)
{
    app_ctl_ble_gatt_t *self = (app_ctl_ble_gatt_t *)ctx;

    if (self == NULL || self->session == NULL) {
        return false;
    }
    return self->session->phase == APP_BLE_PHASE_READY && self->session->connected;
}

static app_ble_status_t ctl_open(void *ctx, const app_ble_peer_t *peer)
{
    app_ctl_ble_gatt_t *self = (app_ctl_ble_gatt_t *)ctx;

    if (self == NULL || self->session == NULL || peer == NULL) {
        return APP_BLE_ERR_INVALID_ARGUMENT;
    }
    return app_ble_gatt_session_open(self->session, peer);
}

static app_ble_status_t ctl_read(void *ctx, uint16_t handle, uint8_t *out, size_t cap,
                                 size_t *out_len)
{
    app_ctl_ble_gatt_t *self = (app_ctl_ble_gatt_t *)ctx;

    if (self == NULL || self->session == NULL) {
        return APP_BLE_ERR_NOT_READY;
    }
    return app_ble_gatt_session_read(self->session, handle, out, cap, out_len);
}

esp_err_t app_ctl_ble_gatt_install(app_ctl_ble_gatt_t *adapter, app_ble_gatt_session_t *session,
                                   app_ctl_ble_t *out_backend)
{
    if (adapter == NULL || session == NULL || out_backend == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(adapter, 0, sizeof(*adapter));
    adapter->session = session;

    /*
     * The backend is cleared and its ops filled here, so a caller cannot end up with
     * a backend whose gatt_ctx points at one adapter and whose ops came from another -
     * a pairing that would fail at the first control with no indication why.
     */
    memset(out_backend, 0, sizeof(*out_backend));
    out_backend->gatt.write = ctl_write;
    out_backend->gatt.open = ctl_open;
    out_backend->gatt.read = ctl_read;
    out_backend->gatt.is_ready = ctl_is_ready;
    out_backend->gatt.resolve = app_ctl_ble_gatt_resolve;
    out_backend->gatt_ctx = adapter;
    /* now_ms is the caller's: the runtime passes its own clock, and a test passes a
     * controlled one. */
    out_backend->now_ms = NULL;

    return ESP_OK;
}
