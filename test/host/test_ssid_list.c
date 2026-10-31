#include "t.h"
#include <stdint.h>
#include "ssid_list.h"

static void test_sanitize_ascii(void) {
    char o[SSID_LIST_NAME_MAX + 1];
    T_EQ_INT(ssid_sanitize((const uint8_t *)"MyNet", 5, o, sizeof o), 5);
    T_EQ_STR(o, "MyNet");

    /* The whole 32-byte SSID limit survives. */
    const char *n32 = "0123456789abcdef0123456789abcdef";
    T_EQ_INT(ssid_sanitize((const uint8_t *)n32, 32, o, sizeof o), 32);
    T_EQ_STR(o, n32);
}

static void test_sanitize_utf8(void) {
    char o[SSID_LIST_NAME_MAX + 1];
    /* Korean "장미": E1 84 8C E1 86 B4 (3 bytes each); 6 bytes in, 6 bytes out. */
    const uint8_t kr[] = { 0xE1, 0x84, 0x8C, 0xE1, 0x86, 0xB4 };
    T_EQ_INT(ssid_sanitize(kr, sizeof kr, o, sizeof o), 6);
    T_EQ_STR(o, "\xE1\x84\x8C\xE1\x86\xB4");
}

static void test_sanitize_invalid(void) {
    char o[SSID_LIST_NAME_MAX + 1];
    /* A lone continuation byte, a truncated 3-byte sequence and 0xFF each become one U+FFFD. */
    const uint8_t lone[] = { 'a', 0x80, 'b' };
    T_EQ_INT(ssid_sanitize(lone, sizeof lone, o, sizeof o), (long long)(sizeof "a\xEF\xBF\xBD" "b" - 1));
    T_EQ_STR(o, "a\xEF\xBF\xBD" "b");

    /* 0xE1 0x84 then end: one replacement, the continuation is eaten with it. */
    const uint8_t trunc[] = { 'x', 0xE1, 0x84 };
    T_EQ_STR((ssid_sanitize(trunc, sizeof trunc, o, sizeof o), o), "x\xEF\xBF\xBD");

    const uint8_t ff[] = { 0xC3, 0xA9, 0xFF };      /* é then 0xFF */
    T_EQ_STR((ssid_sanitize(ff, sizeof ff, o, sizeof o), o), "\xC3\xA9\xEF\xBF\xBD");

    /* Overlong form of '/' (0xC0 0xAF) and a surrogate (ED A0 80) are both invalid. */
    const uint8_t over[] = { 0xC0, 0xAF };
    T_EQ_STR((ssid_sanitize(over, sizeof over, o, sizeof o), o), "\xEF\xBF\xBD");
    const uint8_t sur[] = { 0xED, 0xA0, 0x80 };
    T_EQ_STR((ssid_sanitize(sur, sizeof sur, o, sizeof o), o), "\xEF\xBF\xBD");

    /* A run of stray continuation bytes collapses into one replacement character. */
    uint8_t all[32];
    memset(all, 0x80, sizeof all);
    T_EQ_INT(ssid_sanitize(all, sizeof all, o, sizeof o), 3);
    T_EQ_STR(o, "\xEF\xBF\xBD");

    /* Separated invalid bytes each get their own replacement. */
    uint8_t sep[5] = { 0x80, 'a', 0x80, 'b', 0x80 };
    T_EQ_STR((ssid_sanitize(sep, sizeof sep, o, sizeof o), o), "\xEF\xBF\xBD" "a\xEF\xBF\xBD" "b\xEF\xBF\xBD");
}

static void test_sanitize_bounds(void) {
    char o[8];
    /* Room is left for the NUL: with cap 8 at most 7 bytes are written. */
    T_EQ_INT(ssid_sanitize((const uint8_t *)"abcdefghij", 10, o, sizeof o), 7);
    T_EQ_STR(o, "abcdefg");
    /* No room for a whole replacement character: stop instead of writing half of it. */
    const uint8_t kr[] = { 'a', 0xE1, 0x84, 0x8C };
    T_EQ_INT(ssid_sanitize(kr, sizeof kr, o, 2), 1);
    T_EQ_STR(o, "a");
    /* NULL / zero-size output. */
    T_EQ_INT(ssid_sanitize((const uint8_t *)"x", 1, NULL, 8), 0);
    T_EQ_INT(ssid_sanitize((const uint8_t *)"x", 1, o, 0), 0);
    T_EQ_INT(ssid_sanitize(NULL, 4, o, sizeof o), 0);
}

static void test_merge(void) {
    ssid_ent_t l[8];
    size_t n = 0;
    n = ssid_merge(l, n, 8, (const uint8_t *)"Home", 4, -70, 1, SSID_AUTH_WPA2);
    n = ssid_merge(l, n, 8, (const uint8_t *)"Home", 4, -50, 6, SSID_AUTH_WPA2);   /* stronger: replaces */
    n = ssid_merge(l, n, 8, (const uint8_t *)"Home", 4, -80, 11, SSID_AUTH_OPEN);  /* weaker: ignored */
    T_EQ_INT(n, 1);
    T_EQ_STR(l[0].ssid, "Home");
    T_EQ_INT(l[0].rssi, -50);
    T_EQ_INT(l[0].channel, 6);

    n = ssid_merge(l, n, 8, (const uint8_t *)"Cafe", 4, -60, 1, SSID_AUTH_OPEN);
    T_EQ_INT(n, 2);

    /* Empty SSIDs are hidden networks: not offered. */
    n = ssid_merge(l, n, 8, (const uint8_t *)"", 0, -40, 1, SSID_AUTH_WPA2);
    T_EQ_INT(n, 2);

    /* Invalid UTF-8 merges with the same sanitized name. */
    n = ssid_merge(l, n, 8, (const uint8_t *)"Caf\x80", 4, -30, 1, SSID_AUTH_OPEN);
    T_EQ_INT(n, 3);
    T_EQ_STR(l[2].ssid, "Caf\xEF\xBF\xBD");

    /* The cap is honoured. */
    ssid_ent_t small[2];
    size_t m = 0;
    m = ssid_merge(small, m, 2, (const uint8_t *)"a", 1, -1, 1, 0);
    m = ssid_merge(small, m, 2, (const uint8_t *)"b", 1, -1, 1, 0);
    m = ssid_merge(small, m, 2, (const uint8_t *)"c", 1, -1, 1, 0);
    T_EQ_INT(m, 2);
}

static void test_sort(void) {
    ssid_ent_t l[3] = {
        { .ssid = "weak", .rssi = -90 },
        { .ssid = "zebra", .rssi = -40 },
        { .ssid = "alpha", .rssi = -40 },
    };
    ssid_sort(l, 3);
    T_EQ_STR(l[0].ssid, "alpha");   /* equal RSSI: by name */
    T_EQ_STR(l[1].ssid, "zebra");
    T_EQ_STR(l[2].ssid, "weak");
    ssid_sort(l, 0);
    ssid_sort(NULL, 3);
}

static void test_auth_name(void) {
    T_EQ_STR(ssid_auth_name(SSID_AUTH_OPEN), "open");
    T_EQ_STR(ssid_auth_name(SSID_AUTH_WPA2), "wpa2");
    T_EQ_STR(ssid_auth_name(SSID_AUTH_ENTERPRISE), "enterprise");
    /* OWE is encrypted but needs no password: the page offers it as connectable without one. */
    T_EQ_STR(ssid_auth_name(SSID_AUTH_OWE), "owe");
    T_EQ_STR(ssid_auth_name(250), "unknown");
}

static void test_auth_from_code(void) {
    /* Every wifi_auth_mode_t the scan API can return, including the ones that used to fall into the
     * ">= WIFI_AUTH_ENTERPRISE" bucket. */
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_OPEN), SSID_AUTH_OPEN);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WEP), SSID_AUTH_WEP);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA_PSK), SSID_AUTH_WPA);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA2_PSK), SSID_AUTH_WPA2);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA_WPA2_PSK), SSID_AUTH_WPA2);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA3_PSK), SSID_AUTH_WPA3);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA2_WPA3_PSK), SSID_AUTH_WPA3);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA3_EXT_PSK), SSID_AUTH_WPA3);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE), SSID_AUTH_WPA3);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_OWE), SSID_AUTH_OWE);          /* no password needed */
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_ENTERPRISE), SSID_AUTH_ENTERPRISE);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA3_ENT_192), SSID_AUTH_ENTERPRISE);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA3_ENTERPRISE), SSID_AUTH_ENTERPRISE);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA2_WPA3_ENTERPRISE), SSID_AUTH_ENTERPRISE);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WPA_ENTERPRISE), SSID_AUTH_ENTERPRISE);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_WAPI_PSK), SSID_AUTH_UNKNOWN);
    T_EQ_INT(ssid_auth_from_code(SSID_WIFI_AUTH_DPP), SSID_AUTH_UNKNOWN);
    T_EQ_INT(ssid_auth_from_code(200), SSID_AUTH_UNKNOWN);
}

static void test_finish(void) {
    /* The scan loop merges into a full-size array (one slot per driver record), so a duplicate never takes one of
     * the capped output slots; finish() sorts and only then truncates. */
    ssid_ent_t all[8];
    size_t n = 0;
    n = ssid_merge(all, n, 8, (const uint8_t *)"dup", 3, -90, 1, SSID_AUTH_WPA2);
    n = ssid_merge(all, n, 8, (const uint8_t *)"a", 1, -50, 1, SSID_AUTH_OPEN);
    n = ssid_merge(all, n, 8, (const uint8_t *)"dup", 3, -40, 6, SSID_AUTH_WPA2);   /* stronger duplicate */
    n = ssid_merge(all, n, 8, (const uint8_t *)"b", 1, -60, 1, SSID_AUTH_OPEN);
    n = ssid_merge(all, n, 8, (const uint8_t *)"c", 1, -70, 1, SSID_AUTH_OPEN);
    T_EQ_INT(n, 4);                                   /* dedup happened during merge, not truncation */
    n = ssid_list_finish(all, n, 3);
    T_EQ_INT(n, 3);
    T_EQ_STR(all[0].ssid, "dup");                     /* -40, strongest */
    T_EQ_INT(all[0].rssi, -40);
    T_EQ_INT(all[0].channel, 6);
    T_EQ_STR(all[1].ssid, "a");
    T_EQ_STR(all[2].ssid, "b");
    /* A cap larger than the list, an exact-for-nothing list, and a zero cap. */
    T_EQ_INT(ssid_list_finish(all, 2, 8), 2);
    T_EQ_INT(ssid_list_finish(all, 4, 4), 4);
    T_EQ_INT(ssid_list_finish(all, 4, 0), 0);
    T_EQ_INT(ssid_list_finish(NULL, 4, 2), 0);
}

int main(void) {
    T_RUN(test_sanitize_ascii);
    T_RUN(test_sanitize_utf8);
    T_RUN(test_sanitize_invalid);
    T_RUN(test_sanitize_bounds);
    T_RUN(test_merge);
    T_RUN(test_sort);
    T_RUN(test_auth_name);
    T_RUN(test_auth_from_code);
    T_RUN(test_finish);
    T_DONE();
}
