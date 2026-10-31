#include "settings_json.h"
#include <ctype.h>
#include <string.h>
#include "api_list_core.h"

static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

/* p at the opening quote; returns the char after the closing quote, NULL when unterminated. */
static const char *skip_str(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

/* p at a value; returns the char after it, NULL when malformed. */
static const char *skip_value(const char *p) {
    if (*p == '"') return skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { p = skip_str(p); if (!p) return NULL; p--; }
            else if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return NULL;
    }
    const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ']' && !isspace((unsigned char)*q)) q++;
    return q == p ? NULL : q;
}

/* First top-level member `key`: *val = start of its value, or NULL when absent. false when json is not one well-formed object. */
static bool find_raw(const char *json, const char *key, const char **val) {
    size_t kl = strlen(key);
    *val = NULL;
    const char *p = ws(json);
    if (*p != '{') return false;
    p = ws(p + 1);
    if (*p == '}') return *ws(p + 1) == 0;
    for (;;) {
        if (*p != '"') return false;
        const char *ks = p + 1, *ke = skip_str(p);
        if (!ke) return false;
        bool match = (size_t)(ke - 1 - ks) == kl && !memcmp(ks, key, kl);   /* keys with escapes never match */
        p = ws(ke);
        if (*p++ != ':') return false;
        p = ws(p);
        const char *vs = p;
        p = skip_value(p);
        if (!p) return false;
        if (match && !*val) *val = vs;
        p = ws(p);
        if (*p == ',') { p = ws(p + 1); continue; }
        return *p == '}' && *ws(p + 1) == 0;
    }
}

int sj_get_int(const char *json, const char *key, int64_t *out) {
    const char *v;
    if (!find_raw(json, key, &v)) return -1;
    if (!v) return 0;
    const char *p = v;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    if (!isdigit((unsigned char)*p)) return -1;
    int64_t n = 0;
    for (; isdigit((unsigned char)*p); p++) {
        if (n > (INT64_MAX - 9) / 10) return -1;
        n = n * 10 + (*p - '0');
    }
    if (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) return -1;
    *out = neg ? -n : n;
    return 1;
}

/* 1 = found bool, 0 = absent, -1 = wrong type/malformed */
static int get_bool(const char *json, const char *key, bool *out) {
    const char *v;
    if (!find_raw(json, key, &v)) return -1;
    if (!v) return 0;
    if (!strncmp(v, "true", 4) && !isalnum((unsigned char)v[4])) { *out = true; return 1; }
    if (!strncmp(v, "false", 5) && !isalnum((unsigned char)v[5])) { *out = false; return 1; }
    return -1;
}

static int lang_index(const char *s) {
    static const char *const L[] = { "en", "ko", "zh", "ja" };   /* nm_lang_t order */
    for (int i = 0; i < 4; i++) if (!strcmp(s, L[i])) return i;
    return -1;
}

int settings_merge_json(settings_t *s, const char *json, bool *needs_rebuild) {
    bool rename = s->rename, auto_refresh = s->auto_refresh, rebuild = false;
    nm_lang_t lang = s->lang;
    int16_t tz = s->tz_min;
    char ui[3] = "", str[16];
    bool has_ui = false, has_tz = false;
    int r;
    if ((r = get_bool(json, "rename", &rename)) < 0) return -1;
    if (r && rename != s->rename) rebuild = true;
    if (get_bool(json, "auto_refresh", &auto_refresh) < 0) return -1;
    api_get_t g = api_json_get_str(json, "lang", str, sizeof str);
    if (g == API_INVALID) return -1;
    if (g == API_OK) {
        int i = lang_index(str);
        if (i < 0) return -1;
        if ((nm_lang_t)i != s->lang) rebuild = true;
        lang = (nm_lang_t)i;
    }
    g = api_json_get_str(json, "ui_lang", str, sizeof str);
    if (g == API_INVALID) return -1;
    if (g == API_OK) {
        if (lang_index(str) < 0) return -1;
        memcpy(ui, str, 3); has_ui = true;
    }
    int64_t n;
    r = sj_get_int(json, "tz_min", &n);
    if (r < 0 || (r && (n < -14 * 60 || n > 14 * 60))) return -1;
    if (r) { tz = (int16_t)n; has_tz = true; }
    if (g == API_ABSENT && r == 0) {                /* nothing matched: still reject malformed JSON */
        const char *v;
        if (!find_raw(json, "", &v)) return -1;
    }
    s->rename = rename; s->auto_refresh = auto_refresh;
    s->lang = lang;
    if (has_ui) memcpy(s->ui_lang, ui, 3);
    if (has_tz) s->tz_min = tz;
    if (rebuild && needs_rebuild) *needs_rebuild = true;
    return 0;
}
