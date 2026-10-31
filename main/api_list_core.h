#pragma once
/* Pure helpers of the files API (host-tested): list entries, URL decoding, flat-JSON string fields. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "namemap.h"
#include "webcore.h"

#define API_MAX_DEPTH 8   /* folders deeper than this are reported "too_deep", not shown to the machine */
/* Largest file the disk shows: 4 GiB - 1 (FAT32); on ESP-IDF off_t is 32-bit, so there files >= 2 GiB are hidden.
 * Larger files are reported "too_big" rather than listed. */
#ifdef ESP_PLATFORM
#define API_MAX_FILE_BYTES 0x7FFFFFFFull
#else
#define API_MAX_FILE_BYTES 0xFFFFFFFFull
#endif

/* Writes the fields of one /api/list entry into the OPEN object of w (name, machine, dir, size, hidden_reason);
 * the caller may add more fields (mtime, pending) and closes it. depth = number of path components of the entry
 * (a file in the root is 1). machine = the predicted plain machine name (nm_romanize when rename is on, else
 * the name as is; "" for hidden entries). hidden_reason: "hidden" (fs_hidden_name), "too_big",
 * "too_deep" or "". */
void api_list_entry_fields(const char *name, bool dir, uint64_t size, int depth, bool rename, nm_lang_t lang, wc_json_t *w);
/* One complete {...} entry object. */
void api_list_entry(const char *name, bool dir, uint64_t size, int depth, bool rename, nm_lang_t lang, wc_json_t *w);

/* Percent-decodes in (n bytes, '+' stays '+') into out; false on a bad escape, an embedded NUL or overflow. */
bool api_url_decode(const char *in, size_t n, char *out, size_t out_sz);
typedef enum { API_INVALID = -1, API_ABSENT = 0, API_OK = 1 } api_get_t;
/* Value of query parameter key in "a=b&c=d" (not including the '?'), percent-decoded. API_ABSENT when the key is not there, API_INVALID for a
 * bad escape, an embedded NUL or a value too long for out. */
api_get_t api_query_get(const char *query, const char *key, char *out, size_t out_sz);
/* String member `key` of a flat JSON object (top-level only; handles \" \\ \/ \b \f \n \r \t \uXXXX incl. surrogate
 * pairs). API_ABSENT when the key is not there in a well-formed object; API_INVALID when the JSON is malformed, the
 * member is not a string or it is too long for out. */
api_get_t api_json_get_str(const char *json, const char *key, char *out, size_t out_sz);
