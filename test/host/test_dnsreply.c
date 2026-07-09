#include "core/dnsreply.h"
#include "test_util.h"

static const uint8_t IP[4] = {192, 168, 4, 1};

/* A well-formed query for "a.bc" (labels 1:'a', 2:'b','c', root). */
static size_t make_query(uint8_t *b, size_t cap)
{
    static const uint8_t q[] = {
        0xAB, 0xCD,             /* id */
        0x01, 0x00,             /* flags: standard query, RD */
        0x00, 0x01,             /* qdcount 1 */
        0x00, 0x00, 0x00, 0x00, /* an, ns */
        0x00, 0x00,             /* ar */
        1, 'a', 2, 'b', 'c', 0, /* qname */
        0x00, 0x01,             /* qtype A */
        0x00, 0x01,             /* qclass IN */
    };
    CHECK(cap >= sizeof(q));
    memcpy(b, q, sizeof(q));
    return sizeof(q);
}

TEST_MAIN("dnsreply", {
    uint8_t b[256];

    /* --- the happy path --- */
    size_t n = make_query(b, sizeof(b));
    size_t r = dns_build_reply(b, n, sizeof(b), IP);
    CHECK_EQ_INT(r, n + 16);

    CHECK_EQ_INT(b[0], 0xAB);      /* transaction id preserved */
    CHECK_EQ_INT(b[1], 0xCD);
    CHECK_EQ_INT(b[2] & 0x80, 0x80); /* QR = response */
    CHECK_EQ_INT(b[5], 1);           /* qdcount untouched */
    CHECK_EQ_INT(b[7], 1);           /* ancount = 1 */
    CHECK_EQ_INT(b[9], 0);           /* nscount */
    CHECK_EQ_INT(b[11], 0);          /* arcount */

    const uint8_t *ans = b + n;
    CHECK_EQ_INT(ans[0], 0xC0);  /* compression pointer... */
    CHECK_EQ_INT(ans[1], 0x0C);  /* ...to offset 12, the question */
    CHECK_EQ_INT(ans[2], 0x00); CHECK_EQ_INT(ans[3], 0x01); /* TYPE A */
    CHECK_EQ_INT(ans[4], 0x00); CHECK_EQ_INT(ans[5], 0x01); /* CLASS IN */
    CHECK_EQ_INT(ans[9], 60);                               /* TTL */
    CHECK_EQ_INT(ans[10], 0x00); CHECK_EQ_INT(ans[11], 0x04); /* RDLENGTH */
    CHECK_EQ_INT(ans[12], 192); CHECK_EQ_INT(ans[13], 168);
    CHECK_EQ_INT(ans[14], 4);   CHECK_EQ_INT(ans[15], 1);

    /* The question section is left byte-for-byte intact; the pointer at
     * offset 12 depends on it. */
    CHECK_EQ_INT(b[12], 1); CHECK_EQ_INT(b[13], 'a');
    CHECK_EQ_INT(b[14], 2); CHECK_EQ_INT(b[15], 'b'); CHECK_EQ_INT(b[16], 'c');
    CHECK_EQ_INT(b[17], 0);

    /* --- packets we must drop, not answer --- */

    /* Shorter than a header. */
    n = make_query(b, sizeof(b));
    CHECK_EQ_INT(dns_build_reply(b, 11, sizeof(b), IP), 0);
    CHECK_EQ_INT(dns_build_reply(b, 0, sizeof(b), IP), 0);

    /* Already a response: answering it would make two servers shout at once. */
    n = make_query(b, sizeof(b));
    b[2] |= 0x80;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);

    /* No question section. */
    n = make_query(b, sizeof(b));
    b[4] = 0; b[5] = 0;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);

    /* No root label: the name runs off the end of the packet. */
    n = make_query(b, sizeof(b));
    b[17] = 3; /* was the terminating 0; now claims another label */
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);

    /* A label length that jumps past the end of the buffer. */
    n = make_query(b, sizeof(b));
    b[12] = 60;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);

    /* A compression pointer (0xC0) where a label length belongs. Left
     * unhandled, this is where a pointer loop or an over-read would start. */
    n = make_query(b, sizeof(b));
    b[12] = 0xC0;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);
    /* Any length above the legal 63 is rejected, not just 0xC0. */
    n = make_query(b, sizeof(b));
    b[12] = 64;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);
    /* A pointer to itself would spin forever if lengths were followed. */
    n = make_query(b, sizeof(b));
    b[12] = 0xC0; b[13] = 0x0C;
    CHECK_EQ_INT(dns_build_reply(b, n, sizeof(b), IP), 0);

    /* Question truncated exactly at the root label, so QTYPE/QCLASS are
     * missing: p would run past len. */
    n = make_query(b, sizeof(b));
    CHECK_EQ_INT(dns_build_reply(b, 18, sizeof(b), IP), 0); /* ends at the 0 byte */
    CHECK_EQ_INT(dns_build_reply(b, 21, sizeof(b), IP), 0); /* one byte short */

    /* Exactly enough for the question is fine, if the buffer has answer room. */
    n = make_query(b, sizeof(b));
    CHECK_EQ_INT(dns_build_reply(b, 22, sizeof(b), IP), 38);

    /* No room in the buffer for the 16-byte answer. */
    n = make_query(b, sizeof(b));
    CHECK_EQ_INT(dns_build_reply(b, n, n, IP), 0);
    CHECK_EQ_INT(dns_build_reply(b, n, n + 15, IP), 0);
    CHECK_EQ_INT(dns_build_reply(b, n, n + 16, IP), n + 16); /* exact fit */

    /* cap smaller than len is nonsense. */
    n = make_query(b, sizeof(b));
    CHECK_EQ_INT(dns_build_reply(b, n, n - 1, IP), 0);

    /* Null arguments. */
    CHECK_EQ_INT(dns_build_reply(NULL, 22, 64, IP), 0);
    CHECK_EQ_INT(dns_build_reply(b, 22, 64, NULL), 0);

    /* --- a root query ("." — one zero byte, no labels) is well-formed --- */
    uint8_t root[] = {0, 1, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x00, 0x01};
    CHECK_EQ_INT(dns_build_reply(root, sizeof(root), sizeof(root), IP), 0); /* no answer room */
    uint8_t root2[64];
    memcpy(root2, root, sizeof(root));
    CHECK_EQ_INT(dns_build_reply(root2, sizeof(root), sizeof(root2), IP), sizeof(root) + 16);

    /* --- fuzz: no input may write outside the buffer or return a bad length ---
     *
     * A reply may be SHORTER than the query: the answer is appended right
     * after the question, so any trailing bytes (a stray additional section,
     * padding) are dropped. That is consistent with the ARCOUNT=0 we write. */
    for (unsigned seed = 0; seed < 4096; seed++) {
        uint8_t f[64];
        const uint8_t canary = 0xA5;
        memset(f, canary, sizeof(f));
        size_t flen = 12 + (seed % 30);
        for (size_t i = 0; i < flen; i++) {
            f[i] = (uint8_t)((seed * 31u + i * 17u) & 0xFF);
        }
        f[2] &= 0x7F;       /* make it a query so we exercise the parser */
        f[4] = 0; f[5] = 1; /* qdcount 1 */

        const size_t got = dns_build_reply(f, flen, sizeof(f), IP);

        /* Never longer than the buffer, and never more than one answer past
         * the query it was built from. */
        CHECK(got == 0 || got <= sizeof(f));
        CHECK(got == 0 || got <= flen + 16);
        if (got == 0) {
            continue;
        }
        /* Past both the reply and the region the fuzzer filled, the canary
         * must survive: the function wrote nothing out of bounds. */
        const size_t high = got > flen ? got : flen;
        for (size_t i = high; i < sizeof(f); i++) {
            CHECK_EQ_INT(f[i], canary);
        }
    }
})
