/* Boot-time /sd/silbap-setup.txt and /sd/silbap-help.txt handling. See setup_apply.h for the contract.
 *
 * Runs once from app_main, after the card is mounted and refresh_prepare_sd() has prepared the folders, but before
 * net_start()/refresh_start(). That timing is why the Wi-Fi credentials are copied straight into settings_t: the net
 * layer does not exist yet (net_apply_credentials() would answer ESP_ERR_INVALID_STATE) and net_start(&s_set, false)
 * reads the struct right after this returns. Plain POSIX file calls under sdcard_lock, never fsops: fsops is for web
 * mutations and asks for a refresh, which has no task to serve it yet. */
#include "setup_apply.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "board.h"
#include "sdcard.h"
#include "sdlog.h"
#include "setupfile.h"
#include "timekeep.h"
#include "webdav_xml.h"

static const char *TAG = "setup_apply";

#define SETUP_FILE  SD_MOUNT "/silbap-setup.txt"
#define DONE_FILE   SD_MOUNT "/silbap-setup.done.txt"
#define HELP_FILE   SD_MOUNT "/silbap-help.txt"
#define SETUP_MAX   4096                          /* a bigger setup file is refused and left untouched */
#define MASKED_MAX  (3 * SETUP_MAX)               /* body + "# applied" + the warning block (itself capped at 4 KiB) */
#define HELP_MAX    8192                          /* sf_help_text() is ~6 KiB */
#define FIRST_LINE  96

static char s_setup[SETUP_MAX + 1];               /* static: app_main's stack must not hold these */
static char s_masked[MASKED_MAX];
static char s_help[HELP_MAX];
static settings_t s_next;                         /* the settings copy applied before NVS: also off the stack */

static volatile bool s_applied;                    /* one-shot banner flag */

bool setup_apply_took_effect(void) {
    bool v = s_applied;
    s_applied = false;
    return v;
}

/* Whole file into buf when it fits; returns the byte count, -1 when it is larger than SETUP_MAX or unreadable. */
static int read_capped(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    bool more = fgetc(f) != EOF;
    fclose(f);
    if (more || n == cap) return -1;               /* cap is SETUP_MAX + 1: n == cap means "over the limit" */
    buf[n] = 0;
    return (int)n;
}

/* sf_parse yields a two-letter lowercase code; index into nm_lang_t order. */
static nm_lang_t lang_enum(const char *code) {
    if (!strcmp(code, "ko")) return NM_LANG_KO;
    if (!strcmp(code, "zh")) return NM_LANG_ZH;
    if (!strcmp(code, "ja")) return NM_LANG_JA;
    return NM_LANG_EN;
}

/* Applies the parsed keys to s. Returns how many fields changed. Never logs a value. */
static int apply(const sf_config_t *c, settings_t *s) {
    int n = 0;
    if (c->has_ssid) {
        strlcpy(s->wifi_ssid, c->ssid, sizeof s->wifi_ssid);
        n++;
    }
    if (c->has_wifi_pw) {
        strlcpy(s->wifi_pw, c->wifi_pw, sizeof s->wifi_pw);
        n++;
    }
    if (c->has_web_pw && settings_set_password(s, c->web_pw)) n++;   /* "" clears it */
    if (c->has_rename) {
        s->rename = c->rename;                    /* no refresh here: refresh_start() has not built the catalog yet */
        n++;
    }
    if (c->has_lang) {
        s->lang = lang_enum(c->lang);             /* sf_parse only accepts ko/en/zh/ja */
        memcpy(s->ui_lang, c->lang, 3);           /* two letters plus NUL */
        n++;
    }
    return n;
}

/* Replaces the setup file with its masked copy. The original (which may hold passwords) is removed only after the
 * masked file is fully on the card: a full card can make fopen("wb") succeed (it truncates any older copy) while the
 * write or the flush fails, and FatFs often reports that only at fclose. */
static bool write_done_file(int n, const char *iso) {
    char line[80];
    bool ok = false;
    FILE *f = fopen(DONE_FILE, "wb");
    if (!f) return false;
    if (n > 0) {
        ok = fwrite(s_masked, 1, (size_t)n, f) == (size_t)n;
    } else {
        int m = snprintf(line, sizeof line, "# applied %s (masked copy too large)\n", iso);
        if (m > 0) ok = fwrite(line, 1, (size_t)m, f) == (size_t)m;
    }
    if (ferror(f)) ok = false;            /* must be read before fclose() */
    if (fclose(f) != 0) ok = false;       /* where a full card usually surfaces */
    return ok;
}

static void finish_setup_file(const uint8_t *data, size_t len, const sf_config_t *c) {
    char iso[24];
    dav_iso_date(timekeep_now(), iso, sizeof iso);
    int n = sf_masked(data, len, iso, s_masked, sizeof s_masked);
    sdcard_lock();
    bool done = write_done_file(n, iso);
    if (!done) {
        /* Keep the original so the next boot retries; a partial .done is simply overwritten next time. */
        ESP_LOGW(TAG, "writing " DONE_FILE " failed; keeping " SETUP_FILE);
    } else if (remove(SETUP_FILE) == 0) {
        sdlog_event("setup.log", 0x51, "setup file applied: %d keys, %u unknown, %u invalid",
                    (int)(!!c->has_ssid + !!c->has_wifi_pw + !!c->has_web_pw + !!c->has_rename + !!c->has_lang),
                    c->unknown_count, c->bad_count);
    } else {
        ESP_LOGW(TAG, "removing " SETUP_FILE " failed");
    }
    sdcard_unlock();
}

static void apply_setup_file(settings_t *s) {
    sdcard_lock();
    int n = read_capped(SETUP_FILE, s_setup, sizeof s_setup);
    sdcard_unlock();
    if (n < 0) {
        struct stat st;
        sdcard_lock();
        bool there = stat(SETUP_FILE, &st) == 0;
        sdcard_unlock();
        if (there) {
            ESP_LOGW(TAG, "ignoring " SETUP_FILE ": unreadable or over %d bytes", SETUP_MAX);
            sdlog_event("setup.log", 0x50, "setup file ignored: unreadable or over %d bytes", SETUP_MAX);
        }
        memset(s_setup, 0, sizeof s_setup);        /* drop whatever a short read left behind */
        return;
    }

    sf_config_t c;
    if (sf_parse((const uint8_t *)s_setup, (size_t)n, &c) != 0) {
        ESP_LOGW(TAG, "ignoring " SETUP_FILE ": undecodable");
        sdlog_event("setup.log", 0x52, "setup file ignored: undecodable");
        memset(s_setup, 0, (size_t)n);
        return;
    }

    /* Apply to a copy, save, and only then copy back: a failed save must not leave the live struct ahead of NVS. */
    s_next = *s;
    int applied = apply(&c, &s_next);
    esp_err_t e = applied ? settings_save(&s_next) : ESP_OK;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "saving setup settings failed: %s", esp_err_to_name(e));
        /* Keep the setup file: the next boot retries instead of marking it done with nothing persisted. */
        memset(&s_next, 0, sizeof s_next);
        memset(&c.web_pw, 0, sizeof c.web_pw);
        memset(s_setup, 0, (size_t)n);
        return;
    }
    *s = s_next;
    memset(&s_next, 0, sizeof s_next);
    memset(&c.web_pw, 0, sizeof c.web_pw);        /* the clear-text password has done its job */
    if (applied) s_applied = true;
    ESP_LOGI(TAG, "setup file: %d fields applied, %u unknown, %u invalid", applied, c.unknown_count, c.bad_count);
    finish_setup_file((const uint8_t *)s_setup, (size_t)n, &c);
    memset(s_setup, 0, (size_t)n);                /* do not leave the clear text (passwords) in RAM */
}

static void ensure_help_file(void) {
    const char *version = esp_app_get_description()->version;
    char first[FIRST_LINE] = "";
    struct stat st;
    sdcard_lock();
    bool there = stat(HELP_FILE, &st) == 0 && S_ISREG(st.st_mode);
    if (there) {
        FILE *f = fopen(HELP_FILE, "rb");
        if (f) {
            size_t n = fread(first, 1, sizeof first - 1, f);
            first[n] = 0;
            fclose(f);
        } else there = false;
    }
    sdcard_unlock();
    if (there && sf_help_is_current(first, version)) return;

    int n = sf_help_text(version, s_help, sizeof s_help);
    if (n < 0) {
        ESP_LOGW(TAG, "building the help text failed");
        return;
    }
    sdcard_lock();
    FILE *f = fopen(HELP_FILE, "wb");
    if (f) {
        bool ok = fwrite(s_help, 1, (size_t)n, f) == (size_t)n;
        if (ferror(f)) ok = false;                 /* read before fclose: FatFs may report only here */
        if (fclose(f) != 0) ok = false;
        if (!ok) ESP_LOGW(TAG, "writing " HELP_FILE " failed; the next boot rewrites it");
    } else {
        ESP_LOGW(TAG, "writing " HELP_FILE " failed");
    }
    sdcard_unlock();
}

void setup_apply_boot(settings_t *s) {
    if (!s) return;
    apply_setup_file(s);
    ensure_help_file();
}
