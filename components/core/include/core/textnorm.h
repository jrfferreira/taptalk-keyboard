/* UTF-8 decoding and transcript tidying. Pure; no allocation. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Replacement codepoint yielded for malformed UTF-8. Callers treat it as
 * unmappable and drop it, so bad bytes vanish rather than corrupt output. */
#define UTF8_INVALID 0xFFFDu

/* Decode one codepoint from `s` (which holds `len` bytes).
 * Returns bytes consumed, or 0 when len == 0. On malformed input, consumes
 * one byte and yields UTF8_INVALID so callers always make progress. */
size_t utf8_next(const char *s, size_t len, uint32_t *codepoint);

/* Map an accented Latin codepoint to its unaccented ASCII base.
 * Returns 0 when there is no sensible base. */
uint32_t textnorm_deaccent(uint32_t codepoint);

/* Trim leading/trailing whitespace, collapse internal whitespace runs to a
 * single space, and drop C0 control characters.
 *
 * Newline and tab count as whitespace, so they are trimmed and collapsed like
 * any other. That is load-bearing: the keymap turns '\n' into Enter, and
 * OpenAI's response_format=text terminates every transcript with one. A
 * transcript of "hello\n" must type `hello`, not `hello` followed by Enter into
 * whatever form the cursor is in. A transcript of "\n" must come out empty.
 *
 * Copies at most `out_cap - 1` bytes and always NUL-terminates.
 * Returns the length written, excluding the NUL. */
size_t textnorm_clean(const char *in, size_t in_len, char *out, size_t out_cap);
