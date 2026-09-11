#pragma once
/* BLE GATT central: connect, discover, read, write, subscribe, cancel, disconnect.
 *
 * THREADING AND LIFETIME CONTRACT (the caller must honour all of it):
 *
 *  - One operation at a time per session. A second call while one is in flight
 *    returns ESP_ERR_INVALID_STATE; this is not a queue.
 *  - Notification callbacks run on the backend's own task (the NimBLE host task
 *    in the ESP-IDF backend), NOT on the task that called subscribe(). They may
 *    therefore run concurrently with a call from another task.
 *  - A callback must not call back into this component. A GATT operation issued
 *    from a notification callback waits for a completion that only the task it
 *    is running on could produce, and deadlocks.
 *  - The `user` pointer passed to subscribe() must stay valid until either
 *    unsubscribe() or a disconnect()/deinit() that returns ESP_OK. A disconnect
 *    that fails or times out may leave a callback in flight, so the caller must
 *    not free that context on a failed teardown.
 *  - cancel() is the only call that is safe while an operation is outstanding.
 *    It abandons the operation: the operation reports ESP_ERR_INVALID_STATE
 *    rather than the backend's own result, because a cancel that terminates the
 *    link can make a pending operation complete with a meaningless success.
 *  - A timeout tears the link down. It is not retried here.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS 8u
#define ESPHOME_BLE_GATT_SESSION_BYTES 512u
#define ESPHOME_BLE_GATT_UUID128_BYTES 16u
typedef enum { ESPHOME_BLE_UUID_NONE=0, ESPHOME_BLE_UUID16=2, ESPHOME_BLE_UUID32=4, ESPHOME_BLE_UUID128=16 } esphome_ble_uuid_width_t;
typedef struct { esphome_ble_uuid_width_t width; uint8_t value[16]; } esphome_ble_uuid_t;
typedef struct { uint8_t address[6]; uint8_t address_type; } esphome_ble_peer_t;
typedef struct { esphome_ble_uuid_t uuid; uint16_t start_handle,end_handle,first_characteristic,characteristic_count; } esphome_ble_gatt_service_t;
typedef struct { esphome_ble_uuid_t uuid; uint16_t definition_handle,value_handle,end_handle; uint8_t properties; uint16_t first_descriptor,descriptor_count; } esphome_ble_gatt_characteristic_t;
typedef struct { esphome_ble_uuid_t uuid; uint16_t handle; } esphome_ble_gatt_descriptor_t;
typedef struct { esphome_ble_gatt_service_t *services; uint16_t service_capacity,service_count; esphome_ble_gatt_characteristic_t *characteristics; uint16_t characteristic_capacity,characteristic_count; esphome_ble_gatt_descriptor_t *descriptors; uint16_t descriptor_capacity,descriptor_count; bool truncated; } esphome_ble_gatt_db_t;
typedef esp_err_t (*esphome_ble_gatt_radio_suspend_fn)(void *user, uintptr_t *restore_token);
typedef void (*esphome_ble_gatt_radio_resume_fn)(void *user, uintptr_t restore_token);
typedef void (*esphome_ble_gatt_notify_fn)(uint16_t value_handle,const uint8_t *data,size_t data_len,bool truncated,void *user);
typedef struct { uint32_t connect_timeout_ms,operation_timeout_ms,disconnect_timeout_ms; esphome_ble_gatt_radio_suspend_fn radio_suspend; esphome_ble_gatt_radio_resume_fn radio_resume; void *radio_user; } esphome_ble_gatt_config_t;
typedef union { max_align_t _align; uint8_t _opaque[ESPHOME_BLE_GATT_SESSION_BYTES]; } esphome_ble_gatt_session_t;
esp_err_t esphome_ble_gatt_init(esphome_ble_gatt_session_t*,const esphome_ble_gatt_config_t*);
void esphome_ble_gatt_deinit(esphome_ble_gatt_session_t*);
esp_err_t esphome_ble_gatt_connect(esphome_ble_gatt_session_t*,const esphome_ble_peer_t*);
esp_err_t esphome_ble_gatt_discover(esphome_ble_gatt_session_t*,esphome_ble_gatt_db_t*);
esp_err_t esphome_ble_gatt_read(esphome_ble_gatt_session_t*,uint16_t,uint8_t*,size_t,size_t*);
esp_err_t esphome_ble_gatt_write(esphome_ble_gatt_session_t*,uint16_t,const uint8_t*,size_t,bool);
esp_err_t esphome_ble_gatt_subscribe(esphome_ble_gatt_session_t*,uint16_t,uint16_t,bool,esphome_ble_gatt_notify_fn,void*);
esp_err_t esphome_ble_gatt_unsubscribe(esphome_ble_gatt_session_t*,uint16_t);
esp_err_t esphome_ble_gatt_cancel(esphome_ble_gatt_session_t*);
esp_err_t esphome_ble_gatt_disconnect(esphome_ble_gatt_session_t*);
bool esphome_ble_gatt_is_connected(const esphome_ble_gatt_session_t*);
int esphome_ble_gatt_last_native_error(const esphome_ble_gatt_session_t*);
#ifdef __cplusplus
}
#endif
