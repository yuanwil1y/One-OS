            ++m_cur;
            return true;
        }
        return false;
    }

    bool ConsumeLiteral(const char *text)
    {
        SkipWs();
        const size_t n = strlen(text);
        if (static_cast<size_t>(m_end - m_cur) >= n && memcmp(m_cur, text, n) == 0) {
            m_cur += n;
            return true;
        }
        return false;
    }

    CHIP_ERROR ParseTagKey(uint8_t &tag_num)
    {
        char key[4] = {};
        size_t len = 0;
        ReturnErrorOnFailure(ParseString(key, sizeof(key), len));
        VerifyOrReturnError(len > 0 && len <= 3, CHIP_ERROR_INVALID_ARGUMENT);
        unsigned value = 0;
        for (size_t i = 0; i < len; ++i) {
            VerifyOrReturnError(key[i] >= '0' && key[i] <= '9', CHIP_ERROR_INVALID_ARGUMENT);
            value = value * 10u + static_cast<unsigned>(key[i] - '0');
            VerifyOrReturnError(value <= 255u, CHIP_ERROR_INVALID_ARGUMENT);
        }
        tag_num = static_cast<uint8_t>(value);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR ParseString(char *out, size_t cap, size_t &out_len)
    {
        SkipWs();
        VerifyOrReturnError(m_cur < m_end && *m_cur == '"', CHIP_ERROR_INVALID_ARGUMENT);
        ++m_cur;
        out_len = 0;
        while (m_cur < m_end) {
            unsigned char c = static_cast<unsigned char>(*m_cur++);
            if (c == '"') {
                VerifyOrReturnError(out_len < cap, CHIP_ERROR_BUFFER_TOO_SMALL);
                out[out_len] = '\0';
                return CHIP_NO_ERROR;
            }
            if (c == '\\') {
                VerifyOrReturnError(m_cur < m_end, CHIP_ERROR_INVALID_ARGUMENT);
                const char esc = *m_cur++;
                switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default:
                    return CHIP_ERROR_INVALID_ARGUMENT;
                }
            }
            VerifyOrReturnError(c >= 0x20, CHIP_ERROR_INVALID_ARGUMENT);
            VerifyOrReturnError(out_len + 1 < cap, CHIP_ERROR_BUFFER_TOO_SMALL);
            out[out_len++] = static_cast<char>(c);
        }
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    CHIP_ERROR ParseNumber(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag)
    {
        SkipWs();
        const char *start = m_cur;
        if (m_cur < m_end && *m_cur == '-') {
            ++m_cur;
        }
        VerifyOrReturnError(m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur)),
                            CHIP_ERROR_INVALID_ARGUMENT);
        while (m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur))) {
            ++m_cur;
        }
        bool floating = false;
        if (m_cur < m_end && *m_cur == '.') {
            floating = true;
            ++m_cur;
            VerifyOrReturnError(m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur)),
                                CHIP_ERROR_INVALID_ARGUMENT);
            while (m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur))) {
                ++m_cur;
            }
        }
        if (m_cur < m_end && (*m_cur == 'e' || *m_cur == 'E')) {
            floating = true;
            ++m_cur;
            if (m_cur < m_end && (*m_cur == '+' || *m_cur == '-')) {
                ++m_cur;
            }
            VerifyOrReturnError(m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur)),
                                CHIP_ERROR_INVALID_ARGUMENT);
            while (m_cur < m_end && std::isdigit(static_cast<unsigned char>(*m_cur))) {
                ++m_cur;
            }
        }

        const size_t len = static_cast<size_t>(m_cur - start);
        VerifyOrReturnError(len < 48, CHIP_ERROR_INVALID_ARGUMENT);
        char tmp[48] = {};
        memcpy(tmp, start, len);
        if (floating) {
            errno = 0;
            char *endp = nullptr;
            const double value = strtod(tmp, &endp);
            VerifyOrReturnError(errno == 0 && endp == tmp + len && std::isfinite(value),
                                CHIP_ERROR_INVALID_ARGUMENT);
            return writer.Put(tag, value);
        }
        if (tmp[0] == '-') {
            errno = 0;
            char *endp = nullptr;
            const long long value = strtoll(tmp, &endp, 10);
            VerifyOrReturnError(errno == 0 && endp == tmp + len, CHIP_ERROR_INVALID_ARGUMENT);
            return writer.Put(tag, static_cast<int64_t>(value));
        }
        errno = 0;
        char *endp = nullptr;
        const unsigned long long value = strtoull(tmp, &endp, 10);
        VerifyOrReturnError(errno == 0 && endp == tmp + len, CHIP_ERROR_INVALID_ARGUMENT);
        return writer.Put(tag, static_cast<uint64_t>(value));
    }

    CHIP_ERROR ParseValue(chip::TLV::TLVWriter &writer, chip::TLV::Tag tag, uint8_t depth)
    {
        VerifyOrReturnError(depth <= kMaxJsonDepth, CHIP_ERROR_INVALID_ARGUMENT);
        SkipWs();
        VerifyOrReturnError(m_cur < m_end, CHIP_ERROR_INVALID_ARGUMENT);

        if (*m_cur == '"') {
            char text[CHIP_L2_MAX_PAYLOAD_JSON + 1] = {};
            size_t len = 0;
            CHIP_ERROR err = ParseString(text, sizeof(text), len);
            if (err == CHIP_NO_ERROR) {
                err = writer.PutString(tag, text, static_cast<uint32_t>(len));
            }
            secure_zero(text, sizeof(text));
            return err;
        }
        if (ConsumeLiteral("null")) {
            return writer.PutNull(tag);
        }
        if (ConsumeLiteral("true")) {
            return writer.Put(tag, true);
        }
        if (ConsumeLiteral("false")) {
            return writer.Put(tag, false);
        }
        if (*m_cur == '[') {
            ++m_cur;
            chip::TLV::TLVType outer;
            ReturnErrorOnFailure(writer.StartContainer(tag, chip::TLV::kTLVType_Array, outer));
            SkipWs();
            if (!Consume(']')) {
                for (;;) {
                    CHIP_ERROR err = ParseValue(writer, chip::TLV::AnonymousTag(), depth + 1);
                    if (err != CHIP_NO_ERROR) {
                        return err;
                    }
                    SkipWs();
                    if (Consume(']')) {
                        break;
                    }
                    VerifyOrReturnError(Consume(','), CHIP_ERROR_INVALID_ARGUMENT);
                }
            }
            return writer.EndContainer(outer);
        }
        if (*m_cur == '{') {
            ++m_cur;
            chip::TLV::TLVType outer;
            ReturnErrorOnFailure(writer.StartContainer(tag, chip::TLV::kTLVType_Structure, outer));
            SkipWs();
            if (!Consume('}')) {
                for (;;) {
                    uint8_t tag_num = 0;
                    ReturnErrorOnFailure(ParseTagKey(tag_num));
                    VerifyOrReturnError(Consume(':'), CHIP_ERROR_INVALID_ARGUMENT);
                    ReturnErrorOnFailure(ParseValue(writer, chip::TLV::ContextTag(tag_num), depth + 1));
                    SkipWs();
                    if (Consume('}')) {
                        break;
                    }
                    VerifyOrReturnError(Consume(','), CHIP_ERROR_INVALID_ARGUMENT);
                }
            }
            return writer.EndContainer(outer);
        }
        return ParseNumber(writer, tag);
    }

    const char *m_cur;
    const char *m_end;
};

enum class RequestKind : uint8_t { None, Read, Probe, Write, Invoke };

struct Runtime;
static Runtime *s_runtime = nullptr;

class RequestSlot final : public ReadClient::Callback,
                          public chip::app::WriteClient::Callback,
                          public chip::app::CommandSender::ExtendableCallback {
public:
    using NativeStorage = typename std::aligned_union<0, ReadClient, chip::app::WriteClient,
                                                      chip::app::CommandSender>::type;

    RequestSlot() : connected_cb(OnConnected, this), connect_failed_cb(OnConnectFailed, this) {}

    void ResetPublicFields()
    {
        user_done = false;
        kind = RequestKind::None;
        id = 0;
        node_id = 0;
        path = {};
        timed_timeout_ms = 0;
        timeout_ms = 0;
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

    void Reset()
    {
        StopTimer();
        DestroyNative();
        in_use = false;
        case_pending = false;
        ResetPublicFields();
    }

    chip_status_t StartTimer(uint32_t requested_timeout_ms)
    {
