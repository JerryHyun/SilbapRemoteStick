#pragma once
#include <stdbool.h>
#include <stdint.h>
/* PUBLIC: no password needed even when one is set. AUTH: Basic password required when pw_set. */
typedef enum { WEB_PUBLIC, WEB_AUTH } web_access_t;
/* Pure middleware decision: 200 = let through, 403 = Host/Origin rejected, 401 = password needed.
 * Host is always checked; Origin (if present) for every method except GET/HEAD/OPTIONS/PROPFIND; then auth.
 * allowed: NULL-terminated host list (see wc_host_ok). salt/hash may be NULL when !pw_set. */
int web_decide(const char *method, const char *host, const char *origin, const char *auth_hdr, bool pw_set,
               const uint8_t *salt, const uint8_t *hash, web_access_t access, const char *const *allowed);
