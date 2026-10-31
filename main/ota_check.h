#pragma once
/* Start-of-image validation for an uploaded firmware image (main/ota_check.c), host-tested by
 * test/host/test_ota_check.c. Pure byte inspection: nothing here touches flash, the card or the OTA API.
 *
 * The caller has already read the first bytes of the .bin (4 KiB is plenty, less is fine: n is authoritative)
 * and asks whether they belong to this project. A random file that would be written to the OTA slot is
 * rejected instead of bricking it.
 *
 * Layout read (ESP-IDF v5.5.2, ESP32-S3, little-endian):
 *   offset  0: esp_image_header_t          (24 bytes)
 *   offset 24: esp_image_segment_header_t  ( 8 bytes, header of the first segment = the app description)
 *   offset 32: esp_app_desc_t             (256 bytes)
 * Inside esp_app_desc_t: magic_word 0, secure_version 4, reserv1[2] 8, version[32] 16, project_name[32] 48.
 * Project name therefore starts at absolute offset 32 + 48 = 80 and is 32 bytes long.
 * The image header itself (0xE9 magic, chip id, ...) is deliberately not judged here: the project name in the
 * app description is what says "this is our firmware", and it survives every header/section tweak. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OTA_IMAGE_HEADER_SIZE   24u   /* sizeof(esp_image_header_t) */
#define OTA_SEGMENT_HEADER_SIZE  8u   /* sizeof(esp_image_segment_header_t) */
#define OTA_APP_DESC_OFFSET     32u   /* 24 + 8: esp_app_desc_t starts here */
#define OTA_APP_DESC_SIZE      256u   /* sizeof(esp_app_desc_t) in IDF v5.5.2 */
#define OTA_APP_DESC_VERSION_OFF  16u /* desc-relative offset of char version[32] */
#define OTA_APP_DESC_PROJECT_OFF  48u /* desc-relative offset of char project_name[32] (16 + 32) */
#define OTA_PROJECT_NAME_OFFSET  (OTA_APP_DESC_OFFSET + OTA_APP_DESC_PROJECT_OFF)   /* 80 */
#define OTA_PROJECT_NAME_CAP     32u  /* sizeof(esp_app_desc_t::project_name) */
#define OTA_APP_DESC_MAGIC       0xABCD5432u   /* ESP_APP_DESC_MAGIC_WORD */
#define OTA_PROJECT_NAME         "SilbapRemoteStick"
#define OTA_PROJECT_NAME_LEN     (sizeof(OTA_PROJECT_NAME) - 1u)   /* 16, without the NUL */
/* The smallest prefix that can hold a complete app description (32 + 256). */
#define OTA_MIN_IMAGE_BYTES      (OTA_APP_DESC_OFFSET + OTA_APP_DESC_SIZE)   /* 288 */

/* Non-zero results, one per reason, so the caller can pick a status/reason phrase:
 *   OTA_CHECK_SHORT              n (or the pointer) does not even reach the end of the app description
 *   OTA_CHECK_BAD_MAGIC          the description magic word is not ESP_APP_DESC_MAGIC_WORD
 *   OTA_CHECK_NAME_UNTERMINATED  all 32 project_name bytes are non-NUL: not a name we can trust
 *   OTA_CHECK_WRONG_PROJECT      a well-formed name, but not OTA_PROJECT_NAME (another app's image) */
enum {
    OTA_CHECK_OK = 0,
    OTA_CHECK_SHORT = 1,
    OTA_CHECK_BAD_MAGIC = 2,
    OTA_CHECK_NAME_UNTERMINATED = 3,
    OTA_CHECK_WRONG_PROJECT = 4,
};

/* Validates the first bytes of a firmware image. Returns 0 (OTA_CHECK_OK) when the image is ours, one of the
 * codes above otherwise. Never reads at or past first4k[n]; first4k may be NULL when n is 0.
 *
 * Copies the project name found in the image into name_out (NUL-terminated) even when it does not match, so the
 * caller can log what it saw. name_out may be NULL and name_cap may be 0 (then nothing is written). A buffer too
 * small for the whole name is truncated to name_cap - 1 bytes: truncation is not an error and does not change the
 * return code. name_out receives:
 *   - for OTA_CHECK_OK / BAD_MAGIC / NAME_UNTERMINATED / WRONG_PROJECT: the readable part of the 32-byte field
 *     (up to the first NUL; all 32 bytes when unterminated), truncated to fit name_out;
 *   - for OTA_CHECK_SHORT: whatever prefix of the field n still covers (truncated at a NUL), else "". */
int ota_check_header(const uint8_t *first4k, size_t n, char *name_out, size_t name_cap);

/* ---- the two other decisions that are worth testing without hardware ---- */

/* Result of the /api/ota upload gate. */
enum {
    OTA_GATE_PASSWORD = 0,   /* the device booted with a password: Basic auth (middleware) is the gate */
    OTA_GATE_CONFIRM = 1,    /* booted without one: a confirmed BOOT press is open, use and close it */
    OTA_GATE_REFUSE = 2,     /* booted without one and no confirmed press: 403 "arm" */
};

/* Decides the BOOT/password gate from the password state LATCHED AT BOOT, not the live s_set->pw_set. A device that
 * booted without a password therefore always needs the arm + press for OTA, even when a password is set during that
 * same boot (POST /api/password is reachable unauthenticated then, so the live flag cannot carry a physical-presence
 * claim). A device that booted with a password relies on the middleware and never consults confirm_open. */
int ota_gate(bool pw_at_boot, bool confirm_open);

/* Mapping for an esp_ota_end() failure: only ESP_ERR_OTA_VALIDATE_FAILED is the image's fault (400 wrong_image);
 * every other error is flash I/O or partition state (500 err.io). err is the raw esp_err_t value. */
enum {
    OTA_END_WRONG_IMAGE = 0, /* 400 wrong_image */
    OTA_END_IO = 1,          /* 500 err.io */
};
int ota_end_error(int err);
