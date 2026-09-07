#include "wireshark_l2.h"

#include <string.h>

/*
 * Provenance: clean-room implementation from Bluetooth GAP Advertising Data
 * structure definitions and Bluetooth Assigned Numbers. Wireshark's Bluetooth
 * dissectors are REFERENCE-ONLY field-truth/test-oracle material.
 */

#define BLE_AD_FLAGS                         0x01u
#define BLE_AD_UUID16_INCOMPLETE             0x02u
#define BLE_AD_UUID16_COMPLETE               0x03u
#define BLE_AD_UUID32_INCOMPLETE             0x04u
#define BLE_AD_UUID32_COMPLETE               0x05u
#define BLE_AD_UUID128_INCOMPLETE            0x06u
#define BLE_AD_UUID128_COMPLETE              0x07u
#define BLE_AD_SHORT_NAME                    0x08u
#define BLE_AD_COMPLETE_NAME                 0x09u
#define BLE_AD_TX_POWER                      0x0au
#define BLE_AD_SERVICE_DATA_UUID16           0x16u
#define BLE_AD_APPEARANCE                    0x19u
#define BLE_AD_SERVICE_DATA_UUID32           0x20u
#define BLE_AD_SERVICE_DATA_UUID128          0x21u
#define BLE_AD_MANUFACTURER                  0xffu

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void ble_mark_malformed(wireshark_ble_adv_t *out, size_t offset)
{
    out->diag.malformed = true;
    if (out->diag.error_offset == SIZE_MAX) {
        out->diag.error_offset = offset;
    }
}

static void ble_mark_partial(wireshark_ble_adv_t *out)
{
    out->diag.output_limited = true;
}

static void ble_add_uuid16(wireshark_ble_adv_t *out, uint16_t uuid)
{
    if (out->uuid16_count < WIRESHARK_BLE_MAX_UUID16) {
        out->uuid16[out->uuid16_count++] = uuid;
    } else {
        ble_mark_partial(out);
    }
}

static void ble_add_uuid32(wireshark_ble_adv_t *out, uint32_t uuid)
{
    if (out->uuid32_count < WIRESHARK_BLE_MAX_UUID32) {
        out->uuid32[out->uuid32_count++] = uuid;
    } else {
        ble_mark_partial(out);
    }
}

static void ble_add_uuid128(wireshark_ble_adv_t *out, const uint8_t *uuid)
{
    if (out->uuid128_count < WIRESHARK_BLE_MAX_UUID128) {
        wireshark_ble_uuid128_t *dst = &out->uuid128[out->uuid128_count++];
        dst->length = 16;
        memcpy(dst->bytes, uuid, 16);
    } else {
        ble_mark_partial(out);
    }
}

static void ble_parse_uuid_list(wireshark_ble_adv_t *out,
                                uint8_t type,
                                const uint8_t *value,
                                size_t value_len,
                                size_t value_offset)
{
    size_t width;
    if (type == BLE_AD_UUID16_INCOMPLETE || type == BLE_AD_UUID16_COMPLETE) {
        width = 2;
        if (type == BLE_AD_UUID16_COMPLETE) {
            out->uuid16_complete = true;
        }
    } else if (type == BLE_AD_UUID32_INCOMPLETE || type == BLE_AD_UUID32_COMPLETE) {
        width = 4;
        if (type == BLE_AD_UUID32_COMPLETE) {
            out->uuid32_complete = true;
        }
    } else {
        width = 16;
        if (type == BLE_AD_UUID128_COMPLETE) {
            out->uuid128_complete = true;
        }
    }

    const size_t complete = value_len / width;
    for (size_t i = 0; i < complete; ++i) {
        const uint8_t *entry = value + (i * width);
        if (width == 2) {
            ble_add_uuid16(out, read_le16(entry));
        } else if (width == 4) {
            ble_add_uuid32(out, read_le32(entry));
        } else {
            ble_add_uuid128(out, entry);
        }
    }

    if ((value_len % width) != 0u) {
        ble_mark_malformed(out, value_offset + (complete * width));
    }
}

static void ble_parse_manufacturer(wireshark_ble_adv_t *out,
                                   const uint8_t *value,
                                   size_t value_len,
                                   size_t value_offset)
{
    if (value_len < 2u) {
        ble_mark_malformed(out, value_offset);
        return;
    }

    if (out->manufacturer_data_count >= WIRESHARK_BLE_MAX_MFG_DATA) {
        ble_mark_partial(out);
        return;
    }

    wireshark_ble_manufacturer_data_t *dst =
        &out->manufacturer_data[out->manufacturer_data_count++];
    dst->company_id = read_le16(value);

    const size_t payload_len = value_len - 2u;
    const size_t copy_len = payload_len < WIRESHARK_BLE_DATA_MAX
                                ? payload_len
                                : WIRESHARK_BLE_DATA_MAX;
    dst->data_len = (uint8_t)copy_len;
    if (copy_len != 0u) {
        memcpy(dst->data, value + 2, copy_len);
    }
    if (copy_len < payload_len) {
        dst->data_truncated = true;
        ble_mark_partial(out);
    }
}

static void ble_parse_service_data(wireshark_ble_adv_t *out,
                                   uint8_t type,
                                   const uint8_t *value,
                                   size_t value_len,
                                   size_t value_offset)
{
    const size_t uuid_len = type == BLE_AD_SERVICE_DATA_UUID16
                                ? 2u
                                : (type == BLE_AD_SERVICE_DATA_UUID32 ? 4u : 16u);

    if (value_len < uuid_len) {
        ble_mark_malformed(out, value_offset);
        return;
    }

    if (out->service_data_count >= WIRESHARK_BLE_MAX_SERVICE_DATA) {
        ble_mark_partial(out);
        return;
    }

    wireshark_ble_service_data_t *dst =
        &out->service_data[out->service_data_count++];
    dst->ad_type = type;
    dst->uuid_len = (uint8_t)uuid_len;
    memcpy(dst->uuid, value, uuid_len);

    const size_t payload_len = value_len - uuid_len;
    const size_t copy_len = payload_len < WIRESHARK_BLE_DATA_MAX
                                ? payload_len
                                : WIRESHARK_BLE_DATA_MAX;
    dst->data_len = (uint8_t)copy_len;
    if (copy_len != 0u) {
        memcpy(dst->data, value + uuid_len, copy_len);
    }
    if (copy_len < payload_len) {
        dst->data_truncated = true;
        ble_mark_partial(out);
    }
}

static void ble_parse_typed_value(wireshark_ble_adv_t *out,
                                  uint8_t type,
                                  const uint8_t *value,
                                  size_t value_len,
                                  size_t value_offset)
{
    switch (type) {
    case BLE_AD_FLAGS:
        if (value_len != 1u) {
            ble_mark_malformed(out, value_offset);
        } else if (!out->flags_present) {
            out->flags_present = true;
            out->flags = value[0];
        }
        break;

    case BLE_AD_UUID16_INCOMPLETE:
    case BLE_AD_UUID16_COMPLETE:
    case BLE_AD_UUID32_INCOMPLETE:
    case BLE_AD_UUID32_COMPLETE:
    case BLE_AD_UUID128_INCOMPLETE:
    case BLE_AD_UUID128_COMPLETE:
        ble_parse_uuid_list(out, type, value, value_len, value_offset);
        break;

    case BLE_AD_SHORT_NAME:
    case BLE_AD_COMPLETE_NAME:
        if (!out->name_present) {
            const size_t copy_len = value_len < WIRESHARK_BLE_NAME_MAX
                                        ? value_len
                                        : WIRESHARK_BLE_NAME_MAX;
            out->name_present = true;
            out->name_complete = type == BLE_AD_COMPLETE_NAME;
            out->name_len = (uint8_t)copy_len;
            if (copy_len != 0u) {
                memcpy(out->name, value, copy_len);
            }
            if (copy_len < value_len) {
                out->name_truncated = true;
                ble_mark_partial(out);
            }
        }
        break;

    case BLE_AD_TX_POWER:
        if (value_len != 1u) {
            ble_mark_malformed(out, value_offset);
        } else if (!out->tx_power_present) {
            out->tx_power_present = true;
            out->tx_power_dbm = (int8_t)value[0];
        }
        break;

    case BLE_AD_APPEARANCE:
        if (value_len != 2u) {
            ble_mark_malformed(out, value_offset);
        } else if (!out->appearance_present) {
            out->appearance_present = true;
            out->appearance = read_le16(value);
        }
        break;

    case BLE_AD_MANUFACTURER:
        ble_parse_manufacturer(out, value, value_len, value_offset);
        break;

    case BLE_AD_SERVICE_DATA_UUID16:
    case BLE_AD_SERVICE_DATA_UUID32:
    case BLE_AD_SERVICE_DATA_UUID128:
        ble_parse_service_data(out, type, value, value_len, value_offset);
        break;

    default:
        /* Unknown AD types are valid and intentionally ignored. */
        break;
    }
}

wireshark_parse_status_t
wireshark_ble_adv_parse(const uint8_t *data, size_t len, wireshark_ble_adv_t *out)
{
    if (out == NULL) {
        return WIRESHARK_PARSE_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->diag.error_offset = SIZE_MAX;

    if (data == NULL && len != 0u) {
        out->diag.malformed = true;
        out->diag.error_offset = 0;
        out->diag.status = WIRESHARK_PARSE_MALFORMED;
        return out->diag.status;
    }

    size_t offset = 0;
    while (offset < len) {
        const uint8_t field_len = data[offset];
        if (field_len == 0u) {
            ++offset; /* AD zero-length terminator/padding. */
            break;
        }

        if ((size_t)field_len > (len - offset - 1u)) {
            out->diag.truncated = true;
            if (out->diag.error_offset == SIZE_MAX) {
                out->diag.error_offset = offset;
            }
            out->diag.consumed = offset;
            out->diag.status = WIRESHARK_PARSE_TRUNCATED;
            return out->diag.status;
        }

        const uint8_t type = data[offset + 1u];
        const size_t value_len = (size_t)field_len - 1u;
        const uint8_t *value = data + offset + 2u;
        const size_t value_offset = offset + 2u;

        ++out->ad_structure_count;
        ble_parse_typed_value(out, type, value, value_len, value_offset);
        offset += (size_t)field_len + 1u;
    }

    out->diag.consumed = offset;
    if (out->diag.error_offset == SIZE_MAX) {
        out->diag.error_offset = offset;
    }

    if (out->diag.malformed) {
        out->diag.status = WIRESHARK_PARSE_MALFORMED;
    } else if (out->diag.output_limited) {
        out->diag.status = WIRESHARK_PARSE_PARTIAL;
    } else {
        out->diag.status = WIRESHARK_PARSE_OK;
    }
    return out->diag.status;
}
