#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bootloader_random.h"
#include "nvs_flash.h"
#include "board.h"
#include "button.h"
#include "fsops.h"
#include "lcd.h"
#include "lcd_ui.h"
#include "led.h"
#include "netif.h"
#include "refresh.h"
#include "sdcard.h"
#include "sdlog.h"
#include "settings.h"
#include "setup_apply.h"
#include "timekeep.h"
#include "usb_msc.h"
#include "web.h"

static const char *TAG = "main";
static settings_t s_set;

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    led_init();
    lcd_init();                                 /* before the settings load and the mount: black, not garbage, while the SD comes up */
    led_set(255, 160, 0);                       /* yellow: booting */
    settings_load(&s_set);
    if (strlen(s_set.ap_pw) != 8) {              /* once, kept in NVS; true randomness needs the RNG entropy source on (RF is still off here) */
        bootloader_random_enable();
        settings_gen_ap_pw(s_set.ap_pw);
        bootloader_random_disable();
        esp_err_t se = settings_save(&s_set);
        if (se != ESP_OK) ESP_LOGE(TAG, "saving the setup AP password failed: %s", esp_err_to_name(se));
    }
    timekeep_start(&s_set);                     /* before the SD mount: FatFs stamps files with the guarded clock */
    sdlog_init();
    fs_esp_init();                               /* before the mount: refresh_prepare_sd clears upload leftovers */

    /* No catalog and no VBUS swap: /sd is the machine's own FAT32 partition, so bringing the disk up is one mount
     * and then reading the partition window out of it. The host gets the very same sectors, nothing computed. */
    bool sd_ok = sdcard_mount() == ESP_OK;
    if (sd_ok) {
        sdcard_lock(); refresh_prepare_sd(); sdcard_unlock();
        usb_msc_set_volume(sdcard_volume());
        setup_apply_boot(&s_set);                /* before net_start (it carries the Wi-Fi credentials) */
    }
    if (usb_msc_start() != ESP_OK) {
        ESP_LOGE(TAG, "USB start failed");
        led_set(255, 0, 0);
        return;
    }
    if (sd_ok) {
        usb_msc_set_state(MSC_READY);            /* already mounted: no BUSY window at boot */
        ESP_LOGI(TAG, "ready at %lld ms", esp_timer_get_time() / 1000);
    } else {
        usb_msc_set_state(MSC_NO_MEDIUM);        /* USB stays up reporting NO MEDIUM; refresh retries */
        led_set(255, 0, 0);
        ESP_LOGW(TAG, "no SD card");
    }

    net_start(&s_set, false);                    /* after USB: Wi-Fi never delays or blocks the disk */
    web_start(&s_set);                           /* a web failure is logged there; the disk keeps working */
    refresh_start(&s_set);                       /* watches the SD card from here on */
    button_start(&s_set);                        /* only after boot: GPIO0 low at power-on means ROM download mode */
    lcd_ui_start();                              /* after refresh_start() and net_start(): it samples both */
}
