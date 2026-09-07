#include "wireshark_l2.h"

#include <string.h>

/*
 * Provenance: clean-room implementation from IEEE 802.11 management-frame,
 * information-element, and RSN definitions. Wireshark packet-ieee80211.c is
 * REFERENCE-ONLY field-truth/test-oracle material.
 */

#define WIFI_MGMT_HEADER_LEN                  24u

#define WIFI_IE_SSID                          0u
#define WIFI_IE_SUPPORTED_RATES               1u
#define WIFI_IE_DS_PARAMETER                  3u
#define WIFI_IE_HT_CAPABILITIES              45u
#define WIFI_IE_RSN                          48u
#define WIFI_IE_EXT_SUPPORTED_RATES          50u
#define WIFI_IE_HT_OPERATION                 61u
#define WIFI_IE_VENDOR_SPECIFIC             221u
#define WIFI_IE_EXTENSION                   255u

#define WIFI_EXT_HE_CAPABILITIES             35u
#define WIFI_EXT_HE_OPERATION                36u

#define WIFI_SUBTYPE_ASSOC_REQ                0u
#define WIFI_SUBTYPE_ASSOC_RESP               1u
#define WIFI_SUBTYPE_REASSOC_REQ              2u
#define WIFI_SUBTYPE_REASSOC_RESP             3u
#define WIFI_SUBTYPE_PROBE_REQ                4u
#define WIFI_SUBTYPE_PROBE_RESP               5u
#define WIFI_SUBTYPE_BEACON                   8u

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static void wifi_ie_mark_malformed(wireshark_wifi_ie_summary_t *out, size_t offset)
{
    out->diag.malformed = true;
    if (out->diag.error_offset == SIZE_MAX) {
        out->diag.error_offset = offset;
    }
}

static void wifi_ie_mark_partial(wireshark_wifi_ie_summary_t *out)
{
    out->diag.output_limited = true;
}

static void wifi_add_rate(wireshark_wifi_ie_summary_t *out, uint8_t octet)
{
    if (out->rate_count >= WIRESHARK_WIFI_MAX_RATES) {
        wifi_ie_mark_partial(out);
        return;
    }

    wireshark_wifi_rate_t *rate = &out->rates[out->rate_count++];
    rate->rate_500kbps = octet & 0x7fu;
    rate->basic = (octet & 0x80u) != 0u;
}

static void wifi_copy_suite(wireshark_wifi_suite_t *dst, const uint8_t *src)
{
    memcpy(dst->oui, src, 3);
    dst->type = src[3];
}

static bool wifi_rsn_need(size_t offset, size_t need, size_t len)
{
    return offset <= len && need <= (len - offset);
}

static void wifi_parse_rsn(wireshark_wifi_ie_summary_t *out,
                           const uint8_t *value,
                           size_t value_len,
                           size_t value_offset)
{
    wireshark_wifi_rsn_t parsed = {0};
    parsed.present = true;

    size_t offset = 0;
    if (!wifi_rsn_need(offset, 2, value_len)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    parsed.version = read_le16(value + offset);
    offset += 2;
    if (parsed.version != 1u) {
        wifi_ie_mark_malformed(out, value_offset);
    }

    if (!wifi_rsn_need(offset, 4, value_len)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    wifi_copy_suite(&parsed.group_cipher, value + offset);
    offset += 4;

    if (!wifi_rsn_need(offset, 2, value_len)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    const uint16_t pairwise_wire_count = read_le16(value + offset);
    offset += 2;

    if ((size_t)pairwise_wire_count > ((value_len - offset) / 4u)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    for (uint16_t i = 0; i < pairwise_wire_count; ++i) {
        if (parsed.pairwise_count < WIRESHARK_WIFI_MAX_PAIRWISE) {
            wifi_copy_suite(&parsed.pairwise[parsed.pairwise_count++], value + offset);
        } else {
            wifi_ie_mark_partial(out);
        }
        offset += 4;
    }

    if (!wifi_rsn_need(offset, 2, value_len)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    const uint16_t akm_wire_count = read_le16(value + offset);
    offset += 2;

    if ((size_t)akm_wire_count > ((value_len - offset) / 4u)) {
        wifi_ie_mark_malformed(out, value_offset + offset);
        return;
    }
    for (uint16_t i = 0; i < akm_wire_count; ++i) {
        if (parsed.akm_count < WIRESHARK_WIFI_MAX_AKM) {
            wifi_copy_suite(&parsed.akm[parsed.akm_count++], value + offset);
        } else {
            wifi_ie_mark_partial(out);
        }
        offset += 4;
    }

    /* RSN capabilities are optional. A single trailing byte is invalid. */
    if (offset < value_len) {
        if (!wifi_rsn_need(offset, 2, value_len)) {
            wifi_ie_mark_malformed(out, value_offset + offset);
            return;
        }
        parsed.capabilities_present = true;
        parsed.capabilities = read_le16(value + offset);
        offset += 2;
    }

    /* Optional PMKID count/list: validate lengths but do not retain PMKIDs. */
    if (offset < value_len) {
        if (!wifi_rsn_need(offset, 2, value_len)) {
            wifi_ie_mark_malformed(out, value_offset + offset);
            return;
        }
        const uint16_t pmkid_count = read_le16(value + offset);
        offset += 2;
        if ((size_t)pmkid_count > ((value_len - offset) / 16u)) {
            wifi_ie_mark_malformed(out, value_offset + offset);
            return;
        }
        offset += (size_t)pmkid_count * 16u;
    }

    /* Optional group-management cipher suite. */
    if (offset < value_len) {
        if (!wifi_rsn_need(offset, 4, value_len)) {
            wifi_ie_mark_malformed(out, value_offset + offset);
            return;
        }
        offset += 4;
    }

    /* Any trailing bytes are safely ignored for forward-compatible RSN growth. */
    (void)offset;

    if (!out->rsn.present) {
        out->rsn = parsed;
    }
}

static void wifi_parse_vendor_ie(wireshark_wifi_ie_summary_t *out,
                                 const uint8_t *value,
                                 size_t value_len,
                                 size_t value_offset)
{
    if (value_len < 3u) {
        wifi_ie_mark_malformed(out, value_offset);
        return;
    }

    if (out->vendor_ie_count >= WIRESHARK_WIFI_MAX_VENDOR_IE) {
        wifi_ie_mark_partial(out);
        return;
    }

    wireshark_wifi_vendor_ie_t *dst = &out->vendor_ie[out->vendor_ie_count++];
    memcpy(dst->oui, value, 3);

    size_t payload_offset = 3;
    if (value_len >= 4u) {
        dst->type_present = true;
        dst->type = value[3];
        payload_offset = 4;
    }

    const size_t payload_len = value_len - payload_offset;
    const size_t copy_len = payload_len < WIRESHARK_WIFI_VENDOR_DATA_MAX
                                ? payload_len
                                : WIRESHARK_WIFI_VENDOR_DATA_MAX;
    dst->data_len = (uint8_t)copy_len;
    if (copy_len != 0u) {
        memcpy(dst->data, value + payload_offset, copy_len);
    }
    if (copy_len < payload_len) {
        dst->data_truncated = true;
        wifi_ie_mark_partial(out);
    }
}

static void wifi_parse_ie_value(wireshark_wifi_ie_summary_t *out,
                                uint8_t id,
                                const uint8_t *value,
                                size_t value_len,
                                size_t value_offset)
{
    switch (id) {
    case WIFI_IE_SSID:
        if (value_len > WIRESHARK_WIFI_SSID_MAX) {
            wifi_ie_mark_malformed(out, value_offset);
        } else if (!out->ssid_present) {
            out->ssid_present = true;
            out->ssid_hidden = value_len == 0u;
            out->ssid_len = (uint8_t)value_len;
            if (value_len != 0u) {
                memcpy(out->ssid, value, value_len);
            }
        }
        break;

    case WIFI_IE_SUPPORTED_RATES:
        if (value_len == 0u || value_len > 8u) {
            wifi_ie_mark_malformed(out, value_offset);
        }
        for (size_t i = 0; i < value_len; ++i) {
            wifi_add_rate(out, value[i]);
        }
        break;

    case WIFI_IE_EXT_SUPPORTED_RATES:
        if (value_len == 0u) {
            wifi_ie_mark_malformed(out, value_offset);
        }
        for (size_t i = 0; i < value_len; ++i) {
            wifi_add_rate(out, value[i]);
        }
        break;

    case WIFI_IE_DS_PARAMETER:
        if (value_len != 1u) {
            wifi_ie_mark_malformed(out, value_offset);
        } else if (!out->channel_present) {
            out->channel_present = true;
            out->channel = value[0];
        }
        break;

    case WIFI_IE_RSN:
        wifi_parse_rsn(out, value, value_len, value_offset);
        break;

    case WIFI_IE_HT_CAPABILITIES:
        if (value_len != 26u) {
            wifi_ie_mark_malformed(out, value_offset);
        }
        if (value_len >= 2u && !out->ht_capabilities_present) {
            out->ht_capabilities_present = true;
            out->ht_capability_info = read_le16(value);
        }
        break;

    case WIFI_IE_HT_OPERATION:
        if (value_len != 22u) {
            wifi_ie_mark_malformed(out, value_offset);
        }
        if (value_len >= 1u && !out->ht_operation_present) {
            out->ht_operation_present = true;
            out->ht_primary_channel = value[0];
            if (!out->channel_present) {
                out->channel_present = true;
                out->channel = value[0];
            }
        }
        break;

    case WIFI_IE_EXTENSION:
        if (value_len == 0u) {
            wifi_ie_mark_malformed(out, value_offset);
        } else if (value[0] == WIFI_EXT_HE_CAPABILITIES) {
            out->he_capabilities_present = true;
        } else if (value[0] == WIFI_EXT_HE_OPERATION) {
            out->he_operation_present = true;
        }
        break;

    case WIFI_IE_VENDOR_SPECIFIC:
        wifi_parse_vendor_ie(out, value, value_len, value_offset);
        break;

    default:
        /* Unknown/unused IEs are valid and deliberately skipped. */
        break;
    }
}

wireshark_parse_status_t
wireshark_wifi_ie_parse(const uint8_t *ies, size_t len,
                        wireshark_wifi_ie_summary_t *out)
{
    if (out == NULL) {
        return WIRESHARK_PARSE_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->diag.error_offset = SIZE_MAX;

    if (ies == NULL && len != 0u) {
        out->diag.malformed = true;
        out->diag.error_offset = 0;
        out->diag.status = WIRESHARK_PARSE_MALFORMED;
        return out->diag.status;
    }

    size_t offset = 0;
    while (offset < len) {
        if ((len - offset) < 2u) {
            out->diag.truncated = true;
            out->diag.error_offset = offset;
            out->diag.consumed = offset;
            out->diag.status = WIRESHARK_PARSE_TRUNCATED;
            return out->diag.status;
        }

        const uint8_t id = ies[offset];
        const uint8_t value_len = ies[offset + 1u];
        if ((size_t)value_len > (len - offset - 2u)) {
            out->diag.truncated = true;
            out->diag.error_offset = offset;
            out->diag.consumed = offset;
            out->diag.status = WIRESHARK_PARSE_TRUNCATED;
            return out->diag.status;
        }

        const uint8_t *value = ies + offset + 2u;
        ++out->ie_count;
        wifi_parse_ie_value(out, id, value, value_len, offset + 2u);
        offset += 2u + (size_t)value_len;
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

static bool wifi_supported_subtype(uint8_t subtype, size_t *fixed_len)
{
    switch (subtype) {
    case WIFI_SUBTYPE_ASSOC_REQ:
        *fixed_len = 4;
        return true;
    case WIFI_SUBTYPE_ASSOC_RESP:
        *fixed_len = 6;
        return true;
    case WIFI_SUBTYPE_REASSOC_REQ:
        *fixed_len = 10;
        return true;
    case WIFI_SUBTYPE_REASSOC_RESP:
        *fixed_len = 6;
        return true;
    case WIFI_SUBTYPE_PROBE_REQ:
        *fixed_len = 0;
        return true;
    case WIFI_SUBTYPE_PROBE_RESP:
    case WIFI_SUBTYPE_BEACON:
        *fixed_len = 12;
        return true;
    default:
        *fixed_len = 0;
        return false;
    }
}

static void wifi_parse_fixed_fields(const uint8_t *body,
                                    uint8_t subtype,
                                    wireshark_wifi_mgmt_t *out)
{
    switch (subtype) {
    case WIFI_SUBTYPE_ASSOC_REQ:
    case WIFI_SUBTYPE_REASSOC_REQ:
    case WIFI_SUBTYPE_ASSOC_RESP:
    case WIFI_SUBTYPE_REASSOC_RESP:
        out->capability_present = true;
        out->capability = read_le16(body);
        break;

    case WIFI_SUBTYPE_PROBE_RESP:
    case WIFI_SUBTYPE_BEACON:
        out->timestamp_present = true;
        out->timestamp = read_le64(body);
        out->beacon_interval_present = true;
        out->beacon_interval_tu = read_le16(body + 8);
        out->capability_present = true;
        out->capability = read_le16(body + 10);
        break;

    default:
        break;
    }
}

wireshark_parse_status_t
wireshark_wifi_mgmt_parse(const uint8_t *frame, size_t len,
                          wireshark_wifi_mgmt_t *out)
{
    if (out == NULL) {
        return WIRESHARK_PARSE_MALFORMED;
    }

    memset(out, 0, sizeof(*out));
    out->diag.error_offset = SIZE_MAX;

    if (frame == NULL && len != 0u) {
        out->diag.malformed = true;
        out->diag.error_offset = 0;
        out->diag.status = WIRESHARK_PARSE_MALFORMED;
        return out->diag.status;
    }

    if (len < 2u) {
        out->diag.truncated = true;
        out->diag.error_offset = 0;
        out->diag.status = WIRESHARK_PARSE_TRUNCATED;
        return out->diag.status;
    }

    out->frame_control = read_le16(frame);
    out->subtype = (uint8_t)((out->frame_control >> 4) & 0x0fu);

    /* Type 0 is management. */
    if (((out->frame_control >> 2) & 0x03u) != 0u) {
        out->diag.status = WIRESHARK_PARSE_UNSUPPORTED;
        out->diag.error_offset = 0;
        out->diag.consumed = 2;
        return out->diag.status;
    }

    if (len < WIFI_MGMT_HEADER_LEN) {
        out->diag.truncated = true;
        out->diag.error_offset = len;
        out->diag.consumed = 2;
        out->diag.status = WIRESHARK_PARSE_TRUNCATED;
        return out->diag.status;
    }

    memcpy(out->destination, frame + 4, 6);
    memcpy(out->source, frame + 10, 6);
    memcpy(out->bssid, frame + 16, 6);
    out->sequence_control = read_le16(frame + 22);

    /* To-DS/From-DS are reserved for ordinary management frames. */
    if ((out->frame_control & 0x0300u) != 0u) {
        out->diag.malformed = true;
        out->diag.error_offset = 1;
    }

    /* The approved discovery subtypes use the ordinary 24-byte header. */
    if ((out->frame_control & 0x8000u) != 0u) {
        out->diag.status = WIRESHARK_PARSE_UNSUPPORTED;
        out->diag.error_offset = 1;
        out->diag.consumed = WIFI_MGMT_HEADER_LEN;
        return out->diag.status;
    }

    size_t fixed_len = 0;
    if (!wifi_supported_subtype(out->subtype, &fixed_len)) {
        out->diag.status = WIRESHARK_PARSE_UNSUPPORTED;
        out->diag.error_offset = WIFI_MGMT_HEADER_LEN;
        out->diag.consumed = WIFI_MGMT_HEADER_LEN;
        return out->diag.status;
    }

    if (fixed_len > (len - WIFI_MGMT_HEADER_LEN)) {
        out->diag.truncated = true;
        out->diag.error_offset = WIFI_MGMT_HEADER_LEN;
        out->diag.consumed = WIFI_MGMT_HEADER_LEN;
        out->diag.status = WIRESHARK_PARSE_TRUNCATED;
        return out->diag.status;
    }

    const uint8_t *body = frame + WIFI_MGMT_HEADER_LEN;
    wifi_parse_fixed_fields(body, out->subtype, out);

    const size_t ie_offset = WIFI_MGMT_HEADER_LEN + fixed_len;
    const wireshark_parse_status_t ie_status =
        wireshark_wifi_ie_parse(frame + ie_offset, len - ie_offset, &out->ies);

    out->diag.consumed = ie_offset + out->ies.diag.consumed;
    out->diag.output_limited = out->ies.diag.output_limited;
    out->diag.truncated = out->ies.diag.truncated;
    out->diag.malformed = out->diag.malformed || out->ies.diag.malformed;

    if (ie_status == WIRESHARK_PARSE_TRUNCATED) {
        out->diag.error_offset = ie_offset + out->ies.diag.error_offset;
        out->diag.status = WIRESHARK_PARSE_TRUNCATED;
    } else if (out->diag.malformed) {
        if (out->diag.error_offset == SIZE_MAX) {
            out->diag.error_offset = ie_offset + out->ies.diag.error_offset;
        }
        out->diag.status = WIRESHARK_PARSE_MALFORMED;
    } else if (ie_status == WIRESHARK_PARSE_PARTIAL) {
        out->diag.error_offset = len;
        out->diag.status = WIRESHARK_PARSE_PARTIAL;
    } else {
        out->diag.error_offset = len;
        out->diag.status = WIRESHARK_PARSE_OK;
    }

    return out->diag.status;
}
