                    probe.partial = true;
                    return;
                }
                uint32_t device_type = 0;
                uint16_t revision = 0;
                bool have_type = false;
                bool have_revision = false;
                CHIP_ERROR err;
                while ((err = item.Next()) == CHIP_NO_ERROR) {
                    chip::TLV::Tag tag = item.GetTag();
                    if (!chip::TLV::IsContextTag(tag)) {
                        continue;
                    }
                    uint32_t tag_num = chip::TLV::TagNumFromTag(tag);
                    uint64_t value = 0;
                    if (item.Get(value) != CHIP_NO_ERROR) {
                        continue;
                    }
                    if (tag_num == 0) {
                        device_type = static_cast<uint32_t>(value);
                        have_type = true;
                    } else if (tag_num == 1) {
                        revision = static_cast<uint16_t>(value);
                        have_revision = true;
                    }
                }
                if (err != CHIP_END_OF_TLV || item.ExitContainer(outer) != CHIP_NO_ERROR) {
                    probe.partial = true;
                }
                if (have_type && have_revision) {
                    matter_bounds_add_device_type(&probe, endpoint, device_type, revision);
                } else {
                    probe.partial = true;
                }
            });
        }
    }

    void DestroyNative()
    {
        if (!native_live) {
            return;
        }
        if (native_kind == RequestKind::Read || native_kind == RequestKind::Probe) {
            reinterpret_cast<ReadClient *>(&native)->~ReadClient();
        } else if (native_kind == RequestKind::Write) {
            reinterpret_cast<chip::app::WriteClient *>(&native)->~WriteClient();
        } else if (native_kind == RequestKind::Invoke) {
            reinterpret_cast<chip::app::CommandSender *>(&native)->~CommandSender();
        }
        native_live = false;
        native_kind = RequestKind::None;
        secure_zero(tlv_payload, sizeof(tlv_payload));
    }

    static void TimerCallback(void *arg);
    static void OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session);
    static void OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR);

    bool in_use = false;
    bool user_done = false;
    bool case_pending = false;
    bool native_live = false;
    RequestKind native_kind = RequestKind::None;
    RequestKind kind = RequestKind::None;
    chip_request_id_t id = 0;
    uint64_t node_id = 0;
    chip_path_t path = {};
    uint16_t timed_timeout_ms = 0;
    uint32_t timeout_ms = 0;
    chip_read_callback_t read_cb = nullptr;
    chip_operation_callback_t op_cb = nullptr;
    matter_node_probe_callback_t probe_cb = nullptr;
    void *context = nullptr;
    char json[CHIP_L2_MAX_PAYLOAD_JSON + 1] = {};
    uint8_t tlv_payload[kTlvPayloadBytes] = {};
    matter_node_info_t probe = {};
    chip_status_t probe_status = CHIP_STATUS_OK;
    bool probe_saw_data = false;
    AttributePathParams attr_paths[kMaxProbePaths];
    size_t attr_path_count = 0;
    esp_timer_handle_t timer = nullptr;
    NativeStorage native;
    chip::Callback::Callback<chip::OnDeviceConnected> connected_cb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> connect_failed_cb;
};

class SubscriptionSlot final : public ReadClient::Callback {
public:
    using NativeStorage = typename std::aligned_storage<sizeof(ReadClient), alignof(ReadClient)>::type;

    SubscriptionSlot() : connected_cb(OnConnected, this), connect_failed_cb(OnConnectFailed, this) {}

    void Reset()
    {
        if (native_live) {
            reinterpret_cast<ReadClient *>(&native)->~ReadClient();
            native_live = false;
        }
        in_use = false;
        stopping = false;
        established = false;
        case_pending = false;
        terminal_reported = false;
        local_id = 0;
        remote_id = 0;
        node_id = 0;
        path = {};
        callback = nullptr;
        context = nullptr;
    }

    void StartCase();

    void ReportTerminal(chip_status_t status)
    {
        if (!terminal_reported && callback) {
            terminal_reported = true;
            callback(local_id, status, path, nullptr, context);
        }
    }

    void OnAttributeData(const ConcreteDataAttributePath &aPath, chip::TLV::TLVReader *data,
                         const StatusIB &status) override
    {
        if (!callback || stopping) {
            return;
        }
        if (!status.IsSuccess()) {
            callback(local_id, CHIP_STATUS_IM_ERROR, to_public_path(aPath), nullptr, context);
            return;
        }
        chip_value_t value = {};
        chip_status_t st = decode_value(data, &value);
        callback(local_id, st, to_public_path(aPath), &value, context);
        secure_zero(&value, sizeof(value));
    }

    void OnEventData(const chip::app::EventHeader &, chip::TLV::TLVReader *, const StatusIB *) override {}
    void OnDeallocatePaths(chip::app::ReadPrepareParams &&) override {}

    void OnError(CHIP_ERROR error) override
    {
        if (!stopping) {
            ReportTerminal(map_chip_error(error));
        }
    }

    void OnDone(ReadClient *) override
    {
        if (!stopping) {
            ReportTerminal(CHIP_STATUS_IM_ERROR);
        }
        if (native_live) {
            reinterpret_cast<ReadClient *>(&native)->~ReadClient();
            native_live = false;
        }
        Reset();
    }

    void OnSubscriptionEstablished(chip::SubscriptionId id) override
    {
        remote_id = id;
        established = true;
        if (callback && !stopping) {
            callback(local_id, CHIP_STATUS_OK, path, nullptr, context);
        }
    }

    CHIP_ERROR OnResubscriptionNeeded(ReadClient *, CHIP_ERROR termination_cause) override
    {
        return termination_cause;
    }

    static void OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session);
    static void OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR);

    bool in_use = false;
    bool stopping = false;
    bool established = false;
    bool case_pending = false;
    bool terminal_reported = false;
    bool native_live = false;
    chip_subscription_id_t local_id = 0;
    chip::SubscriptionId remote_id = 0;
    uint64_t node_id = 0;
    chip_path_t path = {};
    uint16_t min_interval = 0;
    uint16_t max_interval = 0;
    chip_subscription_callback_t callback = nullptr;
    void *context = nullptr;
    AttributePathParams attr_path;
    NativeStorage native;
    chip::Callback::Callback<chip::OnDeviceConnected> connected_cb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> connect_failed_cb;
};

class CommissionState final : public chip::Controller::DevicePairingDelegate {
public:
    void WipeSecrets()
    {
        secure_zero(password, sizeof(password));
        secure_zero(dataset, sizeof(dataset));
        secure_zero(&pin, sizeof(pin));
        dataset_len = 0;
    }

    void Reset()
    {
        StopTimer();
        WipeSecrets();
        active = false;
        user_done = false;
        cancel_requested = false;
        request_id = 0;
        node_id = 0;
        callback = nullptr;
        context = nullptr;
        mode = 0;
        discriminator = 0;
        port = 0;
        peer_ip[0] = '\0';
        ssid[0] = '\0';
    }

    chip_status_t StartTimer(uint32_t timeout_ms)
    {
        if (timeout_ms == 0) {
            return CHIP_STATUS_OK;
        }
        if (!timer) {
            esp_timer_create_args_t args = {};
            args.callback = TimerCallback;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "matter_pair";
            if (esp_timer_create(&args, &timer) != ESP_OK) {
                return CHIP_STATUS_INTERNAL;
            }
        }
        esp_timer_stop(timer);
        return esp_timer_start_once(timer, static_cast<uint64_t>(timeout_ms) * 1000ULL) == ESP_OK
                   ? CHIP_STATUS_OK : CHIP_STATUS_INTERNAL;
    }

    void StopTimer()
    {
        if (timer) {
            esp_timer_stop(timer);
        }
    }

    void Complete(chip_status_t status, uint32_t stage)
    {
        if (user_done) {
            return;
        }
        user_done = true;
        StopTimer();
        if (callback) {
            callback(request_id, status, node_id, stage, context);
        }
    }

    void Finish();

    void OnPairingComplete(CHIP_ERROR error) override
    {
        if (error != CHIP_NO_ERROR && !cancel_requested) {
            Complete(map_chip_error(error), 0);
            Finish();
        }
    }

    void OnCommissioningSuccess(chip::PeerId) override
    {
        if (!cancel_requested) {
