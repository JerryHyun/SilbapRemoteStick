#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "settings.h"
#include "web_decide.h"   /* web_access_t */

typedef esp_err_t (*web_handler_t)(httpd_req_t *req);

/* Starts the HTTP server (port 80) with the static SPA, /api/recovery and the middleware. Call after net_start;
 * idempotent; never blocks. s must outlive the server (its password fields are read per request). */
esp_err_t web_start(settings_t *s);

/* Registers a route behind the middleware: Host check (always), Origin check (methods other than
 * GET/HEAD/OPTIONS/PROPFIND), then auth (when s->pw_set and access == WEB_AUTH). A rejected request gets a 403 JSON
 * error or a 401 page with WWW-Authenticate. Call after web_start (otherwise logged and ignored). uri may end in '*'. */
void web_route(const char *uri, httpd_method_t m, web_access_t a, web_handler_t h);

/* Removes one route again (same uri + method as web_route). The route table slot is reused, so a
 * web_route/web_unroute cycle can repeat any number of times. Not registered -> logged, nothing else. */
void web_unroute(const char *uri, httpd_method_t m);

/* True once web_start() has a running server. */
bool web_started(void);

/* True while the setup portal is up AND the device is actually in setup mode, so GET / must answer the redirect to
 * /setup instead of the SPA. The strong definition lives in main/provision.c; main/web.c provides a weak fallback
 * returning false, the same pattern as the provision_start/stop hooks in main/netif.h. If provision.c is ever
 * dropped from the build the linker picks the fallback and "/" serves the SPA in setup mode -- the old behaviour,
 * not a crash.
 *
 * The __attribute__((weak)) must be on the DEFINITION in web.c only, never here. A function attribute is part of
 * the function's type in every translation unit that includes this header, so putting it on this declaration also
 * makes provision.c's definition weak; the linker then keeps web.c's "return false", the redirect never happens,
 * and nothing warns. That exact mistake was made once and only the nm check caught it. */
bool web_setup_only(void);

/* Runs fn(arg) in the HTTP server task (httpd_queue_work), so a route-table change never races a dispatch. Returns
 * ESP_ERR_INVALID_STATE when the server is not running yet. */
esp_err_t web_run_in_server(void (*fn)(void *), void *arg);

/* Extra host names accepted by the middleware while a subsystem (the setup portal) is active; cleared with
 * web_clear_extra_hosts(). Hosts are not copied: pass string literals or other static storage. */
void web_allow_host(const char *host);
void web_clear_extra_hosts(void);

/* Sends one of the embedded gzip assets by its path in ASSETS[] ("/", "/assets/setup.html", "/assets/app.js", ...). */
esp_err_t web_send_asset(httpd_req_t *req, const char *path);

esp_err_t web_json(httpd_req_t *req, int status, const char *json);    /* sets type + Cache-Control: no-store */
esp_err_t web_error(httpd_req_t *req, int status, const char *code);   /* {"error":"<code>"}; code is an i18n key */

/* Reads the whole request body (Content-Length, at most cap - 1 bytes) into buf and NUL-terminates it. On failure the
 * error is sent (400 empty_key for an empty body, 413 err.too_large) or, when the client stops sending, nothing is
 * sent; false is returned and *rc is what the handler returns (ESP_FAIL: the connection is dropped, because an unread
 * body would be parsed as the next request). */
bool web_read_body(httpd_req_t *req, char *buf, size_t cap, const char *empty_key, esp_err_t *rc);

/* At most one upload (one ~16.5 KiB fs_upload handle) in flight across /api/upload, WebDAV PUT and COPY.
 * claim: true when the caller may start one; release when it is committed or aborted. */
bool web_upload_claim(void);
void web_upload_release(void);
