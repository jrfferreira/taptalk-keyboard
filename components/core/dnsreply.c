#include "core/dnsreply.h"

#define ANSWER_LEN 16 /* name ptr 2 + type 2 + class 2 + ttl 4 + rdlen 2 + A 4 */

size_t dns_build_reply(uint8_t *pkt, size_t len, size_t cap, const uint8_t ip[4])
{
    if (pkt == NULL || ip == NULL || len < DNS_MIN_PACKET || cap < len) {
        return 0;
    }

    const int is_response = (pkt[2] & 0x80) != 0;
    const unsigned qdcount = ((unsigned)pkt[4] << 8) | pkt[5];
    if (is_response || qdcount < 1) {
        return 0;
    }

    /* Walk the question's length-prefixed labels to the root label. A label
     * length above 63 is either a compression pointer (0xC0) or garbage;
     * neither is legal in a question we are willing to answer. */
    size_t p = DNS_MIN_PACKET;
    while (p < len && pkt[p] != 0) {
        if (pkt[p] > 63) {
            return 0;
        }
        p += (size_t)pkt[p] + 1;
    }
    /* The loop also exits by running off the end, meaning there was no root
     * label and the question is truncated. */
    if (p >= len || pkt[p] != 0) {
        return 0;
    }

    p += 5; /* the root label's zero byte, then QTYPE and QCLASS */
    if (p > len || p + ANSWER_LEN > cap) {
        return 0;
    }

    pkt[2] = 0x81; /* QR=1, opcode 0, RD preserved as 1 */
    pkt[3] = 0x80; /* RA=1, rcode 0 */
    pkt[6] = 0x00; /* ANCOUNT = 1 */
    pkt[7] = 0x01;
    pkt[8] = 0x00; /* NSCOUNT = 0 */
    pkt[9] = 0x00;
    pkt[10] = 0x00; /* ARCOUNT = 0 */
    pkt[11] = 0x00;

    uint8_t *a = pkt + p;
    *a++ = 0xC0; *a++ = 0x0C; /* name: compression pointer back to the question */
    *a++ = 0x00; *a++ = 0x01; /* TYPE  A */
    *a++ = 0x00; *a++ = 0x01; /* CLASS IN */
    *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C; /* TTL 60 s */
    *a++ = 0x00; *a++ = 0x04;                           /* RDLENGTH */
    *a++ = ip[0]; *a++ = ip[1]; *a++ = ip[2]; *a++ = ip[3];

    return p + ANSWER_LEN;
}
