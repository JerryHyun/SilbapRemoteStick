#include "t.h"
#include "setupfile.h"
static void test_setup_file_basic(void) {
    const char *t = "# comment\r\nWIFI_SSID=My Home\r\nwifi_password=a=b c d e\r\nweb_password=\r\nrename=OFF\r\nlanguage=ko\r\nfoo=1\r\n";
    sf_config_t c; T_EQ_INT(sf_parse((const uint8_t *)t, strlen(t), &c), 0);
    T_CHECK(c.has_ssid); T_EQ_STR(c.ssid, "My Home");
    T_CHECK(c.has_wifi_pw); T_EQ_STR(c.wifi_pw, "a=b c d e");
    T_CHECK(c.has_web_pw); T_EQ_STR(c.web_pw, "");          /* empty = remove the password */
    T_CHECK(c.has_rename && !c.rename); T_EQ_STR(c.lang, "ko");
    T_EQ_INT(c.unknown_count, 1); T_EQ_STR(c.unknown_first, "foo");
}
static void test_setup_file_bom_utf16(void) {
    const uint8_t u8bom[] = {0xEF,0xBB,0xBF,'r','e','n','a','m','e','=','o','n','\n'};
    sf_config_t c; T_EQ_INT(sf_parse(u8bom, sizeof u8bom, &c), 0); T_CHECK(c.has_rename && c.rename);
    const uint8_t u16le[] = {0xFF,0xFE,'l',0,'a',0,'n',0,'g',0,'u',0,'a',0,'g',0,'e',0,'=',0,'j',0,'a',0,'\r',0,'\n',0};
    T_EQ_INT(sf_parse(u16le, sizeof u16le, &c), 0); T_EQ_STR(c.lang, "ja");
    const uint8_t u16be[] = {0xFE,0xFF,0,'r',0,'e',0,'n',0,'a',0,'m',0,'e',0,'=',0,'1'};
    T_EQ_INT(sf_parse(u16be, sizeof u16be, &c), 0); T_CHECK(c.rename);
}
static void test_setup_file_bad_values(void) {
    const char *t = "rename=maybe\nlanguage=fr\nwifi_ssid=" "123456789012345678901234567890123" "\n";
    sf_config_t c; T_EQ_INT(sf_parse((const uint8_t *)t, strlen(t), &c), 0);
    T_CHECK(!c.has_rename); T_CHECK(!c.has_lang); T_CHECK(!c.has_ssid); T_EQ_INT(c.bad_count, 3);
}
static void test_setup_file_mask(void) {
    const char *t = "wifi_ssid=Home\nwifi_password=secret\nweb_password=pw\nrename=on\n";
    char o[256]; T_CHECK(sf_masked((const uint8_t *)t, strlen(t), "2026-10-30T12:00:00", o, sizeof o) > 0);
    T_CHECK(strstr(o, "wifi_ssid=Home")); T_CHECK(strstr(o, "wifi_password=****")); T_CHECK(strstr(o, "web_password=****"));
    T_CHECK(!strstr(o, "secret")); T_CHECK(strstr(o, "# applied 2026-10-30T12:00:00"));
}
static void test_help_text(void) {
    static char o[16384]; T_CHECK(sf_help_text("v0.2.0", o, sizeof o) > 0);
    T_CHECK(!strncmp(o, "SilbapRemoteStick help v0.2.0\n", 30));   /* leading 'v' of the version is not doubled */
    T_CHECK(strstr(o, "BOOT")); T_CHECK(strstr(o, "silbap-setup.txt")); T_CHECK(strstr(o, "FAT32"));
    T_CHECK(strstr(o, "한국어") || strstr(o, "[ko]")); T_CHECK(strstr(o, "[ja]"));
    T_CHECK(sf_help_is_current("SilbapRemoteStick help v0.2.0", "0.2.0"));
    T_CHECK(!sf_help_is_current("SilbapRemoteStick help v0.1.0", "0.2.0"));
}
/* extras: mask warnings, overflow, section sizes */
static void test_extras(void) {
    const char *t = "foo=1\nrename=maybe\nweb_password=x\n";
    char o[256]; T_CHECK(sf_masked((const uint8_t *)t, strlen(t), "T", o, sizeof o) > 0);
    T_CHECK(strstr(o, "# warning: unknown key 'foo'")); T_CHECK(strstr(o, "# warning: invalid value for 'rename'"));
    char small[8]; T_EQ_INT(sf_masked((const uint8_t *)t, strlen(t), "T", small, sizeof small), -1);
    static char h[16384]; T_CHECK(sf_help_text("0.2.0", h, sizeof h) > 0);
    T_EQ_INT(sf_help_text("0.2.0", small, sizeof small), -1);
    const char *tags[] = {"\n[ko]\n", "\n[en]\n", "\n[zh]\n", "\n[ja]\n", NULL};
    for (int i = 0; i < 4; i++) {
        const char *a = strstr(h, tags[i]); T_CHECK(a);
        const char *b = tags[i + 1] ? strstr(h, tags[i + 1]) : h + strlen(h);
        T_CHECK(a && b && (b - a) <= 2200);
    }
    T_CHECK(sf_help_is_current("SilbapRemoteStick help v0.2.0\r\n", "v0.2.0"));
}
static void test_fix1(void) {
    const char *t = "wifi_pasword=hunter22\nwifi_password=short\n";
    char o[256]; T_CHECK(sf_masked((const uint8_t *)t, strlen(t), "T", o, sizeof o) > 0);
    T_CHECK(!strstr(o, "hunter22")); T_CHECK(strstr(o, "wifi_pasword=****")); T_CHECK(!strstr(o, "short"));
    sf_config_t c; T_EQ_INT(sf_parse((const uint8_t *)t, strlen(t), &c), 0);
    T_CHECK(!c.has_wifi_pw); T_EQ_INT(c.bad_count, 1); T_EQ_INT(c.unknown_count, 1);
}
int main(void) {
    T_RUN(test_fix1);
    T_RUN(test_setup_file_basic); T_RUN(test_setup_file_bom_utf16); T_RUN(test_setup_file_bad_values);
    T_RUN(test_setup_file_mask); T_RUN(test_help_text); T_RUN(test_extras);
    T_DONE();
}
