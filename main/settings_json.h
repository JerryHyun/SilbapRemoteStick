#pragma once
/* Pure helpers of the settings API (host-tested): a tiny scanner over a flat JSON object and the partial-settings merge. */
#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

/* Merges the known keys of a flat JSON object into s: rename and auto_refresh (booleans), lang and
 * ui_lang ("ko"/"en"/"zh"/"ja"), media ("sense"/"replug"), tz_min (integer, -840..840). Unknown keys (including
 * any password key) are ignored. Nothing is changed when 0 is not returned. Only the touched fields are written,
 * one by one, so the password fields are never copied over. *needs_rebuild is set (never cleared) when rename or
 * lang changes value. Returns 0, or -1 for malformed JSON or a known key with a wrong type or value. */
int settings_merge_json(settings_t *s, const char *json, bool *needs_rebuild);

/* Top-level integer member. 1 = found, 0 = absent, -1 = malformed JSON or not an integer (no fraction/exponent). */
int sj_get_int(const char *json, const char *key, int64_t *out);
