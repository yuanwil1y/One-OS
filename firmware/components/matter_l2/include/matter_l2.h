#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIP_L2_MAX_VALUE_BYTES 128u
#define CHIP_L2_MAX_PAYLOAD_JSON 384u
#define CHIP_L2_MAX_PAA_CERTS 4u
#define CHIP_L2_MAX_PAA_DER_BYTES 640u
#define CHIP_L2_MAX_NOC_DER_BYTES 600u
#define CHIP_L2_MAX_ENDPOINTS 8u
#define CHIP_L2_MAX_DEVICE_TYPES_PER_ENDPOINT 4u
#define CHIP_L2_MAX_SERVER_CLUSTERS_PER_ENDPOINT 24u
#define CHIP_L2_MAX_PARTS_PER_ENDPOINT 8u
#define CHIP_L2_MAX_SUBSCRIPTIONS 2u
#define CHIP_L2_MAX_THREAD_DATASET_BYTES 254u
#define CHIP_L2_MAX_WIFI_SSID_BYTES 32u
#define CHIP_L2_MAX_WIFI_PASSWORD_BYTES 64u
#define CHIP_L2_IPK_BYTES 16u

typedef uint32_t chip_request_id_t;
typedef uint32_t chip_subscription_id_t;

typedef enum {
    CHIP_STATUS_OK = 0,
    CHIP_STATUS_INVALID_ARGUMENT,
    CHIP_STATUS_NOT_READY,
    CHIP_STATUS_NO_FABRIC,
    CHIP_STATUS_MULTIPLE_FABRICS,
    CHIP_STATUS_BUSY,
    CHIP_STATUS_NO_MEMORY,
    CHIP_STATUS_TIMEOUT,
    CHIP_STATUS_CANCELLED,
    CHIP_STATUS_CASE_FAILED,
    CHIP_STATUS_IM_ERROR,
    CHIP_STATUS_SECURITY_ERROR,
    CHIP_STATUS_NO_TRUST_ROOTS,
    CHIP_STATUS_UNSUPPORTED_VALUE,
    CHIP_STATUS_TRUNCATED,
    CHIP_STATUS_INTERNAL,
} chip_status_t;

typedef struct {
    uint16_t endpoint_id;
    uint32_t cluster_id;
    uint32_t item_id;
} chip_path_t;

typedef enum {
    CHIP_VALUE_NULL = 0,
    CHIP_VALUE_BOOL,
    CHIP_VALUE_SIGNED,
    CHIP_VALUE_UNSIGNED,
    CHIP_VALUE_FLOAT,
    CHIP_VALUE_UTF8,
    CHIP_VALUE_BYTES,
    CHIP_VALUE_CONTAINER,
} chip_value_type_t;

typedef struct {
    chip_value_type_t type;
    bool truncated;
    uint16_t length;
    union {
        bool boolean;
        int64_t signed_value;
        uint64_t unsigned_value;
        double float_value;
        uint8_t bytes[CHIP_L2_MAX_VALUE_BYTES];
    } data;
} chip_value_t;

typedef struct {
    const uint8_t *der;
    uint16_t der_len;
} chip_paa_cert_t;

/*
 * Caller-filled fixed-capacity certificate output. noc and rcac are required;
 * icac is optional. ipk is always exactly CHIP_L2_IPK_BYTES.
 */
typedef struct {
    uint8_t noc[CHIP_L2_MAX_NOC_DER_BYTES];
    uint16_t noc_len;
    uint8_t icac[CHIP_L2_MAX_NOC_DER_BYTES];
    uint16_t icac_len;
    uint8_t rcac[CHIP_L2_MAX_NOC_DER_BYTES];
    uint16_t rcac_len;
    uint8_t ipk[CHIP_L2_IPK_BYTES];
    bool has_admin_subject;
    uint64_t admin_subject;
} chip_noc_chain_t;

/*
 * The buffers passed to these callbacks are borrowed only for the duration of
 * the call. Implementations must not retain them. The callback must fill a
 * bounded chip_noc_chain_t and must never log private CA material or IPK bytes.
 */
typedef chip_status_t (*chip_generate_controller_noc_callback_t)(
    uint64_t controller_node_id, uint64_t fabric_id,
    const uint8_t *csr_der, size_t csr_der_len,
    chip_noc_chain_t *out_chain, void *context);

typedef chip_status_t (*chip_generate_device_noc_callback_t)(
    uint64_t requested_node_id, uint64_t fabric_id,
    const uint8_t *csr_elements, size_t csr_elements_len,
    const uint8_t *csr_nonce, size_t csr_nonce_len,
    const uint8_t *attestation_signature, size_t attestation_signature_len,
    const uint8_t *attestation_challenge, size_t attestation_challenge_len,
    const uint8_t *dac_der, size_t dac_der_len,
    const uint8_t *pai_der, size_t pai_der_len,
    chip_noc_chain_t *out_chain, void *context);

typedef struct {
    chip_generate_controller_noc_callback_t generate_controller_noc;
    chip_generate_device_noc_callback_t generate_device_noc;
    void *context;
} chip_operational_credentials_provider_t;

typedef struct {
    uint64_t controller_node_id;
    uint64_t fabric_id;
    uint16_t controller_vendor_id;
    uint16_t listen_port;
    bool create_fabric_if_missing;
    const chip_paa_cert_t *paa_certs;
    size_t paa_cert_count;
    const chip_operational_credentials_provider_t *operational_credentials;
} chip_controller_config_t;

typedef struct {
    bool ready;
    uint8_t fabric_index;
    uint64_t fabric_id;
    uint64_t controller_node_id;
    uint64_t compressed_fabric_id;
} chip_fabric_info_t;

typedef struct {
    uint32_t device_type_id;
    uint16_t revision;
} matter_device_type_t;

typedef struct {
    uint16_t endpoint_id;
    matter_device_type_t device_types[CHIP_L2_MAX_DEVICE_TYPES_PER_ENDPOINT];
    uint8_t device_type_count;
    uint32_t server_clusters[CHIP_L2_MAX_SERVER_CLUSTERS_PER_ENDPOINT];
    uint8_t server_cluster_count;
    uint16_t parts[CHIP_L2_MAX_PARTS_PER_ENDPOINT];
    uint8_t parts_count;
    bool truncated;
} matter_endpoint_info_t;

typedef struct {
    uint64_t node_id;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t software_version;
    char vendor_name[33];
    char product_name[33];
    matter_endpoint_info_t endpoints[CHIP_L2_MAX_ENDPOINTS];
    uint8_t endpoint_count;
    bool partial;
    bool truncated;
} matter_node_info_t;

typedef void (*chip_read_callback_t)(chip_request_id_t request_id, chip_status_t status, chip_path_t path,
                                     const chip_value_t *value, void *context);
typedef void (*chip_operation_callback_t)(chip_request_id_t request_id, chip_status_t status, void *context);
typedef void (*matter_node_probe_callback_t)(chip_request_id_t request_id, chip_status_t status,
                                             const matter_node_info_t *info, void *context);
typedef void (*chip_subscription_callback_t)(chip_subscription_id_t subscription_id, chip_status_t status,
                                             chip_path_t path, const chip_value_t *value, void *context);
typedef void (*chip_commission_callback_t)(chip_request_id_t request_id, chip_status_t status, uint64_t node_id,
                                           uint32_t commissioning_stage, void *context);

typedef struct {
    uint64_t node_id;
    uint32_t setup_pin_code;
    const char *peer_ip;
    uint16_t peer_port;
    uint32_t timeout_ms;
} chip_commission_onnetwork_params_t;

typedef struct {
    uint64_t node_id;
    uint32_t setup_pin_code;
    uint16_t discriminator;
    const char *ssid;
    const char *password;
    uint32_t timeout_ms;
} chip_commission_ble_wifi_params_t;

typedef struct {
    uint64_t node_id;
    uint32_t setup_pin_code;
    uint16_t discriminator;
    const uint8_t *thread_dataset;
    uint8_t thread_dataset_len;
    uint32_t timeout_ms;
} chip_commission_ble_thread_params_t;

chip_status_t chip_controller_init(const chip_controller_config_t *config);
chip_status_t chip_controller_shutdown(void);
bool chip_controller_is_ready(void);
chip_status_t chip_controller_get_fabric(chip_fabric_info_t *out_info);

chip_status_t chip_request_cancel(chip_request_id_t request_id);

chip_status_t chip_read_attribute(uint64_t node_id, chip_path_t path, uint32_t timeout_ms,
                                  chip_read_callback_t callback, void *context, chip_request_id_t *out_request_id);
chip_status_t chip_write_attribute(uint64_t node_id, chip_path_t path, const char *bounded_value_json,
                                   uint16_t timed_write_timeout_ms, uint32_t timeout_ms,
                                   chip_operation_callback_t callback, void *context,
                                   chip_request_id_t *out_request_id);
chip_status_t chip_invoke(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t command_id,
                          const char *bounded_command_json, uint16_t timed_invoke_timeout_ms,
                          uint32_t timeout_ms, chip_operation_callback_t callback, void *context,
                          chip_request_id_t *out_request_id);

chip_status_t matter_node_probe(uint64_t node_id, uint32_t timeout_ms,
                                matter_node_probe_callback_t callback, void *context,
                                chip_request_id_t *out_request_id);

chip_status_t chip_subscribe_start(uint64_t node_id, chip_path_t path, uint16_t min_interval_s,
                                   uint16_t max_interval_s, chip_subscription_callback_t callback,
                                   void *context, chip_subscription_id_t *out_subscription_id);
chip_status_t chip_subscribe_stop(chip_subscription_id_t subscription_id);

chip_status_t chip_commission_onnetwork(const chip_commission_onnetwork_params_t *params,
                                        chip_commission_callback_t callback, void *context,
                                        chip_request_id_t *out_request_id);
chip_status_t chip_commission_ble_wifi(const chip_commission_ble_wifi_params_t *params,
                                       chip_commission_callback_t callback, void *context,
                                       chip_request_id_t *out_request_id);
chip_status_t chip_commission_ble_thread(const chip_commission_ble_thread_params_t *params,
                                         chip_commission_callback_t callback, void *context,
                                         chip_request_id_t *out_request_id);

#ifdef __cplusplus
}
#endif
