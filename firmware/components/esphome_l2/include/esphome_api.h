#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ESPHOME_API_DEFAULT_PORT 6053u
#define ESPHOME_API_MAX_FRAME_BYTES 1024u
#define ESPHOME_API_MAX_HOST_LEN 63u
#define ESPHOME_API_MAX_NAME_LEN 120u
#define ESPHOME_API_MAX_OBJECT_ID_LEN 120u
#define ESPHOME_API_MAX_DEVICE_CLASS_LEN 47u
#define ESPHOME_API_MAX_UNIT_LEN 63u
#define ESPHOME_API_MAX_STATE_TEXT_LEN 120u
#define ESPHOME_API_SESSION_BYTES 1280u
typedef enum { ESPHOME_API_PROTOCOL_ERROR_NONE=0,ESPHOME_API_PROTOCOL_ERROR_IO,ESPHOME_API_PROTOCOL_ERROR_MALFORMED,ESPHOME_API_PROTOCOL_ERROR_OVERSIZED,ESPHOME_API_PROTOCOL_ERROR_ENCRYPTION_REQUIRED,ESPHOME_API_PROTOCOL_ERROR_NOISE_NOT_SUPPORTED,ESPHOME_API_PROTOCOL_ERROR_AUTH_REQUIRED,ESPHOME_API_PROTOCOL_ERROR_UNEXPECTED_MESSAGE,ESPHOME_API_PROTOCOL_ERROR_PEER_DISCONNECTED } esphome_api_protocol_error_t;
typedef struct { const char *host; uint16_t port; uint32_t timeout_ms; uint16_t max_frame_bytes; const uint8_t *noise_psk; size_t noise_psk_len; } esphome_api_config_t;
typedef struct { uint32_t api_version_major,api_version_minor; char server_info[33],name[32],mac_address[18],esphome_version[33],compilation_time[26],model[128],manufacturer[21],friendly_name[121],project_name[128],project_version[128]; bool api_encryption_supported; } esphome_api_probe_result_t;
typedef enum { ESPHOME_API_ENTITY_UNKNOWN=0,ESPHOME_API_ENTITY_BINARY_SENSOR,ESPHOME_API_ENTITY_SENSOR,ESPHOME_API_ENTITY_SWITCH,ESPHOME_API_ENTITY_TEXT_SENSOR,ESPHOME_API_ENTITY_LIGHT,ESPHOME_API_ENTITY_NUMBER,ESPHOME_API_ENTITY_SELECT,ESPHOME_API_ENTITY_LOCK,ESPHOME_API_ENTITY_BUTTON } esphome_api_entity_kind_t;
typedef struct { esphome_api_entity_kind_t kind; uint32_t key,device_id; char object_id[121],name[121],device_class[48],unit[64]; uint32_t entity_category; bool disabled_by_default; union { struct{bool is_status;} binary_sensor; struct{int32_t accuracy_decimals;uint32_t state_class;} sensor; struct{bool assumed_state;} switch_; struct{float min_value,max_value,step;uint32_t mode;} number; struct{uint16_t option_count;} select; struct{bool assumed_state,supports_open,requires_code;} lock; struct{float min_mireds,max_mireds;uint32_t color_modes_mask;} light; } detail; } esphome_api_entity_t;
typedef struct { esphome_api_entity_t *items; size_t capacity,count,total_seen,unsupported_seen; bool truncated; } esphome_api_entity_list_t;
typedef struct { esphome_api_entity_kind_t kind; uint32_t key,device_id; bool missing; union { bool boolean; float number; char text[121]; struct{bool state;float brightness;uint32_t color_mode;float red,green,blue,white,color_temperature;} light; uint32_t enum_value; } value; } esphome_api_state_t;
typedef void (*esphome_api_state_fn)(const esphome_api_state_t*,void*);
typedef enum { ESPHOME_API_COMMAND_SWITCH=0,ESPHOME_API_COMMAND_LIGHT,ESPHOME_API_COMMAND_BUTTON,ESPHOME_API_COMMAND_NUMBER,ESPHOME_API_COMMAND_SELECT,ESPHOME_API_COMMAND_LOCK } esphome_api_command_kind_t;
typedef struct { esphome_api_command_kind_t kind; uint32_t key,device_id; union { struct{bool state;} switch_; struct{bool has_state,state,has_brightness;float brightness;} light; struct{float state;} number; struct{const char*state;} select; struct{uint32_t command;const char*code;} lock; } value; } esphome_api_command_t;
typedef union { max_align_t _align; uint8_t _opaque[ESPHOME_API_SESSION_BYTES]; } esphome_api_session_t;
esp_err_t esphome_api_init(esphome_api_session_t*,const esphome_api_config_t*); void esphome_api_deinit(esphome_api_session_t*);
esp_err_t esphome_api_probe(esphome_api_session_t*,esphome_api_probe_result_t*); esp_err_t esphome_api_entities(esphome_api_session_t*,esphome_api_entity_list_t*); esp_err_t esphome_api_subscribe(esphome_api_session_t*,esphome_api_state_fn,void*); esp_err_t esphome_api_poll(esphome_api_session_t*,uint32_t); esp_err_t esphome_api_command(esphome_api_session_t*,const esphome_api_command_t*); esp_err_t esphome_api_close(esphome_api_session_t*); void esphome_api_cancel(esphome_api_session_t*); bool esphome_api_is_connected(const esphome_api_session_t*); esphome_api_protocol_error_t esphome_api_last_protocol_error(const esphome_api_session_t*);
#ifdef __cplusplus
}
#endif
