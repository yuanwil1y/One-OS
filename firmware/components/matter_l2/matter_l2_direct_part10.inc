    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->ready = true;
    out_info->fabric_index = g_runtime.commissioner.GetFabricIndex();
    out_info->fabric_id = g_runtime.commissioner.GetFabricId();
    out_info->controller_node_id = g_runtime.commissioner.GetNodeId();
    out_info->compressed_fabric_id = g_runtime.commissioner.GetCompressedFabricId();
    return CHIP_STATUS_OK;
}

chip_status_t chip_request_cancel(chip_request_id_t request_id)
{
    if (RequireReady() != CHIP_STATUS_OK || request_id == 0) {
        return CHIP_STATUS_NOT_READY;
    }
    RequestSlot *slot = g_runtime.FindRequest(request_id);
    if (slot) {
        return chip::DeviceLayer::PlatformMgr().ScheduleWork(
                   ScheduleRequestCancel, reinterpret_cast<intptr_t>(slot)) == CHIP_NO_ERROR
                   ? CHIP_STATUS_OK : CHIP_STATUS_INTERNAL;
    }
    if (g_runtime.commission.active && g_runtime.commission.request_id == request_id) {
        return chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleCommissionCancel, 0) == CHIP_NO_ERROR
                   ? CHIP_STATUS_OK : CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_INVALID_ARGUMENT;
}

chip_status_t chip_read_attribute(uint64_t node_id, chip_path_t path, uint32_t timeout_ms,
                                  chip_read_callback_t callback, void *context, chip_request_id_t *out_request_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (!chip::IsOperationalNodeId(node_id) || !callback || !out_request_id) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    RequestSlot *slot = g_runtime.AllocateRequest(RequestKind::Read);
    if (!slot) {
        return CHIP_STATUS_BUSY;
    }
    slot->node_id = node_id;
    slot->path = path;
    slot->read_cb = callback;
    slot->context = context;
    slot->attr_paths[0] = AttributePathParams(path.endpoint_id, path.cluster_id, path.item_id);
    slot->attr_path_count = 1;
    return QueueRequest(slot, timeout_ms, out_request_id);
}

chip_status_t chip_write_attribute(uint64_t node_id, chip_path_t path, const char *bounded_value_json,
                                   uint16_t timed_write_timeout_ms, uint32_t timeout_ms,
                                   chip_operation_callback_t callback, void *context,
                                   chip_request_id_t *out_request_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (!chip::IsOperationalNodeId(node_id) || !callback || !out_request_id ||
        !bounded_string(bounded_value_json, CHIP_L2_MAX_PAYLOAD_JSON)) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    RequestSlot *slot = g_runtime.AllocateRequest(RequestKind::Write);
    if (!slot) {
        return CHIP_STATUS_BUSY;
    }
    slot->node_id = node_id;
    slot->path = path;
    slot->timed_timeout_ms = timed_write_timeout_ms;
    slot->op_cb = callback;
    slot->context = context;
    strcpy(slot->json, bounded_value_json);
    return QueueRequest(slot, timeout_ms, out_request_id);
}

chip_status_t chip_invoke(uint64_t node_id, uint16_t endpoint_id, uint32_t cluster_id, uint32_t command_id,
                          const char *bounded_command_json, uint16_t timed_invoke_timeout_ms,
                          uint32_t timeout_ms, chip_operation_callback_t callback, void *context,
                          chip_request_id_t *out_request_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (!chip::IsOperationalNodeId(node_id) || !callback || !out_request_id ||
        (bounded_command_json && !bounded_string(bounded_command_json, CHIP_L2_MAX_PAYLOAD_JSON))) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    RequestSlot *slot = g_runtime.AllocateRequest(RequestKind::Invoke);
    if (!slot) {
        return CHIP_STATUS_BUSY;
    }
    slot->node_id = node_id;
    slot->path = chip_path_t{endpoint_id, cluster_id, command_id};
    slot->timed_timeout_ms = timed_invoke_timeout_ms;
    slot->op_cb = callback;
    slot->context = context;
    if (bounded_command_json) {
        strcpy(slot->json, bounded_command_json);
    } else {
        strcpy(slot->json, "{}");
    }
    return QueueRequest(slot, timeout_ms, out_request_id);
}

chip_status_t matter_node_probe(uint64_t node_id, uint32_t timeout_ms,
                                matter_node_probe_callback_t callback, void *context,
                                chip_request_id_t *out_request_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (!chip::IsOperationalNodeId(node_id) || !callback || !out_request_id) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    RequestSlot *slot = g_runtime.AllocateRequest(RequestKind::Probe);
    if (!slot) {
        return CHIP_STATUS_BUSY;
    }
    slot->node_id = node_id;
    slot->probe_cb = callback;
    slot->context = context;
    matter_bounds_reset_node(&slot->probe, node_id);
    size_t i = 0;
    slot->attr_paths[i++] = AttributePathParams(0, kBasicInformationCluster, kBasicVendorName);
    slot->attr_paths[i++] = AttributePathParams(0, kBasicInformationCluster, kBasicVendorId);
    slot->attr_paths[i++] = AttributePathParams(0, kBasicInformationCluster, kBasicProductName);
    slot->attr_paths[i++] = AttributePathParams(0, kBasicInformationCluster, kBasicProductId);
    slot->attr_paths[i++] = AttributePathParams(0, kBasicInformationCluster, kBasicSoftwareVersion);
    slot->attr_paths[i++] = AttributePathParams(chip::kInvalidEndpointId, kDescriptorCluster, kDescriptorDeviceTypeList);
    slot->attr_paths[i++] = AttributePathParams(chip::kInvalidEndpointId, kDescriptorCluster, kDescriptorServerList);
    slot->attr_paths[i++] = AttributePathParams(chip::kInvalidEndpointId, kDescriptorCluster, kDescriptorPartsList);
    slot->attr_path_count = i;
    return QueueRequest(slot, timeout_ms, out_request_id);
}

chip_status_t chip_subscribe_start(uint64_t node_id, chip_path_t path, uint16_t min_interval_s,
                                   uint16_t max_interval_s, chip_subscription_callback_t callback,
                                   void *context, chip_subscription_id_t *out_subscription_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (!chip::IsOperationalNodeId(node_id) || !callback || !out_subscription_id || min_interval_s > max_interval_s) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    SubscriptionSlot *slot = g_runtime.AllocateSubscription();
    if (!slot) {
        return CHIP_STATUS_BUSY;
    }
    slot->node_id = node_id;
    slot->path = path;
    slot->min_interval = min_interval_s;
    slot->max_interval = max_interval_s;
    slot->callback = callback;
    slot->context = context;
    *out_subscription_id = slot->local_id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleSubscriptionStart,
                                                       reinterpret_cast<intptr_t>(slot)) != CHIP_NO_ERROR) {
        slot->Reset();
        return CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_OK;
}

chip_status_t chip_subscribe_stop(chip_subscription_id_t subscription_id)
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    SubscriptionSlot *slot = g_runtime.FindSubscription(subscription_id);
    if (!slot) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    slot->stopping = true;
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(
               ScheduleSubscriptionStop, reinterpret_cast<intptr_t>(slot)) == CHIP_NO_ERROR
               ? CHIP_STATUS_OK : CHIP_STATUS_INTERNAL;
}

chip_status_t chip_commission_onnetwork(const chip_commission_onnetwork_params_t *params,
                                        chip_commission_callback_t callback, void *context,
                                        chip_request_id_t *out_request_id)
{
    chip_status_t preflight = PrepareCommissioning();
    if (preflight != CHIP_STATUS_OK) {
        return preflight;
    }
    if (!params || !callback || !out_request_id || !chip::IsOperationalNodeId(params->node_id) ||
        !params->peer_ip || params->peer_port == 0 || params->setup_pin_code == 0 ||
        !bounded_string(params->peer_ip, sizeof(g_runtime.commission.peer_ip) - 1)) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    auto &state = g_runtime.commission;
    state.Reset();
    state.active = true;
    state.request_id = g_runtime.NextRequestId();
    state.node_id = params->node_id;
    state.callback = callback;
    state.context = context;
    state.mode = 0;
    state.pin = params->setup_pin_code;
    state.port = params->peer_port;
    strcpy(state.peer_ip, params->peer_ip);
    chip_status_t st = state.StartTimer(params->timeout_ms);
    if (st != CHIP_STATUS_OK) {
