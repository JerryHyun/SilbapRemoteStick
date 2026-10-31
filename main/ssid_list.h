#pragma once
/* Pure helpers for the setup portal's Wi-Fi scan list (host-tested by test/host/test_ssid_list.c): SSIDs are bytes,
 * not guaranteed UTF-8, and the page is UTF-8; merge duplicates by SSID and order the result deterministically.
 * Nothing here touches ESP-IDF. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSID_LIST_NAME_MAX 96   /* 32 SSID bytes, each worst case one U+FFFD (3 bytes) */

typedef enum {
    SSID_AUTH_OPEN = 0, SSID_AUTH_WEP, SSID_AUTH_WPA, SSID_AUTH_WPA2, SSID_AUTH_WPA3,
    SSID_AUTH_OWE,                       /* encrypted, but no password: the page may leave it empty */
    SSID_AUTH_ENTERPRISE, SSID_AUTH_UNKNOWN,
} ssid_auth_t;

/* Mirror of wifi_auth_mode_t (main/ssid_list.c must not depend on ESP-IDF). The numbers are the ESP-IDF v5.5 enum
 * values; main/provision.c static-asserts each one against the real WIFI_AUTH_* so a future reorder cannot drift. */
typedef enum {
    SSID_WIFI_AUTH_OPEN = 0,
    SSID_WIFI_AUTH_WEP = 1,
    SSID_WIFI_AUTH_WPA_PSK = 2,
    SSID_WIFI_AUTH_WPA2_PSK = 3,
    SSID_WIFI_AUTH_WPA_WPA2_PSK = 4,
    SSID_WIFI_AUTH_ENTERPRISE = 5,          /* == WIFI_AUTH_WPA2_ENTERPRISE */
    SSID_WIFI_AUTH_WPA3_PSK = 6,
    SSID_WIFI_AUTH_WPA2_WPA3_PSK = 7,
    SSID_WIFI_AUTH_WAPI_PSK = 8,
    SSID_WIFI_AUTH_OWE = 9,
    SSID_WIFI_AUTH_WPA3_ENT_192 = 10,
    SSID_WIFI_AUTH_WPA3_EXT_PSK = 11,
    SSID_WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE = 12,
    SSID_WIFI_AUTH_DPP = 13,
    SSID_WIFI_AUTH_WPA3_ENTERPRISE = 14,
    SSID_WIFI_AUTH_WPA2_WPA3_ENTERPRISE = 15,
    SSID_WIFI_AUTH_WPA_ENTERPRISE = 16,
} ssid_wifi_auth_t;

typedef struct {
    char ssid[SSID_LIST_NAME_MAX + 1];   /* sanitized: valid UTF-8, never empty */
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;                        /* ssid_auth_t */
} ssid_ent_t;

/* Copies in[0..len) to out as valid UTF-8. Each invalid or truncated sequence (and its continuation bytes) becomes
 * one U+FFFD; a byte above 0x7F can never reach the output unchecked. Control bytes are kept (the JSON writer
 * escapes them). out is always NUL-terminated; returns strlen(out). */
size_t ssid_sanitize(const uint8_t *in, size_t len, char *out, size_t out_sz);

/* Adds one scan result: an empty SSID is ignored; a duplicate (byte-equal after sanitizing) keeps the stronger
 * RSSI (and its channel/auth); a full list keeps the first cap entries. Returns the number of entries in use.
 * Pass the record count as cap while merging (duplicates then never take a slot), then call ssid_list_finish(). */
size_t ssid_merge(ssid_ent_t *list, size_t n, size_t cap, const uint8_t *ssid, size_t len,
                  int rssi, unsigned channel, unsigned auth);

/* Sorts the deduplicated list (strongest first) and truncates it to cap. Returns the new length; NULL or a zero cap
 * gives 0. Truncating here, after dedup, means duplicate scan results cannot push a real network out. */
size_t ssid_list_finish(ssid_ent_t *list, size_t n, size_t cap);

/* Strongest first; equal RSSI ordered by SSID bytes, so the answer is deterministic. */
void ssid_sort(ssid_ent_t *list, size_t n);

/* Maps a wifi_auth_mode_t value (see ssid_wifi_auth_t) to the page's auth class. OWE is open-but-encrypted, so it
 * is its own class; the enterprise variants are enterprise; WAPI and DPP are unknown (the page cannot join them). */
ssid_auth_t ssid_auth_from_code(unsigned code);

/* "open" / "wep" / "wpa" / "wpa2" / "wpa3" / "owe" / "enterprise" / "unknown" (an enum value from the future
 * included). */
const char *ssid_auth_name(unsigned auth);
