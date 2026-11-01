#include "web_decide.h"
#include <string.h>
#include "webcore.h"

web_root_t web_root_target(bool setup_only) { return setup_only ? WEB_ROOT_SETUP : WEB_ROOT_APP; }

/* Methods a browser can issue cross-origin without a CORS preflight, and which cannot change the volume. The
 * Origin check exists to stop a drive-by page from making the device *mutate* (CSRF); a read-only method has no
 * state to protect (HOST is still checked on every request, so DNS rebinding is refused, and a cross-origin
 * response cannot be read by the page without CORS headers, which this server never sends).
 *
 * PROPFIND is deliberately in this list: webdav.c dispatches it to dav_propfind, which only stats, opendirs and
 * streams XML -- it never opens a file for writing and never calls fsops. The only cross-origin effect is that the
 * Origin header is not required, and a cross-origin PROPFIND cannot read the answer. LOCK/PROPPATCH/UNLOCK look
 * like they might belong here but are handled by dav_lock/dav_proppatch, which answer without touching the card;
 * they are NOT listed because they are not methods a browser sends as a simple request, so the Origin check stays
 * on for them at no cost. Keep this list minimal: any method that reaches fsops or a card write must not be here. */
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
