#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WC_PATH_MAX 512
/* rel: URL-decoded path from the web, '/' separated, may start with '/'. out: normalised "a/b/c" (no leading '/'; "" = root).
 * false for "..", ".", empty segments, control chars, any segment starting with ".silbap", or > WC_PATH_MAX. */
bool wc_norm_path(const char *rel, char *out, size_t out_sz);
/* One name from a client -> name safe on FAT: \ / : * ? " < > | -> full-width; control chars -> '_'; trailing '.'/' ' trimmed.
 * Returns false (reject) if empty after cleanup or > 255 UTF-8 bytes. */
bool wc_sd_safe_name(const char *in, char *out, size_t out_sz);
/* "rose.jef", n=2 -> "rose (2).jef"; "noext", 3 -> "noext (3)"; ".hidden", 2 -> ".hidden (2)". */
void wc_conflict_name(const char *name, unsigned n, char *out, size_t out_sz);
/* allowed: list of host names (no port), e.g. {"192.168.0.42","silbapstick.local",NULL}. */
bool wc_host_ok(const char *host_hdr, const char *const *allowed);
bool wc_origin_ok(const char *origin_hdr /* NULL = absent */, const char *const *allowed);
/* "Basic base64(user:pass)" -> pass (user ignored). false if malformed. */
bool wc_basic_password(const char *auth_hdr, char *pass, size_t pass_sz);
void wc_sha256(const void *data, size_t len, uint8_t out[32]);
void wc_pw_hash(const char *pw, const uint8_t salt[16], uint8_t out[32]);   /* sha256(salt || pw); uses at most 128 bytes of pw (callers cap passwords at 64) */
bool wc_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);
/* JSON writer: appends to buf; on overflow sets w->overflow and stops writing. */
typedef struct { char *buf; size_t cap, len; bool overflow; bool need_comma; } wc_json_t;
void wc_json_init(wc_json_t *w, char *buf, size_t cap);
void wc_json_obj_open(wc_json_t *w); void wc_json_obj_close(wc_json_t *w);
void wc_json_arr_open(wc_json_t *w, const char *key /* NULL inside arrays */); void wc_json_arr_close(wc_json_t *w);
void wc_json_str(wc_json_t *w, const char *key, const char *val);  /* escapes " \ control chars; UTF-8 passed through */
void wc_json_num(wc_json_t *w, const char *key, int64_t val);
void wc_json_bool(wc_json_t *w, const char *key, bool val);
