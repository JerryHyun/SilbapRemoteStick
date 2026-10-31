#include "ssid_list.h"
#include <string.h>

#define REPLACEMENT "\xEF\xBF\xBD"   /* U+FFFD */

/* Length of the valid UTF-8 sequence starting at p (at most n bytes), or 0 when it is invalid or truncated.
 * Rejects overlong forms, surrogates and > U+10FFFF, so the output is always well-formed UTF-8. */
static size_t utf8_seq(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    size_t need;
    uint32_t cp, min;
    if ((c & 0xE0) == 0xC0) { need = 2; cp = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { need = 3; cp = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { need = 4; cp = c & 0x07; min = 0x10000; }
    else return 0;                                  /* continuation byte or 0xF8..0xFF */
    if (n < need) return 0;
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return need;
}

size_t ssid_sanitize(const uint8_t *in, size_t len, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return 0;
    size_t o = 0;
    out[0] = 0;
    if (!in) return 0;
    for (size_t i = 0; i < len;) {
        size_t k = utf8_seq(in + i, len - i);
        if (k == 0) {
            /* one replacement character for this byte; eat the continuation bytes that belonged to it */
            if (o + sizeof REPLACEMENT - 1 >= out_sz) break;
            memcpy(out + o, REPLACEMENT, sizeof REPLACEMENT - 1);
            o += sizeof REPLACEMENT - 1;
            i++;
            while (i < len && (in[i] & 0xC0) == 0x80) i++;
            continue;
        }
        if (o + k >= out_sz) break;
        memcpy(out + o, in + i, k);
        o += k;
        i += k;
    }
    out[o] = 0;
    return o;
}

size_t ssid_merge(ssid_ent_t *list, size_t n, size_t cap, const uint8_t *ssid, size_t len,
                  int rssi, unsigned channel, unsigned auth) {
    if (!list || !ssid || cap == 0) return n;
    ssid_ent_t e;
    if (ssid_sanitize(ssid, len, e.ssid, sizeof e.ssid) == 0) return n;
    e.rssi = (int8_t)(rssi < -128 ? -128 : rssi > 127 ? 127 : rssi);
    e.channel = (uint8_t)(channel > 255 ? 255 : channel);
    e.auth = (uint8_t)(auth > 255 ? 255 : auth);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].ssid, e.ssid)) continue;
        if (e.rssi > list[i].rssi) list[i] = e;     /* same SSID: keep the best signal */
        return n;
    }
    if (n >= cap) return n;
    list[n] = e;
    return n + 1;
}

static int rssi_cmp(int8_t a, int8_t b) { return a < b ? 1 : a > b ? -1 : 0; }   /* strongest first */

void ssid_sort(ssid_ent_t *list, size_t n) {
    if (!list) return;
    for (size_t i = 1; i < n; i++) {                /* insertion sort: n is a handful of entries */
        ssid_ent_t e = list[i];
        size_t j = i;
        while (j > 0) {
            int c = rssi_cmp(list[j - 1].rssi, e.rssi);
            if (c == 0) c = strcmp(list[j - 1].ssid, e.ssid);
            if (c <= 0) break;
            list[j] = list[j - 1];
            j--;
        }
        list[j] = e;
    }
}

ssid_auth_t ssid_auth_from_code(unsigned code) {
    switch (code) {
    case SSID_WIFI_AUTH_OPEN: return SSID_AUTH_OPEN;
    case SSID_WIFI_AUTH_WEP: return SSID_AUTH_WEP;
    case SSID_WIFI_AUTH_WPA_PSK: return SSID_AUTH_WPA;
    case SSID_WIFI_AUTH_WPA2_PSK:
    case SSID_WIFI_AUTH_WPA_WPA2_PSK: return SSID_AUTH_WPA2;
    case SSID_WIFI_AUTH_WPA3_PSK:
    case SSID_WIFI_AUTH_WPA2_WPA3_PSK:
    case SSID_WIFI_AUTH_WPA3_EXT_PSK:
    case SSID_WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return SSID_AUTH_WPA3;
    case SSID_WIFI_AUTH_OWE: return SSID_AUTH_OWE;   /* encrypted, no password */
    case SSID_WIFI_AUTH_ENTERPRISE:                 /* == WPA2-Enterprise */
    case SSID_WIFI_AUTH_WPA3_ENT_192:
    case SSID_WIFI_AUTH_WPA3_ENTERPRISE:
    case SSID_WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case SSID_WIFI_AUTH_WPA_ENTERPRISE: return SSID_AUTH_ENTERPRISE;
    default: return SSID_AUTH_UNKNOWN;               /* WAPI_PSK, DPP, future values */
    }
}

size_t ssid_list_finish(ssid_ent_t *list, size_t n, size_t cap) {
    if (!list || cap == 0) return 0;
    ssid_sort(list, n);                     /* sort first: the cap must keep the strongest, not the first seen */
    return n > cap ? cap : n;
}

const char *ssid_auth_name(unsigned auth) {
    switch (auth) {
    case SSID_AUTH_OPEN: return "open";
    case SSID_AUTH_WEP: return "wep";
    case SSID_AUTH_WPA: return "wpa";
    case SSID_AUTH_WPA2: return "wpa2";
    case SSID_AUTH_WPA3: return "wpa3";
    case SSID_AUTH_OWE: return "owe";
    case SSID_AUTH_ENTERPRISE: return "enterprise";
    default: return "unknown";
    }
}
