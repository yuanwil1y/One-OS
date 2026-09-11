#pragma once

/*
 * One-OS Device DB (.nbdb) container: format constants and the validating
 * reader.
 *
 * The full normative specification is docs/device-db-format.md. This header is
 * the machine-readable half of it and is shared by the firmware reader and the
 * host validator, so both classify a damaged or incompatible file identically.
 *
 * Design rules this code follows:
 *   - every multi-byte field is assembled byte by byte; struct layout is never
 *     part of the format and no struct is memcpy'd out of the file;
 *   - all arithmetic on file-supplied values is done in 64-bit and range-checked
 *     before use, so an overflowing offset+length is detected, not wrapped;
 *   - the reader never trusts a length, count or offset it has not validated;
 *   - the index is an accelerator only. A caller must confirm a match against
 *     the fingerprint record itself.
 *
 * Platform independent: no ESP-IDF/FreeRTOS/LVGL headers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- container constants ---------------- */

#define DEVICE_DB_MAGIC_0 'N'
#define DEVICE_DB_MAGIC_1 'B'
#define DEVICE_DB_MAGIC_2 'D'
#define DEVICE_DB_MAGIC_3 'B'

#define DEVICE_DB_FORMAT_VERSION 1u
#define DEVICE_DB_SCHEMA_VERSION 1u
#define DEVICE_DB_READER_ABI 1u

#define DEVICE_DB_HEADER_SIZE 128u
#define DEVICE_DB_PROVENANCE_SIZE 24u
#define DEVICE_DB_IDENTITY_SIZE 24u
#define DEVICE_DB_PROFILE_SIZE 96u
#define DEVICE_DB_FINGERPRINT_SIZE 40u
#define DEVICE_DB_RECIPE_SIZE 72u
#define DEVICE_DB_INDEX_BUCKET_SIZE 32u
#define DEVICE_DB_INDEX_ENTRIES_PER_BUCKET 3u

/* Hard cap: a malicious or corrupt size field cannot make a reader attempt a
 * 4 GiB allocation. */
#define DEVICE_DB_MAX_FILE_BYTES (16u * 1024u * 1024u)

/* ---------------- enumerations ---------------- */

typedef enum {
    DEVICE_DB_OK = 0,
    DEVICE_DB_ERR_NOT_A_DB,      /* magic mismatch */
    DEVICE_DB_ERR_TRUNCATED,     /* shorter than a header, or a region is cut */
    DEVICE_DB_ERR_INCOMPATIBLE,  /* version/ABI/flags we cannot honour */
    DEVICE_DB_ERR_CORRUPT,       /* checksum, bounds, reference or field error */
    DEVICE_DB_ERR_TOO_LARGE,     /* declared size exceeds the hard cap */
} device_db_status_t;

const char *device_db_status_name(device_db_status_t status);
const char *device_db_status_detail(void);

typedef enum {
    DEVICE_DB_PROTO_BLE = 0,
    DEVICE_DB_PROTO_WIFI = 1,
    DEVICE_DB_PROTO_MDNS = 2,
    DEVICE_DB_PROTO_SSDP = 3,
    DEVICE_DB_PROTO_LAN = 4,
    DEVICE_DB_PROTO_ZIGBEE = 5,
    DEVICE_DB_PROTO_MATTER = 6,
    DEVICE_DB_PROTO_ESPHOME = 7,
    DEVICE_DB_PROTO_COUNT = 8,
} device_db_protocol_t;

typedef enum {
    DEVICE_DB_REUSE_COPY = 0,
    DEVICE_DB_REUSE_PORT = 1,
    DEVICE_DB_REUSE_CLEAN_ROOM = 2,
    DEVICE_DB_REUSE_REFERENCE_ONLY = 3,
} device_db_reuse_t;

typedef enum {
    DEVICE_DB_IDENTITY_NONE = 0,
    DEVICE_DB_IDENTITY_ZIGBEE_IEEE = 1,
    DEVICE_DB_IDENTITY_MATTER_NODE_FABRIC = 2,
    DEVICE_DB_IDENTITY_VENDOR_PROTOCOL_UUID = 3,
    DEVICE_DB_IDENTITY_ESPHOME_NODE_NAME = 4,
    DEVICE_DB_IDENTITY_MDNS_TXT_IDENTIFIER = 5,
    DEVICE_DB_IDENTITY_BLE_PUBLIC_ADDRESS = 6,
    DEVICE_DB_IDENTITY_BLE_RANDOM_ADDRESS = 7, /* never safe */
    DEVICE_DB_IDENTITY_WIFI_BSSID = 8,          /* never safe */
    DEVICE_DB_IDENTITY_IP_ADDRESS = 9,          /* never safe */
    DEVICE_DB_IDENTITY_RSSI = 10,               /* never safe */
    DEVICE_DB_IDENTITY_SSID = 11,               /* never safe */
    DEVICE_DB_IDENTITY_MODEL_NAME = 12,         /* never safe */
    DEVICE_DB_IDENTITY_MATTER_VID_PID = 13,     /* never safe alone */
} device_db_identity_kind_t;

typedef enum {
    DEVICE_DB_STRENGTH_UNSAFE = 0,
    DEVICE_DB_STRENGTH_WEAK = 1,
    DEVICE_DB_STRENGTH_STRONG = 2,
} device_db_identity_strength_t;

typedef enum {
    DEVICE_DB_BACKEND_NONE = 0,
    DEVICE_DB_BACKEND_PASSIVE_VALUE = 1,
    DEVICE_DB_BACKEND_BLE_GATT = 2,
    DEVICE_DB_BACKEND_ESPHOME_API = 3,
    DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE = 4,
    DEVICE_DB_BACKEND_ZIGBEE_COMMAND = 5,
    DEVICE_DB_BACKEND_MATTER_ATTRIBUTE = 6,
    DEVICE_DB_BACKEND_MATTER_COMMAND = 7,
} device_db_backend_t;

typedef enum {
    DEVICE_DB_DOMAIN_SENSOR = 0,
    DEVICE_DB_DOMAIN_BINARY_SENSOR = 1,
    DEVICE_DB_DOMAIN_SWITCH = 2,
    DEVICE_DB_DOMAIN_LIGHT = 3,
    DEVICE_DB_DOMAIN_BUTTON = 4,
    DEVICE_DB_DOMAIN_NUMBER = 5,
    DEVICE_DB_DOMAIN_SELECT = 6,
    DEVICE_DB_DOMAIN_CLIMATE = 7,
} device_db_domain_t;

#define DEVICE_DB_POLICY_PASSIVE_ONLY (1u << 0)
#define DEVICE_DB_POLICY_SAFE_READ (1u << 1)
#define DEVICE_DB_POLICY_AUTH_REQUIRED (1u << 2)
#define DEVICE_DB_POLICY_USER_ACTION_REQUIRED (1u << 3)
#define DEVICE_DB_POLICY_WRITE_CONTROL (1u << 4)

#define DEVICE_DB_NO_INDEX 0xFFFFFFFFu
#define DEVICE_DB_NO_ENDPOINT 0xFFFFu

/* ---------------- validated views ---------------- */

/* A string reference resolved to a bounded, non-NUL-terminated view inside the
 * caller's buffer. Nothing is copied and nothing is allocated. */
typedef struct {
    const uint8_t *data;
    uint8_t length;
} device_db_string_t;

typedef struct {
    uint32_t source_id;
    uint8_t reuse;
    uint8_t license;
    uint32_t source_revision;
    device_db_string_t source_name;
    device_db_string_t review;
} device_db_provenance_t;

typedef struct {
    uint8_t kind;
    uint8_t strength;
    uint32_t profile_id;
    uint32_t flags;
    device_db_string_t key;
} device_db_identity_t;

typedef struct {
    uint32_t profile_id;
    device_db_string_t vendor;
    device_db_string_t model;
    device_db_string_t display_name;
    device_db_string_t icon;
    uint32_t provenance_index;
    uint32_t theengs_decoder_id; /* DEVICE_DB_NO_INDEX = none */
    uint32_t zha_quirk_id;       /* DEVICE_DB_NO_INDEX = none */
    uint32_t first_recipe_index;
    uint32_t recipe_count;
    uint32_t policy_flags;
    uint32_t identity_first;
    uint32_t identity_count;
    uint8_t protocol_mask;
    bool writable;
    uint32_t fingerprint_first;
    uint32_t fingerprint_count;
} device_db_profile_t;

typedef struct {
    uint8_t protocol;
    uint8_t match_kind;
    uint32_t key_hash;
    device_db_string_t key;
    uint32_t profile_id;
    uint32_t mask_length;
    device_db_string_t mask;
    uint32_t flags;
} device_db_fingerprint_t;

typedef struct {
    uint32_t profile_id;
    device_db_string_t domain;
    device_db_string_t name;
    device_db_string_t device_class;
    device_db_string_t unit;
    uint8_t domain_id;
    uint8_t backend;
    uint32_t read_source_id;
    uint32_t write_target_id; /* DEVICE_DB_NO_INDEX = read-only */
    uint32_t codec_id;
    uint32_t subscription_id;
    uint16_t endpoint;
    uint16_t cluster;
    uint16_t attribute;
    uint16_t command;
    int32_t min_value;
    int32_t max_value;
    uint32_t scale;
    uint32_t flags;
} device_db_recipe_t;

typedef struct {
    uint32_t content_version;
    uint64_t build_timestamp;
    uint32_t profile_count;
    uint32_t fingerprint_count;
    uint32_t recipe_count;
    uint32_t identity_count;
    uint32_t provenance_count;
    uint32_t index_bucket_count;
} device_db_info_t;

/*
 * A validated database.
 *
 * `open` walks the whole structure once and rejects inconsistent files up front,
 * so later accessors cannot observe a malformed record. It performs no
 * allocation: everything points into the caller's buffer, which must stay alive
 * and unmodified for as long as the handle is used.
 */
typedef struct {
    const uint8_t *bytes;
    uint32_t size;
    device_db_info_t info;
    uint32_t strings_offset;
    uint32_t strings_length;
    uint32_t provenance_offset;
    uint32_t identity_offset;
    uint32_t profiles_offset;
    uint32_t fingerprints_offset;
    uint32_t recipes_offset;
    uint32_t index_offset;
} device_db_t;

/*
 * Validate a file image.
 *
 * Returns DEVICE_DB_OK and fills `out` on success. On failure `out` is left in a
 * defined but unusable state, and `device_db_status_detail()` describes the exact
 * failed check so a test or an operator can tell corruption from incompatibility.
 */
device_db_status_t device_db_open(const uint8_t *bytes, uint32_t size,
                                  device_db_t *out);

/* Detail of the most recent failed device_db_open, e.g. "profile_id duplicate".
 * Returns "" when the last call succeeded. Not thread safe by design: it is a
 * diagnostic aid, not part of the data path. */
const char *device_db_status_detail(void);

/* 32-bit FNV-1a over `length` bytes; the same function the generator uses. */
uint32_t device_db_key_hash(const uint8_t *data, size_t length);

/*
 * Canonical fingerprint key hash.
 *
 * The record stores the human-readable key declaration, but `key_hash` covers the
 * CANONICAL form: trimmed, lowercased, with ':' and '-' separators removed. That
 * rule is part of the format contract, not an implementation detail, because the
 * generator, this reader and the host validator must all agree or a valid file
 * looks corrupt.
 *
 * Use this function rather than device_db_key_hash() when checking or producing a
 * fingerprint key_hash.
 */
uint32_t device_db_canonical_key_hash(const uint8_t *data, size_t length);

/*
 * CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF).
 *
 * Exposed because it is part of the format contract: the generator and the host
 * validator must compute exactly this value, so it is not an implementation
 * detail. The firmware reader uses it internally too.
 */
uint32_t device_db_crc32(const uint8_t *data, size_t length);

/* Accessors. All return false when the index is out of range or a string would
 * not fit the caller's expectations. Item accessors do not re-validate the whole
 * file; they rely on device_db_open having done so. */
bool device_db_get_info(const device_db_t *db, device_db_info_t *out);
bool device_db_get_profile(const device_db_t *db, uint32_t index,
                           device_db_profile_t *out);
bool device_db_get_fingerprint(const device_db_t *db, uint32_t index,
                               device_db_fingerprint_t *out);
bool device_db_get_recipe(const device_db_t *db, uint32_t index,
                          device_db_recipe_t *out);
bool device_db_get_identity(const device_db_t *db, uint32_t index,
                            device_db_identity_t *out);
bool device_db_get_provenance(const device_db_t *db, uint32_t index,
                              device_db_provenance_t *out);

/* Find a profile by its id. Returns false when absent. */
bool device_db_find_profile(const device_db_t *db, uint32_t profile_id,
                            device_db_profile_t *out);

/*
 * Candidate lookup through the index.
 *
 * Writes at most `capacity` fingerprint indices into `out_indices` and reports
 * how many were written in `*out_count`. `*out_may_be_incomplete` is set when the
 * bucket overflowed, meaning the caller must also scan that protocol's
 * fingerprints rather than conclude "no match".
 *
 * This performs no matching: it only narrows the candidate set.
 */
bool device_db_index_lookup(const device_db_t *db, uint8_t protocol,
                            uint32_t key_hash, uint32_t *out_indices,
                            uint32_t capacity, uint32_t *out_count,
                            bool *out_may_be_incomplete);

#ifdef __cplusplus
}
#endif
