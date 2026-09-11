/*
 * One-OS Device DB (.nbdb) container: validating reader.
 *
 * See docs/device-db-format.md. Platform independent: this file is compiled into
 * the firmware and into the host validator, so both classify a damaged file
 * identically.
 *
 * Every value read out of the file is treated as hostile until checked. Offsets
 * and lengths are combined in 64-bit arithmetic so an overflowing sum is
 * detected rather than wrapped, and no field is used to index before its range
 * has been established.
 */

#include "device_db_format.h"

#include <string.h>

static const char *s_detail = "";

const char *device_db_status_name(device_db_status_t status)
{
    switch (status) {
    case DEVICE_DB_OK:               return "ok";
    case DEVICE_DB_ERR_NOT_A_DB:     return "not_a_db";
    case DEVICE_DB_ERR_TRUNCATED:    return "truncated";
    case DEVICE_DB_ERR_INCOMPATIBLE: return "incompatible";
    case DEVICE_DB_ERR_CORRUPT:      return "corrupt";
    case DEVICE_DB_ERR_TOO_LARGE:    return "too_large";
    default:                         return "invalid";
    }
}

const char *device_db_status_detail(void)
{
    return s_detail;
}

/* ---------------- little-endian readers ---------------- */

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64(const uint8_t *p)
{
    return (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
}

/* ---------------- CRC-32 (IEEE) ---------------- */

uint32_t device_db_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0u; i < length; ++i) {
        crc ^= (uint32_t)data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

uint32_t device_db_key_hash(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u; /* FNV-1a 32 */

    if (data == NULL) {
        return 0u;
    }
    for (size_t i = 0u; i < length; ++i) {
        hash ^= (uint32_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ---------------- validation helpers ---------------- */

#define FAIL(status, reason)                                                  \
    do {                                                                      \
        s_detail = (reason);                                                  \
        return (status);                                                      \
    } while (0)

/* True when [offset, offset+length) fits inside [base, base+limit). */
static bool range_ok(uint32_t base, uint32_t limit, uint32_t offset,
                     uint32_t length)
{
    uint64_t end = (uint64_t)offset + (uint64_t)length;
    return (uint64_t)offset >= (uint64_t)base && end <= (uint64_t)limit;
}

/* Resolve a StringRef (offset,length packed into a u32 cell) against the string
 * region. `required` rejects the empty string. */
static bool resolve_string(const device_db_t *db, uint32_t ref, bool required,
                           device_db_string_t *out)
{
    uint8_t length = (uint8_t)(ref & 0xFFu);
    uint32_t offset = ref >> 8;

    out->data = NULL;
    out->length = 0u;

    if (length == 0u) {
        return !required;
    }
    if (!range_ok(db->strings_offset, db->strings_offset + db->strings_length,
                  db->strings_offset + offset, length)) {
        return false;
    }
    out->data = db->bytes + db->strings_offset + offset;
    out->length = length;
    return true;
}

static bool is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

/* ---------------- cross-field rules ---------------- */

static const char *recipe_rule_violation(uint8_t backend, uint32_t write_target,
                                         uint16_t endpoint, uint16_t cluster,
                                         uint16_t attribute, uint16_t command)
{
    const bool has_write = write_target != DEVICE_DB_NO_INDEX;

    /* Not every parameter applies to every backend. Each case uses what it needs
     * and the rest are explicitly unused rather than silently ignored, which is
     * also what keeps this warning-clean under -Werror. */
    (void)has_write;
    (void)endpoint;
    (void)cluster;
    (void)attribute;
    (void)command;

    switch (backend) {
    case DEVICE_DB_BACKEND_NONE:
        /* A recipe with no backend is presentation-only; it must never look
         * controllable. */
        if (has_write) return "NONE recipe must not set write_target_id";
        return NULL;
    case DEVICE_DB_BACKEND_PASSIVE_VALUE:
        /* Broadcast-only telemetry: writable is meaningless. */
        if (has_write) return "PASSIVE_VALUE recipe must not set write_target_id";
        return NULL;
    case DEVICE_DB_BACKEND_BLE_GATT:
    case DEVICE_DB_BACKEND_ESPHOME_API:
        /* These backends can be read-only (a sensor over GATT or the Native
         * API), so a write target is optional. Whether the entity is writable is
         * decided by the profile's `writable` flag plus the presence of a write
         * target, not by the backend alone. */
        return NULL;
    case DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE:
    case DEVICE_DB_BACKEND_ZIGBEE_COMMAND:
        if (endpoint == DEVICE_DB_NO_ENDPOINT) return "zigbee recipe needs endpoint";
        if (cluster == DEVICE_DB_NO_ENDPOINT) return "zigbee recipe needs cluster";
        if (backend == DEVICE_DB_BACKEND_ZIGBEE_COMMAND &&
            command == DEVICE_DB_NO_ENDPOINT) {
            return "zigbee command recipe needs command id";
        }
        if (backend == DEVICE_DB_BACKEND_ZIGBEE_ATTRIBUTE &&
            attribute == DEVICE_DB_NO_ENDPOINT) {
            return "zigbee attribute recipe needs attribute id";
        }
        return NULL;
    case DEVICE_DB_BACKEND_MATTER_ATTRIBUTE:
    case DEVICE_DB_BACKEND_MATTER_COMMAND:
        if (endpoint == DEVICE_DB_NO_ENDPOINT) return "matter recipe needs endpoint";
        if (backend == DEVICE_DB_BACKEND_MATTER_COMMAND &&
            command == DEVICE_DB_NO_ENDPOINT) {
            return "matter command recipe needs command id";
        }
        if (backend == DEVICE_DB_BACKEND_MATTER_ATTRIBUTE &&
            attribute == DEVICE_DB_NO_ENDPOINT) {
            return "matter attribute recipe needs attribute id";
        }
        return NULL;
    default:
        return "unknown recipe backend";
    }
}

static bool identity_kind_is_never_safe(uint8_t kind)
{
    switch (kind) {
    case DEVICE_DB_IDENTITY_BLE_RANDOM_ADDRESS:
    case DEVICE_DB_IDENTITY_WIFI_BSSID:
    case DEVICE_DB_IDENTITY_IP_ADDRESS:
    case DEVICE_DB_IDENTITY_RSSI:
    case DEVICE_DB_IDENTITY_SSID:
    case DEVICE_DB_IDENTITY_MODEL_NAME:
    case DEVICE_DB_IDENTITY_MATTER_VID_PID:
        return true;
    default:
        return false;
    }
}

static bool identity_kind_known(uint8_t kind)
{
    return kind <= (uint8_t)DEVICE_DB_IDENTITY_MATTER_VID_PID;
}

static bool protocol_known(uint8_t protocol)
{
    return protocol < (uint8_t)DEVICE_DB_PROTO_COUNT;
}

static bool domain_known(uint8_t domain)
{
    return domain <= (uint8_t)DEVICE_DB_DOMAIN_CLIMATE;
}

/* ---------------- open / validate ---------------- */

device_db_status_t device_db_open(const uint8_t *bytes, uint32_t size,
                                  device_db_t *out)
{
    device_db_info_t info;
    uint32_t header_crc;
    uint32_t body_crc;

    s_detail = "";

    if (out == NULL) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "null output");
    }
    memset(out, 0, sizeof(*out));

    if (bytes == NULL) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "null buffer");
    }
    if (size < DEVICE_DB_HEADER_SIZE) {
        FAIL(DEVICE_DB_ERR_TRUNCATED, "smaller than the 128-byte header");
    }
    if (size > DEVICE_DB_MAX_FILE_BYTES) {
        FAIL(DEVICE_DB_ERR_TOO_LARGE, "file exceeds the 16 MiB cap");
    }

    if (bytes[0] != DEVICE_DB_MAGIC_0 || bytes[1] != DEVICE_DB_MAGIC_1 ||
        bytes[2] != DEVICE_DB_MAGIC_2 || bytes[3] != DEVICE_DB_MAGIC_3) {
        FAIL(DEVICE_DB_ERR_NOT_A_DB, "magic mismatch");
    }

    /* Version and ABI gates come before any structural trust. */
    if (read_u16(bytes + 4) != DEVICE_DB_FORMAT_VERSION ||
        read_u16(bytes + 6) != DEVICE_DB_SCHEMA_VERSION) {
        FAIL(DEVICE_DB_ERR_INCOMPATIBLE, "format or schema version not supported");
    }
    if (read_u32(bytes + 8) > DEVICE_DB_READER_ABI) {
        FAIL(DEVICE_DB_ERR_INCOMPATIBLE, "reader ABI newer than this reader");
    }
    if (read_u32(bytes + 12) != 0u) {
        FAIL(DEVICE_DB_ERR_INCOMPATIBLE, "reserved flag bits set");
    }

    if (read_u32(bytes + 16) != size) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "file_length does not match the actual size");
    }

    header_crc = device_db_crc32(bytes, 120u);
    if (header_crc != read_u32(bytes + 120)) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "header_crc32 mismatch");
    }
    body_crc = device_db_crc32(bytes + DEVICE_DB_HEADER_SIZE,
                               (size_t)(size - DEVICE_DB_HEADER_SIZE));
    if (body_crc != read_u32(bytes + 116)) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "body_crc32 mismatch");
    }

    memset(&info, 0, sizeof(info));
    info.content_version = read_u32(bytes + 20);
    info.build_timestamp = read_u64(bytes + 24);
    info.profile_count = read_u32(bytes + 32);
    info.fingerprint_count = read_u32(bytes + 36);
    info.recipe_count = read_u32(bytes + 40);
    info.identity_count = read_u32(bytes + 44);
    info.provenance_count = read_u32(bytes + 48);
    info.index_bucket_count = read_u16(bytes + 52);

    if (info.index_bucket_count != 0u && !is_power_of_two(info.index_bucket_count)) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "index_bucket_count is not a power of two");
    }

    out->bytes = bytes;
    out->size = size;
    out->info = info;
    out->strings_offset = read_u32(bytes + 56);
    out->strings_length = read_u32(bytes + 60);
    out->provenance_offset = read_u32(bytes + 64);
    out->identity_offset = read_u32(bytes + 72);
    out->profiles_offset = read_u32(bytes + 80);
    out->fingerprints_offset = read_u32(bytes + 88);
    out->recipes_offset = read_u32(bytes + 96);
    out->index_offset = read_u32(bytes + 104);

    /* Region table: offset, length, expected record size, count. */
    {
        const uint32_t region_offset[6] = {
            out->strings_offset,      out->provenance_offset,
            out->identity_offset,     out->profiles_offset,
            out->fingerprints_offset, out->recipes_offset,
        };
        const uint32_t region_length[6] = {
            out->strings_length,
            read_u32(bytes + 68),
            read_u32(bytes + 76),
            read_u32(bytes + 84),
            read_u32(bytes + 92),
            read_u32(bytes + 100),
        };
        const uint32_t record_size[6] = {
            0u, /* strings: variable length */
            DEVICE_DB_PROVENANCE_SIZE,
            DEVICE_DB_IDENTITY_SIZE,
            DEVICE_DB_PROFILE_SIZE,
            DEVICE_DB_FINGERPRINT_SIZE,
            DEVICE_DB_RECIPE_SIZE,
        };
        const uint32_t record_count[6] = {
            0u,
            info.provenance_count,
            info.identity_count,
            info.profile_count,
            info.fingerprint_count,
            info.recipe_count,
        };

        for (int i = 0; i < 6; ++i) {
            if (!range_ok(DEVICE_DB_HEADER_SIZE, size, region_offset[i],
                          region_length[i])) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "region lies outside the file");
            }
            if (record_size[i] != 0u) {
                /* Fixed-size region: the declared length must be exactly
                 * record_size * count, in 64-bit arithmetic so a hostile count
                 * cannot wrap the product into a small accepted length. */
                if ((uint64_t)record_size[i] * (uint64_t)record_count[i] !=
                    (uint64_t)region_length[i]) {
                    FAIL(DEVICE_DB_ERR_CORRUPT,
                         "record count does not match region length");
                }
            } else if (record_count[i] != 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT,
                     "variable-length region must not declare a record count");
            }
        }
    }

    /* Index region: bucket array plus a possible overflow run. The overflow run
     * is allowed to extend past bucket_count * bucket_size, so the region length
     * is only checked for containment and alignment here. */
    if (info.index_bucket_count != 0u) {
        uint64_t buckets_bytes = (uint64_t)info.index_bucket_count *
                                 DEVICE_DB_INDEX_BUCKET_SIZE;
        if (buckets_bytes > (uint64_t)read_u32(bytes + 108)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "index shorter than its bucket array");
        }
    }
    if (!range_ok(DEVICE_DB_HEADER_SIZE, size, out->index_offset,
                  read_u32(bytes + 108))) {
        FAIL(DEVICE_DB_ERR_CORRUPT, "index region lies outside the file");
    }

    /* ---- profiles ---- */
    {
        uint32_t previous_id = 0u;

        for (uint32_t i = 0u; i < info.profile_count; ++i) {
            const uint8_t *p = bytes + out->profiles_offset +
                               (size_t)i * DEVICE_DB_PROFILE_SIZE;
            uint32_t profile_id = read_u32(p + 0);
            uint32_t provenance_index = read_u32(p + 20);
            uint32_t first_recipe = read_u32(p + 32);
            uint32_t recipe_count = read_u32(p + 36);
            uint32_t policy = read_u32(p + 40);
            uint32_t identity_first = read_u32(p + 44);
            uint32_t identity_count = read_u32(p + 48);
            uint8_t protocol_mask = p[52];
            uint8_t writable = p[53];
            uint32_t fingerprint_first = read_u32(p + 56);
            uint32_t fingerprint_count = read_u32(p + 60);

            if (profile_id == 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile_id 0 is reserved");
            }
            /* Strictly ascending: catches both duplicates and an unsorted file,
             * which is what makes binary search and determinism possible. */
            if (profile_id <= previous_id) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile ids must ascend without duplicates");
            }
            previous_id = profile_id;

            if (provenance_index >= info.provenance_count) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile provenance_index out of range");
            }
            if ((uint64_t)first_recipe + recipe_count > info.recipe_count) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile recipe range out of bounds");
            }
            if ((uint64_t)identity_first + identity_count > info.identity_count) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile identity range out of bounds");
            }
            if ((uint64_t)fingerprint_first + fingerprint_count >
                info.fingerprint_count) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile fingerprint range out of bounds");
            }
            if (fingerprint_count == 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile has no fingerprint and could never match");
            }
            if ((protocol_mask & 0xFFu) == 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile protocol_mask is empty");
            }
            /* Both halves of the control rule from the product spec. */
            if ((policy & DEVICE_DB_POLICY_WRITE_CONTROL) != 0u &&
                (policy & DEVICE_DB_POLICY_USER_ACTION_REQUIRED) == 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT,
                     "WRITE_CONTROL requires USER_ACTION_REQUIRED");
            }
            if (writable != 0u && writable != 1u) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "profile writable is not a boolean");
            }
            /* A writable profile must actually have a writable recipe. */
            if (writable == 1u) {
                bool found_writable = false;

                for (uint32_t r = 0u; r < recipe_count; ++r) {
                    const uint8_t *rp = bytes + out->recipes_offset +
                                        ((size_t)first_recipe + r) *
                                            DEVICE_DB_RECIPE_SIZE;
                    uint8_t backend = rp[21];
                    if (backend != DEVICE_DB_BACKEND_NONE &&
                        backend != DEVICE_DB_BACKEND_PASSIVE_VALUE) {
                        found_writable = true;
                        break;
                    }
                }
                if (!found_writable) {
                    FAIL(DEVICE_DB_ERR_CORRUPT,
                         "writable profile has no writable recipe");
                }
            }
        }
    }

    /* ---- identity rules ---- */
    for (uint32_t i = 0u; i < info.identity_count; ++i) {
        const uint8_t *p = bytes + out->identity_offset +
                           (size_t)i * DEVICE_DB_IDENTITY_SIZE;
        uint8_t kind = p[0];
        uint8_t strength = p[1];
        uint32_t profile_id = read_u32(p + 8);
        device_db_string_t key;

        if (!identity_kind_known(kind)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "unknown identity rule kind");
        }
        if (strength > (uint8_t)DEVICE_DB_STRENGTH_STRONG) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "identity rule strength out of range");
        }
        /* An identity kind that must never merge across protocols may only be
         * recorded as UNSAFE; it can then be shown to a user, never used. */
        if (identity_kind_is_never_safe(kind) &&
            strength != (uint8_t)DEVICE_DB_STRENGTH_UNSAFE) {
            FAIL(DEVICE_DB_ERR_CORRUPT,
                 "unsafe identity kind declared with a usable strength");
        }
        if (kind == (uint8_t)DEVICE_DB_IDENTITY_NONE &&
            strength != (uint8_t)DEVICE_DB_STRENGTH_UNSAFE) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "NONE identity rule must be UNSAFE");
        }
        if (profile_id == 0u) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "identity rule has no profile id");
        }
        if (!resolve_string(out, read_u32(p + 4), false, &key)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "identity rule key string out of range");
        }
    }

    /* ---- fingerprints ---- */
    for (uint32_t i = 0u; i < info.fingerprint_count; ++i) {
        const uint8_t *p = bytes + out->fingerprints_offset +
                           (size_t)i * DEVICE_DB_FINGERPRINT_SIZE;
        uint8_t protocol = p[0];
        uint8_t match_kind = p[1];
        uint32_t key_hash = read_u32(p + 4);
        uint32_t profile_id = read_u32(p + 12);
        uint32_t mask_length = read_u32(p + 16);
        device_db_string_t key;
        device_db_string_t mask;

        if (!protocol_known(protocol)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint protocol out of range");
        }
        if (match_kind > 2u) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint match_kind out of range");
        }
        if (profile_id == 0u) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint has no profile id");
        }
        if (!resolve_string(out, read_u32(p + 8), true, &key)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint key string invalid");
        }
        if (!resolve_string(out, read_u32(p + 20), match_kind == 2u, &mask)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint mask string invalid");
        }
        if (match_kind == 2u && mask_length != mask.length) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint mask_length disagrees with mask");
        }
        /* The stored hash must be the hash of the stored key, otherwise a reader
         * would compare a hash that can never match the record it names. */
        if (device_db_key_hash(key.data, key.length) != key_hash) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint key_hash does not match its key");
        }
    }

    /* ---- recipes ---- */
    for (uint32_t i = 0u; i < info.recipe_count; ++i) {
        const uint8_t *p = bytes + out->recipes_offset +
                           (size_t)i * DEVICE_DB_RECIPE_SIZE;
        uint32_t profile_id = read_u32(p + 0);
        uint8_t domain_id = p[20];
        uint8_t backend = p[21];
        uint32_t write_target = read_u32(p + 28);
        uint16_t endpoint = read_u16(p + 40);
        uint16_t cluster = read_u16(p + 42);
        uint16_t attribute = read_u16(p + 44);
        uint16_t command = read_u16(p + 46);
        int32_t min_value = (int32_t)read_u32(p + 48);
        int32_t max_value = (int32_t)read_u32(p + 52);
        const char *violation;
        device_db_string_t domain;

        if (profile_id == 0u) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "recipe has no profile id");
        }
        if (!domain_known(domain_id)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "recipe domain_id out of range");
        }
        if (backend > (uint8_t)DEVICE_DB_BACKEND_MATTER_COMMAND) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "recipe backend out of range");
        }
        if (!resolve_string(out, read_u32(p + 4), true, &domain)) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "recipe domain string invalid");
        }
        if (min_value > max_value) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "recipe min_value exceeds max_value");
        }

        violation = recipe_rule_violation(backend, write_target, endpoint, cluster,
                                         attribute, command);
        if (violation != NULL) {
            FAIL(DEVICE_DB_ERR_CORRUPT, violation);
        }
    }

    /* ---- cross-reference: every recipe/fingerprint/identity names a profile ----
     * The profiles region ascends, so a binary search is valid. */
    {
        for (uint32_t i = 0u; i < info.recipe_count; ++i) {
            const uint8_t *p = bytes + out->recipes_offset +
                               (size_t)i * DEVICE_DB_RECIPE_SIZE;
            uint32_t wanted = read_u32(p + 0);
            uint32_t lo = 0u;
            uint32_t hi = info.profile_count;
            bool found = false;

            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;
                const uint8_t *mp = bytes + out->profiles_offset +
                                    (size_t)mid * DEVICE_DB_PROFILE_SIZE;
                uint32_t have = read_u32(mp + 0);
                if (have == wanted) {
                    found = true;
                    break;
                }
                if (have < wanted) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            if (!found) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "recipe references an unknown profile");
            }
        }

        for (uint32_t i = 0u; i < info.identity_count; ++i) {
            const uint8_t *p = bytes + out->identity_offset +
                               (size_t)i * DEVICE_DB_IDENTITY_SIZE;
            uint32_t wanted = read_u32(p + 8);
            uint32_t lo = 0u;
            uint32_t hi = info.profile_count;
            bool found = false;

            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;
                const uint8_t *mp = bytes + out->profiles_offset +
                                    (size_t)mid * DEVICE_DB_PROFILE_SIZE;
                uint32_t have = read_u32(mp + 0);
                if (have == wanted) {
                    found = true;
                    break;
                }
                if (have < wanted) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            if (!found) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "identity rule references an unknown profile");
            }
        }

        for (uint32_t i = 0u; i < info.fingerprint_count; ++i) {
            const uint8_t *p = bytes + out->fingerprints_offset +
                               (size_t)i * DEVICE_DB_FINGERPRINT_SIZE;
            uint32_t wanted = read_u32(p + 12);
            uint32_t lo = 0u;
            uint32_t hi = info.profile_count;
            bool found = false;

            while (lo < hi) {
                uint32_t mid = lo + (hi - lo) / 2u;
                const uint8_t *mp = bytes + out->profiles_offset +
                                    (size_t)mid * DEVICE_DB_PROFILE_SIZE;
                uint32_t have = read_u32(mp + 0);
                if (have == wanted) {
                    found = true;
                    break;
                }
                if (have < wanted) {
                    lo = mid + 1u;
                } else {
                    hi = mid;
                }
            }
            if (!found) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "fingerprint references an unknown profile");
            }
        }
    }

    /* ---- index entries point at real fingerprints ---- */
    if (info.index_bucket_count != 0u) {
        uint32_t index_length = read_u32(bytes + 108);
        uint32_t bucket_bytes = info.index_bucket_count * DEVICE_DB_INDEX_BUCKET_SIZE;
        uint32_t overflow_bytes = index_length - bucket_bytes;
        uint32_t overflow_entries = overflow_bytes / 10u;

        if (overflow_bytes % 10u != 0u) {
            FAIL(DEVICE_DB_ERR_CORRUPT, "index overflow run is not entry aligned");
        }
        for (uint32_t b = 0u; b < info.index_bucket_count; ++b) {
            const uint8_t *bucket = bytes + out->index_offset +
                                    (size_t)b * DEVICE_DB_INDEX_BUCKET_SIZE;
            uint16_t entries = read_u16(bucket);
            uint16_t flags = read_u16(bucket + 30);

            if (entries > DEVICE_DB_INDEX_ENTRIES_PER_BUCKET) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "index bucket entry count out of range");
            }
            if ((flags & ~1u) != 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT, "index bucket has unknown flag bits");
            }
            for (uint16_t e = 0u; e < entries; ++e) {
                uint32_t fp_index = read_u32(bucket + 2u + (size_t)e * 10u + 4u);
                if (fp_index >= info.fingerprint_count) {
                    FAIL(DEVICE_DB_ERR_CORRUPT,
                         "index entry points outside the fingerprint region");
                }
            }
        }
        /* Spot-check the overflow run length is consistent with the flag. */
        for (uint32_t b = 0u; b < info.index_bucket_count; ++b) {
            const uint8_t *bucket = bytes + out->index_offset +
                                    (size_t)b * DEVICE_DB_INDEX_BUCKET_SIZE;
            uint16_t flags = read_u16(bucket + 30);
            if ((flags & 1u) != 0u && overflow_entries == 0u) {
                FAIL(DEVICE_DB_ERR_CORRUPT,
                     "bucket marks overflow but the overflow run is empty");
            }
        }
    }

    return DEVICE_DB_OK;
}

/* ---------------- accessors ---------------- */

bool device_db_get_info(const device_db_t *db, device_db_info_t *out)
{
    if (db == NULL || out == NULL || db->bytes == NULL) {
        return false;
    }
    *out = db->info;
    return true;
}

bool device_db_get_profile(const device_db_t *db, uint32_t index,
                           device_db_profile_t *out)
{
    const uint8_t *p;

    if (db == NULL || out == NULL || db->bytes == NULL ||
        index >= db->info.profile_count) {
        return false;
    }
    p = db->bytes + db->profiles_offset + (size_t)index * DEVICE_DB_PROFILE_SIZE;
    memset(out, 0, sizeof(*out));
    out->profile_id = read_u32(p + 0);
    (void)resolve_string(db, read_u32(p + 4), false, &out->vendor);
    (void)resolve_string(db, read_u32(p + 8), false, &out->model);
    (void)resolve_string(db, read_u32(p + 12), false, &out->display_name);
    (void)resolve_string(db, read_u32(p + 16), false, &out->icon);
    out->provenance_index = read_u32(p + 20);
    out->theengs_decoder_id = read_u32(p + 24);
    out->zha_quirk_id = read_u32(p + 28);
    out->first_recipe_index = read_u32(p + 32);
    out->recipe_count = read_u32(p + 36);
    out->policy_flags = read_u32(p + 40);
    out->identity_first = read_u32(p + 44);
    out->identity_count = read_u32(p + 48);
    out->protocol_mask = p[52];
    out->writable = p[53] != 0u;
    out->fingerprint_first = read_u32(p + 56);
    out->fingerprint_count = read_u32(p + 60);
    return true;
}

bool device_db_find_profile(const device_db_t *db, uint32_t profile_id,
                            device_db_profile_t *out)
{
    uint32_t lo = 0u;
    uint32_t hi;

    if (db == NULL || out == NULL || db->bytes == NULL || profile_id == 0u) {
        return false;
    }
    hi = db->info.profile_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        device_db_profile_t candidate;
        if (!device_db_get_profile(db, mid, &candidate)) {
            return false;
        }
        if (candidate.profile_id == profile_id) {
            *out = candidate;
            return true;
        }
        if (candidate.profile_id < profile_id) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return false;
}

bool device_db_get_fingerprint(const device_db_t *db, uint32_t index,
                               device_db_fingerprint_t *out)
{
    const uint8_t *p;

    if (db == NULL || out == NULL || db->bytes == NULL ||
        index >= db->info.fingerprint_count) {
        return false;
    }
    p = db->bytes + db->fingerprints_offset +
        (size_t)index * DEVICE_DB_FINGERPRINT_SIZE;
    memset(out, 0, sizeof(*out));
    out->protocol = p[0];
    out->match_kind = p[1];
    out->key_hash = read_u32(p + 4);
    (void)resolve_string(db, read_u32(p + 8), false, &out->key);
    out->profile_id = read_u32(p + 12);
    out->mask_length = read_u32(p + 16);
    (void)resolve_string(db, read_u32(p + 20), false, &out->mask);
    out->flags = read_u32(p + 28);
    return true;
}

bool device_db_get_recipe(const device_db_t *db, uint32_t index,
                          device_db_recipe_t *out)
{
    const uint8_t *p;

    if (db == NULL || out == NULL || db->bytes == NULL ||
        index >= db->info.recipe_count) {
        return false;
    }
    p = db->bytes + db->recipes_offset + (size_t)index * DEVICE_DB_RECIPE_SIZE;
    memset(out, 0, sizeof(*out));
    out->profile_id = read_u32(p + 0);
    (void)resolve_string(db, read_u32(p + 4), false, &out->domain);
    (void)resolve_string(db, read_u32(p + 8), false, &out->name);
    (void)resolve_string(db, read_u32(p + 12), false, &out->device_class);
    (void)resolve_string(db, read_u32(p + 16), false, &out->unit);
    out->domain_id = p[20];
    out->backend = p[21];
    out->read_source_id = read_u32(p + 24);
    out->write_target_id = read_u32(p + 28);
    out->codec_id = read_u32(p + 32);
    out->subscription_id = read_u32(p + 36);
    out->endpoint = read_u16(p + 40);
    out->cluster = read_u16(p + 42);
    out->attribute = read_u16(p + 44);
    out->command = read_u16(p + 46);
    out->min_value = (int32_t)read_u32(p + 48);
    out->max_value = (int32_t)read_u32(p + 52);
    out->scale = read_u32(p + 56);
    out->flags = read_u32(p + 60);
    return true;
}

bool device_db_get_identity(const device_db_t *db, uint32_t index,
                            device_db_identity_t *out)
{
    const uint8_t *p;

    if (db == NULL || out == NULL || db->bytes == NULL ||
        index >= db->info.identity_count) {
        return false;
    }
    p = db->bytes + db->identity_offset + (size_t)index * DEVICE_DB_IDENTITY_SIZE;
    memset(out, 0, sizeof(*out));
    out->kind = p[0];
    out->strength = p[1];
    (void)resolve_string(db, read_u32(p + 4), false, &out->key);
    out->profile_id = read_u32(p + 8);
    out->flags = read_u32(p + 12);
    return true;
}

bool device_db_get_provenance(const device_db_t *db, uint32_t index,
                              device_db_provenance_t *out)
{
    const uint8_t *p;

    if (db == NULL || out == NULL || db->bytes == NULL ||
        index >= db->info.provenance_count) {
        return false;
    }
    p = db->bytes + db->provenance_offset +
        (size_t)index * DEVICE_DB_PROVENANCE_SIZE;
    memset(out, 0, sizeof(*out));
    out->source_id = read_u32(p + 0);
    out->reuse = p[4];
    out->license = p[5];
    out->source_revision = read_u32(p + 8);
    (void)resolve_string(db, read_u32(p + 12), false, &out->source_name);
    (void)resolve_string(db, read_u32(p + 16), false, &out->review);
    return true;
}

bool device_db_index_lookup(const device_db_t *db, uint8_t protocol,
                            uint32_t key_hash, uint32_t *out_indices,
                            uint32_t capacity, uint32_t *out_count,
                            bool *out_may_be_incomplete)
{
    const uint8_t *bucket;
    uint16_t entries;
    uint16_t flags;
    uint32_t written = 0u;

    if (out_count != NULL) {
        *out_count = 0u;
    }
    if (out_may_be_incomplete != NULL) {
        *out_may_be_incomplete = false;
    }
    if (db == NULL || db->bytes == NULL) {
        return false;
    }
    if (db->info.index_bucket_count == 0u) {
        /* No index: the caller must scan. Say so rather than reporting empty. */
        if (out_may_be_incomplete != NULL) {
            *out_may_be_incomplete = true;
        }
        return true;
    }

    bucket = db->bytes + db->index_offset +
             (size_t)(key_hash & (db->info.index_bucket_count - 1u)) *
                 DEVICE_DB_INDEX_BUCKET_SIZE;
    entries = read_u16(bucket);
    flags = read_u16(bucket + 30);

    if ((flags & 1u) != 0u && out_may_be_incomplete != NULL) {
        *out_may_be_incomplete = true;
    }

    for (uint16_t e = 0u; e < entries; ++e) {
        const uint8_t *entry = bucket + 2u + (size_t)e * 10u;
        uint32_t entry_hash = read_u32(entry);
        uint32_t fp_index = read_u32(entry + 4);

        if (entry_hash != key_hash) {
            continue;
        }
        /* Confirm the protocol through the fingerprint record: the index is only
         * an accelerator, so the record remains the authority. */
        {
            device_db_fingerprint_t fp;
            if (!device_db_get_fingerprint(db, fp_index, &fp)) {
                continue;
            }
            if (fp.protocol != protocol) {
                continue;
            }
        }
        if (out_indices != NULL && written < capacity) {
            out_indices[written] = fp_index;
        }
        ++written;
    }

    if (written > capacity && out_may_be_incomplete != NULL) {
        *out_may_be_incomplete = true;
    }
    if (out_count != NULL) {
        *out_count = written;
    }
    return true;
}
