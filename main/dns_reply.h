#pragma once
/* Captive-portal DNS answer for the setup AP: pure packet building, host-tested by test/host/test_dns.c.
 * Answers every A/IN question with the AP's own IPv4 address, so phones that probe for connectivity land on us.
 * Nothing here touches lwIP, sockets or ESP-IDF: it reads one query datagram and writes one reply datagram. */
#include <stddef.h>
#include <stdint.h>

/* Builds a DNS reply to one query packet. Returns the reply length, or 0 when no reply should be sent.
 *
 * ipv4_be is the address as the four octets that go into the packet, read as one big-endian integer. That is, the
 * RDATA written is out[n+12] = ipv4_be >> 24, .., out[n+15] = ipv4_be & 0xFF. For 192.168.4.1 pass 0xC0A80401u
 * (equivalently (192u << 24) | (168u << 16) | (4u << 8) | 1u) and the RDATA bytes are C0 A8 04 01. The value
 * 0x0104A8C0 is the little-endian in-memory image of those same bytes; do NOT pass that one.
 *
 * Reply shape: 12-byte header, then the question exactly as it appeared in the query (verbatim, same case, no
 * re-encoding), then -- only for an A/IN question -- one answer record:
 *   NAME = 0xC0 0x0C (compression pointer to the question name at offset 12), TYPE = A (1), CLASS = IN (1),
 *   TTL = 0 (a captive portal must never be cached), RDLENGTH = 4, RDATA = the four address bytes.
 * The header echoes the ID, sets QR, copies the opcode and the RD bit, and clears AA/TC/RA/Z/RCODE (NOERROR).
 * QDCOUNT = 1, ANCOUNT = 1 for A/IN and 0 otherwise, NSCOUNT = ARCOUNT = 0.
 *
 * Any question that is not A/IN -- AAAA (28), MX, CH-class, .. -- gets a header-only reply with ANCOUNT = 0 and
 * the question echoed (chosen over returning 0 so the client sees a clean empty answer instead of a timeout).
 *
 * Returns 0 (and writes nothing) when: query or out is NULL, the packet is shorter than the 12-byte header, QR is
 * already set (it is a response, not a query), QDCOUNT != 1, the question name is unparseable (a compression
 * pointer, a reserved label type, a label running past the packet, a name over 255 octets, or a missing
 * QTYPE/QCLASS), or out_cap is smaller than the whole reply. */
size_t dns_build_reply(const uint8_t *query, size_t q_len, uint8_t *out, size_t out_cap, uint32_t ipv4_be);
