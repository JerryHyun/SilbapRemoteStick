#pragma once
#include "settings.h"
/* Registers /api/list, /api/count, /api/mkdir, /api/rename, /api/move, /api/delete and /api/upload behind the web
 * middleware (WEB_AUTH). Call from web_start after the server runs. s must outlive the server (rename/lang/tz_min are
 * read per request). */
void api_files_register(settings_t *s);
