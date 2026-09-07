#include "nmap_l2.h"
#include "nmap_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#define NMAP_COORDINATOR_STACK 3072u
#define NMAP_WORKER_STACK 4096u
#define NMAP_TASK_PRIORITY 4u
#define NMAP_DEFAULT_CAPTURE_BYTES 256u
#define NMAP_RATE_WAIT_SLICE_MS 50u
#define NMAP_PING_END_GRACE_MS 100u

typedef enum {
    NMAP_SCAN_NONE = 0,
    NMAP_SCAN_DISCOVERY,
    NMAP_SCAN_PORT,
    NMAP_SCAN_SERVICE,
} nmap_scan_kind_t;

typedef struct nmap_worker_arg nmap_worker_arg_t;

typedef struct {
    nmap_ipv4_target_t targets[NMAP_DISCOVERY_MAX_TARGETS];
    uint16_t target_count;
    uint16_t fallback_ports[NMAP_DISCOVERY_MAX_FALLBACK_PORTS];
    uint8_t fallback_port_count;
    bool enable_icmp;
    nmap_host_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_discovery_state_t;

typedef struct {
    nmap_ipv4_target_t targets[NMAP_PORT_SCAN_MAX_TARGETS];
    uint16_t target_count;
    uint16_t ports[NMAP_PORT_SCAN_MAX_PORTS];
    uint16_t port_count;
    nmap_port_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_port_state_internal_t;

typedef struct {
    nmap_service_endpoint_t endpoints[NMAP_SERVICE_MAX_ENDPOINTS];
    uint16_t endpoint_count;
    uint16_t capture_bytes;
    uint8_t capture[NMAP_MAX_CONCURRENCY][NMAP_SERVICE_CAPTURE_MAX];
    nmap_service_result_cb_t result_cb;
    nmap_progress_cb_t progress_cb;
    void *user_ctx;
} nmap_service_state_t;

struct nmap_scan {
    nmap_scan_kind_t kind;
    bool sync_initialized;
    volatile bool in_use;
    volatile bool cancel_requested;
    volatile bool timed_out;
    volatile uint32_t next_job;
    uint32_t total_jobs;
    TickType_t start_tick;
    TickType_t duration_ticks;
    TickType_t next_probe_tick;
    nmap_timing_policy_t timing;
    nmap_scan_summary_t summary;
    bool summary_valid;
    TaskHandle_t coordinator_task;

    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t callback_mutex;
    SemaphoreHandle_t rate_mutex;
    StaticSemaphore_t done_storage;
    StaticSemaphore_t callback_storage;
    StaticSemaphore_t rate_storage;

    nmap_worker_arg_t *worker_refs[NMAP_MAX_CONCURRENCY];

    union {
        nmap_discovery_state_t discovery;
        nmap_port_state_internal_t port;
        nmap_service_state_t service;
    } u;
};

struct nmap_worker_arg {
    struct nmap_scan *scan;
    uint8_t worker_index;
    SemaphoreHandle_t ping_done_sem;
    StaticSemaphore_t ping_done_storage;
    volatile bool ping_success;
    volatile uint32_t ping_rtt_ms;
};

static struct nmap_scan s_scan;
static nmap_worker_arg_t s_workers[NMAP_MAX_CONCURRENCY];
static portMUX_TYPE s_guard = portMUX_INITIALIZER_UNLOCKED;
static const uint16_t s_default_discovery_ports[] = {80u, 443u, 22u};

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * 1000u) / configTICK_RATE_HZ);
}

static TickType_t ms_to_nonzero_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 ? 1 : ticks;
}

static bool tick_before(TickType_t a, TickType_t b)
{
    return (int32_t)(a - b) < 0;
}

static esp_err_t get_sta_ip_info(esp_netif_ip_info_t *out)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) return ESP_ERR_INVALID_STATE;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0u || info.netmask.addr == 0u) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out != NULL) *out = info;
    return ESP_OK;
}

bool nmap_lan_prerequisite_ready(void)
{
    return get_sta_ip_info(NULL) == ESP_OK;
}

static bool same_subnet(uint32_t target_be, const esp_netif_ip_info_t *info)
{
    return (target_be & info->netmask.addr) == (info->ip.addr & info->netmask.addr);
}

static bool target_is_local_host(uint32_t target_be, const esp_netif_ip_info_t *info)
{
    return target_be == info->ip.addr;
}

static bool target_is_network_or_broadcast(uint32_t target_be, const esp_netif_ip_info_t *info)
{
    const uint32_t target = lwip_ntohl(target_be);
    const uint32_t ip = lwip_ntohl(info->ip.addr);
    const uint32_t mask = lwip_ntohl(info->netmask.addr);
    const uint32_t network = ip & mask;
    const uint32_t broadcast = network | ~mask;
    return target == network || target == broadcast;
}

static esp_err_t validate_lan_target(nmap_ipv4_target_t target, const esp_netif_ip_info_t *info, bool allow_self)
{
    if (!nmap_internal_ipv4_is_unicast(target.addr_be) || !same_subnet(target.addr_be, info) ||
        target_is_network_or_broadcast(target.addr_be, info) ||
        (!allow_self && target_is_local_host(target.addr_be, info))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool append_unique_target(nmap_ipv4_target_t *dst, uint16_t *count, uint16_t cap, nmap_ipv4_target_t target)
{
    for (uint16_t i = 0; i < *count; ++i) {
        if (dst[i].addr_be == target.addr_be) return true;
    }
    if (*count >= cap) return false;
    dst[*count] = target;
    ++(*count);
    return true;
}

static bool append_unique_port(uint16_t *dst, uint16_t *count, uint16_t cap, uint16_t port)
{
    if (port == 0u) return false;
    for (uint16_t i = 0; i < *count; ++i) {
        if (dst[i] == port) return true;
    }
    if (*count >= cap) return false;
    dst[*count] = port;
    ++(*count);
    return true;
}

static esp_err_t ensure_sync_initialized(void)
{
    if (s_scan.sync_initialized) return ESP_OK;
    s_scan.done_sem = xSemaphoreCreateBinaryStatic(&s_scan.done_storage);
    s_scan.callback_mutex = xSemaphoreCreateMutexStatic(&s_scan.callback_storage);
    s_scan.rate_mutex = xSemaphoreCreateMutexStatic(&s_scan.rate_storage);
    if (s_scan.done_sem == NULL || s_scan.callback_mutex == NULL || s_scan.rate_mutex == NULL) return ESP_ERR_NO_MEM;
    for (uint8_t i = 0; i < NMAP_MAX_CONCURRENCY; ++i) {
        s_workers[i].scan = &s_scan;
        s_workers[i].worker_index = i;
        s_workers[i].ping_done_sem = xSemaphoreCreateBinaryStatic(&s_workers[i].ping_done_storage);
        if (s_workers[i].ping_done_sem == NULL) return ESP_ERR_NO_MEM;
        s_scan.worker_refs[i] = &s_workers[i];
    }
    s_scan.sync_initialized = true;
    return ESP_OK;
}

static esp_err_t reserve_scan(nmap_scan_kind_t kind, const nmap_timing_policy_t *timing)
{
    esp_err_t err = ensure_sync_initialized();
    if (err != ESP_OK) return err;
    err = nmap_internal_timing_validate(timing);
    if (err != ESP_OK) return err;

    taskENTER_CRITICAL(&s_guard);
    if (s_scan.in_use) {
        taskEXIT_CRITICAL(&s_guard);
        return ESP_ERR_INVALID_STATE;
    }
    s_scan.in_use = true;
    taskEXIT_CRITICAL(&s_guard);

    while (xSemaphoreTake(s_scan.done_sem, 0) == pdTRUE) {}
    for (uint8_t i = 0; i < NMAP_MAX_CONCURRENCY; ++i) {
        while (xSemaphoreTake(s_workers[i].ping_done_sem, 0) == pdTRUE) {}
    }

    s_scan.kind = kind;
    s_scan.cancel_requested = false;
    s_scan.timed_out = false;
    s_scan.next_job = 0;
    s_scan.total_jobs = 0;
    s_scan.timing = *timing;
    s_scan.start_tick = xTaskGetTickCount();
    s_scan.duration_ticks = ms_to_nonzero_ticks(timing->scan_timeout_ms);
    s_scan.next_probe_tick = s_scan.start_tick;
    s_scan.coordinator_task = NULL;
    memset(&s_scan.summary, 0, sizeof(s_scan.summary));
    s_scan.summary.terminal_error = ESP_OK;
    s_scan.summary_valid = true;
    return ESP_OK;
}

static void release_reserved_scan(void)
{
    taskENTER_CRITICAL(&s_guard);
    s_scan.in_use = false;
    s_scan.kind = NMAP_SCAN_NONE;
    taskEXIT_CRITICAL(&s_guard);
}

static bool scan_expired(struct nmap_scan *scan)
{
    const TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - scan->start_tick);
    if (elapsed >= scan->duration_ticks) {
        scan->timed_out = true;
        return true;
    }
    return false;
}

static bool scan_should_stop(struct nmap_scan *scan)
{
    return scan->cancel_requested || scan_expired(scan);
}

static bool scan_take_next_job(struct nmap_scan *scan, uint32_t *out_index)
{
    if (scan_should_stop(scan)) return false;
    bool ok = false;
    taskENTER_CRITICAL(&s_guard);
    if (scan->next_job < scan->total_jobs) {
        *out_index = scan->next_job++;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_guard);
    return ok;
}

static bool rate_gate(struct nmap_scan *scan)
{
    if (xSemaphoreTake(scan->rate_mutex, portMAX_DELAY) != pdTRUE) return false;
    const TickType_t interval = ms_to_nonzero_ticks((1000u + scan->timing.max_probes_per_second - 1u) /
                                                    scan->timing.max_probes_per_second);
    while (!scan_should_stop(scan)) {
        const TickType_t now = xTaskGetTickCount();
        if (!tick_before(now, scan->next_probe_tick)) {
            scan->next_probe_tick = now + interval;
            xSemaphoreGive(scan->rate_mutex);
            return true;
        }
        TickType_t wait_ticks = scan->next_probe_tick - now;
        const TickType_t slice = ms_to_nonzero_ticks(NMAP_RATE_WAIT_SLICE_MS);
        if (wait_ticks > slice) wait_ticks = slice;
        vTaskDelay(wait_ticks);
    }
    xSemaphoreGive(scan->rate_mutex);
    return false;
}

static void emit_progress_locked(struct nmap_scan *scan)
{
    nmap_scan_progress_t progress = {
        .total_jobs = scan->summary.total_jobs,
        .completed_jobs = scan->summary.completed_jobs,
        .error_jobs = scan->summary.error_jobs,
        .cancelled = scan->cancel_requested,
        .timed_out = scan->timed_out,
    };
    nmap_progress_cb_t cb = NULL;
    void *ctx = NULL;
    switch (scan->kind) {
    case NMAP_SCAN_DISCOVERY: cb = scan->u.discovery.progress_cb; ctx = scan->u.discovery.user_ctx; break;
    case NMAP_SCAN_PORT: cb = scan->u.port.progress_cb; ctx = scan->u.port.user_ctx; break;
    case NMAP_SCAN_SERVICE: cb = scan->u.service.progress_cb; ctx = scan->u.service.user_ctx; break;
    default: break;
    }
    if (cb != NULL) cb(&progress, ctx);
}

static void complete_host_job(struct nmap_scan *scan, const nmap_host_result_t *result, bool error)
{
    xSemaphoreTake(scan->callback_mutex, portMAX_DELAY);
    if (scan->u.discovery.result_cb != NULL) scan->u.discovery.result_cb(result, scan->u.discovery.user_ctx);
    ++scan->summary.completed_jobs;
    if (error) ++scan->summary.error_jobs;
    emit_progress_locked(scan);
    xSemaphoreGive(scan->callback_mutex);
}

static void complete_port_job(struct nmap_scan *scan, const nmap_port_result_t *result, bool error)
{
    xSemaphoreTake(scan->callback_mutex, portMAX_DELAY);
    if (scan->u.port.result_cb != NULL) scan->u.port.result_cb(result, scan->u.port.user_ctx);
    ++scan->summary.completed_jobs;
    if (error) ++scan->summary.error_jobs;
    emit_progress_locked(scan);
    xSemaphoreGive(scan->callback_mutex);
}

static void complete_service_job(struct nmap_scan *scan, const nmap_service_result_t *result, bool error)
{
    xSemaphoreTake(scan->callback_mutex, portMAX_DELAY);
    if (scan->u.service.result_cb != NULL) scan->u.service.result_cb(result, scan->u.service.user_ctx);
    ++scan->summary.completed_jobs;
    if (error) ++scan->summary.error_jobs;
    emit_progress_locked(scan);
    xSemaphoreGive(scan->callback_mutex);
}

static uint32_t probe_wait_ms(struct nmap_scan *scan)
{
    const TickType_t elapsed_ticks = (TickType_t)(xTaskGetTickCount() - scan->start_tick);
    const uint32_t elapsed_ms = ticks_to_ms(elapsed_ticks);
    if (elapsed_ms >= scan->timing.scan_timeout_ms) {
        scan->timed_out = true;
        return 0;
    }
    const uint32_t remaining = scan->timing.scan_timeout_ms - elapsed_ms;
    return remaining < scan->timing.probe_timeout_ms ? remaining : scan->timing.probe_timeout_ms;
}

static void tcp_connect_attempt(struct nmap_scan *scan, nmap_ipv4_target_t target, uint16_t port,
                                bool keep_open, int *out_fd, nmap_port_state_t *out_state,
                                nmap_reason_t *out_reason, uint32_t *out_elapsed_ms)
{
    *out_fd = -1;
    *out_state = NMAP_PORT_ERROR;
    *out_reason = NMAP_REASON_IO_ERROR;
    *out_elapsed_ms = 0;
    if (!rate_gate(scan)) {
        *out_reason = scan->cancel_requested ? NMAP_REASON_CANCELLED : NMAP_REASON_TIMEOUT;
        return;
    }

    const uint32_t wait_ms = probe_wait_ms(scan);
    if (wait_ms == 0) {
        *out_reason = NMAP_REASON_TIMEOUT;
        return;
    }

    const TickType_t started = xTaskGetTickCount();
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        nmap_internal_classify_connect_error(errno, out_state, out_reason);
        return;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        nmap_internal_classify_connect_error(errno, out_state, out_reason);
        close(fd);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = target.addr_be,
    };
    const int rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    int socket_error = 0;
    if (rc != 0) {
        if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
            socket_error = errno;
        } else {
            fd_set writefds;
            fd_set errorfds;
            FD_ZERO(&writefds);
            FD_ZERO(&errorfds);
            FD_SET(fd, &writefds);
            FD_SET(fd, &errorfds);
            struct timeval timeout = {
                .tv_sec = (time_t)(wait_ms / 1000u),
                .tv_usec = (suseconds_t)((wait_ms % 1000u) * 1000u),
            };
            const int selected = select(fd + 1, NULL, &writefds, &errorfds, &timeout);
            if (selected == 0) socket_error = ETIMEDOUT;
            else if (selected < 0) socket_error = errno;
            else {
                socklen_t opt_len = sizeof(socket_error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &opt_len) != 0) socket_error = errno;
            }
        }
    }

    nmap_internal_classify_connect_error(socket_error, out_state, out_reason);
    *out_elapsed_ms = ticks_to_ms((TickType_t)(xTaskGetTickCount() - started));
    if (*out_state == NMAP_PORT_OPEN && keep_open) *out_fd = fd;
    else close(fd);
}

static void tcp_probe(struct nmap_scan *scan, nmap_ipv4_target_t target, uint16_t port, bool keep_open,
                      int *out_fd, nmap_port_state_t *out_state, nmap_reason_t *out_reason,
                      uint32_t *out_elapsed_ms, uint8_t *out_attempts)
{
    *out_fd = -1;
    *out_elapsed_ms = 0;
    *out_attempts = 0;
    for (uint8_t attempt = 0; attempt <= scan->timing.max_retries; ++attempt) {
        if (scan_should_stop(scan)) {
            *out_state = NMAP_PORT_ERROR;
            *out_reason = scan->cancel_requested ? NMAP_REASON_CANCELLED : NMAP_REASON_TIMEOUT;
            return;
        }
        int fd = -1;
        nmap_port_state_t state;
        nmap_reason_t reason;
        uint32_t elapsed_ms = 0;
        tcp_connect_attempt(scan, target, port, keep_open, &fd, &state, &reason, &elapsed_ms);
        ++(*out_attempts);
        *out_elapsed_ms += elapsed_ms;
        *out_state = state;
        *out_reason = reason;
        if (state != NMAP_PORT_FILTERED || attempt == scan->timing.max_retries || scan_should_stop(scan)) {
            *out_fd = fd;
            return;
        }
        if (fd >= 0) close(fd);
    }
}

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    nmap_worker_arg_t *worker = (nmap_worker_arg_t *)args;
    uint32_t rtt_ms = 0;
    (void)esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt_ms, sizeof(rtt_ms));
    worker->ping_rtt_ms = rtt_ms;
    worker->ping_success = true;
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    nmap_worker_arg_t *worker = (nmap_worker_arg_t *)args;
    xSemaphoreGive(worker->ping_done_sem);
}

static esp_err_t ping_once(nmap_worker_arg_t *worker, nmap_ipv4_target_t target,
                           bool *out_success, uint32_t *out_rtt_ms)
{
    struct nmap_scan *scan = worker->scan;
    *out_success = false;
    *out_rtt_ms = 0;
    if (!rate_gate(scan)) return ESP_ERR_INVALID_STATE;
    while (xSemaphoreTake(worker->ping_done_sem, 0) == pdTRUE) {}
    worker->ping_success = false;
    worker->ping_rtt_ms = 0;

    const uint32_t wait_ms = probe_wait_ms(scan);
    if (wait_ms == 0) return ESP_ERR_TIMEOUT;

    const uint32_t host = lwip_ntohl(target.addr_be);
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    IP_ADDR4(&cfg.target_addr,
             (uint8_t)(host >> 24), (uint8_t)(host >> 16), (uint8_t)(host >> 8), (uint8_t)host);
    cfg.count = 1;
    cfg.interval_ms = 1;
    cfg.timeout_ms = wait_ms;
    cfg.task_stack_size = 2048;
    cfg.task_prio = NMAP_TASK_PRIORITY;

    const esp_ping_callbacks_t callbacks = {
        .cb_args = worker,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = NULL,
        .on_ping_end = ping_on_end,
    };
    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&cfg, &callbacks, &ping);
    if (err != ESP_OK) return err;
    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        (void)esp_ping_delete_session(ping);
        return err;
    }

    const TickType_t wait_total = ms_to_nonzero_ticks(wait_ms + NMAP_PING_END_GRACE_MS);
    const TickType_t wait_start = xTaskGetTickCount();
    bool ended = false;
    while ((TickType_t)(xTaskGetTickCount() - wait_start) < wait_total) {
        const TickType_t slice = ms_to_nonzero_ticks(NMAP_RATE_WAIT_SLICE_MS);
        if (xSemaphoreTake(worker->ping_done_sem, slice) == pdTRUE) {
            ended = true;
            break;
        }
        if (scan_should_stop(scan)) {
            (void)esp_ping_stop(ping);
            break;
        }
    }
    if (!ended) {
        (void)esp_ping_stop(ping);
        (void)xSemaphoreTake(worker->ping_done_sem, ms_to_nonzero_ticks(NMAP_PING_END_GRACE_MS));
    }
    (void)esp_ping_delete_session(ping);
    *out_success = worker->ping_success;
    *out_rtt_ms = worker->ping_rtt_ms;
    return ESP_OK;
}

static esp_err_t ping_probe(nmap_worker_arg_t *worker, nmap_ipv4_target_t target,
                            bool *out_success, uint32_t *out_rtt_ms, uint8_t *out_attempts)
{
    *out_success = false;
    *out_rtt_ms = 0;
    *out_attempts = 0;
    esp_err_t first_error = ESP_OK;
    for (uint8_t attempt = 0; attempt <= worker->scan->timing.max_retries; ++attempt) {
        if (scan_should_stop(worker->scan)) break;
        bool success = false;
        uint32_t rtt_ms = 0;
        const esp_err_t err = ping_once(worker, target, &success, &rtt_ms);
        ++(*out_attempts);
        if (err != ESP_OK && first_error == ESP_OK) first_error = err;
        if (success) {
            *out_success = true;
            *out_rtt_ms = rtt_ms;
            return ESP_OK;
        }
    }
    return first_error;
}

static void process_discovery_job(nmap_worker_arg_t *worker, uint32_t job_index)
{
    struct nmap_scan *scan = worker->scan;
    const nmap_ipv4_target_t target = scan->u.discovery.targets[job_index];
    nmap_host_result_t result = {
        .target = target,
        .state = NMAP_HOST_NO_RESPONSE,
        .reason = NMAP_REASON_TIMEOUT,
    };
    bool local_error = false;
    bool saw_unreachable = false;
    nmap_reason_t unreachable_reason = NMAP_REASON_NONE;

    if (scan->u.discovery.enable_icmp) {
        bool success = false;
        uint32_t rtt_ms = 0;
        uint8_t attempts = 0;
        const esp_err_t err = ping_probe(worker, target, &success, &rtt_ms, &attempts);
        result.attempts += attempts;
        if (success) {
            result.state = NMAP_HOST_UP;
            result.reason = NMAP_REASON_REPLY;
            result.evidence_flags |= NMAP_HOST_EVIDENCE_ICMP_REPLY;
            result.rtt_ms = rtt_ms;
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            local_error = true;
        }
    }

    if (result.state != NMAP_HOST_UP && !scan_should_stop(scan)) {
        for (uint8_t i = 0; i < scan->u.discovery.fallback_port_count; ++i) {
            int fd = -1;
            nmap_port_state_t state = NMAP_PORT_ERROR;
            nmap_reason_t reason = NMAP_REASON_IO_ERROR;
            uint32_t elapsed_ms = 0;
            uint8_t attempts = 0;
            tcp_probe(scan, target, scan->u.discovery.fallback_ports[i], false,
                      &fd, &state, &reason, &elapsed_ms, &attempts);
            (void)elapsed_ms;
            result.attempts += attempts;
            if (fd >= 0) close(fd);
            if (state == NMAP_PORT_OPEN) {
                result.state = NMAP_HOST_UP;
                result.reason = NMAP_REASON_CONNECTION_ACCEPTED;
                result.evidence_flags |= NMAP_HOST_EVIDENCE_TCP_ACCEPTED;
                break;
            }
            if (state == NMAP_PORT_CLOSED) {
                result.state = NMAP_HOST_UP;
                result.reason = NMAP_REASON_CONNECTION_REFUSED;
                result.evidence_flags |= NMAP_HOST_EVIDENCE_TCP_REFUSED;
                break;
            }
            if (state == NMAP_PORT_UNREACHABLE) {
                saw_unreachable = true;
                unreachable_reason = reason;
            } else if (state == NMAP_PORT_ERROR && reason != NMAP_REASON_CANCELLED && reason != NMAP_REASON_TIMEOUT) {
                local_error = true;
            }
            if (scan_should_stop(scan)) break;
        }
    }

    if (result.state != NMAP_HOST_UP) {
        if (scan_should_stop(scan)) {
            result.result_flags |= NMAP_RESULT_PARTIAL;
            result.reason = scan->cancel_requested ? NMAP_REASON_CANCELLED : NMAP_REASON_TIMEOUT;
        } else if (saw_unreachable) {
            result.state = NMAP_HOST_UNREACHABLE;
            result.reason = unreachable_reason;
        } else if (local_error) {
            result.state = NMAP_HOST_ERROR;
            result.reason = NMAP_REASON_IO_ERROR;
        }
    }
    complete_host_job(scan, &result, result.state == NMAP_HOST_ERROR);
}

static void process_port_job(nmap_worker_arg_t *worker, uint32_t job_index)
{
    struct nmap_scan *scan = worker->scan;
    const uint16_t port_count = scan->u.port.port_count;
    const uint16_t target_index = (uint16_t)(job_index / port_count);
    const uint16_t port_index = (uint16_t)(job_index % port_count);
    nmap_port_result_t result = {
        .target = scan->u.port.targets[target_index],
        .port = scan->u.port.ports[port_index],
        .state = NMAP_PORT_ERROR,
        .reason = NMAP_REASON_IO_ERROR,
    };
    int fd = -1;
    tcp_probe(scan, result.target, result.port, false, &fd,
              &result.state, &result.reason, &result.elapsed_ms, &result.attempts);
    if (fd >= 0) close(fd);
    if (scan_should_stop(scan) && result.reason != NMAP_REASON_CONNECTION_ACCEPTED &&
        result.reason != NMAP_REASON_CONNECTION_REFUSED) {
        result.result_flags |= NMAP_RESULT_PARTIAL;
        result.reason = scan->cancel_requested ? NMAP_REASON_CANCELLED : NMAP_REASON_TIMEOUT;
    }
    complete_port_job(scan, &result, result.state == NMAP_PORT_ERROR && result.reason != NMAP_REASON_CANCELLED);
}

static int wait_socket_readable(int fd, uint32_t timeout_ms)
{
    fd_set readfds;
    fd_set errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&errorfds);
    FD_SET(fd, &readfds);
    FD_SET(fd, &errorfds);
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    return select(fd + 1, &readfds, NULL, &errorfds, &timeout);
}

static size_t read_service_response(struct nmap_scan *scan, int fd, uint8_t *buffer, size_t cap,
                                    bool *out_truncated, bool *out_io_error)
{
    *out_truncated = false;
    *out_io_error = false;
    size_t used = 0;
    const TickType_t started = xTaskGetTickCount();
    const uint32_t allowed_ms = probe_wait_ms(scan);
    const TickType_t timeout_ticks = ms_to_nonzero_ticks(allowed_ms == 0 ? 1 : allowed_ms);
    while (used < cap && !scan_should_stop(scan)) {
        const TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - started);
        if (elapsed >= timeout_ticks) break;
        const uint32_t elapsed_ms = ticks_to_ms(elapsed);
        const uint32_t remaining_ms = allowed_ms > elapsed_ms ? allowed_ms - elapsed_ms : 1;
        const int selected = wait_socket_readable(fd, remaining_ms);
        if (selected == 0) break;
        if (selected < 0) {
            if (errno == EINTR) continue;
            *out_io_error = true;
            break;
        }
        const ssize_t n = recv(fd, buffer + used, cap - used, 0);
        if (n > 0) {
            used += (size_t)n;
            if (used == cap) {
                *out_truncated = true;
                break;
            }
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        *out_io_error = true;
        break;
    }
    return used;
}

static bool send_http_head(struct nmap_scan *scan, int fd, nmap_ipv4_target_t target)
{
    if (!rate_gate(scan)) return false;
    char ip[INET_ADDRSTRLEN] = {0};
    struct in_addr in = {.s_addr = target.addr_be};
    if (inet_ntop(AF_INET, &in, ip, sizeof(ip)) == NULL) return false;
    char request[112];
    const int request_len = snprintf(request, sizeof(request),
                                     "HEAD / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", ip);
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) return false;
    size_t sent = 0;
    const TickType_t started = xTaskGetTickCount();
    const uint32_t allowed_ms = probe_wait_ms(scan);
    const TickType_t timeout_ticks = ms_to_nonzero_ticks(allowed_ms == 0 ? 1 : allowed_ms);
    while (sent < (size_t)request_len && !scan_should_stop(scan)) {
        const ssize_t n = send(fd, request + sent, (size_t)request_len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if ((TickType_t)(xTaskGetTickCount() - started) >= timeout_ticks) break;
            vTaskDelay(1);
            continue;
        }
        return false;
    }
    return sent == (size_t)request_len;
}

static void process_service_job(nmap_worker_arg_t *worker, uint32_t job_index)
{
    struct nmap_scan *scan = worker->scan;
    const nmap_service_endpoint_t endpoint = scan->u.service.endpoints[job_index];
    nmap_service_result_t result;
    memset(&result, 0, sizeof(result));
    result.target = endpoint.target;
    result.port = endpoint.port;
    result.service = NMAP_SERVICE_UNKNOWN;
    result.reason = NMAP_REASON_NONE;

    int fd = -1;
    nmap_port_state_t connect_state = NMAP_PORT_ERROR;
    nmap_reason_t connect_reason = NMAP_REASON_IO_ERROR;
    uint32_t elapsed_ms = 0;
    uint8_t attempts = 0;
    tcp_probe(scan, endpoint.target, endpoint.port, true, &fd,
              &connect_state, &connect_reason, &elapsed_ms, &attempts);
    (void)elapsed_ms;
    (void)attempts;
    if (connect_state != NMAP_PORT_OPEN || fd < 0) {
        result.reason = connect_reason;
        if (scan_should_stop(scan)) result.result_flags |= NMAP_RESULT_PARTIAL;
        complete_service_job(scan, &result,
                             connect_state == NMAP_PORT_ERROR && connect_reason != NMAP_REASON_CANCELLED);
        return;
    }

    const nmap_service_probe_profile_t profile =
        nmap_internal_service_profile_for_port(endpoint.probe_profile, endpoint.port);
    bool active_http = false;
    bool io_error = false;
    bool truncated = false;
    uint8_t *capture = scan->u.service.capture[worker->worker_index];
    size_t len = 0;

    if (profile == NMAP_SERVICE_PROBE_HTTP_HEAD) {
        active_http = true;
        if (!send_http_head(scan, fd, endpoint.target)) io_error = !scan_should_stop(scan);
        else len = read_service_response(scan, fd, capture, scan->u.service.capture_bytes, &truncated, &io_error);
    } else {
        len = read_service_response(scan, fd, capture, scan->u.service.capture_bytes, &truncated, &io_error);
    }
    close(fd);

    nmap_internal_service_match(capture, len, truncated, active_http, &result);
    result.target = endpoint.target;
    result.port = endpoint.port;
    if (len > 0) result.reason = NMAP_REASON_REPLY;
    else if (io_error) result.reason = NMAP_REASON_IO_ERROR;
    else if (scan_should_stop(scan)) {
        result.reason = scan->cancel_requested ? NMAP_REASON_CANCELLED : NMAP_REASON_TIMEOUT;
        result.result_flags |= NMAP_RESULT_PARTIAL;
    } else result.reason = NMAP_REASON_TIMEOUT;
    complete_service_job(scan, &result, io_error);
}

static void nmap_worker_task(void *arg)
{
    nmap_worker_arg_t *worker = (nmap_worker_arg_t *)arg;
    struct nmap_scan *scan = worker->scan;
    uint32_t job_index;
    while (scan_take_next_job(scan, &job_index)) {
        switch (scan->kind) {
        case NMAP_SCAN_DISCOVERY: process_discovery_job(worker, job_index); break;
        case NMAP_SCAN_PORT: process_port_job(worker, job_index); break;
        case NMAP_SCAN_SERVICE: process_service_job(worker, job_index); break;
        default: scan->cancel_requested = true; break;
        }
    }
    xTaskNotifyGive(scan->coordinator_task);
    vTaskDelete(NULL);
}

static void nmap_coordinator_task(void *arg)
{
    struct nmap_scan *scan = (struct nmap_scan *)arg;
    uint8_t worker_count = scan->timing.max_inflight;
    if (worker_count > scan->total_jobs) worker_count = (uint8_t)scan->total_jobs;
    uint8_t created = 0;
    for (uint8_t i = 0; i < worker_count; ++i) {
        s_workers[i].scan = scan;
        s_workers[i].worker_index = i;
        const BaseType_t rc = xTaskCreate(nmap_worker_task, "nmap_worker", NMAP_WORKER_STACK,
                                          &s_workers[i], NMAP_TASK_PRIORITY, NULL);
        if (rc != pdPASS) {
            scan->summary.terminal_error = ESP_ERR_NO_MEM;
            scan->cancel_requested = true;
            break;
        }
        ++created;
    }
    for (uint8_t i = 0; i < created; ++i) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xSemaphoreTake(scan->callback_mutex, portMAX_DELAY);
    scan->summary.cancelled = scan->cancel_requested;
    scan->summary.timed_out = scan->timed_out;
    scan->summary.partial = scan->summary.completed_jobs < scan->summary.total_jobs;
    scan->summary.elapsed_ms = ticks_to_ms((TickType_t)(xTaskGetTickCount() - scan->start_tick));
    emit_progress_locked(scan);
    xSemaphoreGive(scan->callback_mutex);

    scan->coordinator_task = NULL;
    xSemaphoreGive(scan->done_sem);
    vTaskDelete(NULL);
}

static esp_err_t launch_reserved_scan(nmap_scan_handle_t *out_scan)
{
    s_scan.summary.total_jobs = s_scan.total_jobs;
    const BaseType_t rc = xTaskCreate(nmap_coordinator_task, "nmap_scan", NMAP_COORDINATOR_STACK,
                                      &s_scan, NMAP_TASK_PRIORITY, &s_scan.coordinator_task);
    if (rc != pdPASS) {
        s_scan.summary.terminal_error = ESP_ERR_NO_MEM;
        release_reserved_scan();
        return ESP_ERR_NO_MEM;
    }
    *out_scan = &s_scan;
    return ESP_OK;
}

static esp_err_t copy_discovery_targets(const nmap_discovery_config_t *config, const esp_netif_ip_info_t *info)
{
    nmap_discovery_state_t *state = &s_scan.u.discovery;
    state->target_count = 0;
    if (config->use_local_subnet) {
        const int prefix = nmap_internal_netmask_prefix(info->netmask.addr);
        if (prefix < 24 || prefix > 30) return ESP_ERR_NOT_SUPPORTED;
        const uint32_t ip = lwip_ntohl(info->ip.addr);
        const uint32_t mask = lwip_ntohl(info->netmask.addr);
        const uint32_t network = ip & mask;
        const uint32_t broadcast = network | ~mask;
        for (uint32_t host = network + 1u; host < broadcast; ++host) {
            const uint32_t addr_be = lwip_htonl(host);
            if (addr_be == info->ip.addr) continue;
            nmap_ipv4_target_t target = {.addr_be = addr_be};
            if (!append_unique_target(state->targets, &state->target_count, NMAP_DISCOVERY_MAX_TARGETS, target)) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        return state->target_count > 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (config->targets == NULL || config->target_count == 0 || config->target_count > NMAP_DISCOVERY_MAX_TARGETS) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint16_t i = 0; i < config->target_count; ++i) {
        esp_err_t err = validate_lan_target(config->targets[i], info, true);
        if (err != ESP_OK) return err;
        if (!append_unique_target(state->targets, &state->target_count, NMAP_DISCOVERY_MAX_TARGETS, config->targets[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return state->target_count > 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nmap_discovery_start(const nmap_discovery_config_t *config, nmap_scan_handle_t *out_scan)
{
    if (config == NULL || out_scan == NULL) return ESP_ERR_INVALID_ARG;
    esp_netif_ip_info_t info;
    esp_err_t err = get_sta_ip_info(&info);
    if (err != ESP_OK) return err;
    err = reserve_scan(NMAP_SCAN_DISCOVERY, &config->timing);
    if (err != ESP_OK) return err;

    nmap_discovery_state_t *state = &s_scan.u.discovery;
    memset(state, 0, sizeof(*state));
    err = copy_discovery_targets(config, &info);
    if (err != ESP_OK) {
        release_reserved_scan();
        return err;
    }
    state->enable_icmp = config->enable_icmp;
    state->result_cb = config->result_cb;
    state->progress_cb = config->progress_cb;
    state->user_ctx = config->user_ctx;

    uint16_t fallback_count = 0;
    if (config->tcp_fallback_port_count == 0) {
        for (size_t i = 0; i < sizeof(s_default_discovery_ports) / sizeof(s_default_discovery_ports[0]); ++i) {
            (void)append_unique_port(state->fallback_ports, &fallback_count,
                                     NMAP_DISCOVERY_MAX_FALLBACK_PORTS, s_default_discovery_ports[i]);
        }
    } else {
        if (config->tcp_fallback_ports == NULL ||
            config->tcp_fallback_port_count > NMAP_DISCOVERY_MAX_FALLBACK_PORTS) {
            release_reserved_scan();
            return ESP_ERR_INVALID_ARG;
        }
        for (uint8_t i = 0; i < config->tcp_fallback_port_count; ++i) {
            if (!append_unique_port(state->fallback_ports, &fallback_count,
                                    NMAP_DISCOVERY_MAX_FALLBACK_PORTS, config->tcp_fallback_ports[i])) {
                release_reserved_scan();
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    state->fallback_port_count = (uint8_t)fallback_count;
    s_scan.total_jobs = state->target_count;
    return launch_reserved_scan(out_scan);
}

esp_err_t nmap_port_scan_start(const nmap_port_scan_config_t *config, nmap_scan_handle_t *out_scan)
{
    if (config == NULL || out_scan == NULL || config->targets == NULL || config->ports == NULL ||
        config->target_count == 0 || config->port_count == 0 ||
        config->target_count > NMAP_PORT_SCAN_MAX_TARGETS || config->port_count > NMAP_PORT_SCAN_MAX_PORTS) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t info;
    esp_err_t err = get_sta_ip_info(&info);
    if (err != ESP_OK) return err;
    err = reserve_scan(NMAP_SCAN_PORT, &config->timing);
    if (err != ESP_OK) return err;
    nmap_port_state_internal_t *state = &s_scan.u.port;
    memset(state, 0, sizeof(*state));
    for (uint16_t i = 0; i < config->target_count; ++i) {
        err = validate_lan_target(config->targets[i], &info, true);
        if (err != ESP_OK || !append_unique_target(state->targets, &state->target_count,
                                                   NMAP_PORT_SCAN_MAX_TARGETS, config->targets[i])) {
            release_reserved_scan();
            return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
        }
    }
    for (uint16_t i = 0; i < config->port_count; ++i) {
        if (!append_unique_port(state->ports, &state->port_count, NMAP_PORT_SCAN_MAX_PORTS, config->ports[i])) {
            release_reserved_scan();
            return ESP_ERR_INVALID_ARG;
        }
    }
    const uint32_t jobs = (uint32_t)state->target_count * state->port_count;
    if (jobs == 0 || jobs > NMAP_PORT_SCAN_MAX_JOBS) {
        release_reserved_scan();
        return ESP_ERR_INVALID_ARG;
    }
    state->result_cb = config->result_cb;
    state->progress_cb = config->progress_cb;
    state->user_ctx = config->user_ctx;
    s_scan.total_jobs = jobs;
    return launch_reserved_scan(out_scan);
}

esp_err_t nmap_service_scan_start(const nmap_service_scan_config_t *config, nmap_scan_handle_t *out_scan)
{
    if (config == NULL || out_scan == NULL || config->endpoints == NULL || config->endpoint_count == 0 ||
        config->endpoint_count > NMAP_SERVICE_MAX_ENDPOINTS) return ESP_ERR_INVALID_ARG;
    const uint16_t capture_bytes = config->capture_bytes == 0 ? NMAP_DEFAULT_CAPTURE_BYTES : config->capture_bytes;
    if (capture_bytes < 32 || capture_bytes > NMAP_SERVICE_CAPTURE_MAX) return ESP_ERR_INVALID_ARG;
    esp_netif_ip_info_t info;
    esp_err_t err = get_sta_ip_info(&info);
    if (err != ESP_OK) return err;
    err = reserve_scan(NMAP_SCAN_SERVICE, &config->timing);
    if (err != ESP_OK) return err;
    nmap_service_state_t *state = &s_scan.u.service;
    memset(state, 0, sizeof(*state));
    for (uint16_t i = 0; i < config->endpoint_count; ++i) {
        const nmap_service_endpoint_t endpoint = config->endpoints[i];
        if (endpoint.port == 0 || endpoint.probe_profile > NMAP_SERVICE_PROBE_HTTP_HEAD ||
            validate_lan_target(endpoint.target, &info, true) != ESP_OK) {
            release_reserved_scan();
            return ESP_ERR_INVALID_ARG;
        }
        bool duplicate = false;
        for (uint16_t j = 0; j < state->endpoint_count; ++j) {
            if (state->endpoints[j].target.addr_be == endpoint.target.addr_be &&
                state->endpoints[j].port == endpoint.port &&
                state->endpoints[j].probe_profile == endpoint.probe_profile) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) state->endpoints[state->endpoint_count++] = endpoint;
    }
    if (state->endpoint_count == 0) {
        release_reserved_scan();
        return ESP_ERR_INVALID_ARG;
    }
    state->capture_bytes = capture_bytes;
    state->result_cb = config->result_cb;
    state->progress_cb = config->progress_cb;
    state->user_ctx = config->user_ctx;
    s_scan.total_jobs = state->endpoint_count;
    return launch_reserved_scan(out_scan);
}

static esp_err_t cancel_kind(nmap_scan_handle_t scan, nmap_scan_kind_t kind)
{
    if (scan != &s_scan || !s_scan.in_use || s_scan.kind != kind) return ESP_ERR_INVALID_STATE;
    s_scan.cancel_requested = true;
    return ESP_OK;
}

static esp_err_t wait_kind(nmap_scan_handle_t scan, nmap_scan_kind_t kind, uint32_t timeout_ms)
{
    if (scan != &s_scan || !s_scan.in_use || s_scan.kind != kind) return ESP_ERR_INVALID_STATE;
    const TickType_t ticks = timeout_ms == NMAP_WAIT_FOREVER ? portMAX_DELAY : ms_to_nonzero_ticks(timeout_ms);
    if (xSemaphoreTake(s_scan.done_sem, ticks) != pdTRUE) return ESP_ERR_TIMEOUT;
    taskENTER_CRITICAL(&s_guard);
    s_scan.in_use = false;
    taskEXIT_CRITICAL(&s_guard);
    return s_scan.summary.terminal_error;
}

esp_err_t nmap_discovery_cancel(nmap_scan_handle_t scan) { return cancel_kind(scan, NMAP_SCAN_DISCOVERY); }
esp_err_t nmap_discovery_wait(nmap_scan_handle_t scan, uint32_t timeout_ms) { return wait_kind(scan, NMAP_SCAN_DISCOVERY, timeout_ms); }
esp_err_t nmap_port_scan_cancel(nmap_scan_handle_t scan) { return cancel_kind(scan, NMAP_SCAN_PORT); }
esp_err_t nmap_port_scan_wait(nmap_scan_handle_t scan, uint32_t timeout_ms) { return wait_kind(scan, NMAP_SCAN_PORT, timeout_ms); }
esp_err_t nmap_service_scan_cancel(nmap_scan_handle_t scan) { return cancel_kind(scan, NMAP_SCAN_SERVICE); }
esp_err_t nmap_service_scan_wait(nmap_scan_handle_t scan, uint32_t timeout_ms) { return wait_kind(scan, NMAP_SCAN_SERVICE, timeout_ms); }

esp_err_t nmap_scan_get_summary(nmap_scan_handle_t scan, nmap_scan_summary_t *out_summary)
{
    if (scan != &s_scan || out_summary == NULL || !s_scan.summary_valid) return ESP_ERR_INVALID_ARG;
    if (s_scan.callback_mutex != NULL) xSemaphoreTake(s_scan.callback_mutex, portMAX_DELAY);
    *out_summary = s_scan.summary;
    out_summary->cancelled = s_scan.cancel_requested;
    out_summary->timed_out = s_scan.timed_out;
    out_summary->partial = out_summary->completed_jobs < out_summary->total_jobs;
    if (s_scan.in_use) out_summary->elapsed_ms = ticks_to_ms((TickType_t)(xTaskGetTickCount() - s_scan.start_tick));
    if (s_scan.callback_mutex != NULL) xSemaphoreGive(s_scan.callback_mutex);
    return ESP_OK;
}
