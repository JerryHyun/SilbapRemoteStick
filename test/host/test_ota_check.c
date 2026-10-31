/* Host tests for main/ota_check.c: validates the head of an uploaded firmware image before it reaches the OTA
 * slot. Synthetic images only; nothing is read from disk. */
#include "t.h"
#include "ota_check.h"

#define IMG_SIZE 4096u

/* A plausible image: zeroes, a 24-byte image header and an 8-byte first-segment header, then the app description
 * at offset 32 with the right magic and either a good or a supplied project name. */
static void make_image(uint8_t *img, size_t n, const char *project)
{
    size_t len = strlen(project);
    memset(img, 0, n);
    img[0] = 0xE9;              /* ESP_IMAGE_HEADER_MAGIC */
    img[1] = 3;                 /* segment count */
    img[2] = 0;                 /* SPI mode */
    img[23] = 1;                /* hash appended */
    img[24] = 0x20;             /* first segment load address 0x3C000020, little-endian */
    img[25] = 0x00;
    img[26] = 0x00;
    img[27] = 0x3C;
    /* desc magic at 32, bytewise little-endian 0xABCD5432 */
    img[OTA_APP_DESC_OFFSET + 0] = 0x32;
    img[OTA_APP_DESC_OFFSET + 1] = 0x54;
    img[OTA_APP_DESC_OFFSET + 2] = 0xCD;
    img[OTA_APP_DESC_OFFSET + 3] = 0xAB;
    memcpy(img + OTA_APP_DESC_OFFSET + OTA_APP_DESC_VERSION_OFF, "1.2.3", 5);
    if (len > OTA_PROJECT_NAME_CAP) {
        len = OTA_PROJECT_NAME_CAP;
    }
    memcpy(img + OTA_PROJECT_NAME_OFFSET, project, len);
}

static void test_accept(void)
{
    uint8_t img[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_OK);
    T_EQ_STR(name, "SilbapRemoteStick");
    T_EQ_INT(strlen(name), OTA_PROJECT_NAME_LEN);

    /* Exactly the minimum number of bytes is enough; one less is not. */
    T_EQ_INT(ota_check_header(img, OTA_MIN_IMAGE_BYTES, name, sizeof name), OTA_CHECK_OK);
    T_EQ_STR(name, "SilbapRemoteStick");
    T_EQ_INT(ota_check_header(img, OTA_MIN_IMAGE_BYTES - 1, name, sizeof name), OTA_CHECK_SHORT);

    /* A caller that does not want the name, and a full 4 KiB read, alike. */
    T_EQ_INT(ota_check_header(img, sizeof img, NULL, 0), OTA_CHECK_OK);
    T_EQ_INT(ota_check_header(img, IMG_SIZE, NULL, 0), OTA_CHECK_OK);
}

static void test_bad_magic(void)
{
    uint8_t img[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    img[OTA_APP_DESC_OFFSET + 3] = 0xAB ^ 0xFF;               /* corrupt the top byte */
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_BAD_MAGIC);
    T_EQ_STR(name, "SilbapRemoteStick");                       /* the name is still reported */

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    img[OTA_APP_DESC_OFFSET] = 0x00;                          /* all-zero desc (a random .bin) */
    img[OTA_APP_DESC_OFFSET + 1] = 0x00;
    img[OTA_APP_DESC_OFFSET + 2] = 0x00;
    img[OTA_APP_DESC_OFFSET + 3] = 0x00;
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_BAD_MAGIC);

    /* A byte-swapped magic is a different word, not an accepted one. */
    make_image(img, sizeof img, OTA_PROJECT_NAME);
    img[OTA_APP_DESC_OFFSET + 0] = 0xAB;
    img[OTA_APP_DESC_OFFSET + 1] = 0xCD;
    img[OTA_APP_DESC_OFFSET + 2] = 0x54;
    img[OTA_APP_DESC_OFFSET + 3] = 0x32;
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_BAD_MAGIC);
}

static void test_short(void)
{
    uint8_t img[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    T_EQ_INT(ota_check_header(img, 0, name, sizeof name), OTA_CHECK_SHORT); T_EQ_STR(name, "");
    T_EQ_INT(ota_check_header(img, 1, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_INT(ota_check_header(img, 31, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_INT(ota_check_header(img, OTA_APP_DESC_OFFSET, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_INT(ota_check_header(img, OTA_APP_DESC_OFFSET + 4, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_INT(ota_check_header(img, OTA_MIN_IMAGE_BYTES - 1, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_INT(ota_check_header(img, OTA_PROJECT_NAME_OFFSET, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_STR(name, "");                                        /* not one name byte present yet */

    /* A prefix that reaches into the name reports that prefix; it never claims a whole name. */
    T_EQ_INT(ota_check_header(img, OTA_PROJECT_NAME_OFFSET + 5, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_STR(name, "Silba");

    /* NULL with n == 0 is the empty buffer, not a crash. */
    T_EQ_INT(ota_check_header(NULL, 0, name, sizeof name), OTA_CHECK_SHORT);
    T_EQ_STR(name, "");
}

static void test_wrong_project(void)
{
    uint8_t img[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];

    make_image(img, sizeof img, "SomeOtherApp");
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    T_EQ_STR(name, "SomeOtherApp");                            /* the caller can log what it saw */

    make_image(img, sizeof img, "");
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    T_EQ_STR(name, "");

    /* Every near miss is a miss: case, a prefix, a suffix, one changed byte. */
    make_image(img, sizeof img, "silbapremotestick");
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    make_image(img, sizeof img, "SilbapRemoteStic");           /* 15 chars + NUL */
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    make_image(img, sizeof img, "SilbapRemoteStickX");         /* 17 chars + NUL */
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    make_image(img, sizeof img, "SilbapRemoteSticj");
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    /* A 16-byte prefix with the right length but trailing garbage instead of the NUL. */
    make_image(img, sizeof img, OTA_PROJECT_NAME);
    img[OTA_PROJECT_NAME_OFFSET + OTA_PROJECT_NAME_LEN] = 'x';
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_WRONG_PROJECT);
    T_EQ_STR(name, "SilbapRemoteStickx");
}

static void test_unterminated(void)
{
    uint8_t img[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    memset(img + OTA_PROJECT_NAME_OFFSET, 'A', OTA_PROJECT_NAME_CAP);   /* 32 non-NUL bytes */
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_NAME_UNTERMINATED);
    T_EQ_STR(name, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");                 /* all 32, NUL-terminated by us */

    /* Even the exact name followed by 17 filler bytes is unterminated, hence rejected. */
    make_image(img, sizeof img, OTA_PROJECT_NAME);
    memset(img + OTA_PROJECT_NAME_OFFSET + OTA_PROJECT_NAME_LEN, 'A',
           OTA_PROJECT_NAME_CAP - OTA_PROJECT_NAME_LEN);
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_NAME_UNTERMINATED);
    T_CHECK(strncmp(name, "SilbapRemoteStickA", 18) == 0);
    T_EQ_INT(strlen(name), OTA_PROJECT_NAME_CAP);

    /* One NUL anywhere inside the 32 bytes is enough to be "terminated", even at the last byte. */
    make_image(img, sizeof img, OTA_PROJECT_NAME);
    img[OTA_PROJECT_NAME_OFFSET + OTA_PROJECT_NAME_CAP - 1] = '\0';
    T_EQ_INT(ota_check_header(img, sizeof img, name, sizeof name), OTA_CHECK_OK);
    T_EQ_STR(name, "SilbapRemoteStick");
}

/* name_out may be tiny or absent: truncate, never overflow, never crash, and never change the verdict. */
static void test_name_cap(void)
{
    uint8_t img[IMG_SIZE];
    char small[5];
    char one[1];

    make_image(img, sizeof img, OTA_PROJECT_NAME);
    memset(small, 'Z', sizeof small);
    T_EQ_INT(ota_check_header(img, sizeof img, small, sizeof small), OTA_CHECK_OK);
    T_EQ_STR(small, "Silb");                                   /* truncated, still a valid C string */

    memset(one, 'Z', sizeof one);
    T_EQ_INT(ota_check_header(img, sizeof img, one, sizeof one), OTA_CHECK_OK);
    T_EQ_INT(one[0], 0);                                       /* cap 1 holds only the NUL */

    T_EQ_INT(ota_check_header(img, sizeof img, small, 0), OTA_CHECK_OK);
    T_EQ_STR(small, "Silb");                                   /* cap 0 writes nothing at all */

    /* Truncation does not turn a wrong image into an accepted one. */
    make_image(img, sizeof img, "SomeOtherApp");
    T_EQ_INT(ota_check_header(img, sizeof img, small, sizeof small), OTA_CHECK_WRONG_PROJECT);
    T_EQ_STR(small, "Some");

    /* A name exactly as large as the field fits with its NUL. */
    char exact[OTA_PROJECT_NAME_LEN + 1];
    make_image(img, sizeof img, OTA_PROJECT_NAME);
    T_EQ_INT(ota_check_header(img, sizeof img, exact, sizeof exact), OTA_CHECK_OK);
    T_EQ_STR(exact, "SilbapRemoteStick");
}

/* The buffer length is authoritative: bytes past n are garbage and must not be read. Run the check against a copy
 * that is truncated at every length, for both a good and a bad image, and make sure the answer only flips at
 * OTA_MIN_IMAGE_BYTES and the name never contains bytes from outside n. */
static void test_never_reads_past_n(void)
{
    uint8_t full[IMG_SIZE];
    uint8_t part[IMG_SIZE];
    char name[OTA_PROJECT_NAME_CAP + 1];
    size_t n;

    make_image(full, sizeof full, OTA_PROJECT_NAME);
    for (n = 0; n <= OTA_MIN_IMAGE_BYTES + 8; n++) {
        memset(part, 0x5A, sizeof part);                       /* garbage past the cut */
        memcpy(part, full, n);
        int rc = ota_check_header(part, n, name, sizeof name);
        T_EQ_INT(rc, n >= OTA_MIN_IMAGE_BYTES ? OTA_CHECK_OK : OTA_CHECK_SHORT);
        T_CHECK(strlen(name) <= n);                            /* nothing from beyond n leaked in */
        if (n >= OTA_PROJECT_NAME_OFFSET) {
            T_CHECK(strlen(name) <= n - OTA_PROJECT_NAME_OFFSET);
        } else {
            T_EQ_STR(name, "");
        }
    }
}

/* The BOOT gate is decided from the password state captured at boot, never from the live one: a device that booted
 * without a password always needs the arm + press, even if a password is set later in that boot. */
static void test_gate(void)
{
    /* Booted with a password: the middleware is the gate, the arm window plays no part. */
    T_EQ_INT(ota_gate(true, false), OTA_GATE_PASSWORD);
    T_EQ_INT(ota_gate(true, true), OTA_GATE_PASSWORD);
    /* Booted without one: only a confirmed BOOT press lets the upload through, and it is consumed. */
    T_EQ_INT(ota_gate(false, true), OTA_GATE_CONFIRM);
    T_EQ_INT(ota_gate(false, false), OTA_GATE_REFUSE);
}

/* esp_ota_end() fails with ESP_ERR_OTA_VALIDATE_FAILED for an unbootable image (400 wrong_image); its other errors
 * are flash/partition I/O and must be reported as a server error (500 err.io), like esp_ota_write's. */
static void test_end_error(void)
{
    T_EQ_INT(ota_end_error(0x1503), OTA_END_WRONG_IMAGE);       /* ESP_ERR_OTA_VALIDATE_FAILED */
    T_EQ_INT(ota_end_error(0x1504), OTA_END_IO);                /* ESP_ERR_OTA_SMALL_SEC_VER */
    T_EQ_INT(ota_end_error(0x1505), OTA_END_IO);                /* ESP_ERR_OTA_ROLLBACK_FAILED */
    T_EQ_INT(ota_end_error(0x1501), OTA_END_IO);                /* ESP_ERR_OTA_PARTITION_CONFLICT */
    T_EQ_INT(ota_end_error(0x103), OTA_END_IO);                 /* ESP_ERR_INVALID_STATE */
    T_EQ_INT(ota_end_error(0x107), OTA_END_IO);                 /* ESP_ERR_TIMEOUT */
    T_EQ_INT(ota_end_error(0), OTA_END_IO);                     /* not an image verdict: never "wrong image" */
    T_EQ_INT(ota_end_error(-1), OTA_END_IO);
}

int main(void)
{
    T_RUN(test_accept);
    T_RUN(test_bad_magic);
    T_RUN(test_short);
    T_RUN(test_wrong_project);
    T_RUN(test_unterminated);
    T_RUN(test_name_cap);
    T_RUN(test_never_reads_past_n);
    T_RUN(test_gate);
    T_RUN(test_end_error);
    T_DONE();
}
