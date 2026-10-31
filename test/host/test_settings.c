/* The defaults settings_load() hands out when NVS is empty, and that a stored value still overrides them.
 * There is one media-change strategy: after a rebuild the controller is taken off the bus for 500 ms
 * (usb_msc_vbus) so the host sees a real unplug and re-reads the directory, as robbellie/embroidery-stick
 * does in do_usb_refresh(). Releasing D+ alone was invisible to some hosts. It is not configurable, so it
 * has no place in settings_t and no test here. */
#include "t.h"
#include "settings.h"
#include "nvs_stub.h"
#include <string.h>

static void defaults(void) {
    nvs_stub_reset();
    settings_t s;
    memset(&s, 0xEE, sizeof s);                /* settings_load must fully overwrite the struct */
    settings_load(&s);
    T_EQ_INT(s.rename, true);
    T_EQ_INT(s.lang, NM_LANG_EN);
    T_EQ_INT(s.auto_refresh, true);
    T_EQ_INT(s.update_check, true);
    T_EQ_INT(s.tz_min, 0);
    T_CHECK(!s.pw_set);
    T_EQ_STR(s.wifi_ssid, "");
    T_EQ_STR(s.wifi_pw, "");
    T_EQ_STR(s.ui_lang, "");
}

static void stored_value_wins(void) {
    nvs_stub_reset();
    nvs_stub_set_u8("rename", 0);
    nvs_stub_set_u8("autoref", 0);
    settings_t s;
    settings_load(&s);
    T_EQ_INT(s.rename, false);
    T_EQ_INT(s.auto_refresh, false);
    T_EQ_INT(s.update_check, true);            /* untouched keys keep the default */
}

static void round_trip(void) {
    nvs_stub_reset();
    settings_t s;
    settings_load(&s);
    s.lang = NM_LANG_KO;
    s.tz_min = -540;
    s.auto_refresh = false;                    /* non-default, so the round trip actually proves the store */
    strcpy(s.wifi_ssid, "net");
    T_EQ_INT(settings_save(&s), 0);
    settings_t r;
    settings_load(&r);
    T_EQ_INT(r.lang, NM_LANG_KO);
    T_EQ_INT(r.tz_min, -540);
    T_EQ_INT(r.auto_refresh, false);
    T_EQ_STR(r.wifi_ssid, "net");
    T_EQ_INT(r.rename, true);                  /* untouched fields keep the defaults */
}

static void factory_reset_keeps_ap_pw(void) {
    nvs_stub_reset();
    settings_t s;
    settings_load(&s);
    strcpy(s.ap_pw, "12345678");
    s.auto_refresh = false;
    T_EQ_INT(settings_save(&s), 0);
    settings_factory_reset();
    settings_t r;
    settings_load(&r);
    T_EQ_INT(r.auto_refresh, true);            /* reset returns to the default */
    T_EQ_STR(r.ap_pw, "12345678");
}

int main(void) {
    T_RUN(defaults); T_RUN(stored_value_wins); T_RUN(round_trip); T_RUN(factory_reset_keeps_ap_pw);
    T_DONE();
}
