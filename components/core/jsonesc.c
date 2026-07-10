#include "core/jsonesc.h"

static const char HEX[] = "0123456789abcdef";

int json_escape(const char *in, size_t in_len, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return -1;
    }
    out[0] = '\0';
    if (in == NULL) {
        return 0;
    }

    size_t w = 0;

/* Every write goes through this, so a single bounds check covers them all. */
#define PUT(c)                                                                                     \
    do {                                                                                           \
        if (w + 1 >= out_cap) {                                                                    \
            out[0] = '\0';                                                                         \
            return -1;                                                                             \
        }                                                                                          \
        out[w++] = (c);                                                                            \
    } while (0)

    for (size_t r = 0; r < in_len; r++) {
        const unsigned char c = (unsigned char)in[r];
        switch (c) {
        case '"':  PUT('\\'); PUT('"');  break;
        case '\\': PUT('\\'); PUT('\\'); break;
        case '\b': PUT('\\'); PUT('b');  break;
        case '\f': PUT('\\'); PUT('f');  break;
        case '\n': PUT('\\'); PUT('n');  break;
        case '\r': PUT('\\'); PUT('r');  break;
        case '\t': PUT('\\'); PUT('t');  break;
        default:
            if (c < 0x20) {
                /* JSON forbids raw control characters inside a string. */
                PUT('\\'); PUT('u'); PUT('0'); PUT('0');
                PUT(HEX[(c >> 4) & 0xF]);
                PUT(HEX[c & 0xF]);
            } else {
                PUT((char)c);
            }
            break;
        }
    }

#undef PUT

    out[w] = '\0';
    return (int)w;
}
