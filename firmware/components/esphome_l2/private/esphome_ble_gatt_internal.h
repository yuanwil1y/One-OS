#pragma once
#include "esphome_ble_gatt.h"
#define ESPHOME_BLE_GATT_BACKEND_BYTES 224u
#define ESPHOME_BLE_GATT_IMPL_MAGIC 0x45424754u
typedef struct { uint16_t value_handle,cccd_handle; bool indications,active; esphome_ble_gatt_notify_fn callback; void *user; } esphome_ble_gatt_subscription_t;
typedef void (*esphome_ble_gatt_backend_notify_fn)(void*,uint16_t,const uint8_t*,size_t,bool);
typedef struct esphome_ble_gatt_backend_ops {
 esp_err_t (*init)(void*,void*,esphome_ble_gatt_backend_notify_fn,int*); void (*deinit)(void*);
 esp_err_t (*connect)(void*,const esphome_ble_peer_t*,uint32_t,int*); esp_err_t (*discover)(void*,esphome_ble_gatt_db_t*,uint32_t,int*);
 esp_err_t (*read)(void*,uint16_t,uint8_t*,size_t,size_t*,uint32_t,int*); esp_err_t (*write)(void*,uint16_t,const uint8_t*,size_t,bool,uint32_t,int*);
 esp_err_t (*set_notify)(void*,uint16_t,bool,bool,uint32_t,int*); esp_err_t (*cancel)(void*,uint32_t,int*); esp_err_t (*disconnect)(void*,uint32_t,int*); bool (*connected)(const void*);
} esphome_ble_gatt_backend_ops_t;
typedef struct { uint32_t magic; esphome_ble_gatt_config_t config; const esphome_ble_gatt_backend_ops_t *backend; alignas(max_align_t) uint8_t backend_ctx[ESPHOME_BLE_GATT_BACKEND_BYTES]; esphome_ble_gatt_subscription_t subscriptions[ESPHOME_BLE_GATT_MAX_SUBSCRIPTIONS]; uintptr_t restore_token; int last_native_error; uint32_t op_epoch,op_epoch_at_start; bool initialized,connected,radio_suspended,op_active; } esphome_ble_gatt_impl_t;
_Static_assert(sizeof(esphome_ble_gatt_impl_t)<=ESPHOME_BLE_GATT_SESSION_BYTES,"increase ESPHOME_BLE_GATT_SESSION_BYTES");
extern const esphome_ble_gatt_backend_ops_t esphome_ble_gatt_nimble_backend;
esp_err_t esphome_ble_gatt_init_with_backend(esphome_ble_gatt_session_t*,const esphome_ble_gatt_config_t*,const esphome_ble_gatt_backend_ops_t*);
