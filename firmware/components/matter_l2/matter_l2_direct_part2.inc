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
            const bool match = candidate_skid_span.size() == skid.size() &&
                               memcmp(candidate_skid_span.data(), skid.data(), skid.size()) == 0;
            secure_zero(candidate_skid, sizeof(candidate_skid));
            if (match) {
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
    Cert m_certs[CHIP_L2_MAX_PAA_CERTS] = {};
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

static chip_status_t map_chip_error(CHIP_ERROR err)
{
    if (err == CHIP_NO_ERROR) {
        return CHIP_STATUS_OK;
    }
    if (err == CHIP_ERROR_NO_MEMORY || err == CHIP_ERROR_BUFFER_TOO_SMALL) {
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

    void Clear()
    {
        chip::Crypto::ClearSecretData(m_ipk, sizeof(m_ipk));
        m_ipk_ready = false;
        m_next_node_id = chip::kUndefinedNodeId;
        m_next_fabric_id = 0;
        m_provider = {};
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
        if (status != CHIP_STATUS_OK || !valid_noc_chain(out)) {
            secure_zero(&out, sizeof(out));
            return status == CHIP_STATUS_OK ? CHIP_STATUS_SECURITY_ERROR : status;
        }
        return CHIP_STATUS_OK;
    }

    CHIP_ERROR GenerateNOCChain(const ByteSpan &csr_elements, const ByteSpan &csr_nonce,
                                const ByteSpan &attestation_signature, const ByteSpan &attestation_challenge,
                                const ByteSpan &dac, const ByteSpan &pai,
