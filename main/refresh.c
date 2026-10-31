#include "refresh.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "board.h"
#include "fsops.h"
#include "fs_names.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "refresh_policy.h"
#include "sdcard.h"
#include "sdlog.h"
#include "usb_msc.h"
#include "webcore.h"

static const char *TAG = "refresh";

/* The names the disk does not show as "content" (dot names, Windows/macOS clutter, our own help and setup files).
 * One rule for the web and the empty-card check; see fs_names.h. */

/* Removes everything below path (depth-first) except, directly in path, the names in keep (NULL-terminated);
 * names are collected first so readdir is not disturbed. */
static void rm_children(const char *path, int depth, const char *const *keep) {
    DIR *d = opendir(path);
    if (!d) return;
    char (*names)[64] = NULL; int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strlen(e->d_name) >= 64) continue;
        bool kept = false;
        for (const char *const *k = keep; k && *k && !kept; k++) kept = !strcmp(e->d_name, *k);
        if (kept) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char (*nn)[64] = realloc(names, (size_t)cap * 64);
            if (!nn) break;
            names = nn;
        }
        strcpy(names[n++], e->d_name);
    }
    closedir(d);
    char p[300];
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "%s/%s", path, names[i]);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (depth < 8) rm_children(p, depth + 1, NULL);
            rmdir(p);
        } else unlink(p);
    }
    free(names);
}

void refresh_prepare_sd(void) {
    mkdir(SILBAP_DIR, 0775);
    mkdir(SILBAP_DIR "/tmp", 0775);
    /* catalog.new may be the only complete catalog after a crash between dropping catalog.bin and the rename:
     * catalog_load recovers it, so the wipe keeps it (and names.new, written in the same step) */
    static const char *const keep[] = { "catalog.new", "names.new", NULL };
    fs_cleanup_tmp();                            /* upload leftovers (up-*); the wipe below would also take them */
    rm_children(SILBAP_DIR "/tmp", 0, keep);
    /* f_getfree counts the whole FAT when FSINFO has no valid free count (seconds on a big card): do it now, while
     * USB is not READY yet, so fsops' upload space check never stalls READ10s with that scan (FatFs keeps the count) */
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info(SD_MOUNT, &total, &free_b);
    int visible = 0;
    DIR *d = opendir(SD_MOUNT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !fs_hidden_name(e->d_name)) visible++;
        closedir(d);
    }
    if (!visible) {   /* empty card: give the machine its usual folder */
        mkdir(SD_MOUNT "/EMB", 0775);
        mkdir(SD_MOUNT "/EMB/Embf", 0775);
    }
}


#define TICK_MS          100
#define SD_POLL_MS       2000
#define BLINK_MS         300
/* How often the volume may be counted, at most. Every web mutation (fsops' changed()) asks for a count; the flag
 * coalesces a burst (an upload fires one per chunk, a folder delete runs for seconds) into one walk. 5 s is wider
 * than the 3 s quiet window of the coherency re-mount, so the count normally happens just after a re-mount has
 * re-read the FAT: it then includes what the machine saved over USB, not what FatFs had cached before. */
#define COUNT_MIN_MS     5000u
/* Bound on one walk. A pathological card (a FAT32 directory chain many thousands of entries long) must not own the
 * watcher: at ~0.2-0.5 ms per entry (readdir plus stat plus two lock hand-overs) 5000 entries is ~1-2.5 s of SD
 * work spread over 5000 short lock holds, and the presence poll and the LED still run on the ticks in between. A
 * real embroidery card holds hundreds of entries. Hitting the cap is logged at ESP_LOGW and the counts are
 * published as the lower bound they are, rather than left at the previous value. */
#define CNT_MAX_ENTRIES  5000u
/* Same depth bound api_files' /api/count uses (COUNT_MAX_DEPTH): a folder deeper than this is counted, not
 * descended into. Nothing the web can address lives below it. */
#define CNT_MAX_DEPTH    24
/* Coherency: the machine writes the volume as raw sectors over USB MSC, behind FatFs's back, so the FAT and
 * directory window FatFs caches in the FATFS object go stale in both directions. Drop and re-mount it, but
 * only when the bus has been quiet this long (the machine's last READ10/WRITE10, the web's last mutation, and
 * the last re-mount all count). REMOUNT_MIN_MS is the same window used as a floor between re-mounts, so a
 * volume nobody touches is still refreshed about every 3 s but never twice inside one window. */
#define QUIET_MS         3000
#define REMOUNT_MIN_MS   3000

static TaskHandle_t s_task;
static bool s_sd_ok;
static volatile uint32_t s_web_activity_ms;   /* last fsops mutation, stamped by refresh_activity() */
static uint32_t s_last_remount_ms;            /* refresh task only */
static volatile bool s_count_due;             /* a change (or a fresh mount) asked for a re-count */
static uint32_t s_last_count_ms;              /* refresh task only; 0 = never counted */
static uint32_t s_count_seen_write;           /* usb_msc_last_write_ms() taken when the last count started */
static const settings_t *s_set;                /* auto_refresh decides whether changes re-enumerate the host */
/* Re-enumeration (VBUS off/on): the host caches the directory, so it only re-reads it after a genuine unplug.
 * s_forced_req_ms: the last REFRESH_FORCED request (button, POST /api/refresh), 0 = none pending.
 * s_replug_seen_*: the change stamps already covered by a cycle, so one upload or a burst of them coalesces into
 * one cycle instead of one per chunk. s_last_replug_ms: the floor between automatic cycles, like s_last_remount_ms. */
static volatile uint32_t s_forced_req_ms;
static uint32_t s_last_replug_ms;             /* refresh task only */
static uint32_t s_replug_seen_web_ms;
static uint32_t s_replug_seen_write_ms;
static uint32_t s_wait_log_ms;                /* refresh task only: rate-limits the "still waiting" debug line */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static refresh_status_t s_status;  /* guarded by s_mux */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void refresh_activity(void) { s_web_activity_ms = now_ms(); }

/* Milliseconds since the last of the given stamps. A zero stamp is "never", so it is ignored; with no stamp at
 * all the volume has been quiet since boot (idle = now), which re-mounts once at 3 s: harmless and desirable.
 * Ages, not raw stamps: a 32-bit ms counter wraps. */
static uint32_t since_last(uint32_t now, const uint32_t *stamps, size_t n) {
    uint32_t idle = now;
    for (size_t i = 0; i < n; i++)
        if (stamps[i]) {
            uint32_t age = now - stamps[i];
            if (age < idle) idle = age;
        }
    return idle;
}

/* Kept for the API surface: with no catalog to rebuild there is nothing to debounce. It wakes the watcher so a
 * caller that just changed the card (button, settings) gets the coherency step without waiting a tick, and it asks
 * for a re-count of the volume (coalesced by COUNT_MIN_MS in refresh_task). REFRESH_FORCED additionally asks for a
 * replug cycle that runs even when nothing changed: the web's "미싱에 반영" button. The stamp is the debounce. */
void refresh_request(refresh_kind_t kind) {
    if (kind == REFRESH_FORCED) __atomic_store_n(&s_forced_req_ms, now_ms(), __ATOMIC_RELEASE);
    s_count_due = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void refresh_status(refresh_status_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

/* The counts are published separately: the walk runs on the refresh task and must not touch busy, which stays the
 * user-visible "an operation is in progress" flag (there is none during a background count). */
static void status_counts_set(uint32_t files, uint32_t dirs) {
    portENTER_CRITICAL(&s_mux);
    s_status.files = files; s_status.dirs = dirs;
    portEXIT_CRITICAL(&s_mux);
}

static void status_set(bool busy, bool sd_ok) {
    portENTER_CRITICAL(&s_mux);
    s_status.busy = busy; s_status.sd_ok = sd_ok; s_status.started = true;
    if (!sd_ok) s_status.files = s_status.dirs = 0;   /* no card, no contents: never report the last card's */
    portEXIT_CRITICAL(&s_mux);
}

/* ---- counting the volume ----
 * One depth-first walk from SD_MOUNT, with exactly the rules /api/list applies, so the header numbers and the
 * listing cannot disagree: "." and ".." are skipped, fs_hidden_name() names are skipped (the listing hides them
 * unless ?hidden=1, and the header always counts the visible ones), and anything starting with ".silbap" is
 * skipped (never exposed). That last rule is also what keeps the upload temp folder out of the count:
 * SILBAP_DIR is SD_MOUNT "/.silbap", so SILBAP_DIR "/tmp" is /sd/.silbap/tmp, and the prefix test runs at the root
 * before that directory is ever opened. Verified, not assumed: the temp folder is a dot name under the root.
 *
 * The lock is taken once per entry (the readdir that finds it plus the stat that classifies it), per opendir and per
 * closedir, never across the walk: the same discipline as api_files' dir_next. A directory listing is a handful of
 * sectors from FatFs's window, so each hold is short (one sector over 4-bit SD at 20 MHz is ~130 us, and FatFs
 * usually has the sector cached); a USB READ10 waits for at most one of those, never for the walk. The
 * price is that the answer is a snapshot stitched from many instants: a file deleted while the walk is in another
 * folder is counted, one created behind the cursor is not. That is fine for a header figure refreshed on the next
 * change anyway; it is not, and must not be used as, an atomic count. */
typedef struct { uint32_t files, dirs, entries; bool capped; } count_t;

/* One path, one name and one stat buffer for the whole walk, as api_files' count_dir does with s_cpath: only the
 * refresh task walks, and the FAT allows 24 levels of recursion. */
static char s_cpath[sizeof SD_MOUNT + WC_PATH_MAX + 2];
static char s_cname[256];
static struct stat s_cstat;

enum { CNT_EOD = 0, CNT_ENT = 1 };

/* The next entry the listing would show, with the same rules and the same lock bracketing as api_files' dir_next:
 * one sdcard_lock around the readdir that finds it and the stat that classifies it, so the directory handle is
 * never used outside the lock and no USB read waits on the whole walk. The path is left in s_cpath for the caller's
 * recursion; the caller clears it back to len. */
static int cnt_next(DIR *d, size_t len, char *name, size_t nsz, bool *isdir) {
    int rc = CNT_EOD;
    sdcard_lock();
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strncmp(e->d_name, ".silbap", 7)) continue;   /* never exposed; contains the upload tmp folder */
        if (fs_hidden_name(e->d_name)) continue;
        size_t nl = strlen(e->d_name);
        if (nl >= nsz) continue;                           /* longer than any name the web accepts */
        if (len + 1 + nl >= sizeof s_cpath) continue;      /* deeper than the web can address */
        strcpy(name, e->d_name);
        s_cpath[len] = '/';
        memcpy(s_cpath + len + 1, e->d_name, nl + 1);
        if (stat(s_cpath, &s_cstat) != 0) { s_cpath[len] = 0; continue; }   /* vanished since the readdir */
        *isdir = S_ISDIR(s_cstat.st_mode);
        rc = CNT_ENT;
        break;
    }
    sdcard_unlock();
    return rc;
}

/* Depth-first, stopping only at the entry cap (c->capped). A folder that cannot be opened, or sits at CNT_MAX_DEPTH,
 * is counted but not descended into: the web cannot address anything below the depth bound (wc_norm_path caps a
 * path at WC_PATH_MAX), so nothing its listing would show is lost. */
static void count_dir(size_t len, int depth, count_t *c) {
    if (depth >= CNT_MAX_DEPTH) return;
    sdcard_lock();
    DIR *d = opendir(s_cpath);
    sdcard_unlock();
    if (!d) return;
    for (;;) {
        bool isdir = false;
        if (cnt_next(d, len, s_cname, sizeof s_cname, &isdir) == CNT_EOD) break;
        if (c->entries >= CNT_MAX_ENTRIES) { c->capped = true; s_cpath[len] = 0; break; }
        c->entries++;
        if (isdir) { c->dirs++; count_dir(len + 1 + strlen(s_cname), depth + 1, c); }
        else c->files++;
        s_cpath[len] = 0;
        if (c->capped) break;
    }
    sdcard_lock();
    closedir(d);
    sdcard_unlock();
}

/* Runs on the refresh task only. Publishes the counts; the caller decides when. */
static void count_volume(void) {
    /* Stamp the write cursor before the walk, not after: a sector the machine writes while the walk is running is
     * then still "unseen", so the re-mount after the quiet window re-arms the count instead of the walk silently
     * publishing a number that missed the new file. */
    uint32_t w0 = usb_msc_last_write_ms();
    count_t c = {0, 0, 0, false};
    strcpy(s_cpath, SD_MOUNT);
    count_dir(strlen(SD_MOUNT), 0, &c);
    if (c.capped) ESP_LOGW(TAG, "count stopped at %u entries: %u files, %u folders so far (volume is larger)",
                           (unsigned)CNT_MAX_ENTRIES, (unsigned)c.files, (unsigned)c.dirs);
    status_counts_set(c.files, c.dirs);
    s_count_seen_write = w0;
}


/* ---- LED: yellow while mounting, green idle, blue while the PC reads, red on errors ---- */
static int s_led = -1;
enum { LED_YELLOW, LED_GREEN, LED_BLUE, LED_RED };
static void led_state(int st) {
    if (st == s_led) return;
    s_led = st;
    switch (st) {
    case LED_YELLOW: led_set(255, 160, 0); break;
    case LED_GREEN:  led_set(0, 255, 0); break;
    case LED_BLUE:   led_set(0, 0, 255); break;
    default:         led_set(255, 0, 0); break;
    }
}

void refresh_led_reset(void) { s_led = -1; }

static void log_diag_once(void) {
    static bool first = true;
    static uint32_t last_dropped;
    if (first) {
        first = false;
        ESP_LOGI(TAG, "sdlog stack high-water: %u bytes unused", (unsigned)sdlog_stack_hwm());
    }
    uint32_t d = sdlog_dropped();
    if (d) { ESP_LOGW(TAG, "sdlog dropped %lu events so far (%lu new)", (unsigned long)d, (unsigned long)(d - last_dropped)); last_dropped = d; }
}

/* Takes the card away from USB, then unmounts. State first (new reads bounce), then the lock (an in-flight
 * READ10 finishes) and the mount that the callbacks use. */
static void sd_gone(void) {
    ESP_LOGW(TAG, "SD card removed");
    usb_msc_set_state(MSC_NO_MEDIUM);
    usb_msc_set_volume(NULL);
    sdcard_unmount();
    s_sd_ok = false;
    status_set(false, false);
    led_state(LED_RED);
}

/* The disk is only served to the host once /sd is mounted: the two views are the same partition. */
static bool sd_inserted(void) {
    led_state(LED_YELLOW);
    if (sdcard_mount() != ESP_OK) return false;
    sdcard_lock(); refresh_prepare_sd(); sdcard_unlock();
    const card_vol_t *v = sdcard_volume();
    if (!v) { sdcard_unmount(); return false; }   /* mounted but no window: cannot happen, but never serve garbage */
    usb_msc_set_volume(v);
    usb_msc_set_state(MSC_READY);
    ESP_LOGI(TAG, "SD card inserted");
    log_diag_once();
    s_sd_ok = true;
    s_last_count_ms = 0;                          /* a new volume: count it on the next tick, not after COUNT_MIN_MS */
    s_count_due = true;
    status_set(false, true);
    led_state(LED_GREEN);
    /* The host has just enumerated this medium: everything on it is current to it, so no replug and no stale
     * request carried over from the card that was here before. */
    s_forced_req_ms = 0;
    s_replug_seen_web_ms = s_web_activity_ms;
    s_replug_seen_write_ms = usb_msc_last_write_ms();
    return true;
}

/* Take the controller off the bus long enough that the host treats it as a real unplug and re-reads the
 * directory, then put it back. Straight-line on purpose: everything between the two calls is a delay that cannot
 * fail, and there is no return, goto or error branch, so usb_msc_vbus(true) is always reached. The one way to lose
 * the disk until reboot would be to skip it. MSC state deliberately untouched: the medium never went away, the host
 * is only being told to look again, so READ10 answers exactly as before once the gap ends.
 * Modelled on robbellie/embroidery-stick's do_usb_refresh(); 50 ms was debounced away by the machine's host stack,
 * RP_REPLUG_HOLD_MS (500 ms) was not. */
static void replug(const char *why) {
    uint32_t hold = rp_replug_hold_ms(RP_REPLUG_HOLD_MS);
    ESP_LOGI(TAG, "replug (%s): VBUS off for %u ms so the host re-reads the directory", why, (unsigned)hold);
    usb_msc_vbus(false);
    vTaskDelay(pdMS_TO_TICKS(hold));
    usb_msc_vbus(true);
    s_last_replug_ms = now_ms();
}

static void refresh_task(void *arg) {
    (void)arg;
    uint32_t next_sd = now_ms();
    int missed = 0;

    s_sd_ok = sdcard_present();
    status_set(false, s_sd_ok);
    if (!s_sd_ok) { led_state(LED_RED); usb_msc_set_state(MSC_NO_MEDIUM); }
    else {
        led_state(LED_GREEN);
        s_count_due = true;                       /* the volume app_main mounted, before this task existed */
        /* Nothing on the card is unseen by the host: it enumerated this medium a moment ago at boot. */
        s_replug_seen_web_ms = s_web_activity_ms;
        s_replug_seen_write_ms = usb_msc_last_write_ms();
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        uint32_t now = now_ms();

        /* Coherency: FatFs caches the FAT and the directory window in the FATFS object, while the machine writes
         * the same volume as raw sectors over USB MSC. Once the bus is quiet, drop those caches by mounting the
         * FATFS object again, so the web lists what the machine saved and the machine sees what the web wrote.
         * Not while a transfer or an upload is in flight: s_web_activity_ms covers every fs_* mutation, the
         * upload chunks included. The MSC state does not change: the host sees the same disk throughout.
         *
         * Re-enumeration rides on the same quiet window, and after the re-mount: FatFs must have re-read the FAT
         * before the host is told to re-read the directory, or the host would cache the old one again. It runs for
         * a request (button / POST /api/refresh: always, even with no change) or for a change the host does not
         * know about (a web mutation, a machine write). One cycle per change: the change stamps are recorded when
         * the cycle runs, so a burst of uploads coalesces into one replug. */
        bool remount_due = false;
        if (s_sd_ok) {
            const uint32_t stamps[] = { usb_msc_last_read_ms(), usb_msc_last_write_ms(), s_web_activity_ms };
            uint32_t idle = since_last(now, stamps, sizeof stamps / sizeof stamps[0]);
            uint32_t since_remount = since_last(now, &s_last_remount_ms, 1);
            remount_due = idle >= QUIET_MS && since_remount >= REMOUNT_MIN_MS;

            /* What asks for a replug, newest first: the forced request, else the newest change not yet shown to
             * the host. A stamp of 0 is "never", so an unchanged disk asks for nothing. */
            uint32_t write_ms = usb_msc_last_write_ms();
            uint32_t web_ms = s_web_activity_ms;
            uint32_t forced_ms = __atomic_load_n(&s_forced_req_ms, __ATOMIC_ACQUIRE);
            bool forced = forced_ms != 0;
            uint32_t req_ms = 0;
            const char *why = "";
            if (forced) { req_ms = forced_ms; why = "forced button"; }
            else if (web_ms && web_ms != s_replug_seen_web_ms) { req_ms = web_ms; why = "web change"; }
            else if (write_ms && write_ms != s_replug_seen_write_ms) { req_ms = write_ms; why = "machine write"; }

            /* rp_replug applies the debounce and the idle rule (automatic policy) and the forced cap, so there is
             * one rule for both paths, and a machine that reads continuously cannot starve the button. */
            bool auto_ok = s_set && s_set->auto_refresh;
            bool replug_due = req_ms &&
                rp_replug(now, req_ms, usb_msc_last_read_ms(), forced, auto_ok, s_last_replug_ms) == RP_RUN;
            /* "still waiting" is only true when the policy would ever let it run: with automatic replug off an
             * unforced change waits forever, and saying so once a second would be noise, not a diagnosis. */
            if (req_ms && !replug_due && (forced || auto_ok) &&
                (!s_wait_log_ms || (uint32_t)(now - s_wait_log_ms) >= 5000)) {
                s_wait_log_ms = now;
                ESP_LOGD(TAG, "replug (%s) waiting for a quiet bus: last READ10 %u ms ago, request %u ms ago",
                         why, (unsigned)since_last(now, &stamps[0], 1), (unsigned)(now - req_ms));
            }

            /* Re-mount first, then the VBUS cycle: the host must re-read a FAT this side has already re-read. A
             * re-mount that fails leaves both pending (the request stays armed) and retries on the next tick,
             * rather than taking the disk away to show the host a disk whose caches were not refreshed. */
            if (remount_due || replug_due) {
                esp_err_t e = sdcard_remount();
                if (e == ESP_OK) {
                    s_last_remount_ms = now;
                    /* The machine saved over USB, behind FatFs's back, so no fs_* mutation told us: the files it
                     * wrote are counted from the volume the re-mount just re-read. Quiet-gated by the 3 s rule, so
                     * a long copy is counted once, when it ends, not every 5 s while it runs. */
                    uint32_t w = usb_msc_last_write_ms();
                    if (w && w != s_count_seen_write) s_count_due = true;
                } else {
                    ESP_LOGD(TAG, "re-mount deferred: %s (retrying next tick)", esp_err_to_name(e));
                    replug_due = false;          /* and with it the cycle: see above */
                }
            }

            if (replug_due) {
                /* Record what this cycle covers *before* it starts: a mutation that lands during the hold is then
                 * still unseen, and the floor keeps it from taking the disk away again at once. */
                s_wait_log_ms = 0;
                if (forced) {
                    /* Clear only the request that ran: the web may have pressed the button during the hold, and
                     * that newer one must not be lost. It runs on the next tick, after the debounce. */
                    uint32_t expect = forced_ms;
                    __atomic_compare_exchange_n(&s_forced_req_ms, &expect, 0, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                }
                s_replug_seen_web_ms = web_ms;
                s_replug_seen_write_ms = write_ms;
                replug(why);
            }
        }

        /* Counting the volume, on this task only (a /api/status poll must never hold the card for a walk). It runs
         * when something changed and at most once per COUNT_MIN_MS, coalescing the burst of requests a folder
         * upload or delete produces into one walk; never on the 2 s presence poll, and never on a tick where the
         * re-mount above is due (still holding FatFs's stale directory window is exactly when a count would be
         * wrong). A request that lands on such a tick keeps its flag and is served ~100 ms later, after the mount. */
        if (s_sd_ok && !remount_due) {
            if (s_count_due && (!s_last_count_ms || (uint32_t)(now - s_last_count_ms) >= COUNT_MIN_MS)) {
                s_count_due = false;
                count_volume();
                s_last_count_ms = now;
            }
        }

        if ((int32_t)(now - next_sd) >= 0) {
            next_sd = now + SD_POLL_MS;
            if (s_sd_ok) {
                if (sdcard_present()) missed = 0;
                else if (++missed >= 2) {            /* one failed status poll is not a removal */
                    missed = 0;
                    sd_gone();
                }
            } else {
                sd_inserted();
            }
        }
        if (s_sd_ok) {
            uint32_t lr = usb_msc_last_read_ms();
            led_state(lr && (uint32_t)(now - lr) < BLINK_MS ? LED_BLUE : LED_GREEN);
        }
    }
}

void refresh_close_data_files(void) {
    /* The catalog held open data files; the real FAT has none to close. Kept so fsops' hook and the web's calls
     * need not know that (Task 7 removes the hook). */
}

void refresh_start(const settings_t *s) {
    s_set = s;                                   /* auto_refresh gates the automatic replug, read-only */
    xTaskCreatePinnedToCore(refresh_task, "refresh", 8192, NULL, 4, &s_task, 0);
}
