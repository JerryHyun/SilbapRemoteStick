#include "t.h"
#include "dns_reply.h"

/* 192.168.4.1 as it appears in the packet: RDATA bytes C0 A8 04 01. */
#define AP_IPV4 0xC0A80401u

/* "connectivitycheck.gstatic.com" as wire labels, root label included. */
static const uint8_t QNAME[] = {
    17, 'c','o','n','n','e','c','t','i','v','i','t','y','c','h','e','c','k',
    7,  'g','s','t','a','t','i','c',
    3,  'c','o','m',
    0,
};
#define QNAME_LEN (sizeof QNAME)

/* Builds a query by hand: header + QNAME + QTYPE + QCLASS. Returns the length. */
static size_t make_query(uint8_t *b, uint16_t id, uint16_t flags, uint16_t qd,
                         uint16_t qtype, uint16_t qclass) {
    size_t n = 0;
    b[n++] = (uint8_t)(id >> 8); b[n++] = (uint8_t)id;
    b[n++] = (uint8_t)(flags >> 8); b[n++] = (uint8_t)flags;
    b[n++] = (uint8_t)(qd >> 8); b[n++] = (uint8_t)qd;
    for (int i = 0; i < 6; i++) b[n++] = 0;              /* ANCOUNT, NSCOUNT, ARCOUNT */
    memcpy(b + n, QNAME, QNAME_LEN); n += QNAME_LEN;
    b[n++] = (uint8_t)(qtype >> 8); b[n++] = (uint8_t)qtype;
    b[n++] = (uint8_t)(qclass >> 8); b[n++] = (uint8_t)qclass;
    return n;
}

/* Offsets inside the reply, for the standard A query built above. */
#define HDR_LEN   12u
#define QSEC_LEN  (QNAME_LEN + 4u)
#define ANS_OFF   (HDR_LEN + QSEC_LEN)

static uint16_t u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static void test_a_query(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);     /* RD set, QTYPE A, QCLASS IN */
    T_EQ_INT(qn, HDR_LEN + QSEC_LEN);

    size_t rn = dns_build_reply(q, qn, r, sizeof r, AP_IPV4);
    T_EQ_INT(rn, (long long)(HDR_LEN + QSEC_LEN + 16u));

    T_EQ_INT(u16(r), 0x1234);                               /* ID echoed */
    T_CHECK(u16(r + 2) & 0x8000);                           /* QR set */
    T_CHECK(u16(r + 2) & 0x0100);                           /* RD preserved */
    T_EQ_INT(u16(r + 2) & 0x000F, 0);                       /* RCODE NOERROR */
    T_EQ_INT(u16(r + 4), 1);                                /* QDCOUNT */
    T_EQ_INT(u16(r + 6), 1);                                /* ANCOUNT == 1 */
    T_EQ_INT(u16(r + 8), 0);                                /* NSCOUNT */
    T_EQ_INT(u16(r + 10), 0);                               /* ARCOUNT */

    /* The question is copied verbatim, byte for byte. */
    T_CHECK(!memcmp(r + HDR_LEN, q + HDR_LEN, QSEC_LEN));

    /* The answer: pointer to the question name, A/IN, zero TTL, 4 address bytes C0 A8 04 01. */
    T_EQ_INT(r[ANS_OFF], 0xC0); T_EQ_INT(r[ANS_OFF + 1], 0x0C);
    T_EQ_INT(u16(r + ANS_OFF + 2), 1);                      /* TYPE A */
    T_EQ_INT(u16(r + ANS_OFF + 4), 1);                      /* CLASS IN */
    T_EQ_INT(u16(r + ANS_OFF + 10), 4);                     /* RDLENGTH */
    T_EQ_INT(r[ANS_OFF + 12], 0xC0);
    T_EQ_INT(r[ANS_OFF + 13], 0xA8);
    T_EQ_INT(r[ANS_OFF + 14], 0x04);
    T_EQ_INT(r[ANS_OFF + 15], 0x01);
}

/* Any other ID must come back unchanged; this catches offset mistakes in the ID echo. */
static void test_id_echo(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0xABCD, 0, 1, 1, 1);
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), HDR_LEN + QSEC_LEN + 16u);
    T_EQ_INT(u16(r), 0xABCD);
    T_CHECK(!(u16(r + 2) & 0x0100));                        /* RD was clear in the query */
}

static void test_too_short(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);

    T_EQ_INT(dns_build_reply(q, 11, r, sizeof r, AP_IPV4), 0);          /* no complete header */
    T_EQ_INT(dns_build_reply(q, 0, r, sizeof r, AP_IPV4), 0);
    T_EQ_INT(dns_build_reply(q, HDR_LEN, r, sizeof r, AP_IPV4), 0);     /* header only: no question */
    T_EQ_INT(dns_build_reply(q, HDR_LEN + QNAME_LEN, r, sizeof r, AP_IPV4), 0);   /* no QTYPE/QCLASS */
    T_EQ_INT(dns_build_reply(q, qn - 1, r, sizeof r, AP_IPV4), 0);      /* last class byte cut */
    T_EQ_INT(dns_build_reply(q, qn - 4, r, sizeof r, AP_IPV4), 0);      /* both type and class cut */
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), (long long)(HDR_LEN + QSEC_LEN + 16u));
}

static void test_aaaa(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0x5678, 0x0100, 1, 28, 1);    /* QTYPE AAAA */

    size_t rn = dns_build_reply(q, qn, r, sizeof r, AP_IPV4);
    T_EQ_INT(rn, (long long)(HDR_LEN + QSEC_LEN));          /* header + question, no answer */
    T_CHECK(rn > 0);
    T_EQ_INT(u16(r), 0x5678);
    T_CHECK(u16(r + 2) & 0x8000);                           /* QR set */
    T_EQ_INT(u16(r + 4), 1);                                /* QDCOUNT */
    T_EQ_INT(u16(r + 6), 0);                                /* ANCOUNT == 0 */
    T_CHECK(!memcmp(r + HDR_LEN, q + HDR_LEN, QSEC_LEN));   /* question still echoed */
}

/* Non-IN classes and other types also get the header-only answer rather than a bogus A record. */
static void test_other_questions(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 1, 0, 1, 1, 3);               /* A, CH class */
    size_t rn = dns_build_reply(q, qn, r, sizeof r, AP_IPV4);
    T_EQ_INT(rn, (long long)(HDR_LEN + QSEC_LEN));
    T_EQ_INT(u16(r + 6), 0);

    qn = make_query(q, 1, 0, 1, 15, 1);                     /* MX, IN */
    rn = dns_build_reply(q, qn, r, sizeof r, AP_IPV4);
    T_EQ_INT(rn, (long long)(HDR_LEN + QSEC_LEN));
    T_EQ_INT(u16(r + 6), 0);
}

static void test_rejections(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);

    /* QR already set: this is a response, not a query. */
    size_t bad = make_query(q, 0x1234, 0x8100, 1, 1, 1);
    T_EQ_INT(dns_build_reply(q, bad, r, sizeof r, AP_IPV4), 0);

    /* QDCOUNT 0 and QDCOUNT 2. */
    qn = make_query(q, 0x1234, 0x0100, 0, 1, 1);
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), 0);
    qn = make_query(q, 0x1234, 0x0100, 2, 1, 1);
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), 0);

    /* A compressed question name is refused (we would have to copy a pointer we cannot re-point). */
    qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);
    q[HDR_LEN] = 0xC0; q[HDR_LEN + 1] = 0x0C;
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), 0);

    /* A label that claims more bytes than the packet holds. */
    qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);
    q[HDR_LEN] = 200;
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), 0);

    /* A name that never terminates inside the packet. */
    qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);
    qn = HDR_LEN + 1;                                       /* one length byte, no label, no root */
    q[HDR_LEN] = 63;
    T_EQ_INT(dns_build_reply(q, qn, r, sizeof r, AP_IPV4), 0);

    /* NULL arguments. */
    qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);
    T_EQ_INT(dns_build_reply(NULL, qn, r, sizeof r, AP_IPV4), 0);
    T_EQ_INT(dns_build_reply(q, qn, NULL, sizeof r, AP_IPV4), 0);
}

static void test_out_cap(void) {
    uint8_t q[512], r[512];
    size_t qn = make_query(q, 0x1234, 0x0100, 1, 1, 1);
    size_t need = HDR_LEN + QSEC_LEN + 16u;

    T_EQ_INT(dns_build_reply(q, qn, r, need - 1, AP_IPV4), 0);   /* one byte short */
    T_EQ_INT(dns_build_reply(q, qn, r, HDR_LEN + QSEC_LEN, AP_IPV4), 0);
    T_EQ_INT(dns_build_reply(q, qn, r, 0, AP_IPV4), 0);
    T_EQ_INT(dns_build_reply(q, qn, r, need, AP_IPV4), (long long)need);   /* exactly fits */

    /* The AAAA reply is shorter, so a buffer that fails an A query can still hold it. */
    qn = make_query(q, 0x1234, 0x0100, 1, 28, 1);
    T_EQ_INT(dns_build_reply(q, qn, r, HDR_LEN + QSEC_LEN, AP_IPV4), (long long)(HDR_LEN + QSEC_LEN));
}

int main(void) {
    T_RUN(test_a_query);
    T_RUN(test_id_echo);
    T_RUN(test_too_short);
    T_RUN(test_aaaa);
    T_RUN(test_other_questions);
    T_RUN(test_rejections);
    T_RUN(test_out_cap);
    T_DONE();
}
