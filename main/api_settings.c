/* Status / settings / password / Wi-Fi / time / refresh / help API. esp_http_server runs every handler on its one
 * task, so the static buffers below are never used concurrently. Settings changes are saved from a copy first and
 * applied to the live struct only when the save worked. */
#include "api_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "api_list_core.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "netif.h"
#include "refresh.h"
#include "sdcard.h"
#include "setupfile.h"
#include "settings_json.h"
#include "setup_apply.h"
#include "timekeep.h"
#include "web.h"
#include "webcore.h"

static const char *TAG = "api_settings";
static settings_t *s_set;

#define BODY_MAX 1024
#define HELP_FILE SD_MOUNT "/silbap-help.txt"
#define HELP_CHUNK 4096            /* well under the 16 KiB allowed per sdcard_lock hold */
#define HELP_MAX   (64 * 1024)     /* a bigger file on the card is ignored (built-in text instead) */
#define HELP_BUILTIN 16384

static char s_body[BODY_MAX];
static char s_out[1024];
static char s_chunk[HELP_CHUNK];

static esp_err_t s_body_rc;
static bool read_body(httpd_req_t *req) { return web_read_body(req, s_body, sizeof s_body, "err.bad_request", &s_body_rc); }
#define BODY_OR_RETURN(req) do { if (!read_body(req)) return s_body_rc; } while (0)

static const char *version(void) { return esp_app_get_description()->version; }
static const char *LANG_NAME[] = { "en", "ko", "zh", "ja" };   /* nm_lang_t order */

static esp_err_t send_obj(httpd_req_t *req, const wc_json_t *w) {
    if (w->overflow) return web_error(req, 500, "err.internal");
    return web_json(req, 200, w->buf);
}

/* Room on the 16 GiB volume, so the SPA can warn without listing a folder. esp_vfs_fat_info is the same call
 * api_files.c and fsops use: it walks the mounted FatFs volume, not raw sectors, so it takes the lock like any other
 * FS access. A card whose FSINFO has no free count makes it count the whole FAT (seconds on 16 GiB), but
 * refresh_prepare_sd already calls it at boot before USB is READY, and FatFs keeps the count, so later calls are cheap. */
static void volume_kb(bool sd_ok, int64_t *free_kb, int64_t *total_kb) {
    uint64_t total = 0, freeb = 0;
    *free_kb = *total_kb = 0;
    if (!sd_ok) return;
    sdcard_lock();
    esp_err_t e = esp_vfs_fat_info(SD_MOUNT, &total, &freeb);
    sdcard_unlock();
    if (e != ESP_OK) return;
    *free_kb = (int64_t)(freeb / 1024);
    *total_kb = (int64_t)(total / 1024);
}

/* ---- GET /api/status (public: no secrets, no password state beyond pw_set/pw_at_boot) ---- */
static esp_err_t status_get(httpd_req_t *req) {
    refresh_status_t st;
    refresh_status(&st);
    int64_t free_kb, total_kb;
    volume_kb(st.sd_ok, &free_kb, &total_kb);
    char ip[24];
    net_ip(ip, sizeof ip);
    wc_json_t w;
    wc_json_init(&w, s_out, sizeof s_out);
    wc_json_obj_open(&w);
    wc_json_str(&w, "version", version());
    wc_json_str(&w, "ip", ip);
    wc_json_str(&w, "host", net_hostname());
    wc_json_num(&w, "rssi", net_rssi());
    wc_json_str(&w, "ssid", s_set->wifi_ssid);
    wc_json_num(&w, "files", st.files);
    wc_json_num(&w, "dirs", st.dirs);
    wc_json_num(&w, "hidden", 0);
    wc_json_num(&w, "free_kb", free_kb);
    wc_json_num(&w, "total_kb", total_kb);
    wc_json_bool(&w, "busy", st.busy);
    wc_json_bool(&w, "sd_ok", st.sd_ok);
    wc_json_bool(&w, "fs_ok", st.started ? st.fs_ok : st.sd_ok);
    wc_json_bool(&w, "over_limit", false);
    wc_json_bool(&w, "low_power", net_low_power());
    wc_json_bool(&w, "time_synced", timekeep_synced());
    wc_json_bool(&w, "pw_set", s_set->pw_set);
    wc_json_bool(&w, "setup_applied", setup_apply_took_effect());
    wc_json_str(&w, "ui_lang", s_set->ui_lang);
    wc_json_obj_close(&w);
    return send_obj(req, &w);
}

/* ---- GET/POST /api/settings ---- */
static esp_err_t send_settings(httpd_req_t *req) {
    wc_json_t w;
    wc_json_init(&w, s_out, sizeof s_out);
    wc_json_obj_open(&w);
    wc_json_bool(&w, "rename", s_set->rename);
    wc_json_str(&w, "lang", LANG_NAME[s_set->lang & 3]);
    wc_json_str(&w, "ui_lang", s_set->ui_lang);
    wc_json_bool(&w, "auto_refresh", s_set->auto_refresh);
    wc_json_num(&w, "tz_min", s_set->tz_min);
    wc_json_str(&w, "wifi_ssid", s_set->wifi_ssid);
    wc_json_obj_close(&w);
    return send_obj(req, &w);
}
static esp_err_t settings_get(httpd_req_t *req) { return send_settings(req); }

static esp_err_t settings_post(httpd_req_t *req) {
    BODY_OR_RETURN(req);
    settings_t t = *s_set;
    bool rebuild = false;
    if (settings_merge_json(&t, s_body, &rebuild) != 0) return web_error(req, 400, "err.bad_request");
    if (settings_save(&t) != ESP_OK) return web_error(req, 500, "err.io");
    bool again = false;
    settings_merge_json(s_set, s_body, &again);   /* same input, same result: applies the touched fields only */
    memset(&t, 0, sizeof t);
    if (rebuild) refresh_request(REFRESH_FORCED);
    return send_settings(req);
}

/* ---- POST /api/password ---- */
static esp_err_t password_post(httpd_req_t *req) {
    BODY_OR_RETURN(req);
    char pw[SETTINGS_PW_MAX + 1];
    api_get_t g = api_json_get_str(s_body, "password", pw, sizeof pw);
    if (g != API_OK) return web_error(req, 400, g == API_INVALID ? "err.bad_password" : "err.bad_request");
    settings_t t = *s_set;
    if (!settings_set_password(&t, pw)) { memset(pw, 0, sizeof pw); return web_error(req, 400, "err.bad_password"); }
    memset(pw, 0, sizeof pw);
    esp_err_t e = settings_save(&t);
    if (e != ESP_OK) { memset(&t, 0, sizeof t); return web_error(req, 500, "err.io"); }
    /* web_decide reads these fields without a lock. Clearing: pw_set first (the open state is what was asked for).
     * Setting: salt and hash first, pw_set last; while an old password is replaced the check just fails (closed). */
    if (!t.pw_set) {
        s_set->pw_set = false;
        memset(s_set->pw_salt, 0, sizeof s_set->pw_salt);
        memset(s_set->pw_hash, 0, sizeof s_set->pw_hash);
    } else {
        memcpy(s_set->pw_salt, t.pw_salt, sizeof t.pw_salt);
        memcpy(s_set->pw_hash, t.pw_hash, sizeof t.pw_hash);
        s_set->pw_set = true;
    }
    memset(&t, 0, sizeof t);
    ESP_LOGI(TAG, "web password %s", s_set->pw_set ? "set" : "cleared");
    return web_json(req, 200, s_set->pw_set ? "{\"pw_set\":true}" : "{\"pw_set\":false}");
}

/* ---- POST /api/wifi ---- */
static esp_err_t wifi_post(httpd_req_t *req) {
    BODY_OR_RETURN(req);
    char ssid[34] = "", pw[66] = "";
    api_get_t a = api_json_get_str(s_body, "ssid", ssid, sizeof ssid);
    api_get_t b = api_json_get_str(s_body, "password", pw, sizeof pw);
    if (b == API_ABSENT) { pw[0] = 0; b = API_OK; }
    size_t sl = strlen(ssid), pl = strlen(pw);
    if (a != API_OK || b != API_OK || sl == 0 || sl > 32 || pl > 64 || (pl > 0 && pl < 8)) {
        memset(pw, 0, sizeof pw);
        return web_error(req, 400, "err.bad_wifi");
    }
    esp_err_t e = net_apply_credentials(ssid, pw);
    memset(pw, 0, sizeof pw);
    if (e != ESP_OK) return web_error(req, 500, "err.io");
    return web_json(req, 200, "{\"ok\":true}");
}

/* ---- POST /api/time ---- */
static esp_err_t time_post(httpd_req_t *req) {
    BODY_OR_RETURN(req);
    int64_t ms, tz;
    if (sj_get_int(s_body, "ms", &ms) != 1 || sj_get_int(s_body, "tz_min", &tz) != 1 || ms <= 0 || tz < -14 * 60 || tz > 14 * 60)
        return web_error(req, 400, "err.bad_request");
    bool set = timekeep_client_hint(ms, (int)tz);
    if (s_set->tz_min != (int16_t)tz) {
        settings_t t = *s_set;
        t.tz_min = (int16_t)tz;
        if (settings_save(&t) == ESP_OK) s_set->tz_min = (int16_t)tz;
        else ESP_LOGW(TAG, "saving tz_min failed");
        memset(&t, 0, sizeof t);
    }
    return web_json(req, 200, set ? "{\"set\":true}" : "{\"set\":false}");
}

/* ---- POST /api/refresh ---- */
static esp_err_t refresh_post(httpd_req_t *req) {
    refresh_request(REFRESH_FORCED);
    return web_json(req, 200, "{\"ok\":true}");
}

/* ---- GET /api/help: /sd/silbap-help.txt streamed read-only, else the built-in text ---- */
static esp_err_t help_get(httpd_req_t *req) {
    long size = -1;
    sdcard_lock();
    struct stat st;
    if (stat(HELP_FILE, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= HELP_MAX) size = (long)st.st_size;
    sdcard_unlock();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (size > 0) {
        long off = 0;
        while (off < size) {
            size_t got = 0;
            sdcard_lock();                       /* reopen per chunk: a pulled card only ends the text */
            FILE *f = fopen(HELP_FILE, "r");
            if (f) {
                if (fseek(f, off, SEEK_SET) == 0) got = fread(s_chunk, 1, sizeof s_chunk, f);
                fclose(f);
            }
            sdcard_unlock();
            if (got == 0) break;
            if (httpd_resp_send_chunk(req, s_chunk, (ssize_t)got) != ESP_OK) return ESP_FAIL;
            off += (long)got;
        }
        if (off > 0) return httpd_resp_send_chunk(req, NULL, 0);
        /* nothing readable: fall through to the built-in text (no chunk was sent yet) */
    }
    char *b = malloc(HELP_BUILTIN);
    if (!b) return web_error(req, 500, "err.internal");
    int n = sf_help_text(version(), b, HELP_BUILTIN);
    esp_err_t e = n < 0 ? web_error(req, 500, "err.internal") : httpd_resp_send(req, b, n);
    free(b);
    return e;
}

/* ---- GET /api/diag?file=NAME: the .silbap logs, for field diagnosis ----
 * .silbap is unreachable from every other route on purpose (it holds the catalog and the card's
 * bookkeeping), but the logs in it are the only evidence available when a machine misbehaves and
 * the dongle is the thing under test. Serve exactly two known names, authenticated, text/plain,
 * with no directory traversal: the name must match one of the two literals. */
static esp_err_t diag_get(httpd_req_t *req) {
    char q[40] = "", name[24] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "file", name, sizeof name) == ESP_OK) {
        const char *ok = (!strcmp(name, "usb-diag.log") || !strcmp(name, "msc-writes.log")) ? name : NULL;
        if (!ok) return web_error(req, 400, "err.bad_request");
        char path[96];
        snprintf(path, sizeof path, "%s/%s", SILBAP_DIR, ok);
        long size = -1;
        sdcard_lock();
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size <= 256 * 1024) size = (long)st.st_size;
        sdcard_unlock();
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (size < 0) return httpd_resp_send(req, "(no log yet)", HTTPD_RESP_USE_STRLEN);
        long off = 0;
        while (off < size) {
            size_t got = 0;
            sdcard_lock();                       /* reopen per chunk: a pulled card only ends the text */
            FILE *f = fopen(path, "r");
            if (f) {
                if (fseek(f, off, SEEK_SET) == 0) got = fread(s_chunk, 1, sizeof s_chunk, f);
                fclose(f);
            }
            sdcard_unlock();
            if (got == 0) break;
            if (httpd_resp_send_chunk(req, s_chunk, (ssize_t)got) != ESP_OK) return ESP_FAIL;
            off += (long)got;
        }
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    return web_error(req, 400, "err.bad_request");
}

void api_settings_register(settings_t *s) {
    s_set = s;
    web_route("/api/status", HTTP_GET, WEB_PUBLIC, status_get);
    web_route("/api/diag", HTTP_GET, WEB_AUTH, diag_get);
    web_route("/api/help", HTTP_GET, WEB_PUBLIC, help_get);
    web_route("/api/settings", HTTP_GET, WEB_AUTH, settings_get);
    web_route("/api/settings", HTTP_POST, WEB_AUTH, settings_post);
    web_route("/api/password", HTTP_POST, WEB_AUTH, password_post);
    web_route("/api/wifi", HTTP_POST, WEB_AUTH, wifi_post);
    web_route("/api/time", HTTP_POST, WEB_AUTH, time_post);
    web_route("/api/refresh", HTTP_POST, WEB_AUTH, refresh_post);
}
