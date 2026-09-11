/*
 * Provisioning portal wire layer. See app_portal.h for the contract.
 *
 * Platform independent: no sockets, no NVS, no ESP-IDF task or event types. The
 * only ESP-IDF name used is esp_err_t, which the host stubs provide with the real
 * values, so this file compiles for both the target and the test build.
 */

#include "app_portal.h"

#include "app_str.h"

#include <stdio.h>
#include <string.h>

/* ---------------- form parsing ---------------- */

const char *app_portal_form_status_name(app_portal_form_status_t status)
{
    switch (status) {
    case APP_PORTAL_FORM_OK:               return "ok";
    case APP_PORTAL_FORM_MISSING_SSID:     return "missing_ssid";
    case APP_PORTAL_FORM_SSID_TOO_LONG:    return "ssid_too_long";
    case APP_PORTAL_FORM_PASSWORD_TOO_LONG:return "password_too_long";
    case APP_PORTAL_FORM_MALFORMED:        return "malformed_form";
    case APP_PORTAL_FORM_NOT_FORM:         return "not_form_encoded";
    case APP_PORTAL_FORM_SSID_INVALID:     return "ssid_invalid";
    default:                               return "invalid";
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * Find the value of `key` in a form-encoded body.
 *
 * The body is `key=value&key=value...` and is not necessarily terminated. A field
 * is only matched at the start of the body or immediately after a '&', so
 * "xssid" cannot be mistaken for "ssid".
 *
 * `*out_length` receives the DECODED length, which is not strlen(): a value may
 * legitimately decode to contain a NUL (from "%00"), and the caller has to be able
 * to see that byte in order to refuse it. Returning a C string alone would make the
 * embedded NUL invisible and let it through as a truncation.
 *
 * A malformed percent-escape anywhere in the matched value makes the whole parse
 * fail, because silently substituting a literal '%' would change a credential
 * without telling anyone.
 */
static bool form_find(const char *body, size_t length, const char *key, char *out,
                      size_t out_size, size_t *out_length, bool *out_too_long,
                      bool *out_malformed)
{
    size_t key_len = strlen(key);
    size_t i = 0u;
    size_t used = 0u;

    *out_too_long = false;
    *out_malformed = false;
    if (out_length != NULL) {
        *out_length = 0u;
    }
    if (out_size == 0u) {
        return false;
    }
    out[0] = '\0';

    while (i < length) {
        size_t field_start = i;
        size_t value_start;
        size_t value_end;

        /* Field boundaries. */
        while (i < length && body[i] != '&') {
            i++;
        }
        value_end = i;
        if (i < length) {
            i++; /* skip '&' */
        }

        /* "key=" prefix. */
        if (value_end - field_start <= key_len ||
            memcmp(body + field_start, key, key_len) != 0 ||
            body[field_start + key_len] != '=') {
            continue;
        }
        value_start = field_start + key_len + 1u;

        for (size_t p = value_start; p < value_end; ++p) {
            char c = body[p];

            if (c == '+') {
                c = ' ';
            } else if (c == '%') {
                int high;
                int low;

                if (p + 2u >= value_end) {
                    *out_malformed = true;
                    return false;
                }
                high = hex_digit(body[p + 1u]);
                low = hex_digit(body[p + 2u]);
                if (high < 0 || low < 0) {
                    *out_malformed = true;
                    return false;
                }
                c = (char)((high << 4) | low);
                p += 2u;
            }
            if (used + 1u >= out_size) {
                /* Overflow: reported, and the caller refuses the whole request
                 * rather than truncating a credential into a different one. */
                *out_too_long = true;
                return false;
            }
            out[used++] = c;
        }
        out[used] = '\0';
        if (out_length != NULL) {
            *out_length = used;
        }
        return true;
    }
    return false;
}

app_portal_form_status_t app_portal_parse_wifi_form(const char *body, size_t length,
                                                    app_portal_wifi_form_t *out)
{
    bool missing_ssid = false;
    bool ssid_too_long = false;
    bool ssid_malformed = false;
    bool password_too_long = false;
    bool password_malformed = false;
    bool password_found;
    size_t ssid_length = 0u;
    size_t password_length = 0u;

    if (out == NULL) {
        return APP_PORTAL_FORM_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    if (body == NULL) {
        return APP_PORTAL_FORM_MALFORMED;
    }

    password_found = form_find(body, length, "password", out->password,
                               sizeof(out->password), &password_length,
                               &password_too_long, &password_malformed);
    if (password_malformed) {
        return APP_PORTAL_FORM_MALFORMED;
    }
    if (password_too_long) {
        return APP_PORTAL_FORM_PASSWORD_TOO_LONG;
    }
    out->password_present = password_found;

    if (!form_find(body, length, "ssid", out->ssid, sizeof(out->ssid), &ssid_length,
                   &ssid_too_long, &ssid_malformed)) {
        if (ssid_malformed) {
            return APP_PORTAL_FORM_MALFORMED;
        }
        if (ssid_too_long) {
            return APP_PORTAL_FORM_SSID_TOO_LONG;
        }
        missing_ssid = true;
    }
    if (missing_ssid || ssid_length == 0u) {
        return APP_PORTAL_FORM_MISSING_SSID;
    }
    if (ssid_length > WIFI_MGR_SSID_MAX) {
        return APP_PORTAL_FORM_SSID_TOO_LONG;
    }
    /*
     * An SSID is a byte string on the air but a credential in every log and status
     * document it touches, so control characters are refused rather than escaped at
     * each of those places.
     *
     * The loop runs to the DECODED length, not to the first NUL: "bad%00name"
     * decodes to a value whose second byte is NUL, and stopping at strlen() would
     * see only "bad" and accept it - silently turning a rejected credential into an
     * accepted one for a different network.
     */
    for (size_t i = 0u; i < ssid_length; ++i) {
        unsigned char c = (unsigned char)out->ssid[i];

        if (c < 0x20u || c == 0x7Fu) {
            return APP_PORTAL_FORM_SSID_INVALID;
        }
    }
    /* An empty password field means the same as no password field: an open
     * network. Normalising it here keeps that decision in one place. */
    if (password_found && password_length == 0u) {
        out->password_present = false;
    }
    return APP_PORTAL_FORM_OK;
}

bool app_portal_parse_content_length(const char *value, uint64_t *out_length)
{
    uint64_t total = 0u;
    size_t i = 0u;

    if (value == NULL || out_length == NULL) {
        return false;
    }
    *out_length = 0u;

    /* Leading spaces are legal in a header value; a sign is not, because a
     * negative length is meaningless and a '+'-prefixed one is not a number a
     * browser sends. */
    while (value[i] == ' ' || value[i] == '\t') {
        i++;
    }
    if (value[i] == '\0') {
        return false;
    }
    for (; value[i] != '\0'; ++i) {
        char c = value[i];

        if (c == ' ' || c == '\t') {
            /* Trailing spaces are legal; anything after them is not. */
            for (size_t j = i; value[j] != '\0'; ++j) {
                if (value[j] != ' ' && value[j] != '\t') {
                    return false;
                }
            }
            break;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        if (total > (UINT64_MAX - (uint64_t)(c - '0')) / 10u) {
            return false; /* would overflow: refuse rather than wrap */
        }
        total = total * 10u + (uint64_t)(c - '0');
    }
    *out_length = total;
    return true;
}

/* ---------------- response building ---------------- */

/*
 * A bounded append helper.
 *
 * Every builder below uses it, so the "did it fit" decision exists once. On
 * overflow it records the failure and stops appending; the caller then returns 0
 * and the handler sends an error rather than a truncated document.
 */
typedef struct {
    char *out;
    size_t size;
    size_t used;
    bool overflow;
    /*
     * The document is composed here first, so a buffer too small can be answered
     * with nothing rather than with a truncated JSON document. A browser that
     * received half a document would either fail to parse it or, worse, parse a
     * prefix as if it were the whole answer.
     *
     * A pointer, not an array: this struct is created inside the builder, so an
     * inline 4 KiB array would put 4 KiB on the caller's stack - which, on the HTTP
     * server task, is where that would actually hurt. The builders pass a local
     * buffer instead, so the cost is stack at the point of use and every call gets
     * its own, with no shared state.
     */
    char *staging;
} json_writer_t;

static void jw_init(json_writer_t *w, char *out, size_t size, char *staging)
{
    w->out = out;
    w->size = size;
    w->used = 0u;
    w->overflow = false;
    w->staging = staging;
    if (staging != NULL) {
        staging[0] = '\0';
    }
    /*
     * `out` is not cleared here.
     *
     * The builders promise that a buffer too small produces NOTHING, and that is
     * only true if nothing is written until the document is known to fit. jw_flush()
     * below is what performs that copy. Clearing here would be enough on its own but
     * would leave the partial document visible to a caller that ignored the return
     * value, which is exactly the case the promise exists for.
     */
}

/*
 * Publish the document, or nothing at all.
 *
 * A caller that ignores the return value cannot send a half-written JSON document,
 * because a document that did not fit is never copied out.
 */
static size_t jw_flush(json_writer_t *w)
{
    if (w->overflow || w->out == NULL) {
        return 0u;
    }
    if (w->used + 1u > w->size) {
        return 0u;
    }
    memcpy(w->out, w->staging, w->used);
    w->out[w->used] = '\0';
    return w->used;
}

static void jw_raw(json_writer_t *w, const char *text)
{
    size_t len;

    if (w->overflow || text == NULL) {
        return;
    }
    len = strlen(text);
    if (w->staging == NULL || w->used + len + 1u > APP_PORTAL_JSON_MAX) {
        w->overflow = true;
        return;
    }
    memcpy(w->staging + w->used, text, len);
    w->used += len;
    w->staging[w->used] = '\0';
}

static void jw_u64(json_writer_t *w, uint64_t value)
{
    char scratch[24];

    (void)snprintf(scratch, sizeof(scratch), "%llu", (unsigned long long)value);
    jw_raw(w, scratch);
}

static void jw_i32(json_writer_t *w, int32_t value)
{
    char scratch[16];

    (void)snprintf(scratch, sizeof(scratch), "%ld", (long)value);
    jw_raw(w, scratch);
}

bool app_portal_json_token_is_safe(const char *text)
{
    if (text == NULL) {
        return false;
    }
    for (size_t i = 0u; text[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)text[i];

        if (c < 0x20u || c == 0x7Fu || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

static void jw_token(json_writer_t *w, const char *text)
{
    if (!app_portal_json_token_is_safe(text)) {
        /* Refused rather than escaped. Every value that reaches here is one of a
         * small fixed set produced by this firmware, so unsafe text means something
         * upstream broke; emitting it escaped would hide that. */
        w->overflow = true;
        return;
    }
    jw_raw(w, "\"");
    jw_raw(w, text);
    jw_raw(w, "\"");
}

/*
 * Emit a string known to consist only of lowercase hex digits.
 *
 * Separate from jw_token() because it accepts a character class jw_token()
 * deliberately refuses: an empty string. A hidden network's SSID is empty, and its
 * hex form is "", which is a legal JSON string. Routing it through jw_token()
 * would have failed the whole document for a perfectly ordinary access point.
 */
static void jw_hex_string(json_writer_t *w, const char *hex)
{
    if (hex == NULL) {
        w->overflow = true;
        return;
    }
    for (size_t i = 0u; hex[i] != '\0'; ++i) {
        char c = hex[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            w->overflow = true;
            return;
        }
    }
    jw_raw(w, "\"");
    jw_raw(w, hex);
    jw_raw(w, "\"");
}

size_t app_portal_hex_encode(const uint8_t *data, size_t length, char *out,
                             size_t out_size)
{
    static const char digits[] = "0123456789abcdef";

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    if (length * 2u + 1u > out_size) {
        out[0] = '\0';
        return 0u;
    }
    for (size_t i = 0u; i < length; ++i) {
        out[i * 2u] = digits[(data[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = digits[data[i] & 0x0Fu];
    }
    out[length * 2u] = '\0';
    return length * 2u;
}

size_t app_portal_build_status_json(const app_portal_status_t *status, char *out,
                                    size_t out_size)
{
    char staging[APP_PORTAL_STATUS_MAX];
    json_writer_t w;

    if (status == NULL || out == NULL || out_size == 0u) {
        return 0u;
    }
    jw_init(&w, out, out_size, staging);

    jw_raw(&w, "{\"active\":");
    jw_raw(&w, status->active ? "true" : "false");
    jw_raw(&w, ",\"ap_ssid\":");
    jw_token(&w, status->ap_ssid);
    jw_raw(&w, ",\"ap_ipv4\":");
    jw_token(&w, status->ap_ipv4);
    jw_raw(&w, ",\"sta_state\":");
    jw_token(&w, status->sta_state != NULL ? status->sta_state : "unknown");
    jw_raw(&w, ",\"sta_ssid\":");
    jw_token(&w, status->sta_ssid);
    jw_raw(&w, ",\"sta_ipv4\":");
    jw_token(&w, status->sta_ipv4);
    jw_raw(&w, ",\"db_state\":");
    jw_token(&w, status->db_state != NULL ? status->db_state : "closed");
    jw_raw(&w, ",\"db_version\":");
    jw_u64(&w, status->db_version);
    jw_raw(&w, ",\"db_profiles\":");
    jw_u64(&w, status->db_profiles);
    jw_raw(&w, ",\"firmware\":");
    jw_token(&w, status->firmware != NULL ? status->firmware : "unknown");
    jw_raw(&w, ",\"upload_active\":");
    jw_raw(&w, status->upload_active ? "true" : "false");
    jw_raw(&w, ",\"upload_received\":");
    jw_u64(&w, status->upload_received);
    jw_raw(&w, ",\"upload_total\":");
    jw_u64(&w, status->upload_total);
    jw_raw(&w, ",\"upload_phase\":");
    jw_token(&w, status->upload_phase != NULL ? status->upload_phase : "idle");
    /* No password field exists in this document, by construction: there is nothing
     * to omit because nothing was ever accepted here. */
    jw_raw(&w, "}");

    return jw_flush(&w);
}

size_t app_portal_build_scan_json(const app_portal_ap_t *aps, size_t count,
                                  char *out, size_t out_size)
{
    char staging[APP_PORTAL_JSON_MAX];
    json_writer_t w;
    char hex[APP_PORTAL_SSID_MAX * 2u + 1u];

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    jw_init(&w, out, out_size, staging);
    jw_raw(&w, "[");

    for (size_t i = 0u; i < count && i < APP_PORTAL_WIFI_SCAN_MAX; ++i) {
        if (i > 0u) {
            jw_raw(&w, ",");
        }
        if (app_portal_hex_encode(aps[i].ssid, aps[i].ssid_len, hex,
                                  sizeof(hex)) == 0u &&
            aps[i].ssid_len > 0u) {
            w.overflow = true;
            break;
        }
        jw_raw(&w, "{\"ssid_hex\":");
        jw_hex_string(&w, hex);
        jw_raw(&w, ",\"ssid_len\":");
        jw_u64(&w, aps[i].ssid_len);
        jw_raw(&w, ",\"rssi\":");
        jw_i32(&w, aps[i].rssi);
        jw_raw(&w, ",\"channel\":");
        jw_u64(&w, aps[i].channel);
        jw_raw(&w, ",\"auth\":");
        jw_u64(&w, aps[i].auth_mode);
        jw_raw(&w, ",\"hidden\":");
        jw_raw(&w, aps[i].hidden ? "true" : "false");
        jw_raw(&w, "}");
    }

    jw_raw(&w, "]");
    return jw_flush(&w);
}

size_t app_portal_build_result_json(bool ok, const char *error, char *out,
                                    size_t out_size)
{
    char staging[128];
    json_writer_t w;

    if (out == NULL || out_size == 0u) {
        return 0u;
    }
    jw_init(&w, out, out_size, staging);
    jw_raw(&w, "{\"ok\":");
    jw_raw(&w, ok ? "true" : "false");
    if (!ok) {
        jw_raw(&w, ",\"error\":");
        jw_token(&w, error != NULL ? error : "unknown");
    }
    jw_raw(&w, "}");
    return jw_flush(&w);
}

/* ---------------- the temporary AP credential ---------------- */

static const char *const k_ap_words[APP_PORTAL_AP_WORDS] = {
    "otter",   "lantern", "cedar",   "harbor",  "pebble",  "willow",
    "copper",  "thistle", "meadow",  "quartz",  "falcon",  "juniper",
    "almond",  "bramble", "cobalt",  "dune",    "ember",   "fennel",
    "ginger",  "hollow",  "indigo",  "kestrel", "lichen",  "marble",
    "nettle",  "orchid",  "pumice",  "quill",   "rowan",   "saffron",
    "tundra",  "umber",
};

const char *app_portal_ap_word(size_t index)
{
    return k_ap_words[index % APP_PORTAL_AP_WORDS];
}

bool app_portal_generate_ap_password(uint32_t (*random_u32)(void), char *out,
                                     size_t out_size)
{
    char scratch[APP_PORTAL_AP_PASSWORD_MAX];
    int written;

    if (random_u32 == NULL || out == NULL || out_size == 0u) {
        return false;
    }

    /*
     * Three words and two two-digit numbers. The words come from a fixed list and
     * the numbers from the injected source, so the entropy is
     * 3*log2(32) + 2*log2(100) ~= 28.3 bits. That is deliberately modest and it is
     * not the security boundary: the AP exists for one provisioning session, on a
     * device the operator is holding, and the session ends when the portal closes.
     * What matters more is that the password is never a fixed value and never
     * derived from anything guessable such as the MAC address.
     */
    written = snprintf(scratch, sizeof(scratch), "%s-%s-%s-%02u%02u",
                       app_portal_ap_word((size_t)(random_u32() %
                                                   APP_PORTAL_AP_WORDS)),
                       app_portal_ap_word((size_t)(random_u32() %
                                                   APP_PORTAL_AP_WORDS)),
                       app_portal_ap_word((size_t)(random_u32() %
                                                   APP_PORTAL_AP_WORDS)),
                       (unsigned)(random_u32() % 100u),
                       (unsigned)(random_u32() % 100u));
    if (written < 0 || (size_t)written >= sizeof(scratch)) {
        return false;
    }
    if ((size_t)written >= out_size) {
        /* Refused rather than truncated: a truncated password is one the operator
         * cannot type, and it would be reported as if it were usable. */
        out[0] = '\0';
        return false;
    }
    memcpy(out, scratch, (size_t)written + 1u);
    return true;
}
