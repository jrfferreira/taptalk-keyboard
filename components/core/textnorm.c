#include "core/textnorm.h"

size_t utf8_next(const char *s, size_t len, uint32_t *codepoint)
{
    if (len == 0) {
        return 0;
    }

    const unsigned char *u = (const unsigned char *)s;
    const unsigned char b0 = u[0];

    size_t need;
    uint32_t cp;
    if (b0 < 0x80) {
        *codepoint = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        cp   = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        cp   = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        cp   = b0 & 0x07u;
    } else {
        *codepoint = UTF8_INVALID; /* stray continuation or 5+ byte lead */
        return 1;
    }

    if (len < need + 1) {
        *codepoint = UTF8_INVALID; /* truncated at end of input */
        return 1;
    }
    for (size_t i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *codepoint = UTF8_INVALID;
            return 1;
        }
        cp = (cp << 6) | (u[i] & 0x3Fu);
    }

    /* Reject overlong encodings, surrogates, and out-of-range values, which
     * would otherwise let the same character have several byte spellings. */
    static const uint32_t min_for_len[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < min_for_len[need] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *codepoint = UTF8_INVALID;
        return 1;
    }

    *codepoint = cp;
    return need + 1;
}

uint32_t textnorm_deaccent(uint32_t cp)
{
    switch (cp) {
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: return 'A';
    case 0x00C7: return 'C';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return 'E';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return 'I';
    case 0x00D1: return 'N';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: return 'O';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return 'U';
    case 0x00DD: return 'Y';

    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
    case 0x00E7: return 'c';
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return 'e';
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return 'i';
    case 0x00F1: return 'n';
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: return 'o';
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return 'u';
    case 0x00FD: case 0x00FF: return 'y';

    /* Punctuation a transcription service is apt to emit. */
    case 0x2018: case 0x2019: return '\''; /* curly single quotes */
    case 0x201C: case 0x201D: return '"';  /* curly double quotes */
    case 0x2013: case 0x2014: return '-';  /* en/em dash */
    case 0x00A0: return ' ';               /* non-breaking space */
    default: return 0;
    }
}

static int is_ws(uint32_t cp)
{
    return cp == ' ' || cp == '\r' || cp == 0x00A0;
}

/* C0 controls we drop outright. '\n' and '\t' survive; the keymap turns them
 * into Enter and Tab. */
static int is_droppable_control(uint32_t cp)
{
    return cp < 0x20 && cp != '\n' && cp != '\t';
}

size_t textnorm_clean(const char *in, size_t in_len, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return 0;
    }
    if (in == NULL) {
        out[0] = '\0';
        return 0;
    }

    size_t r = 0, w = 0;
    int pending_space = 0;   /* a whitespace run seen but not yet emitted */
    int have_visible  = 0;   /* suppresses leading whitespace */

    while (r < in_len && in[r] != '\0') {
        uint32_t cp;
        const size_t used = utf8_next(in + r, in_len - r, &cp);
        if (used == 0) {
            break;
        }

        if (is_ws(cp)) {
            pending_space = 1;
            r += used;
            continue;
        }
        if (is_droppable_control(cp) || cp == UTF8_INVALID) {
            r += used;
            continue;
        }

        /* Emitting a newline or tab makes any pending space redundant. */
        if (pending_space && have_visible && cp != '\n' && cp != '\t') {
            if (w + 1 >= out_cap) {
                break;
            }
            out[w++] = ' ';
        }
        pending_space = 0;

        if (w + used >= out_cap) {
            break; /* no room for the whole codepoint; stop cleanly */
        }
        for (size_t i = 0; i < used; i++) {
            out[w++] = in[r + i];
        }
        have_visible = 1;
        r += used;
    }

    out[w] = '\0'; /* trailing whitespace never emitted, so no trim needed */
    return w;
}
