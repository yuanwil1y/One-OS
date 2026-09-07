#include "wireshark_l2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void test_ble_golden(void)
{
    static const uint8_t adv[] = {
        0x02, 0x01, 0x06,
        0x05, 0x09, 'T', 'e', 's', 't',
        0x05, 0x03, 0x0d, 0x18, 0x0f, 0x18,
        0x02, 0x0a, 0xf4,
        0x03, 0x19, 0xc1, 0x03,
        0x07, 0xff, 0x4c, 0x00, 0x02, 0x15, 0xaa, 0xbb,
        0x06, 0x16, 0x0f, 0x18, 0x01, 0x02, 0x03,
    };

    wireshark_ble_adv_t out;
    assert(wireshark_ble_adv_parse(adv, sizeof(adv), &out) == WIRESHARK_PARSE_OK);
    assert(out.ad_structure_count == 7);
    assert(out.flags_present && out.flags == 0x06);
    assert(out.name_present && out.name_complete && out.name_len == 4);
    assert(memcmp(out.name, "Test", 4) == 0);
    assert(out.uuid16_count == 2);
    assert(out.uuid16[0] == 0x180d && out.uuid16[1] == 0x180f);
    assert(out.uuid16_complete);
    assert(out.tx_power_present && out.tx_power_dbm == -12);
    assert(out.appearance_present && out.appearance == 0x03c1);
    assert(out.manufacturer_data_count == 1);
    assert(out.manufacturer_data[0].company_id == 0x004c);
    assert(out.manufacturer_data[0].data_len == 4);
    assert(out.manufacturer_data[0].data[0] == 0x02);
    assert(out.service_data_count == 1);
    assert(out.service_data[0].uuid_len == 2);
    assert(out.service_data[0].uuid[0] == 0x0f && out.service_data[0].uuid[1] == 0x18);
    assert(out.service_data[0].data_len == 3);
    assert(out.diag.consumed == sizeof(adv));
}

static void test_ble_zero_terminator(void)
{
    static const uint8_t adv[] = {0x00, 0xff, 0xff};
    wireshark_ble_adv_t out;
    assert(wireshark_ble_adv_parse(adv, sizeof(adv), &out) == WIRESHARK_PARSE_OK);
    assert(out.ad_structure_count == 0);
    assert(out.diag.consumed == 1);
}

static void test_ble_truncated_and_malformed(void)
{
    static const uint8_t truncated[] = {0x05, 0x09, 'A'};
    static const uint8_t bad_mfg[] = {0x02, 0xff, 0x4c};
    static const uint8_t bad_uuid16[] = {0x04, 0x03, 0x0d, 0x18, 0xff};
    static const uint8_t bad_flags[] = {0x03, 0x01, 0x06, 0x07};
    wireshark_ble_adv_t out;

    assert(wireshark_ble_adv_parse(truncated, sizeof(truncated), &out) == WIRESHARK_PARSE_TRUNCATED);
    assert(out.diag.truncated && out.diag.error_offset == 0);

    assert(wireshark_ble_adv_parse(bad_mfg, sizeof(bad_mfg), &out) == WIRESHARK_PARSE_MALFORMED);
    assert(out.diag.malformed);

    assert(wireshark_ble_adv_parse(bad_uuid16, sizeof(bad_uuid16), &out) == WIRESHARK_PARSE_MALFORMED);
    assert(out.uuid16_count == 1 && out.uuid16[0] == 0x180d);

    assert(wireshark_ble_adv_parse(bad_flags, sizeof(bad_flags), &out) == WIRESHARK_PARSE_MALFORMED);
    assert(!out.flags_present);
}

static void test_ble_output_caps(void)
{
    uint8_t long_name[2 + 65];
    long_name[0] = 66;
    long_name[1] = 0x09;
    memset(long_name + 2, 'N', 65);

    wireshark_ble_adv_t out;
    assert(wireshark_ble_adv_parse(long_name, sizeof(long_name), &out) == WIRESHARK_PARSE_PARTIAL);
    assert(out.name_present && out.name_truncated);
    assert(out.name_len == WIRESHARK_BLE_NAME_MAX);
    assert(out.diag.output_limited);

    uint8_t many_uuid16[2 + (WIRESHARK_BLE_MAX_UUID16 + 1) * 2];
    many_uuid16[0] = (uint8_t)(1 + (WIRESHARK_BLE_MAX_UUID16 + 1) * 2);
    many_uuid16[1] = 0x03;
    for (size_t i = 0; i < WIRESHARK_BLE_MAX_UUID16 + 1; ++i) {
        many_uuid16[2 + i * 2] = (uint8_t)i;
        many_uuid16[3 + i * 2] = 0x18;
    }
    assert(wireshark_ble_adv_parse(many_uuid16, sizeof(many_uuid16), &out) == WIRESHARK_PARSE_PARTIAL);
    assert(out.uuid16_count == WIRESHARK_BLE_MAX_UUID16);
}

static size_t put_mgmt_header(uint8_t *frame, uint8_t subtype)
{
    memset(frame, 0, 24);
    frame[0] = (uint8_t)(subtype << 4);
    memset(frame + 4, 0xff, 6);
    static const uint8_t sa[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(frame + 10, sa, 6);
    memcpy(frame + 16, sa, 6);
    frame[22] = 0x10;
    return 24;
}

static size_t append_ie(uint8_t *buf, size_t offset, uint8_t id,
                        const uint8_t *value, size_t value_len)
{
    assert(value_len <= 255);
    buf[offset++] = id;
    buf[offset++] = (uint8_t)value_len;
    if (value_len != 0) {
        memcpy(buf + offset, value, value_len);
    }
    return offset + value_len;
}

static void test_wifi_beacon_golden(void)
{
    uint8_t frame[256];
    size_t n = put_mgmt_header(frame, 8);

    static const uint8_t fixed[12] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        0x64, 0x00,
        0x11, 0x00,
    };
    memcpy(frame + n, fixed, sizeof(fixed));
    n += sizeof(fixed);

    static const uint8_t ssid[] = {'T', 'e', 's', 't'};
    static const uint8_t rates[] = {0x82, 0x84, 0x8b, 0x96};
    static const uint8_t channel[] = {6};
    static const uint8_t rsn[] = {
        0x01, 0x00,
        0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00,
        0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00,
        0x00, 0x0f, 0xac, 0x02,
        0x0c, 0x00,
    };
    uint8_t htcap[26] = {0x6e, 0x01};
    uint8_t htop[22] = {6};
    static const uint8_t hecap[] = {35, 0};
    static const uint8_t heop[] = {36, 0};
    static const uint8_t vendor[] = {0x00, 0x50, 0xf2, 0x04, 0x10, 0x4a};

    n = append_ie(frame, n, 0, ssid, sizeof(ssid));
    n = append_ie(frame, n, 1, rates, sizeof(rates));
    n = append_ie(frame, n, 3, channel, sizeof(channel));
    n = append_ie(frame, n, 48, rsn, sizeof(rsn));
    n = append_ie(frame, n, 45, htcap, sizeof(htcap));
    n = append_ie(frame, n, 61, htop, sizeof(htop));
    n = append_ie(frame, n, 255, hecap, sizeof(hecap));
    n = append_ie(frame, n, 255, heop, sizeof(heop));
    n = append_ie(frame, n, 221, vendor, sizeof(vendor));

    wireshark_wifi_mgmt_t out;
    assert(wireshark_wifi_mgmt_parse(frame, n, &out) == WIRESHARK_PARSE_OK);
    assert(out.subtype == 8);
    assert(out.capability_present && out.capability == 0x0011);
    assert(out.beacon_interval_present && out.beacon_interval_tu == 100);
    assert(out.timestamp_present && out.timestamp == UINT64_C(0x0807060504030201));
    assert(out.ies.ssid_present && !out.ies.ssid_hidden && out.ies.ssid_len == 4);
    assert(memcmp(out.ies.ssid, "Test", 4) == 0);
    assert(out.ies.channel_present && out.ies.channel == 6);
    assert(out.ies.rate_count == 4 && out.ies.rates[0].basic);
    assert(out.ies.rsn.present && out.ies.rsn.version == 1);
    assert(out.ies.rsn.group_cipher.type == 4);
    assert(out.ies.rsn.pairwise_count == 1 && out.ies.rsn.pairwise[0].type == 4);
    assert(out.ies.rsn.akm_count == 1 && out.ies.rsn.akm[0].type == 2);
    assert(out.ies.rsn.capabilities_present && out.ies.rsn.capabilities == 0x000c);
    assert(out.ies.ht_capabilities_present && out.ies.ht_capability_info == 0x016e);
    assert(out.ies.ht_operation_present && out.ies.ht_primary_channel == 6);
    assert(out.ies.he_capabilities_present && out.ies.he_operation_present);
    assert(out.ies.vendor_ie_count == 1);
    assert(out.ies.vendor_ie[0].type_present && out.ies.vendor_ie[0].type == 0x04);
    assert(out.diag.consumed == n);
}

static void test_wifi_probe_hidden(void)
{
    uint8_t frame[32];
    size_t n = put_mgmt_header(frame, 4);
    frame[n++] = 0;
    frame[n++] = 0;

    wireshark_wifi_mgmt_t out;
    assert(wireshark_wifi_mgmt_parse(frame, n, &out) == WIRESHARK_PARSE_OK);
    assert(out.ies.ssid_present && out.ies.ssid_hidden && out.ies.ssid_len == 0);
}

static void test_wifi_truncated_and_unsupported(void)
{
    uint8_t frame[32];
    wireshark_wifi_mgmt_t out;

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;
    assert(wireshark_wifi_mgmt_parse(frame, 24, &out) == WIRESHARK_PARSE_UNSUPPORTED);

    put_mgmt_header(frame, 13);
    assert(wireshark_wifi_mgmt_parse(frame, 24, &out) == WIRESHARK_PARSE_UNSUPPORTED);

    put_mgmt_header(frame, 8);
    assert(wireshark_wifi_mgmt_parse(frame, 23, &out) == WIRESHARK_PARSE_TRUNCATED);

    size_t n = put_mgmt_header(frame, 4);
    frame[n++] = 0;
    assert(wireshark_wifi_mgmt_parse(frame, n, &out) == WIRESHARK_PARSE_TRUNCATED);
    assert(out.diag.error_offset == 24);
}

static void test_wifi_ie_malformed(void)
{
    wireshark_wifi_ie_summary_t out;

    uint8_t bad_ssid[35] = {0, 33};
    memset(bad_ssid + 2, 'S', 33);
    assert(wireshark_wifi_ie_parse(bad_ssid, sizeof(bad_ssid), &out) == WIRESHARK_PARSE_MALFORMED);
    assert(!out.ssid_present);

    static const uint8_t bad_ds[] = {3, 2, 1, 6};
    assert(wireshark_wifi_ie_parse(bad_ds, sizeof(bad_ds), &out) == WIRESHARK_PARSE_MALFORMED);

    static const uint8_t short_value[] = {0, 4, 'A', 'B'};
    assert(wireshark_wifi_ie_parse(short_value, sizeof(short_value), &out) == WIRESHARK_PARSE_TRUNCATED);

    static const uint8_t bad_rsn[] = {
        48, 14,
        1, 0,
        0, 0x0f, 0xac, 4,
        2, 0,
        0, 0x0f, 0xac, 4,
        1, 0,
    };
    assert(wireshark_wifi_ie_parse(bad_rsn, sizeof(bad_rsn), &out) == WIRESHARK_PARSE_MALFORMED);
    assert(out.diag.malformed);
}

static void test_wifi_output_caps(void)
{
    uint8_t ies[128];
    size_t n = 0;
    uint8_t many_rates[WIRESHARK_WIFI_MAX_RATES + 1];
    for (size_t i = 0; i < ARRAY_SIZE(many_rates); ++i) {
        many_rates[i] = (uint8_t)(2 + i);
    }
    n = append_ie(ies, n, 50, many_rates, sizeof(many_rates));

    wireshark_wifi_ie_summary_t out;
    assert(wireshark_wifi_ie_parse(ies, n, &out) == WIRESHARK_PARSE_PARTIAL);
    assert(out.rate_count == WIRESHARK_WIFI_MAX_RATES);
    assert(out.diag.output_limited);

    n = 0;
    static const uint8_t vendor[] = {0x00, 0x50, 0xf2, 1};
    for (size_t i = 0; i < WIRESHARK_WIFI_MAX_VENDOR_IE + 1; ++i) {
        n = append_ie(ies, n, 221, vendor, sizeof(vendor));
    }
    assert(wireshark_wifi_ie_parse(ies, n, &out) == WIRESHARK_PARSE_PARTIAL);
    assert(out.vendor_ie_count == WIRESHARK_WIFI_MAX_VENDOR_IE);
}

static void test_wifi_rsn_capacity_and_ht_fallback(void)
{
    uint8_t ies[160];
    size_t n = 0;
    uint8_t rsn[96];
    size_t r = 0;
    rsn[r++] = 1; rsn[r++] = 0;
    rsn[r++] = 0; rsn[r++] = 0x0f; rsn[r++] = 0xac; rsn[r++] = 4;
    rsn[r++] = 9; rsn[r++] = 0;
    for (int i = 0; i < 9; ++i) {
        rsn[r++] = 0; rsn[r++] = 0x0f; rsn[r++] = 0xac; rsn[r++] = (uint8_t)(i + 1);
    }
    rsn[r++] = 1; rsn[r++] = 0;
    rsn[r++] = 0; rsn[r++] = 0x0f; rsn[r++] = 0xac; rsn[r++] = 2;
    n = append_ie(ies, n, 48, rsn, r);

    uint8_t htop[22] = {11};
    n = append_ie(ies, n, 61, htop, sizeof(htop));

    wireshark_wifi_ie_summary_t out;
    assert(wireshark_wifi_ie_parse(ies, n, &out) == WIRESHARK_PARSE_PARTIAL);
    assert(out.rsn.pairwise_count == WIRESHARK_WIFI_MAX_PAIRWISE);
    assert(out.channel_present && out.channel == 11);
}

static void test_prefix_safety(void)
{
    static const uint8_t ble[] = {
        0x02, 0x01, 0x06,
        0x05, 0x09, 'T', 'e', 's', 't',
        0x05, 0x03, 0x0d, 0x18, 0x0f, 0x18,
    };
    wireshark_ble_adv_t bout;
    for (size_t n = 0; n <= sizeof(ble); ++n) {
        wireshark_parse_status_t status = wireshark_ble_adv_parse(ble, n, &bout);
        assert(status == WIRESHARK_PARSE_OK || status == WIRESHARK_PARSE_TRUNCATED);
        assert(bout.diag.consumed <= n);
    }

    uint8_t frame[64];
    size_t n = put_mgmt_header(frame, 8);
    memset(frame + n, 0, 12);
    frame[n + 8] = 100;
    n += 12;
    static const uint8_t ssid[] = {'A', 'P'};
    n = append_ie(frame, n, 0, ssid, sizeof(ssid));

    wireshark_wifi_mgmt_t wout;
    for (size_t cut = 0; cut <= n; ++cut) {
        wireshark_parse_status_t status = wireshark_wifi_mgmt_parse(frame, cut, &wout);
        assert(status == WIRESHARK_PARSE_OK ||
               status == WIRESHARK_PARSE_TRUNCATED ||
               status == WIRESHARK_PARSE_UNSUPPORTED);
        assert(wout.diag.consumed <= cut || cut < 2);
    }
}

static void test_null_contract(void)
{
    wireshark_ble_adv_t b;
    wireshark_wifi_ie_summary_t i;
    wireshark_wifi_mgmt_t w;

    assert(wireshark_ble_adv_parse(NULL, 0, &b) == WIRESHARK_PARSE_OK);
    assert(wireshark_ble_adv_parse(NULL, 1, &b) == WIRESHARK_PARSE_MALFORMED);
    assert(wireshark_ble_adv_parse(NULL, 0, NULL) == WIRESHARK_PARSE_MALFORMED);

    assert(wireshark_wifi_ie_parse(NULL, 0, &i) == WIRESHARK_PARSE_OK);
    assert(wireshark_wifi_ie_parse(NULL, 1, &i) == WIRESHARK_PARSE_MALFORMED);
    assert(wireshark_wifi_mgmt_parse(NULL, 1, &w) == WIRESHARK_PARSE_MALFORMED);
}

int main(void)
{
    test_ble_golden();
    test_ble_zero_terminator();
    test_ble_truncated_and_malformed();
    test_ble_output_caps();
    test_wifi_beacon_golden();
    test_wifi_probe_hidden();
    test_wifi_truncated_and_unsupported();
    test_wifi_ie_malformed();
    test_wifi_output_caps();
    test_wifi_rsn_capacity_and_ht_fallback();
    test_prefix_safety();
    test_null_contract();

    puts("wireshark_l2 host tests: PASS");
    return 0;
}
