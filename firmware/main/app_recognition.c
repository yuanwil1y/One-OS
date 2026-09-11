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
    case DEVICE_DB_BACKEND_ESPHOME_API:
    case DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE:
    case DEVICE_DB_BACKEND_ZIGBEE_COMMAND:
    case DEVICE_DB_BACKEND_MATTER_ATTRIBUTE:
    case DEVICE_DB_BACKEND_MATTER_COMMAND:
        /*
         * The control backends are not wired into the application yet. Until each
         * has a real, tested controller path, recognition must not present a
         * control for it: an entity that cannot be driven is worse than no entity,
         * because the UI would offer an action that silently does nothing.
         *
         * This one place is the switch that flips per backend as B7/B8/B9 land,
         * rather than per-family conditions scattered through the code.
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
