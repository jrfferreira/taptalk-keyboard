#include "core/textnorm.h"
#include "test_util.h"

static void check_clean(const char *in, const char *want)
{
    char out[128];
    textnorm_clean(in, strlen(in), out, sizeof(out));
    CHECK_EQ_STR(out, want);
}

TEST_MAIN("textnorm", {
    uint32_t cp;

    /* --- utf8_next --- */
    CHECK_EQ_INT(utf8_next("a", 1, &cp), 1);      CHECK_EQ_INT(cp, 'a');
    CHECK_EQ_INT(utf8_next("\xC3\xA1", 2, &cp), 2); CHECK_EQ_INT(cp, 0xE1); /* á */
    CHECK_EQ_INT(utf8_next("\xE2\x80\x99", 3, &cp), 3); CHECK_EQ_INT(cp, 0x2019); /* ’ */
    CHECK_EQ_INT(utf8_next("\xF0\x9F\x98\x80", 4, &cp), 4); CHECK_EQ_INT(cp, 0x1F600); /* 😀 */
    CHECK_EQ_INT(utf8_next("", 0, &cp), 0);

    /* Malformed input must consume a byte and signal invalid, never stall. */
    CHECK_EQ_INT(utf8_next("\x80", 1, &cp), 1);     CHECK_EQ_INT(cp, UTF8_INVALID);
    CHECK_EQ_INT(utf8_next("\xC3", 1, &cp), 1);     CHECK_EQ_INT(cp, UTF8_INVALID); /* truncated */
    CHECK_EQ_INT(utf8_next("\xC3\x28", 2, &cp), 1); CHECK_EQ_INT(cp, UTF8_INVALID); /* bad cont. */
    CHECK_EQ_INT(utf8_next("\xC0\xAF", 2, &cp), 1); CHECK_EQ_INT(cp, UTF8_INVALID); /* overlong '/' */
    CHECK_EQ_INT(utf8_next("\xED\xA0\x80", 3, &cp), 1); CHECK_EQ_INT(cp, UTF8_INVALID); /* surrogate */

    /* --- deaccent --- */
    CHECK_EQ_INT(textnorm_deaccent(0x00E1), 'a'); /* á */
    CHECK_EQ_INT(textnorm_deaccent(0x00E3), 'a'); /* ã */
    CHECK_EQ_INT(textnorm_deaccent(0x00E7), 'c'); /* ç */
    CHECK_EQ_INT(textnorm_deaccent(0x00C7), 'C'); /* Ç */
    CHECK_EQ_INT(textnorm_deaccent(0x00F5), 'o'); /* õ */
    CHECK_EQ_INT(textnorm_deaccent(0x2019), '\''); /* ’ */
    CHECK_EQ_INT(textnorm_deaccent(0x201C), '"');  /* “ */
    CHECK_EQ_INT(textnorm_deaccent(0x2014), '-');  /* em dash */
    CHECK_EQ_INT(textnorm_deaccent(0x00A0), ' ');  /* nbsp */
    CHECK_EQ_INT(textnorm_deaccent('a'), 0);       /* already ASCII */
    CHECK_EQ_INT(textnorm_deaccent(0x1F600), 0);   /* emoji: no base */

    /* --- clean --- */
    check_clean("hello world", "hello world");
    check_clean("  leading and trailing  ", "leading and trailing");
    check_clean("collapse    the   runs", "collapse the runs");
    check_clean("\r\ncarriage\r\n", "\ncarriage\n"); /* \r dropped, \n survives */
    check_clean("tab\tsep", "tab\tsep");
    check_clean("bell\x07gone", "bellgone");         /* C0 control dropped */
    check_clean("", "");
    check_clean("   ", "");
    check_clean("\xC3\xA7\xC3\xA3o", "\xC3\xA7\xC3\xA3o"); /* ção passes through intact */

    /* A space pending before a newline is dropped rather than emitted. */
    check_clean("a \n b", "a\n b");

    /* Truncation must not split a multi-byte codepoint. */
    char small[4];
    size_t n = textnorm_clean("\xC3\xA1\xC3\xA1", 4, small, sizeof(small)); /* "áá" = 4 bytes */
    CHECK_EQ_INT(n, 2);            /* only the first á fits with its NUL */
    CHECK_EQ_INT(small[2], '\0');
    CHECK_EQ_INT((unsigned char)small[0], 0xC3);
    CHECK_EQ_INT((unsigned char)small[1], 0xA1);

    /* Degenerate buffers. */
    char one[1];
    CHECK_EQ_INT(textnorm_clean("abc", 3, one, sizeof(one)), 0);
    CHECK_EQ_INT(one[0], '\0');
    CHECK_EQ_INT(textnorm_clean(NULL, 0, one, sizeof(one)), 0);
})
