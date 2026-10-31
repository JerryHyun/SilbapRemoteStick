#pragma once
#include <stdbool.h>
#include "settings.h"
/* Boot-time setup file. Call from app_main once the card is mounted and refresh_prepare_sd() has run, and strictly
 * before net_start(): the Wi-Fi credentials are written into s (the net layer is not up yet and
 * net_apply_credentials() would only return ESP_ERR_INVALID_STATE) and net_start() connects with them.
 *
 * /sd/silbap-setup.txt (at most 4 KiB) is parsed and whatever is present is applied: Wi-Fi credentials, the web
 * password (an empty value clears it), rename, language (which also sets ui_lang). Settings are saved once. The file
 * is then replaced by /sd/silbap-setup.done.txt, written through sf_masked() so every secret reads "****", and the
 * original is deleted. A larger setup file is refused and left in place. /sd/silbap-help.txt is written when it is
 * missing or its version line is stale. Every card access uses plain POSIX calls under sdcard_lock: fsops is for web
 * mutations and would request a refresh, and the refresh task does not exist yet. */
void setup_apply_boot(settings_t *s);

/* One-shot banner flag: true when this boot applied a setup file, cleared by the read. Feeds /api/status. */
bool setup_apply_took_effect(void);
