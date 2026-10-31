#pragma once
#include "settings.h"
/* Registers /api/status, /api/help (PUBLIC) and /api/settings (GET, POST), /api/password, /api/wifi, /api/time,
 * /api/refresh (WEB_AUTH). Call from web_start after the server runs. s must outlive the server. */
void api_settings_register(settings_t *s);
