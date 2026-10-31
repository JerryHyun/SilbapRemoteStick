/* The disk's hide rule, shared by the web listing, the upload listing and the "is the card empty" check. */
#include "fs_names.h"
#include <string.h>

static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool ieq(const char *a, const char *b) { for (; *a && lc(*a) == *b; a++, b++) {} return !*a && !*b; }
static bool iprefix(const char *a, const char *p) { for (; *p; a++, p++) if (lc(*a) != *p) return false; return true; }
bool fs_hidden_name(const char *n) {
    size_t l = strlen(n);
    return n[0] == '.' || ieq(n, "silbap-help.txt") || ieq(n, "thumbs.db") || ieq(n, "desktop.ini") ||
           ieq(n, "system volume information") || ieq(n, "$recycle.bin") ||
           (iprefix(n, "silbap-setup") && l >= 16 && ieq(n + l - 4, ".txt"));
}
