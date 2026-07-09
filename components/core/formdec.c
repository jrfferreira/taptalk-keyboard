#include "core/formdec.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode [src, src+len) into out. Returns length, or FORMDEC_BAD. */
static int decode(const char *src, size_t len, char *out, size_t out_cap)
{
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        char c = src[r];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            /* A truncated escape at the end of the body must not read past it. */
            if (r + 2 >= len) {
                return FORMDEC_BAD;
            }
            const int hi = hexval(src[r + 1]);
            const int lo = hexval(src[r + 2]);
            if (hi < 0 || lo < 0) {
                return FORMDEC_BAD;
            }
            c = (char)((hi << 4) | lo);
            r += 2;
        }
        if (w + 1 >= out_cap) {
            return FORMDEC_BAD; /* refuse to truncate a credential silently */
        }
        out[w++] = c;
    }
    out[w] = '\0';
    return (int)w;
}

int form_get(const char *body, size_t body_len, const char *name, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return FORMDEC_BAD;
    }
    out[0] = '\0';
    if (body == NULL || name == NULL) {
        return FORMDEC_NOT_FOUND;
    }

    const size_t name_len = strlen(name);
    size_t i = 0;

    while (i < body_len) {
        /* One `key=value` pair, bounded by '&' or the end of the body. */
        size_t pair_end = i;
        while (pair_end < body_len && body[pair_end] != '&') {
            pair_end++;
        }

        size_t eq = i;
        while (eq < pair_end && body[eq] != '=') {
            eq++;
        }

        /* Whole-key match: "pass" must not match "password". A pair with no
         * '=' has eq == pair_end and cannot match a non-empty name. */
        if (eq < pair_end && (eq - i) == name_len && memcmp(body + i, name, name_len) == 0) {
            const int n = decode(body + eq + 1, pair_end - eq - 1, out, out_cap);
            if (n < 0) {
                out[0] = '\0';
            }
            return n;
        }

        i = pair_end + 1; /* skip the '&'; harmlessly overshoots at the end */
    }

    return FORMDEC_NOT_FOUND;
}
