/*
 * Provisioning session. See app_provision.h for the contract and for why the
 * ordering it enforces is the point of the module.
 *
 * Platform independent: radio, server, storage and clock all arrive through
 * app_provision_ops_t, so the host tests drive the real state machine - including
 * every rollback path - without a board.
 */

#include "app_provision.h"

#include "app_str.h"

#include <stdio.h>
#include <string.h>

const char *app_provision_phase_name(app_provision_phase_t phase)
{
    switch (phase) {
    case APP_PROVISION_IDLE:    return "idle";
    case APP_PROVISION_STARTING:return "starting";
    case APP_PROVISION_ACTIVE:  return "active";
    case APP_PROVISION_STOPPING:return "stopping";
    case APP_PROVISION_STOPPED: return "stopped";
    case APP_PROVISION_FAILED:  return "failed";
    default:                    return "invalid";
    }
}

const char *app_provision_status_name(app_provision_status_t status)
{
    switch (status) {
    case APP_PROVISION_OK:                 return "ok";
    case APP_PROVISION_ERR_BUSY:           return "busy";
    case APP_PROVISION_ERR_AP_FAILED:      return "ap_failed";
    case APP_PROVISION_ERR_SERVER_FAILED:  return "server_failed";
    case APP_PROVISION_ERR_NOT_ACTIVE:     return "not_active";
    case APP_PROVISION_ERR_NOT_AUTHORIZED: return "not_authorized";
    case APP_PROVISION_ERR_NO_MEDIUM:      return "no_medium";
    case APP_PROVISION_ERR_IMPORT_FAILED:  return "import_failed";
    default:                               return "unknown";
    }
}

void app_provision_config_default(app_provision_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->session_timeout_ms = APP_PROVISION_DEFAULT_TIMEOUT_MS;
    out->server_port = APP_PROVISION_DEFAULT_PORT;
    (void)app_strlcpy(out->ap_ssid_prefix, "NearBy-One-", sizeof(out->ap_ssid_prefix));
}

esp_err_t app_provision_init(app_provision_t *session,
                             const app_provision_ops_t *ops, void *ops_ctx,
                             app_ops_t *gate,
                             const app_provision_config_t *config)
{
    app_provision_config_t cfg;

    if (session == NULL || ops == NULL || gate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ops->ap_start == NULL || ops->ap_stop == NULL || ops->sta_release == NULL ||
        ops->sta_restore == NULL || ops->server_start == NULL ||
        ops->server_stop == NULL || ops->db_close == NULL || ops->db_reopen == NULL ||
        ops->now_ms == NULL || ops->random_u32 == NULL) {
        /* Every one of these is required. A partially wired session would silently
         * skip a rollback step, which is exactly the failure this module exists to
         * prevent. */
        return ESP_ERR_INVALID_ARG;
    }

    memset(session, 0, sizeof(*session));
    session->ops = *ops;
    session->ops_ctx = ops_ctx;
    session->gate = gate;
    session->phase = APP_PROVISION_IDLE;

    if (config != NULL) {
        cfg = *config;
    } else {
        app_provision_config_default(&cfg);
    }
    if (cfg.session_timeout_ms == 0u) {
        cfg.session_timeout_ms = APP_PROVISION_DEFAULT_TIMEOUT_MS;
    }
    if (cfg.server_port == 0u) {
        cfg.server_port = APP_PROVISION_DEFAULT_PORT;
    }
    if (cfg.ap_ssid_prefix[0] == '\0') {
        (void)app_strlcpy(cfg.ap_ssid_prefix, "NearBy-One-",
                          sizeof(cfg.ap_ssid_prefix));
    }
    session->config = cfg;
    return ESP_OK;
}

/*
 * Build the AP SSID and the session token.
 *
 * The SSID ends in four hex digits from the random source. Deriving them from the
 * MAC would be the obvious choice and is deliberately not done: a device whose SSID
 * is its MAC address is trackable by anything listening, and the board's MAC is
 * printed on the case. The suffix only has to be unique enough to pick the right
 * device out of a list.
 */
static bool generate_credentials(app_provision_t *session)
{
    uint32_t suffix;
    uint32_t token_hi;
    uint32_t token_lo;
    int written;

    suffix = session->ops.random_u32() & 0xFFFFu;
    written = snprintf(session->ap_ssid, sizeof(session->ap_ssid), "%s%04X",
                       session->config.ap_ssid_prefix, (unsigned)suffix);
    if (written < 0 || (size_t)written >= sizeof(session->ap_ssid)) {
        return false;
    }

    if (!app_portal_generate_ap_password(session->ops.random_u32,
                                         session->ap_password,
                                         sizeof(session->ap_password))) {
        return false;
    }

    token_hi = session->ops.random_u32();
    token_lo = session->ops.random_u32();
    written = snprintf(session->token, sizeof(session->token), "%08lx%08lx",
                       (unsigned long)token_hi, (unsigned long)token_lo);
    if (written < 0 || (size_t)written >= sizeof(session->token)) {
        return false;
    }
    /* A token of all zeroes would mean the random source is not random, and an
     * all-zero token is the one guessable value. Refused rather than shipped. */
    if (token_hi == 0u && token_lo == 0u) {
        return false;
    }
    return true;
}

app_provision_status_t app_provision_start(app_provision_t *session)
{
    esp_err_t err;

    if (session == NULL) {
        return APP_PROVISION_ERR_NOT_ACTIVE;
    }
    if (session->phase == APP_PROVISION_ACTIVE ||
        session->phase == APP_PROVISION_STARTING) {
        /* Starting twice would leave the first session's AP running with nothing
         * holding a handle to it. */
        session->status = APP_PROVISION_ERR_BUSY;
        return session->status;
    }

    /*
     * The gate is the application's own mutual exclusion, shared with scanning. The
     * portal therefore cannot run beside a scan, which is required rather than
     * convenient: the session takes the radio and a scan's handover re-initialises
     * the driver, so both at once means the driver changing owner underneath
     * whichever one thought it had it.
     */
    if (app_ops_begin_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT) !=
        APP_OPS_OK) {
        session->phase = APP_PROVISION_IDLE;
        session->status = APP_PROVISION_ERR_BUSY;
        return session->status;
    }

    session->phase = APP_PROVISION_STARTING;
    session->status = APP_PROVISION_OK;
    session->ap_up = false;
    session->server_up = false;
    session->reader_closed = false;
    session->sta_verified = false;

    if (!generate_credentials(session)) {
        /* No usable credential means no session. Rolling back now, before the radio
         * is touched, is what keeps a random-source failure from leaving the device
         * in AP mode with an empty password. */
        (void)app_ops_end_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT);
        session->phase = APP_PROVISION_FAILED;
        session->status = APP_PROVISION_ERR_AP_FAILED;
        return session->status;
    }

    /* The station hands the radio over before the AP is configured, so the AP is
     * never configured on top of a live station. */
    err = session->ops.sta_release(session->ops_ctx);
    if (err != ESP_OK) {
        (void)app_ops_end_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT);
        session->phase = APP_PROVISION_FAILED;
        session->last_io_error = err;
        session->status = APP_PROVISION_ERR_AP_FAILED;
        return session->status;
    }

    err = session->ops.ap_start(session->ops_ctx, session->ap_ssid,
                                session->ap_password);
    if (err != ESP_OK) {
        /* Give the radio straight back: the operator's network must not be left
         * down because the portal could not start. */
        (void)session->ops.sta_restore(session->ops_ctx);
        (void)app_ops_end_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT);
        session->phase = APP_PROVISION_FAILED;
        session->last_io_error = err;
        session->status = APP_PROVISION_ERR_AP_FAILED;
        return session->status;
    }
    session->ap_up = true;

    err = session->ops.server_start(session->ops_ctx, session->config.server_port);
    if (err != ESP_OK) {
        /* Roll back in the reverse order of the way in: server, then AP, then the
         * radio, then the gate. */
        (void)session->ops.ap_stop(session->ops_ctx);
        session->ap_up = false;
        (void)session->ops.sta_restore(session->ops_ctx);
        (void)app_ops_end_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT);
        session->phase = APP_PROVISION_FAILED;
        session->last_io_error = err;
        session->status = APP_PROVISION_ERR_SERVER_FAILED;
        return session->status;
    }
    session->server_up = true;

    session->started_ms = session->ops.now_ms(session->ops_ctx);
    session->phase = APP_PROVISION_ACTIVE;
    return APP_PROVISION_OK;
}

app_provision_status_t app_provision_stop(app_provision_t *session)
{
    bool reader_was_closed;

    if (session == NULL) {
        return APP_PROVISION_ERR_NOT_ACTIVE;
    }
    if (session->phase == APP_PROVISION_IDLE ||
        session->phase == APP_PROVISION_STOPPED) {
        /* Ending a session that already ended is normal - the operator closing the
         * page twice - so it is a no-op, not an error. */
        session->phase = APP_PROVISION_STOPPED;
        session->status = APP_PROVISION_OK;
        return APP_PROVISION_OK;
    }

    session->phase = APP_PROVISION_STOPPING;

    /*
     * The server goes down first. Its handlers touch the reader and the import; a
     * request still in flight when the AP disappears underneath it would be operating
     * on a transport that no longer exists.
     */
    if (session->server_up) {
        (void)session->ops.server_stop(session->ops_ctx);
        session->server_up = false;
    }

    if (session->ap_up) {
        (void)session->ops.ap_stop(session->ops_ctx);
        session->ap_up = false;
    }

    /*
     * If an import was interrupted between closing the reader and reopening it, the
     * reader is still closed. Reopening here is what stops a failed import from
     * leaving the device without recognition for the rest of its uptime.
     */
    reader_was_closed = session->reader_closed;
    if (reader_was_closed) {
        (void)session->ops.db_reopen(session->ops_ctx);
        session->reader_closed = false;
    }

    /* Hand the radio back to the station owner. A failure here is reported rather
     * than hidden: an AP that is down but a station that never came back is exactly
     * the state an operator cannot see. */
    {
        esp_err_t err = session->ops.sta_restore(session->ops_ctx);

        if (err != ESP_OK) {
            session->last_io_error = err;
            session->status = APP_PROVISION_ERR_AP_FAILED;
        } else {
            session->status = APP_PROVISION_OK;
        }
    }

    (void)app_ops_end_exclusive(session->gate, APP_OP_STATE_WEB_MANAGEMENT);
    session->phase = APP_PROVISION_STOPPED;
    return session->status;
}

bool app_provision_tick(app_provision_t *session)
{
    uint32_t now;
    uint32_t elapsed;

    if (session == NULL || session->phase != APP_PROVISION_ACTIVE) {
        return false;
    }
    now = session->ops.now_ms(session->ops_ctx);
    elapsed = now - session->started_ms;
    if (elapsed < session->config.session_timeout_ms) {
        return false;
    }
    /* The deadline is not optional: a portal that never ended would leave the device
     * in AP mode - and its station disconnected - until someone power-cycled it. */
    (void)app_provision_stop(session);
    return true;
}

bool app_provision_is_active(const app_provision_t *session)
{
    return session != NULL && session->phase == APP_PROVISION_ACTIVE;
}

bool app_provision_token_matches(const app_provision_t *session, const char *token)
{
    size_t expected;
    unsigned char diff = 0u;

    if (session == NULL || token == NULL) {
        return false;
    }
    expected = strlen(session->token);
    if (expected == 0u || strlen(token) != expected) {
        /* Length first, and a mismatch is a refusal. This leaks the length, which is
         * fixed and public, and nothing else. */
        return false;
    }
    /*
     * No early exit. A loop that returned on the first difference would take
     * measurably longer for a token that shares a longer prefix, which turns the
     * check into an oracle for guessing the token one character at a time.
     */
    for (size_t i = 0u; i < expected; ++i) {
        diff |= (unsigned char)(session->token[i] ^ token[i]);
    }
    return diff == 0u;
}

/* ---------------- database import ---------------- */

app_provision_status_t app_provision_begin_import(app_provision_t *session,
                                                  app_db_import_t *imp,
                                                  uint64_t declared_length)
{
    app_db_import_error_t err;

    if (session == NULL || imp == NULL) {
        return APP_PROVISION_ERR_IMPORT_FAILED;
    }
    if (!app_provision_is_active(session)) {
        return APP_PROVISION_ERR_NOT_ACTIVE;
    }
    /*
     * app_db_import_begin() requires a handle that app_db_import_init() succeeded on;
     * it calls the vtable unconditionally. Checking that here rather than adding a
     * defensive test inside the import state machine keeps the import's own contract
     * simple ("you must have initialised this") and puts the boundary check at the
     * boundary, where the caller that could get it wrong actually is.
     */
    if (imp->io.medium_present == NULL) {
        session->status = APP_PROVISION_ERR_IMPORT_FAILED;
        return session->status;
    }

    err = app_db_import_begin(imp, declared_length);
    if (err == APP_DB_IMPORT_ERR_NO_MEDIUM) {
        /* Refused up front rather than after streaming megabytes to a card that is
         * not there. */
        session->status = APP_PROVISION_ERR_NO_MEDIUM;
        return session->status;
    }
    if (err != APP_DB_IMPORT_ERR_NONE) {
        session->status = APP_PROVISION_ERR_IMPORT_FAILED;
        return session->status;
    }
    session->status = APP_PROVISION_OK;
    return APP_PROVISION_OK;
}

app_provision_status_t app_provision_finish_import(app_provision_t *session,
                                                   app_db_import_t *imp,
                                                   uint32_t *out_version)
{
    app_db_import_error_t err;

    if (out_version != NULL) {
        *out_version = 0u;
    }
    if (session == NULL || imp == NULL) {
        return APP_PROVISION_ERR_IMPORT_FAILED;
    }
    if (!app_provision_is_active(session)) {
        return APP_PROVISION_ERR_NOT_ACTIVE;
    }

    /*
     * The reader is closed for the whole replace, not just for the renames.
     *
     * FAT gives no atomic replace, so between the two renames the corpus path holds
     * nothing. A reader holding the file open across that window would be seeking
     * into a file that no longer has a name, and could answer a match out of a
     * corpus that is being retired. Closing it first is what makes the window
     * harmless - and it is also why the portal takes the operation gate: no scan can
     * be running to reopen the reader behind this function's back.
     */
    session->ops.db_close(session->ops_ctx);
    session->reader_closed = true;

    /*
     * An import handle that was never begun reports NOT_ACTIVE and touches no
     * medium, so a caller that somehow reaches here with one still gets the
     * close/reopen pair rather than a reader left closed for the rest of uptime.
     */
    err = app_db_import_finish(imp, out_version);

    /* Reopen unconditionally, including after a failure: a failed import leaves the
     * old corpus in place, and the device must keep recognising devices with it. */
    {
        esp_err_t reopen_err = session->ops.db_reopen(session->ops_ctx);

        session->reader_closed = false;
        if (reopen_err != ESP_OK) {
            session->last_io_error = reopen_err;
        }
    }

    if (err != APP_DB_IMPORT_ERR_NONE) {
        session->status = APP_PROVISION_ERR_IMPORT_FAILED;
        return session->status;
    }
    session->status = APP_PROVISION_OK;
    return APP_PROVISION_OK;
}

void app_provision_cancel_import(app_provision_t *session, app_db_import_t *imp)
{
    if (imp != NULL) {
        app_db_import_cancel(imp);
    }
    if (session == NULL) {
        return;
    }
    /*
     * Nothing to reopen here: cancel() only ever removes the .part file, and the
     * reader is closed and reopened entirely inside finish_import(). Reopening a
     * reader that was never closed would be harmless but would hide a future change
     * that made cancel touch the live corpus.
     */
    session->status = APP_PROVISION_OK;
}

/* ---------------- status snapshot ---------------- */

void app_provision_status_snapshot(const app_provision_t *session,
                                   bool ap_active,
                                   app_portal_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (session == NULL) {
        return;
    }
    out->active = session->phase == APP_PROVISION_ACTIVE;
    (void)app_strlcpy(out->ap_ssid, session->ap_ssid, sizeof(out->ap_ssid));
    (void)app_strlcpy(out->ap_ipv4, session->ap_ipv4, sizeof(out->ap_ipv4));
    /*
     * `ap_active` is the transport's own view, passed in rather than read from this
     * struct, because this module knows what it ASKED for and only the transport
     * knows what is actually up. Reporting the request as the state is how a status
     * document ends up lying.
     */
    if (!ap_active && out->active) {
        out->active = false;
    }
    /* No password, no token: they are not copied anywhere near this structure, so
     * there is nothing for a later change to accidentally include. */
}
