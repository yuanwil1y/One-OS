#ifndef WIRESHARK_L2_H
#define WIRESHARK_L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bounded clean-room protocol parsing for One-OS.
 *
 * Wireshark is used only as a field-truth/test-oracle reference. Public
 * structures contain copied protocol evidence only and never expose
 * Wireshark, ESP-IDF, NimBLE, Kismet, or other Level-2-family types.
 */

typedef enum {
    WIRESHARK_PARSE_OK = 0,
    WIRESHARK_PARSE_PARTIAL,
    WIRESHARK_PARSE_MALFORMED,
    WIRESHARK_PARSE_UNSUPPORTED,
    WIRESHARK_PARSE_TRUNCATED,
} wireshark_parse_status_t;

typedef struct {
    wireshark_parse_status_t status;
    size_t consumed;
    size_t error_offset;
    bool malformed;
    bool truncated;
    bool output_limited;
} wireshark_parse_diag_t;

/* ---------------- BLE advertising data ---------------- */

#define WIRESHARK_BLE_NAME_MAX              64u
#define WIRESHARK_BLE_MAX_UUID16            12u
#define WIRESHARK_BLE_MAX_UUID32             6u
#define WIRESHARK_BLE_MAX_UUID128            4u
#define WIRESHARK_BLE_MAX_MFG_DATA           4u
#define WIRESHARK_BLE_MAX_SERVICE_DATA       6u
#define WIRESHARK_BLE_DATA_MAX              32u

typedef struct {
    uint8_t length;
    uint8_t bytes[16]; /* On-wire little-endian UUID byte order. */
} wireshark_ble_uuid128_t;

typedef struct {
    uint16_t company_id;
    uint8_t data_len;
    uint8_t data[WIRESHARK_BLE_DATA_MAX];
    bool data_truncated;
} wireshark_ble_manufacturer_data_t;

typedef struct {
    uint8_t ad_type;   /* 0x16, 0x20, or 0x21. */
    uint8_t uuid_len;  /* 2, 4, or 16 bytes. */
    uint8_t uuid[16];  /* On-wire little-endian UUID byte order. */
    uint8_t data_len;
    uint8_t data[WIRESHARK_BLE_DATA_MAX];
    bool data_truncated;
} wireshark_ble_service_data_t;

typedef struct {
    wireshark_parse_diag_t diag;

    uint16_t ad_structure_count;

    bool flags_present;
    uint8_t flags;

    bool name_present;
    bool name_complete;
    uint8_t name_len;
    uint8_t name[WIRESHARK_BLE_NAME_MAX];
    bool name_truncated;

    bool tx_power_present;
    int8_t tx_power_dbm;

    bool appearance_present;
    uint16_t appearance;

    bool uuid16_complete;
    uint8_t uuid16_count;
    uint16_t uuid16[WIRESHARK_BLE_MAX_UUID16];

    bool uuid32_complete;
    uint8_t uuid32_count;
    uint32_t uuid32[WIRESHARK_BLE_MAX_UUID32];

    bool uuid128_complete;
    uint8_t uuid128_count;
    wireshark_ble_uuid128_t uuid128[WIRESHARK_BLE_MAX_UUID128];

    uint8_t manufacturer_data_count;
    wireshark_ble_manufacturer_data_t manufacturer_data[WIRESHARK_BLE_MAX_MFG_DATA];

    uint8_t service_data_count;
    wireshark_ble_service_data_t service_data[WIRESHARK_BLE_MAX_SERVICE_DATA];
} wireshark_ble_adv_t;

wireshark_parse_status_t
wireshark_ble_adv_parse(const uint8_t *data, size_t len, wireshark_ble_adv_t *out);

/* ---------------- IEEE 802.11 management / IE parsing ---------------- */

#define WIRESHARK_WIFI_SSID_MAX             32u
#define WIRESHARK_WIFI_MAX_RATES            16u
#define WIRESHARK_WIFI_MAX_PAIRWISE          8u
#define WIRESHARK_WIFI_MAX_AKM               8u
#define WIRESHARK_WIFI_MAX_VENDOR_IE         8u
#define WIRESHARK_WIFI_VENDOR_DATA_MAX      32u

#define WIRESHARK_WIFI_CAP_ESS              0x0001u
#define WIRESHARK_WIFI_CAP_IBSS             0x0002u
#define WIRESHARK_WIFI_CAP_PRIVACY          0x0010u
#define WIRESHARK_WIFI_CAP_SHORT_PREAMBLE   0x0020u
#define WIRESHARK_WIFI_CAP_SHORT_SLOT       0x0400u

typedef struct {
    uint8_t rate_500kbps;
    bool basic;
} wireshark_wifi_rate_t;

typedef struct {
    uint8_t oui[3];
    uint8_t type;
} wireshark_wifi_suite_t;

typedef struct {
    bool present;
    uint16_t version;
    wireshark_wifi_suite_t group_cipher;
    uint8_t pairwise_count;
    wireshark_wifi_suite_t pairwise[WIRESHARK_WIFI_MAX_PAIRWISE];
    uint8_t akm_count;
    wireshark_wifi_suite_t akm[WIRESHARK_WIFI_MAX_AKM];
    bool capabilities_present;
    uint16_t capabilities;
} wireshark_wifi_rsn_t;

typedef struct {
    uint8_t oui[3];
    bool type_present;
    uint8_t type;
    uint8_t data_len;
    uint8_t data[WIRESHARK_WIFI_VENDOR_DATA_MAX];
    bool data_truncated;
} wireshark_wifi_vendor_ie_t;

typedef struct {
    wireshark_parse_diag_t diag;

    uint16_t ie_count;

    bool ssid_present;
    bool ssid_hidden;
    uint8_t ssid_len;
    uint8_t ssid[WIRESHARK_WIFI_SSID_MAX];

    bool channel_present;
    uint8_t channel;

    uint8_t rate_count;
    wireshark_wifi_rate_t rates[WIRESHARK_WIFI_MAX_RATES];

    wireshark_wifi_rsn_t rsn;

    bool ht_capabilities_present;
    uint16_t ht_capability_info;
    bool ht_operation_present;
    uint8_t ht_primary_channel;

    bool he_capabilities_present;
    bool he_operation_present;

    uint8_t vendor_ie_count;
    wireshark_wifi_vendor_ie_t vendor_ie[WIRESHARK_WIFI_MAX_VENDOR_IE];
} wireshark_wifi_ie_summary_t;

wireshark_parse_status_t
wireshark_wifi_ie_parse(const uint8_t *ies, size_t len,
                        wireshark_wifi_ie_summary_t *out);

typedef struct {
    wireshark_parse_diag_t diag;

    uint16_t frame_control;
    uint8_t subtype;
    uint8_t destination[6];
    uint8_t source[6];
    uint8_t bssid[6];

    uint16_t sequence_control;

    bool timestamp_present;
    uint64_t timestamp;
    bool beacon_interval_present;
    uint16_t beacon_interval_tu;
    bool capability_present;
    uint16_t capability;

    wireshark_wifi_ie_summary_t ies;
} wireshark_wifi_mgmt_t;

wireshark_parse_status_t
wireshark_wifi_mgmt_parse(const uint8_t *frame, size_t len,
                          wireshark_wifi_mgmt_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WIRESHARK_L2_H */
