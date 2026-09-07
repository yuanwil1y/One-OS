#include "theengs_internal.h"

#include <stdint.h>

static uint16_t read_be_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t read_be_i16(const uint8_t *p)
{
    return (int16_t)read_be_u16(p);
}

static theengs_status_t append_status(theengs_status_t current, theengs_status_t next)
{
    return current == THEENGS_STATUS_OK ? next : current;
}

theengs_status_t theengs_decode_ruuvi_raw_v2(const theengs_parsed_adv_t *adv,
                                             theengs_decoded_values_t *out)
{
    const uint8_t *data = adv->manufacturer_data;
    const size_t len = adv->manufacturer_data_len;

    if (data == NULL || len < 24u) return THEENGS_STATUS_TRUNCATED_INPUT;
    if (data[0] != 0x05u) return THEENGS_STATUS_UNSUPPORTED_PAYLOAD;

    theengs_status_t status = THEENGS_STATUS_OK;
    const int16_t temperature = read_be_i16(&data[1]);
    if (temperature != INT16_MIN) {
        status = append_status(status, theengs_append_float(out, THEENGS_PROPERTY_TEMPERATURE_C,
                                                            (float)temperature * 0.005f));
    }
    const uint16_t humidity = read_be_u16(&data[3]);
    if (humidity != UINT16_MAX) {
        status = append_status(status, theengs_append_float(out, THEENGS_PROPERTY_HUMIDITY_PERCENT,
                                                            (float)humidity * 0.0025f));
    }
    const uint16_t pressure = read_be_u16(&data[5]);
    if (pressure != UINT16_MAX) {
        status = append_status(status, theengs_append_u32(out, THEENGS_PROPERTY_PRESSURE_PA,
                                                          (uint32_t)pressure + 50000u));
    }

    const int16_t accel_x = read_be_i16(&data[7]);
    if (accel_x != INT16_MIN) status = append_status(status, theengs_append_i32(out, THEENGS_PROPERTY_ACCEL_X_MG, accel_x));
    const int16_t accel_y = read_be_i16(&data[9]);
    if (accel_y != INT16_MIN) status = append_status(status, theengs_append_i32(out, THEENGS_PROPERTY_ACCEL_Y_MG, accel_y));
    const int16_t accel_z = read_be_i16(&data[11]);
    if (accel_z != INT16_MIN) status = append_status(status, theengs_append_i32(out, THEENGS_PROPERTY_ACCEL_Z_MG, accel_z));

    const uint16_t power = read_be_u16(&data[13]);
    const uint16_t voltage_raw = (uint16_t)(power >> 5);
    const uint8_t tx_raw = (uint8_t)(power & 0x1fu);
    if (voltage_raw != 0x07ffu) {
        status = append_status(status, theengs_append_u32(out, THEENGS_PROPERTY_VOLTAGE_MV,
                                                          (uint32_t)voltage_raw + 1600u));
    }
    if (tx_raw != 0x1fu) {
        status = append_status(status, theengs_append_i32(out, THEENGS_PROPERTY_TX_POWER_DBM,
                                                          -40 + (int32_t)tx_raw * 2));
    }
    if (data[15] != 0xffu) {
        status = append_status(status, theengs_append_u32(out, THEENGS_PROPERTY_MOVEMENT_COUNT, data[15]));
    }
    const uint16_t sequence = read_be_u16(&data[16]);
    if (sequence != UINT16_MAX) {
        status = append_status(status, theengs_append_u32(out, THEENGS_PROPERTY_MEASUREMENT_SEQUENCE, sequence));
    }

    if (status == THEENGS_STATUS_RESULT_FULL) out->incomplete = true;
    return status;
}
