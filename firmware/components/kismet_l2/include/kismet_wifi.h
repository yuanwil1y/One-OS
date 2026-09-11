#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KISMET_WIFI_DEFAULT_MAX_DEVICES 64u
#define KISMET_WIFI_DEFAULT_MAX_SSIDS 48u
#define KISMET_WIFI_DEFAULT_MAX_RELATIONS 64u
#define KISMET_WIFI_DEFAULT_MAX_SSID_LINKS 96u
#define KISMET_WIFI_DEFAULT_DEVICE_IDLE_MS 120000u
#define KISMET_WIFI_DEFAULT_SSID_IDLE_MS 120000u
#define KISMET_WIFI_DEFAULT_RELATION_IDLE_MS 60000u

#define KISMET_WIFI_HARD_MAX_DEVICES 128u
#define KISMET_WIFI_HARD_MAX_SSIDS 96u
#define KISMET_WIFI_HARD_MAX_RELATIONS 128u
#define KISMET_WIFI_HARD_MAX_SSID_LINKS 192u

#define KISMET_WIFI_MAX_CHANNELS 14u
#define KISMET_WIFI_MAX_SSID_LEN 32u
#define KISMET_WIFI_MAX_FRAME_COPY 256u

#define KISMET_WIFI_DEFAULT_DWELL_MS 200u
#define KISMET_WIFI_DEFAULT_DURATION_MS 5000u
#define KISMET_WIFI_DEFAULT_FRAME_COPY_LEN 256u
#define KISMET_WIFI_DEFAULT_RX_QUEUE_DEPTH 8u
#define KISMET_WIFI_HARD_MAX_RX_QUEUE_DEPTH 16u
#define KISMET_WIFI_HARD_MAX_DURATION_MS 600000u

#define KISMET_WIFI_PARTIAL_DEVICE_EVICTION (1u << 0)
#define KISMET_WIFI_PARTIAL_SSID_EVICTION (1u << 1)
#define KISMET_WIFI_PARTIAL_RELATION_EVICTION (1u << 2)
#define KISMET_WIFI_PARTIAL_LINK_EVICTION (1u << 3)
#define KISMET_WIFI_PARTIAL_RX_DROP (1u << 4)
#define KISMET_WIFI_PARTIAL_TRUNCATED_FRAME (1u << 5)
#define KISMET_WIFI_PARTIAL_MALFORMED_FRAME (1u << 6)

#define KISMET_WIFI_RELATION_EVIDENCE_TO_DS (1u << 0)
#define KISMET_WIFI_RELATION_EVIDENCE_FROM_DS (1u << 1)
#define KISMET_WIFI_RELATION_EVIDENCE_ASSOC_REQUEST (1u << 2)
#define KISMET_WIFI_RELATION_EVIDENCE_ASSOC_RESPONSE (1u << 3)

#define KISMET_WIFI_ROLE_FLAG_AP (1u << 0)
#define KISMET_WIFI_ROLE_FLAG_STA (1u << 1)
#define KISMET_WIFI_ROLE_FLAG_PEER (1u << 2)

#define KISMET_WIFI_SSID_FLAG_HIDDEN (1u << 0)

typedef struct kismet_wifi_tracker kismet_wifi_tracker_t;
typedef struct kismet_wifi_session kismet_wifi_session_t;

typedef struct {
    uint16_t max_devices;
    uint16_t max_ssids;
    uint16_t max_relations;
    uint16_t max_ssid_links;
    uint32_t device_idle_ms;
    uint32_t ssid_idle_ms;
    uint32_t relation_idle_ms;
} kismet_wifi_tracker_config_t;

typedef enum {
    KISMET_WIFI_ROLE_UNKNOWN = 0,
    KISMET_WIFI_ROLE_AP,
    KISMET_WIFI_ROLE_STA,
    KISMET_WIFI_ROLE_PEER,
} kismet_wifi_role_t;

typedef enum {
    KISMET_WIFI_FRAME_OTHER = 0,
    KISMET_WIFI_FRAME_MANAGEMENT,
    KISMET_WIFI_FRAME_DATA,
} kismet_wifi_frame_class_t;

typedef enum {
    KISMET_WIFI_SSID_NONE = 0,
    KISMET_WIFI_SSID_ADVERTISED,
    KISMET_WIFI_SSID_RESPONDED,
    KISMET_WIFI_SSID_PROBED,
} kismet_wifi_ssid_observation_kind_t;

/* Shallow RF observation consumed by the bounded tracker. Deep management/IE
 * parsing is intentionally outside this family; applications may populate the
 * optional SSID fields from an independent parser before calling ingest. */
typedef struct {
    uint64_t seen_ms;
    uint8_t device[6];
    kismet_wifi_role_t role;
    int8_t rssi;
    uint8_t channel;
    kismet_wifi_frame_class_t frame_class;

    bool has_ssid;
    kismet_wifi_ssid_observation_kind_t ssid_kind;
    uint8_t ssid[KISMET_WIFI_MAX_SSID_LEN];
    uint8_t ssid_len;

    bool has_relation;
    uint8_t relation_client[6];
    uint8_t relation_bssid[6];
    uint32_t relation_evidence_flags;
} kismet_wifi_observation_t;

typedef struct {
    uint8_t mac[6];
    kismet_wifi_role_t role;
    uint32_t role_flags;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
    uint32_t management_count;
    uint32_t data_count;
    int8_t rssi_last;
    int8_t rssi_min;
    int8_t rssi_max;
    uint8_t last_channel;
    uint16_t channel_bitmap;
} kismet_wifi_device_t;

typedef struct {
    uint8_t ssid[KISMET_WIFI_MAX_SSID_LEN];
    uint8_t ssid_len;
    uint32_t flags;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
    uint16_t advertiser_count;
    uint16_t responder_count;
    uint16_t prober_count;
} kismet_wifi_ssid_t;

typedef struct {
    uint8_t client[6];
    uint8_t bssid[6];
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint32_t seen_count;
    uint32_t evidence_flags;
} kismet_wifi_relation_t;

typedef struct {
    uint32_t device_evictions;
    uint32_t ssid_evictions;
    uint32_t relation_evictions;
    uint32_t ssid_link_evictions;
    uint32_t rx_queue_drops;
    uint32_t malformed_frames;
    uint32_t truncated_frames;
    uint32_t partial_flags;
} kismet_wifi_tracker_stats_t;

esp_err_t kismet_wifi_tracker_create(const kismet_wifi_tracker_config_t *config,
                                     kismet_wifi_tracker_t **out_tracker);
void kismet_wifi_tracker_destroy(kismet_wifi_tracker_t *tracker);
void kismet_wifi_tracker_reset(kismet_wifi_tracker_t *tracker);
esp_err_t kismet_wifi_tracker_ingest(kismet_wifi_tracker_t *tracker,
                                     const kismet_wifi_observation_t *observation);
void kismet_wifi_tracker_expire(kismet_wifi_tracker_t *tracker, uint64_t now_ms);
size_t kismet_wifi_tracker_device_count(const kismet_wifi_tracker_t *tracker);
size_t kismet_wifi_tracker_ssid_count(const kismet_wifi_tracker_t *tracker);
size_t kismet_wifi_tracker_relation_count(const kismet_wifi_tracker_t *tracker);
esp_err_t kismet_wifi_tracker_get_device(const kismet_wifi_tracker_t *tracker,
                                         size_t index,
                                         kismet_wifi_device_t *out_device);
esp_err_t kismet_wifi_tracker_get_ssid(const kismet_wifi_tracker_t *tracker,
                                       size_t index,
                                       kismet_wifi_ssid_t *out_ssid);

/*
 * Look up the SSID currently associated with one device (BSSID).
 *
 * A device and its SSID live in separate bounded tables joined by links, so the
 * application cannot name an AP from the device list alone. Returns
 * ESP_ERR_NOT_FOUND when the device is unknown, has no SSID link, or its linked
 * SSID is a hidden one (ssid_len 0), in which case *out_len is 0.
 *
 * `out_ssid` is the caller's buffer of at least KISMET_WIFI_MAX_SSID_LEN bytes;
 * bytes are copied, never referenced.
 */
esp_err_t kismet_wifi_tracker_get_device_ssid(const kismet_wifi_tracker_t *tracker,
                                              const uint8_t mac[6],
                                              uint8_t *out_ssid,
                                              size_t out_capacity,
                                              uint8_t *out_len,
                                              bool *out_hidden);
esp_err_t kismet_wifi_tracker_get_relation(const kismet_wifi_tracker_t *tracker,
                                           size_t index,
                                           kismet_wifi_relation_t *out_relation);
esp_err_t kismet_wifi_tracker_get_stats(const kismet_wifi_tracker_t *tracker,
                                        kismet_wifi_tracker_stats_t *out_stats);

typedef enum {
    KISMET_WIFI_CAPTURE_OTHER = 0,
    KISMET_WIFI_CAPTURE_MANAGEMENT,
    KISMET_WIFI_CAPTURE_DATA,
} kismet_wifi_capture_type_t;

typedef struct {
    uint64_t seen_ms;
    int8_t rssi;
    uint8_t channel;
    kismet_wifi_capture_type_t type;
    uint16_t original_len; /* 802.11 bytes excluding FCS when native RX supplied it. */
    uint16_t captured_len;
    bool truncated;
    uint8_t bytes[KISMET_WIFI_MAX_FRAME_COPY];
} kismet_wifi_frame_t;

typedef void (*kismet_wifi_frame_callback_t)(const kismet_wifi_frame_t *frame,
                                              void *user_ctx);

typedef struct {
    uint8_t channels[KISMET_WIFI_MAX_CHANNELS];
    uint8_t channel_count; /* 0 = derive legal 2.4 GHz channels from country config. */
    uint16_t dwell_ms;     /* 0 = KISMET_WIFI_DEFAULT_DWELL_MS. */
    uint32_t duration_ms;  /* 0 = KISMET_WIFI_DEFAULT_DURATION_MS. */
    uint16_t frame_copy_len; /* 0 = KISMET_WIFI_DEFAULT_FRAME_COPY_LEN. */
    uint8_t rx_queue_depth;  /* 0 = KISMET_WIFI_DEFAULT_RX_QUEUE_DEPTH. */
} kismet_wifi_session_config_t;

typedef struct {
    esp_err_t native_error;
    bool canceled;
    uint32_t elapsed_ms;
    uint32_t channel_switches;
    uint32_t frames_received;
    uint32_t frames_delivered;
    uint32_t rx_queue_drops;
    uint32_t malformed_frames;
    uint32_t truncated_frames;
    uint32_t partial_flags;
} kismet_wifi_session_result_t;

/* v1 owns the native Wi-Fi driver lifecycle and refuses to start if Wi-Fi is
 * already initialized. This avoids clobbering provisioning/STA mode or an
 * unknown promiscuous callback. The application owns cross-family scheduling
 * and reconnects STA after this bounded RF stage when needed. */
esp_err_t kismet_wifi_session_start(const kismet_wifi_session_config_t *config,
                                    kismet_wifi_tracker_t *tracker,
                                    kismet_wifi_frame_callback_t frame_cb,
                                    void *frame_cb_ctx,
                                    kismet_wifi_session_t **out_session);
esp_err_t kismet_wifi_session_cancel(kismet_wifi_session_t *session);
esp_err_t kismet_wifi_session_wait(kismet_wifi_session_t *session, uint32_t timeout_ms);
esp_err_t kismet_wifi_session_get_result(const kismet_wifi_session_t *session,
                                         kismet_wifi_session_result_t *out_result);
void kismet_wifi_session_destroy(kismet_wifi_session_t *session);

/* Tracker enumeration/get calls are intended after the mutating session has
 * completed (or otherwise under application-owned serialization). */

#ifdef __cplusplus
}
#endif
