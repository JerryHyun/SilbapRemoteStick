/* OTA firmware update: /api/ota/arm, /api/ota and the post-boot self-check. See main/ota.h.
 *
 * The image is streamed from the socket straight into esp_ota_write() on the inactive OTA slot in 4 KiB pieces: it
 * is never buffered whole and never touches the SD card (no sdcard_lock is taken, so a USB READ10 is never made to
 * wait for the upload; the two do share the flash bus, which only slows the disk down while it runs).
 *
 * Gates, in the order the brief asks for:
 *   1. password (the middleware's WEB_AUTH check) or, on a device that BOOTED with no password, a confirmed BOOT
 *      press in the arm window opened by POST /api/ota/arm (the boot-time password state is latched, see s_pw_at_boot);
 *   2. Content-Length present, not chunked, and not larger than the slot being written;
 *   3. no machine READ10 in the last 3 s (409 busy);
 *   4. the first 4 KiB must carry our esp_app_desc_t project name (main/ota_check.c) or the slot is abandoned.
 *
 * The single-upload guard (web_upload_claim) is deliberately not taken: esp_http_server runs every handler on its
 * one task, so no SD upload can be in flight at the same time anyway, and the guard exists to protect fsops' one
 * upload handle, which OTA never uses. */
#include "ota.h"
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "ota_check.h"
#include "refresh.h"
#include "usb_msc.h"
#include "web.h"

static const char *TAG = "ota";

#define SELFCHECK_MAX_MS  60000u   /* the refresh task must leave BUSY within this after a fresh boot */
#define SELFCHECK_MIN_MS   1000u   /* let the refresh task publish its first status before judging it */
#define SELFCHECK_POLL_MS   250u
#define RECV_RETRIES          5    /* consecutive socket timeouts tolerated inside one chunk */

static settings_t *s_set;
/* s_set->pw_set is LIVE and can flip to true mid-boot through an unauthenticated POST /api/password (the middleware
 * passes WEB_AUTH while no password is set). The BOOT gate therefore uses the value captured here at boot: a device
 * that booted open always needs the arm + press for OTA, even after a password is set during that boot. */
static bool s_pw_at_boot;

/* One-shot (RAM only): a rollback could not run, so this boot keeps running an image that failed its self-check.
 * Read by /api/status as `selfcheck_failed`. */
static bool s_selfcheck_failed;

/* Arm window. Written by the httpd task (arm) and the button task (confirm), read by both; the critical sections
 * are a few instructions and never call into flash or FatFs. 0 = no window. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_arm_until;       /* /api/ota/arm: a BOOT short press may still confirm until this */
static uint32_t s_confirm_until;   /* BOOT pressed: an upload may start until this */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* True when deadline is set and still in the future. Caller holds the lock. */
static bool alive_locked(uint32_t deadline, uint32_t now) {
    return deadline != 0 && (int32_t)(deadline - now) > 0;
}

bool ota_armed(void) {
    uint32_t now = now_ms();
    bool armed;
    portENTER_CRITICAL(&s_mux);
    armed = alive_locked(s_arm_until, now);
    portEXIT_CRITICAL(&s_mux);
    return armed;
}

void ota_confirm(void) {
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    if (alive_locked(s_arm_until, now)) {
        s_arm_until = 0;                              /* one press, one update */
        s_confirm_until = now + OTA_CONFIRM_WINDOW_MS;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* The confirmed window, without consuming it: an upload that fails a gate before writing may be retried. */
static bool confirm_open(void) {
    uint32_t now = now_ms();
    bool ok;
    portENTER_CRITICAL(&s_mux);
    ok = alive_locked(s_confirm_until, now);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

/* Closes the confirmed window: the next upload needs a fresh BOOT press. Called once every gate has passed, just
 * before the first bytes are written, so a refused upload (busy, wrong size) may still be retried inside the window;
 * an upload that fails later (bad image, flash I/O) does consume it. */
static void confirm_close(void) {
    portENTER_CRITICAL(&s_mux);
    s_confirm_until = 0;
    portEXIT_CRITICAL(&s_mux);
}

/* Drains a body we do not want (the arm call may be sent with an empty JSON object). Best effort: a broken
 * connection just ends the request. */
static void drain_body(httpd_req_t *req) {
    char t[64];
    size_t left = req->content_len;
    while (left > 0) {
        int r = httpd_req_recv(req, t, left < sizeof t ? left : sizeof t);
        if (r <= 0) return;
        left -= (size_t)r;
    }
}

/* ---- POST /api/ota/arm ---- */
static esp_err_t arm_post(httpd_req_t *req) {
    drain_body(req);
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    s_arm_until = now + OTA_ARM_WINDOW_MS;
    s_confirm_until = 0;                              /* a fresh arm supersedes an older confirmation */
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "armed for %u s: press BOOT", (unsigned)(OTA_ARM_WINDOW_MS / 1000));
    return web_json(req, 200, "{\"armed\":true,\"window\":30}");
}

/* ---- POST /api/ota ---- */
/* The body is not read on an early error: answer, then drop the connection so the leftover bytes are not parsed as
 * the next request (same rule as the SD upload path). */
static esp_err_t ota_reject(httpd_req_t *req, int st, const char *key) {
    web_error(req, st, key);
    return ESP_FAIL;
}

/* Receives exactly n bytes into buf; false on a closed socket or too many consecutive timeouts. */
static bool recv_all(httpd_req_t *req, char *buf, size_t n) {
    size_t got = 0;
    int tries = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++tries <= RECV_RETRIES) continue;
        if (r <= 0) {
            ESP_LOGW(TAG, "recv failed (%d) after %u bytes", r, (unsigned)got);
            return false;
        }
        tries = 0;
        got += (size_t)r;
    }
    return true;
}

static esp_err_t ota_post(httpd_req_t *req) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) return web_error(req, 500, "err.internal");
    /* The active slot is never written, but its size is the real ceiling for what this device may boot: use the
     * smaller of the two rather than a constant (both are 4 MiB in partitions.csv). */
    size_t slot = ota_max_image();

    /* 1. Gate. One read of the latched boot value decides both this check and the confirm_close() below: the live
     * s_set->pw_set must never be consulted here. With a password at boot the middleware already checked it;
     * without one the BOOT press must have confirmed. */
    const bool pw_at_boot = s_pw_at_boot;
    if (ota_gate(pw_at_boot, confirm_open()) == OTA_GATE_REFUSE) {
        ESP_LOGW(TAG, "upload refused: no password at boot and no BOOT confirmation");
        return ota_reject(req, 403, "arm");
    }
    /* 2. Size, known before anything is erased. */
    if (httpd_req_get_hdr_value_len(req, "Content-Length") == 0 ||
        httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0 || req->content_len == 0)
        return ota_reject(req, 411, "length");
    if ((size_t)req->content_len > slot) {
        ESP_LOGW(TAG, "upload of %u bytes is larger than the %u-byte slot", (unsigned)req->content_len, (unsigned)slot);
        return ota_reject(req, 413, "too_large");
    }

    /* 3. The machine reading the disk while flash is written makes both slow; wait for a quiet moment. */
    uint32_t lr = usb_msc_last_read_ms();
    if (lr && (uint32_t)(now_ms() - lr) < OTA_READ_QUIET_MS) {
        ESP_LOGW(TAG, "upload refused: the machine read the disk %u ms ago", (unsigned)(now_ms() - lr));
        return ota_reject(req, 409, "busy");
    }

    /* All gates passed: one BOOT press authorises exactly one attempt. Refusals above (busy, wrong size) never reach
     * here, so they can still be retried inside the window; from here on a failure (bad image, flash I/O) does not
     * reopen it. A device that booted with a password has no confirmation to consume. */
    if (!pw_at_boot) confirm_close();

    /* 4. First 4 KiB: the project name is checked before the slot is opened, so a wrong file never erases it. The
     * buffer is static because the httpd task's 8 KiB stack must not carry it. */
    static uint8_t s_buf[OTA_CHUNK];
    size_t left = req->content_len;
    led_set(0, 200, 255);                             /* cyan: firmware upload, distinct from purple SD uploads */
    size_t first = left < OTA_CHUNK ? left : OTA_CHUNK;
    if (!recv_all(req, (char *)s_buf, first)) {
        refresh_led_reset();
        return ESP_FAIL;                              /* the client is gone: no response */
    }
    char seen[OTA_PROJECT_NAME_CAP + 1];
    int chk = ota_check_header(s_buf, first, seen, sizeof seen);
    if (chk != OTA_CHECK_OK) {
        ESP_LOGW(TAG, "wrong image (%d), project name \"%s\"", chk, seen);
        refresh_led_reset();
        return ota_reject(req, 400, "wrong_image");
    }

    /* 5. Stream into the inactive slot. */
    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(target, left, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
        refresh_led_reset();
        return ota_reject(req, 500, "err.io");
    }
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)left, target->label);
    e = esp_ota_write(h, s_buf, first);
    left -= first;
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {           /* esp_ota_write re-checks the image magic byte */
        ESP_LOGW(TAG, "esp_ota_write rejected the image header");
        esp_ota_abort(h);
        refresh_led_reset();
        return ota_reject(req, 400, "wrong_image");
    }
    while (e == ESP_OK && left) {
        size_t want = left < OTA_CHUNK ? left : OTA_CHUNK;
        if (!recv_all(req, (char *)s_buf, want)) {
            esp_ota_abort(h);
            refresh_led_reset();
            return ESP_FAIL;
        }
        e = esp_ota_write(h, s_buf, want);
        left -= want;
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(e));
        esp_ota_abort(h);
        refresh_led_reset();
        return ota_reject(req, 500, "err.io");
    }
    e = esp_ota_end(h);
    if (e != ESP_OK) {                                /* VALIDATE_FAILED = unbootable image; anything else is I/O */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));
        refresh_led_reset();
        if (ota_end_error(e) == OTA_END_WRONG_IMAGE) return ota_reject(req, 400, "wrong_image");
        return ota_reject(req, 500, "err.io");
    }
    e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(e));
        refresh_led_reset();
        return ota_reject(req, 500, "err.io");
    }

    ESP_LOGI(TAG, "%u bytes written to %s; restarting", (unsigned)req->content_len, target->label);
    led_set(0, 255, 0);                               /* green: written; handed back so a failure to reboot still repaints */
    refresh_led_reset();
    esp_err_t r = web_json(req, 200, "{\"ok\":true,\"restart\":true}");
    vTaskDelay(pdMS_TO_TICKS(1000));                  /* let the browser read the answer */
    esp_restart();
    return r;
}

esp_err_t ota_register(settings_t *s) {
    s_set = s;
    /* Latch here, once: see s_pw_at_boot's comment. This is the value every later gate decision reads. */
    s_pw_at_boot = s->pw_set;
    web_route("/api/ota/arm", HTTP_POST, WEB_AUTH, arm_post);
    web_route("/api/ota", HTTP_POST, WEB_AUTH, ota_post);
    /* web_route() only logs; the self-check must see a missing route so an image whose OTA endpoints did not come up
     * cannot mark itself valid. */
    if (!web_route_registered("/api/ota/arm", HTTP_POST) || !web_route_registered("/api/ota", HTTP_POST)) {
        ESP_LOGE(TAG, "OTA routes are not registered");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---- self-check / rollback ---- */
bool ota_pending_verify(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return false;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return false;
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

/* A rollback that cannot run (the ordinary "first OTA on a USB-flashed device" case: otadata is all-0xFF, so there
 * is no second slot to fall back to) must not be silent. The flag is RAM-only and one-shot; /api/status exposes it
 * as `selfcheck_failed` for the banner. The LED is reserved white, a colour used by nothing else, so the device
 * itself says "update not verified" without the web UI (refresh keeps it white; see refresh_led_fault). */
static void selfcheck_mark_failed(void) {
    s_selfcheck_failed = true;
    refresh_led_fault();
}

bool ota_selfcheck_failed(void) { return s_selfcheck_failed; }

bool ota_pw_at_boot(void) { return s_pw_at_boot; }

size_t ota_max_image(void) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!target) return 0;
    size_t slot = target->size;
    if (running && running->size < slot) slot = running->size;
    return slot;
}

/* Returns only when the device kept running: a successful rollback reboots inside the IDF call. main.c's USB
 * failure path and the self-check's own failures share it, so both leave the same unmistakable state behind. */
void ota_rollback_now(const char *why) {
    ESP_LOGE(TAG, "self-check failed (%s): rolling back", why);
    esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only reached when the rollback itself failed (no other bootable slot, e.g. the first OTA of a USB-flashed
     * device). Do NOT reboot unconditionally: with try-all-partitions the bootloader could pick this same slot again
     * and loop. Keep running, say so loudly, and let the user power-cycle. */
    ESP_LOGE(TAG, "self-check failed (%s) and rollback did not reboot: %s. "
                  "The update is NOT verified; power-cycle the device.", why, esp_err_to_name(e));
    selfcheck_mark_failed();
}

static void selfcheck_task(void *arg) {
    bool web_ok = (bool)(intptr_t)arg;
    uint32_t boot = now_ms();
    ESP_LOGI(TAG, "pending verify: self-check running");

    if (!web_ok) {
        ota_rollback_now("HTTP server did not start");
        vTaskDelete(NULL);
        return;
    }

    /* The refresh task publishes its first status within milliseconds of refresh_start(); requiring `started`
     * keeps this from reading the zero-initialised struct before that and declaring a disk that was never brought
     * up healthy. The short floor keeps a still-rebuilding disk from passing as well. */
    for (;;) {
        refresh_status_t rs;
        refresh_status(&rs);
        if (rs.started && !rs.busy && (uint32_t)(now_ms() - boot) >= SELFCHECK_MIN_MS) break;
        if ((uint32_t)(now_ms() - boot) >= SELFCHECK_MAX_MS) {
            ota_rollback_now(rs.busy ? "refresh task is still busy after 60 s" : "refresh task never started");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(SELFCHECK_POLL_MS));
    }

    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) ESP_LOGE(TAG, "mark_app_valid: %s", esp_err_to_name(e));
    else ESP_LOGI(TAG, "self-check passed: image marked valid");
    vTaskDelete(NULL);
}

void ota_selfcheck_start(bool web_ok) {
    if (xTaskCreate(selfcheck_task, "ota-check", 4096, (void *)(intptr_t)web_ok, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task for the self-check");
        ota_rollback_now("no task");
    }
}
