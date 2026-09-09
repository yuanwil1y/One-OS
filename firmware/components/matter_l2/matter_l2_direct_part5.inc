        if (requested_timeout_ms == 0) {
            return CHIP_STATUS_OK;
        }
        if (!timer) {
            esp_timer_create_args_t args = {};
            args.callback = TimerCallback;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "matter_req";
            if (esp_timer_create(&args, &timer) != ESP_OK) {
                return CHIP_STATUS_INTERNAL;
            }
        }
        esp_timer_stop(timer);
        return esp_timer_start_once(timer, static_cast<uint64_t>(requested_timeout_ms) * 1000ULL) == ESP_OK
                   ? CHIP_STATUS_OK : CHIP_STATUS_INTERNAL;
    }

    void StopTimer()
    {
        if (timer) {
            esp_timer_stop(timer);
        }
    }

    void Complete(chip_status_t status)
    {
        if (user_done) {
            return;
        }
        user_done = true;
        StopTimer();
        if (kind == RequestKind::Read && read_cb) {
            read_cb(id, status, path, nullptr, context);
        } else if (kind == RequestKind::Probe && probe_cb) {
            probe_cb(id, status, &probe, context);
        } else if ((kind == RequestKind::Write || kind == RequestKind::Invoke) && op_cb) {
            op_cb(id, status, context);
        }
    }

    void CancelOrTimeout(chip_status_t status)
    {
        if (!in_use || user_done) {
            return;
        }
        Complete(status);
        if (case_pending) {
            return;
        }
        DestroyNative();
        Reset();
    }

    void StartCase();
    void SendInteraction(ExchangeManager &exchange_mgr, const SessionHandle &session);

    void OnAttributeData(const ConcreteDataAttributePath &aPath, chip::TLV::TLVReader *data,
                         const StatusIB &status) override
    {
        if (user_done) {
            return;
        }
        if (!status.IsSuccess()) {
            if (kind == RequestKind::Probe) {
                probe.partial = true;
                probe_status = CHIP_STATUS_IM_ERROR;
            } else {
                Complete(CHIP_STATUS_IM_ERROR);
            }
            return;
        }
        if (kind == RequestKind::Read) {
            chip_value_t value = {};
            chip_status_t st = decode_value(data, &value);
            if (!user_done && read_cb) {
                user_done = true;
                StopTimer();
                read_cb(id, st, to_public_path(aPath), &value, context);
            }
            secure_zero(&value, sizeof(value));
        } else if (kind == RequestKind::Probe) {
            probe_saw_data = true;
            ProcessProbeAttribute(aPath, data);
        }
    }

    void OnEventData(const chip::app::EventHeader &, chip::TLV::TLVReader *, const StatusIB *) override {}
    void OnDeallocatePaths(chip::app::ReadPrepareParams &&) override {}

    void OnError(CHIP_ERROR error) override
    {
        if (user_done) {
            return;
        }
        if (kind == RequestKind::Probe) {
            probe.partial = true;
            probe_status = map_chip_error(error);
        } else {
            Complete(map_chip_error(error));
        }
    }

    void OnDone(ReadClient *) override
    {
        if (kind == RequestKind::Probe && !user_done) {
            chip_status_t st = probe_status;
            if (st == CHIP_STATUS_OK && !probe_saw_data) {
                st = CHIP_STATUS_IM_ERROR;
            }
            if (probe.truncated && st == CHIP_STATUS_OK) {
                st = CHIP_STATUS_TRUNCATED;
            }
            user_done = true;
            StopTimer();
            if (probe_cb) {
                probe_cb(id, st, &probe, context);
            }
        } else if (kind == RequestKind::Read && !user_done) {
            Complete(CHIP_STATUS_IM_ERROR);
        }
        DestroyNative();
        Reset();
    }

    void OnResponse(const chip::app::WriteClient *, const ConcreteDataAttributePath &, StatusIB status) override
    {
        if (!user_done) {
            Complete(status.IsSuccess() ? CHIP_STATUS_OK : CHIP_STATUS_IM_ERROR);
        }
    }

    void OnError(const chip::app::WriteClient *, CHIP_ERROR error) override
    {
        if (!user_done) {
            Complete(map_chip_error(error));
        }
    }

    void OnDone(chip::app::WriteClient *) override
    {
        if (!user_done) {
            Complete(CHIP_STATUS_IM_ERROR);
        }
        DestroyNative();
        Reset();
    }

    void OnResponse(chip::app::CommandSender *,
                    const chip::app::CommandSender::ResponseData &response) override
    {
        if (!user_done) {
            Complete(response.statusIB.IsSuccess() ? CHIP_STATUS_OK : CHIP_STATUS_IM_ERROR);
        }
    }

    void OnError(const chip::app::CommandSender *,
                 const chip::app::CommandSender::ErrorData &error_data) override
    {
        if (!user_done) {
            Complete(map_chip_error(error_data.error));
        }
    }

    void OnDone(chip::app::CommandSender *) override
    {
        if (!user_done) {
            Complete(CHIP_STATUS_IM_ERROR);
        }
        DestroyNative();
        Reset();
    }

    template <typename F>
    void ForEachListItem(const ConcreteDataAttributePath &p, chip::TLV::TLVReader *data, F fn)
    {
        if (p.mListOp == ConcreteDataAttributePath::ListOperation::ReplaceAll) {
            if (data->GetType() != chip::TLV::kTLVType_Array) {
                probe.partial = true;
                return;
            }
            chip::TLV::TLVType outer;
            if (data->EnterContainer(outer) != CHIP_NO_ERROR) {
                probe.partial = true;
                return;
            }
            CHIP_ERROR err;
            while ((err = data->Next()) == CHIP_NO_ERROR) {
                fn(*data);
            }
            if (err != CHIP_END_OF_TLV || data->ExitContainer(outer) != CHIP_NO_ERROR) {
                probe.partial = true;
            }
        } else {
            fn(*data);
        }
    }

    void ProcessProbeAttribute(const ConcreteDataAttributePath &aPath, chip::TLV::TLVReader *data)
    {
        if (!data) {
            probe.partial = true;
            return;
        }
        if (aPath.mClusterId == kBasicInformationCluster && aPath.mEndpointId == 0) {
            if (aPath.mAttributeId == kBasicVendorName || aPath.mAttributeId == kBasicProductName) {
                chip::CharSpan span;
                if (data->Get(span) != CHIP_NO_ERROR) {
                    probe.partial = true;
                    return;
                }
                char *dst = aPath.mAttributeId == kBasicVendorName ? probe.vendor_name : probe.product_name;
                size_t cap = aPath.mAttributeId == kBasicVendorName ? sizeof(probe.vendor_name) : sizeof(probe.product_name);
                size_t n = std::min(span.size(), cap - 1);
                memcpy(dst, span.data(), n);
                dst[n] = '\0';
                if (span.size() > n) {
                    probe.truncated = true;
                }
                return;
            }
            uint64_t value = 0;
            if (data->Get(value) != CHIP_NO_ERROR) {
                probe.partial = true;
                return;
            }
            if (aPath.mAttributeId == kBasicVendorId) {
                probe.vendor_id = static_cast<uint16_t>(value);
            } else if (aPath.mAttributeId == kBasicProductId) {
                probe.product_id = static_cast<uint16_t>(value);
            } else if (aPath.mAttributeId == kBasicSoftwareVersion) {
                probe.software_version = static_cast<uint32_t>(value);
            }
            return;
        }

        if (aPath.mClusterId != kDescriptorCluster) {
            return;
        }
        const uint16_t endpoint = aPath.mEndpointId;
        if (aPath.mAttributeId == kDescriptorServerList || aPath.mAttributeId == kDescriptorPartsList) {
            const bool clusters = aPath.mAttributeId == kDescriptorServerList;
            ForEachListItem(aPath, data, [&](chip::TLV::TLVReader &item) {
                uint64_t value = 0;
                if (item.Get(value) != CHIP_NO_ERROR) {
                    probe.partial = true;
                    return;
                }
                if (clusters) {
                    matter_bounds_add_server_cluster(&probe, endpoint, static_cast<uint32_t>(value));
                } else {
                    matter_bounds_add_part(&probe, endpoint, static_cast<uint16_t>(value));
                }
            });
            return;
        }

        if (aPath.mAttributeId == kDescriptorDeviceTypeList) {
            ForEachListItem(aPath, data, [&](chip::TLV::TLVReader &item) {
                if (item.GetType() != chip::TLV::kTLVType_Structure) {
                    probe.partial = true;
                    return;
                }
                chip::TLV::TLVType outer;
                if (item.EnterContainer(outer) != CHIP_NO_ERROR) {
