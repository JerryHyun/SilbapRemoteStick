#include "t.h"
#include "webcore.h"
static void test_norm_path(void) {
    char o[WC_PATH_MAX];
    T_CHECK(wc_norm_path("/EMB/Embf/", o, sizeof o)); T_EQ_STR(o, "EMB/Embf");
    T_CHECK(wc_norm_path("", o, sizeof o)); T_EQ_STR(o, "");
    T_CHECK(wc_norm_path("/꽃/작은 꽃", o, sizeof o)); T_EQ_STR(o, "꽃/작은 꽃");
    T_CHECK(!wc_norm_path("/a/../b", o, sizeof o));
    T_CHECK(!wc_norm_path("/a//b", o, sizeof o));
    T_CHECK(!wc_norm_path("/.silbap/catalog.bin", o, sizeof o));
    T_CHECK(!wc_norm_path("/x/.silbapX", o, sizeof o));
    T_CHECK(!wc_norm_path("/a\x01" "b", o, sizeof o));
    /* '\' is a separator to FatFs (ff.c IsSeparator), so a path that hides one behind a segment
     * test would resolve differently there than here: "\\.silbap\\log" normalised to itself and
     * FatFs then opened .silbap/log; "..\\..\\x" escaped the root the same way. */
    T_CHECK(!wc_norm_path("\\.silbap\\usb-diag.log", o, sizeof o));
    T_CHECK(!wc_norm_path("..\\..\\etc", o, sizeof o));
    T_CHECK(!wc_norm_path("EMB\\..\\..\\.silbap", o, sizeof o));
    T_CHECK(!wc_norm_path("EMB/Embf\\x.jef", o, sizeof o));
    T_CHECK(!wc_norm_path("a\\b", o, sizeof o));
    T_CHECK(wc_norm_path("/.hidden/x", o, sizeof o));
    T_CHECK(!wc_norm_path("/.SILBAP/x", o, sizeof o));
    T_CHECK(!wc_norm_path("/a/.Silbap", o, sizeof o));
    char lp[600]; memset(lp, 'a', sizeof lp);
    lp[0] = '/'; lp[512] = 0;  /* 511 chars */
    T_CHECK(wc_norm_path(lp, o, sizeof o));
    lp[512] = 'a'; lp[513] = 0;  /* 512 chars */
    T_CHECK(!wc_norm_path(lp, o, sizeof o));
}
static void test_sd_safe_name(void) {
    char o[300];
    T_CHECK(wc_sd_safe_name("a:b?.jef", o, sizeof o)); T_EQ_STR(o, "a：b？.jef");
    T_CHECK(wc_sd_safe_name("x|y\"z<>*\\.pes", o, sizeof o)); T_EQ_STR(o, "x｜y＂z＜＞＊＼.pes");
    T_CHECK(wc_sd_safe_name("name. . ", o, sizeof o)); T_EQ_STR(o, "name");
    T_CHECK(wc_sd_safe_name("tab\tname", o, sizeof o)); T_EQ_STR(o, "tab_name");
    T_CHECK(!wc_sd_safe_name("...", o, sizeof o));
    T_CHECK(!wc_sd_safe_name(".SilBap", o, sizeof o));
    char big[400]; memset(big, 'a', 256); big[256] = 0;
    T_CHECK(!wc_sd_safe_name(big, o, sizeof o));
    T_CHECK(wc_sd_safe_name("장미.jef", o, sizeof o)); T_EQ_STR(o, "장미.jef");
}
static void test_conflict_name(void) {
    char o[64];
    wc_conflict_name("rose.jef", 2, o, sizeof o); T_EQ_STR(o, "rose (2).jef");
    wc_conflict_name("noext", 3, o, sizeof o); T_EQ_STR(o, "noext (3)");
    wc_conflict_name(".hidden", 2, o, sizeof o); T_EQ_STR(o, ".hidden (2)");
    wc_conflict_name("a.b.dst", 10, o, sizeof o); T_EQ_STR(o, "a.b (10).dst");
}
static void test_host_origin_check(void) {
    const char *const ok[] = {"192.168.0.42", "silbapstick.local", NULL};
    T_CHECK(wc_host_ok("192.168.0.42", ok)); T_CHECK(wc_host_ok("192.168.0.42:80", ok));
    T_CHECK(wc_host_ok("SilbapStick.local", ok));
    T_CHECK(!wc_host_ok("evil.example", ok)); T_CHECK(!wc_host_ok(NULL, ok));
    T_CHECK(!wc_host_ok("192.168.0.42.evil.example", ok));
    T_CHECK(wc_origin_ok(NULL, ok));
    T_CHECK(wc_origin_ok("http://192.168.0.42", ok));
    T_CHECK(!wc_origin_ok("http://evil.example", ok));
    T_CHECK(!wc_origin_ok("null", ok));
    T_CHECK(!wc_origin_ok("https://192.168.0.42", ok));
}
static void test_basic_auth(void) {
    char p[64];
    T_CHECK(wc_basic_password("Basic dXNlcjpzZWNyZXQ=", p, sizeof p)); T_EQ_STR(p, "secret");  /* user:secret */
    T_CHECK(wc_basic_password("Basic OnB3", p, sizeof p)); T_EQ_STR(p, "pw");                    /* :pw */
    T_CHECK(!wc_basic_password("Bearer x", p, sizeof p));
    T_CHECK(!wc_basic_password("Basic !!!", p, sizeof p));
    T_CHECK(!wc_basic_password("Basic Zm9v====", p, sizeof p));
    T_CHECK(!wc_basic_password("Basic YQ=A", p, sizeof p));
    T_CHECK(!wc_basic_password("Basic dTpwAHg=", p, sizeof p));  /* u:p\0x */
    uint8_t salt[16] = {1}, a[32], b[32];
    wc_pw_hash("pw", salt, a); wc_pw_hash("pw", salt, b); T_CHECK(wc_ct_equal(a, b, 32));
    wc_pw_hash("pX", salt, b); T_CHECK(!wc_ct_equal(a, b, 32));
    uint8_t h[32]; wc_sha256("abc", 3, h);
    T_EQ_INT(h[0], 0xba); T_EQ_INT(h[31], 0xad);                      /* FIPS 180-2 "abc" */
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    wc_sha256(m, strlen(m), h);
    T_EQ_INT(h[0], 0x24); T_EQ_INT(h[1], 0x8d); T_EQ_INT(h[2], 0x6a); T_EQ_INT(h[3], 0x61); T_EQ_INT(h[31], 0xc1); T_EQ_INT(h[30], 0x06); T_EQ_INT(h[29], 0xdb); T_EQ_INT(h[28], 0x19);
}
static void test_json(void) {
    char b[128]; wc_json_t w; wc_json_init(&w, b, sizeof b);
    wc_json_obj_open(&w); wc_json_str(&w, "n", "a\"b\\\n장"); wc_json_num(&w, "s", -5);
    wc_json_arr_open(&w, "l"); wc_json_bool(&w, NULL, true); wc_json_arr_close(&w); wc_json_obj_close(&w);
    T_EQ_STR(b, "{\"n\":\"a\\\"b\\\\\\n장\",\"s\":-5,\"l\":[true]}");
    char s[8]; wc_json_init(&w, s, sizeof s); wc_json_obj_open(&w); wc_json_str(&w, "k", "toolong"); T_CHECK(w.overflow);
}

static void test_host_trailing_dot(void) {
    const char *const ok[] = {"192.168.0.42", "silbapstick.local", NULL};
    T_CHECK(wc_host_ok("silbapstick.local.", ok));
    T_CHECK(wc_host_ok("silbapstick.local.:80", ok));
    T_CHECK(!wc_host_ok("silbapstick.local..", ok));
    T_CHECK(!wc_host_ok(".", ok));
    T_CHECK(wc_origin_ok("http://silbapstick.local.", ok));
    T_CHECK(!wc_origin_ok("", ok));
}
int main(void) { T_RUN(test_norm_path); T_RUN(test_sd_safe_name); T_RUN(test_conflict_name); T_RUN(test_host_origin_check); T_RUN(test_basic_auth); T_RUN(test_json); T_RUN(test_host_trailing_dot); T_DONE(); }
