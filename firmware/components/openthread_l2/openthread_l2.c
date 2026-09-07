#include "openthread_l2.h"

#include <string.h>

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/dataset.h"
#include "openthread/ip6.h"
#include "openthread/joiner.h"
#include "openthread/link.h"
#include "openthread/thread.h"
#if OPENTHREAD_FTD
#include "openthread/thread_ftd.h"
#endif

#include "openthread_l2_internal.h"

#define OPENTHREAD_L2_LOCK_TIMEOUT_MS 1000u
#define OPENTHREAD_L2_POLL_INTERVAL_MS 50u
#define OPENTHREAD_L2_INTERNAL_DISCOVERY_MAX 16u

static portMUX_TYPE s_context_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_control_active;

typedef struct {
    bool active;
    bool completed;
    bool restore_ip6;
    otInstance *instance;
    size_t count;
    bool truncated;
    openthread_network_t entries[OPENTHREAD_L2_INTERNAL_DISCOVERY_MAX];
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
} discovery_context_t;

static discovery_context_t s_discovery;

typedef struct {
    bool active;
    bool completed;
    otError error;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
} joiner_context_t;

static joiner_context_t s_joiner;

static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0u) {
        *p++ = 0u;
    }
}

static size_t bounded_strlen(const char *text, size_t max_len_plus_one)
{
    size_t len = 0;
    if (text == NULL) {
        return 0;
    }
    while (len < max_len_plus_one && text[len] != '\0') {
        ++len;
    }
    return len;
}

static uint32_t normalized_timeout(uint32_t requested, uint32_t default_ms)
{
    return requested == 0u ? default_ms : requested;
}

static bool timeout_valid(uint32_t timeout_ms)
{
    return timeout_ms > 0u && timeout_ms <= OPENTHREAD_OPERATION_TIMEOUT_MAX_MS;
}

static openthread_status_t map_ot_error(otError error)
{
    switch (error) {
    case OT_ERROR_NONE: return OPENTHREAD_STATUS_OK;
    case OT_ERROR_INVALID_ARGS: return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    case OT_ERROR_INVALID_STATE: return OPENTHREAD_STATUS_INVALID_STATE;
    case OT_ERROR_BUSY: return OPENTHREAD_STATUS_BUSY;
    case OT_ERROR_NO_BUFS: return OPENTHREAD_STATUS_NO_MEMORY;
    case OT_ERROR_NOT_FOUND: return OPENTHREAD_STATUS_NOT_FOUND;
    case OT_ERROR_SECURITY: return OPENTHREAD_STATUS_SECURITY;
    case OT_ERROR_RESPONSE_TIMEOUT: return OPENTHREAD_STATUS_TIMEOUT;
    default: return OPENTHREAD_STATUS_INTERNAL;
    }
}

static openthread_role_t map_role(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return OPENTHREAD_ROLE_DISABLED;
    case OT_DEVICE_ROLE_DETACHED: return OPENTHREAD_ROLE_DETACHED;
    case OT_DEVICE_ROLE_CHILD: return OPENTHREAD_ROLE_CHILD;
    case OT_DEVICE_ROLE_ROUTER: return OPENTHREAD_ROLE_ROUTER;
    case OT_DEVICE_ROLE_LEADER: return OPENTHREAD_ROLE_LEADER;
    default: return OPENTHREAD_ROLE_DISABLED;
    }
}

static bool role_is_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

static bool acquire_ot_lock(void)
{
    return esp_openthread_lock_acquire(pdMS_TO_TICKS(OPENTHREAD_L2_LOCK_TIMEOUT_MS));
}

static SemaphoreHandle_t ensure_binary_semaphore(StaticSemaphore_t *storage, SemaphoreHandle_t *handle)
{
    portENTER_CRITICAL(&s_context_lock);
    if (*handle == NULL) {
        *handle = xSemaphoreCreateBinaryStatic(storage);
    }
    SemaphoreHandle_t result = *handle;
    portEXIT_CRITICAL(&s_context_lock);
    return result;
}

static void drain_semaphore(SemaphoreHandle_t sem)
{
    while (sem != NULL && xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static bool try_begin_control(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_context_lock);
    if (!s_control_active && !s_discovery.active) {
        s_control_active = true;
        ok = true;
    }
    portEXIT_CRITICAL(&s_context_lock);
    return ok;
}

static void end_control(void)
{
    portENTER_CRITICAL(&s_context_lock);
    s_control_active = false;
    portEXIT_CRITICAL(&s_context_lock);
}

static void copy_ext_address(uint8_t dst[8], const otExtAddress *src)
{
    memcpy(dst, src->m8, 8u);
}

static void normalize_discovery_result(openthread_network_t *out, const otActiveScanResult *in)
{
    memset(out, 0, sizeof(*out));
    out->channel = in->mChannel;
    out->pan_id = in->mPanId;
    memcpy(out->extended_pan_id, in->mExtendedPanId.m8, sizeof(out->extended_pan_id));
    copy_ext_address(out->extended_address, &in->mExtAddress);
    memcpy(out->network_name, in->mNetworkName.m8, OPENTHREAD_NETWORK_NAME_MAX);
    out->network_name[OPENTHREAD_NETWORK_NAME_MAX] = '\0';
    out->rssi_dbm = in->mRssi;
    out->lqi = in->mLqi;
    out->thread_version = (uint8_t)in->mVersion;
    out->joinable = in->mIsJoinable;
    out->native_commissioner = in->mIsNative;
    out->joiner_udp_port = in->mJoinerUdpPort;
    out->steering_data_len = in->mSteeringData.mLength <= OPENTHREAD_STEERING_DATA_MAX
                                 ? in->mSteeringData.mLength
                                 : OPENTHREAD_STEERING_DATA_MAX;
    memcpy(out->steering_data, in->mSteeringData.m8, out->steering_data_len);
}

static void discovery_callback(otActiveScanResult *scan_result, void *context)
{
    (void)context;

    if (scan_result != NULL) {
        if (!scan_result->mDiscover) {
            return;
        }
        openthread_network_t normalized;
        normalize_discovery_result(&normalized, scan_result);

        portENTER_CRITICAL(&s_context_lock);
        if (s_discovery.active) {
            openthread_l2_merge_network(s_discovery.entries,
                                        &s_discovery.count,
                                        OPENTHREAD_L2_INTERNAL_DISCOVERY_MAX,
                                        &s_discovery.truncated,
                                        &normalized);
        }
        portEXIT_CRITICAL(&s_context_lock);
        return;
    }

    bool restore_ip6 = false;
    otInstance *instance = NULL;
    portENTER_CRITICAL(&s_context_lock);
    if (s_discovery.active) {
        restore_ip6 = s_discovery.restore_ip6;
        instance = s_discovery.instance;
    }
    portEXIT_CRITICAL(&s_context_lock);

    if (restore_ip6 && instance != NULL && otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_DISABLED) {
        (void)otIp6SetEnabled(instance, false);
    }

    SemaphoreHandle_t done = NULL;
    portENTER_CRITICAL(&s_context_lock);
    if (s_discovery.active) {
        s_discovery.completed = true;
        s_discovery.active = false;
        s_discovery.restore_ip6 = false;
        s_discovery.instance = NULL;
        done = s_discovery.done;
    }
    portEXIT_CRITICAL(&s_context_lock);
    if (done != NULL) {
        (void)xSemaphoreGive(done);
    }
}

static void copy_discovery_snapshot(openthread_network_t *networks,
                                    size_t capacity,
                                    openthread_discovery_result_t *result)
{
    size_t available;
    bool internal_truncated;

    portENTER_CRITICAL(&s_context_lock);
    available = s_discovery.count;
    internal_truncated = s_discovery.truncated;
    size_t copy_count = available < capacity ? available : capacity;
    if (copy_count > 0u && networks != NULL) {
        memcpy(networks, s_discovery.entries, copy_count * sizeof(*networks));
    }
    result->count = copy_count;
    result->truncated = internal_truncated || copy_count < available;
    portEXIT_CRITICAL(&s_context_lock);
}

openthread_status_t openthread_discover_networks(const openthread_discovery_options_t *options,
                                                  openthread_network_t *networks,
                                                  size_t capacity,
                                                  openthread_discovery_result_t *result)
{
    if (result == NULL || (capacity > 0u && networks == NULL)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));

    openthread_discovery_options_t effective = {
        .channel_mask = 0u,
        .pan_id = 0xffffu,
        .joiner_flag = false,
        .enable_eui64_filtering = false,
        .timeout_ms = OPENTHREAD_DISCOVERY_TIMEOUT_DEFAULT_MS,
    };
    if (options != NULL) {
        effective = *options;
        effective.timeout_ms = normalized_timeout(effective.timeout_ms, OPENTHREAD_DISCOVERY_TIMEOUT_DEFAULT_MS);
    }
    if (!timeout_valid(effective.timeout_ms)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }

    SemaphoreHandle_t done = ensure_binary_semaphore(&s_discovery.done_storage, &s_discovery.done);
    if (done == NULL) {
        return OPENTHREAD_STATUS_NO_MEMORY;
    }
    drain_semaphore(done);

    portENTER_CRITICAL(&s_context_lock);
    if (s_discovery.active || s_control_active) {
        portEXIT_CRITICAL(&s_context_lock);
        return OPENTHREAD_STATUS_BUSY;
    }
    s_discovery.active = true;
    s_discovery.completed = false;
    s_discovery.restore_ip6 = false;
    s_discovery.instance = NULL;
    s_discovery.count = 0u;
    s_discovery.truncated = false;
    memset(s_discovery.entries, 0, sizeof(s_discovery.entries));
    portEXIT_CRITICAL(&s_context_lock);

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        portENTER_CRITICAL(&s_context_lock);
        s_discovery.active = false;
        portEXIT_CRITICAL(&s_context_lock);
        return OPENTHREAD_STATUS_INVALID_STATE;
    }
    if (!acquire_ot_lock()) {
        portENTER_CRITICAL(&s_context_lock);
        s_discovery.active = false;
        portEXIT_CRITICAL(&s_context_lock);
        return OPENTHREAD_STATUS_BUSY;
    }

    bool restore_ip6 = false;
    if (!otIp6IsEnabled(instance)) {
        otError ip6_error = otIp6SetEnabled(instance, true);
        if (ip6_error != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            portENTER_CRITICAL(&s_context_lock);
            s_discovery.active = false;
            portEXIT_CRITICAL(&s_context_lock);
            return map_ot_error(ip6_error);
        }
        restore_ip6 = true;
    }

    uint32_t supported = otLinkGetSupportedChannelMask(instance);
    uint32_t channels = effective.channel_mask == 0u ? supported : (effective.channel_mask & supported);
    if (channels == 0u) {
        if (restore_ip6 && otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_DISABLED) {
            (void)otIp6SetEnabled(instance, false);
        }
        esp_openthread_lock_release();
        portENTER_CRITICAL(&s_context_lock);
        s_discovery.active = false;
        portEXIT_CRITICAL(&s_context_lock);
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }

    portENTER_CRITICAL(&s_context_lock);
    s_discovery.restore_ip6 = restore_ip6;
    s_discovery.instance = instance;
    portEXIT_CRITICAL(&s_context_lock);

    otError start_error = otThreadDiscover(instance,
                                           channels,
                                           effective.pan_id,
                                           effective.joiner_flag,
                                           effective.enable_eui64_filtering,
                                           discovery_callback,
                                           NULL);
    if (start_error != OT_ERROR_NONE) {
        if (restore_ip6 && otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_DISABLED) {
            (void)otIp6SetEnabled(instance, false);
        }
        esp_openthread_lock_release();
        portENTER_CRITICAL(&s_context_lock);
        s_discovery.active = false;
        s_discovery.restore_ip6 = false;
        s_discovery.instance = NULL;
        portEXIT_CRITICAL(&s_context_lock);
        return map_ot_error(start_error);
    }
    esp_openthread_lock_release();

    BaseType_t waited = xSemaphoreTake(done, pdMS_TO_TICKS(effective.timeout_ms));
    bool completed;
    portENTER_CRITICAL(&s_context_lock);
    completed = s_discovery.completed;
    portEXIT_CRITICAL(&s_context_lock);

    copy_discovery_snapshot(networks, capacity, result);
    if (waited != pdTRUE && !completed) {
        return OPENTHREAD_STATUS_TIMEOUT;
    }
    return OPENTHREAD_STATUS_OK;
}

openthread_status_t openthread_get_state_snapshot(openthread_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    memset(snapshot, 0, sizeof(*snapshot));

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        return OPENTHREAD_STATUS_INVALID_STATE;
    }
    if (!acquire_ot_lock()) {
        return OPENTHREAD_STATUS_BUSY;
    }

    snapshot->commissioned = otDatasetIsCommissioned(instance);
    snapshot->ip6_enabled = otIp6IsEnabled(instance);
    otDeviceRole native_role = otThreadGetDeviceRole(instance);
    snapshot->role = map_role(native_role);
    snapshot->attach_duration_s = otThreadGetCurrentAttachDuration(instance);

    snapshot->channel = otLinkGetChannel(instance);
    snapshot->pan_id = otLinkGetPanId(instance);
    const otExtendedPanId *ext_pan_id = otThreadGetExtendedPanId(instance);
    if (ext_pan_id != NULL) {
        memcpy(snapshot->extended_pan_id, ext_pan_id->m8, sizeof(snapshot->extended_pan_id));
    }
    const char *network_name = otThreadGetNetworkName(instance);
    if (network_name != NULL) {
        size_t name_len = bounded_strlen(network_name, OPENTHREAD_NETWORK_NAME_MAX);
        memcpy(snapshot->network_name, network_name, name_len);
        snapshot->network_name[name_len] = '\0';
    }

    snapshot->rloc16 = otThreadGetRloc16(instance);
    snapshot->partition_id = otThreadGetPartitionId(instance);

    otLeaderData leader_data;
    if (otThreadGetLeaderData(instance, &leader_data) == OT_ERROR_NONE) {
        snapshot->has_leader_data = true;
        snapshot->partition_id = leader_data.mPartitionId;
        snapshot->leader_router_id = leader_data.mLeaderRouterId;
        snapshot->leader_weight = leader_data.mWeighting;
        snapshot->data_version = leader_data.mDataVersion;
        snapshot->stable_data_version = leader_data.mStableDataVersion;
    }

    if (native_role == OT_DEVICE_ROLE_CHILD) {
        otRouterInfo parent;
        if (otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
            snapshot->has_parent = true;
            copy_ext_address(snapshot->parent_ext_address, &parent.mExtAddress);
            snapshot->parent_rloc16 = parent.mRloc16;
        }
        if (otThreadGetParentAverageRssi(instance, &snapshot->parent_average_rssi_dbm) == OT_ERROR_NONE) {
            snapshot->has_parent_average_rssi = true;
        }
        if (otThreadGetParentLastRssi(instance, &snapshot->parent_last_rssi_dbm) == OT_ERROR_NONE) {
            snapshot->has_parent_last_rssi = true;
        }
    }

    const otIpCounters *ip_counters = otThreadGetIp6Counters(instance);
    if (ip_counters != NULL) {
        snapshot->ip_tx_success = ip_counters->mTxSuccess;
        snapshot->ip_rx_success = ip_counters->mRxSuccess;
        snapshot->ip_tx_failure = ip_counters->mTxFailure;
        snapshot->ip_rx_failure = ip_counters->mRxFailure;
    }
    const otMleCounters *mle_counters = otThreadGetMleCounters(instance);
    if (mle_counters != NULL) {
        snapshot->attach_attempts = mle_counters->mAttachAttempts;
        snapshot->parent_changes = mle_counters->mParentChanges;
    }

    esp_openthread_lock_release();
    return OPENTHREAD_STATUS_OK;
}

static bool topology_contains_rloc(const openthread_peer_t *peers, size_t count, uint16_t rloc16, size_t *index)
{
    for (size_t i = 0; i < count; ++i) {
        if (peers[i].rloc16 == rloc16) {
            if (index != NULL) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

static void topology_append(openthread_peer_t *peers,
                            size_t capacity,
                            openthread_topology_result_t *result,
                            const openthread_peer_t *peer)
{
    if (result->count >= capacity) {
        result->truncated = true;
        return;
    }
    peers[result->count++] = *peer;
}

openthread_status_t openthread_get_local_topology(openthread_peer_t *peers,
                                                   size_t capacity,
                                                   openthread_topology_result_t *result)
{
    if (result == NULL || (capacity > 0u && peers == NULL)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
#if OPENTHREAD_FTD
    result->ftd_details_available = true;
#endif

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        return OPENTHREAD_STATUS_INVALID_STATE;
    }
    if (!acquire_ot_lock()) {
        return OPENTHREAD_STATUS_BUSY;
    }

    otDeviceRole role = otThreadGetDeviceRole(instance);
    uint16_t self_rloc = otThreadGetRloc16(instance);
    uint16_t parent_rloc = 0xffffu;
    bool has_parent = false;
    if (role == OT_DEVICE_ROLE_CHILD) {
        otRouterInfo parent;
        if (otThreadGetParentInfo(instance, &parent) == OT_ERROR_NONE) {
            has_parent = true;
            parent_rloc = parent.mRloc16;
        }
    }

    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor;
    while (otThreadGetNextNeighborInfo(instance, &iterator, &neighbor) == OT_ERROR_NONE) {
        openthread_peer_t peer = {0};
        peer.kind = neighbor.mIsChild ? OPENTHREAD_PEER_CHILD : OPENTHREAD_PEER_NEIGHBOR;
        if (has_parent && neighbor.mRloc16 == parent_rloc) {
            peer.kind = OPENTHREAD_PEER_PARENT;
        }
        copy_ext_address(peer.ext_address, &neighbor.mExtAddress);
        peer.rloc16 = neighbor.mRloc16;
        peer.thread_version = neighbor.mVersion;
        peer.age_s = neighbor.mAge;
        peer.has_average_rssi = true;
        peer.average_rssi_dbm = neighbor.mAverageRssi;
        peer.has_last_rssi = true;
        peer.last_rssi_dbm = neighbor.mLastRssi;
        peer.has_link_quality_in = true;
        peer.link_quality_in = neighbor.mLinkQualityIn;
        peer.rx_on_when_idle = neighbor.mRxOnWhenIdle;
        peer.full_thread_device = neighbor.mFullThreadDevice;
        peer.full_network_data = neighbor.mFullNetworkData;
        topology_append(peers, capacity, result, &peer);
    }

#if OPENTHREAD_FTD
    for (uint16_t router_id = 0u; router_id <= OT_NETWORK_MAX_ROUTER_ID; ++router_id) {
        otRouterInfo router;
        if (otThreadGetRouterInfo(instance, router_id, &router) != OT_ERROR_NONE || !router.mAllocated ||
            router.mRloc16 == self_rloc) {
            continue;
        }

        size_t existing_index = 0u;
        if (topology_contains_rloc(peers, result->count, router.mRloc16, &existing_index)) {
            openthread_peer_t *existing = &peers[existing_index];
            if (existing->kind == OPENTHREAD_PEER_NEIGHBOR) {
                existing->kind = OPENTHREAD_PEER_ROUTER;
            }
            existing->has_link_quality_out = true;
            existing->link_quality_out = router.mLinkQualityOut;
            existing->has_path_cost = true;
            existing->path_cost = router.mPathCost;
            continue;
        }

        openthread_peer_t peer = {0};
        peer.kind = OPENTHREAD_PEER_ROUTER;
        copy_ext_address(peer.ext_address, &router.mExtAddress);
        peer.rloc16 = router.mRloc16;
        peer.thread_version = router.mVersion;
        peer.age_s = router.mAge;
        peer.has_link_quality_in = true;
        peer.link_quality_in = router.mLinkQualityIn;
        peer.has_link_quality_out = true;
        peer.link_quality_out = router.mLinkQualityOut;
        peer.has_path_cost = true;
        peer.path_cost = router.mPathCost;
        topology_append(peers, capacity, result, &peer);
    }
#endif

    esp_openthread_lock_release();
    return OPENTHREAD_STATUS_OK;
}

static openthread_status_t wait_for_attach(otInstance *instance,
                                            uint32_t timeout_ms,
                                            openthread_attach_result_t *result)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        if (!acquire_ot_lock()) {
            return OPENTHREAD_STATUS_BUSY;
        }
        otDeviceRole role = otThreadGetDeviceRole(instance);
        if (role_is_attached(role)) {
            result->final_role = map_role(role);
            result->attach_duration_s = otThreadGetCurrentAttachDuration(instance);
            esp_openthread_lock_release();
            return OPENTHREAD_STATUS_OK;
        }
        esp_openthread_lock_release();

        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout_ticks) {
            return OPENTHREAD_STATUS_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(OPENTHREAD_L2_POLL_INTERVAL_MS));
    }
}

static openthread_status_t stop_failed_attach_and_restore_ip6(otInstance *instance,
                                                               bool restore_ip6,
                                                               openthread_attach_result_t *result,
                                                               openthread_status_t failure)
{
    if (!acquire_ot_lock()) {
        return failure;
    }
    otDeviceRole role = otThreadGetDeviceRole(instance);
    if (role_is_attached(role)) {
        result->final_role = map_role(role);
        result->attach_duration_s = otThreadGetCurrentAttachDuration(instance);
        esp_openthread_lock_release();
        return OPENTHREAD_STATUS_OK;
    }
    (void)otThreadSetEnabled(instance, false);
    if (restore_ip6) {
        (void)otIp6SetEnabled(instance, false);
    }
    result->final_role = OPENTHREAD_ROLE_DISABLED;
    esp_openthread_lock_release();
    return failure;
}

openthread_status_t openthread_attach_dataset(const openthread_dataset_tlvs_t *dataset,
                                               const openthread_attach_options_t *options,
                                               openthread_attach_result_t *result)
{
    if (dataset == NULL || result == NULL || !openthread_l2_dataset_length_valid(dataset->length)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->final_role = OPENTHREAD_ROLE_DISABLED;

    openthread_attach_options_t effective = {
        .timeout_ms = OPENTHREAD_ATTACH_TIMEOUT_DEFAULT_MS,
        .require_complete_active_dataset = true,
    };
    if (options != NULL) {
        effective = *options;
        effective.timeout_ms = normalized_timeout(effective.timeout_ms, OPENTHREAD_ATTACH_TIMEOUT_DEFAULT_MS);
    }
    if (!timeout_valid(effective.timeout_ms)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }

    otOperationalDatasetTlvs tlvs = {0};
    tlvs.mLength = (uint8_t)dataset->length;
    memcpy(tlvs.mTlvs, dataset->bytes, dataset->length);
    otOperationalDataset parsed = {0};
    otError parse_error = otDatasetParseTlvs(&tlvs, &parsed);
    if (parse_error != OT_ERROR_NONE || (effective.require_complete_active_dataset && !otDatasetIsValid(&tlvs, true))) {
        secure_zero(&parsed, sizeof(parsed));
        secure_zero(&tlvs, sizeof(tlvs));
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }

    if (!try_begin_control()) {
        secure_zero(&parsed, sizeof(parsed));
        secure_zero(&tlvs, sizeof(tlvs));
        return OPENTHREAD_STATUS_BUSY;
    }

    openthread_status_t status = OPENTHREAD_STATUS_INTERNAL;
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        status = OPENTHREAD_STATUS_INVALID_STATE;
        goto cleanup;
    }
    if (!acquire_ot_lock()) {
        status = OPENTHREAD_STATUS_BUSY;
        goto cleanup;
    }

    if (otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED) {
        esp_openthread_lock_release();
        status = OPENTHREAD_STATUS_INVALID_STATE;
        goto cleanup;
    }

    bool restore_ip6 = !otIp6IsEnabled(instance);
    otError set_error = otDatasetSetActiveTlvs(instance, &tlvs);
    if (set_error != OT_ERROR_NONE) {
        esp_openthread_lock_release();
        status = map_ot_error(set_error);
        goto cleanup;
    }
    result->dataset_persisted = true;

    if (restore_ip6) {
        otError ip6_error = otIp6SetEnabled(instance, true);
        if (ip6_error != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            status = map_ot_error(ip6_error);
            goto cleanup;
        }
    }
    otError thread_error = otThreadSetEnabled(instance, true);
    if (thread_error != OT_ERROR_NONE) {
        if (restore_ip6) {
            (void)otIp6SetEnabled(instance, false);
        }
        esp_openthread_lock_release();
        status = map_ot_error(thread_error);
        goto cleanup;
    }
    esp_openthread_lock_release();

    status = wait_for_attach(instance, effective.timeout_ms, result);
    if (status != OPENTHREAD_STATUS_OK) {
        status = stop_failed_attach_and_restore_ip6(instance, restore_ip6, result, status);
    }

cleanup:
    secure_zero(&parsed, sizeof(parsed));
    secure_zero(&tlvs, sizeof(tlvs));
    end_control();
    return status;
}

static void joiner_callback(otError error, void *context)
{
    (void)context;
    SemaphoreHandle_t done = NULL;
    portENTER_CRITICAL(&s_context_lock);
    if (s_joiner.active) {
        s_joiner.error = error;
        s_joiner.completed = true;
        done = s_joiner.done;
    }
    portEXIT_CRITICAL(&s_context_lock);
    if (done != NULL) {
        (void)xSemaphoreGive(done);
    }
}

openthread_status_t openthread_joiner_join(const openthread_joiner_options_t *options,
                                            openthread_attach_result_t *result)
{
    if (options == NULL || options->pskd == NULL || result == NULL) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    size_t pskd_len = bounded_strlen(options->pskd, OPENTHREAD_JOINER_PSKD_MAX + 1u);
    if (pskd_len < 6u || pskd_len > OPENTHREAD_JOINER_PSKD_MAX) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }
    uint32_t join_timeout = normalized_timeout(options->join_timeout_ms, OPENTHREAD_JOINER_TIMEOUT_DEFAULT_MS);
    uint32_t attach_timeout = normalized_timeout(options->attach_timeout_ms, OPENTHREAD_ATTACH_TIMEOUT_DEFAULT_MS);
    if (!timeout_valid(join_timeout) || !timeout_valid(attach_timeout)) {
        return OPENTHREAD_STATUS_INVALID_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));
    result->final_role = OPENTHREAD_ROLE_DISABLED;
    if (!try_begin_control()) {
        return OPENTHREAD_STATUS_BUSY;
    }

    SemaphoreHandle_t done = ensure_binary_semaphore(&s_joiner.done_storage, &s_joiner.done);
    if (done == NULL) {
        end_control();
        return OPENTHREAD_STATUS_NO_MEMORY;
    }
    drain_semaphore(done);

    char pskd[OPENTHREAD_JOINER_PSKD_MAX + 1u] = {0};
    memcpy(pskd, options->pskd, pskd_len);

    openthread_status_t status = OPENTHREAD_STATUS_INTERNAL;
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        status = OPENTHREAD_STATUS_INVALID_STATE;
        goto cleanup;
    }
    if (!acquire_ot_lock()) {
        status = OPENTHREAD_STATUS_BUSY;
        goto cleanup;
    }
    if (otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED) {
        esp_openthread_lock_release();
        status = OPENTHREAD_STATUS_INVALID_STATE;
        goto cleanup;
    }

    bool restore_ip6 = !otIp6IsEnabled(instance);
    if (restore_ip6) {
        otError ip6_error = otIp6SetEnabled(instance, true);
        if (ip6_error != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            status = map_ot_error(ip6_error);
            goto cleanup;
        }
    }

    portENTER_CRITICAL(&s_context_lock);
    s_joiner.active = true;
    s_joiner.completed = false;
    s_joiner.error = OT_ERROR_NONE;
    portEXIT_CRITICAL(&s_context_lock);

    otError start_error = otJoinerStart(instance,
                                        pskd,
                                        options->provisioning_url,
                                        options->vendor_name,
                                        options->vendor_model,
                                        options->vendor_sw_version,
                                        options->vendor_data,
                                        joiner_callback,
                                        NULL);
    if (start_error != OT_ERROR_NONE) {
        portENTER_CRITICAL(&s_context_lock);
        s_joiner.active = false;
        portEXIT_CRITICAL(&s_context_lock);
        if (restore_ip6) {
            (void)otIp6SetEnabled(instance, false);
        }
        esp_openthread_lock_release();
        status = map_ot_error(start_error);
        goto cleanup;
    }
    esp_openthread_lock_release();

    BaseType_t waited = xSemaphoreTake(done, pdMS_TO_TICKS(join_timeout));
    bool completed;
    otError join_error;
    portENTER_CRITICAL(&s_context_lock);
    completed = s_joiner.completed;
    join_error = s_joiner.error;
    portEXIT_CRITICAL(&s_context_lock);

    if (waited != pdTRUE && !completed) {
        if (acquire_ot_lock()) {
            otJoinerStop(instance);
            if (restore_ip6) {
                (void)otIp6SetEnabled(instance, false);
            }
            esp_openthread_lock_release();
        }
        portENTER_CRITICAL(&s_context_lock);
        s_joiner.active = false;
        portEXIT_CRITICAL(&s_context_lock);
        status = OPENTHREAD_STATUS_TIMEOUT;
        goto cleanup;
    }

    portENTER_CRITICAL(&s_context_lock);
    s_joiner.active = false;
    portEXIT_CRITICAL(&s_context_lock);

    if (join_error != OT_ERROR_NONE) {
        if (acquire_ot_lock()) {
            otJoinerStop(instance);
            if (restore_ip6) {
                (void)otIp6SetEnabled(instance, false);
            }
            esp_openthread_lock_release();
        }
        status = map_ot_error(join_error);
        goto cleanup;
    }

    if (!acquire_ot_lock()) {
        status = OPENTHREAD_STATUS_BUSY;
        goto cleanup;
    }
    result->dataset_persisted = otDatasetIsCommissioned(instance);
    otError thread_error = otThreadSetEnabled(instance, true);
    if (thread_error != OT_ERROR_NONE) {
        if (restore_ip6) {
            (void)otIp6SetEnabled(instance, false);
        }
        esp_openthread_lock_release();
        status = map_ot_error(thread_error);
        goto cleanup;
    }
    esp_openthread_lock_release();

    status = wait_for_attach(instance, attach_timeout, result);
    if (status != OPENTHREAD_STATUS_OK) {
        status = stop_failed_attach_and_restore_ip6(instance, restore_ip6, result, status);
    }

cleanup:
    secure_zero(pskd, sizeof(pskd));
    end_control();
    return status;
}
