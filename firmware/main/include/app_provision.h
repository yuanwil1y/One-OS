#pragma once

/*
 * Provisioning session: the application side of the portal.
 *
 * Owns one bounded session in which the device presents a temporary access point,
 * serves the provisioning API and can import a new recognition database. It is the
 * application logic behind the HTTP endpoints; the endpoints themselves are a thin
 * transport over this, exactly as the serial console is a thin transport over the
 * runtime.
 *
 * Platform independent. Every radio, storage and clock operation arrives through
 * app_provision_ops_t, so the whole session - including the ordering that matters -
 * is exercised on the host.
 *
 * THE ORDERING THAT MATTERS
 *
 * A session is an exclusive product operation. It takes the same operation gate a
 * scan does, so the portal and a scan cannot run at once. That is not a convention:
 * the portal hands out the Wi-Fi driver, a scan takes it away, and a scan's handover
 * ends with the STA owner re-initialising it. Allowing both would mean the driver
 * changing owner underneath whichever operation thought it had it.
 *
 * A database import happens INSIDE the session and is coordinated explicitly:
 *
 *     close the recognition reader  (nobody may be seeking the file)
 *       -> promote the new corpus   (the rename pair)
 *     reopen the recognition reader
 *
 * The reader is closed first because FAT gives no atomic replace: between the two
 * renames the path holds nothing, and a reader mid-seek would be reading a file that
 * no longer has a name. Closing it is the only way to make that window harmless.
 * The import's own state machine (app_db_import.c) handles what happens if power is
 * lost inside that window; this module's job is only to keep the reader out of it.
 *
 * ENDING A SESSION
 *
 * Ending is deliberately explicit and idempotent, and it always:
 *   - stops the HTTP server before the AP goes away, so no request is in flight
 *     when its transport disappears;
 *   - brings the AP down and hands the radio back to the station owner;
 *   - releases the operation gate, so a scan can run again.
 *
 * A session that cannot be ended cleanly is reported as such rather than being
 * silently forgotten, because a device stuck in AP mode with no station is not
 * something the operator can see from the UI.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_db_import.h"
#include "app_ops.h"
#include "app_portal.h"
#include "app_wifi.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_PROVISION_IDLE = 0,
    APP_PROVISION_STARTING,
    APP_PROVISION_ACTIVE,
    APP_PROVISION_STOPPING,
    APP_PROVISION_STOPPED,
    APP_PROVISION_FAILED,
} app_provision_phase_t;

typedef enum {
    APP_PROVISION_OK = 0,
    APP_PROVISION_ERR_BUSY,            /* a scan or another portal owns the app */
    APP_PROVISION_ERR_AP_FAILED,       /* the access point could not be started */
    APP_PROVISION_ERR_SERVER_FAILED,   /* the HTTP server could not be started */
    APP_PROVISION_ERR_NOT_ACTIVE,      /* stop() without start() */
    APP_PROVISION_ERR_NOT_AUTHORIZED,  /* caller did not present the session token */
    APP_PROVISION_ERR_NO_MEDIUM,       /* an import was requested without a card */
    APP_PROVISION_ERR_IMPORT_FAILED,
} app_provision_status_t;

const char *app_provision_phase_name(app_provision_phase_t phase);
const char *app_provision_status_name(app_provision_status_t status);

/*
 * Everything the session needs from the platform.
 *
 * `wifi_*` are the same functions the rest of the application uses, so the portal
 * cannot invent a second Wi-Fi lifecycle. `db_close`/`db_reopen` are the runtime's
 * own reader control. `random_u32` is the only entropy source.
 */
typedef struct {
    /* Wi-Fi access point, owned for the duration of the session. */
    esp_err_t (*ap_start)(void *ctx, const char *ssid, const char *password);
    esp_err_t (*ap_stop)(void *ctx);
    /* The station owner's own bring-up, called after the AP is down. */
    esp_err_t (*sta_restore)(void *ctx);
    /* Tell the station owner that the radio is held for the session. */
    esp_err_t (*sta_release)(void *ctx);

    /* HTTP transport. */
    esp_err_t (*server_start)(void *ctx, uint16_t port);
    esp_err_t (*server_stop)(void *ctx);

    /* Recognition reader control. */
    void (*db_close)(void *ctx);
    esp_err_t (*db_reopen)(void *ctx);

    /* Monotonic milliseconds, for the session deadline. */
    uint32_t (*now_ms)(void *ctx);

    uint32_t (*random_u32)(void);
} app_provision_ops_t;

typedef struct {
    /* 0 selects the default. A session that never ends on its own would leave the
     * device in AP mode forever, so there is always a deadline. */
    uint32_t session_timeout_ms;
    uint16_t server_port;
    /* AP SSID suffix source. The SSID must be unique enough for an operator to pick
     * the right device out of a list, so it ends in four hex digits derived from the
     * random source - never from the MAC, which would make the device trackable. */
    char ap_ssid_prefix[16];
} app_provision_config_t;

#define APP_PROVISION_DEFAULT_TIMEOUT_MS (15u * 60u * 1000u)
#define APP_PROVISION_DEFAULT_PORT 80u

/*
 * The session token.
 *
 * Every mutating endpoint requires it. Without one, any device on the temporary AP -
 * which is to say any device in radio range while the portal is up - could replace
 * the recognition database or rewrite the station credentials. The token is
 * generated with the AP password and is shown the same way: on the device's own
 * presentation and once on the local serial console, never in a status response.
 */
#define APP_PROVISION_TOKEN_HEX 16u /* 8 random bytes */

typedef struct {
    app_provision_ops_t ops;
    void *ops_ctx;
    app_ops_t *gate; /* the application operation gate, owned by the runtime */

    app_provision_phase_t phase;
    app_provision_status_t status;
    esp_err_t last_io_error;

    app_provision_config_t config;

    char ap_ssid[WIFI_MGR_SSID_STORE];
    char ap_password[APP_PORTAL_AP_PASSWORD_MAX];
    char token[APP_PROVISION_TOKEN_HEX + 1u];
    char ap_ipv4[WIFI_MGR_IPV4_MAX];

    uint32_t started_ms;
    bool ap_up;
    bool server_up;
    bool reader_closed;
    /* Set once the operator has confirmed the credentials work, so the portal can
     * say "the station is connected" without claiming it from the AP side. */
    bool sta_verified;
} app_provision_t;

void app_provision_config_default(app_provision_config_t *out);

esp_err_t app_provision_init(app_provision_t *session,
                             const app_provision_ops_t *ops, void *ops_ctx,
                             app_ops_t *gate,
                             const app_provision_config_t *config);

/*
 * Start a session.
 *
 * Order: take the operation gate, hand the radio over from the station owner, bring
 * the AP up, generate the credentials, start the server. Every step that fails rolls
 * back the steps that succeeded, so a failed start leaves the device exactly as it
 * was - in particular with its station connection intact.
 */
app_provision_status_t app_provision_start(app_provision_t *session);

/*
 * End a session. Idempotent: calling it on an already-stopped session is a no-op
 * rather than an error, because "the operator closed the page twice" is normal.
 */
app_provision_status_t app_provision_stop(app_provision_t *session);

/*
 * Advance the session: expires it when its deadline passes.
 *
 * Called from the application worker. Returns true when the session ended, so the
 * caller can log it once instead of on every poll.
 */
bool app_provision_tick(app_provision_t *session);

bool app_provision_is_active(const app_provision_t *session);

/*
 * Constant-time comparison of a presented token against the session's.
 *
 * Length is compared first and must match. The loop does not exit early: a token
 * check that returns faster for a wrong first character tells a caller how much of
 * the token it guessed correctly.
 */
bool app_provision_token_matches(const app_provision_t *session, const char *token);

/*
 * Import a database into the running session.
 *
 * `imp` comes from the caller so the HTTP layer can stream into it; this function
 * owns only the coordination: the reader is closed for the rename and reopened after
 * it, and the session refuses the import when no card is present rather than letting
 * the upload write a file it can never promote.
 *
 * `declared_length` is the browser's Content-Length. The caller feeds bytes with
 * app_db_import_write() and then calls this; see app_db_import.h for the streaming
 * contract.
 */
app_provision_status_t app_provision_begin_import(app_provision_t *session,
                                                  app_db_import_t *imp,
                                                  uint64_t declared_length);

app_provision_status_t app_provision_finish_import(app_provision_t *session,
                                                   app_db_import_t *imp,
                                                   uint32_t *out_version);

/* Abandon an import and remove its .part file. Never touches the live corpus. */
void app_provision_cancel_import(app_provision_t *session, app_db_import_t *imp);

/* Fill a status snapshot for /api/status. Never includes a password or the token. */
void app_provision_status_snapshot(const app_provision_t *session,
                                   bool ap_active,
                                   app_portal_status_t *out);

#ifdef __cplusplus
}
#endif
