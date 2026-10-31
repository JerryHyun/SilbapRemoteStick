#include "t.h"
#include "webdav_xml.h"

static void test_href_encode(void) {
    char b[128];
    T_CHECK(dav_href_encode("\xEC\x9E\xA5\xEB\xAF\xB8 1.jef", b, sizeof b));   /* "장미 1.jef" */
    T_EQ_STR(b, "%EC%9E%A5%EB%AF%B8%201.jef");
    T_CHECK(dav_href_encode("a/b c/A-Z_0.9~", b, sizeof b));
    T_EQ_STR(b, "a/b%20c/A-Z_0.9~");
    T_CHECK(dav_href_encode("x&y<z>%#?", b, sizeof b));
    T_EQ_STR(b, "x%26y%3Cz%3E%25%23%3F");
    T_CHECK(dav_href_encode("", b, sizeof b)); T_EQ_STR(b, "");
    char s[7];
    T_CHECK(!dav_href_encode("ab \xEC", s, sizeof s));      /* "ab%20%EC" needs 9 bytes */
    T_EQ_STR(s, "");
    T_CHECK(dav_href_encode("abc%", s, sizeof s)); T_EQ_STR(s, "abc%25");   /* exactly fits */
}

static void test_xml_escape(void) {
    char b[64];
    T_CHECK(xml_escape("a&b<c>d\"e'f", b, sizeof b));
    T_EQ_STR(b, "a&amp;b&lt;c&gt;d&quot;e&apos;f");
    T_CHECK(xml_escape("\xEC\x9E\xA5\xEB\xAF\xB8", b, sizeof b)); T_EQ_STR(b, "\xEC\x9E\xA5\xEB\xAF\xB8");
    char s[6];
    T_CHECK(!xml_escape("&&", s, sizeof s)); T_EQ_STR(s, "");   /* never a half entity */
    T_CHECK(xml_escape("&", s, sizeof s)); T_EQ_STR(s, "&amp;");
}

static void test_dates(void) {
    char b[40];
    dav_http_date(784111777, b, sizeof b); T_EQ_STR(b, "Sun, 06 Nov 1994 08:49:37 GMT");
    dav_iso_date(784111777, b, sizeof b);  T_EQ_STR(b, "1994-11-06T08:49:37Z");
}

static void test_propfind_entry(void) {
    char b[4096];
    int n = dav_propfind_entry(b, sizeof b, "\xEC\x9E\xA5\xEB\xAF\xB8/a&b <1>.jef", "a&b <1>.jef", false, 1234, 784111777);
    T_CHECK(n > 0); T_EQ_INT(n, (long long)strlen(b));
    T_CHECK(strstr(b, "<D:href>/dav/%EC%9E%A5%EB%AF%B8/a%26b%20%3C1%3E.jef</D:href>") != NULL);
    T_CHECK(strstr(b, "<D:displayname>a&amp;b &lt;1&gt;.jef</D:displayname>") != NULL);
    T_CHECK(strstr(b, "<D:resourcetype/>") != NULL);
    T_CHECK(strstr(b, "<D:getcontentlength>1234</D:getcontentlength>") != NULL);
    T_CHECK(strstr(b, "<D:getlastmodified>Sun, 06 Nov 1994 08:49:37 GMT</D:getlastmodified>") != NULL);
    T_CHECK(strstr(b, "<D:creationdate>1994-11-06T08:49:37Z</D:creationdate>") != NULL);
    T_CHECK(strstr(b, "<D:getetag>") != NULL);
    T_CHECK(strstr(b, "<D:status>HTTP/1.1 200 OK</D:status>") != NULL);
    T_CHECK(!strncmp(b, "<D:response>", 12)); T_CHECK(strstr(b, "</D:response>") == b + n - 13);
    T_CHECK(strchr(b, '<') && !strstr(b, "a&b")); /* the raw name never appears unescaped */

    n = dav_propfind_entry(b, sizeof b, "dir one", "dir one", true, 0, 0);
    T_CHECK(n > 0);
    T_CHECK(strstr(b, "<D:href>/dav/dir%20one/</D:href>") != NULL);    /* collections end in '/' */
    T_CHECK(strstr(b, "<D:resourcetype><D:collection/></D:resourcetype>") != NULL);
    T_CHECK(strstr(b, "getcontentlength") == NULL);
    n = dav_propfind_entry(b, sizeof b, "", "", true, 0, 0);
    T_CHECK(n > 0); T_CHECK(strstr(b, "<D:href>/dav/</D:href>") != NULL);

    char s[200];
    T_EQ_INT(dav_propfind_entry(s, sizeof s, "x.jef", "x.jef", false, 1, 1), -1);   /* does not fit */
}

static void test_range(void) {
    uint64_t a = 0, z = 0;
    T_EQ_INT(dav_parse_range(NULL, 100, &a, &z), 0);
    T_EQ_INT(dav_parse_range("bytes=0-9", 100, &a, &z), 1); T_EQ_INT(a, 0); T_EQ_INT(z, 9);
    T_EQ_INT(dav_parse_range("bytes=90-", 100, &a, &z), 1); T_EQ_INT(a, 90); T_EQ_INT(z, 99);
    T_EQ_INT(dav_parse_range("bytes=90-500", 100, &a, &z), 1); T_EQ_INT(z, 99);   /* clamped */
    T_EQ_INT(dav_parse_range("bytes=-10", 100, &a, &z), 1); T_EQ_INT(a, 90); T_EQ_INT(z, 99);
    T_EQ_INT(dav_parse_range("bytes=-500", 100, &a, &z), 1); T_EQ_INT(a, 0); T_EQ_INT(z, 99);
    T_EQ_INT(dav_parse_range("bytes=100-", 100, &a, &z), -1);
    T_EQ_INT(dav_parse_range("bytes=-0", 100, &a, &z), -1);
    T_EQ_INT(dav_parse_range("bytes=0-", 0, &a, &z), -1);
    T_EQ_INT(dav_parse_range("bytes=0-1,5-6", 100, &a, &z), 0);   /* several ranges: whole file */
    T_EQ_INT(dav_parse_range("bytes=9-1", 100, &a, &z), 0);       /* malformed: ignored */
    T_EQ_INT(dav_parse_range("items=0-1", 100, &a, &z), 0);
    T_EQ_INT(dav_parse_range("bytes=x-1", 100, &a, &z), 0);
    T_EQ_INT(dav_parse_range("bytes=-", 100, &a, &z), 0);
    T_EQ_INT(dav_parse_range("bytes=99999999999999999999-", 100, &a, &z), 0);   /* overflow: ignored */
    T_EQ_INT(dav_parse_range("bytes= 1-2", 100, &a, &z), 1); T_EQ_INT(a, 1); T_EQ_INT(z, 2);
}

static void test_uri_path(void) {
    char b[256];
    T_CHECK(dav_uri_path("/dav", b, sizeof b)); T_EQ_STR(b, "");
    T_CHECK(dav_uri_path("/dav/", b, sizeof b)); T_EQ_STR(b, "/");
    T_CHECK(dav_uri_path("/dav/%EC%9E%A5%EB%AF%B8%201.jef?x=1", b, sizeof b)); T_EQ_STR(b, "/\xEC\x9E\xA5\xEB\xAF\xB8 1.jef");
    T_CHECK(dav_uri_path("http://192.168.0.5/dav/a/b.jef", b, sizeof b)); T_EQ_STR(b, "/a/b.jef");
    T_CHECK(dav_uri_path("https://silbapstick.local:80/dav/a%2Bb+c", b, sizeof b)); T_EQ_STR(b, "/a+b+c");
    T_CHECK(!dav_uri_path("/davx/a", b, sizeof b));
    T_CHECK(!dav_uri_path("/api/list", b, sizeof b));
    T_CHECK(!dav_uri_path("http://host", b, sizeof b));
    T_CHECK(!dav_uri_path("http://host/other/dav/a", b, sizeof b));
    T_CHECK(!dav_uri_path("/dav/a%2", b, sizeof b));
    T_CHECK(!dav_uri_path("/dav/a%00b", b, sizeof b));
    T_CHECK(!dav_uri_path(NULL, b, sizeof b));
}

static void test_rel(void) {
    char b[600];
    T_CHECK(dav_rel("", b, sizeof b)); T_EQ_STR(b, "");
    T_CHECK(dav_rel("/", b, sizeof b)); T_EQ_STR(b, "");
    T_CHECK(dav_rel("/a/b.jef", b, sizeof b)); T_EQ_STR(b, "a/b.jef");
    T_CHECK(dav_rel("/dir/", b, sizeof b)); T_EQ_STR(b, "dir");
    T_CHECK(dav_rel("/a:b/c?.jef", b, sizeof b));                  /* SD-safe names on every segment */
    T_EQ_STR(b, "a\xEF\xBC\x9A" "b/c\xEF\xBC\x9F.jef");
    T_CHECK(dav_rel("/name. ", b, sizeof b)); T_EQ_STR(b, "name");
    T_CHECK(!dav_rel("/.silbap", b, sizeof b));                      /* invisible and unreachable */
    T_CHECK(!dav_rel("/.SILBAP/catalog.bin", b, sizeof b));
    T_CHECK(!dav_rel("/.sIlBaP/catalog.bin", b, sizeof b));          /* every case variant */
    T_CHECK(!dav_rel("/EMB/.silbap", b, sizeof b));                  /* also as the last segment */
    T_CHECK(!dav_rel("/a/.Silbap-x", b, sizeof b));
    T_CHECK(dav_rel("/EMB/catalog.silbap", b, sizeof b));            /* only the leading ".silbap" is reserved */
    T_EQ_STR(b, "EMB/catalog.silbap");
    T_CHECK(!dav_rel("/a/../b", b, sizeof b));
    T_CHECK(!dav_rel("/a//b", b, sizeof b));
    T_CHECK(!dav_rel("/ . ", b, sizeof b));                         /* empty after cleanup */
    /* the same path repeatedly: the buffers are reused, no stale prefix survives */
    T_CHECK(dav_rel("/a/b", b, sizeof b)); T_EQ_STR(b, "a/b");
    T_CHECK(dav_rel("/c", b, sizeof b)); T_EQ_STR(b, "c");
    char tiny[8];
    T_CHECK(!dav_rel("/abcdefgh", tiny, sizeof tiny));               /* out too small: rejected, not truncated */
    T_CHECK(dav_rel("/abcdefg", tiny, sizeof tiny)); T_EQ_STR(tiny, "abcdefg");
}

/* 404 (hidden/unreachable) and 400 (bad input) must not be conflated. */
static void test_rel_status(void) {
    char b[600];
    T_EQ_INT(dav_rel_status("/a/b", b, sizeof b), DAV_PATH_OK); T_EQ_STR(b, "a/b");
    T_EQ_INT(dav_rel_status("", b, sizeof b), DAV_PATH_OK); T_EQ_STR(b, "");
    T_EQ_INT(dav_rel_status("/.silbap", b, sizeof b), DAV_PATH_ABSENT);
    T_EQ_INT(dav_rel_status("/.SILBAP/x", b, sizeof b), DAV_PATH_ABSENT);
    T_EQ_INT(dav_rel_status("/a/../b", b, sizeof b), DAV_PATH_ABSENT);
    T_EQ_INT(dav_rel_status("/a/.", b, sizeof b), DAV_PATH_ABSENT);
    T_EQ_INT(dav_rel_status("/a//b", b, sizeof b), DAV_PATH_BAD);        /* malformed, not hidden */
    T_EQ_INT(dav_rel_status("/ . ", b, sizeof b), DAV_PATH_BAD);         /* trims to nothing */
    T_EQ_INT(dav_rel_status("/a\x01" "b", b, sizeof b), DAV_PATH_BAD);   /* control char */
    char longname[258];
    memset(longname, 'a', 256); longname[256] = 0;
    char path[300];
    snprintf(path, sizeof path, "/%s", longname);
    T_EQ_INT(dav_rel_status(path, b, sizeof b), DAV_PATH_BAD);           /* 256 bytes > 255 */
    memset(longname, 'a', 255); longname[255] = 0;
    snprintf(path, sizeof path, "/%s", longname);
    T_EQ_INT(dav_rel_status(path, b, sizeof b), DAV_PATH_OK);            /* exactly 255 is fine */
    char tiny[8];
    T_EQ_INT(dav_rel_status("/abcdefgh", tiny, sizeof tiny), DAV_PATH_BAD);   /* out too small */
}

static void test_expect(void) {
    T_EQ_INT(dav_expect(NULL), DAV_EXPECT_NONE);
    T_EQ_INT(dav_expect(""), DAV_EXPECT_NONE);
    T_EQ_INT(dav_expect("100-continue"), DAV_EXPECT_CONTINUE);
    T_EQ_INT(dav_expect("100-Continue"), DAV_EXPECT_CONTINUE);
    T_EQ_INT(dav_expect("  100-CONTINUE  "), DAV_EXPECT_CONTINUE);
    T_EQ_INT(dav_expect("100-continue; foo=bar"), DAV_EXPECT_CONTINUE);
    T_EQ_INT(dav_expect("200-ok"), DAV_EXPECT_UNSUPPORTED);
    T_EQ_INT(dav_expect("100-continue, 200-ok"), DAV_EXPECT_UNSUPPORTED);
    T_EQ_INT(dav_expect("x100-continue"), DAV_EXPECT_UNSUPPORTED);   /* a different token */
}

/* A bad Destination is the client's fault (400); a URI outside /dav is 502. */
static void test_uri_path_status(void) {
    char b[256];
    T_EQ_INT(dav_uri_path_ex("/dav/a", b, sizeof b), DAV_URI_OK); T_EQ_STR(b, "/a");
    T_EQ_INT(dav_uri_path_ex("/dav", b, sizeof b), DAV_URI_OK); T_EQ_STR(b, "");
    T_EQ_INT(dav_uri_path_ex("/dav/a%2", b, sizeof b), DAV_URI_BAD);
    T_EQ_INT(dav_uri_path_ex("/dav/a%00b", b, sizeof b), DAV_URI_BAD);
    T_EQ_INT(dav_uri_path_ex("", b, sizeof b), DAV_URI_BAD);
    T_EQ_INT(dav_uri_path_ex("http://host/dav/a%ZZ", b, sizeof b), DAV_URI_BAD);
    T_EQ_INT(dav_uri_path_ex("http://host/other/a", b, sizeof b), DAV_URI_NOT_DAV);
    T_EQ_INT(dav_uri_path_ex("/api/list", b, sizeof b), DAV_URI_NOT_DAV);
    T_EQ_INT(dav_uri_path_ex("http://host", b, sizeof b), DAV_URI_NOT_DAV);
    T_EQ_INT(dav_uri_path_ex("not a uri", b, sizeof b), DAV_URI_BAD);
    T_EQ_INT(dav_uri_path_ex("/dav/a", b, 2), DAV_URI_BAD);              /* out too small */
}

static void test_proppatch_props(void) {
    char b[1024];
    const char *win =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
        "<D:propertyupdate xmlns:D=\"DAV:\" xmlns:Z=\"urn:schemas-microsoft-com:\"><D:set><D:prop>"
        "<Z:Win32CreationTime>Thu, 30 Oct 2026 01:02:03 GMT</Z:Win32CreationTime>"
        "<Z:Win32FileAttributes>00000020</Z:Win32FileAttributes>"
        "</D:prop></D:set></D:propertyupdate>";
    int n = dav_proppatch_props(win, b, sizeof b);
    T_CHECK(n > 0);
    T_EQ_STR(b, "<z:Win32CreationTime xmlns:z=\"urn:schemas-microsoft-com:\"/>"
                "<z:Win32FileAttributes xmlns:z=\"urn:schemas-microsoft-com:\"/>");
    const char *mac =
        "<?xml version=\"1.0\"?><propertyupdate xmlns=\"DAV:\"><remove><prop>"
        "<x:a xmlns:x=\"http://a&amp;b/\"/><getlastmodified/></prop></remove></propertyupdate>";
    n = dav_proppatch_props(mac, b, sizeof b);
    T_CHECK(n > 0);
    T_EQ_STR(b, "<z:a xmlns:z=\"http://a&amp;b/\"/><z:getlastmodified xmlns:z=\"DAV:\"/>");
    T_EQ_INT(dav_proppatch_props("", b, sizeof b), 0); T_EQ_STR(b, "");
    T_EQ_INT(dav_proppatch_props("<D:prop><D:a", b, sizeof b), 0);   /* truncated: nothing echoed */
    T_EQ_INT(dav_proppatch_props("<prop><a/></prop>", b, sizeof b), (long long)strlen("<a xmlns=\"\"/>"));
    T_EQ_STR(b, "<a xmlns=\"\"/>");
    char s[16];
    T_EQ_INT(dav_proppatch_props(win, s, sizeof s), -1);

    /* namespace URIs: '>', a bare '&' and quotes must not break the 207 body; a client's entity stays an entity */
    const char *bad =
        "<prop><x:a xmlns:x=\"http://h/?a=1&b=2&gt;\"/><y:b xmlns:y=\"x&quot;y\"/><z:c xmlns:z=\"&#38;'\"/></prop>";
    n = dav_proppatch_props(bad, b, sizeof b);
    T_CHECK(n > 0);
    T_EQ_STR(b, "<z:a xmlns:z=\"http://h/?a=1&amp;b=2&gt;\"/><z:b xmlns:z=\"x&quot;y\"/>"
                "<z:c xmlns:z=\"&#38;&apos;\"/>");
}

int main(void) {
    T_RUN(test_href_encode);
    T_RUN(test_xml_escape);
    T_RUN(test_dates);
    T_RUN(test_propfind_entry);
    T_RUN(test_range);
    T_RUN(test_uri_path);
    T_RUN(test_rel);
    T_RUN(test_rel_status);
    T_RUN(test_expect);
    T_RUN(test_uri_path_status);
    T_RUN(test_proppatch_props);
    T_DONE();
}
