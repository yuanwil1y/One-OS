#include "matter_l2.h"
#include "matter_l2_bounds.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <type_traits>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include <app/AttributePathParams.h>
#include <app/CommandPathParams.h>
#include <app/CommandSender.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/ReadPrepareParams.h>
#include <app/StatusIB.h>
#include <app/WriteClient.h>
#include <app/data-model-provider/Provider.h>
#include <controller/AutoCommissioner.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/OperationalCredentialsDelegate.h>
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
#include <lib/support/CHIPMem.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/internal/BLEManager.h>
#include <protocols/interaction_model/StatusCode.h>
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
constexpr size_t kTlvPayloadBytes = CHIP_L2_MAX_PAYLOAD_JSON + 96;
constexpr uint8_t kMaxJsonDepth = 4;
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

static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = static_cast<volatile uint8_t *>(ptr);
    while (len-- > 0) {
        *p++ = 0;
    }
}

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
        Clear();
        if (count > CHIP_L2_MAX_PAA_CERTS || (count != 0 && certs == nullptr)) {
            return CHIP_STATUS_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < count; ++i) {
            if (certs[i].der == nullptr || certs[i].der_len == 0 || certs[i].der_len > CHIP_L2_MAX_PAA_DER_BYTES) {
                Clear();
                return CHIP_STATUS_INVALID_ARGUMENT;
            }
            m_certs[i].len = certs[i].der_len;
            memcpy(m_certs[i].der, certs[i].der, certs[i].der_len);
        }
        m_count = count;
        return CHIP_STATUS_OK;
    }

    void Clear()
    {
        secure_zero(m_certs, sizeof(m_certs));
        m_count = 0;
    }

