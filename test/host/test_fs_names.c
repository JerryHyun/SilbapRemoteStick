#include "t.h"
#include "fs_names.h"

/* The hide rule shared by the web listing, the upload listing and the "is the card empty" check. */
static void test_hidden_name(void) {
    T_CHECK(fs_hidden_name("System Volume Information")); T_CHECK(fs_hidden_name("$RECYCLE.BIN"));
    T_CHECK(fs_hidden_name(".silbap")); T_CHECK(fs_hidden_name("Thumbs.db")); T_CHECK(fs_hidden_name("silbap-setup-ko.txt"));
    T_CHECK(fs_hidden_name("silbap-setup.done.txt"));
    T_CHECK(fs_hidden_name("silbap-setup.txt"));
    T_CHECK(fs_hidden_name("silbap-help.txt"));
    T_CHECK(fs_hidden_name("DESKTOP.INI"));
    T_CHECK(!fs_hidden_name("EMB")); T_CHECK(!fs_hidden_name("silbap-setup.jef"));
    /* "silbap-setup" + ".txt" is exactly 16 bytes; anything shorter is not the setup file */
    T_CHECK(!fs_hidden_name("silbap-setup.txt "));
    T_CHECK(fs_hidden_name("silbap-setup-.txt"));
}
int main(void) { T_RUN(test_hidden_name); T_DONE(); }
