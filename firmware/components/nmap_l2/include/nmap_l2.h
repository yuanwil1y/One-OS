#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NMAP_MAX_CONCURRENCY 4u
#define NMAP_MAX_RETRIES 1u
#define NMAP_DISCOVERY_MAX_TARGETS 254u
#define NMAP_DISCOVERY_MAX_FALLBACK_PORTS 4u
#define NMAP_PORT_SCAN_MAX_TARGETS 32u
#define NMAP_PORT_SCAN_MAX_PORTS 32u
#define NMAP_PORT_SCAN_MAX_JOBS 512u
#define NMAP_SERVICE_MAX_ENDPOINTS 64u
#define NMAP_SERVICE_CAPTURE_MAX 512u
#define NMAP_WAIT_FOREVER UINT32_MAX

#define NMAP_RESULT_PARTIAL   (1u << 0)
#define NMAP_RESULT_TRUNCATED (1u << 1)

#define NMAP_HOST_EVIDENCE_ICMP_REPLY    (1u << 0)
#define NMAP_HOST_EVIDENCE_TCP_ACCEPTED  (1u << 1)
#define NMAP_HOST_EVIDENCE_TCP_REFUSED   (1u << 2)

#define NMAP_SERVICE_EVIDENCE_PASSIVE_BANNER (1u << 0)
#define NMAP_SERVICE_EVIDENCE_ACTIVE_PROBE   (1u << 1)
#define NMAP_SERVICE_EVIDENCE_PROTOCOL_MATCH (1u << 2)

typedef struct nmap_scan *nmap_scan_handle_t;

typedef struct {
    uint32_t addr_be;
} nmap_ipv4_target_t;

typedef enum {
    NMAP_HOST_UP = 0,
    NMAP_HOST_NO_RESPONSE,
    NMAP_HOST_UNREACHABLE,
    NMAP_HOST_ERROR,
} nmap_host_state_t;

typedef enum {
    NMAP_PORT_OPEN = 0,
    NMAP_PORT_CLOSED,
    NMAP_PORT_FILTERED,
    NMAP_PORT_UNREACHABLE,
    NMAP_PORT_ERROR,
} nmap_port_state_t;

typedef enum {
    NMAP_REASON_NONE = 0,
    NMAP_REASON_REPLY,
    NMAP_REASON_CONNECTION_ACCEPTED,
    NMAP_REASON_CONNECTION_REFUSED,
    NMAP_REASON_TIMEOUT,
    NMAP_REASON_HOST_UNREACHABLE,
    NMAP_REASON_NETWORK_UNREACHABLE,
    NMAP_REASON_CANCELLED,
    NMAP_REASON_LOCAL_RESOURCE,
    NMAP_REASON_IO_ERROR,
} nmap_reason_t;

typedef enum {
    NMAP_SERVICE_UNKNOWN = 0,
    NMAP_SERVICE_SSH,
    NMAP_SERVICE_FTP,
    NMAP_SERVICE_SMTP,
    NMAP_SERVICE_HTTP,
} nmap_service_kind_t;

typedef enum {
    NMAP_SERVICE_CONFIDENCE_NONE = 0,
    NMAP_SERVICE_CONFIDENCE_PROBABLE,
    NMAP_SERVICE_CONFIDENCE_STRONG,
} nmap_service_confidence_t;

typedef enum {
    NMAP_SERVICE_PROBE_AUTO = 0,
    NMAP_SERVICE_PROBE_PASSIVE,
    NMAP_SERVICE_PROBE_HTTP_HEAD,
} nmap_service_probe_profile_t;

typedef struct {
    uint8_t max_inflight;
    uint8_t max_retries;
    uint16_t max_probes_per_second;
    uint32_t probe_timeout_ms;
    uint32_t scan_timeout_ms;
} nmap_timing_policy_t;

typedef struct {
    uint32_t total_jobs;
    uint32_t completed_jobs;
    uint32_t error_jobs;
    uint32_t elapsed_ms;
    bool cancelled;
    bool timed_out;
    bool partial;
    esp_err_t terminal_error;
} nmap_scan_summary_t;

typedef struct {
    uint32_t total_jobs;
    uint32_t completed_jobs;
    uint32_t error_jobs;
    bool cancelled;
    bool timed_out;
} nmap_scan_progress_t;

typedef struct {
    nmap_ipv4_target_t target;
    nmap_host_state_t state;
    nmap_reason_t reason;
    uint32_t evidence_flags;
    uint32_t rtt_ms;
    uint8_t attempts;
    uint8_t result_flags;
} nmap_host_result_t;

typedef struct {
    nmap_ipv4_target_t target;
    uint16_t port;
    nmap_port_state_t state;
    nmap_reason_t reason;
    uint32_t elapsed_ms;
    uint8_t attempts;
    uint8_t result_flags;
} nmap_port_result_t;

typedef struct {
    nmap_ipv4_target_t target;
    uint16_t port;
    nmap_service_kind_t service;
    nmap_service_confidence_t confidence;
    nmap_reason_t reason;
    uint32_t evidence_flags;
    uint16_t response_bytes;
    uint8_t result_flags;
    char service_name[16];
    char product[48];
    char version[24];
    char probe_id[24];
    char banner[128];
} nmap_service_result_t;

typedef struct {
    nmap_ipv4_target_t target;
    uint16_t port;
    nmap_service_probe_profile_t probe_profile;
} nmap_service_endpoint_t;

typedef void (*nmap_host_result_cb_t)(const nmap_host_result_t *result, void *user_ctx);
typedef void (*nmap_port_result_cb_t)(const nmap_port_result_t *result, void *user_ctx);
typedef void (*nmap_service_result_cb_t)(const nmap_service_result_t *result, void *user_ctx);
typedef void (*nmap_progress_cb_t)(const nmap_scan_progress_t *progress, void *user_ctx);

typedef struct {
    bool use_local_subnet;
    const nmap_ipv4_target_t *targets;
    uint16_t target_count;
    bool enable_icmp;
    const uint16_t *tcp_fallback_ports;
    uint8_t tcp_fallback_port_count;
    nmap_timing_policy_t timing;
    nmap_host_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_discovery_config_t;

typedef struct {
    const nmap_ipv4_target_t *targets;
    uint16_t target_count;
    const uint16_t *ports;
    uint16_t port_count;
    nmap_timing_policy_t timing;
    nmap_port_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_port_scan_config_t;

typedef struct {
    const nmap_service_endpoint_t *endpoints;
    uint16_t endpoint_count;
    uint16_t capture_bytes;
    nmap_timing_policy_t timing;
    nmap_service_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_service_scan_config_t;

nmap_timing_policy_t nmap_timing_policy_default(void);
bool nmap_lan_prerequisite_ready(void);

esp_err_t nmap_discovery_start(const nmap_discovery_config_t *config, nmap_scan_handle_t *out_scan);
esp_err_t nmap_discovery_cancel(nmap_scan_handle_t scan);
esp_err_t nmap_discovery_wait(nmap_scan_handle_t scan, uint32_t timeout_ms);

esp_err_t nmap_port_scan_start(const nmap_port_scan_config_t *config, nmap_scan_handle_t *out_scan);
esp_err_t nmap_port_scan_cancel(nmap_scan_handle_t scan);
esp_err_t nmap_port_scan_wait(nmap_scan_handle_t scan, uint32_t timeout_ms);

esp_err_t nmap_service_scan_start(const nmap_service_scan_config_t *config, nmap_scan_handle_t *out_scan);
esp_err_t nmap_service_scan_cancel(nmap_scan_handle_t scan);
esp_err_t nmap_service_scan_wait(nmap_scan_handle_t scan, uint32_t timeout_ms);

esp_err_t nmap_scan_get_summary(nmap_scan_handle_t scan, nmap_scan_summary_t *out_summary);

#ifdef __cplusplus
}
#endif
