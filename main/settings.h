#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "namemap.h"
typedef struct {
    bool rename; nm_lang_t lang; bool auto_refresh;   /* plan 1 */
    char wifi_ssid[33], wifi_pw[65];
    uint8_t pw_salt[16], pw_hash[32]; bool pw_set;
    int16_t tz_min;            /* minutes east of UTC */
    char ap_pw[9];             /* 8 digits, generated on first boot */
    char ui_lang[3];           /* "ko"/"en"/"zh"/"ja"; "" = not chosen yet */
} settings_t;
#define SETTINGS_PW_MAX 64
/* NVS namespace "silbap"; strings are stored as blobs. Defaults: rename=true, lang=EN,
 * auto_refresh=true, tz_min=0, no Wi-Fi, no password, ui_lang "". */
void      settings_load(settings_t *s);
esp_err_t settings_save(const settings_t *s);
/* "" clears the password. Returns false (and changes nothing) when pw is longer than 64 bytes. Fresh random salt. */
bool      settings_set_password(settings_t *s, const char *pw);
/* false when no password is set, pw is longer than 64 bytes, or it does not match (constant-time compare) */
bool      settings_check_password(const settings_t *s, const char *pw);
/* Erase the whole namespace except ap_pw (the setup AP password stays what is printed/known) and last_time (clock floor). */
void      settings_factory_reset(void);
/* 8 random digits into out[9] */
void      settings_gen_ap_pw(char out[9]);
/* Last known good time (Unix s) in its own NVS key "last_time", so the hourly update does not rewrite the settings.
 * load: leaves *out untouched and returns ESP_ERR_NVS_NOT_FOUND when never saved. */
esp_err_t settings_load_last_time(int64_t *out);
esp_err_t settings_save_last_time(int64_t t);
