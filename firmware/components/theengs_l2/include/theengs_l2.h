#ifndef THEENGS_L2_H
#define THEENGS_L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THEENGS_MAX_VALUES 16u
#define THEENGS_MODEL_ID_MAX 24u
#define THEENGS_MODEL_NAME_MAX 32u
#define THEENGS_PROPERTY_NAME_MAX 24u

typedef uint16_t theengs_decoder_id_t;

enum {
    THEENGS_DECODER_RUUVI_RAW_V2 = 0x0001u,
    THEENGS_DECODER_BTHOME_V2 = 0x0002u,
};

typedef enum {
    THEENGS_STATUS_OK = 0,
    THEENGS_STATUS_PARTIAL = 1,
    THEENGS_STATUS_INVALID_ARGUMENT = -1,
    THEENGS_STATUS_UNKNOWN_DECODER = -2,
    THEENGS_STATUS_TRUNCATED_INPUT = -3,
    THEENGS_STATUS_UNSUPPORTED_PAYLOAD = -4,
    THEENGS_STATUS_RESULT_FULL = -5,
    THEENGS_STATUS_NOT_FOUND = -6,
} theengs_status_t;

typedef enum {
    THEENGS_INPUT_MANUFACTURER_DATA = 1,
    THEENGS_INPUT_SERVICE_DATA = 2,
} theengs_input_kind_t;

typedef enum {
    THEENGS_VALUE_FLOAT = 1,
    THEENGS_VALUE_INT32 = 2,
    THEENGS_VALUE_UINT32 = 3,
    THEENGS_VALUE_BOOL = 4,
    THEENGS_VALUE_ENUM = 5,
} theengs_value_type_t;

typedef enum {
    THEENGS_UNIT_NONE = 0,
    THEENGS_UNIT_CELSIUS,
    THEENGS_UNIT_PERCENT,
    THEENGS_UNIT_PASCAL,
    THEENGS_UNIT_MILLIG,
    THEENGS_UNIT_MILLIVOLT,
    THEENGS_UNIT_DBM,
    THEENGS_UNIT_COUNT,
    THEENGS_UNIT_LUX,
    THEENGS_UNIT_PPM,
} theengs_unit_t;

typedef enum {
    THEENGS_PROPERTY_TEMPERATURE_C = 1,
    THEENGS_PROPERTY_HUMIDITY_PERCENT,
    THEENGS_PROPERTY_PRESSURE_PA,
    THEENGS_PROPERTY_ACCEL_X_MG,
    THEENGS_PROPERTY_ACCEL_Y_MG,
    THEENGS_PROPERTY_ACCEL_Z_MG,
    THEENGS_PROPERTY_VOLTAGE_MV,
    THEENGS_PROPERTY_TX_POWER_DBM,
    THEENGS_PROPERTY_MOVEMENT_COUNT,
    THEENGS_PROPERTY_MEASUREMENT_SEQUENCE,
    THEENGS_PROPERTY_BATTERY_PERCENT,
    THEENGS_PROPERTY_ILLUMINANCE_LUX,
    THEENGS_PROPERTY_MOISTURE_PERCENT,
    THEENGS_PROPERTY_OPENING,
    THEENGS_PROPERTY_MOTION,
    THEENGS_PROPERTY_BUTTON_EVENT,
    THEENGS_PROPERTY_PACKET_ID,
    THEENGS_PROPERTY_CO2_PPM,
} theengs_property_id_t;

typedef enum {
    THEENGS_BUTTON_EVENT_NONE = 0x00,
    THEENGS_BUTTON_EVENT_PRESS = 0x01,
    THEENGS_BUTTON_EVENT_DOUBLE_PRESS = 0x02,
    THEENGS_BUTTON_EVENT_TRIPLE_PRESS = 0x03,
    THEENGS_BUTTON_EVENT_LONG_PRESS = 0x04,
    THEENGS_BUTTON_EVENT_LONG_DOUBLE_PRESS = 0x05,
    THEENGS_BUTTON_EVENT_LONG_TRIPLE_PRESS = 0x06,
    THEENGS_BUTTON_EVENT_HOLD_PRESS = 0x80,
} theengs_button_event_t;

/*
 * The application supplies already-normalized BLE data fields selected by
 * Device DB. service_data excludes the 16-bit Service UUID. manufacturer_data
 * excludes the Bluetooth SIG Company Identifier. No AD parsing or matching is
 * performed by this component.
 */
typedef struct {
    const uint8_t *service_data;
    size_t service_data_len;
    const uint8_t *manufacturer_data;
    size_t manufacturer_data_len;
} theengs_parsed_adv_t;

typedef struct {
    theengs_property_id_t property_id;
    uint8_t instance;
    theengs_value_type_t type;
    union {
        float f32;
        int32_t i32;
        uint32_t u32;
        bool boolean;
        uint32_t enumeration;
    } value;
} theengs_value_t;

typedef struct {
    uint8_t value_count;
    bool incomplete;
    theengs_value_t values[THEENGS_MAX_VALUES];
} theengs_decoded_values_t;

typedef struct {
    theengs_decoder_id_t decoder_id;
    theengs_input_kind_t input_kind;
    uint8_t property_count;
    char model_id[THEENGS_MODEL_ID_MAX];
    char model_name[THEENGS_MODEL_NAME_MAX];
} theengs_model_info_t;

typedef struct {
    theengs_property_id_t property_id;
    theengs_value_type_t value_type;
    theengs_unit_t unit;
    char name[THEENGS_PROPERTY_NAME_MAX];
} theengs_property_info_t;

theengs_status_t theengs_decode_by_id(theengs_decoder_id_t decoder_id,
                                      const theengs_parsed_adv_t *parsed_adv,
                                      theengs_decoded_values_t *out_values);

/* Decoder-family metadata only; physical profile identity remains Device DB owned. */
theengs_status_t theengs_model_info(theengs_decoder_id_t decoder_id,
                                    theengs_model_info_t *out_info);

theengs_status_t theengs_property_info(theengs_decoder_id_t decoder_id,
                                       theengs_property_id_t property_id,
                                       theengs_property_info_t *out_info);

#ifdef __cplusplus
}
#endif

#endif
