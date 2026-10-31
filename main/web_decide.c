#include "web_decide.h"
#include <string.h>
#include "webcore.h"

static bool is_safe_method(const char *m) {
    return !strcmp(m, "GET") || !strcmp(m, "HEAD") || !strcmp(m, "OPTIONS") || !strcmp(m, "PROPFIND");
}

int web_decide(const char *method, const char *host, const char *origin, const char *auth_hdr, bool pw_set,
               const uint8_t *salt, const uint8_t *hash, web_access_t access, const char *const *allowed) {
    if (!method || !wc_host_ok(host, allowed)) return 403;
    if (!is_safe_method(method) && !wc_origin_ok(origin, allowed)) return 403;
    if (access == WEB_AUTH && pw_set) {
        char pw[65];
        uint8_t h[32];
        if (!salt || !hash || !wc_basic_password(auth_hdr, pw, sizeof pw)) return 401;
        wc_pw_hash(pw, salt, h);
        memset(pw, 0, sizeof pw);
        if (!wc_ct_equal(h, hash, 32)) return 401;
    }
    return 200;
}
