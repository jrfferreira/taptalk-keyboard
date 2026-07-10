/* Escape a string for embedding inside a JSON string literal.
 *
 * The strings this handles are Wi-Fi SSIDs, which are 32 arbitrary bytes
 * broadcast by anything within radio range. They reach a browser through our
 * /scan endpoint. A neighbour naming their network `","x":"` must not be able
 * to reshape our JSON. */
#pragma once

#include <stddef.h>

/* Writes the escaped form of `in` (in_len bytes) into `out`, NUL-terminated.
 * Does NOT write the surrounding quotes.
 *
 * Escapes `"` and `\`, and every C0 control byte as \u00XX. Bytes >= 0x20 pass
 * through untouched, so valid UTF-8 survives.
 *
 * Returns the length written, or -1 if it would not fit (out is then ""). */
int json_escape(const char *in, size_t in_len, char *out, size_t out_cap);
