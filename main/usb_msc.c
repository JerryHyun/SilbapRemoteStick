#include "tusb.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"
#if CONFIG_SILBAP_DEV_CDC
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#endif
#include "usb_msc.h"
#include "netif.h"
#include "sdcard.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static volatile msc_state_t s_state = MSC_BUSY;   /* never claim NO_MEDIUM before main decides */
static card_vol_t s_vol;                          /* the host-visible window; s_vol_set says it is valid */
static bool s_vol_set;
static volatile uint32_t s_last_read_ms;
static volatile uint32_t s_last_write_ms;
static void vbus_set(bool on);

esp_err_t usb_msc_start(void) {
    /* Same string table as esp_tinyusb's default (usb_descriptors.c), with the serial = base MAC in hex.
     * The pointers are copied at install time but the strings must outlive the call: static. */
    static char serial[13];
    static const char *strs[8];
    static const char langid[2] = { 0x09, 0x04 };
    net_mac_hex(serial);
    int n = 0;
    strs[n++] = langid;                                   /* 0: language English (0x0409) */
    strs[n++] = CONFIG_TINYUSB_DESC_MANUFACTURER_STRING;  /* 1 */
    strs[n++] = CONFIG_TINYUSB_DESC_PRODUCT_STRING;       /* 2 */
    strs[n++] = serial;                                   /* 3 */
#if CONFIG_TINYUSB_CDC_ENABLED
    strs[n++] = CONFIG_TINYUSB_DESC_CDC_STRING;           /* CDC interface */
#endif
    strs[n++] = CONFIG_TINYUSB_DESC_MSC_STRING;           /* MSC interface */
    strs[n] = NULL;
    tinyusb_config_t cfg = { 0 };         /* device/config descriptors come from Kconfig defaults */
    cfg.string_descriptor = strs;
    cfg.string_descriptor_count = n;
    esp_err_t e = tinyusb_driver_install(&cfg);
    if (e != ESP_OK) return e;
    vbus_set(true);
#if CONFIG_SILBAP_DEV_CDC
    const tinyusb_config_cdcacm_t acm = { .usb_dev = TINYUSB_USBDEV_0, .cdc_port = TINYUSB_CDC_ACM_0 };
    e = tusb_cdc_acm_init(&acm);
    if (e != ESP_OK) return e;
    e = esp_tusb_init_console(TINYUSB_CDC_ACM_0);
#endif
    return e;
}

void usb_msc_set_state(msc_state_t s) { s_state = s; }

/* Copy, never keep the pointer: sdcard_unmount() clears the volume behind it. */
void usb_msc_set_volume(const card_vol_t *v) {
    bool ok = v && v->sectors;
    if (ok) s_vol = *v;
    __atomic_store_n(&s_vol_set, ok, __ATOMIC_RELEASE);
}

uint32_t usb_msc_last_read_ms(void) { return s_last_read_ms; }
uint32_t usb_msc_last_write_ms(void) { return s_last_write_ms; }
/* Force the DWC2 controller's view of VBUS, the way robbellie/embroidery-stick does it
 * (main/app_main.c, usbd_vbus_enable): drive the three USB VBUS/session inputs from the GPIO matrix
 * constant sources instead of from a real VBUS pin. With no VBUS the controller drops off the bus
 * (SESSEND) and the host sees a genuine unplug, which is what makes it re-read the directory;
 * tud_disconnect() alone only releases D+ and some hosts debounce that away as a glitch. */
static void vbus_set(bool on) {
    const uint32_t in = on ? GPIO_MATRIX_CONST_ONE_INPUT : GPIO_MATRIX_CONST_ZERO_INPUT;
    esp_rom_gpio_connect_in_signal(in, USB_OTG_VBUSVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(in, USB_SRP_BVALID_IN_IDX, 0);
    /* SESSEND is active-low in meaning: assert it (1) while the bus is gone. */
    esp_rom_gpio_connect_in_signal(on ? GPIO_MATRIX_CONST_ZERO_INPUT : GPIO_MATRIX_CONST_ONE_INPUT,
                                   USB_SRP_SESSEND_IN_IDX, 0);
}
void usb_msc_vbus(bool on) { vbus_set(on); }
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vid[8], uint8_t pid[16], uint8_t rev[4]) {
    (void)lun; memcpy(vid, "Silbap  ", 8); memcpy(pid, "RemoteStick     ", 16); memcpy(rev, "1.0 ", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (s_state == MSC_NO_MEDIUM) { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); return false; }
    if (s_state == MSC_BUSY)      { tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01); return false; }
    return true;
}
void tud_msc_capacity_cb(uint8_t lun, uint32_t *count, uint16_t *size) {
    (void)lun;
    *count = __atomic_load_n(&s_vol_set, __ATOMIC_ACQUIRE) ? s_vol.sectors : 0;
    *size = 512;
}
/* The disk is writable on purpose: it is the machine's own 16 GiB FAT32 volume on the card, and a design saved
 * here has to survive the unplug. */
bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t pc, bool start, bool eject) { (void)lun; (void)pc; (void)start; (void)eject; return true; }
/* READ10/WRITE10 are straight sector moves on the card, shifted by the partition's first LBA: the MBR and the
 * archival partition are never exposed. Both take sdcard_lock so the web side, FatFs and the presence poll never
 * interleave a transfer with them; the callback is not re-entered while it runs. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buf, uint32_t n) {
    s_last_read_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_state != MSC_READY || !__atomic_load_n(&s_vol_set, __ATOMIC_ACQUIRE)) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01); return -1;
    }
    if (offset != 0 || (n % 512) != 0) {          /* 512-byte blocks only: TinyUSB never asks otherwise */
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); return -1;
    }
    const uint32_t sectors = n / 512;
    if ((uint64_t)lba + sectors > s_vol.sectors) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); return -1;
    }
    sdcard_lock();
    sdmmc_card_t *card = sdcard_card();
    esp_err_t e = card ? sdmmc_read_sectors(card, buf, (size_t)lba + s_vol.first_lba, sectors) : ESP_ERR_INVALID_STATE;
    sdcard_unlock();
    if (e != ESP_OK) {
        /* Known limitation: TinyUSB (msc_device.c proc_read_io_data, TUD_MSC_RET_ERROR) overwrites the sense with
         * medium-not-present (2/3A/00) after -1, so the host does not see 3/11/00. There is no hook for it. Hosts
         * gate on TEST UNIT READY anyway, so -1 stays. */
        ESP_LOGW("usb_msc", "read lba %lu+%lu failed: %s", (unsigned long)lba, (unsigned long)sectors, esp_err_to_name(e));
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x11, 0x00);
        return -1;
    }
    return (int32_t)n;
}
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buf, uint32_t n) {
    s_last_write_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_state != MSC_READY || !__atomic_load_n(&s_vol_set, __ATOMIC_ACQUIRE)) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x04, 0x01); return -1;
    }
    if (offset != 0 || (n % 512) != 0) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); return -1;
    }
    const uint32_t sectors = n / 512;
    if ((uint64_t)lba + sectors > s_vol.sectors) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0x00); return -1;
    }
    sdcard_lock();
    sdmmc_card_t *card = sdcard_card();
    esp_err_t e = card ? sdmmc_write_sectors(card, buf, (size_t)lba + s_vol.first_lba, sectors) : ESP_ERR_INVALID_STATE;
    sdcard_unlock();
    if (e != ESP_OK) {
        ESP_LOGW("usb_msc", "write lba %lu+%lu failed: %s", (unsigned long)lba, (unsigned long)sectors, esp_err_to_name(e));
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x03, 0x0C);   /* WRITE ERROR */
        return -1;
    }
    return (int32_t)n;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const cmd[16], void *buf, uint16_t n) {
    if (cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) return 0;
    if (cmd[0] == 0x35 /* SYNCHRONIZE CACHE(10) */) return 0;
    if (cmd[0] == 0x5A /* MODE SENSE(10) */) {
        /* TinyUSB answers MODE SENSE(6) itself; (10) gets the same: an 8-byte header, no pages or block
         * descriptors. WP (0x80 in the device-specific parameter) is CLEAR: the disk is writable. Mode data
         * length counts the 6 bytes after it. Cut to the allocation length and the buffer. */
        static const uint8_t hdr[8] = { 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        uint32_t alloc = (uint32_t)cmd[7] << 8 | cmd[8], len = sizeof hdr;
        if (len > alloc) len = alloc;
        if (len > n) len = n;
        memcpy(buf, hdr, len);
        return (int32_t)len;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
