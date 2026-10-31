/* Start-of-image validation for an uploaded firmware image: see main/ota_check.h. Pure byte inspection so the
 * same code runs on the ESP32-S3 and on a plain host (test/host/test_ota_check.c). */
#include "ota_check.h"

#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
/* The constants in ota_check.h mirror the IDF layout. If IDF ever changes it, this file stops compiling
 * instead of silently reading the wrong bytes. */
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"

/* ota_end_error() is pure, so it compares against the literal; this keeps the literal honest. */
_Static_assert(ESP_ERR_OTA_VALIDATE_FAILED == 0x1503, "ESP_ERR_OTA_VALIDATE_FAILED moved");

_Static_assert(sizeof(esp_image_header_t) == OTA_IMAGE_HEADER_SIZE, "esp_image_header_t is not 24 bytes");
_Static_assert(sizeof(esp_image_segment_header_t) == OTA_SEGMENT_HEADER_SIZE,
               "esp_image_segment_header_t is not 8 bytes");
_Static_assert(sizeof(esp_app_desc_t) == OTA_APP_DESC_SIZE, "esp_app_desc_t is not 256 bytes");
_Static_assert(offsetof(esp_app_desc_t, version) == OTA_APP_DESC_VERSION_OFF,
               "esp_app_desc_t::version moved");
_Static_assert(offsetof(esp_app_desc_t, project_name) == OTA_APP_DESC_PROJECT_OFF,
               "esp_app_desc_t::project_name moved");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == OTA_APP_DESC_MAGIC, "ESP_APP_DESC_MAGIC_WORD changed");
#endif

/* Little-endian u32 from p; bytewise, so alignment and strict aliasing never matter. */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Copies at most cap - 1 bytes of src[0..len) into out and terminates it. len is never above the bytes that were
 * actually present in the image. */
static void copy_limited(char *out, size_t cap, const uint8_t *src, size_t len)
{
    size_t i;

    if (out == NULL || cap == 0) {
        return;
    }
    for (i = 0; i + 1 < cap && i < len && src[i] != '\0'; i++) {
        out[i] = (char)src[i];
    }
    out[i] = '\0';
}

int ota_check_header(const uint8_t *first4k, size_t n, char *name_out, size_t name_cap)
{
    uint32_t magic;
    size_t field_off = OTA_PROJECT_NAME_OFFSET;
    size_t avail;      /* project_name bytes present in the buffer */
    size_t name_len;   /* bytes before the first NUL, or 32 when there is none */
    bool terminated;

    if (first4k == NULL || n < OTA_MIN_IMAGE_BYTES) {
        /* Too short to hold a description: still report the readable part of the name we did get. */
        if (first4k != NULL && n > field_off) {
            avail = n - field_off;
            if (avail > OTA_PROJECT_NAME_CAP) {
                avail = OTA_PROJECT_NAME_CAP;
            }
        } else {
            avail = 0;
        }
        copy_limited(name_out, name_cap, first4k != NULL ? first4k + field_off : NULL, avail);
        return OTA_CHECK_SHORT;
    }

    magic = read_le32(first4k + OTA_APP_DESC_OFFSET);
    if (magic != OTA_APP_DESC_MAGIC) {
        copy_limited(name_out, name_cap, first4k + field_off, OTA_PROJECT_NAME_CAP);
        return OTA_CHECK_BAD_MAGIC;
    }

    avail = OTA_PROJECT_NAME_CAP;
    name_len = 0;
    while (name_len < avail && first4k[field_off + name_len] != '\0') {
        name_len++;
    }
    terminated = name_len < avail;
    copy_limited(name_out, name_cap, first4k + field_off, terminated ? name_len : avail);

    if (!terminated) {
        return OTA_CHECK_NAME_UNTERMINATED;
    }
    if (name_len != OTA_PROJECT_NAME_LEN ||
        memcmp(first4k + field_off, OTA_PROJECT_NAME, OTA_PROJECT_NAME_LEN) != 0) {
        return OTA_CHECK_WRONG_PROJECT;
    }
    return OTA_CHECK_OK;
}

int ota_gate(bool pw_at_boot, bool confirm_open)
{
    if (pw_at_boot) {
        return OTA_GATE_PASSWORD;
    }
    return confirm_open ? OTA_GATE_CONFIRM : OTA_GATE_REFUSE;
}

int ota_end_error(int err)
{
    /* ESP_ERR_OTA_VALIDATE_FAILED is 0x1503 (ESP_ERR_OTA_BASE + 3); spelled out because this file stays pure. */
    return err == 0x1503 ? OTA_END_WRONG_IMAGE : OTA_END_IO;
}
