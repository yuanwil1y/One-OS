#pragma once

/*
 * Private test hooks for the application binding table.
 *
 * Included by firmware/main/app_device.c only, and by nothing else. The functions are
 * compiled into that translation unit only when APP_DEVICE_TEST_HOOKS is defined, so
 * they do not exist in the firmware image at all - not merely unreferenced.
 *
 * They live here rather than in include/app_device.h because production code must not
 * be able to place a writable binding without recognition. A test needs one entity
 * with one exact capability set; the materialiser needs a whole evidence set, a
 * recognizer and a database to produce the same thing, which would make a control-loop
 * test a materialiser test.
 */

#include <stdbool.h>
#include <stdint.h>

#include "app_device.h"

void app_device_test_bind_entity(const char *entity_id, const char *device_id,
                                 uint8_t backend, bool has_range, int32_t min_value,
                                 int32_t max_value);

void app_device_test_mark_availability(const char *device_id,
                                       app_availability_t availability);

void app_device_test_forget_entity(const char *entity_id);
