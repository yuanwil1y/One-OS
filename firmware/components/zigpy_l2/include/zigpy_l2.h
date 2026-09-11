#ifndef ONE_OS_ZIGPY_L2_H
#define ONE_OS_ZIGPY_L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGPY_MAX_ENDPOINTS 16u
#define ZIGPY_MAX_CLUSTERS_PER_ENDPOINT 24u
#define ZIGPY_MAX_TRANSACTIONS 8u
#define ZIGPY_MAX_VALUE_SIZE 64u
#define ZIGPY_MAX_IDENTITY_LEN 32u

/*
 * Largest retry count this implementation will honour.
 *
 * zigpy_transaction_result_t::attempts is uint8_t and starts at 1, so a retry
 * limit of 255 could never be exceeded by a counter of that width: the counter
 * would wrap to 0 and polling would retry forever. Callers asking for more than
 * this value are clamped down instead of silently generating an unbounded retry
 * loop.
 */
#define ZIGPY_MAX_RETRIES 254u

/*
 * Interview deadlines.
 *
 * An interview only advances when the backend invokes the matching
 * zigpy_interview_*_complete callback. If the backend never calls back (silent
 * device, dropped frame, backend fault) the interview would otherwise stay
 * active forever and block every later interview with ZIGPY_STATUS_BUSY.
 * `phase_timeout_ms` bounds each individual step; `overall_timeout_ms` bounds
 * the whole interview. A zero field selects the corresponding default.
 */
#define ZIGPY_INTERVIEW_DEFAULT_PHASE_TIMEOUT_MS 5000u
#define ZIGPY_INTERVIEW_DEFAULT_OVERALL_TIMEOUT_MS 30000u

typedef struct {
    uint32_t phase_timeout_ms;
    uint32_t overall_timeout_ms;
} zigpy_interview_config_t;

#define ZIGPY_INTERVIEW_COMPLETE_NODE_DESC (1u << 0)
#define ZIGPY_INTERVIEW_COMPLETE_ACTIVE_EP (1u << 1)
#define ZIGPY_INTERVIEW_COMPLETE_SIMPLE_DESC (1u << 2)
#define ZIGPY_INTERVIEW_COMPLETE_IDENTITY (1u << 3)

#define ZIGPY_INTERVIEW_TRUNC_ENDPOINTS (1u << 0)
#define ZIGPY_INTERVIEW_TRUNC_CLUSTERS (1u << 1)
#define ZIGPY_INTERVIEW_TRUNC_IDENTITY (1u << 2)

typedef enum {
    ZIGPY_STATUS_OK = 0,
    ZIGPY_STATUS_INVALID_ARG,
    ZIGPY_STATUS_BUSY,
    ZIGPY_STATUS_NOT_FOUND,
    ZIGPY_STATUS_NO_SPACE,
    ZIGPY_STATUS_TIMEOUT,
    ZIGPY_STATUS_CANCELLED,
    ZIGPY_STATUS_PARTIAL,
    ZIGPY_STATUS_TRUNCATED,
    ZIGPY_STATUS_BACKEND_ERROR,
    ZIGPY_STATUS_STALE,
} zigpy_status_t;

typedef struct {
    uint64_t ieee;
    uint16_t nwk;
} zigpy_device_ref_t;

typedef struct {
    uint8_t logical_type;
    uint8_t mac_capability_flags;
    uint16_t manufacturer_code;
} zigpy_node_descriptor_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    zigpy_status_t descriptor_status;
    uint8_t input_cluster_count;
    uint8_t output_cluster_count;
    uint16_t input_clusters[ZIGPY_MAX_CLUSTERS_PER_ENDPOINT];
    uint16_t output_clusters[ZIGPY_MAX_CLUSTERS_PER_ENDPOINT];
} zigpy_endpoint_snapshot_t;

typedef struct {
    zigpy_device_ref_t device;
    zigpy_node_descriptor_t node_descriptor;
    uint32_t complete_mask;
    uint32_t truncated_mask;
    uint8_t endpoint_count;
    zigpy_endpoint_snapshot_t endpoints[ZIGPY_MAX_ENDPOINTS];
    char manufacturer[ZIGPY_MAX_IDENTITY_LEN + 1u];
    char model[ZIGPY_MAX_IDENTITY_LEN + 1u];
} zigpy_device_snapshot_t;

typedef enum {
    ZIGPY_INTERVIEW_IDLE = 0,
    ZIGPY_INTERVIEW_NODE_DESC,
    ZIGPY_INTERVIEW_ACTIVE_EP,
    ZIGPY_INTERVIEW_SIMPLE_DESC,
    ZIGPY_INTERVIEW_BASIC_IDENTITY,
    ZIGPY_INTERVIEW_DONE,
    ZIGPY_INTERVIEW_FAILED,
    ZIGPY_INTERVIEW_CANCELLED,
} zigpy_interview_phase_t;

typedef struct {
    uint32_t interview_id;
    zigpy_interview_phase_t phase;
    zigpy_status_t status;
    uint8_t completed_endpoints;
    uint8_t total_endpoints;
    uint32_t complete_mask;
    uint32_t truncated_mask;
    /* Absolute deadlines owned by the component; 0 when no deadline applies. */
    uint32_t phase_deadline_ms;
    uint32_t overall_deadline_ms;
} zigpy_interview_status_t;

typedef struct {
    zigpy_device_ref_t device;
    uint8_t endpoint_id;
    uint16_t cluster_id;
    uint16_t attribute_id;
    bool has_manufacturer_code;
    uint16_t manufacturer_code;
} zigpy_attr_path_t;

typedef struct {
    zigpy_device_ref_t device;
    uint8_t endpoint_id;
    uint16_t cluster_id;
    uint8_t command_id;
    bool has_manufacturer_code;
    uint16_t manufacturer_code;
} zigpy_command_path_t;

typedef struct {
    zigpy_attr_path_t path;
    uint16_t min_interval_s;
    uint16_t max_interval_s;
    int64_t reportable_change;
} zigpy_reporting_request_t;

typedef enum {
    ZIGPY_TX_NONE = 0,
    ZIGPY_TX_ATTR_READ,
    ZIGPY_TX_ATTR_WRITE,
    ZIGPY_TX_COMMAND,
    ZIGPY_TX_REPORTING,
} zigpy_transaction_kind_t;

typedef enum {
    ZIGPY_TX_FREE = 0,
    ZIGPY_TX_PENDING,
    ZIGPY_TX_COMPLETE,
    ZIGPY_TX_CANCELLED,
} zigpy_transaction_state_t;

typedef struct {
    uint32_t transaction_id;
    zigpy_transaction_kind_t kind;
    zigpy_transaction_state_t state;
    zigpy_status_t status;
    uint8_t attempts;
    size_t value_len;
    uint8_t value[ZIGPY_MAX_VALUE_SIZE];
} zigpy_transaction_result_t;

typedef struct zigpy_backend_ops {
    zigpy_status_t (*commissioning_set)(void *ctx, bool enabled, uint8_t duration_s);
    zigpy_status_t (*node_desc_request)(void *ctx, uint32_t interview_id,
                                        const zigpy_device_ref_t *device);
    zigpy_status_t (*active_ep_request)(void *ctx, uint32_t interview_id,
                                       const zigpy_device_ref_t *device);
    zigpy_status_t (*simple_desc_request)(void *ctx, uint32_t interview_id,
                                         const zigpy_device_ref_t *device,
                                         uint8_t endpoint_id);
    zigpy_status_t (*basic_identity_request)(void *ctx, uint32_t interview_id,
                                            const zigpy_device_ref_t *device,
                                            uint8_t endpoint_id,
                                            bool manufacturer, bool model);
    zigpy_status_t (*attr_read_request)(void *ctx, uint32_t transaction_id,
                                       const zigpy_attr_path_t *path);
    zigpy_status_t (*attr_write_request)(void *ctx, uint32_t transaction_id,
                                        const zigpy_attr_path_t *path,
                                        const uint8_t *value, size_t value_len);
    zigpy_status_t (*command_request)(void *ctx, uint32_t transaction_id,
                                     const zigpy_command_path_t *path,
                                     const uint8_t *payload, size_t payload_len);
    zigpy_status_t (*reporting_request)(void *ctx, uint32_t transaction_id,
                                       const zigpy_reporting_request_t *request);
    void (*cancel_request)(void *ctx, uint32_t token, bool interview);
} zigpy_backend_ops_t;

typedef struct {
    zigpy_transaction_result_t result;
    uint32_t timeout_ms;
    uint32_t deadline_ms;
    uint8_t max_retries;
    union {
        zigpy_attr_path_t read;
        struct {
            zigpy_attr_path_t path;
            size_t value_len;
            uint8_t value[ZIGPY_MAX_VALUE_SIZE];
        } write;
        struct {
            zigpy_command_path_t path;
            size_t payload_len;
            uint8_t payload[ZIGPY_MAX_VALUE_SIZE];
        } command;
        zigpy_reporting_request_t reporting;
    } request;
} zigpy_transaction_slot_t;

typedef struct {
    zigpy_backend_ops_t backend;
    void *backend_ctx;
    uint32_t next_token;

    bool commissioning_active;
    uint32_t commissioning_deadline_ms;

    zigpy_interview_status_t interview;
    zigpy_device_snapshot_t snapshot;
    /*
     * Last snapshot from an interview that reached ZIGPY_INTERVIEW_DONE.
     *
     * A re-interview resets `snapshot` when it starts, so without this a failed
     * or timed-out re-interview would destroy already-known device information.
     * The application can recover it with zigpy_interview_get_last_good_snapshot.
     */
    zigpy_device_snapshot_t last_good_snapshot;
    bool has_last_good_snapshot;

    uint32_t interview_phase_timeout_ms;
    uint32_t interview_overall_timeout_ms;
    uint32_t interview_phase_started_ms;
    /* Phase the current phase deadline was computed for; used to refresh the
     * per-phase deadline exactly once per phase change. */
    zigpy_interview_phase_t interview_deadline_phase;

    uint8_t simple_index;
    uint8_t simple_success_count;
    uint8_t identity_endpoint;
    uint8_t identity_stage;
    bool identity_had_error;

    zigpy_transaction_slot_t transactions[ZIGPY_MAX_TRANSACTIONS];
} zigpy_ctx_t;

zigpy_status_t zigpy_init(zigpy_ctx_t *ctx, const zigpy_backend_ops_t *backend,
                          void *backend_ctx);

zigpy_status_t zigpy_commissioning_start(zigpy_ctx_t *ctx, uint8_t duration_s,
                                         uint32_t now_ms);
zigpy_status_t zigpy_commissioning_stop(zigpy_ctx_t *ctx);
bool zigpy_commissioning_is_active(const zigpy_ctx_t *ctx);

zigpy_status_t zigpy_interview_begin(zigpy_ctx_t *ctx,
                                     const zigpy_device_ref_t *device,
                                     uint32_t *out_interview_id);

/*
 * Begin an interview with explicit deadlines.
 *
 * `config` may be NULL, in which case the default phase/overall timeouts apply
 * (identical to zigpy_interview_begin). Deadlines are enforced by zigpy_poll:
 * when a phase or the overall interview exceeds its deadline the component
 * cancels the backend request, marks the interview FAILED with
 * ZIGPY_STATUS_TIMEOUT, and leaves the previous good snapshot recoverable.
 */
zigpy_status_t zigpy_interview_begin_ex(zigpy_ctx_t *ctx,
                                        const zigpy_device_ref_t *device,
                                        const zigpy_interview_config_t *config,
                                        uint32_t now_ms,
                                        uint32_t *out_interview_id);

zigpy_status_t zigpy_interview_cancel(zigpy_ctx_t *ctx, uint32_t interview_id);
zigpy_status_t zigpy_interview_get_status(const zigpy_ctx_t *ctx,
                                          zigpy_interview_status_t *out);
zigpy_status_t zigpy_interview_get_snapshot(const zigpy_ctx_t *ctx,
                                            zigpy_device_snapshot_t *out);

/*
 * Retrieve the most recent successfully completed interview snapshot.
 *
 * Returns ZIGPY_STATUS_NOT_FOUND when no interview has ever completed, so
 * callers can distinguish "no known-good data" from "empty device".
 */
zigpy_status_t zigpy_interview_get_last_good_snapshot(
    const zigpy_ctx_t *ctx, zigpy_device_snapshot_t *out);

zigpy_status_t zigpy_interview_node_desc_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const zigpy_node_descriptor_t *descriptor);
zigpy_status_t zigpy_interview_active_ep_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const uint8_t *endpoint_ids, size_t endpoint_count);
zigpy_status_t zigpy_interview_simple_desc_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, uint8_t endpoint_id,
    zigpy_status_t status, uint16_t profile_id, uint16_t device_id,
    const uint16_t *input_clusters, size_t input_cluster_count,
    const uint16_t *output_clusters, size_t output_cluster_count);
zigpy_status_t zigpy_interview_basic_identity_complete(
    zigpy_ctx_t *ctx, uint32_t interview_id, zigpy_status_t status,
    const char *manufacturer, const char *model);

zigpy_status_t zigpy_attr_read_async(zigpy_ctx_t *ctx,
                                     const zigpy_attr_path_t *path,
                                     uint32_t timeout_ms, uint8_t retries,
                                     uint32_t now_ms, uint32_t *out_transaction_id);
zigpy_status_t zigpy_attr_write_async(zigpy_ctx_t *ctx,
                                      const zigpy_attr_path_t *path,
                                      const uint8_t *value, size_t value_len,
                                      uint32_t timeout_ms, uint8_t retries,
                                      uint32_t now_ms, uint32_t *out_transaction_id);
zigpy_status_t zigpy_command_invoke_async(zigpy_ctx_t *ctx,
                                          const zigpy_command_path_t *path,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          uint32_t timeout_ms, uint8_t retries,
                                          uint32_t now_ms,
                                          uint32_t *out_transaction_id);
zigpy_status_t zigpy_reporting_configure_async(
    zigpy_ctx_t *ctx, const zigpy_reporting_request_t *request,
    uint32_t timeout_ms, uint8_t retries, uint32_t now_ms,
    uint32_t *out_transaction_id);
zigpy_status_t zigpy_transaction_cancel(zigpy_ctx_t *ctx,
                                        uint32_t transaction_id);
zigpy_status_t zigpy_transaction_complete(zigpy_ctx_t *ctx,
                                          uint32_t transaction_id,
                                          zigpy_status_t status,
                                          const uint8_t *value,
                                          size_t value_len);
zigpy_status_t zigpy_transaction_get_result(const zigpy_ctx_t *ctx,
                                            uint32_t transaction_id,
                                            zigpy_transaction_result_t *out);
zigpy_status_t zigpy_transaction_release(zigpy_ctx_t *ctx,
                                         uint32_t transaction_id);

void zigpy_poll(zigpy_ctx_t *ctx, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
