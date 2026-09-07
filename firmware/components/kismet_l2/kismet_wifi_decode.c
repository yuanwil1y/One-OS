#include "kismet_internal.h"

#include <string.h>

#define DOT11_MIN_HEADER_LEN 24u
#define DOT11_WDS_HEADER_LEN 30u
#define DOT11_ADDR1_OFFSET 4u
#define DOT11_ADDR2_OFFSET 10u
#define DOT11_ADDR3_OFFSET 16u
#define DOT11_ADDR_LEN 6u

#define DOT11_TYPE_MANAGEMENT 0u
#define DOT11_TYPE_CONTROL 1u
#define DOT11_TYPE_DATA 2u

#define DOT11_SUBTYPE_ASSOC_REQ 0u
#define DOT11_SUBTYPE_ASSOC_RESP 1u
#define DOT11_SUBTYPE_REASSOC_REQ 2u
#define DOT11_SUBTYPE_REASSOC_RESP 3u
#define DOT11_SUBTYPE_PROBE_REQ 4u
#define DOT11_SUBTYPE_PROBE_RESP 5u
#define DOT11_SUBTYPE_BEACON 8u

static bool mac_equal(const uint8_t a[DOT11_ADDR_LEN],
                      const uint8_t b[DOT11_ADDR_LEN])
{
    return memcmp(a, b, DOT11_ADDR_LEN) == 0;
}

static bool mac_is_zero(const uint8_t mac[DOT11_ADDR_LEN])
{
    static const uint8_t zero[DOT11_ADDR_LEN] = {0};
    return mac_equal(mac, zero);
}

static bool mac_is_trackable(const uint8_t mac[DOT11_ADDR_LEN])
{
    return !mac_is_zero(mac) && (mac[0] & 0x01u) == 0u;
}

static void init_observation(kismet_wifi_observation_t *obs,
                             uint64_t seen_ms,
                             int8_t rssi,
                             uint8_t channel,
                             kismet_wifi_frame_class_t frame_class)
{
    memset(obs, 0, sizeof(*obs));
    obs->seen_ms = seen_ms;
    obs->rssi = rssi;
    obs->channel = channel;
    obs->frame_class = frame_class;
}

static void set_device(kismet_wifi_observation_t *obs,
                       const uint8_t mac[DOT11_ADDR_LEN],
                       kismet_wifi_role_t role)
{
    if (!mac_is_trackable(mac)) {
        return;
    }
    memcpy(obs->device, mac, DOT11_ADDR_LEN);
    obs->role = role;
}

static void set_relation(kismet_wifi_observation_t *obs,
                         const uint8_t client[DOT11_ADDR_LEN],
                         const uint8_t bssid[DOT11_ADDR_LEN],
                         uint32_t evidence)
{
    if (!mac_is_trackable(client) || !mac_is_trackable(bssid) ||
        mac_equal(client, bssid)) {
        return;
    }
    obs->has_relation = true;
    memcpy(obs->relation_client, client, DOT11_ADDR_LEN);
    memcpy(obs->relation_bssid, bssid, DOT11_ADDR_LEN);
    obs->relation_evidence_flags = evidence;
}

static bool observation_has_device(const kismet_wifi_observation_t *obs)
{
    return mac_is_trackable(obs->device);
}

kismet_wifi_shallow_result_t kismet_wifi_shallow_observe(
    const uint8_t *frame,
    size_t frame_len,
    int8_t rssi,
    uint8_t channel,
    uint64_t seen_ms,
    kismet_wifi_observation_t *out_primary,
    kismet_wifi_observation_t *out_secondary)
{
    if (out_primary == NULL || out_secondary == NULL) {
        return KISMET_WIFI_SHALLOW_MALFORMED;
    }

    init_observation(out_primary, seen_ms, rssi, channel,
                     KISMET_WIFI_FRAME_OTHER);
    init_observation(out_secondary, seen_ms, rssi, channel,
                     KISMET_WIFI_FRAME_OTHER);

    if (frame == NULL || frame_len < 2u) {
        return KISMET_WIFI_SHALLOW_MALFORMED;
    }

    const uint16_t frame_control =
        (uint16_t)frame[0] | ((uint16_t)frame[1] << 8u);
    const uint8_t version = (uint8_t)(frame_control & 0x03u);
    const uint8_t type = (uint8_t)((frame_control >> 2u) & 0x03u);
    const uint8_t subtype = (uint8_t)((frame_control >> 4u) & 0x0fu);
    const bool to_ds = (frame_control & 0x0100u) != 0u;
    const bool from_ds = (frame_control & 0x0200u) != 0u;

    if (version != 0u) {
        return KISMET_WIFI_SHALLOW_MALFORMED;
    }

    if (type == DOT11_TYPE_CONTROL || type == 3u) {
        return KISMET_WIFI_SHALLOW_IGNORED;
    }

    if (frame_len < DOT11_MIN_HEADER_LEN) {
        return KISMET_WIFI_SHALLOW_MALFORMED;
    }

    const uint8_t *addr1 = frame + DOT11_ADDR1_OFFSET;
    const uint8_t *addr2 = frame + DOT11_ADDR2_OFFSET;
    const uint8_t *addr3 = frame + DOT11_ADDR3_OFFSET;
    (void)addr3;

    if (type == DOT11_TYPE_MANAGEMENT) {
        out_primary->frame_class = KISMET_WIFI_FRAME_MANAGEMENT;
        out_secondary->frame_class = KISMET_WIFI_FRAME_MANAGEMENT;

        switch (subtype) {
        case DOT11_SUBTYPE_BEACON:
        case DOT11_SUBTYPE_PROBE_RESP:
            set_device(out_primary, addr2, KISMET_WIFI_ROLE_AP);
            break;

        case DOT11_SUBTYPE_PROBE_REQ:
            set_device(out_primary, addr2, KISMET_WIFI_ROLE_STA);
            break;

        case DOT11_SUBTYPE_ASSOC_REQ:
        case DOT11_SUBTYPE_REASSOC_REQ:
            set_device(out_primary, addr2, KISMET_WIFI_ROLE_STA);
            set_device(out_secondary, addr1, KISMET_WIFI_ROLE_AP);
            set_relation(out_primary, addr2, addr1,
                         KISMET_WIFI_RELATION_EVIDENCE_ASSOC_REQUEST);
            break;

        case DOT11_SUBTYPE_ASSOC_RESP:
        case DOT11_SUBTYPE_REASSOC_RESP:
            set_device(out_primary, addr2, KISMET_WIFI_ROLE_AP);
            set_device(out_secondary, addr1, KISMET_WIFI_ROLE_STA);
            set_relation(out_primary, addr1, addr2,
                         KISMET_WIFI_RELATION_EVIDENCE_ASSOC_RESPONSE);
            break;

        default:
            /* For other management subtypes the transmitter is still a valid
             * nearby identity, but AP/STA role is not inferred from the fixed
             * header alone. */
            set_device(out_primary, addr2, KISMET_WIFI_ROLE_UNKNOWN);
            break;
        }

        return observation_has_device(out_primary) ||
                       observation_has_device(out_secondary)
                   ? KISMET_WIFI_SHALLOW_OK
                   : KISMET_WIFI_SHALLOW_IGNORED;
    }

    if (type != DOT11_TYPE_DATA) {
        return KISMET_WIFI_SHALLOW_IGNORED;
    }

    if (to_ds && from_ds && frame_len < DOT11_WDS_HEADER_LEN) {
        return KISMET_WIFI_SHALLOW_MALFORMED;
    }

    out_primary->frame_class = KISMET_WIFI_FRAME_DATA;
    out_secondary->frame_class = KISMET_WIFI_FRAME_DATA;

    if (to_ds && !from_ds) {
        /* Infrastructure STA -> distribution system: addr1 is BSSID and
         * addr2 is the transmitting station. */
        set_device(out_primary, addr2, KISMET_WIFI_ROLE_STA);
        set_device(out_secondary, addr1, KISMET_WIFI_ROLE_AP);
        set_relation(out_primary, addr2, addr1,
                     KISMET_WIFI_RELATION_EVIDENCE_TO_DS);
    } else if (!to_ds && from_ds) {
        /* Distribution system -> STA: addr2 is BSSID/AP and addr1 is the
         * station when unicast. */
        set_device(out_primary, addr2, KISMET_WIFI_ROLE_AP);
        set_device(out_secondary, addr1, KISMET_WIFI_ROLE_STA);
        set_relation(out_primary, addr1, addr2,
                     KISMET_WIFI_RELATION_EVIDENCE_FROM_DS);
    } else {
        /* IBSS/direct/WDS traffic does not provide a reliable client-BSSID
         * relationship from these bits alone. Track only the transmitter. */
        set_device(out_primary, addr2, KISMET_WIFI_ROLE_PEER);
    }

    return observation_has_device(out_primary) ||
                   observation_has_device(out_secondary)
               ? KISMET_WIFI_SHALLOW_OK
               : KISMET_WIFI_SHALLOW_IGNORED;
}
