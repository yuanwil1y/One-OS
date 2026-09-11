/*
 * HTTP transport for the provisioning portal. See app_http_portal.h for the routes, the
 * authorisation rule and the streaming contract.
 *
 * Every handler is a mapping: read the request, call the session, format the answer. If
 * a handler here ever needs to make a decision, that decision belongs one layer down.
 */

#include "app_http_portal.h"

#include "app_portal.h"
#include "app_str.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "app_portal_http";

/* Request body chunk handed to us by esp_http_server, and the largest we will accept in
 * one read. Bounded so a single chunk cannot be used to make us allocate. */
#define PORTAL_RX_CHUNK 1024u
#define PORTAL_TOKEN_HEADER "X-Portal-Token"

/* Counters are read from any task through the accessors below; the handlers are the only
 * writers, and they run on the server's task.
 */
typedef struct {
    app_http_portal_t config;
    httpd_handle_t server;
    bool running;
    uint32_t requests;
    uint32_t rejected;
    /* The import in flight, if any. Guarded by `import_lock` because an upload can be
     * abandoned by a disconnect. */
    bool upload_active;
} portal_state_t;

static portal_state_t s_state;

bool app_http_portal_is_running(const app_http_portal_t *portal)
{
    (void)portal;
    return s_state.running;
}

uint32_t app_http_portal_request_count(const app_http_portal_t *portal)
{
    (void)portal;
    return s_state.requests;
}

uint32_t app_http_portal_rejected_count(const app_http_portal_t *portal)
{
    (void)portal;
    return s_state.rejected;
}

/* ---------------- helpers ---------------- */

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    /* Nothing here is cacheable: a status document describes the present. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/*
 * Is this request authorised?
 *
 * The token is accepted from the header the page's own JavaScript sends, or from a
 * `token` form field so a plain HTML form works without JavaScript. Nothing else is
 * accepted: a query parameter would end up in a log and a cookie would outlive the
 * session.
 */
static bool request_authorised(httpd_req_t *req, char *token_out, size_t token_size)
{
    size_t header_len;

    token_out[0] = '\0';

    header_len = httpd_req_get_hdr_value_len(req, PORTAL_TOKEN_HEADER);
    if (header_len > 0u && header_len < token_size) {
        if (httpd_req_get_hdr_value_str(req, PORTAL_TOKEN_HEADER, token_out,
                                        token_size) == ESP_OK) {
            return app_provision_token_matches(s_state.config.session, token_out);
        }
    }
    return false;
}

static esp_err_t refuse_unauthorised(httpd_req_t *req)
{
    char body[96];

    s_state.rejected++;
    (void)app_portal_build_result_json(false, "not_authorized", body, sizeof(body));
    ESP_LOGW(TAG, "rejected an unauthorised request to %s", req->uri);
    return send_json(req, "401 Unauthorized", body);
}

/*
 * Read a form body into a bounded buffer and hand it to app_portal.
 *
 * The body is never larger than the buffer: a form with credentials in it is a few
 * hundred bytes, and anything bigger is refused rather than grown into.
 */
static esp_err_t read_form(httpd_req_t *req, char *out, size_t out_size,
                           size_t *out_length)
{
    size_t total = 0u;
    int received;

    if (req->content_len >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    while (total < (size_t)req->content_len) {
        received = httpd_req_recv(req, out + total, (size_t)req->content_len - total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += (size_t)received;
    }
    out[total] = '\0';
    *out_length = total;
    return ESP_OK;
}

/* ---------------- GET / ---------------- */

static esp_err_t handler_index(httpd_req_t *req)
{
    char body[512];
    size_t devices = 0u;
    size_t entities = 0u;

    s_state.requests++;
    if (s_state.config.hooks != NULL) {
        if (s_state.config.hooks->device_count != NULL) {
            devices = s_state.config.hooks->device_count(s_state.config.hooks_ctx);
        }
        if (s_state.config.hooks->entity_count != NULL) {
            entities = s_state.config.hooks->entity_count(s_state.config.hooks_ctx);
        }
    }
    (void)snprintf(body, sizeof(body),
                   "One-OS provisioning portal\n"
                   "devices: %u  entities: %u\n"
                   "\n"
                   "GET  /api/status\n"
                   "GET  /api/wifi/scan\n"
                   "POST /api/wifi/connect   (token required)\n"
                   "POST /api/db/upload      (token required)\n"
                   "POST /api/portal/finish  (token required)\n"
                   "\n"
                   "The token is shown on the device itself. It is never sent in a "
                   "status response.\n",
                   (unsigned)devices, (unsigned)entities);
    return send_text(req, "200 OK", body);
}

/* ---------------- GET /api/status ---------------- */

static esp_err_t handler_status(httpd_req_t *req)
{
    app_portal_status_t status;
    char body[APP_PORTAL_STATUS_MAX];
    size_t written;

    s_state.requests++;

    memset(&status, 0, sizeof(status));
    if (s_state.config.hooks != NULL && s_state.config.hooks->status_fill != NULL) {
        s_state.config.hooks->status_fill(s_state.config.hooks_ctx, &status);
    }
    /*
     * The session fills what only it knows, so the document always reports the live AP
     * state rather than whatever the runtime last cached.
     */
    app_provision_status_snapshot(s_state.config.session, true, &status);

    written = app_portal_build_status_json(&status, body, sizeof(body));
    if (written == 0u) {
        /* Refused rather than truncated: half a JSON document is not an answer. */
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"status_unavailable\"}");
    }
    return send_json(req, "200 OK", body);
}

/* ---------------- GET /api/wifi/scan ---------------- */

static esp_err_t handler_wifi_scan(httpd_req_t *req)
{
    app_portal_ap_t aps[APP_PORTAL_WIFI_SCAN_MAX];
    char body[APP_PORTAL_JSON_MAX];
    size_t count = 0u;
    size_t written;

    s_state.requests++;
    memset(aps, 0, sizeof(aps));

    /*
     * The scan goes through the runtime's own Wi-Fi facilities, and it is refused while
     * the portal holds the operation gate for its own AP - which is exactly why the scan
     * result may legitimately be empty here. An empty list is reported as an empty list,
     * never as an error.
     */
    if (!app_portal_native_wifi_scan(aps, APP_PORTAL_WIFI_SCAN_MAX, &count)) {
        return send_json(req, "503 Service Unavailable",
                         "{\"ok\":false,\"error\":\"scan_unavailable\"}");
    }

    written = app_portal_build_scan_json(aps, count, body, sizeof(body));
    if (written == 0u) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"scan_result_too_large\"}");
    }
    return send_json(req, "200 OK", body);
}

/* ---------------- POST /api/wifi/connect ---------------- */

static esp_err_t handler_wifi_connect(httpd_req_t *req)
{
    char form[512];
    size_t form_length = 0u;
    char token[APP_PROVISION_TOKEN_HEX + 8u];
    app_portal_wifi_form_t credentials;
    app_portal_form_status_t parsed;
    char body[160];
    esp_err_t err;

    s_state.requests++;

    if (!request_authorised(req, token, sizeof(token))) {
        return refuse_unauthorised(req);
    }
    err = read_form(req, form, sizeof(form), &form_length);
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_json(req, "413 Content Too Large",
                         "{\"ok\":false,\"error\":\"form_too_large\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"error\":\"body_incomplete\"}");
    }

    parsed = app_portal_parse_wifi_form(form, form_length, &credentials);
    if (parsed != APP_PORTAL_FORM_OK) {
        (void)app_portal_build_result_json(false,
                                           app_portal_form_status_name(parsed), body,
                                           sizeof(body));
        /* The reason is one of the parser's own stable names, never echoed input. */
        return send_json(req, "400 Bad Request", body);
    }

    /*
     * Stored first, then connected. The product rule is that a failed connection leaves
     * the credentials for a retry, so a failure at the connect step must not undo the
     * store - which means storing cannot be conditional on connecting.
     */
    if (wifi_mgr_set_credentials(credentials.ssid,
                                 credentials.password_present ? credentials.password
                                                              : "") != ESP_OK) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"error\":\"store_failed\"}");
    }

    (void)app_portal_build_result_json(true, NULL, body, sizeof(body));
    ESP_LOGI(TAG, "station credentials stored from the portal (values not logged)");
    return send_json(req, "200 OK", body);
}

/* ---------------- POST /api/db/upload ---------------- */

static esp_err_t handler_db_upload(httpd_req_t *req)
{
    char token[APP_PROVISION_TOKEN_HEX + 8u];
    char chunk[PORTAL_RX_CHUNK];
    char body[192];
    uint64_t declared = 0u;
    uint64_t received = 0u;
    uint32_t version = 0u;
    app_provision_status_t status;
    bool length_known = false;

    s_state.requests++;

    if (!request_authorised(req, token, sizeof(token))) {
        return refuse_unauthorised(req);
    }
    if (s_state.upload_active) {
        /* One upload at a time: a second would write into the same .part file. */
        return send_json(req, "409 Conflict",
                         "{\"ok\":false,\"error\":\"upload_in_progress\"}");
    }

    /*
     * Content-Length, if the client sent one. It is enforced in both directions by the
     * import state machine: a longer body is refused mid-stream and a shorter one fails
     * at finish(). A client that sends none is still bounded by the policy cap.
     */
    {
        size_t header_len = httpd_req_get_hdr_value_len(req, "Content-Length");

        if (header_len > 0u && header_len < 32u) {
            char value[32];

            if (httpd_req_get_hdr_value_str(req, "Content-Length", value,
                                            sizeof(value)) == ESP_OK) {
                length_known = app_portal_parse_content_length(value, &declared);
            }
        }
    }

    status = app_provision_begin_import(s_state.config.session,
                                       s_state.config.import,
                                       length_known ? declared : 0u);
    if (status == APP_PROVISION_ERR_NO_MEDIUM) {
        return send_json(req, "503 Service Unavailable",
                         "{\"ok\":false,\"error\":\"no_medium\"}");
    }
    if (status != APP_PROVISION_OK) {
        return send_json(req, "409 Conflict",
                         "{\"ok\":false,\"error\":\"import_unavailable\"}");
    }

    s_state.upload_active = true;

    /*
     * Stream. Each chunk goes straight into the import writer, which checks the
     * declared length and the policy cap BEFORE writing - so an oversized body cannot
     * fill the card even transiently. A failure here aborts and removes the .part.
     */
    while (received < (uint64_t)req->content_len) {
        int want = (int)sizeof(chunk);
        int got;
        app_db_import_error_t write_error;

        if ((uint64_t)want > (uint64_t)req->content_len - received) {
            want = (int)((uint64_t)req->content_len - received);
        }
        got = httpd_req_recv(req, chunk, (size_t)want);
        if (got <= 0) {
            /* A dropped connection mid-upload. Nothing is promoted and the .part goes. */
            app_provision_cancel_import(s_state.config.session, s_state.config.import);
            s_state.upload_active = false;
            return send_json(req, "400 Bad Request",
                             "{\"ok\":false,\"error\":\"upload_interrupted\"}");
        }
        write_error = app_db_import_write(s_state.config.import, chunk,
                                         (size_t)got);
        if (write_error != APP_DB_IMPORT_ERR_NONE) {
            app_provision_cancel_import(s_state.config.session, s_state.config.import);
            s_state.upload_active = false;
            (void)app_portal_build_result_json(
                false, app_db_import_error_name(write_error), body, sizeof(body));
            return send_json(req, "413 Content Too Large", body);
        }
        received += (uint64_t)got;
    }

    status = app_provision_finish_import(s_state.config.session,
                                         s_state.config.import, &version);
    s_state.upload_active = false;

    if (status != APP_PROVISION_OK) {
        return send_json(req, "422 Unprocessable Content",
                         "{\"ok\":false,\"error\":\"invalid_database\"}");
    }

    (void)snprintf(body, sizeof(body),
                   "{\"ok\":true,\"db_version\":%lu,\"bytes\":%llu}",
                   (unsigned long)version, (unsigned long long)received);
    ESP_LOGI(TAG, "database replaced from the portal: version %lu, %llu bytes",
             (unsigned long)version, (unsigned long long)received);
    return send_json(req, "200 OK", body);
}

/* ---------------- POST /api/portal/finish ---------------- */

static esp_err_t handler_portal_finish(httpd_req_t *req)
{
    char token[APP_PROVISION_TOKEN_HEX + 8u];
    char body[96];

    s_state.requests++;

    if (!request_authorised(req, token, sizeof(token))) {
        return refuse_unauthorised(req);
    }
    (void)app_portal_build_result_json(true, NULL, body, sizeof(body));

    /*
     * The answer is sent BEFORE the session ends. Ending it stops this server, and a
     * response attempted after that would never reach the browser - the operator would
     * see a network error for an action that in fact succeeded.
     */
    {
        esp_err_t err = httpd_resp_set_status(req, "200 OK");
        (void)err;
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, body);
    }
    app_provision_stop(s_state.config.session);
    return ESP_OK;
}

/* ---------------- server lifecycle ---------------- */

static httpd_uri_t s_routes[] = {
    { .uri = "/", .method = HTTP_GET, .handler = handler_index },
    { .uri = "/api/status", .method = HTTP_GET, .handler = handler_status },
    { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = handler_wifi_scan },
    { .uri = "/api/wifi/connect", .method = HTTP_POST,
      .handler = handler_wifi_connect },
    { .uri = "/api/db/upload", .method = HTTP_POST, .handler = handler_db_upload },
    { .uri = "/api/portal/finish", .method = HTTP_POST,
      .handler = handler_portal_finish },
};

esp_err_t app_http_portal_start(app_http_portal_t *portal,
                                const app_provision_ops_t *server_ops)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;

    (void)server_ops;
    if (portal == NULL || portal->session == NULL || portal->import == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_provision_is_active(portal->session)) {
        /* Without an active session there is no AP to serve and no token to check
         * against, so the mutating endpoints would have nothing to authenticate. */
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.running) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.config = *portal;

    /*
     * A small server: a handful of routes, one upload at a time, and a control path that
     * never allocates per request.
     */
    config.max_uri_handlers = (uint16_t)(sizeof(s_routes) / sizeof(s_routes[0]));
    config.stack_size = 6144u;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (portal->port != 0u) {
        config.server_port = portal->port;
    }

    err = httpd_start(&s_state.server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        err = httpd_register_uri_handler(s_state.server, &s_routes[i]);
        if (err != ESP_OK) {
            /* A partially registered server would answer some routes and 404 others,
             * which is worse than not starting: stop it and report. */
            ESP_LOGE(TAG, "registering %s failed: %s", s_routes[i].uri,
                     esp_err_to_name(err));
            (void)httpd_stop(s_state.server);
            s_state.server = NULL;
            return err;
        }
    }

    s_state.running = true;
    ESP_LOGI(TAG, "provisioning portal listening on port %u",
             (unsigned)config.server_port);
    return ESP_OK;
}

esp_err_t app_http_portal_stop(app_http_portal_t *portal)
{
    (void)portal;

    if (!s_state.running || s_state.server == NULL) {
        return ESP_OK; /* idempotent */
    }
    /*
     * An upload still in flight when the server stops is abandoned, and its .part file
     * removed: leaving it would let a later begin() find a partial file, and the live
     * corpus must not be touched by an upload that never finished.
     */
    if (s_state.upload_active) {
        app_provision_cancel_import(s_state.config.session, s_state.config.import);
        s_state.upload_active = false;
    }
    (void)httpd_stop(s_state.server);
    s_state.server = NULL;
    s_state.running = false;
    ESP_LOGI(TAG, "provisioning portal stopped");
    return ESP_OK;
}
