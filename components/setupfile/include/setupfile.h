#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    bool has_ssid, has_wifi_pw, has_web_pw, has_rename, has_lang;
    char ssid[33], wifi_pw[65], web_pw[65];
    bool rename;                 /* on|off|true|false|1|0 (case-insensitive) */
    char lang[3];                /* ko|en|zh|ja */
    unsigned unknown_count; char unknown_first[32];   /* first unknown key, for the .done warning */
    unsigned bad_count;   char bad_first[32];         /* first known key with an invalid value */
} sf_config_t;
/* Duplicate keys: last valid one wins. Lines without '=' are ignored. Unknown keys are masked ("key=****") in the .done text.
 * data/len: raw file bytes (UTF-8 with/without BOM, UTF-16LE/BE with BOM). Returns 0 on success, -1 if undecodable. */
int  sf_parse(const uint8_t *data, size_t len, sf_config_t *out);
/* Writes the masked .done text (UTF-8, LF): same keys in file order, secret values replaced by "****",
 * then "# applied <iso time>" and one "# warning: ..." line per unknown/bad key. Returns bytes written or -1. */
int  sf_masked(const uint8_t *data, size_t len, const char *iso_time, char *out, size_t out_sz);
/* Full help text: first line "SilbapRemoteStick help v<version>" (leading 'v' of version stripped), then ko, en, zh, ja sections. */
int  sf_help_text(const char *version, char *out, size_t out_sz);
bool sf_help_is_current(const char *existing_first_line, const char *version);
