#include "matter_l2.h"
#include "matter_l2_bounds.h"

#include <algorithm>
#include <cstring>

#include "esp_err.h"
#include "esp_matter.h"
#include "esp_matter_client.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include <app/CommandPathParams.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/ReadPrepareParams.h>
#include <app/StatusIB.h>
#include <controller/AutoCommissioner.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/OperationalCredentialsDelegate.h>
#include <controller/OperationalDeviceProxy.h>
#include <controller_data_model_provider.h>
#include <credentials/FabricTable.h>
#include <credentials/GroupDataProvider.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/PersistentStorageOpCertStore.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <crypto/CHIPCryptoPAL.h>
#include <crypto/DefaultSessionKeystore.h>
#include <crypto/PersistentStorageOperationalKeystore.h>
#include <lib/core/CHIPPersistentStorageDelegate.h>
#include <lib/core/TLV.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/internal/BLEManager.h>
#include <transport/PeerAddress.h>

using chip::ByteSpan;
using chip::MutableByteSpan;
using chip::NodeId;
using chip::ScopedNodeId;
using chip::SessionHandle;
using chip::app::AttributePathParams;
using chip::app::ConcreteDataAttributePath;
using chip::app::InteractionModelEngine;
using chip::app::ReadClient;
using chip::app::StatusIB;
using chip::Messaging::ExchangeManager;

namespace {

constexpr size_t kMaxInflightRequests = 4;
constexpr size_t kMaxProbePaths = 8;
constexpr uint32_t kDescriptorCluster = 0x001d;
constexpr uint32_t kBasicInformationCluster = 0x0028;
constexpr uint32_t kDescriptorDeviceTypeList = 0;
constexpr uint32_t kDescriptorServerList = 1;
constexpr uint32_t kDescriptorPartsList = 3;
constexpr uint32_t kBasicVendorName = 1;
constexpr uint32_t kBasicVendorId = 2;
constexpr uint32_t kBasicProductName = 3;
constexpr uint32_t kBasicProductId = 4;
constexpr uint32_t kBasicSoftwareVersion = 9;

class ControllerStorageDelegate final : public chip::PersistentStorageDelegate {
public:
    CHIP_ERROR SyncGetKeyValue(const char *key, void *buffer, uint16_t &size) override
    {
        size_t bytes_read = 0;
        CHIP_ERROR err = chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get(key, buffer, size, &bytes_read);
        if (err == CHIP_NO_ERROR) {
            size = static_cast<uint16_t>(bytes_read);
        }
        return err;
    }

    CHIP_ERROR SyncSetKeyValue(const char *key, const void *value, uint16_t size) override
    {
        return chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put(key, value, size);
    }

    CHIP_ERROR SyncDeleteKeyValue(const char *key) override
    {
        return chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr().Delete(key);
    }
};

class FixedPaaStore final : public chip::Credentials::AttestationTrustStore {
public:
    chip_status_t Load(const chip_paa_cert_t *certs, size_t count)
    {
        m_count = 0;
        if (count > CHIP_L2_MAX_PAA_CERTS || (count != 0 && certs == nullptr)) {
            return CHIP_STATUS_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < count; ++i) {
            if (certs[i].der == nullptr || certs[i].der_len == 0 || certs[i].der_len > CHIP_L2_MAX_PAA_DER_BYTES) {
                return CHIP_STATUS_INVALID_ARGUMENT;
            }
            m_certs[i].len = certs[i].der_len;
            memcpy(m_certs[i].der, certs[i].der, certs[i].der_len);
        }
        m_count = count;
        return CHIP_STATUS_OK;
    }

    size_t Count() const { return m_count; }

    CHIP_ERROR GetProductAttestationAuthorityCert(const ByteSpan &skid, MutableByteSpan &out_paa_der) const override
    {
        if (skid.size() != chip::Crypto::kSubjectKeyIdentifierLength) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < m_count; ++i) {
            uint8_t candidate_skid[chip::Crypto::kSubjectKeyIdentifierLength] = {};
            MutableByteSpan candidate_skid_span(candidate_skid);
            ByteSpan cert(m_certs[i].der, m_certs[i].len);
            CHIP_ERROR err = chip::Crypto::ExtractSKIDFromX509Cert(cert, candidate_skid_span);
            if (err != CHIP_NO_ERROR) {
                continue;
            }
            if (candidate_skid_span.size() == skid.size() &&
                memcmp(candidate_skid_span.data(), skid.data(), skid.size()) == 0) {
                return chip::CopySpanToMutableSpan(cert, out_paa_der);
            }
        }
        return CHIP_ERROR_CA_CERT_NOT_FOUND;
    }

private:
    struct Cert {
        uint16_t len = 0;
        uint8_t der[CHIP_L2_MAX_PAA_DER_BYTES] = {};
    };
    Cert m_certs[CHIP_L2_MAX_PAA_CERTS];
    size_t m_count = 0;
};

static CHIP_ERROR to_chip_error(chip_status_t status)
{
    switch (status) {
    case CHIP_STATUS_OK:
        return CHIP_NO_ERROR;
    case CHIP_STATUS_INVALID_ARGUMENT:
        return CHIP_ERROR_INVALID_ARGUMENT;
    case CHIP_STATUS_NO_MEMORY:
        return CHIP_ERROR_NO_MEMORY;
    case CHIP_STATUS_TIMEOUT:
        return CHIP_ERROR_TIMEOUT;
    case CHIP_STATUS_NOT_READY:
    case CHIP_STATUS_SECURITY_ERROR:
    case CHIP_STATUS_NO_TRUST_ROOTS:
        return CHIP_ERROR_INCORRECT_STATE;
    default:
        return CHIP_ERROR_INTERNAL;
    }
}

static bool valid_noc_chain(const chip_noc_chain_t &chain)
{
    return chain.noc_len > 0 && chain.noc_len <= CHIP_L2_MAX_NOC_DER_BYTES &&
           chain.rcac_len > 0 && chain.rcac_len <= CHIP_L2_MAX_NOC_DER_BYTES &&
           chain.icac_len <= CHIP_L2_MAX_NOC_DER_BYTES;
}

class OperationalCredentialsAdapter final : public chip::Controller::OperationalCredentialsDelegate {
public:
    void Configure(const chip_operational_credentials_provider_t *provider)
    {
        m_provider = {};
        if (provider) {
            m_provider = *provider;
        }
    }

    void SetIpk(const uint8_t ipk[CHIP_L2_IPK_BYTES])
    {
        memcpy(m_ipk, ipk, CHIP_L2_IPK_BYTES);
        m_ipk_ready = true;
    }

    void ClearIpk()
    {
        chip::Crypto::ClearSecretData(m_ipk, sizeof(m_ipk));
        m_ipk_ready = false;
    }

    bool CanGenerateControllerNoc() const { return m_provider.generate_controller_noc != nullptr; }
    bool CanCommission() const { return m_provider.generate_device_noc != nullptr && m_ipk_ready; }

    chip_status_t GenerateControllerNoc(NodeId controller_node_id, chip::FabricId fabric_id,
                                        const ByteSpan &csr, chip_noc_chain_t &out)
    {
        if (!m_provider.generate_controller_noc) {
            return CHIP_STATUS_SECURITY_ERROR;
        }
        memset(&out, 0, sizeof(out));
        chip_status_t status = m_provider.generate_controller_noc(
            controller_node_id, fabric_id, csr.data(), csr.size(), &out, m_provider.context);
        if (status != CHIP_STATUS_OK) {
            return status;
        }
        if (!valid_noc_chain(out)) {
            return CHIP_STATUS_SECURITY_ERROR;
        }
        return CHIP_STATUS_OK;
    }

    CHIP_ERROR GenerateNOCChain(const ByteSpan &csr_elements, const ByteSpan &csr_nonce,
                                const ByteSpan &attestation_signature, const ByteSpan &attestation_challenge,
                                const ByteSpan &dac, const ByteSpan &pai,
                                chip::Callback::Callback<chip::Controller::OnNOCChainGeneration> *on_completion) override
    {
        if (!on_completion || !m_provider.generate_device_noc || !m_ipk_ready) {
            return CHIP_ERROR_INCORRECT_STATE;
        }
        chip_noc_chain_t chain = {};
        chip_status_t status = m_provider.generate_device_noc(
            m_next_node_id, m_next_fabric_id,
            csr_elements.data(), csr_elements.size(),
            csr_nonce.data(), csr_nonce.size(),
            attestation_signature.data(), attestation_signature.size(),
            attestation_challenge.data(), attestation_challenge.size(),
            dac.data(), dac.size(), pai.data(), pai.size(),
            &chain, m_provider.context);
        if (status != CHIP_STATUS_OK) {
            return to_chip_error(status);
        }
        if (!valid_noc_chain(chain) || memcmp(chain.ipk, m_ipk, sizeof(m_ipk)) != 0) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }

        chip::Crypto::IdentityProtectionKeySpan ipk_span(chain.ipk);
        chip::Optional<NodeId> admin_subject = chain.has_admin_subject
            ? chip::MakeOptional(static_cast<NodeId>(chain.admin_subject))
            : chip::Optional<NodeId>();
        on_completion->mCall(on_completion->mContext, CHIP_NO_ERROR,
                             ByteSpan(chain.noc, chain.noc_len),
                             ByteSpan(chain.icac, chain.icac_len),
                             ByteSpan(chain.rcac, chain.rcac_len),
                             chip::MakeOptional(ipk_span), admin_subject);
        return CHIP_NO_ERROR;
    }

    void SetNodeIdForNextNOCRequest(NodeId node_id) override { m_next_node_id = node_id; }
    void SetFabricIdForNextNOCRequest(chip::FabricId fabric_id) override { m_next_fabric_id = fabric_id; }

private:
    chip_operational_credentials_provider_t m_provider = {};
    uint8_t m_ipk[CHIP_L2_IPK_BYTES] = {};
    bool m_ipk_ready = false;
    NodeId m_next_node_id = chip::kUndefinedNodeId;
    chip::FabricId m_next_fabric_id = 0;
};

enum class RequestKind : uint8_t { None, Read, Probe, Write, Invoke };

static chip_status_t map_chip_error(CHIP_ERROR err)
{
    if (err == CHIP_NO_ERROR) {
        return CHIP_STATUS_OK;
    }
    if (err == CHIP_ERROR_NO_MEMORY) {
        return CHIP_STATUS_NO_MEMORY;
    }
    if (err == CHIP_ERROR_INVALID_ARGUMENT) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    if (err == CHIP_ERROR_TIMEOUT) {
        return CHIP_STATUS_TIMEOUT;
    }
    return CHIP_STATUS_IM_ERROR;
}

static chip_path_t to_public_path(const ConcreteDataAttributePath &path)
{
    return chip_path_t{path.mEndpointId, path.mClusterId, path.mAttributeId};
}

static chip_status_t decode_value(chip::TLV::TLVReader *reader, chip_value_t *out)
{
    if (!reader || !out) {
        return CHIP_STATUS_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    CHIP_ERROR err = CHIP_NO_ERROR;
    switch (reader->GetType()) {
    case chip::TLV::kTLVType_Null:
        out->type = CHIP_VALUE_NULL;
        return CHIP_STATUS_OK;
    case chip::TLV::kTLVType_Boolean:
        out->type = CHIP_VALUE_BOOL;
        err = reader->Get(out->data.boolean);
        break;
    case chip::TLV::kTLVType_SignedInteger:
        out->type = CHIP_VALUE_SIGNED;
        err = reader->Get(out->data.signed_value);
        break;
    case chip::TLV::kTLVType_UnsignedInteger:
        out->type = CHIP_VALUE_UNSIGNED;
        err = reader->Get(out->data.unsigned_value);
        break;
    case chip::TLV::kTLVType_FloatingPointNumber:
        out->type = CHIP_VALUE_FLOAT;
        err = reader->Get(out->data.float_value);
        break;
    case chip::TLV::kTLVType_UTF8String: {
        out->type = CHIP_VALUE_UTF8;
        chip::CharSpan span;
        err = reader->Get(span);
        if (err == CHIP_NO_ERROR) {
            size_t n = std::min(span.size(), static_cast<size_t>(CHIP_L2_MAX_VALUE_BYTES));
            memcpy(out->data.bytes, span.data(), n);
            out->length = static_cast<uint16_t>(n);
            out->truncated = span.size() > n;
        }
        break;
    }
    case chip::TLV::kTLVType_ByteString: {
        out->type = CHIP_VALUE_BYTES;
        ByteSpan span;
        err = reader->Get(span);
        if (err == CHIP_NO_ERROR) {
            size_t n = std::min(span.size(), static_cast<size_t>(CHIP_L2_MAX_VALUE_BYTES));
            memcpy(out->data.bytes, span.data(), n);
            out->length = static_cast<uint16_t>(n);
            out->truncated = span.size() > n;
        }
        break;
    }
    default:
        out->type = CHIP_VALUE_CONTAINER;
        return CHIP_STATUS_UNSUPPORTED_VALUE;
    }
    return map_chip_error(err);
}

static bool bounded_string(const char *src, size_t max_len)
{
    return src && strnlen(src, max_len + 1) <= max_len;
}

struct Runtime;
static Runtime *s_runtime = nullptr;

class RequestSlot final : public ReadClient::Callback, public chip::app::WriteClient::Callback {
public:
    RequestSlot() : connected_cb(OnConnected, this), connect_failed_cb(OnConnectFailed, this) {}

    void Reset()
    {
        StopTimer();
        in_use = false;
        user_done = false;
        kind = RequestKind::None;
        id = 0;
        node_id = 0;
        path = {};
        timed_timeout_ms = 0;
        read_cb = nullptr;
        op_cb = nullptr;
        probe_cb = nullptr;
        context = nullptr;
        json[0] = '\0';
        attr_path_count = 0;
        matter_bounds_reset_node(&probe, 0);
        probe_status = CHIP_STATUS_OK;
        probe_saw_data = false;
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
            args.name = "matter_req";
            if (esp_timer_create(&args, &timer) != ESP_OK) {
                return CHIP_STATUS_INTERNAL;
            }
        }
        esp_timer_stop(timer);
        return esp_timer_start_once(timer, static_cast<uint64_t>(timeout_ms) * 1000ULL) == ESP_OK
                   ? CHIP_STATUS_OK
                   : CHIP_STATUS_INTERNAL;
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

    void StartCase();
    void SendInteraction(ExchangeManager &exchange_mgr, const SessionHandle &session);

    void OnAttributeData(const ConcreteDataAttributePath &aPath, chip::TLV::TLVReader *data,
                         const StatusIB &status) override
    {
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
        } else if (kind == RequestKind::Probe) {
            probe_saw_data = true;
            ProcessProbeAttribute(aPath, data);
        }
    }

    void OnEventData(const chip::app::EventHeader &, chip::TLV::TLVReader *, const StatusIB *) override {}
    void OnDeallocatePaths(chip::app::ReadPrepareParams &&) override {}

    void OnError(CHIP_ERROR error) override
    {
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
        Reset();
    }

    void OnResponse(const chip::app::WriteClient *, const ConcreteDataAttributePath &, StatusIB status) override
    {
        Complete(status.IsSuccess() ? CHIP_STATUS_OK : CHIP_STATUS_IM_ERROR);
    }

    void OnError(const chip::app::WriteClient *, CHIP_ERROR error) override { Complete(map_chip_error(error)); }

    void OnDone(chip::app::WriteClient *) override
    {
        if (!user_done) {
            Complete(CHIP_STATUS_IM_ERROR);
        }
        Reset();
    }

    template <typename F>
    void ForEachListItem(const ConcreteDataAttributePath &path, chip::TLV::TLVReader *data, F fn)
    {
        if (path.mListOp == ConcreteDataAttributePath::ListOperation::ReplaceAll) {
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
        } else if (path.mListOp == ConcreteDataAttributePath::ListOperation::AppendItem) {
            fn(*data);
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
        uint16_t endpoint = aPath.mEndpointId;
        if (aPath.mAttributeId == kDescriptorServerList || aPath.mAttributeId == kDescriptorPartsList) {
            bool clusters = aPath.mAttributeId == kDescriptorServerList;
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

    static void TimerCallback(void *arg);
    static void OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session);
    static void OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR);

    bool in_use = false;
    bool user_done = false;
    RequestKind kind = RequestKind::None;
    chip_request_id_t id = 0;
    uint64_t node_id = 0;
    chip_path_t path = {};
    uint16_t timed_timeout_ms = 0;
    chip_read_callback_t read_cb = nullptr;
    chip_operation_callback_t op_cb = nullptr;
    matter_node_probe_callback_t probe_cb = nullptr;
    void *context = nullptr;
    char json[CHIP_L2_MAX_PAYLOAD_JSON + 1] = {};
    matter_node_info_t probe = {};
    chip_status_t probe_status = CHIP_STATUS_OK;
    bool probe_saw_data = false;
    AttributePathParams attr_paths[kMaxProbePaths];
    size_t attr_path_count = 0;
    esp_timer_handle_t timer = nullptr;
    chip::Callback::Callback<chip::OnDeviceConnected> connected_cb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> connect_failed_cb;
};

class SubscriptionSlot final : public ReadClient::Callback {
public:
    SubscriptionSlot() : connected_cb(OnConnected, this), connect_failed_cb(OnConnectFailed, this) {}

    void Reset()
    {
        in_use = false;
        stopping = false;
        established = false;
        local_id = 0;
        remote_id = 0;
        node_id = 0;
        path = {};
        callback = nullptr;
        context = nullptr;
    }

    void StartCase();

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
    }

    void OnEventData(const chip::app::EventHeader &, chip::TLV::TLVReader *, const StatusIB *) override {}
    void OnDeallocatePaths(chip::app::ReadPrepareParams &&) override {}

    void OnError(CHIP_ERROR) override
    {
        if (callback && !stopping) {
            callback(local_id, CHIP_STATUS_IM_ERROR, path, nullptr, context);
        }
    }

    void OnDone(ReadClient *) override
    {
        if (callback && !stopping) {
            callback(local_id, CHIP_STATUS_IM_ERROR, path, nullptr, context);
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
    chip_subscription_id_t local_id = 0;
    chip::SubscriptionId remote_id = 0;
    uint64_t node_id = 0;
    chip_path_t path = {};
    uint16_t min_interval = 0;
    uint16_t max_interval = 0;
    chip_subscription_callback_t callback = nullptr;
    void *context = nullptr;
    AttributePathParams attr_path;
    chip::Callback::Callback<chip::OnDeviceConnected> connected_cb;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> connect_failed_cb;
};

class CommissionState final : public chip::Controller::DevicePairingDelegate {
public:
    void Reset()
    {
        StopTimer();
        active = false;
        user_done = false;
        request_id = 0;
        node_id = 0;
        callback = nullptr;
        context = nullptr;
        mode = 0;
        pin = 0;
        discriminator = 0;
        port = 0;
        peer_ip[0] = '\0';
        ssid[0] = '\0';
        password[0] = '\0';
        dataset_len = 0;
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
                   ? CHIP_STATUS_OK
                   : CHIP_STATUS_INTERNAL;
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
        if (error != CHIP_NO_ERROR) {
            Complete(map_chip_error(error), 0);
            Finish();
        }
    }

    void OnCommissioningSuccess(chip::PeerId) override
    {
        Complete(CHIP_STATUS_OK, 0);
        Finish();
    }

    void OnCommissioningFailure(chip::PeerId, CHIP_ERROR error, chip::Controller::CommissioningStage stage,
                                chip::Optional<chip::Credentials::AttestationVerificationResult>) override
    {
        chip_status_t st = map_chip_error(error);
        if (st == CHIP_STATUS_IM_ERROR) {
            st = CHIP_STATUS_SECURITY_ERROR;
        }
        Complete(st, static_cast<uint32_t>(stage));
        Finish();
    }

    void OnICDRegistrationComplete(chip::ScopedNodeId, uint32_t) override {}
    void OnICDStayActiveComplete(chip::ScopedNodeId, uint32_t) override {}

    static void TimerCallback(void *arg);

    bool active = false;
    bool user_done = false;
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
    bool platform_started = false;
    bool factory_initialized = false;
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
    chip::Controller::AutoCommissioner auto_commissioner;
    chip::Controller::DeviceCommissioner commissioner;
    chip::Credentials::DeviceAttestationVerifier *dac_verifier = nullptr;

    RequestSlot requests[kMaxInflightRequests];
    SubscriptionSlot subscriptions[CHIP_L2_MAX_SUBSCRIPTIONS];
    CommissionState commission;

    RequestSlot *AllocateRequest(RequestKind kind)
    {
        for (auto &slot : requests) {
            if (!slot.in_use) {
                slot.Reset();
                slot.in_use = true;
                slot.kind = kind;
                slot.id = next_request_id++;
                if (next_request_id == 0) {
                    next_request_id = 1;
                }
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
    if (slot && slot->in_use && !slot->user_done) {
        slot->Complete(CHIP_STATUS_TIMEOUT);
    }
}

static void ScheduleRequestCancel(intptr_t arg)
{
    auto *slot = reinterpret_cast<RequestSlot *>(arg);
    if (slot && slot->in_use && !slot->user_done) {
        slot->Complete(CHIP_STATUS_CANCELLED);
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
    if (slot->established) {
        TEMPORARY_RETURN_IGNORED InteractionModelEngine::GetInstance()->ShutdownSubscription(
            ScopedNodeId(slot->node_id, s_runtime->commissioner.GetFabricIndex()), slot->remote_id);
    } else {
        slot->Reset();
    }
}

static void ScheduleCommissionCancel(intptr_t)
{
    if (!s_runtime || !s_runtime->commission.active) {
        return;
    }
    TEMPORARY_RETURN_IGNORED s_runtime->commissioner.StopPairing(s_runtime->commission.node_id);
    s_runtime->commission.Finish();
}

static void ScheduleCommissionTimeout(intptr_t)
{
    if (!s_runtime || !s_runtime->commission.active || s_runtime->commission.user_done) {
        return;
    }
    s_runtime->commission.Complete(CHIP_STATUS_TIMEOUT, 0);
    ScheduleCommissionCancel(0);
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
        rendezvous.SetPeerAddress(chip::Transport::PeerAddress::UDP(address, state.port, chip::Inet::InterfaceId::Null()));
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

void RequestSlot::OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session)
{
    auto *self = static_cast<RequestSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
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
    self->Complete(CHIP_STATUS_CASE_FAILED);
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
    CHIP_ERROR err = s_runtime->commissioner.GetConnectedDevice(node_id, &connected_cb, &connect_failed_cb);
    if (err != CHIP_NO_ERROR) {
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
    chip::OperationalDeviceProxy proxy(&exchange_mgr, session);
    esp_err_t err = ESP_FAIL;
    if (kind == RequestKind::Read || kind == RequestKind::Probe) {
        err = esp_matter::client::interaction::read::send_request(&proxy, attr_paths, attr_path_count, nullptr, 0, *this);
    } else if (kind == RequestKind::Write) {
        AttributePathParams attr(path.endpoint_id, path.cluster_id, path.item_id);
        chip::Optional<uint16_t> timed = timed_timeout_ms ? chip::MakeOptional(timed_timeout_ms) : chip::NullOptional;
        err = esp_matter::client::interaction::write::send_request(&proxy, attr, json, *this, timed);
    } else if (kind == RequestKind::Invoke) {
        chip::app::CommandPathParams command_path = {path.endpoint_id, 0, path.cluster_id, path.item_id,
                                                     chip::app::CommandPathFlags::kEndpointIdValid};
        chip::Optional<uint16_t> timed = timed_timeout_ms ? chip::MakeOptional(timed_timeout_ms) : chip::NullOptional;
        err = esp_matter::client::interaction::invoke::send_request(
            this, &proxy, command_path, json[0] ? json : "{}",
            [](void *ctx, const chip::app::ConcreteCommandPath &, const StatusIB &status, chip::TLV::TLVReader *) {
                auto *self = static_cast<RequestSlot *>(ctx);
                if (self && self->in_use) {
                    self->Complete(status.IsSuccess() ? CHIP_STATUS_OK : CHIP_STATUS_IM_ERROR);
                    self->Reset();
                }
            },
            [](void *ctx, CHIP_ERROR error) {
                auto *self = static_cast<RequestSlot *>(ctx);
                if (self && self->in_use) {
                    self->Complete(map_chip_error(error));
                    self->Reset();
                }
            }, timed);
    }
    if (err != ESP_OK) {
        Complete(CHIP_STATUS_IM_ERROR);
        Reset();
    }
}

void SubscriptionSlot::StartCase()
{
    if (!s_runtime || !s_runtime->ready || !in_use) {
        if (callback) {
            callback(local_id, CHIP_STATUS_NOT_READY, path, nullptr, context);
        }
        Reset();
        return;
    }
    CHIP_ERROR err = s_runtime->commissioner.GetConnectedDevice(node_id, &connected_cb, &connect_failed_cb);
    if (err != CHIP_NO_ERROR) {
        if (callback) {
            callback(local_id, CHIP_STATUS_CASE_FAILED, path, nullptr, context);
        }
        Reset();
    }
}

void SubscriptionSlot::OnConnected(void *context, ExchangeManager &exchange_mgr, const SessionHandle &session)
{
    auto *self = static_cast<SubscriptionSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    if (self->stopping) {
        self->Reset();
        return;
    }
    chip::OperationalDeviceProxy proxy(&exchange_mgr, session);
    self->attr_path = AttributePathParams(self->path.endpoint_id, self->path.cluster_id, self->path.item_id);
    esp_err_t err = esp_matter::client::interaction::subscribe::send_request(
        &proxy, &self->attr_path, 1, nullptr, 0, self->min_interval, self->max_interval, true, false, *self);
    if (err != ESP_OK) {
        if (self->callback) {
            self->callback(self->local_id, CHIP_STATUS_IM_ERROR, self->path, nullptr, self->context);
        }
        self->Reset();
    }
}

void SubscriptionSlot::OnConnectFailed(void *context, const ScopedNodeId &, CHIP_ERROR)
{
    auto *self = static_cast<SubscriptionSlot *>(context);
    if (!self || !self->in_use) {
        return;
    }
    if (self->callback) {
        self->callback(self->local_id, CHIP_STATUS_CASE_FAILED, self->path, nullptr, self->context);
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
        return CHIP_STATUS_SECURITY_ERROR;
    }
    rt.op_creds.SetIpk(keyset.epoch_keys[0].key);
    return CHIP_STATUS_OK;
}

static chip_status_t InitializeController(Runtime &rt)
{
    if (!rt.platform_started) {
        esp_err_t start_err = esp_matter::start(nullptr);
        if (start_err != ESP_OK) {
            return CHIP_STATUS_INTERNAL;
        }
        rt.platform_started = true;
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
        factory_params.sessionKeystore = &rt.session_keystore;
        factory_params.groupDataProvider = &rt.group_data_provider;
        factory_params.enableServerInteractions = false;
        factory_params.enableTCPServer = false;
        factory_params.dataModelProvider = &esp_matter::controller::data_model::provider::get_instance();
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
    uint8_t fabric_count = fabrics->FabricCount();
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
            return map_chip_error(err);
        }

        chip_noc_chain_t chain = {};
        chip_status_t provider_status = rt.op_creds.GenerateControllerNoc(
            rt.config.controller_node_id, rt.config.fabric_id, ByteSpan(csr.data(), csr.size()), chain);
        if (provider_status != CHIP_STATUS_OK) {
            fabrics->RevertPendingFabricData();
            return provider_status;
        }

        setup.operationalKeypair = nullptr;
        setup.controllerRCAC = ByteSpan(chain.rcac, chain.rcac_len);
        setup.controllerICAC = ByteSpan(chain.icac, chain.icac_len);
        setup.controllerNOC = ByteSpan(chain.noc, chain.noc_len);
        err = factory.SetupCommissioner(setup, rt.commissioner);
        if (err != CHIP_NO_ERROR) {
            fabrics->RevertPendingFabricData();
            return map_chip_error(err);
        }

        uint8_t compressed[sizeof(uint64_t)] = {};
        MutableByteSpan compressed_span(compressed);
        err = rt.commissioner.GetCompressedFabricIdBytes(compressed_span);
        if (err != CHIP_NO_ERROR) {
            rt.commissioner.Shutdown();
            return map_chip_error(err);
        }
        err = chip::Credentials::SetSingleIpkEpochKey(
            &rt.group_data_provider, rt.commissioner.GetFabricIndex(),
            ByteSpan(chain.ipk, sizeof(chain.ipk)), compressed_span);
        if (err != CHIP_NO_ERROR) {
            rt.commissioner.Shutdown();
            return map_chip_error(err);
        }
        rt.op_creds.SetIpk(chain.ipk);
        chip::Crypto::ClearSecretData(chain.ipk, sizeof(chain.ipk));
    }

    rt.commissioner.RegisterPairingDelegate(nullptr);
    return CHIP_STATUS_OK;
}

static chip_status_t RequireReady()
{
    return s_runtime && s_runtime->ready ? CHIP_STATUS_OK : CHIP_STATUS_NOT_READY;
}

static chip_status_t QueueRequest(RequestSlot *slot, uint32_t timeout_ms, chip_request_id_t *out_id)
{
    chip_status_t st = slot->StartTimer(timeout_ms);
    if (st != CHIP_STATUS_OK) {
        slot->Reset();
        return st;
    }
    *out_id = slot->id;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleRequestStart, reinterpret_cast<intptr_t>(slot)) != CHIP_NO_ERROR) {
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
    if (g_runtime.ready) {
        return CHIP_STATUS_BUSY;
    }
    chip_status_t paa_status = g_runtime.paa_store.Load(config->paa_certs, config->paa_cert_count);
    if (paa_status != CHIP_STATUS_OK) {
        return paa_status;
    }
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
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
        return status;
    }
    g_runtime.ready = true;
    return CHIP_STATUS_OK;
}

chip_status_t chip_controller_shutdown(void)
{
    if (!s_runtime || !g_runtime.ready) {
        return CHIP_STATUS_OK;
    }
    if (g_runtime.Busy()) {
        return CHIP_STATUS_BUSY;
    }
    g_runtime.ready = false;
    g_runtime.commissioner.Shutdown();
    g_runtime.op_creds.ClearIpk();
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
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleSubscriptionStart, reinterpret_cast<intptr_t>(slot)) != CHIP_NO_ERROR) {
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
    if (slot->callback) {
        slot->callback(slot->local_id, CHIP_STATUS_CANCELLED, slot->path, nullptr, slot->context);
    }
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(ScheduleSubscriptionStop, reinterpret_cast<intptr_t>(slot)) == CHIP_NO_ERROR
               ? CHIP_STATUS_OK
               : CHIP_STATUS_INTERNAL;
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
    state.request_id = g_runtime.next_request_id++;
    state.node_id = params->node_id;
    state.callback = callback;
    state.context = context;
    state.mode = 0;
    state.pin = params->setup_pin_code;
    state.port = params->peer_port;
    strcpy(state.peer_ip, params->peer_ip);
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
    state.request_id = g_runtime.next_request_id++;
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
    state.request_id = g_runtime.next_request_id++;
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
