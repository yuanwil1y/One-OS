        factory_params.sessionKeystore = &rt.session_keystore;
        factory_params.groupDataProvider = &rt.group_data_provider;
        factory_params.enableServerInteractions = false;
        factory_params.dataModelProvider = &rt.data_model_provider;
        CHIP_ERROR err = factory.Init(factory_params);
        if (err != CHIP_NO_ERROR) {
            return map_chip_error(err);
        }
        rt.factory_initialized = true;
    }

    auto *system_state = factory.GetSystemState();
    if (!system_state || !system_state->Fabrics()) {
        return CHIP_STATUS_INTERNAL;
    }
    chip::FabricTable *fabrics = system_state->Fabrics();
    const uint8_t fabric_count = fabrics->FabricCount();
    if (fabric_count > 1) {
        return CHIP_STATUS_MULTIPLE_FABRICS;
    }

    rt.dac_verifier = chip::Credentials::GetDefaultDACVerifier(&rt.paa_store, nullptr);
    if (!rt.dac_verifier) {
        return CHIP_STATUS_INTERNAL;
    }
    rt.dac_verifier->EnableCdTestKeySupport(false);
    chip::Credentials::SetDeviceAttestationVerifier(rt.dac_verifier);

    chip::Controller::SetupParams setup;
    setup.operationalCredentialsDelegate = &rt.op_creds;
    setup.controllerVendorId = chip::VendorId(rt.config.controller_vendor_id);
    setup.defaultCommissioner = &rt.auto_commissioner;
    setup.deviceAttestationVerifier = rt.dac_verifier;
    setup.enableServerInteractions = false;
    setup.permitMultiControllerFabrics = false;
    setup.removeFromFabricTableOnShutdown = false;
    setup.deleteFromFabricTableOnShutdown = false;

    CHIP_ERROR err = CHIP_NO_ERROR;
    if (fabric_count == 1) {
        const chip::FabricInfo *stored = nullptr;
        for (const auto &fabric : *fabrics) {
            stored = &fabric;
            break;
        }
        if (!stored) {
            return CHIP_STATUS_INTERNAL;
        }
        if ((rt.config.fabric_id != 0 && rt.config.fabric_id != stored->GetFabricId()) ||
            (rt.config.controller_node_id != 0 && rt.config.controller_node_id != stored->GetNodeId())) {
            return CHIP_STATUS_SECURITY_ERROR;
        }
        setup.fabricIndex = chip::MakeOptional(stored->GetFabricIndex());
        err = factory.SetupCommissioner(setup, rt.commissioner);
        if (err != CHIP_NO_ERROR) {
            return map_chip_error(err);
        }
        chip_status_t ipk_status = load_persisted_ipk(rt);
        if (ipk_status != CHIP_STATUS_OK) {
            rt.commissioner.Shutdown();
            return ipk_status;
        }
    } else {
        if (!rt.config.create_fabric_if_missing) {
            return CHIP_STATUS_NO_FABRIC;
        }
        if (!chip::IsOperationalNodeId(rt.config.controller_node_id) || rt.config.fabric_id == 0 ||
            !rt.op_creds.CanGenerateControllerNoc()) {
            return CHIP_STATUS_SECURITY_ERROR;
        }

        uint8_t csr_buf[chip::Crypto::kMIN_CSR_Buffer_Size] = {};
        MutableByteSpan csr(csr_buf);
        err = fabrics->AllocatePendingOperationalKey(chip::NullOptional, csr);
        if (err != CHIP_NO_ERROR) {
            secure_zero(csr_buf, sizeof(csr_buf));
            return map_chip_error(err);
        }

        chip_noc_chain_t chain = {};
        chip_status_t provider_status = rt.op_creds.GenerateControllerNoc(
            rt.config.controller_node_id, rt.config.fabric_id, ByteSpan(csr.data(), csr.size()), chain);
        secure_zero(csr_buf, sizeof(csr_buf));
        if (provider_status != CHIP_STATUS_OK) {
            fabrics->RevertPendingFabricData();
            secure_zero(&chain, sizeof(chain));
            return provider_status;
        }

        setup.operationalKeypair = nullptr;
        setup.controllerRCAC = ByteSpan(chain.rcac, chain.rcac_len);
        setup.controllerICAC = ByteSpan(chain.icac, chain.icac_len);
        setup.controllerNOC = ByteSpan(chain.noc, chain.noc_len);
        err = factory.SetupCommissioner(setup, rt.commissioner);
        if (err != CHIP_NO_ERROR) {
            fabrics->RevertPendingFabricData();
            secure_zero(&chain, sizeof(chain));
            return map_chip_error(err);
        }

        uint8_t compressed[sizeof(uint64_t)] = {};
        MutableByteSpan compressed_span(compressed);
        err = rt.commissioner.GetCompressedFabricIdBytes(compressed_span);
        if (err == CHIP_NO_ERROR) {
            err = chip::Credentials::SetSingleIpkEpochKey(
                &rt.group_data_provider, rt.commissioner.GetFabricIndex(),
                ByteSpan(chain.ipk, sizeof(chain.ipk)), compressed_span);
        }
        secure_zero(compressed, sizeof(compressed));
        if (err != CHIP_NO_ERROR) {
            rt.commissioner.Shutdown();
            secure_zero(&chain, sizeof(chain));
            return map_chip_error(err);
        }
        rt.op_creds.SetIpk(chain.ipk);
        secure_zero(&chain, sizeof(chain));
    }

    rt.commissioner.RegisterPairingDelegate(nullptr);
    if (!rt.event_loop_started) {
        err = factory.ServiceEvents();
        if (err != CHIP_NO_ERROR) {
            rt.commissioner.Shutdown();
            return map_chip_error(err);
        }
        rt.event_loop_started = true;
    }
    return CHIP_STATUS_OK;
}

static chip_status_t RequireReady()
{
    return s_runtime && s_runtime->ready ? CHIP_STATUS_OK : CHIP_STATUS_NOT_READY;
}

static chip_status_t QueueRequest(RequestSlot *slot, uint32_t timeout_ms, chip_request_id_t *out_id)
{
    slot->timeout_ms = timeout_ms;
    chip_status_t st = slot->StartTimer(timeout_ms);
    if (st != CHIP_STATUS_OK) {
        slot->Reset();
        return st;
    }
    *out_id = slot->id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleRequestStart,
                                                       reinterpret_cast<intptr_t>(slot)) != CHIP_NO_ERROR) {
        slot->Reset();
        return CHIP_STATUS_INTERNAL;
    }
    return CHIP_STATUS_OK;
}

static chip_status_t PrepareCommissioning()
{
    if (RequireReady() != CHIP_STATUS_OK) {
        return CHIP_STATUS_NOT_READY;
    }
    if (g_runtime.paa_store.Count() == 0) {
        return CHIP_STATUS_NO_TRUST_ROOTS;
    }
    if (!g_runtime.op_creds.CanCommission()) {
        return CHIP_STATUS_SECURITY_ERROR;
    }
    if (g_runtime.commission.active) {
        return CHIP_STATUS_BUSY;
    }
    return CHIP_STATUS_OK;
}

} // namespace

extern "C" {

chip_status_t chip_controller_init(const chip_controller_config_t *config)
{
    if (!config || config->controller_vendor_id == 0) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    if (g_runtime.ready || g_runtime.factory_initialized || g_runtime.event_loop_started) {
        return CHIP_STATUS_BUSY;
    }

    chip_status_t paa_status = g_runtime.paa_store.Load(config->paa_certs, config->paa_cert_count);
    if (paa_status != CHIP_STATUS_OK) {
        return paa_status;
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        g_runtime.paa_store.Clear();
        return CHIP_STATUS_INTERNAL;
    }

    g_runtime.config = *config;
    g_runtime.config.paa_certs = nullptr;
    g_runtime.config.paa_cert_count = g_runtime.paa_store.Count();
    g_runtime.config.operational_credentials = nullptr;
    g_runtime.op_creds.Configure(config->operational_credentials);
    for (auto &slot : g_runtime.requests) {
        slot.Reset();
    }
    for (auto &slot : g_runtime.subscriptions) {
        slot.Reset();
    }
    g_runtime.commission.Reset();
    s_runtime = &g_runtime;

    chip_status_t status = InitializeController(g_runtime);
    if (status != CHIP_STATUS_OK) {
        g_runtime.ready = false;
        g_runtime.op_creds.Clear();
        return status;
    }
    g_runtime.ready = true;
    return CHIP_STATUS_OK;
}

chip_status_t chip_controller_shutdown(void)
{
    if (!s_runtime || (!g_runtime.ready && !g_runtime.factory_initialized)) {
        return CHIP_STATUS_OK;
    }
    if (g_runtime.Busy()) {
        return CHIP_STATUS_BUSY;
    }

    g_runtime.ready = false;
    if (g_runtime.event_loop_started) {
        TEMPORARY_RETURN_IGNORED chip::DeviceLayer::PlatformMgr().StopEventLoopTask();
        g_runtime.event_loop_started = false;
    }
    g_runtime.commissioner.Shutdown();
    chip::Controller::DeviceControllerFactory::GetInstance().Shutdown();
    g_runtime.factory_initialized = false;
    g_runtime.group_data_provider.Finish();
    g_runtime.op_cert_store.Finish();
    g_runtime.operational_keystore.Finish();
    g_runtime.op_creds.Clear();
    g_runtime.paa_store.Clear();
    if (g_runtime.memory_initialized) {
        chip::Platform::MemoryShutdown();
        g_runtime.memory_initialized = false;
    }
    g_runtime.config = {};
    s_runtime = nullptr;
    return CHIP_STATUS_OK;
}

bool chip_controller_is_ready(void)
{
    return s_runtime && s_runtime->ready;
}

chip_status_t chip_controller_get_fabric(chip_fabric_info_t *out_info)
{
    if (!out_info) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
