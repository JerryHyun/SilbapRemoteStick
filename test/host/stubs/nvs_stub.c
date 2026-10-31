/* Host-test stub of the NVS API: one in-RAM namespace table, no persistence, no wear concerns.
 * Only what main/settings.c uses; a missing key or an absent namespace returns ESP_ERR_NVS_NOT_FOUND,
 * which is exactly what settings_load() treats as "use the default". */
#include "nvs.h"
#include "esp_random.h"
#include "nvs_stub.h"
#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 16
#define MAX_KEY     15
#define MAX_BLOB    128

typedef struct { int used; char key[MAX_KEY + 1]; unsigned char blob[MAX_BLOB]; size_t len; } entry_t;
static entry_t s_e[MAX_ENTRIES];
static int s_open;          /* the namespace exists once it has been opened read-write */

void nvs_stub_reset(void) {
    memset(s_e, 0, sizeof s_e);
    s_open = 0;
}
void nvs_stub_set_u8(const char *key, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open("silbap", NVS_READWRITE, &h) != 0) return;
    nvs_set_u8(h, key, v);
}

static entry_t *find(const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (s_e[i].used && !strcmp(s_e[i].key, key)) return &s_e[i];
    return NULL;
}
static entry_t *slot(const char *key) {
    entry_t *e = find(key);
    if (e) return e;
    for (int i = 0; i < MAX_ENTRIES; i++)
        if (!s_e[i].used) {
            e = &s_e[i];
            e->used = 1;
            snprintf(e->key, sizeof e->key, "%s", key);
            return e;
        }
    return NULL;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    if (!out || strcmp(name, "silbap")) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READONLY && !s_open) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) s_open = 1;
    *out = 1;
    return 0;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return 0; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof *e);
    return 0;
}
esp_err_t nvs_erase_all(nvs_handle_t h) {
    (void)h;
    for (int i = 0; i < MAX_ENTRIES; i++) memset(&s_e[i], 0, sizeof s_e[i]);
    return 0;
}

/* Every scalar is stored as a blob of its native width, so a get with the wrong width fails like NVS. */
static esp_err_t put(nvs_handle_t h, const char *key, const void *v, size_t n) {
    (void)h; entry_t *e = slot(key);
    if (!e || n > MAX_BLOB) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(e->blob, v, n); e->len = n;
    return 0;
}
static esp_err_t get(const char *key, void *v, size_t n) {
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->len != n) return ESP_ERR_NVS_TYPE_MISMATCH;
    memcpy(v, e->blob, n);
    return 0;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) { (void)h; return get(key, out, sizeof *out); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { return put(h, key, &v, sizeof v); }
esp_err_t nvs_get_i16(nvs_handle_t h, const char *key, int16_t *out) { (void)h; return get(key, out, sizeof *out); }
esp_err_t nvs_set_i16(nvs_handle_t h, const char *key, int16_t v) { return put(h, key, &v, sizeof v); }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *out) { (void)h; return get(key, out, sizeof *out); }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) { return put(h, key, &v, sizeof v); }

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
    (void)h;
    entry_t *e = find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) { *len = e->len; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, e->blob, e->len); *len = e->len;
    return 0;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len) { return put(h, key, v, len); }

uint32_t esp_random(void) { return 0x12345678; }
void esp_fill_random(void *buf, size_t len) { memset(buf, 0xA5, len); }
