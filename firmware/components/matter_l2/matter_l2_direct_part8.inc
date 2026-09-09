void RequestSlot::OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session)
{
    auto *self = static_cast<RequestSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    self->case_pending = false;
    if (self->user_done) {
        self->Reset();
        return;
    }
    self->SendInteraction(exchange_mgr, session);
}

void RequestSlot::OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR)
{
    auto *self = static_cast<RequestSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    self->case_pending = false;
    if (!self->user_done) {
        self->Complete(CHIP_STATUS_CASE_FAILED);
    }
    self->Reset();
}

void RequestSlot::StartCase()
{
    if (!s_runtime || !s_runtime->ready || !in_use) {
        Complete(CHIP_STATUS_NOT_READY);
        Reset();
        return;
    }
    if (user_done) {
        Reset();
        return;
    }
    case_pending = true;
    CHIP_ERROR err = s_runtime->commissioner.GetConnectedDevice(node_id, &connected_cb, &connect_failed_cb);
    if (err != CHIP_NO_ERROR) {
        case_pending = false;
        Complete(CHIP_STATUS_CASE_FAILED);
        Reset();
    }
}

void RequestSlot::SendInteraction(ExchangeManager &exchange_mgr, const SessionHandle &session)
{
    if (!s_runtime || user_done) {
        Reset();
        return;
    }

    CHIP_ERROR err = CHIP_NO_ERROR;
    if (kind == RequestKind::Read || kind == RequestKind::Probe) {
        auto *client = new (&native) ReadClient(InteractionModelEngine::GetInstance(), &exchange_mgr, *this,
                                                ReadClient::InteractionType::Read);
        native_live = true;
        native_kind = kind;
        chip::app::ReadPrepareParams params(session);
        params.mpAttributePathParamsList = attr_paths;
        params.mAttributePathParamsListSize = attr_path_count;
        err = client->SendRequest(params);
    } else if (kind == RequestKind::Write) {
        chip::TLV::TLVWriter writer;
        writer.Init(tlv_payload, sizeof(tlv_payload));
        BoundedJsonTlvEncoder encoder(json);
        err = encoder.EncodeValue(writer, chip::TLV::AnonymousTag());
        if (err == CHIP_NO_ERROR) {
            err = writer.Finalize();
        }

        chip::TLV::TLVReader reader;
        if (err == CHIP_NO_ERROR) {
            reader.Init(tlv_payload, writer.GetLengthWritten());
            err = reader.Next();
        }

        chip::Optional<uint16_t> timed = timed_timeout_ms ? chip::MakeOptional(timed_timeout_ms) : chip::NullOptional;
        auto *client = new (&native) chip::app::WriteClient(&exchange_mgr, this, timed);
        native_live = true;
        native_kind = kind;
        if (err == CHIP_NO_ERROR) {
            ConcreteDataAttributePath concrete(path.endpoint_id, path.cluster_id, path.item_id);
            err = client->PutPreencodedAttribute(concrete, reader);
        }
        if (err == CHIP_NO_ERROR) {
            err = client->SendWriteRequest(session);
        }
    } else if (kind == RequestKind::Invoke) {
        const bool is_timed = timed_timeout_ms != 0;
        auto *sender = new (&native) chip::app::CommandSender(this, &exchange_mgr, is_timed);
        native_live = true;
        native_kind = kind;

        chip::app::CommandPathParams command_path = {
            path.endpoint_id, 0, path.cluster_id, path.item_id,
            chip::app::CommandPathFlags::kEndpointIdValid
        };
        chip::app::CommandSender::PrepareCommandParameters prepare;
        prepare.SetStartDataStruct(true);
        err = sender->PrepareCommand(command_path, prepare);
        if (err == CHIP_NO_ERROR) {
            auto *writer = sender->GetCommandDataIBTLVWriter();
            if (writer == nullptr) {
                err = CHIP_ERROR_INCORRECT_STATE;
            } else {
                BoundedJsonTlvEncoder encoder(json[0] ? json : "{}");
                err = encoder.EncodeCommandFields(*writer);
            }
        }
        if (err == CHIP_NO_ERROR) {
            chip::Optional<uint16_t> timed =
                timed_timeout_ms ? chip::MakeOptional(timed_timeout_ms) : chip::NullOptional;
            chip::app::CommandSender::FinishCommandParameters finish(timed);
            finish.SetEndDataStruct(true);
            err = sender->FinishCommand(finish);
        }
        if (err == CHIP_NO_ERROR) {
            err = sender->SendCommandRequest(session);
        }
    }

    if (err != CHIP_NO_ERROR) {
        Complete(map_chip_error(err));
        DestroyNative();
        Reset();
    }
}

void SubscriptionSlot::StartCase()
{
    if (!s_runtime || !s_runtime->ready || !in_use) {
        ReportTerminal(CHIP_STATUS_NOT_READY);
        Reset();
        return;
    }
    case_pending = true;
    CHIP_ERROR err = s_runtime->commissioner.GetConnectedDevice(node_id, &connected_cb, &connect_failed_cb);
    if (err != CHIP_NO_ERROR) {
        case_pending = false;
        ReportTerminal(CHIP_STATUS_CASE_FAILED);
        Reset();
    }
}

void SubscriptionSlot::OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session)
{
    auto *self = static_cast<SubscriptionSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    self->case_pending = false;
    if (self->stopping) {
        self->Reset();
        return;
    }

    self->attr_path = AttributePathParams(self->path.endpoint_id, self->path.cluster_id, self->path.item_id);
    auto *client = new (&self->native) ReadClient(InteractionModelEngine::GetInstance(), &exchange_mgr, *self,
                                                  ReadClient::InteractionType::Subscribe);
    self->native_live = true;
    chip::app::ReadPrepareParams params(session);
    params.mpAttributePathParamsList = &self->attr_path;
    params.mAttributePathParamsListSize = 1;
    params.mMinIntervalFloorSeconds = self->min_interval;
    params.mMaxIntervalCeilingSeconds = self->max_interval;
    params.mKeepSubscriptions = false;
    CHIP_ERROR err = client->SendRequest(params);
    if (err != CHIP_NO_ERROR) {
        self->ReportTerminal(map_chip_error(err));
        self->Reset();
    }
}

void SubscriptionSlot::OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR)
{
    auto *self = static_cast<SubscriptionSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    self->case_pending = false;
    if (!self->stopping) {
        self->ReportTerminal(CHIP_STATUS_CASE_FAILED);
    }
    self->Reset();
}

void CommissionState::Finish()
{
    if (s_runtime) {
        s_runtime->commissioner.RegisterPairingDelegate(nullptr);
    }
    active = false;
    StopTimer();
    WipeSecrets();
    peer_ip[0] = '\0';
    ssid[0] = '\0';
}

void CommissionState::TimerCallback(void *arg)
{
    auto *self = static_cast<CommissionState *>(arg);
    if (!self || !self->active) {
        return;
    }
    TEMPORARY_RETURN_IGNORED chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleCommissionTimeout, 0);
}

static chip_status_t load_persisted_ipk(Runtime &rt)
{
    chip::Credentials::GroupDataProvider::KeySet keyset;
    CHIP_ERROR err = rt.group_data_provider.GetKeySet(
        rt.commissioner.GetFabricIndex(), chip::Credentials::GroupDataProvider::kIdentityProtectionKeySetId, keyset);
    if (err != CHIP_NO_ERROR || keyset.num_keys_used == 0) {
        secure_zero(&keyset, sizeof(keyset));
        return CHIP_STATUS_SECURITY_ERROR;
    }
    rt.op_creds.SetIpk(keyset.epoch_keys[0].key);
    secure_zero(&keyset, sizeof(keyset));
    return CHIP_STATUS_OK;
}

static chip_status_t InitializeController(Runtime &rt)
{
    esp_err_t event_err = esp_event_loop_create_default();
    if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
        return CHIP_STATUS_INTERNAL;
    }

    if (!rt.memory_initialized) {
        CHIP_ERROR mem_err = chip::Platform::MemoryInit();
        if (mem_err != CHIP_NO_ERROR) {
            return map_chip_error(mem_err);
        }
        rt.memory_initialized = true;
    }

    auto &factory = chip::Controller::DeviceControllerFactory::GetInstance();
    if (!rt.factory_initialized) {
        if (rt.operational_keystore.Init(&rt.storage) != CHIP_NO_ERROR ||
            rt.op_cert_store.Init(&rt.storage) != CHIP_NO_ERROR) {
            return CHIP_STATUS_INTERNAL;
        }
        rt.group_data_provider.SetStorageDelegate(&rt.storage);
        rt.group_data_provider.SetSessionKeystore(&rt.session_keystore);
        if (rt.group_data_provider.Init() != CHIP_NO_ERROR) {
            return CHIP_STATUS_INTERNAL;
        }
        chip::Credentials::SetGroupDataProvider(&rt.group_data_provider);

        chip::Controller::FactoryInitParams factory_params;
        factory_params.listenPort = rt.config.listen_port;
        factory_params.fabricIndependentStorage = &rt.storage;
        factory_params.operationalKeystore = &rt.operational_keystore;
        factory_params.opCertStore = &rt.op_cert_store;
