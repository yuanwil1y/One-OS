#pragma once

/*
 * Provisioning portal: the wire layer.
 *
 * Two jobs, both of which are pure data handling and therefore testable on the
 * host without sockets:
 *
 *   1. parsing what a browser sends - form-encoded Wi-Fi credentials and the
 *      Content-Length of an upload - with every bound enforced before the value is
 *      used;
 *   2. building the JSON the portal reads, from bounded snapshots, with escaping
 *      and hex encoding fixed in one place instead of at each call site.
 *
 * The reason this is not done inline in the HTTP handlers is the product rule that
 * the portal is only a front end: diagnostics, HTTP and the future GUI must all
 * reach the same business logic. Keeping the request/response shape here means the
 * handler is thin, and it means hostile input is exercised by tests rather than
 * discovered on a board.
 *
 * SECRETS: nothing in this file logs, echoes or stores a password. An SSID is
 * reported as hex (`ssid_hex`), which is a presentation choice the old portal also
 * made, not an encoding of a secret - an SSID is broadcast. A Wi-Fi password never
 * leaves app_provision.c, and the temporary AP password is only ever printed by the
 * local serial console.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_device_db.h"
#include "app_wifi.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded response sizes. The portal reads small JSON documents; anything larger
 * is a bug in the caller, not a reason to allocate. */
#define APP_PORTAL_STATUS_MAX 512u
#define APP_PORTAL_WIFI_SCAN_MAX 32u
#define APP_PORTAL_SSID_MAX 32u
#define APP_PORTAL_PASSWORD_MAX 64u
#define APP_PORTAL_JSON_MAX 4096u

/* ---------------- request parsing ---------------- */

typedef struct {
    char ssid[WIFI_MGR_SSID_STORE];
    char password[WIFI_MGR_PASSWORD_STORE];
    bool password_present; /* an open network sends no password field at all */
} app_portal_wifi_form_t;

typedef enum {
    APP_PORTAL_FORM_OK = 0,
    APP_PORTAL_FORM_MISSING_SSID,
    APP_PORTAL_FORM_SSID_TOO_LONG,
    APP_PORTAL_FORM_PASSWORD_TOO_LONG,
    APP_PORTAL_FORM_MALFORMED,   /* bad percent-encoding or a stray '&' */
    APP_PORTAL_FORM_NOT_FORM,    /* body is not form-encoded */
    APP_PORTAL_FORM_SSID_INVALID /* control characters: not a real SSID */
} app_portal_form_status_t;

const char *app_portal_form_status_name(app_portal_form_status_t status);

/*
 * Parse an application/x-www-form-urlencoded body into Wi-Fi credentials.
 *
 * Enforced here rather than by the caller:
 *   - `%XX` decoding, and a malformed escape is a refusal, not a literal;
 *   - the SSID must be 1..32 bytes (the 802.11 limit) and may not contain control
 *     characters, so a credential cannot inject anything into a log or a status
 *     document;
 *   - the password may not exceed 64 bytes;
 *   - a missing password field is legal and means "open network"; an empty one
 *     means the same;
 *   - unknown fields are ignored, so a newer browser form does not break an older
 *     device.
 *
 * `body` is not required to be NUL-terminated. A '+' decodes to a space, which is
 * what form encoding means and what a browser sends for an SSID containing spaces.
 */
app_portal_form_status_t app_portal_parse_wifi_form(const char *body, size_t length,
                                                    app_portal_wifi_form_t *out);

/*
 * Parse a Content-Length header value.
 *
 * Returns false for an empty value, a non-digit, a negative number, or one that
 * does not fit in 64 bits. A missing header is the caller's business: this only
 * answers "what does this header say", never "how big may an upload be".
 */
bool app_portal_parse_content_length(const char *value, uint64_t *out_length);

/* ---------------- response building ---------------- */

/* One scanned network. The SSID is carried as bytes plus a length, because an SSID
 * is a byte string: it may be empty (a hidden network) and it may not be printable. */
typedef struct {
    uint8_t ssid[APP_PORTAL_SSID_MAX];
    uint8_t ssid_len;
    int8_t rssi;
    uint8_t channel;
    uint8_t auth_mode; /* wifi_auth_mode_t, as a plain byte */
    bool hidden;
} app_portal_ap_t;

/* Everything /api/status is allowed to reveal. There is deliberately no field for
 * a password, and `ap_password_available` only says that the local operator can
 * read it from the serial console - not what it is. */
typedef struct {
    bool active;
    char ap_ssid[WIFI_MGR_SSID_STORE];
    char ap_ipv4[WIFI_MGR_IPV4_MAX];
    const char *sta_state;
    char sta_ssid[WIFI_MGR_SSID_STORE];
    char sta_ipv4[WIFI_MGR_IPV4_MAX];
    const char *db_state;
    uint32_t db_version;
    uint32_t db_profiles;
    const char *firmware;
    /* Progress of an upload in flight, so the browser can show it honestly. */
    bool upload_active;
    uint64_t upload_received;
    uint64_t upload_total;
    const char *upload_phase;
} app_portal_status_t;

/*
 * Build the /api/status document.
 *
 * `ap_ssid` and `ap_ipv4` are emitted only when `active` is true; with the AP down
 * they are reported as empty strings whatever the caller put in them, so a browser
 * (and a person following the hardware checklist) is never sent looking for a
 * captive portal that is not running.
 *
 * Returns the written length, or 0 when the buffer is too small - in which case
 * nothing is written and the caller must not send a truncated document to a
 * browser that will try to parse it.
 */
size_t app_portal_build_status_json(const app_portal_status_t *status, char *out,
                                    size_t out_size);

/*
 * Build the /api/wifi/scan document from a bounded list.
 *
 * Every SSID is emitted as lowercase hex, so a non-printable or non-UTF-8 SSID
 * cannot produce invalid JSON and cannot inject a document terminator. `hidden`
 * marks a network whose SSID was empty in the beacon.
 */
size_t app_portal_build_scan_json(const app_portal_ap_t *aps, size_t count,
                                  char *out, size_t out_size);

/* A small JSON result document: {"ok":true} or {"ok":false,"error":"..."}. Both
 * strings are emitted by this function, so no caller-supplied text is ever written
 * into a document unescaped. */
size_t app_portal_build_result_json(bool ok, const char *error, char *out,
                                    size_t out_size);

/*
 * Is this string safe to place inside a JSON string literal as-is?
 *
 * Used by the builders above for the values that come from elsewhere (a firmware
 * version, a Wi-Fi state name). Refusing is the right answer rather than escaping:
 * every one of those values is one of a small fixed set, so a value that needs
 * escaping means something upstream produced text it should not have.
 */
bool app_portal_json_token_is_safe(const char *text);

/* Lowercase hex of `length` bytes. Returns the written length, or 0 when
 * `out_size` cannot hold two digits per byte plus a terminator. */
size_t app_portal_hex_encode(const uint8_t *data, size_t length, char *out,
                             size_t out_size);

/* ---------------- the temporary AP credential ---------------- */

/*
 * Wordlist for the human-readable temporary AP password.
 *
 * The password is shown on the device's own screen and printed once to the local
 * serial console, so it has to be something a person can read and type. Short
 * lowercase words joined by hyphens give that without a weak secret: the entropy
 * comes from the number of words and from the injected random source, not from the
 * alphabet.
 */
#define APP_PORTAL_AP_WORDS 32u
#define APP_PORTAL_AP_WORD_MAX 12u
/*
 * Worst case: three 12-character words, three hyphens, two two-digit numbers and a
 * terminator = 44 bytes. 48 leaves room without inviting a longer word list to
 * silently overflow the buffer.
 */
#define APP_PORTAL_AP_PASSWORD_MAX 48u
#define APP_PORTAL_AP_PASSWORD_WORDS 3u
#define APP_PORTAL_AP_NUMBERS 2u

const char *app_portal_ap_word(size_t index);

/*
 * Fill in the AP password.
 *
 * `random_u32` is the only source of entropy and is injected, so the host tests can
 * drive it deterministically. Returns false when the buffer cannot hold the result,
 * rather than emitting a truncated password the operator could not type.
 *
 * Format: three words and two digits, e.g. "otter-lantern-cedar-47".
 */
bool app_portal_generate_ap_password(uint32_t (*random_u32)(void), char *out,
                                     size_t out_size);

/* ---------------- native side (ESP-IDF only) ---------------- */

/*
 * Bounded active scan for the portal's network list.
 *
 * Defined in app_portal_native.c because it needs the Wi-Fi driver. Returns false when a
 * scan cannot run at all - most importantly while the portal's own AP holds the radio -
 * so the caller reports "unavailable" rather than an empty list the operator would read
 * as "no networks in range". Returns true with `*out_count == 0` for a genuinely empty
 * result.
 */
bool app_portal_native_wifi_scan(app_portal_ap_t *out, size_t capacity,
                                 size_t *out_count);

/*
 * Fill the parts of /api/status that come from the runtime and the Wi-Fi manager: the
 * station state and address, the AP address, the firmware string, and the recognition
 * database state and version.
 *
 * Called after app_provision_status_snapshot(), which fills the session's own fields;
 * this supplies what the session has no way to know.
 */
void app_portal_native_status_fill(void *ctx, app_portal_status_t *out);

#ifdef __cplusplus
}
#endif
