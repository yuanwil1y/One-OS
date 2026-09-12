/*
 * Host regression tests for the provisioning portal's wire layer.
 *
 * app_portal.c is the part of the HTTP portal that decides what a request MEANS and
 * what a response REVEALS. Both are security-relevant and neither needs a socket,
 * so both are tested here:
 *
 *   - form decoding, with every bound enforced before the value is used: an
 *     over-long SSID is refused rather than truncated into a different network, a
 *     malformed percent-escape fails the parse instead of silently becoming a
 *     literal, and control characters never reach a credential;
 *   - Content-Length parsing, including the values a hostile client sends
 *     ("-1", "1e9", "", " 12 ", a number that would overflow);
 *   - the JSON documents, where the property that matters is what is NOT in them:
 *     there is no password field, and an SSID is emitted as hex so a non-printable
 *     or non-UTF-8 SSID cannot produce invalid JSON or inject a terminator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_portal.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* A deterministic stand-in for esp_random(), so a password test is reproducible. */
static uint32_t g_rand_state = 1u;

static uint32_t fake_random(void)
{
    /* xorshift32: good enough to exercise the generator's structure, and identical
     * on every run so a failure can be reproduced exactly. */
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return g_rand_state;
}

/* ---------------- form parsing ---------------- */

static void test_form_basic(void)
{
    app_portal_wifi_form_t form;

    CHECK(app_portal_parse_wifi_form("ssid=HomeWiFi&password=hunter2",
                                     strlen("ssid=HomeWiFi&password=hunter2"),
                                     &form) == APP_PORTAL_FORM_OK,
          "a plain form parses");
    CHECK(strcmp(form.ssid, "HomeWiFi") == 0, "ssid, got '%s'", form.ssid);
    CHECK(strcmp(form.password, "hunter2") == 0, "password, got '%s'", form.password);
    CHECK(form.password_present, "password present");

    /* '+' is a space in form encoding. This is what a browser sends for an SSID
     * with a space in it, so getting it wrong silently connects to nothing. */
    CHECK(app_portal_parse_wifi_form("ssid=Cafe+WiFi", strlen("ssid=Cafe+WiFi"),
                                     &form) == APP_PORTAL_FORM_OK,
          "plus decodes");
    CHECK(strcmp(form.ssid, "Cafe WiFi") == 0, "plus is a space, got '%s'", form.ssid);

    /* Percent escapes, including one that decodes to '&' - which must not be
     * treated as a field separator. */
    CHECK(app_portal_parse_wifi_form("ssid=A%26B%20C", strlen("ssid=A%26B%20C"),
                                     &form) == APP_PORTAL_FORM_OK,
          "escaped ampersand is part of the value");
    CHECK(strcmp(form.ssid, "A&B C") == 0, "decoded value, got '%s'", form.ssid);

    /* Field order does not matter, and unknown fields are ignored. */
    CHECK(app_portal_parse_wifi_form("extra=1&ssid=X&other=2&password=p",
                                     strlen("extra=1&ssid=X&other=2&password=p"),
                                     &form) == APP_PORTAL_FORM_OK,
          "unknown fields are ignored");
    CHECK(strcmp(form.ssid, "X") == 0, "ssid found after other fields");
    CHECK(strcmp(form.password, "p") == 0, "password found after other fields");

    /* A field whose name merely ends with the key must not match. */
    CHECK(app_portal_parse_wifi_form("xssid=evil", strlen("xssid=evil"), &form) ==
              APP_PORTAL_FORM_MISSING_SSID,
          "a suffixed field name does not match");
}

static void test_form_refuses_what_it_cannot_represent(void)
{
    app_portal_wifi_form_t form;
    char long_ssid[128];
    char long_password[256];

    /* Missing SSID. */
    CHECK(app_portal_parse_wifi_form("password=x", strlen("password=x"), &form) ==
              APP_PORTAL_FORM_MISSING_SSID,
          "no ssid is a refusal");
    CHECK(app_portal_parse_wifi_form("ssid=", strlen("ssid="), &form) ==
              APP_PORTAL_FORM_MISSING_SSID,
          "an empty ssid is a refusal");
    CHECK(app_portal_parse_wifi_form("", 0u, &form) == APP_PORTAL_FORM_MISSING_SSID,
          "an empty body is a refusal");

    /* An SSID longer than 802.11 allows. Truncating it would silently target a
     * different network, so it is refused. */
    memset(long_ssid, 'a', sizeof(long_ssid));
    long_ssid[0] = '\0';
    (void)snprintf(long_ssid, sizeof(long_ssid), "ssid=%s", "0123456789012345678901234567890123456789");
    CHECK(app_portal_parse_wifi_form(long_ssid, strlen(long_ssid), &form) ==
              APP_PORTAL_FORM_SSID_TOO_LONG,
          "a 40-byte ssid is refused");

    /* An SSID with a control character: legal on the air, hostile in a log. */
    CHECK(app_portal_parse_wifi_form("ssid=bad%0Aname", strlen("ssid=bad%0Aname"),
                                     &form) == APP_PORTAL_FORM_SSID_INVALID,
          "a newline in an ssid is refused");
    CHECK(app_portal_parse_wifi_form("ssid=bad%00name", strlen("ssid=bad%00name"),
                                     &form) == APP_PORTAL_FORM_SSID_INVALID,
          "a NUL in an ssid is refused");

    /* A password longer than the driver accepts. */
    memset(long_password, 'p', sizeof(long_password));
    long_password[0] = '\0';
    (void)snprintf(long_password, sizeof(long_password),
                   "ssid=X&password=%s",
                   "0123456789012345678901234567890123456789012345678901234567890123456789");
    CHECK(app_portal_parse_wifi_form(long_password, strlen(long_password), &form) ==
              APP_PORTAL_FORM_PASSWORD_TOO_LONG,
          "an over-long password is refused");

    /* Malformed escapes: refused rather than partially decoded. */
    CHECK(app_portal_parse_wifi_form("ssid=A%2", strlen("ssid=A%2"), &form) ==
              APP_PORTAL_FORM_MALFORMED,
          "a truncated percent escape is refused");
    CHECK(app_portal_parse_wifi_form("ssid=A%ZZ", strlen("ssid=A%ZZ"), &form) ==
              APP_PORTAL_FORM_MALFORMED,
          "a non-hex percent escape is refused");
    CHECK(app_portal_parse_wifi_form("ssid=A&password=p%", strlen("ssid=A&password=p%"),
                                     &form) == APP_PORTAL_FORM_MALFORMED,
          "a malformed escape in the password is refused too");
}

static void test_an_open_network_is_representable(void)
{
    app_portal_wifi_form_t form;

    /* No password field at all. */
    CHECK(app_portal_parse_wifi_form("ssid=OpenNet", strlen("ssid=OpenNet"), &form) ==
              APP_PORTAL_FORM_OK,
          "an ssid with no password parses");
    CHECK(!form.password_present, "and is recorded as an open network");
    CHECK(form.password[0] == '\0', "with an empty password");

    /* An empty password field means the same thing, and is normalised so callers
     * only have one case to handle. */
    CHECK(app_portal_parse_wifi_form("ssid=OpenNet&password=",
                                     strlen("ssid=OpenNet&password="), &form) ==
              APP_PORTAL_FORM_OK,
          "an empty password field parses");
    CHECK(!form.password_present,
          "and is normalised to the same open-network case");
}

/* ---------------- Content-Length ---------------- */

static void test_content_length_parsing(void)
{
    uint64_t length = 0u;

    CHECK(app_portal_parse_content_length("0", &length) && length == 0u,
          "zero is a length");
    CHECK(app_portal_parse_content_length("2672", &length) && length == 2672u,
          "a plain number, got %llu", (unsigned long long)length);
    CHECK(app_portal_parse_content_length("  42  ", &length) && length == 42u,
          "surrounding spaces are legal");
    CHECK(app_portal_parse_content_length("18446744073709551615", &length) &&
              length == UINT64_MAX,
          "the largest 64-bit value is accepted");

    /* Everything a hostile client might send. */
    CHECK(!app_portal_parse_content_length("", &length), "empty is refused");
    CHECK(!app_portal_parse_content_length("-1", &length), "negative is refused");
    CHECK(!app_portal_parse_content_length("+1", &length), "signed is refused");
    CHECK(!app_portal_parse_content_length("1e9", &length), "exponent is refused");
    CHECK(!app_portal_parse_content_length("0x10", &length), "hex is refused");
    CHECK(!app_portal_parse_content_length("12 34", &length),
          "an embedded space is refused");
    CHECK(!app_portal_parse_content_length("18446744073709551616", &length),
          "a value that would overflow is refused rather than wrapped");
    CHECK(!app_portal_parse_content_length("99999999999999999999", &length),
          "an absurd length is refused");
    CHECK(!app_portal_parse_content_length("12a", &length), "trailing junk refused");
    CHECK(!app_portal_parse_content_length(NULL, &length), "NULL is refused");
}

/* ---------------- JSON ---------------- */

static void test_status_document_shape(void)
{
    app_portal_status_t status;
    char out[APP_PORTAL_STATUS_MAX];
    size_t written;

    memset(&status, 0, sizeof(status));
    status.active = true;
    (void)snprintf(status.ap_ssid, sizeof(status.ap_ssid), "NearBy-One-A1B2");
    (void)snprintf(status.ap_ipv4, sizeof(status.ap_ipv4), "192.168.4.1");
    status.sta_state = "connected";
    (void)snprintf(status.sta_ssid, sizeof(status.sta_ssid), "HomeWiFi");
    (void)snprintf(status.sta_ipv4, sizeof(status.sta_ipv4), "192.168.1.42");
    status.db_state = "ready";
    status.db_version = 20260911u;
    status.db_profiles = 5u;
    status.firmware = "One-OS 0.1.0";
    status.upload_phase = "idle";

    written = app_portal_build_status_json(&status, out, sizeof(out));
    CHECK(written > 0u, "the status document was built");
    CHECK(strstr(out, "\"active\":true") != NULL, "active flag: %s", out);
    CHECK(strstr(out, "\"sta_state\":\"connected\"") != NULL, "sta_state present");
    CHECK(strstr(out, "\"db_version\":20260911") != NULL, "db_version present");
    CHECK(strstr(out, "\"db_profiles\":5") != NULL, "db_profiles present");

    /* The property that matters most: there is no field for a secret, so there is
     * nothing to accidentally include. */
    CHECK(strstr(out, "password") == NULL,
          "the status document contains no password field: %s", out);
    CHECK(strstr(out, "psk") == NULL, "and no key field: %s", out);

    /* A buffer too small must produce NOTHING, not a truncated document a browser
     * would try to parse. The buffer is pre-filled with a sentinel, so "nothing was
     * written" means the sentinel survived rather than that a NUL happened to be
     * there already. */
    {
        char tiny[32];
        bool untouched = true;

        memset(tiny, 0x5A, sizeof(tiny));
        CHECK(app_portal_build_status_json(&status, tiny, sizeof(tiny)) == 0u,
              "a too-small buffer is refused");
        for (size_t i = 0u; i < sizeof(tiny); ++i) {
            if (tiny[i] != 0x5A) {
                untouched = false;
                break;
            }
        }
        CHECK(untouched, "and the caller's buffer was left completely untouched");
    }
}

/*
 * The AP address is only in the document while the AP is up.
 *
 * wifi_mgr_ap_ipv4() answers ESP_ERR_INVALID_STATE once the AP is down and the
 * caller then leaves the field empty, but the builder is what an operator reads:
 * a status document that showed an AP address while `active` was false would have
 * a browser - and a person following the hardware checklist - look for a captive
 * portal that is not running. The pair of fields is pinned together here rather
 * than trusted to the caller.
 */
static void test_status_ap_address_follows_the_ap(void)
{
    app_portal_status_t status;
    char out[APP_PORTAL_STATUS_MAX];

    /* Down: both fields empty even if the caller left stale text in the struct. */
    memset(&status, 0, sizeof(status));
    status.active = false;
    (void)snprintf(status.ap_ssid, sizeof(status.ap_ssid), "NearBy-One-A1B2");
    (void)snprintf(status.ap_ipv4, sizeof(status.ap_ipv4), "192.168.4.1");
    status.sta_state = "disconnected";
    status.db_state = "ready";
    status.firmware = "One-OS 0.1.0";
    status.upload_phase = "idle";

    CHECK(app_portal_build_status_json(&status, out, sizeof(out)) > 0u,
          "the status document was built");
    CHECK(strstr(out, "\"active\":false") != NULL, "active is false: %s", out);
    /* The builder copies what it is given, so the honest reading is: the caller is
     * responsible for clearing these, and the test records which one it is. */
    CHECK(strstr(out, "\"ap_ipv4\":\"\"") != NULL,
          "an AP that is down has no address in the document, got: %s", out);
}

/*
 * No secret can reach the status document, checked against a password that was
 * actually generated rather than against the word "password".
 *
 * test_status_document_shape() checks for the field NAME; this checks for the
 * VALUE, which is the property that matters if someone later adds a field and
 * passes the wrong buffer to it.
 */
static void test_status_carries_no_generated_secret(void)
{
    app_portal_status_t status;
    app_portal_wifi_form_t form;
    char ap_password[APP_PORTAL_AP_PASSWORD_MAX];
    char out[APP_PORTAL_STATUS_MAX];

    CHECK(app_portal_generate_ap_password(fake_random, ap_password, sizeof(ap_password)),
          "an AP password was generated");
    CHECK(ap_password[0] != '\0', "and it is not empty");

    memset(&status, 0, sizeof(status));
    status.active = true;
    (void)snprintf(status.ap_ssid, sizeof(status.ap_ssid), "NearBy-One-A1B2");
    (void)snprintf(status.sta_ssid, sizeof(status.sta_ssid), "HomeWiFi");
    status.sta_state = "connected";
    status.db_state = "ready";
    status.firmware = "One-OS 0.1.0";
    status.upload_phase = "idle";

    CHECK(app_portal_build_status_json(&status, out, sizeof(out)) > 0u, "status built");
    CHECK(strstr(out, ap_password) == NULL,
          "the generated AP password is not in the status document: %s", out);

    /* And the station credential an operator typed into the form must not appear
     * either - the portal receives it, the status document never echoes it. */
    CHECK(app_portal_parse_wifi_form("ssid=HomeWiFi&password=s3cr3t-sta",
                                     strlen("ssid=HomeWiFi&password=s3cr3t-sta"),
                                     &form) == APP_PORTAL_FORM_OK,
          "the form parsed");
    CHECK(strstr(out, form.password) == NULL,
          "the station password is not in the status document: %s", out);
}

static void test_status_refuses_unsafe_text_rather_than_escaping(void)
{
    app_portal_status_t status;
    char out[APP_PORTAL_STATUS_MAX];

    memset(&status, 0, sizeof(status));
    status.sta_state = "ok\"},"; /* a value that would break the document */

    CHECK(app_portal_build_status_json(&status, out, sizeof(out)) == 0u,
          "an unsafe token fails the whole document");
}

static void test_scan_document_handles_hostile_ssids(void)
{
    app_portal_ap_t aps[3];
    char out[APP_PORTAL_JSON_MAX];
    size_t written;

    memset(aps, 0, sizeof(aps));

    /* A perfectly ordinary network. */
    memcpy(aps[0].ssid, "HomeWiFi", 8u);
    aps[0].ssid_len = 8u;
    aps[0].rssi = -54;
    aps[0].channel = 6u;
    aps[0].auth_mode = 3u;

    /* A hidden network: an empty SSID, which is legal and must not fail the
     * document. */
    aps[1].ssid_len = 0u;
    aps[1].rssi = -70;
    aps[1].channel = 1u;
    aps[1].hidden = true;

    /* An SSID that is not printable text at all: quotes, backslashes, a newline
     * and a high byte. As hex it cannot break the document. */
    aps[2].ssid[0] = '"';
    aps[2].ssid[1] = '\\';
    aps[2].ssid[2] = '\n';
    aps[2].ssid[3] = 0xFFu;
    aps[2].ssid_len = 4u;
    aps[2].rssi = -80;
    aps[2].channel = 11u;

    written = app_portal_build_scan_json(aps, 3u, out, sizeof(out));
    CHECK(written > 0u, "the scan document was built");

    CHECK(strstr(out, "\"ssid_hex\":\"486f6d6557694669\"") != NULL,
          "HomeWiFi as hex: %s", out);
    CHECK(strstr(out, "\"hidden\":true") != NULL, "the hidden network is marked");
    CHECK(strstr(out, "\"ssid_hex\":\"\"") != NULL,
          "and its empty SSID is a legal empty hex string");
    CHECK(strstr(out, "\"ssid_hex\":\"225c0aff\"") != NULL,
          "the hostile SSID is hex, so it cannot break the document: %s", out);

    /* The document contains no raw control character or quote from the SSID. */
    CHECK(strchr(out, '\n') == NULL, "no raw newline in the document");
    CHECK(strstr(out, "\\\\") == NULL, "no unescaped backslash was emitted");
}

static void test_scan_document_is_bounded(void)
{
    app_portal_ap_t aps[APP_PORTAL_WIFI_SCAN_MAX + 4u];
    char out[APP_PORTAL_JSON_MAX];
    size_t written;

    memset(aps, 0, sizeof(aps));
    for (size_t i = 0u; i < sizeof(aps) / sizeof(aps[0]); ++i) {
        aps[i].ssid_len = 1u;
        aps[i].ssid[0] = (uint8_t)('a' + (i % 26u));
    }

    written = app_portal_build_scan_json(aps, sizeof(aps) / sizeof(aps[0]), out,
                                         sizeof(out));
    CHECK(written > 0u, "an over-long list still builds");
    {
        /* Only the first APP_PORTAL_WIFI_SCAN_MAX entries may appear, so a hostile
         * or simply crowded RF environment cannot make the response unbounded. */
        unsigned entries = 0u;

        for (const char *p = out; (p = strstr(p, "ssid_hex")) != NULL; ++p) {
            entries++;
        }
        CHECK(entries == APP_PORTAL_WIFI_SCAN_MAX,
              "exactly %u entries, got %u", (unsigned)APP_PORTAL_WIFI_SCAN_MAX,
              entries);
    }
}

static void test_result_document(void)
{
    char out[128];

    CHECK(app_portal_build_result_json(true, NULL, out, sizeof(out)) > 0u,
          "a success document builds");
    CHECK(strcmp(out, "{\"ok\":true}") == 0, "and is minimal, got '%s'", out);

    CHECK(app_portal_build_result_json(false, "invalid_database", out,
                                       sizeof(out)) > 0u,
          "a failure document builds");
    CHECK(strcmp(out, "{\"ok\":false,\"error\":\"invalid_database\"}") == 0,
          "with the reason, got '%s'", out);

    /* A caller-supplied reason that is not a safe token cannot inject anything. */
    CHECK(app_portal_build_result_json(false, "bad\",\"x\":\"y", out,
                                       sizeof(out)) == 0u,
          "an unsafe reason is refused rather than escaped");
}

/* ---------------- AP password ---------------- */

static void test_ap_password_shape(void)
{
    char password[APP_PORTAL_AP_PASSWORD_MAX];
    char first[APP_PORTAL_AP_PASSWORD_MAX];
    int seen_different = 0;

    g_rand_state = 12345u;
    CHECK(app_portal_generate_ap_password(fake_random, password, sizeof(password)),
          "a password was generated");
    (void)snprintf(first, sizeof(first), "%s", password);

    /* Three word groups and two two-digit numbers: readable enough to type from a
     * screen, and long enough not to be guessable. */
    {
        int hyphens = 0;
        size_t digits = 0u;

        for (size_t i = 0u; password[i] != '\0'; ++i) {
            if (password[i] == '-') {
                hyphens++;
            } else if (password[i] >= '0' && password[i] <= '9') {
                digits++;
            }
        }
        CHECK(hyphens == APP_PORTAL_AP_PASSWORD_WORDS,
              "three words means three hyphens, got %d in '%s'", hyphens, password);
        CHECK(digits == APP_PORTAL_AP_NUMBERS * 2u,
              "two two-digit numbers means four digits, got %u in '%s'",
              (unsigned)digits, password);
        CHECK(strlen(password) <= APP_PORTAL_AP_WORD_MAX * APP_PORTAL_AP_PASSWORD_WORDS +
                                       APP_PORTAL_AP_PASSWORD_WORDS +
                                       APP_PORTAL_AP_NUMBERS * 2u,
              "the password fits the documented worst case, got %u bytes",
              (unsigned)strlen(password));
    }

    /* It is not a fixed value: repeated generation must produce different results,
     * or every device in range would share one password. */
    for (int i = 0; i < 8; ++i) {
        char next[APP_PORTAL_AP_PASSWORD_MAX];

        CHECK(app_portal_generate_ap_password(fake_random, next, sizeof(next)),
              "generation %d succeeded", i);
        if (strcmp(next, first) != 0) {
            seen_different = 1;
        }
    }
    CHECK(seen_different, "the password is not constant");

    /* A buffer that cannot hold the whole password is refused, not truncated: a
     * truncated password is one the operator cannot type. */
    {
        char tiny[8];

        memset(tiny, 0x5A, sizeof(tiny));
        CHECK(!app_portal_generate_ap_password(fake_random, tiny, sizeof(tiny)),
              "a too-small buffer is refused");
        CHECK(tiny[0] == '\0', "and nothing was written");
    }
    CHECK(!app_portal_generate_ap_password(NULL, password, sizeof(password)),
          "no random source is a refusal");
}

static void test_hex_encoding_bounds(void)
{
    const uint8_t data[3] = {0x00u, 0xABu, 0xFFu};
    char out[16];

    CHECK(app_portal_hex_encode(data, 3u, out, sizeof(out)) == 6u,
          "six characters for three bytes");
    CHECK(strcmp(out, "00abff") == 0, "lowercase hex, got '%s'", out);

    /* Zero bytes is a legal empty string, which is what a hidden SSID produces. */
    CHECK(app_portal_hex_encode(data, 0u, out, sizeof(out)) == 0u,
          "an empty input writes an empty string");
    CHECK(out[0] == '\0', "and terminates it");

    /* One byte too few in the buffer is a refusal, not a truncated encoding. */
    {
        char tiny[6];

        memset(tiny, 0x5A, sizeof(tiny));
        CHECK(app_portal_hex_encode(data, 3u, tiny, sizeof(tiny)) == 0u,
              "a too-small buffer is refused");
        CHECK(tiny[0] == '\0', "and cleared");
    }
}

static void test_json_token_safety(void)
{
    CHECK(app_portal_json_token_is_safe("ready"), "a plain word is safe");
    CHECK(app_portal_json_token_is_safe("One-OS 0.1.0"), "spaces and dots are safe");
    CHECK(!app_portal_json_token_is_safe("a\"b"), "a quote is not");
    CHECK(!app_portal_json_token_is_safe("a\\b"), "a backslash is not");
    CHECK(!app_portal_json_token_is_safe("a\nb"), "a newline is not");
    /* Split so the hex escape cannot swallow the following 'b'. */
    CHECK(!app_portal_json_token_is_safe("a\x7f" "b"), "DEL is not");
    CHECK(!app_portal_json_token_is_safe(NULL), "NULL is not");
    CHECK(app_portal_json_token_is_safe(""), "an empty token is trivially safe");
}

int main(void)
{
    test_form_basic();
    test_form_refuses_what_it_cannot_represent();
    test_an_open_network_is_representable();
    test_content_length_parsing();

    test_status_document_shape();
    test_status_ap_address_follows_the_ap();
    test_status_carries_no_generated_secret();
    test_status_refuses_unsafe_text_rather_than_escaping();
    test_scan_document_handles_hostile_ssids();
    test_scan_document_is_bounded();
    test_result_document();

    test_ap_password_shape();
    test_hex_encoding_bounds();
    test_json_token_safety();

    printf("app_portal: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
