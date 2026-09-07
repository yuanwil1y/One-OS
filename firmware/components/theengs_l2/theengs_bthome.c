#include "theengs_internal.h"

#include <stdint.h>

static uint16_t read_le_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t read_le_i16(const uint8_t *p)
{
    return (int16_t)read_le_u16(p);
}

static uint32_t read_le_u24(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static theengs_status_t need_bytes(size_t pos, size_t needed, size_t len,
                                   theengs_decoded_values_t *out)
{
    if (needed > len || pos > len - needed) {
        out->incomplete = true;
        return THEENGS_STATUS_TRUNCATED_INPUT;
    }
    return THEENGS_STATUS_OK;
}

theengs_status_t theengs_decode_bthome_v2(const theengs_parsed_adv_t *adv,
                                         theengs_decoded_values_t *out)
{
    const uint8_t *data = adv->service_data;
    const size_t len = adv->service_data_len;
    if (data == NULL || len < 1u) return THEENGS_STATUS_TRUNCATED_INPUT;

    const uint8_t device_info = data[0];
    const uint8_t version = (uint8_t)((device_info >> 5) & 0x07u);
    if (version != 2u || (device_info & 0x01u) != 0u) {
        return THEENGS_STATUS_UNSUPPORTED_PAYLOAD;
    }

    size_t pos = 1u;
    while (pos < len) {
        const uint8_t object_id = data[pos++];
        theengs_status_t status;
        switch (object_id) {
            case 0x00u:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_u32(out, THEENGS_PROPERTY_PACKET_ID, data[pos]); pos += 1u; break;
            case 0x01u:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_u32(out, THEENGS_PROPERTY_BATTERY_PERCENT, data[pos]); pos += 1u; break;
            case 0x02u:
                if ((status = need_bytes(pos, 2u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_TEMPERATURE_C,
                                              (float)read_le_i16(&data[pos]) * 0.01f); pos += 2u; break;
            case 0x03u:
                if ((status = need_bytes(pos, 2u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_HUMIDITY_PERCENT,
                                              (float)read_le_u16(&data[pos]) * 0.01f); pos += 2u; break;
            case 0x04u:
                if ((status = need_bytes(pos, 3u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_u32(out, THEENGS_PROPERTY_PRESSURE_PA, read_le_u24(&data[pos])); pos += 3u; break;
            case 0x05u:
                if ((status = need_bytes(pos, 3u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_ILLUMINANCE_LUX,
                                              (float)read_le_u24(&data[pos]) * 0.01f); pos += 3u; break;
            case 0x0cu:
                if ((status = need_bytes(pos, 2u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_u32(out, THEENGS_PROPERTY_VOLTAGE_MV, read_le_u16(&data[pos])); pos += 2u; break;
            case 0x11u:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                if (data[pos] > 1u) { out->incomplete = true; return THEENGS_STATUS_UNSUPPORTED_PAYLOAD; }
                status = theengs_append_bool(out, THEENGS_PROPERTY_OPENING, data[pos] != 0u); pos += 1u; break;
            case 0x12u:
                if ((status = need_bytes(pos, 2u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_u32(out, THEENGS_PROPERTY_CO2_PPM, read_le_u16(&data[pos])); pos += 2u; break;
            case 0x14u:
                if ((status = need_bytes(pos, 2u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_MOISTURE_PERCENT,
                                              (float)read_le_u16(&data[pos]) * 0.01f); pos += 2u; break;
            case 0x21u:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                if (data[pos] > 1u) { out->incomplete = true; return THEENGS_STATUS_UNSUPPORTED_PAYLOAD; }
                status = theengs_append_bool(out, THEENGS_PROPERTY_MOTION, data[pos] != 0u); pos += 1u; break;
            case 0x2eu:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_HUMIDITY_PERCENT, (float)data[pos]); pos += 1u; break;
            case 0x2fu:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_float(out, THEENGS_PROPERTY_MOISTURE_PERCENT, (float)data[pos]); pos += 1u; break;
            case 0x3au:
                if ((status = need_bytes(pos, 1u, len, out)) != THEENGS_STATUS_OK) return status;
                status = theengs_append_enum(out, THEENGS_PROPERTY_BUTTON_EVENT, data[pos]); pos += 1u; break;
            default:
                out->incomplete = true;
                return out->value_count > 0u ? THEENGS_STATUS_PARTIAL : THEENGS_STATUS_UNSUPPORTED_PAYLOAD;
        }

        if (status == THEENGS_STATUS_RESULT_FULL) out->incomplete = true;
        if (status != THEENGS_STATUS_OK) return status;
    }

    return THEENGS_STATUS_OK;
}
