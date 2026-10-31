#include "t.h"
#include "web_decide.h"
#include "webcore.h"
static const uint8_t salt[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static uint8_t hash[32], other[32];
static const char *const ok[] = {"192.168.0.42", "silbapstick.local", NULL};

static void test_mutation_needs_same_origin(void) {
    T_EQ_INT(web_decide("POST","192.168.0.42","http://evil.example",NULL,false,NULL,NULL,WEB_AUTH,ok), 403);
    T_EQ_INT(web_decide("POST","evil.example",NULL,NULL,false,NULL,NULL,WEB_AUTH,ok), 403);   /* DNS rebinding */
    T_EQ_INT(web_decide("POST","192.168.0.42",NULL,NULL,false,NULL,NULL,WEB_AUTH,ok), 200);
    T_EQ_INT(web_decide("POST","192.168.0.42","http://silbapstick.local",NULL,false,NULL,NULL,WEB_AUTH,ok), 200);
    T_EQ_INT(web_decide("DELETE","192.168.0.42","https://192.168.0.42",NULL,false,NULL,NULL,WEB_AUTH,ok), 403);
    T_EQ_INT(web_decide("PROPFIND","evil.example",NULL,NULL,false,NULL,NULL,WEB_AUTH,ok), 403);   /* Host always checked */
    T_EQ_INT(web_decide("PROPFIND","192.168.0.42","http://evil.example",NULL,false,NULL,NULL,WEB_AUTH,ok), 200);   /* no Origin check */
    T_EQ_INT(web_decide("POST",NULL,NULL,NULL,false,NULL,NULL,WEB_AUTH,ok), 403);
}
static void test_auth(void) {
    wc_pw_hash("pw", salt, hash);
    wc_pw_hash("other", salt, other);
    T_EQ_INT(web_decide("GET","192.168.0.42","http://evil.example",NULL,true,salt,hash,WEB_PUBLIC,ok), 200);
    T_EQ_INT(web_decide("GET","192.168.0.42",NULL,NULL,true,salt,hash,WEB_AUTH,ok), 401);
    T_EQ_INT(web_decide("GET","192.168.0.42",NULL,"Basic OnB3",true,salt,hash,WEB_AUTH,ok), 200);   /* ":pw" */
    T_EQ_INT(web_decide("GET","192.168.0.42",NULL,"Basic OnB3",true,salt,other,WEB_AUTH,ok), 401);
    T_EQ_INT(web_decide("GET","192.168.0.42",NULL,"Bearer x",true,salt,hash,WEB_AUTH,ok), 401);
    T_EQ_INT(web_decide("GET","192.168.0.42",NULL,NULL,false,NULL,NULL,WEB_AUTH,ok), 200);
    T_EQ_INT(web_decide("POST","192.168.0.42","http://evil.example","Basic OnB3",true,salt,hash,WEB_AUTH,ok), 403);
    T_EQ_INT(web_decide("GET","evil.example",NULL,NULL,true,salt,hash,WEB_PUBLIC,ok), 403);
}
static void b64(const char *in, size_t n, char *out) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (i + 1 < n ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < n ? (unsigned char)in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}
static void test_password_length_boundary(void) {
    char raw[80], hdr[160], pw[80];
    for (int len = 64; len <= 65; len++) {
        memset(pw, 'p', (size_t)len); pw[len] = 0;
        raw[0] = ':'; memcpy(raw + 1, pw, (size_t)len);
        strcpy(hdr, "Basic "); b64(raw, (size_t)len + 1, hdr + 6);
        uint8_t h[32]; wc_pw_hash(pw, salt, h);
        /* hash of the same bytes: a 64-byte password is accepted, 65 is rejected by the 64-byte cap */
        T_EQ_INT(web_decide("GET","192.168.0.42",NULL,hdr,true,salt,h,WEB_AUTH,ok), len == 64 ? 200 : 401);
    }
}
static void test_empty_origin(void) {
    T_EQ_INT(web_decide("POST","192.168.0.42","",NULL,false,NULL,NULL,WEB_AUTH,ok), 403);
    T_EQ_INT(web_decide("GET","192.168.0.42","",NULL,false,NULL,NULL,WEB_AUTH,ok), 200);
}
int main(void) { T_RUN(test_mutation_needs_same_origin); T_RUN(test_auth); T_RUN(test_password_length_boundary); T_RUN(test_empty_origin); T_DONE(); }
