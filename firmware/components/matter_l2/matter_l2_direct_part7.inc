            Complete(CHIP_STATUS_OK, 0);
        }
        Finish();
    }

    void OnCommissioningFailure(chip::PeerId, CHIP_ERROR error, chip::Controller::CommissioningStage stage,
                                chip::Optional<chip::Credentials::AttestationVerificationResult>) override
    {
        if (!cancel_requested) {
            chip_status_t st = map_chip_error(error);
            if (st == CHIP_STATUS_IM_ERROR) {
                st = CHIP_STATUS_SECURITY_ERROR;
            }
            Complete(st, static_cast<uint32_t>(stage));
        }
        Finish();
    }

    void OnICDRegistrationComplete(chip::ScopedNodeId, uint32_t) override {}
    void OnICDStayActiveComplete(chip::ScopedNodeId, uint32_t) override {}

    static void TimerCallback(void *arg);

    bool active = false;
    bool user_done = false;
    bool cancel_requested = false;
    chip_request_id_t request_id = 0;
    uint64_t node_id = 0;
    chip_commission_callback_t callback = nullptr;
    void *context = nullptr;
    uint8_t mode = 0;
    uint32_t pin = 0;
    uint16_t discriminator = 0;
    uint16_t port = 0;
    char peer_ip[48] = {};
    char ssid[CHIP_L2_MAX_WIFI_SSID_BYTES + 1] = {};
    char password[CHIP_L2_MAX_WIFI_PASSWORD_BYTES + 1] = {};
    uint8_t dataset[CHIP_L2_MAX_THREAD_DATASET_BYTES] = {};
    uint8_t dataset_len = 0;
    esp_timer_handle_t timer = nullptr;
};

struct Runtime {
    bool ready = false;
    bool memory_initialized = false;
    bool factory_initialized = false;
    bool event_loop_started = false;
    chip_request_id_t next_request_id = 1;
    chip_subscription_id_t next_subscription_id = 1;
    chip_controller_config_t config = {};

    ControllerStorageDelegate storage;
    chip::PersistentStorageOperationalKeystore operational_keystore;
    chip::Credentials::PersistentStorageOpCertStore op_cert_store;
    chip::Crypto::DefaultSessionKeystore session_keystore;
    chip::Credentials::GroupDataProviderImpl group_data_provider{1, 1};
    OperationalCredentialsAdapter op_creds;
    FixedPaaStore paa_store;
    ControllerOnlyDataModelProvider data_model_provider;
    chip::Controller::AutoCommissioner auto_commissioner;
    chip::Controller::DeviceCommissioner commissioner;
    chip::Credentials::DeviceAttestationVerifier *dac_verifier = nullptr;

    RequestSlot requests[kMaxInflightRequests];
    SubscriptionSlot subscriptions[CHIP_L2_MAX_SUBSCRIPTIONS];
    CommissionState commission;

    chip_request_id_t NextRequestId()
    {
        chip_request_id_t id = next_request_id++;
        if (next_request_id == 0) {
            next_request_id = 1;
        }
        return id;
    }

    RequestSlot *AllocateRequest(RequestKind kind)
    {
        for (auto &slot : requests) {
            if (!slot.in_use) {
                slot.Reset();
                slot.in_use = true;
                slot.kind = kind;
                slot.id = NextRequestId();
                return &slot;
            }
        }
        return nullptr;
    }

    RequestSlot *FindRequest(chip_request_id_t id)
    {
        for (auto &slot : requests) {
            if (slot.in_use && slot.id == id) {
                return &slot;
            }
        }
        return nullptr;
    }

    SubscriptionSlot *AllocateSubscription()
    {
        for (auto &slot : subscriptions) {
            if (!slot.in_use) {
                slot.Reset();
                slot.in_use = true;
                slot.local_id = next_subscription_id++;
                if (next_subscription_id == 0) {
                    next_subscription_id = 1;
                }
                return &slot;
            }
        }
        return nullptr;
    }

    SubscriptionSlot *FindSubscription(chip_subscription_id_t id)
    {
        for (auto &slot : subscriptions) {
            if (slot.in_use && slot.local_id == id) {
                return &slot;
            }
        }
        return nullptr;
    }

    bool Busy() const
    {
        for (const auto &slot : requests) {
            if (slot.in_use) {
                return true;
            }
        }
        for (const auto &slot : subscriptions) {
            if (slot.in_use) {
                return true;
            }
        }
        return commission.active;
    }
};

static Runtime g_runtime;

static void ScheduleRequestStart(intptr_t arg)
{
    auto *slot = reinterpret_cast<RequestSlot *>(arg);
    if (slot && slot->in_use) {
        slot->StartCase();
    }
}

static void ScheduleRequestTimeout(intptr_t arg)
{
    auto *slot = reinterpret_cast<RequestSlot *>(arg);
    if (slot && slot->in_use) {
        slot->CancelOrTimeout(CHIP_STATUS_TIMEOUT);
    }
}

static void ScheduleRequestCancel(intptr_t arg)
{
    auto *slot = reinterpret_cast<RequestSlot *>(arg);
    if (slot && slot->in_use) {
        slot->CancelOrTimeout(CHIP_STATUS_CANCELLED);
    }
}

static void ScheduleSubscriptionStart(intptr_t arg)
{
    auto *slot = reinterpret_cast<SubscriptionSlot *>(arg);
    if (slot && slot->in_use) {
        slot->StartCase();
    }
}

static void ScheduleSubscriptionStop(intptr_t arg)
{
    auto *slot = reinterpret_cast<SubscriptionSlot *>(arg);
    if (!slot || !slot->in_use || !s_runtime) {
        return;
    }
    slot->stopping = true;
    slot->ReportTerminal(CHIP_STATUS_CANCELLED);
    if (slot->established) {
        CHIP_ERROR err = InteractionModelEngine::GetInstance()->ShutdownSubscription(
            ScopedNodeId(slot->node_id, s_runtime->commissioner.GetFabricIndex()), slot->remote_id);
        if (err != CHIP_NO_ERROR) {
            slot->Reset();
        }
        return;
    }
    if (!slot->case_pending) {
        slot->Reset();
    }
}

static void ScheduleCommissionCancel(intptr_t)
{
    if (!s_runtime || !s_runtime->commission.active) {
        return;
    }
    auto &state = s_runtime->commission;
    if (!state.user_done) {
        state.cancel_requested = true;
        state.Complete(CHIP_STATUS_CANCELLED, 0);
    }
    TEMPORARY_RETURN_IGNORED s_runtime->commissioner.StopPairing(state.node_id);
    state.Finish();
}

static void ScheduleCommissionTimeout(intptr_t)
{
    if (!s_runtime || !s_runtime->commission.active || s_runtime->commission.user_done) {
        return;
    }
    s_runtime->commission.Complete(CHIP_STATUS_TIMEOUT, 0);
    s_runtime->commission.cancel_requested = true;
    TEMPORARY_RETURN_IGNORED s_runtime->commissioner.StopPairing(s_runtime->commission.node_id);
    s_runtime->commission.Finish();
}

static void ScheduleCommissionStart(intptr_t)
{
    if (!s_runtime || !s_runtime->ready || !s_runtime->commission.active || s_runtime->commission.user_done) {
        return;
    }
    auto &state = s_runtime->commission;
    auto &commissioner = s_runtime->commissioner;
    commissioner.RegisterPairingDelegate(&state);

    chip::RendezvousParameters rendezvous;
    rendezvous.SetSetupPINCode(state.pin);
    chip::Controller::CommissioningParameters commissioning_params;

    if (state.mode == 0) {
        chip::Inet::IPAddress address;
        if (!chip::Inet::IPAddress::FromString(state.peer_ip, address)) {
            state.Complete(CHIP_STATUS_INVALID_ARGUMENT, 0);
            state.Finish();
            return;
        }
        rendezvous.SetPeerAddress(chip::Transport::PeerAddress::UDP(
            address, state.port, chip::Inet::InterfaceId::Null()));
    } else {
        CHIP_ERROR ble_err = chip::DeviceLayer::Internal::BLEMgr().Init();
        if (ble_err != CHIP_NO_ERROR && ble_err != CHIP_ERROR_INCORRECT_STATE) {
            state.Complete(CHIP_STATUS_INTERNAL, 0);
            state.Finish();
            return;
        }
        ble_err = chip::DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(0, true);
        if (ble_err != CHIP_NO_ERROR) {
            state.Complete(CHIP_STATUS_INTERNAL, 0);
            state.Finish();
            return;
        }
        rendezvous.SetDiscriminator(state.discriminator).SetPeerAddress(chip::Transport::PeerAddress::BLE());
        if (state.mode == 1) {
            ByteSpan ssid(reinterpret_cast<const uint8_t *>(state.ssid), strlen(state.ssid));
            ByteSpan password(reinterpret_cast<const uint8_t *>(state.password), strlen(state.password));
            commissioning_params.SetWiFiCredentials(chip::Controller::WiFiCredentials(ssid, password));
        } else {
            commissioning_params.SetThreadOperationalDataset(ByteSpan(state.dataset, state.dataset_len));
        }
    }

    CHIP_ERROR err = commissioner.PairDevice(state.node_id, rendezvous, commissioning_params);
    if (err != CHIP_NO_ERROR) {
        state.Complete(map_chip_error(err), 0);
        state.Finish();
    }
}

void RequestSlot::TimerCallback(void *arg)
{
    auto *self = static_cast<RequestSlot *>(arg);
    if (!self || !self->in_use) {
        return;
    }
    TEMPORARY_RETURN_IGNORED chip::DeviceLayer::PlatformMgr().ScheduleWork(
        ScheduleRequestTimeout, reinterpret_cast<intptr_t>(self));
}

