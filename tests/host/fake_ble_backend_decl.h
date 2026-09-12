/*
 * Selects the scripted radio for the host build. Force-included by
 * tests/host/run_app_ble_native_tests.sh, so every translation unit of the test
 * binary sees it before any project header.
 *
 * Why the choice lives here and not on the command line: ESPHOME_BLE_GATT_BACKEND
 * has a default, and a -D on the command line is applied before a -include but
 * this file is itself what needs the definition ordered first. Defining it at the
 * top of the forced include is the one place that is guaranteed to come before
 * esphome_ble_gatt.c's own #include of the internal header.
 *
 * The alternative - redefining esphome_ble_gatt_init() in the test - would mean not
 * compiling the real one, and the point of this test is that the transport above
 * the backend is the unmodified production source.
 */
#define ESPHOME_BLE_GATT_BACKEND fake_ble_backend_ops

#include "esphome_ble_gatt.h"
#include "private/esphome_ble_gatt_internal.h"

extern const esphome_ble_gatt_backend_ops_t fake_ble_backend_ops;
