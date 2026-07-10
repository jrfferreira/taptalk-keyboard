#include "core/jsonesc.h"
#include "test_util.h"

static int esc(const char *in, char *out, size_t cap)
{
    return json_escape(in, strlen(in), out, cap);
}

TEST_MAIN("jsonesc", {
    char v[128];

    /* --- pass-through --- */
    CHECK_EQ_INT(esc("HomeWiFi", v, sizeof(v)), 8);
    CHECK_EQ_STR(v, "HomeWiFi");
    CHECK_EQ_INT(esc("", v, sizeof(v)), 0);
    CHECK_EQ_STR(v, "");
    /* Valid UTF-8 survives byte for byte: an SSID may be "Café". */
    CHECK_EQ_INT(esc("Caf\xC3\xA9", v, sizeof(v)), 5);
    CHECK_EQ_STR(v, "Caf\xC3\xA9");
    /* Spaces, dashes, emoji-in-SSID all pass. */
    CHECK_EQ_INT(esc("my net-2.4G", v, sizeof(v)), 11);

    /* --- the escapes that matter --- */
    CHECK_EQ_INT(esc("a\"b", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\\"b");
    CHECK_EQ_INT(esc("a\\b", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\\\b");
    CHECK_EQ_INT(esc("a\nb", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\nb");
    CHECK_EQ_INT(esc("a\tb", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\tb");
    CHECK_EQ_INT(esc("a\rb", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\rb");
    CHECK_EQ_INT(esc("a\bb", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\bb");
    CHECK_EQ_INT(esc("a\fb", v, sizeof(v)), 4);   CHECK_EQ_STR(v, "a\\fb");

    /* Other control bytes become \u00XX; a raw one would be invalid JSON. */
    CHECK_EQ_INT(esc("a\x01" "b", v, sizeof(v)), 8);
    CHECK_EQ_STR(v, "a\\u0001b");
    CHECK_EQ_INT(esc("\x1f", v, sizeof(v)), 6);
    CHECK_EQ_STR(v, "\\u001f");
    /* NUL cannot appear via strlen, so drive the length directly. */
    CHECK_EQ_INT(json_escape("a\0b", 3, v, sizeof(v)), 8);
    CHECK_EQ_STR(v, "a\\u0000b");

    /* 0x7F (DEL) is not a C0 control and needs no escape. */
    CHECK_EQ_INT(esc("\x7f", v, sizeof(v)), 1);

    /* --- the attack this exists to stop ---
     * A neighbour names their network so as to break out of the JSON string
     * and inject a key of their own. */
    const char *evil = "\",\"admin\":true,\"x\":\"";
    CHECK(esc(evil, v, sizeof(v)) > 0);
    /* No unescaped quote may survive: every '"' must be preceded by a '\'. */
    for (size_t i = 0; v[i]; i++) {
        if (v[i] == '"') {
            CHECK(i > 0 && v[i - 1] == '\\');
        }
    }
    /* And a lone backslash cannot escape our escaping. */
    CHECK_EQ_INT(esc("\\\"", v, sizeof(v)), 4);
    CHECK_EQ_STR(v, "\\\\\\\"");

    /* --- overflow refuses rather than truncating mid-escape --- */
    char small[4];
    CHECK_EQ_INT(esc("abc", small, sizeof(small)), 3);
    CHECK_EQ_STR(small, "abc");
    CHECK_EQ_INT(esc("abcd", small, sizeof(small)), -1);
    CHECK_EQ_STR(small, "");
    /* An escape that only half fits must fail, not emit a dangling backslash. */
    CHECK_EQ_INT(esc("ab\"", small, sizeof(small)), -1);
    CHECK_EQ_STR(small, "");
    CHECK_EQ_INT(esc("\x01", small, sizeof(small)), -1); /* needs 6 bytes */
    CHECK_EQ_STR(small, "");

    /* Degenerate arguments. */
    CHECK_EQ_INT(json_escape("a", 1, NULL, 10), -1);
    CHECK_EQ_INT(json_escape("a", 1, v, 0), -1);
    CHECK_EQ_INT(json_escape(NULL, 0, v, sizeof(v)), 0);
    CHECK_EQ_STR(v, "");

    /* A worst-case SSID: 32 bytes of quotes needs 64 bytes out. */
    char worst[33];
    memset(worst, '"', 32);
    worst[32] = '\0';
    char big[80];
    CHECK_EQ_INT(esc(worst, big, sizeof(big)), 64);
    char tight[64]; /* 64 chars + NUL does not fit in 64 */
    CHECK_EQ_INT(esc(worst, tight, sizeof(tight)), -1);
})
