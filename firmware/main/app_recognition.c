/*
 * Recognition policy: database state, backend drivability, and the single
 * production path from an observation to a recognised result.
 *
 * Platform independent, and the place where three product rules are enforced:
 *
 *   1. an unmatched, ambiguous or unreadable database never removes a device - it
 *      leaves it generic and read-only;
 *   2. a database record declaring `writable` does NOT by itself create a control
 *      path: the recipe must also name a backend this firmware can drive;
 *   3. an ambiguous match is never permission to guess, so it never produces a
 *      writable binding.
 */

#include "app_recognizer.h"

#include <string.h>

/*
 * The decoder/quirk availability question is answered by the family that owns
 * the id space, so it is asked directly instead of being duplicated as a list
 * here. Two stub headers of the same name let the host test build compile this
 * file without the ESP-IDF-only family sources; see tests/host/stubs/README.md.
 * The stub is a test double and is never part of the firmware build.
 */
#include "theengs_l2.h"
#include "zha_l2.h"

const char *app_db_state_name(app_db_state_t state)
{
    switch (state) {
    case APP_DB_STATE_CLOSED:       return "closed";
    case APP_DB_STATE_READY:        return "ready";
    case APP_DB_STATE_SD_MISSING:   return "sd_missing";
    case APP_DB_STATE_FILE_MISSING: return "file_missing";
    case APP_DB_STATE_CORRUPT:      return "corrupt";
    case APP_DB_STATE_INCOMPATIBLE: return "incompatible";
    case APP_DB_STATE_IO_ERROR:     return "io_error";
    default:                        return "invalid";
    }
}

bool app_db_state_is_usable(app_db_state_t state)
{
    return state == APP_DB_STATE_READY;
}

bool app_backend_is_drivable(uint8_t backend)
{
    switch (backend) {
    case DEVICE_DB_BACKEND_NONE:
    case DEVICE_DB_BACKEND_PASSIVE_VALUE:
        /* Read-only paths: no controller is needed to satisfy them. */
        return true;
    case DEVICE_DB_BACKEND_BLE_GATT:
        /*
         * Drivable as of B7: app_ctl_ble is the controller and app_ctl_ble_gatt
         * answers the one question only the firmware can - which characteristic, of
         * which connected peer. Both have host groups (app_ctl_ble, 85 checks;
         * app_ctl_ble_gatt, 53 checks) and the GATT session underneath them has a
         * third.
         *
         * What is still NOT verified for this backend is the radio: no peer has ever
         * received a write from this firmware. That is item 5b of
         * docs/hardware-acceptance.md, and it does not change what the switch means -
         * an entity whose recipe, profile and controller all agree is controllable,
         * and one whose write is refused is reported as a failed control rather than
         * silently doing nothing.
         */
        return true;
    case DEVICE_DB_BACKEND_ESPHOME_API:
    case DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE:
    case DEVICE_DB_BACKEND_ZIGBEE_COMMAND:
    case DEVICE_DB_BACKEND_MATTER_ATTRIBUTE:
    case DEVICE_DB_BACKEND_MATTER_COMMAND:
        /*
         * Still not wired: no controller exists for these yet. Until each has a real,
         * tested controller path, recognition must not present a control for it: an
         * entity that cannot be driven is worse than no entity, because the UI would
         * offer an action that silently does nothing.
         *
         * This one place is the switch that flips per backend as B8/B9 land, rather
         * than per-family conditions scattered through the code.
         */
        return false;
    default:
        return false;
    }
}

const char *app_backend_name(uint8_t backend)
{
    switch (backend) {
    case DEVICE_DB_BACKEND_NONE:             return "none";
    case DEVICE_DB_BACKEND_PASSIVE_VALUE:    return "passive_value";
    case DEVICE_DB_BACKEND_BLE_GATT:         return "ble_gatt";
    case DEVICE_DB_BACKEND_ESPHOME_API:      return "esphome_api";
    case DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE: return "zigbee_attribute";
    case DEVICE_DB_BACKEND_ZIGBEE_COMMAND:   return "zigbee_command";
    case DEVICE_DB_BACKEND_MATTER_ATTRIBUTE: return "matter_attribute";
    case DEVICE_DB_BACKEND_MATTER_COMMAND:   return "matter_command";
    default:                                 return "unknown";
    }
}

/*
 * Domain ids.
 *
 * The database stores a numeric domain id per recipe. Mapping it here - rather
 * than reading the domain string out of the file as the entity id - is what puts
 * the entity namespace under this firmware's control. A recipe naming a domain
 * this table does not contain is refused by the reader instead of producing an
 * entity id built from an unknown string.
 *
 * The names are the Home Assistant domain strings, so a later GUI and the
 * service dispatcher see the same vocabulary the HA model uses.
 */
const char *app_domain_name(uint8_t domain_id)
{
    switch (domain_id) {
    case DEVICE_DB_DOMAIN_SENSOR:        return "sensor";
    case DEVICE_DB_DOMAIN_BINARY_SENSOR: return "binary_sensor";
    case DEVICE_DB_DOMAIN_SWITCH:        return "switch";
    case DEVICE_DB_DOMAIN_LIGHT:         return "light";
    case DEVICE_DB_DOMAIN_BUTTON:        return "button";
    case DEVICE_DB_DOMAIN_NUMBER:        return "number";
    case DEVICE_DB_DOMAIN_SELECT:        return "select";
    case DEVICE_DB_DOMAIN_CLIMATE:       return "climate";
    default:                             return NULL;
    }
}

bool app_domain_is_known(uint8_t domain_id)
{
    return app_domain_name(domain_id) != NULL;
}

/*
 * Decoder/quirk availability.
 *
 * A profile may select a Theengs decoder, a ZHA quirk, both or neither, and the
 * two id spaces are independent. The question this answers is narrow and
 * deliberate: can this firmware decode the payload of the family named here at
 * all? It is not a statement that a particular profile's recipe is complete -
 * that is the reader's job, per record.
 *
 * The lookup is a real call into the family, not a compiled-in list, so it
 * cannot drift out of sync with the family's own supported set.
 */
bool app_decoder_is_available(uint32_t decoder_id)
{
    theengs_model_info_t info;

    if (decoder_id == DEVICE_DB_NO_INDEX || decoder_id > UINT16_MAX) {
        return false;
    }
    memset(&info, 0, sizeof(info));
    return theengs_model_info((theengs_decoder_id_t)decoder_id, &info) ==
           THEENGS_STATUS_OK;
}

bool app_quirk_is_available(uint32_t quirk_id)
{
    zha_quirk_info_t info;

    if (quirk_id == DEVICE_DB_NO_INDEX) {
        return false;
    }
    memset(&info, 0, sizeof(info));
    return zha_quirk_get_info(quirk_id, &info) == ZHA_STATUS_OK;
}

void app_recognition_copy_string(char *dst, size_t dst_size,
                                 const uint8_t *src, size_t src_len);

void app_recognition_copy_string(char *dst, size_t dst_size,
                                 const uint8_t *src, size_t src_len)
{
    size_t copy_len;

    if (dst_size == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    copy_len = src_len;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1u;
    }
    if (copy_len > 0u) {
        memcpy(dst, src, copy_len);
    }
    dst[copy_len] = '\0';
}
