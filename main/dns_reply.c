#include "dns_reply.h"

/* DNS wire layout we care about (RFC 1035 4.1): a fixed 12-byte header, then the question section, which is a
 * label sequence ending in a zero-length root label, followed by QTYPE and QCLASS (2 bytes each). */
#define DNS_HDR_LEN   12u
#define DNS_MAX_NAME  255u   /* length octets included (RFC 1035 2.3.4) */
#define DNS_QTYPE_A   1u
#define DNS_QCLASS_IN 1u

#define DNS_FLAG_QR   0x8000u
#define DNS_KEEP_MASK 0x7900u   /* opcode (0x7800) and RD (0x0100); AA/TC/RA/Z/RCODE are ours to set */

#define DNS_ANS_LEN   16u   /* 2 name + 2 type + 2 class + 4 ttl + 2 rdlength + 4 rdata */
#define DNS_TTL       0u    /* never cache a captive-portal lie */

/* Walks the question name at query[12..q_len) and returns the offset just past QCLASS, or 0 when the question is
 * missing or malformed: no room for the name, a compression pointer (0xC0) or reserved label type (0x40/0x80), a
 * label that runs off the packet, a name over 255 octets, or no complete QTYPE/QCLASS behind it. */
static size_t question_end(const uint8_t *q, size_t q_len) {
    size_t pos = DNS_HDR_LEN;
    size_t name_len = 0;
    for (;;) {
        if (pos >= q_len) return 0;
        uint8_t len = q[pos++];
        name_len++;
        if (len == 0) break;
        if (len & 0xC0u) return 0;                 /* pointer or reserved label type */
        if (name_len + (size_t)len > DNS_MAX_NAME) return 0;
        if (pos + (size_t)len > q_len) return 0;
        pos += len;
        name_len += len;
    }
    if (name_len > DNS_MAX_NAME) return 0;
    if (q_len - pos < 4) return 0;                 /* QTYPE + QCLASS */
    return pos + 4;
}

size_t dns_build_reply(const uint8_t *query, size_t q_len, uint8_t *out, size_t out_cap, uint32_t ipv4_be) {
    if (!query || !out) return 0;
    if (q_len < DNS_HDR_LEN) return 0;

    uint16_t qflags = (uint16_t)(((uint16_t)query[2] << 8) | query[3]);
    if (qflags & DNS_FLAG_QR) return 0;                                  /* already a response */
    uint16_t qdcount = (uint16_t)(((uint16_t)query[4] << 8) | query[5]);
    if (qdcount != 1) return 0;

    size_t q_end = question_end(query, q_len);
    if (q_end == 0) return 0;

    size_t qsec = q_end - DNS_HDR_LEN;
    uint16_t qtype = (uint16_t)(((uint16_t)query[q_end - 4] << 8) | query[q_end - 3]);
    uint16_t qclass = (uint16_t)(((uint16_t)query[q_end - 2] << 8) | query[q_end - 1]);
    int answer = (qtype == DNS_QTYPE_A && qclass == DNS_QCLASS_IN);

    size_t need = DNS_HDR_LEN + qsec + (answer ? DNS_ANS_LEN : 0u);
    if (out_cap < need) return 0;

    /* Header: echo the ID, preserve opcode and RD, set QR, answer with NOERROR. */
    out[0] = query[0];
    out[1] = query[1];
    uint16_t rflags = (uint16_t)(DNS_FLAG_QR | (qflags & DNS_KEEP_MASK));
    out[2] = (uint8_t)(rflags >> 8);
    out[3] = (uint8_t)rflags;
    out[4] = 0; out[5] = 1;                                  /* QDCOUNT */
    out[6] = 0; out[7] = (uint8_t)(answer ? 1 : 0);          /* ANCOUNT */
    out[8] = 0; out[9] = 0;                                  /* NSCOUNT */
    out[10] = 0; out[11] = 0;                                /* ARCOUNT */

    /* Question section copied verbatim: the name is not re-encoded. */
    if (qsec) {
        for (size_t i = 0; i < qsec; i++) out[DNS_HDR_LEN + i] = query[DNS_HDR_LEN + i];
    }

    if (answer) {
        uint8_t *a = out + DNS_HDR_LEN + qsec;
        a[0] = 0xC0; a[1] = 0x0C;                            /* name: pointer to the question at offset 12 */
        a[2] = 0; a[3] = (uint8_t)DNS_QTYPE_A;
        a[4] = 0; a[5] = (uint8_t)DNS_QCLASS_IN;
        a[6] = (uint8_t)(DNS_TTL >> 24);                     /* TTL = 0: a captive portal must not be cached */
        a[7] = (uint8_t)(DNS_TTL >> 16);
        a[8] = (uint8_t)(DNS_TTL >> 8);
        a[9] = (uint8_t)DNS_TTL;
        a[10] = 0; a[11] = 4;                                /* RDLENGTH */
        a[12] = (uint8_t)(ipv4_be >> 24);
        a[13] = (uint8_t)(ipv4_be >> 16);
        a[14] = (uint8_t)(ipv4_be >> 8);
        a[15] = (uint8_t)ipv4_be;
    }

    return need;
}
