#include "settings.h"
#include <string.h>
#include "nvs.h"
#include "esp_random.h"
#include "webcore.h"

#define NS "silbap"

static void get_str(nvs_handle_t h, const char *key, char *out, size_t cap) {
    char tmp[80];
    size_t n = sizeof tmp;
    if (cap > sizeof tmp || nvs_get_blob(h, key, tmp, &n) != ESP_OK || n >= cap) return;
    memcpy(out, tmp, n); out[n] = 0;
    out[strnlen(out, n)] = 0;                    /* an embedded NUL ends the string */
}
static esp_err_t set_str(nvs_handle_t h, const char *key, const char *v) { return nvs_set_blob(h, key, v, strlen(v)); }

void settings_load(settings_t *s) {
    memset(s, 0, sizeof *s);
    s->rename = true; s->lang = NM_LANG_EN; s->auto_refresh = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "rename", &v) == ESP_OK) s->rename = v != 0;
    if (nvs_get_u8(h, "lang", &v) == ESP_OK && v <= NM_LANG_JA) s->lang = (nm_lang_t)v;
    if (nvs_get_u8(h, "autoref", &v) == ESP_OK) s->auto_refresh = v != 0;
    int16_t tz;
    if (nvs_get_i16(h, "tz_min", &tz) == ESP_OK && tz >= -14 * 60 && tz <= 14 * 60) s->tz_min = tz;
    get_str(h, "wifi_ssid", s->wifi_ssid, sizeof s->wifi_ssid);
    get_str(h, "wifi_pw", s->wifi_pw, sizeof s->wifi_pw);
    get_str(h, "ap_pw", s->ap_pw, sizeof s->ap_pw);
    get_str(h, "ui_lang", s->ui_lang, sizeof s->ui_lang);
    size_t n = sizeof s->pw_salt, m = sizeof s->pw_hash;
    if (nvs_get_blob(h, "pw_salt", s->pw_salt, &n) == ESP_OK && n == sizeof s->pw_salt &&
        nvs_get_blob(h, "pw_hash", s->pw_hash, &m) == ESP_OK && m == sizeof s->pw_hash) s->pw_set = true;
    else { memset(s->pw_salt, 0, sizeof s->pw_salt); memset(s->pw_hash, 0, sizeof s->pw_hash); }
    nvs_close(h);
}

esp_err_t settings_save(const settings_t *s) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if ((e = nvs_set_u8(h, "rename", s->rename)) == ESP_OK &&
        (e = nvs_set_u8(h, "lang", (uint8_t)s->lang)) == ESP_OK &&
        (e = nvs_set_u8(h, "autoref", s->auto_refresh)) == ESP_OK &&
        (e = nvs_set_i16(h, "tz_min", s->tz_min)) == ESP_OK &&
        (e = set_str(h, "wifi_ssid", s->wifi_ssid)) == ESP_OK &&
        (e = set_str(h, "wifi_pw", s->wifi_pw)) == ESP_OK &&
        (e = set_str(h, "ap_pw", s->ap_pw)) == ESP_OK &&
        (e = set_str(h, "ui_lang", s->ui_lang)) == ESP_OK) {
        if (s->pw_set) {
            if ((e = nvs_set_blob(h, "pw_salt", s->pw_salt, sizeof s->pw_salt)) == ESP_OK)
                e = nvs_set_blob(h, "pw_hash", s->pw_hash, sizeof s->pw_hash);
        } else {
            nvs_erase_key(h, "pw_salt"); nvs_erase_key(h, "pw_hash");
        }
        if (e == ESP_OK) e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

bool settings_set_password(settings_t *s, const char *pw) {
    size_t n = strlen(pw);
    if (n > SETTINGS_PW_MAX) return false;
    if (n == 0) { s->pw_set = false; memset(s->pw_salt, 0, sizeof s->pw_salt); memset(s->pw_hash, 0, sizeof s->pw_hash); return true; }
    esp_fill_random(s->pw_salt, sizeof s->pw_salt);
    wc_pw_hash(pw, s->pw_salt, s->pw_hash);
    s->pw_set = true;
    return true;
}

bool settings_check_password(const settings_t *s, const char *pw) {
    if (!s->pw_set || strlen(pw) > SETTINGS_PW_MAX) return false;
    uint8_t h[32];
    wc_pw_hash(pw, s->pw_salt, h);
    return wc_ct_equal(h, s->pw_hash, sizeof h);
}

void settings_factory_reset(void) {
    char ap[9] = "";
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    get_str(h, "ap_pw", ap, sizeof ap);
    int64_t lt; bool have_lt = nvs_get_i64(h, "last_time", &lt) == ESP_OK;   /* the clock floor survives a reset too */
    nvs_erase_all(h);
    if (ap[0]) set_str(h, "ap_pw", ap);
    if (have_lt) nvs_set_i64(h, "last_time", lt);
    nvs_commit(h);
    nvs_close(h);
}

void settings_gen_ap_pw(char out[9]) {
    for (int i = 0; i < 8; i++) out[i] = (char)('0' + esp_random() % 10);
    out[8] = 0;
}

esp_err_t settings_load_last_time(int64_t *out) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_i64(h, "last_time", out);
    nvs_close(h);
    return e;
}
esp_err_t settings_save_last_time(int64_t t) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if ((e = nvs_set_i64(h, "last_time", t)) == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
