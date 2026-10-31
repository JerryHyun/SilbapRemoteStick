#include "t.h"
#include "namemap.h"
#include <string.h>
static const char *R(const char *s) { static char o[NM_LFN_MAX + 1]; nm_romanize(s, NM_LANG_KO, o); return o; }
static void test_hangul_rr(void) {
    T_EQ_STR(R("꽃무늬 자수.jef"), "KKOTMUNUI_JASU.JEF");
    T_EQ_STR(R("신라"), "SINRA");          /* no sound-change rules */
    T_EQ_STR(R("한글"), "HANGEUL");
    T_EQ_STR(R("닭"), "DAK");
    T_EQ_STR(R("값"), "GAP");
}
static void test_nfd_hangul(void) {
    /* ᄒ ᅡ ᆫ (U+1112 U+1161 U+11AB) == 한 */
    T_EQ_STR(R("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB.jef"), "HAN.JEF");
}
static void test_cleanup(void) {
    T_EQ_STR(R("  my  rose!!.PES "), "MY_ROSE.PES");
    T_EQ_STR(R("😀😀.jef"), "DESIGN.JEF");
    T_EQ_STR(R(""), "DESIGN");
    T_EQ_STR(R("noext"), "NOEXT");
    T_EQ_STR(R("a.b.c.dst"), "A_B_C.DST");
    T_EQ_STR(R("con.jef"), "CON_.JEF");
    T_EQ_STR(R("LPT9"), "LPT9_");
    T_EQ_STR(R("x.verylongextension"), "X.VERYLONG");      /* ext capped at 8 */
}
static void test_truncate(void) {
    char o[NM_LFN_MAX + 1];
    nm_romanize("abcdefghijklmnopqrstuvwxyz0123.jef", NM_LANG_EN, o);
    T_EQ_STR(o, "ABCDEFGHIJKLMNOPQRST.JEF");               /* 20 + 4 = 24 */
    T_EQ_INT((long long)strlen(o), 24);
}
static void test_suffix(void) {
    char o[NM_LFN_MAX + 1], tag[5];
    nm_tag4(0, tag); T_EQ_STR(tag, "AAAA");
    nm_tag4(0xFFFFFFFFu, tag); T_EQ_STR(tag, "7777");
    nm_with_suffix("ROSE.JEF", 0, o); T_EQ_STR(o, "ROSE_AAAA.JEF");
    nm_with_suffix("ABCDEFGHIJKLMNOPQRST.JEF", 0, o); T_EQ_STR(o, "ABCDEFGHIJKLMNO_AAAA.JEF");
    T_EQ_INT(nm_fnv1a32("", 0), 0x811C9DC5u);
    T_EQ_INT(nm_fnv1a32("a", 1), 0xE40C292Cu);
}
static void test_suffix_squeeze_and_bounds(void) {   /* N1 */
    char o[NM_LFN_MAX + 1];
    nm_with_suffix("ABCDEFGHIJKLMN_OPQRS.JEF", 0, o); T_EQ_STR(o, "ABCDEFGHIJKLMN_AAAA.JEF");   /* no "__" */
    nm_with_suffix("ABCDEFGHIJKLMN-OPQRS.JEF", 0, o); T_EQ_STR(o, "ABCDEFGHIJKLMN_AAAA.JEF");   /* no "-_" */
    nm_with_suffix("ABCDEFGHIJKLM_-OPQRS.JEF", 0, o); T_EQ_STR(o, "ABCDEFGHIJKLM_AAAA.JEF");
    /* defensive: longer than NM_LFN_MAX, extension longer than NM_EXT_MAX */
    nm_with_suffix("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGH.ABCDEFGHIJKLMNOP", 0, o);
    T_CHECK(strlen(o) <= NM_LFN_MAX);
    nm_with_suffix("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOP", 0, o);
    T_EQ_STR(o, "ABCDEFGHIJKLMNOPQRS_AAAA");
}
int main(void) { T_RUN(test_hangul_rr); T_RUN(test_nfd_hangul); T_RUN(test_cleanup); T_RUN(test_truncate); T_RUN(test_suffix); T_RUN(test_suffix_squeeze_and_bounds); T_DONE(); }
