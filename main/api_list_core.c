#include "api_list_core.h"
#include <string.h>
#include "fs_names.h"

void api_list_entry_fields(const char *name, bool dir, uint64_t size, int depth, bool rename, nm_lang_t lang, wc_json_t *w) {
    const char *reason = "";
    if (fs_hidden_name(name)) reason = "hidden";
    else if (dir && depth > API_MAX_DEPTH) reason = "too_deep";
    else if (!dir && size >= API_MAX_FILE_BYTES) reason = "too_big";
    char machine[NM_LFN_MAX + 1];
    const char *m = name;
    if (reason[0] == 'h') m = "";
    else if (rename) { nm_romanize(name, lang, machine); m = machine; }
    wc_json_str(w, "name", name);
    wc_json_str(w, "machine", m);
    wc_json_bool(w, "dir", dir);
    wc_json_num(w, "size", dir ? 0 : (int64_t)size);
    wc_json_str(w, "hidden_reason", reason);
}

void api_list_entry(const char *name, bool dir, uint64_t size, int depth, bool rename, nm_lang_t lang, wc_json_t *w) {
    wc_json_obj_open(w);
    api_list_entry_fields(name, dir, size, depth, rename, lang, w);
    wc_json_obj_close(w);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool api_url_decode(const char *in, size_t n, char *out, size_t out_sz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= n) return false;
            int h = hexv(in[i + 1]), l = hexv(in[i + 2]);
            if (h < 0 || l < 0) return false;
            c = (char)(h * 16 + l);
            i += 2;
        }
        if (c == 0) return false;
        if (o + 1 >= out_sz) return false;
        out[o++] = c;
    }
    if (o >= out_sz) return false;
    out[o] = 0;
    return true;
}

api_get_t api_query_get(const char *query, const char *key, char *out, size_t out_sz) {
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *e = p + strcspn(p, "&");
        if ((size_t)(e - p) >= kl && !strncmp(p, key, kl) && (p[kl] == '=' || p + kl == e)) {
            const char *v = p + kl + (p[kl] == '=' ? 1 : 0);
            if (v > e) v = e;
            return api_url_decode(v, (size_t)(e - v), out, out_sz) ? API_OK : API_INVALID;
        }
        p = *e ? e + 1 : e;
    }
    return API_ABSENT;
}

/* ---- flat JSON ---- */
static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static void put_utf8(uint32_t cp, char *o, size_t *n) {
    if (cp < 0x80) o[(*n)++] = (char)cp;
    else if (cp < 0x800) { o[(*n)++] = (char)(0xC0 | (cp >> 6)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o[(*n)++] = (char)(0xE0 | (cp >> 12)); o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
    else { o[(*n)++] = (char)(0xF0 | (cp >> 18)); o[(*n)++] = (char)(0x80 | ((cp >> 12) & 63)); o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63)); o[(*n)++] = (char)(0x80 | (cp & 63)); }
}

static int hex4(const char *p, uint32_t *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) { int h = hexv(p[i]); if (h < 0) return 0; *v = *v * 16 + (uint32_t)h; }
    return 1;
}

/* Parses the string starting at the opening quote *pp; out may be NULL (skip). Advances *pp past the closing quote. */
static bool parse_str(const char **pp, char *out, size_t out_sz) {
    const char *p = *pp;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p != '"') {
        if (!*p) return false;
        char tmp[4]; size_t tn = 0;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': case '\\': case '/': tmp[tn++] = *p++; break;
            case 'b': tmp[tn++] = '\b'; p++; break;
            case 'f': tmp[tn++] = '\f'; p++; break;
            case 'n': tmp[tn++] = '\n'; p++; break;
            case 'r': tmp[tn++] = '\r'; p++; break;
            case 't': tmp[tn++] = '\t'; p++; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(p + 1, &cp)) return false;
                p += 5;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t lo;
                    if (p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else if (cp >= 0xDC00 && cp < 0xE000) return false;
                if (cp == 0) return false;
                put_utf8(cp, tmp, &tn);
                break;
            }
            default: return false;
            }
        } else tmp[tn++] = *p++;
        if (out) {
            if (o + tn >= out_sz) return false;
            memcpy(out + o, tmp, tn);
        }
        o += tn;
    }
    if (out) out[o] = 0;
    *pp = p + 1;
    return true;
}

/* Skips one JSON value of any kind (strings, nested containers, scalars). */
static bool skip_value(const char **pp, int depth) {
    const char *p = skip_ws(*pp);
    if (depth > 8) return false;
    if (*p == '"') { if (!parse_str(&p, NULL, 0)) return false; }
    else if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = skip_ws(p + 1);
        while (*p != close) {
            if (close == '}') {
                if (!parse_str(&p, NULL, 0)) return false;
                p = skip_ws(p);
                if (*p++ != ':') return false;
            }
            if (!skip_value(&p, depth + 1)) return false;
            p = skip_ws(p);
            if (*p == ',') p = skip_ws(p + 1);
            else if (*p != close) return false;
        }
        p++;
    } else {
        const char *s = p;
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (p == s) return false;
    }
    *pp = p;
    return true;
}

api_get_t api_json_get_str(const char *json, const char *key, char *out, size_t out_sz) {
    const char *p = skip_ws(json);
    if (*p++ != '{') return API_INVALID;
    p = skip_ws(p);
    char k[32];
    while (*p != '}') {
        bool kok = parse_str(&p, k, sizeof k);   /* a key too long for k cannot be one we ask for */
        if (!kok) {
            const char *q = p;
            if (*q != '"' || !parse_str(&q, NULL, 0)) return API_INVALID;
            p = q; k[0] = 0;
        }
        p = skip_ws(p);
        if (*p++ != ':') return API_INVALID;
        p = skip_ws(p);
        if (kok && !strcmp(k, key)) return parse_str(&p, out, out_sz) ? API_OK : API_INVALID;
        if (!skip_value(&p, 0)) return API_INVALID;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
        else if (*p != '}') return API_INVALID;
    }
    return API_ABSENT;
}
