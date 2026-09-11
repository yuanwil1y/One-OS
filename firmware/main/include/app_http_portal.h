#pragma once

/*
 * HTTP transport for the provisioning portal.
 *
 * A thin transport, and only that. Every endpoint here maps a request onto
 * app_provision.c / app_portal.c and formats the answer; no endpoint decides anything
 * on its own. That is the product rule - diagnostics, HTTP and the future GUI all reach
 * the same business logic - and it is why this file is small.
 *
 * ROUTES
 *
 *   GET  /                  a minimal plain-text index, so an operator who opens the
 *                           root sees what exists instead of a 404
 *   GET  /api/status        the status document, with no secret in it
 *   GET  /api/wifi/scan     a bounded list, SSIDs as hex
 *   POST /api/wifi/connect  form-encoded credentials; requires the session token
 *   POST /api/db/upload     raw .nbdb bytes, streamed; requires the session token
 *   POST /api/portal/finish ends the session
 *
 * AUTHORISATION
 *
 * Every mutating endpoint requires the session token, in the `X-Portal-Token` header or
 * a `token` form field. Without it, anything in radio range while the portal is up could
 * replace the recognition database or rewrite the station credentials - and the portal's
 * whole premise is that the operator is the only one who has the token, because it is
 * only ever shown on the device itself.
 *
 * STREAMING
 *
 * /api/db/upload never buffers the body. esp_http_server hands it over in chunks and each
 * chunk goes straight to app_db_import_write(), which enforces the declared length and
 * the policy cap before it writes. A body longer than allowed is refused mid-stream and
 * the .part file is removed, so a hostile Content-Length cannot fill the card.
 *
 * Platform dependent (ESP-IDF esp_http_server). Everything it calls is not.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_db_import.h"
#include "app_provision.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What the handlers need from the rest of the application.
 *
 * The runtime callbacks are injected rather than called directly so this file does not
 * depend on app_runtime.c and, more importantly, so the handlers cannot reach past the
 * provisioning session into the scanner or the device tables.
 */
typedef struct {
    /* Fill the parts of the status document that come from the runtime. */
    void (*status_fill)(void *ctx, app_portal_status_t *out);
    /* Device count and entity count, for the index page. */
    size_t (*device_count)(void *ctx);
    size_t (*entity_count)(void *ctx);
} app_http_portal_hooks_t;

typedef struct {
    app_provision_t *session;
    app_db_import_t *import;
    const app_http_portal_hooks_t *hooks;
    void *hooks_ctx;
    /* Port the session was started with; the server binds it. */
    uint16_t port;
    /* Largest body /api/db/upload will accept. 0 selects the import's own policy. */
    uint32_t upload_max_bytes;
} app_http_portal_t;

/*
 * Start the HTTP server for an ACTIVE provisioning session.
 *
 * Refuses when the session is not active: a server without a session would have no AP
 * to serve and no credentials to protect, and its mutating endpoints would have nothing
 * to authenticate against.
 */
esp_err_t app_http_portal_start(app_http_portal_t *portal,
                                const app_provision_ops_t *server_ops);

/* Stop the HTTP server. Idempotent. */
esp_err_t app_http_portal_stop(app_http_portal_t *portal);

bool app_http_portal_is_running(const app_http_portal_t *portal);

/* Number of requests handled since start, and how many were rejected as unauthorised.
 * Reported so an operator can see that someone else is trying. */
uint32_t app_http_portal_request_count(const app_http_portal_t *portal);
uint32_t app_http_portal_rejected_count(const app_http_portal_t *portal);

#ifdef __cplusplus
}
#endif
