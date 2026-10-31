#include "t.h"
#include "settings_json.h"

static settings_t base(void) {
    settings_t s; memset(&s, 0, sizeof s);
    s.rename = true; s.lang = NM_LANG_EN; s.auto_refresh = true; s.update_check = true;
    return s;
}
static void merge(void) {
    settings_t s = base(); bool rb = false;
    T_EQ_INT(settings_merge_json(&s, "{\"foo\":1,\"bar\":{\"a\":[1,2,{\"lang\":\"ko\"}]},\"baz\":\"x\"}", &rb), 0);
    T_CHECK(!rb); T_CHECK(s.lang == NM_LANG_EN);               /* unknown keys and nested keys are ignored */
    T_EQ_INT(settings_merge_json(&s, "{\"lang\":\"ko\"}", &rb), 0);
    T_CHECK(rb); T_CHECK(s.lang == NM_LANG_KO);
    rb = false; T_EQ_INT(settings_merge_json(&s, "{ \"lang\" : \"ko\" }", &rb), 0); T_CHECK(!rb);   /* same value: no rebuild */
    rb = false; T_EQ_INT(settings_merge_json(&s, "{\"rename\":false}", &rb), 0); T_CHECK(rb); T_CHECK(!s.rename);
    rb = false; T_EQ_INT(settings_merge_json(&s, "{\"auto_refresh\":false,\"update_check\":false}", &rb), 0);
    T_CHECK(!rb); T_CHECK(!s.auto_refresh); T_CHECK(!s.update_check);
    T_EQ_INT(settings_merge_json(&s, "{\"ui_lang\":\"ja\",\"tz_min\":-540}", &rb), 0);
    T_EQ_STR(s.ui_lang, "ja"); T_EQ_INT(s.tz_min, -540);
    T_EQ_INT(settings_merge_json(&s, "{\"lang\":\"zh\",\"lang\":\"en\"}", &rb), 0); T_CHECK(s.lang == NM_LANG_ZH);   /* duplicates: first wins (same as api_json_get_str) */
    T_EQ_INT(settings_merge_json(&s, "{}", &rb), 0);
}
static void errors(void) {
    settings_t s = base(); bool rb = false;
    T_EQ_INT(settings_merge_json(&s, "{\"lang\":\"fr\"}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"ui_lang\":\"\"}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"rename\":\"yes\"}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"rename\":1}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"tz_min\":841}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"tz_min\":1.5}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"tz_min\":\"60\"}", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"lang\":\"ko\"", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "[1]", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "{\"lang\":\"ko\"} x", &rb), -1);
    T_EQ_INT(settings_merge_json(&s, "", &rb), -1);
    T_CHECK(!rb);
}
static void passwords(void) {
    settings_t s = base(); bool rb = false;
    s.pw_set = true; memset(s.pw_hash, 7, sizeof s.pw_hash); memset(s.pw_salt, 9, sizeof s.pw_salt);
    strcpy(s.wifi_ssid, "net"); strcpy(s.wifi_pw, "secret");
    T_EQ_INT(settings_merge_json(&s, "{\"password\":\"\",\"pw_set\":false,\"pw_hash\":\"x\",\"wifi_pw\":\"z\",\"wifi_ssid\":\"q\",\"ap_pw\":\"1\"}", &rb), 0);
    T_CHECK(s.pw_set); T_EQ_INT(s.pw_hash[0], 7); T_EQ_STR(s.wifi_pw, "secret"); T_EQ_STR(s.wifi_ssid, "net"); T_EQ_STR(s.ap_pw, "");
}
static void getint(void) {
    int64_t v = 5;
    T_EQ_INT(sj_get_int("{\"ms\":1700000000123,\"tz_min\":-540}", "ms", &v), 1); T_EQ_INT(v, 1700000000123LL);
    T_EQ_INT(sj_get_int("{\"ms\":1,\"tz_min\":-540}", "tz_min", &v), 1); T_EQ_INT(v, -540);
    T_EQ_INT(sj_get_int("{\"a\":1}", "ms", &v), 0);
    T_EQ_INT(sj_get_int("{\"ms\":\"1\"}", "ms", &v), -1);
    T_EQ_INT(sj_get_int("{\"ms\":1e3}", "ms", &v), -1);
    T_EQ_INT(sj_get_int("{\"ms\":-}", "ms", &v), -1);
    T_EQ_INT(sj_get_int("{\"ms\":99999999999999999999}", "ms", &v), -1);
    T_EQ_INT(sj_get_int("{\"ms\":1", "ms", &v), -1);
}
int main(void) { T_RUN(merge); T_RUN(errors); T_RUN(passwords); T_RUN(getint); T_DONE(); }
