#pragma once
#include <stdbool.h>
/* true for names the disk does not show as "content" (dot names, Windows/macOS clutter, Silbap's own help and
 * setup files). Used by the web listing, the upload listing and the empty-card check. */
bool fs_hidden_name(const char *name);
