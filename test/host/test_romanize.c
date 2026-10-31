#include "t.h"
#include "namemap.h"
static const char *R(const char *s, nm_lang_t l) { static char o[NM_LFN_MAX + 1]; nm_romanize(s, l, o); return o; }
static void test_han_by_language(void) {
    T_EQ_STR(R("金", NM_LANG_KO), "GEUM");     /* kHangul 금 */
    T_EQ_STR(R("金", NM_LANG_ZH), "JIN");
    T_EQ_STR(R("金", NM_LANG_EN), "JIN");
    T_EQ_STR(R("中国.dst", NM_LANG_ZH), "ZHONGGUO.DST");
    T_EQ_STR(R("山", NM_LANG_JA), "SAN");
}
static void test_kana(void) {
    T_EQ_STR(R("さくら", NM_LANG_JA), "SAKURA");
    T_EQ_STR(R("きゃっと", NM_LANG_JA), "KYATTO");
    T_EQ_STR(R("カード", NM_LANG_JA), "KADO");
    T_EQ_STR(R("ほん", NM_LANG_JA), "HON");
    T_EQ_STR(R("か\xE3\x82\x99", NM_LANG_JA), "GA");        /* か + U+3099 */
}
static void test_kana_small(void) {
    T_EQ_STR(R("じゅう", NM_LANG_JA), "JUU");
    T_EQ_STR(R("じゃ", NM_LANG_JA), "JA");
    T_EQ_STR(R("ジャズ", NM_LANG_JA), "JAZU");
    T_EQ_STR(R("にひゃく", NM_LANG_JA), "NIHYAKU");
    T_EQ_STR(R("ひょう", NM_LANG_JA), "HYOU");
    T_EQ_STR(R("しゃしん", NM_LANG_JA), "SHASHIN");
    T_EQ_STR(R("ちょっと", NM_LANG_JA), "CHOTTO");
    T_EQ_STR(R("っA", NM_LANG_JA), "A");
}
static void test_latin(void) {
    T_EQ_STR(R("\xE1\xBA\x9E", NM_LANG_EN), "SS");
    T_EQ_STR(R("Café.pes", NM_LANG_EN), "CAFE.PES");
    T_EQ_STR(R("Straße", NM_LANG_EN), "STRASSE");
    T_EQ_STR(R("Phở Đà Nẵng", NM_LANG_EN), "PHO_DA_NANG");
    T_EQ_STR(R("Æsir Øl", NM_LANG_EN), "AESIR_OL");
}
static void test_nfd_latin(void) { T_EQ_STR(R("Cafe\xCC\x81", NM_LANG_EN), "CAFE"); }
static void test_cyrillic_greek(void) {
    T_EQ_STR(R("Москва", NM_LANG_EN), "MOSKVA");
    T_EQ_STR(R("Щука", NM_LANG_EN), "SHCHUKA");
    T_EQ_STR(R("Αθήνα", NM_LANG_EN), "ATHINA");
}
static void test_greek_dialytika(void) {             /* N2: U+03AA, U+03AB */
    T_EQ_STR(R("\xCE\xAA\xCE\xAB", NM_LANG_EN), "IY");
    T_EQ_STR(R("\xCE\x9A\xCE\xAA\xCE\xA4\xCE\x91", NM_LANG_EN), "KITA");          /* ΚΪΤΑ */
}
static void test_halfwidth_kana(void) {              /* N3: U+FF66..U+FF9F like full-width katakana */
    T_EQ_STR(R("\xEF\xBD\xBB\xEF\xBD\xB8\xEF\xBE\x97", NM_LANG_JA), "SAKURA");     /* ｻｸﾗ */
    T_EQ_STR(R("\xEF\xBD\xB6\xEF\xBE\x9E", NM_LANG_JA), "GA");                         /* ｶﾞ */
    T_EQ_STR(R("\xEF\xBE\x8A\xEF\xBE\x9F\xEF\xBE\x9D", NM_LANG_JA), "PAN");         /* ﾊﾟﾝ */
    T_EQ_STR(R("\xEF\xBD\xB7\xEF\xBD\xAC\xEF\xBD\xAF\xEF\xBE\x84", NM_LANG_JA), "KYATTO");   /* ｷｬｯﾄ */
    T_EQ_STR(R("\xEF\xBD\xB6\xEF\xBD\xB0\xEF\xBE\x84\xEF\xBE\x9E", NM_LANG_JA), "KADO");     /* ｶｰﾄﾞ (ー dropped) */
    T_EQ_STR(R("\xEF\xBD\xA6\xEF\xBE\x9C", NM_LANG_EN), "WOWA");                       /* ｦﾜ */
    T_EQ_STR(R("\xEF\xBD\xBC\xEF\xBD\xAD.jef", NM_LANG_EN), "SHU.JEF");                /* ｼｭ */
}
/* I4: nm_romanize from four threads at once gives the same names as from one */
#include <pthread.h>
static const char *const RT_IN[] = { "きゃっと.jef", "꽃무늬 자수 디자인", "ｷｬｯﾄ", "Москва Αθήνα", "中国花園.dst", "じゅうしゃしん" };
static char rt_want[6][NM_LFN_MAX + 1];
static int rt_bad;
static void *rt_worker(void *arg) {
    int off = (int)(long)arg; char o[NM_LFN_MAX + 1];
    for (int i = 0; i < 20000; i++) {
        int k = (i + off) % 6;
        nm_romanize(RT_IN[k], NM_LANG_JA, o);
        if (strcmp(o, rt_want[k])) __atomic_add_fetch(&rt_bad, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
static void test_reentrant(void) {
    for (int k = 0; k < 6; k++) nm_romanize(RT_IN[k], NM_LANG_JA, rt_want[k]);
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, rt_worker, (void *)i);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    T_EQ_INT(rt_bad, 0);
}
static void test_fullwidth(void) { T_EQ_STR(R("ＡＢＣ　１２３.jef", NM_LANG_EN), "ABC_123.JEF"); }
int main(void) { T_RUN(test_han_by_language); T_RUN(test_kana); T_RUN(test_kana_small); T_RUN(test_latin); T_RUN(test_nfd_latin); T_RUN(test_cyrillic_greek); T_RUN(test_fullwidth); T_RUN(test_greek_dialytika); T_RUN(test_halfwidth_kana); T_RUN(test_reentrant); T_DONE(); }
