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
            secure_zero(&chain, sizeof(chain));
            return to_chip_error(status);
        }
        if (!valid_noc_chain(chain) || memcmp(chain.ipk, m_ipk, sizeof(m_ipk)) != 0) {
            secure_zero(&chain, sizeof(chain));
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
        secure_zero(&chain, sizeof(chain));
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

class ControllerOnlyDataModelProvider final : public chip::app::DataModel::Provider {
public:
    CHIP_ERROR Endpoints(chip::ReadOnlyBufferBuilder<chip::app::DataModel::EndpointEntry> &) override { return CHIP_NO_ERROR; }
    CHIP_ERROR DeviceTypes(chip::EndpointId, chip::ReadOnlyBufferBuilder<chip::app::DataModel::DeviceTypeEntry> &) override
    {
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR ClientClusters(chip::EndpointId, chip::ReadOnlyBufferBuilder<chip::ClusterId> &) override { return CHIP_NO_ERROR; }
    CHIP_ERROR ServerClusters(chip::EndpointId,
                              chip::ReadOnlyBufferBuilder<chip::app::DataModel::ServerClusterEntry> &) override
    {
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR EventInfo(const chip::app::ConcreteEventPath &, chip::app::DataModel::EventEntry &) override
    {
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &,
                          chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &) override
    {
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GeneratedCommands(const chip::app::ConcreteClusterPath &,
                                 chip::ReadOnlyBufferBuilder<chip::CommandId> &) override
    {
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath &,
                                chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> &) override
    {
        return CHIP_NO_ERROR;
    }

    chip::app::DataModel::ActionReturnStatus ReadAttribute(
        const chip::app::DataModel::ReadAttributeRequest &, chip::app::AttributeValueEncoder &) override
    {
        return chip::Protocols::InteractionModel::Status::UnsupportedAttribute;
    }

    chip::app::DataModel::ActionReturnStatus WriteAttribute(
        const chip::app::DataModel::WriteAttributeRequest &, chip::app::AttributeValueDecoder &) override
    {
        return chip::Protocols::InteractionModel::Status::UnsupportedWrite;
    }

    void ListAttributeWriteNotification(const chip::app::ConcreteAttributePath &,
                                        chip::app::DataModel::ListWriteOperation,
                                        chip::FabricIndex) override
    {}

    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(
        const chip::app::DataModel::InvokeRequest &, chip::TLV::TLVReader &,
        chip::app::CommandHandler *) override
    {
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }
};

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

/*
 * The public API keeps its bounded JSON argument for compatibility, but the
 * direct controller intentionally supports a deterministic subset only:
 * scalar JSON values plus arrays, and structures whose keys are decimal
 * Matter context-tag numbers (0..255). No name/schema database is introduced.
 */
class BoundedJsonTlvEncoder {
public:
    explicit BoundedJsonTlvEncoder(const char *json) : m_cur(json), m_end(json + strlen(json)) {}

    CHIP_ERROR EncodeValue(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag)
    {
        CHIP_ERROR err = ParseValue(writer, tag, 0);
        if (err != CHIP_NO_ERROR) {
            return err;
        }
        SkipWs();
        return m_cur == m_end ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
    }

    CHIP_ERROR EncodeCommandFields(chip::TLV::TLVWriter &writer)
    {
        SkipWs();
        if (!Consume('{')) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        SkipWs();
        if (Consume('}')) {
            SkipWs();
            return m_cur == m_end ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
        }
        for (;;) {
            uint8_t tag_num = 0;
            ReturnErrorOnFailure(ParseTagKey(tag_num));
            SkipWs();
            VerifyOrReturnError(Consume(':'), CHIP_ERROR_INVALID_ARGUMENT);
            ReturnErrorOnFailure(ParseValue(writer, chip::TLV::ContextTag(tag_num), 1));
            SkipWs();
            if (Consume('}')) {
                break;
            }
            VerifyOrReturnError(Consume(','), CHIP_ERROR_INVALID_ARGUMENT);
        }
        SkipWs();
        return m_cur == m_end ? CHIP_NO_ERROR : CHIP_ERROR_INVALID_ARGUMENT;
    }

private:
    void SkipWs()
    {
        while (m_cur < m_end && std::isspace(static_cast<unsigned char>(*m_cur))) {
            ++m_cur;
        }
    }

    bool Consume(char c)
    {
        SkipWs();
        if (m_cur < m_end && *m_cur == c) {
