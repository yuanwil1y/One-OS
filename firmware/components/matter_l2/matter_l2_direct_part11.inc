        state.Reset();
        return st;
    }
    *out_request_id = state.request_id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleCommissionStart, 0) != CHIP_NO_ERROR) {
        state.Reset();
        return CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_OK;
}

chip_status_t chip_commission_ble_wifi(const chip_commission_ble_wifi_params_t *params,
                                       chip_commission_callback_t callback, void *context,
                                       chip_request_id_t *out_request_id)
{
    chip_status_t preflight = PrepareCommissioning();
    if (preflight != CHIP_STATUS_OK) {
        return preflight;
    }
    if (!params || !callback || !out_request_id || !chip::IsOperationalNodeId(params->node_id) ||
        params->setup_pin_code == 0 || !bounded_string(params->ssid, CHIP_L2_MAX_WIFI_SSID_BYTES) ||
        !bounded_string(params->password, CHIP_L2_MAX_WIFI_PASSWORD_BYTES) || strlen(params->ssid) == 0) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    auto &state = g_runtime.commission;
    state.Reset();
    state.active = true;
    state.request_id = g_runtime.NextRequestId();
    state.node_id = params->node_id;
    state.callback = callback;
    state.context = context;
    state.mode = 1;
    state.pin = params->setup_pin_code;
    state.discriminator = params->discriminator;
    strcpy(state.ssid, params->ssid);
    strcpy(state.password, params->password);
    chip_status_t st = state.StartTimer(params->timeout_ms);
    if (st != CHIP_STATUS_OK) {
        state.Reset();
        return st;
    }
    *out_request_id = state.request_id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleCommissionStart, 0) != CHIP_NO_ERROR) {
        state.Reset();
        return CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_OK;
}

chip_status_t chip_commission_ble_thread(const chip_commission_ble_thread_params_t *params,
                                         chip_commission_callback_t callback, void *context,
                                         chip_request_id_t *out_request_id)
{
    chip_status_t preflight = PrepareCommissioning();
    if (preflight != CHIP_STATUS_OK) {
        return preflight;
    }
    if (!params || !callback || !out_request_id || !chip::IsOperationalNodeId(params->node_id) ||
        params->setup_pin_code == 0 || !params->thread_dataset || params->thread_dataset_len == 0 ||
        params->thread_dataset_len > CHIP_L2_MAX_THREAD_DATASET_BYTES) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    auto &state = g_runtime.commission;
    state.Reset();
    state.active = true;
    state.request_id = g_runtime.NextRequestId();
    state.node_id = params->node_id;
    state.callback = callback;
    state.context = context;
    state.mode = 2;
    state.pin = params->setup_pin_code;
    state.discriminator = params->discriminator;
    state.dataset_len = params->thread_dataset_len;
    memcpy(state.dataset, params->thread_dataset, params->thread_dataset_len);
    chip_status_t st = state.StartTimer(params->timeout_ms);
    if (st != CHIP_STATUS_OK) {
        state.Reset();
        return st;
    }
    *out_request_id = state.request_id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleCommissionStart, 0) != CHIP_NO_ERROR) {
        state.Reset();
        return CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_OK;
}

} // extern "C"
