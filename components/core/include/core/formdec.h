/* application/x-www-form-urlencoded field extraction.
 *
 * This parses attacker-adjacent input: whatever a browser (or anything else on
 * the setup access point) POSTs at us. It is pure, so every malformed body we
 * can think of gets a test rather than a field trial. */
#pragma once

#include <stddef.h>

enum {
    FORMDEC_NOT_FOUND = -1,
    FORMDEC_BAD       = -2, /* malformed escape, or the value does not fit */
};

/* Find `name` in `body` and URL-decode its value into `out`.
 *
 * `name` is matched exactly against a whole key, so asking for "pass" never
 * matches a field called "password".
 *
 * Returns the decoded length (>= 0, always NUL-terminated), FORMDEC_NOT_FOUND,
 * or FORMDEC_BAD. On any negative return, out[0] is set to '\0'. */
int form_get(const char *body, size_t body_len, const char *name, char *out, size_t out_cap);
