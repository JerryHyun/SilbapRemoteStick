#pragma once
/* Pure pieces of the WebDAV server (main/webdav.c), host-tested by test/host/test_webdav_xml.c: hrefs, XML text,
 * dates, PROPFIND entries, Range, request paths, Expect and PROPPATCH echoes. Nothing here touches the card. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAV_ROOT "/dav"

/* Percent-encodes path for an href: unreserved bytes (A-Z a-z 0-9 - . _ ~) and '/' stay, every other byte (UTF-8
 * included) becomes %XX in upper-case hex. false (out = "") when out is too small. */
bool dav_href_encode(const char *path, char *out, size_t sz);
/* & < > " ' -> entities. false (out = "") when it does not fit: a truncated result never holds half an entity. */
bool xml_escape(const char *in, char *out, size_t sz);
void dav_http_date(int64_t t, char *out, size_t sz);   /* RFC 1123: "Sun, 06 Nov 1994 08:49:37 GMT" (sz >= 30) */
void dav_iso_date(int64_t t, char *out, size_t sz);    /* RFC 3339: "1994-11-06T08:49:37Z" (sz >= 21) */
const char *dav_content_type(const char *name);        /* by extension; application/octet-stream otherwise */

/* One complete <D:response> of a PROPFIND answer for rel (normalised card path, "" = root): href DAV_ROOT "/" +
 * encoded rel (collections end in '/'), displayname, resourcetype, getcontentlength + getcontenttype (files),
 * creationdate, getlastmodified, getetag, supportedlock. Returns the length, or -1 when it does not fit. */
int dav_propfind_entry(char *out, size_t sz, const char *rel, const char *name, bool dir, uint64_t size, int64_t mtime);

/* Range header against a file of size bytes. 0 = send the whole file (absent, not "bytes=", several ranges or
 * malformed), 1 = *first..*last inclusive (clamped to the file), -1 = unsatisfiable (416). */
int dav_parse_range(const char *hdr, uint64_t size, uint64_t *first, uint64_t *last);

/* Request URI or Destination header -> the percent-decoded path below DAV_ROOT ("" for "/dav", "/x" for "/dav/x").
 * An absolute "scheme://authority/dav/..." URL is accepted; query and fragment are dropped. */
typedef enum {
    DAV_URI_OK = 0,     /* decoded into out */
    DAV_URI_NOT_DAV,    /* well-formed, but not DAV_ROOT or below it */
    DAV_URI_BAD,        /* not a URI we can read: bad escape, embedded NUL, empty, too small/long */
} dav_uri_t;
dav_uri_t dav_uri_path_ex(const char *uri, char *out, size_t sz);
/* true exactly when dav_uri_path_ex() == DAV_URI_OK. */
bool dav_uri_path(const char *uri, char *out, size_t sz);

/* Decoded path -> card path: wc_norm_path (rejects "..", empty segments, control chars and every ".silbap*"
 * segment, case-insensitively), then wc_sd_safe_name on each segment, then wc_norm_path again. out needs
 * WC_PATH_MAX + 1 bytes. */
typedef enum {
    DAV_PATH_OK = 0,    /* decoded into out */
    DAV_PATH_ABSENT,    /* hidden or unreachable: ".silbap*", "..", an empty segment -> answer 404 */
    DAV_PATH_BAD,       /* structurally bad input: control char, name > 255 UTF-8 bytes, a name that trims to
                         * nothing, or a path/out that does not fit -> answer 400 */
} dav_path_t;
dav_path_t dav_rel_status(const char *raw, char *out, size_t sz);
/* true exactly when dav_rel_status() == DAV_PATH_OK. */
bool dav_rel(const char *raw, char *out, size_t sz);

/* Expect header value (RFC 9110 #expectation). NONE when absent or empty; CONTINUE when every listed expectation
 * is "100-continue" (case-insensitive, optional whitespace); UNSUPPORTED otherwise -> 417. */
typedef enum { DAV_EXPECT_NONE = 0, DAV_EXPECT_CONTINUE, DAV_EXPECT_UNSUPPORTED } dav_expect_t;
dav_expect_t dav_expect(const char *hdr);

/* PROPPATCH body -> the children of a 207 <D:prop>: one empty element per property named directly inside any
 * <...:prop> element (set and remove alike), each declaring its own namespace ("<z:name xmlns:z=\"uri\"/>", or
 * "<name xmlns=\"\"/>" without one). Malformed or truncated input echoes what was complete before it. Returns the
 * length written, or -1 when out is too small. */
int dav_proppatch_props(const char *body, char *out, size_t sz);
