#include "theengs_l2.h"

#include <string.h>

#include "theengs_internal.h"

static uint8_t next_instance(const theengs_decoded_values_t *out,
                             theengs_property_id_t property_id)
{
    uint8_t instance = 0;
    for (uint8_t i = 0; i < out->value_count; ++i) {
        if (out->values[i].property_id == property_id) {
            ++instance;
        }
    }
    return instance;
}

static theengs_status_t reserve_value(theengs_decoded_values_t *out,
                                      theengs_property_id_t property_id,
                                      theengs_value_type_t type,
                                      theengs_value_t **slot)
{
    if (out->value_count >= THEENGS_MAX_VALUES) {
        out->incomplete = true;
        return THEENGS_STATUS_RESULT_FULL;
    }

    const uint8_t instance = next_instance(out, property_id);
    *slot = &out->values[out->value_count++];
    memset(*slot, 0, sizeof(**slot));
    (*slot)->property_id = property_id;
    (*slot)->instance = instance;
    (*slot)->type = type;
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_append_float(theengs_decoded_values_t *out,
                                      theengs_property_id_t property_id,
                                      float value)
{
    theengs_value_t *slot;
    theengs_status_t status = reserve_value(out, property_id, THEENGS_VALUE_FLOAT, &slot);
    if (status != THEENGS_STATUS_OK) return status;
    slot->value.f32 = value;
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_append_i32(theengs_decoded_values_t *out,
                                    theengs_property_id_t property_id,
                                    int32_t value)
{
    theengs_value_t *slot;
    theengs_status_t status = reserve_value(out, property_id, THEENGS_VALUE_INT32, &slot);
    if (status != THEENGS_STATUS_OK) return status;
    slot->value.i32 = value;
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_append_u32(theengs_decoded_values_t *out,
                                    theengs_property_id_t property_id,
                                    uint32_t value)
{
    theengs_value_t *slot;
    theengs_status_t status = reserve_value(out, property_id, THEENGS_VALUE_UINT32, &slot);
    if (status != THEENGS_STATUS_OK) return status;
    slot->value.u32 = value;
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_append_bool(theengs_decoded_values_t *out,
                                     theengs_property_id_t property_id,
                                     bool value)
{
    theengs_value_t *slot;
    theengs_status_t status = reserve_value(out, property_id, THEENGS_VALUE_BOOL, &slot);
    if (status != THEENGS_STATUS_OK) return status;
    slot->value.boolean = value;
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_append_enum(theengs_decoded_values_t *out,
                                     theengs_property_id_t property_id,
                                     uint32_t value)
{
    theengs_value_t *slot;
    theengs_status_t status = reserve_value(out, property_id, THEENGS_VALUE_ENUM, &slot);
    if (status != THEENGS_STATUS_OK) return status;
    slot->value.enumeration = value;
    return THEENGS_STATUS_OK;
}

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0u) return;
    size_t n = strlen(src);
    if (n >= dst_len) n = dst_len - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

theengs_status_t theengs_decode_by_id(theengs_decoder_id_t decoder_id,
                                      const theengs_parsed_adv_t *parsed_adv,
                                      theengs_decoded_values_t *out_values)
{
    if (parsed_adv == NULL || out_values == NULL) {
        return THEENGS_STATUS_INVALID_ARGUMENT;
    }

    memset(out_values, 0, sizeof(*out_values));

    switch (decoder_id) {
        case THEENGS_DECODER_RUUVI_RAW_V2:
            return theengs_decode_ruuvi_raw_v2(parsed_adv, out_values);
        case THEENGS_DECODER_BTHOME_V2:
            return theengs_decode_bthome_v2(parsed_adv, out_values);
        default:
            return THEENGS_STATUS_UNKNOWN_DECODER;
    }
}

theengs_status_t theengs_model_info(theengs_decoder_id_t decoder_id,
                                    theengs_model_info_t *out_info)
{
    if (out_info == NULL) return THEENGS_STATUS_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    out_info->decoder_id = decoder_id;

    switch (decoder_id) {
        case THEENGS_DECODER_RUUVI_RAW_V2:
            out_info->input_kind = THEENGS_INPUT_MANUFACTURER_DATA;
            out_info->property_count = 10u;
            copy_string(out_info->model_id, sizeof(out_info->model_id), "RUUVI_RAW_V2");
            copy_string(out_info->model_name, sizeof(out_info->model_name), "Ruuvi RAWv2");
            return THEENGS_STATUS_OK;
        case THEENGS_DECODER_BTHOME_V2:
            out_info->input_kind = THEENGS_INPUT_SERVICE_DATA;
            out_info->property_count = 12u;
            copy_string(out_info->model_id, sizeof(out_info->model_id), "BTHOME_V2");
            copy_string(out_info->model_name, sizeof(out_info->model_name), "BTHome v2");
            return THEENGS_STATUS_OK;
        default:
            return THEENGS_STATUS_UNKNOWN_DECODER;
    }
}

static bool property_belongs_to_decoder(theengs_decoder_id_t decoder_id,
                                        theengs_property_id_t property_id)
{
    switch (decoder_id) {
        case THEENGS_DECODER_RUUVI_RAW_V2:
            switch (property_id) {
                case THEENGS_PROPERTY_TEMPERATURE_C:
                case THEENGS_PROPERTY_HUMIDITY_PERCENT:
                case THEENGS_PROPERTY_PRESSURE_PA:
                case THEENGS_PROPERTY_ACCEL_X_MG:
                case THEENGS_PROPERTY_ACCEL_Y_MG:
                case THEENGS_PROPERTY_ACCEL_Z_MG:
                case THEENGS_PROPERTY_VOLTAGE_MV:
                case THEENGS_PROPERTY_TX_POWER_DBM:
                case THEENGS_PROPERTY_MOVEMENT_COUNT:
                case THEENGS_PROPERTY_MEASUREMENT_SEQUENCE:
                    return true;
                default:
                    return false;
            }
        case THEENGS_DECODER_BTHOME_V2:
            switch (property_id) {
                case THEENGS_PROPERTY_PACKET_ID:
                case THEENGS_PROPERTY_BATTERY_PERCENT:
                case THEENGS_PROPERTY_TEMPERATURE_C:
                case THEENGS_PROPERTY_HUMIDITY_PERCENT:
                case THEENGS_PROPERTY_PRESSURE_PA:
                case THEENGS_PROPERTY_ILLUMINANCE_LUX:
                case THEENGS_PROPERTY_VOLTAGE_MV:
                case THEENGS_PROPERTY_OPENING:
                case THEENGS_PROPERTY_CO2_PPM:
                case THEENGS_PROPERTY_MOISTURE_PERCENT:
                case THEENGS_PROPERTY_MOTION:
                case THEENGS_PROPERTY_BUTTON_EVENT:
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static theengs_status_t fill_property_info(theengs_property_id_t property_id,
                                           theengs_property_info_t *out_info)
{
    switch (property_id) {
        case THEENGS_PROPERTY_TEMPERATURE_C:
            out_info->value_type = THEENGS_VALUE_FLOAT; out_info->unit = THEENGS_UNIT_CELSIUS;
            copy_string(out_info->name, sizeof(out_info->name), "temperature"); break;
        case THEENGS_PROPERTY_HUMIDITY_PERCENT:
            out_info->value_type = THEENGS_VALUE_FLOAT; out_info->unit = THEENGS_UNIT_PERCENT;
            copy_string(out_info->name, sizeof(out_info->name), "humidity"); break;
        case THEENGS_PROPERTY_PRESSURE_PA:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_PASCAL;
            copy_string(out_info->name, sizeof(out_info->name), "pressure"); break;
        case THEENGS_PROPERTY_ACCEL_X_MG:
            out_info->value_type = THEENGS_VALUE_INT32; out_info->unit = THEENGS_UNIT_MILLIG;
            copy_string(out_info->name, sizeof(out_info->name), "accel_x"); break;
        case THEENGS_PROPERTY_ACCEL_Y_MG:
            out_info->value_type = THEENGS_VALUE_INT32; out_info->unit = THEENGS_UNIT_MILLIG;
            copy_string(out_info->name, sizeof(out_info->name), "accel_y"); break;
        case THEENGS_PROPERTY_ACCEL_Z_MG:
            out_info->value_type = THEENGS_VALUE_INT32; out_info->unit = THEENGS_UNIT_MILLIG;
            copy_string(out_info->name, sizeof(out_info->name), "accel_z"); break;
        case THEENGS_PROPERTY_VOLTAGE_MV:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_MILLIVOLT;
            copy_string(out_info->name, sizeof(out_info->name), "voltage"); break;
        case THEENGS_PROPERTY_TX_POWER_DBM:
            out_info->value_type = THEENGS_VALUE_INT32; out_info->unit = THEENGS_UNIT_DBM;
            copy_string(out_info->name, sizeof(out_info->name), "tx_power"); break;
        case THEENGS_PROPERTY_MOVEMENT_COUNT:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_COUNT;
            copy_string(out_info->name, sizeof(out_info->name), "movement_count"); break;
        case THEENGS_PROPERTY_MEASUREMENT_SEQUENCE:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_COUNT;
            copy_string(out_info->name, sizeof(out_info->name), "measurement_sequence"); break;
        case THEENGS_PROPERTY_BATTERY_PERCENT:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_PERCENT;
            copy_string(out_info->name, sizeof(out_info->name), "battery"); break;
        case THEENGS_PROPERTY_ILLUMINANCE_LUX:
            out_info->value_type = THEENGS_VALUE_FLOAT; out_info->unit = THEENGS_UNIT_LUX;
            copy_string(out_info->name, sizeof(out_info->name), "illuminance"); break;
        case THEENGS_PROPERTY_MOISTURE_PERCENT:
            out_info->value_type = THEENGS_VALUE_FLOAT; out_info->unit = THEENGS_UNIT_PERCENT;
            copy_string(out_info->name, sizeof(out_info->name), "moisture"); break;
        case THEENGS_PROPERTY_OPENING:
            out_info->value_type = THEENGS_VALUE_BOOL; out_info->unit = THEENGS_UNIT_NONE;
            copy_string(out_info->name, sizeof(out_info->name), "opening"); break;
        case THEENGS_PROPERTY_MOTION:
            out_info->value_type = THEENGS_VALUE_BOOL; out_info->unit = THEENGS_UNIT_NONE;
            copy_string(out_info->name, sizeof(out_info->name), "motion"); break;
        case THEENGS_PROPERTY_BUTTON_EVENT:
            out_info->value_type = THEENGS_VALUE_ENUM; out_info->unit = THEENGS_UNIT_NONE;
            copy_string(out_info->name, sizeof(out_info->name), "button_event"); break;
        case THEENGS_PROPERTY_PACKET_ID:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_COUNT;
            copy_string(out_info->name, sizeof(out_info->name), "packet_id"); break;
        case THEENGS_PROPERTY_CO2_PPM:
            out_info->value_type = THEENGS_VALUE_UINT32; out_info->unit = THEENGS_UNIT_PPM;
            copy_string(out_info->name, sizeof(out_info->name), "co2"); break;
        default:
            return THEENGS_STATUS_NOT_FOUND;
    }
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_property_info(theengs_decoder_id_t decoder_id,
                                       theengs_property_id_t property_id,
                                       theengs_property_info_t *out_info)
{
    if (out_info == NULL) return THEENGS_STATUS_INVALID_ARGUMENT;
    if (decoder_id != THEENGS_DECODER_RUUVI_RAW_V2 && decoder_id != THEENGS_DECODER_BTHOME_V2) {
        return THEENGS_STATUS_UNKNOWN_DECODER;
    }
    if (!property_belongs_to_decoder(decoder_id, property_id)) {
        return THEENGS_STATUS_NOT_FOUND;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->property_id = property_id;
    return fill_property_info(property_id, out_info);
}
