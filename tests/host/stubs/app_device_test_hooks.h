#pragma once

/*
 * Test-only hooks into the application binding table.
 *
 * WHY THIS EXISTS
 *
 * A writable entity is created by exactly one production path: recognition resolved a
 * recipe, the profile declared it writable, and the backend is one this firmware can
 * drive. Tests for the control loop need an entity with one exact capability set, and
 * building a full evidence set plus a recognizer plus a database for each one would
 * make those tests about the materialiser instead of about the control loop.
 *
 * So this header declares the three placements a test needs, and it is compiled only
 * into host test builds:
 *
 *   - app_device_test_bind_entity()      place one entity binding directly
 *   - app_device_test_mark_availability() drive one device to ONLINE or UNAVAILABLE
 *   - app_device_test_forget_entity()    remove one binding, as a sweep would
 *
 * It is deliberately NOT in include/app_device.h. Production code must not be able to
 * create a writable binding without recognition, and a declaration that only test
 * translation units include cannot be called by accident from firmware - whereas one
 * in the public header would be available to every caller for ever.
 *
 * The functions live in tests/host/stubs/app_device_test_hooks.c, which is linked only
 * by the host groups that need them. If it is ever linked into firmware, that is the
 * signal that something reached for a shortcut.
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_device.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Place or replace one entity binding.
 *
 * `writable` is derived from the service mask the caller installed in ha_core, so a
 * test cannot create an entity that claims to be controllable while advertising no
 * service - the inconsistency the control loop's admission checks exist to catch.
 */
void app_device_test_bind_entity(const char *entity_id, const char *device_id,
                                 uint8_t backend, bool has_range, int32_t min_value,
                                 int32_t max_value);

/* Drive one device's availability, exactly as a generation finish would. */
void app_device_test_mark_availability(const char *device_id,
                                       app_availability_t availability);

/* Remove one entity binding without touching ha_core, as an eviction would. */
void app_device_test_forget_entity(const char *entity_id);

#ifdef __cplusplus
}
#endif
