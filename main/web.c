#include "web.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "api_files.h"
#include "api_settings.h"
#include "esp_log.h"
#include "netif.h"
#include "provision.h"
#include "webdav.h"
#include "webcore.h"

static const char *TAG = "web";
#define MAX_ROUTES 32   /* 20 base routes + 7 setup-portal routes (Task 10); also httpd's max_uri_handlers */
#define MAX_EXTRA_HOSTS 8   /* the captive-portal probe hosts (main/provision.c PROBE_HOSTS) */
#define MAX_HOSTS 12        /* net_allowed_hosts (<=3) + MAX_EXTRA_HOSTS + NULL */
#define CHUNK 4096

static httpd_handle_t s_srv;
static settings_t *s_set;

typedef struct { web_handler_t h; web_access_t access; const char *uri; httpd_method_t method; } route_t;
static route_t s_routes[MAX_ROUTES];
static bool s_used[MAX_ROUTES];
static const char *s_extra_hosts[MAX_EXTRA_HOSTS];   /* setup-portal probe hosts, appended to net_allowed_hosts */

void web_allow_host(const char *host) {
    if (!host || !host[0]) return;
    for (size_t i = 0; i < MAX_EXTRA_HOSTS; i++) {
        if (s_extra_hosts[i] && !strcmp(s_extra_hosts[i], host)) return;
        if (!s_extra_hosts[i]) { s_extra_hosts[i] = host; return; }
    }
    ESP_LOGW(TAG, "no room for extra host %s", host);
}
void web_clear_extra_hosts(void) {
    for (size_t i = 0; i < MAX_EXTRA_HOSTS; i++) s_extra_hosts[i] = NULL;
}

/* ---- recovery text (one place, four languages) ---- */
typedef struct { const char *lang, *title, *text; } recov_t;
static const recov_t RECOV[] = {
    {"ko", "비밀번호가 필요합니다",
     "비밀번호를 잊었다면: 동글이 켜져 있는 동안 BOOT 버튼을 5초간 누르면 비밀번호가 지워집니다. "
     "또는 SD 카드의 silbap-setup.txt에 web_password= 한 줄을 넣고 동글을 다시 꽂으세요."},
    {"en", "Password required",
     "Forgot the password? Hold the BOOT button for 5 seconds while the stick is running to clear it. "
     "Or put web_password= in silbap-setup.txt on the SD card and plug the dongle in again."},
    {"zh", "需要密码",
     "忘记密码？在设备运行时按住 BOOT 按钮 5 秒即可清除密码。"
     "或者在 SD 卡的 silbap-setup.txt 中写入 web_password=，然后重新插入设备。"},
    {"ja", "パスワードが必要です",
     "パスワードを忘れた場合は、動作中に BOOT ボタンを 5 秒間押し続けると消去されます。"
     "または SD カードの silbap-setup.txt に web_password= と書いて、ドングルを挿し直してください。"},
};
static const recov_t *recov_for_ui(void) {
    const char *l = s_set ? s_set->ui_lang : "";
    for (size_t i = 0; i < sizeof RECOV / sizeof RECOV[0]; i++)
        if (!strcmp(RECOV[i].lang, l)) return &RECOV[i];
    return &RECOV[1];   /* en fallback */
}

/* ---- responses ---- */
static const char *status_line(int st) {
    switch (st) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 411: return "411 Length Required";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 416: return "416 Range Not Satisfiable";
    case 422: return "422 Unprocessable Entity";
    case 423: return "423 Locked";
    case 503: return "503 Service Unavailable";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

esp_err_t web_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_error(httpd_req_t *req, int status, const char *code) {
    char b[96];
    wc_json_t w;
    wc_json_init(&w, b, sizeof b);
    wc_json_obj_open(&w);
    wc_json_str(&w, "error", code);
    wc_json_obj_close(&w);
    return web_json(req, status, w.overflow ? "{\"error\":\"err.internal\"}" : b);
}

/* Built pages are static: the httpd task is one request at a time (see web_route), and a >1 KiB frame on top of the
 * handler's own would eat the 8 KiB task stack for nothing. send_401, send_403_page and recovery_get share it. */
static char s_page[1280];

static esp_err_t send_401(httpd_req_t *req) {
    const recov_t *r = recov_for_ui();
    char *b = s_page;
    int n = snprintf(b, sizeof s_page,
             "<!doctype html><html lang=\"%s\"><head><meta charset=\"utf-8\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>%s</title></head>"
             "<body style=\"font:16px/1.5 system-ui,sans-serif;max-width:32em;margin:2em auto;padding:0 16px\">"
             "<h1>%s</h1><p>%s</p></body></html>",
             !strcmp(r->lang, "zh") ? "zh-Hans" : r->lang, r->title, r->title, r->text);
    httpd_resp_set_status(req, status_line(401));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SilbapStick\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n < 0 || (size_t)n >= sizeof s_page) {
        ESP_LOGW(TAG, "401 page truncated");
        return httpd_resp_send(req, "<!doctype html><meta charset=\"utf-8\"><title>401</title><p>401: BOOT 5 s / silbap-setup.txt web_password=", HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

/* ---- middleware ---- */
static const char *method_name(int m) {
    switch (m) {
    case HTTP_GET: return "GET";
    case HTTP_HEAD: return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_PROPFIND: return "PROPFIND";
    default: return "POST";   /* anything else is treated as a mutation */
    }
}

/* Copies a header into buf; NULL when absent (a present-but-empty header gives ""). Too long to fit: returns ""
 * (never matches a host/origin/password). */
static const char *hdr(httpd_req_t *req, const char *name, char *buf, size_t sz) {
    esp_err_t e = httpd_req_get_hdr_value_str(req, name, buf, sz);
    if (e == ESP_OK) return buf;
    if (e == ESP_ERR_NOT_FOUND) return NULL;
    buf[0] = 0;
    return buf;
}

/* ---- Host-mismatch page for browsers ---- */
typedef struct { const char *lang, *title, *fmt; } wronghost_t;   /* fmt: %s = IP, %s = mDNS name */
static const wronghost_t WRONGHOST[] = {
    {"ko", "다른 주소로 접속했습니다", "이 기기는 http://%s 또는 http://%s.local 로 열어 주세요."},
    {"en", "Wrong address", "Open this device at http://%s or http://%s.local"},
    {"zh", "地址不正确", "请通过 http://%s 或 http://%s.local 访问此设备。"},
    {"ja", "アドレスが違います", "このデバイスは http://%s または http://%s.local で開いてください。"},
};

static esp_err_t send_403_page(httpd_req_t *req) {
    char al[64] = "", ip[24] = "";
    char *b = s_page;
    httpd_req_get_hdr_value_str(req, "Accept-Language", al, sizeof al);   /* truncated is fine: only the first tag is used */
    const wronghost_t *w = &WRONGHOST[1];
    for (size_t i = 0; i < sizeof WRONGHOST / sizeof WRONGHOST[0]; i++)
        if (!strncasecmp(al, WRONGHOST[i].lang, 2)) w = &WRONGHOST[i];
    net_ip(ip, sizeof ip);
    char msg[260];
    snprintf(msg, sizeof msg, w->fmt, ip, net_hostname());
    int n = snprintf(b, sizeof s_page,
                     "<!doctype html><html lang=\"%s\"><head><meta charset=\"utf-8\">"
                     "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>%s</title></head>"
                     "<body style=\"font:16px/1.5 system-ui,sans-serif;max-width:32em;margin:2em auto;padding:0 16px\">"
                     "<h1>%s</h1><p>%s</p></body></html>",
                     !strcmp(w->lang, "zh") ? "zh-Hans" : w->lang, w->title, w->title, msg);
    httpd_resp_set_status(req, status_line(403));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (n < 0 || (size_t)n >= sizeof s_page) { ESP_LOGW(TAG, "403 page truncated"); return httpd_resp_sendstr(req, "403"); }
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t dispatch(httpd_req_t *req) {
    const route_t *rt = req->user_ctx;
    char host[64], origin[96], auth[256];
    const char *allowed[MAX_HOSTS];
    const char *base[4];                          /* net_allowed_hosts: at most IP + <host>.local + 192.168.4.1 */
    net_allowed_hosts(base, sizeof base / sizeof base[0]);
    size_t n = 0;
    for (size_t i = 0; i < sizeof base / sizeof base[0] && base[i] && n < MAX_HOSTS - 1; i++) allowed[n++] = base[i];
    for (size_t i = 0; i < MAX_EXTRA_HOSTS && n < MAX_HOSTS - 1; i++)
        if (s_extra_hosts[i]) allowed[n++] = s_extra_hosts[i];
    allowed[n] = NULL;
    int d = web_decide(method_name(req->method), hdr(req, "Host", host, sizeof host),
                       hdr(req, "Origin", origin, sizeof origin), hdr(req, "Authorization", auth, sizeof auth),
                       s_set->pw_set, s_set->pw_salt, s_set->pw_hash, rt->access, allowed);
    if (d == 401) return send_401(req);
    if (d == 403) {
        char acc[96];
        if (req->method == HTTP_GET && httpd_req_get_hdr_value_str(req, "Accept", acc, sizeof acc) != ESP_ERR_NOT_FOUND
            && strstr(acc, "text/html"))
            return send_403_page(req);
        return web_error(req, 403, "err.forbidden");
    }
    return rt->h(req);
}

/* Finds a free slot (slots are never moved, so user_ctx pointers handed to httpd stay valid across unregister). */
static route_t *free_slot(void) {
    for (int i = 0; i < MAX_ROUTES; i++) if (!s_used[i]) return &s_routes[i];
    return NULL;
}

void web_route(const char *uri, httpd_method_t m, web_access_t a, web_handler_t h) {
    if (!s_srv) { ESP_LOGE(TAG, "cannot register %s: no server", uri); return; }
    route_t *rt = free_slot();
    if (!rt) { ESP_LOGE(TAG, "cannot register %s: route table full", uri); return; }
    rt->h = h; rt->access = a; rt->uri = uri; rt->method = m;
    httpd_uri_t u = { .uri = uri, .method = m, .handler = dispatch, .user_ctx = rt };
    esp_err_t e = httpd_register_uri_handler(s_srv, &u);
    if (e != ESP_OK) { ESP_LOGE(TAG, "register %s: %s", uri, esp_err_to_name(e)); rt->uri = NULL; return; }
    s_used[rt - s_routes] = true;
}

void web_unroute(const char *uri, httpd_method_t m) {
    if (!s_srv) return;
    esp_err_t e = httpd_unregister_uri_handler(s_srv, uri, m);
    if (e != ESP_OK) { ESP_LOGE(TAG, "unregister %s: %s", uri, esp_err_to_name(e)); return; }
    for (int i = 0; i < MAX_ROUTES; i++)
        if (s_used[i] && s_routes[i].uri && !strcmp(s_routes[i].uri, uri) && s_routes[i].method == m) {
            s_used[i] = false;
            s_routes[i].uri = NULL;
            return;
        }
}

bool web_started(void) { return s_srv != NULL; }

esp_err_t web_run_in_server(void (*fn)(void *), void *arg) {
    if (!s_srv) return ESP_ERR_INVALID_STATE;
    return httpd_queue_work(s_srv, fn, arg);
}

/* ---- shared request helpers ---- */
#define RECV_RETRIES 5   /* consecutive receive timeouts tolerated before giving up */
bool web_read_body(httpd_req_t *req, char *buf, size_t cap, const char *empty_key, esp_err_t *rc) {
    size_t n = req->content_len;
    *rc = ESP_FAIL;
    if (n == 0) { web_error(req, 400, empty_key); return false; }
    if (n >= cap) { web_error(req, 413, "err.too_large"); return false; }
    size_t got = 0;
    int tries = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++tries <= RECV_RETRIES) continue;
        if (r <= 0) return false;
        tries = 0;
        got += (size_t)r;
    }
    buf[n] = 0;
    *rc = ESP_OK;
    return true;
}

/* Every handler runs on the one httpd task, so a plain flag is enough; it only has to survive a handler that
 * returns while an upload is open (none does today). */
static bool s_uploading;
bool web_upload_claim(void) {
    if (s_uploading) return false;
    s_uploading = true;
    return true;
}
void web_upload_release(void) { s_uploading = false; }

/* ---- embedded SPA ---- */
/* The asset list exists in three places that must match (checked by test_web_assets.py): tools/embed_web.py FILES,
 * main/CMakeLists.txt WEB_NAMES, and the ASSET()/ASSETS[] table below. */
#define ASSET(sym) extern const uint8_t _binary_##sym##_start[] asm("_binary_" #sym "_start"); \
                   extern const uint8_t _binary_##sym##_end[] asm("_binary_" #sym "_end");
ASSET(index_html_gz) ASSET(app_js_gz) ASSET(app_css_gz) ASSET(setup_html_gz)
ASSET(ko_json_gz) ASSET(en_json_gz) ASSET(zh_json_gz) ASSET(ja_json_gz)

typedef struct { const char *path, *type; const uint8_t *start, *end; } asset_t;
#define A(p, ty, sym) { p, ty, _binary_##sym##_start, _binary_##sym##_end }
static const asset_t ASSETS[] = {
    A("/", "text/html; charset=utf-8", index_html_gz),
    A("/assets/setup.html", "text/html; charset=utf-8", setup_html_gz),
    A("/assets/app.js", "text/javascript; charset=utf-8", app_js_gz),
    A("/assets/app.css", "text/css; charset=utf-8", app_css_gz),
    A("/assets/i18n/ko.json", "application/json; charset=utf-8", ko_json_gz),
    A("/assets/i18n/en.json", "application/json; charset=utf-8", en_json_gz),
    A("/assets/i18n/zh.json", "application/json; charset=utf-8", zh_json_gz),
    A("/assets/i18n/ja.json", "application/json; charset=utf-8", ja_json_gz),
};

static esp_err_t static_get(httpd_req_t *req) {
    char path[64];
    size_t n = strcspn(req->uri, "?#");
    if (n >= sizeof path) return web_error(req, 404, "err.not_found");
    memcpy(path, req->uri, n);
    path[n] = 0;
    return web_send_asset(req, path);
}

esp_err_t web_send_asset(httpd_req_t *req, const char *path) {
    for (size_t i = 0; i < sizeof ASSETS / sizeof ASSETS[0]; i++) {
        const asset_t *a = &ASSETS[i];
        if (strcmp(a->path, path)) continue;
        httpd_resp_set_type(req, a->type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        for (const uint8_t *p = a->start; p < a->end; p += CHUNK) {
            size_t l = (size_t)(a->end - p);
            if (l > CHUNK) l = CHUNK;
            if (httpd_resp_send_chunk(req, (const char *)p, (ssize_t)l) != ESP_OK) return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    return web_error(req, 404, "err.not_found");
}

static esp_err_t recovery_get(httpd_req_t *req) {
    const recov_t *r = recov_for_ui();
    char *b = s_page;                    /* static, like send_401: built and sent by one request at a time */
    wc_json_t w;
    wc_json_init(&w, b, sizeof s_page);
    wc_json_obj_open(&w);
    wc_json_str(&w, "lang", r->lang);
    wc_json_str(&w, "title", r->title);
    wc_json_str(&w, "text", r->text);
    wc_json_obj_close(&w);
    if (w.overflow) return web_error(req, 500, "err.internal");
    return web_json(req, 200, b);
}

esp_err_t web_start(settings_t *s) {
    if (s_srv) return ESP_OK;
    s_set = s;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = 80;
    c.max_open_sockets = 7;
    c.lru_purge_enable = true;
    c.max_uri_handlers = MAX_ROUTES;
    c.max_req_hdr_len = 4096;   /* WebDAV: Destination with a percent-encoded Korean path, long client headers */
    c.stack_size = 8192;
    c.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t e = httpd_start(&s_srv, &c);
    if (e != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e)); s_srv = NULL; return e; }
    web_route("/", HTTP_GET, WEB_PUBLIC, static_get);
    web_route("/assets/*", HTTP_GET, WEB_PUBLIC, static_get);
    web_route("/api/recovery", HTTP_GET, WEB_PUBLIC, recovery_get);
    api_files_register(s);
    api_settings_register(s);
    webdav_register();
    provision_web_ready();
    ESP_LOGI(TAG, "HTTP server on :80");
    return ESP_OK;
}
