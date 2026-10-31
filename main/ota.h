#pragma once
/* OTA firmware update: POST /api/ota/arm, POST /api/ota, and the boot self-check that confirms (or rolls back) a
 * freshly started image. main/ota.c; the image header inspection and the two pure gate/error decisions live in
 * main/ota_check.c.
 *
 * Gate: with a web password the upload needs Basic auth (the route is WEB_AUTH and the middleware checks it). On a
 * device that BOOTED without a password, physical presence is required instead: /api/ota/arm opens a 30 s window and
 * a short BOOT press during it confirms the update (button_policy's BP_CONFIRM_OTA -> ota_confirm()), which opens a
 * further 30 s upload window. The boot-time password state is latched in ota_register(), never read live, because
 * POST /api/password is reachable unauthenticated while no password is set. The image itself is streamed straight
 * into the inactive OTA slot; it never touches the SD card. */
#include <stdbool.h>
#include "esp_err.h"
#include "settings.h"

#define OTA_ARM_WINDOW_MS      30000u   /* /api/ota/arm opens this long a window for the BOOT press */
#define OTA_CONFIRM_WINDOW_MS  30000u   /* BOOT press confirmed: the upload may start within this */
#define OTA_READ_QUIET_MS       3000u   /* refuse while the machine read this recently (409 busy) */
#define OTA_CHUNK               4096u   /* flash write / socket receive unit */

/* Registers both routes (WEB_AUTH; the middleware lets them through when no password is set). Call after
 * web_start() returned ESP_OK. s must outlive the server. ESP_OK only when both routes are really registered;
 * ESP_FAIL otherwise (the caller must treat that as a failed boot self-check). Also latches the boot-time password
 * state that decides the BOOT gate (see ota_gate in ota_check.h). */
esp_err_t ota_register(settings_t *s);

/* True while /api/ota/arm's window is open and no BOOT press has consumed it yet: main/button.c passes this as
 * the armed input of bp_on_release_ex(), so a short press means BP_CONFIRM_OTA instead of BP_REFRESH. */
bool ota_armed(void);

/* The BOOT short press that confirms the pending arm window (BP_CONFIRM_OTA). Ignored when not armed. */
void ota_confirm(void);

/* The password state latched in ota_register(): true when this boot started with a web password. /api/status exposes
 * it as `pw_at_boot` so the SPA shows the arm button exactly when the gate will require a BOOT press, even after a
 * password was set during this boot. */
bool ota_pw_at_boot(void);

/* True when the running partition is ESP_OTA_IMG_PENDING_VERIFY, i.e. this boot came from a freshly written slot
 * and has to prove itself. A normal boot returns false without side effects or log output. */
bool ota_pending_verify(void);

/* Largest image this device may accept: min(next-update slot, running slot). 0 when no target partition exists.
 * /api/status exposes it as `ota_max` so web/app.js does not hardcode a size. */
size_t ota_max_image(void);

/* Starts the small self-check task: within 60 s the refresh task must leave BUSY (SD ready or NO_MEDIUM reached)
 * and web_ok says web_start() returned ESP_OK AND ota_register() returned ESP_OK (so the OTA endpoints really exist).
 * Success marks the image valid; failure rolls back and reboots. Never call it when ota_pending_verify() is false. */
void ota_selfcheck_start(bool web_ok);

/* True (RAM only, latched for this boot) when the self-check failed and the rollback could not run, so the device is
 * still running an unverified image. Feeds /api/status `selfcheck_failed`. */
bool ota_selfcheck_failed(void);

/* Rolls back and reboots; returns only when the rollback failed, in which case it sets the flag above, paints the
 * LED white and logs the "power-cycle the device" remedy. For main.c's USB failure path in a pending-verify boot. */
void ota_rollback_now(const char *why);
