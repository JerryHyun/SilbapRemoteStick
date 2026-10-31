#include "t.h"
#include "api_list_core.h"

static void entries(void) {
    char b[512]; wc_json_t w; wc_json_init(&w, b, sizeof b);
    api_list_entry("장미.jef", false, 70000, 1, true, NM_LANG_KO, &w);
    T_CHECK(strstr(b, "\"machine\":\"JANGMI.JEF\"")); T_CHECK(strstr(b, "\"hidden_reason\":\"\""));
    T_CHECK(strstr(b, "\"name\":\"장미.jef\"")); T_CHECK(strstr(b, "\"dir\":false")); T_CHECK(strstr(b, "\"size\":70000"));
    T_CHECK(b[0] == '{' && b[strlen(b) - 1] == '}');
    wc_json_init(&w, b, sizeof b); api_list_entry("Thumbs.db", false, 1, 1, true, NM_LANG_KO, &w);
    T_CHECK(strstr(b, "\"hidden_reason\":\"hidden\""));
    wc_json_init(&w, b, sizeof b); api_list_entry("장미.jef", false, 1, 1, false, NM_LANG_KO, &w);
    T_CHECK(strstr(b, "\"machine\":\"장미.jef\""));
    wc_json_init(&w, b, sizeof b); api_list_entry("big.jef", false, API_MAX_FILE_BYTES, 1, true, NM_LANG_EN, &w);
    T_CHECK(strstr(b, "\"hidden_reason\":\"too_big\""));
    wc_json_init(&w, b, sizeof b); api_list_entry("deep", true, 0, 9, true, NM_LANG_EN, &w);
    T_CHECK(strstr(b, "\"hidden_reason\":\"too_deep\"")); T_CHECK(strstr(b, "\"dir\":true"));
    wc_json_init(&w, b, sizeof b); api_list_entry("deep", true, 0, 8, true, NM_LANG_EN, &w);
    T_CHECK(strstr(b, "\"hidden_reason\":\"\""));
    wc_json_init(&w, b, sizeof b); api_list_entry("a\"b.jef", false, 1, 1, true, NM_LANG_EN, &w);
    T_CHECK(strstr(b, "\"name\":\"a\\\"b.jef\""));
    /* fields variant leaves the object open */
    wc_json_init(&w, b, sizeof b); wc_json_obj_open(&w);
    api_list_entry_fields("a.jef", false, 5, 1, true, NM_LANG_EN, &w); wc_json_num(&w, "mtime", 7); wc_json_obj_close(&w);
    T_CHECK(strstr(b, "\"mtime\":7}") && !w.overflow);
}
static void urldec(void) {
    char o[32];
    T_CHECK(api_url_decode("a%20b%2Fc", 9, o, sizeof o)); T_EQ_STR(o, "a b/c");
    T_CHECK(api_url_decode("%EC%9E%A5", 9, o, sizeof o)); T_EQ_STR(o, "장");
    T_CHECK(api_url_decode("a+b", 3, o, sizeof o)); T_EQ_STR(o, "a+b");
    T_CHECK(!api_url_decode("%zz", 3, o, sizeof o)); T_CHECK(!api_url_decode("%4", 2, o, sizeof o));
    T_CHECK(!api_url_decode("%00", 3, o, sizeof o)); T_CHECK(!api_url_decode("abcdef", 6, o, 4));
    T_CHECK(api_query_get("path=EMB%2Fa&name=x%20y", "name", o, sizeof o) == API_OK); T_EQ_STR(o, "x y");
    T_CHECK(api_query_get("path=EMB%2Fa&name=x", "path", o, sizeof o) == API_OK); T_EQ_STR(o, "EMB/a");
    T_CHECK(api_query_get("path=&n=1", "path", o, sizeof o) == API_OK); T_EQ_STR(o, "");
    T_CHECK(api_query_get("xpath=1", "path", o, sizeof o) == API_ABSENT); T_CHECK(api_query_get("a=1", "path", o, sizeof o) == API_ABSENT);
    T_CHECK(api_query_get("path=%zz", "path", o, sizeof o) == API_INVALID); T_CHECK(api_query_get("a=1&path=%4", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_query_get("path=abcdefghij", "path", o, 5) == API_INVALID); T_CHECK(api_query_get("path=a%00b", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_query_get("hidden", "hidden", o, sizeof o) == API_OK && !o[0]);
}
static void jsonget(void) {
    char o[32];
    T_CHECK(api_json_get_str("{\"path\":\"EMB/a.jef\",\"name\":\"b.jef\"}", "name", o, sizeof o) == API_OK); T_EQ_STR(o, "b.jef");
    T_CHECK(api_json_get_str(" { \"path\" : \"x\\\"y\\\\z\\u0041\" } ", "path", o, sizeof o) == API_OK); T_EQ_STR(o, "x\"y\\zA");
    T_CHECK(api_json_get_str("{\"p\":\"\\uD83D\\uDE00\"}", "p", o, sizeof o) == API_OK); T_EQ_STR(o, "\xF0\x9F\x98\x80");
    T_CHECK(api_json_get_str("{\"p\":\"장미\"}", "p", o, sizeof o) == API_OK); T_EQ_STR(o, "장미");
    T_CHECK(api_json_get_str("{\"n\":1,\"o\":{\"path\":\"in\"},\"path\":\"top\"}", "path", o, sizeof o) == API_OK); T_EQ_STR(o, "top");
    T_CHECK(api_json_get_str("{\"a\":\"{\\\"path\\\":\\\"x\\\"}\",\"path\":\"y\"}", "path", o, sizeof o) == API_OK); T_EQ_STR(o, "y");
    T_CHECK(api_json_get_str("{\"path\":5}", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("{}", "path", o, sizeof o) == API_ABSENT);
    T_CHECK(api_json_get_str("{\"a\":\"x\"}", "path", o, sizeof o) == API_ABSENT);
    T_CHECK(api_json_get_str("{\"path\":\"abc", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("[1]", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("{\"a\":1,,}", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("{\"a\" 1}", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("{\"path\":\"0123456789\"}", "path", o, 5) == API_INVALID);
    T_CHECK(api_json_get_str("{\"path\":\"\\u0000\"}", "path", o, sizeof o) == API_INVALID);
    T_CHECK(api_json_get_str("{\"path\":\"\\uD83D\"}", "path", o, sizeof o) == API_INVALID);
}
int main(void) { T_RUN(entries); T_RUN(urldec); T_RUN(jsonget); T_DONE(); }
