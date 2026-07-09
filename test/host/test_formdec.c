#include "core/formdec.h"
#include "test_util.h"

static int get(const char *body, const char *name, char *out, size_t cap)
{
    return form_get(body, strlen(body), name, out, cap);
}

TEST_MAIN("formdec", {
    char v[64];

    /* --- the happy path --- */
    CHECK_EQ_INT(get("ssid=home&pass=secret", "ssid", v, sizeof(v)), 4);
    CHECK_EQ_STR(v, "home");
    CHECK_EQ_INT(get("ssid=home&pass=secret", "pass", v, sizeof(v)), 6);
    CHECK_EQ_STR(v, "secret");

    /* First and last field, and a single-field body. */
    CHECK(get("a=1&b=2&c=3", "a", v, sizeof(v)) == 1); CHECK_EQ_STR(v, "1");
    CHECK(get("a=1&b=2&c=3", "c", v, sizeof(v)) == 1); CHECK_EQ_STR(v, "3");
    CHECK(get("only=x", "only", v, sizeof(v)) == 1);   CHECK_EQ_STR(v, "x");

    /* --- whole-key matching ---
     * "pass" must not match "password". Getting this wrong hands the Wi-Fi
     * password field whatever the API-key field contained, or vice versa. */
    CHECK_EQ_INT(get("password=wrong&pass=right", "pass", v, sizeof(v)), 5);
    CHECK_EQ_STR(v, "right");
    CHECK_EQ_INT(get("pass=right&password=wrong", "password", v, sizeof(v)), 5);
    CHECK_EQ_STR(v, "wrong");
    /* A key that is a suffix of another must not match either. */
    CHECK_EQ_INT(get("mypass=x", "pass", v, sizeof(v)), FORMDEC_NOT_FOUND);

    /* --- percent and plus decoding --- */
    CHECK_EQ_INT(get("k=%41%42", "k", v, sizeof(v)), 2); CHECK_EQ_STR(v, "AB");
    CHECK_EQ_INT(get("k=a+b", "k", v, sizeof(v)), 3);    CHECK_EQ_STR(v, "a b");
    CHECK_EQ_INT(get("k=%2b", "k", v, sizeof(v)), 1);    CHECK_EQ_STR(v, "+");
    CHECK_EQ_INT(get("k=%2F", "k", v, sizeof(v)), 1);    CHECK_EQ_STR(v, "/");
    /* Lower and upper hex both decode. */
    CHECK_EQ_INT(get("k=%7e", "k", v, sizeof(v)), 1);    CHECK_EQ_STR(v, "~");
    CHECK_EQ_INT(get("k=%7E", "k", v, sizeof(v)), 1);    CHECK_EQ_STR(v, "~");

    /* A Wi-Fi password with the characters that actually break naive parsers. */
    CHECK_EQ_INT(get("pass=p%26w%3Dd%2Bx", "pass", v, sizeof(v)), 7);
    CHECK_EQ_STR(v, "p&w=d+x");

    /* A realistic OpenAI key survives intact. */
    CHECK_EQ_INT(get("key=sk-proj-AbC_123-xyz", "key", v, sizeof(v)), 19);
    CHECK_EQ_STR(v, "sk-proj-AbC_123-xyz");

    /* --- malformed escapes must be rejected, not read past the end --- */
    CHECK_EQ_INT(get("k=%zz", "k", v, sizeof(v)), FORMDEC_BAD);
    CHECK_EQ_INT(get("k=%4", "k", v, sizeof(v)), FORMDEC_BAD);   /* truncated */
    CHECK_EQ_INT(get("k=%", "k", v, sizeof(v)), FORMDEC_BAD);
    CHECK_EQ_INT(get("k=%4g", "k", v, sizeof(v)), FORMDEC_BAD);
    /* Truncated escape immediately before the next field: must not consume '&'. */
    CHECK_EQ_INT(get("k=%&j=1", "k", v, sizeof(v)), FORMDEC_BAD);
    CHECK_EQ_INT(get("k=ab%", "k", v, sizeof(v)), FORMDEC_BAD);
    /* On a bad value, out is cleared rather than left half-decoded. */
    CHECK_EQ_STR(v, "");

    /* --- missing, empty, degenerate --- */
    CHECK_EQ_INT(get("a=1", "b", v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(get("", "a", v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(get("a=", "a", v, sizeof(v)), 0);  CHECK_EQ_STR(v, "");
    CHECK_EQ_INT(get("a=&b=2", "a", v, sizeof(v)), 0); CHECK_EQ_STR(v, "");
    CHECK_EQ_INT(get("a=&b=2", "b", v, sizeof(v)), 1); CHECK_EQ_STR(v, "2");
    /* A pair with no '=' is skipped, not matched. */
    CHECK_EQ_INT(get("novalue&a=1", "novalue", v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(get("novalue&a=1", "a", v, sizeof(v)), 1);
    /* Stray separators. */
    CHECK_EQ_INT(get("&&a=1&&", "a", v, sizeof(v)), 1); CHECK_EQ_STR(v, "1");
    CHECK_EQ_INT(get("=orphan&a=1", "a", v, sizeof(v)), 1);

    /* An '=' inside the value belongs to the value. */
    CHECK_EQ_INT(get("a=x=y", "a", v, sizeof(v)), 3); CHECK_EQ_STR(v, "x=y");

    /* --- overflow refuses rather than truncating a credential --- */
    char small[4];
    CHECK_EQ_INT(get("a=abc", "a", small, sizeof(small)), 3); /* exactly fits with NUL */
    CHECK_EQ_STR(small, "abc");
    CHECK_EQ_INT(get("a=abcd", "a", small, sizeof(small)), FORMDEC_BAD);
    CHECK_EQ_STR(small, "");
    /* Overflow detected mid-escape, too. */
    CHECK_EQ_INT(get("a=abc%41", "a", small, sizeof(small)), FORMDEC_BAD);

    /* NUL bytes cannot be smuggled in to truncate a stored credential.
     * They decode literally; the caller stores by returned length. */
    CHECK_EQ_INT(get("a=x%00y", "a", v, sizeof(v)), 3);
    CHECK_EQ_INT(v[1], 0);

    /* Defensive arguments. */
    CHECK_EQ_INT(form_get(NULL, 0, "a", v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(form_get("a=1", 3, NULL, v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(form_get("a=1", 3, "a", NULL, 10), FORMDEC_BAD);
    CHECK_EQ_INT(form_get("a=1", 3, "a", v, 0), FORMDEC_BAD);

    /* body_len is honoured over any NUL: a short length must not read on. */
    CHECK_EQ_INT(form_get("a=1&b=2", 3, "b", v, sizeof(v)), FORMDEC_NOT_FOUND);
    CHECK_EQ_INT(form_get("a=1&b=2", 3, "a", v, sizeof(v)), 1);
})
