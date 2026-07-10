#include "core/typeplan.h"
#include "test_util.h"

/* A synthetic dead-key layout, as in test_keymap.c: 'á' composes from two
 * presses. Not a real ABNT2 table -- those keycodes are unverified. */
#define DEAD_ACUTE HID_KEY_LBRACKET

static bool fake_lookup(uint32_t cp, hid_seq_t *out)
{
    if (cp == 'a') {
        out->steps[0] = (hid_step_t){0, HID_KEY_A};
        out->n = 1;
        return true;
    }
    if (cp == 0x00E1) { /* á */
        out->steps[0] = (hid_step_t){0, DEAD_ACUTE};
        out->steps[1] = (hid_step_t){0, HID_KEY_A};
        out->n = 2;
        return true;
    }
    return false;
}
static const keymap_layout_t fake_layout = {.name = "fake", .lookup = fake_lookup};

/* Type a whole string, concatenating the frames, the way hid_kbd.c streams. */
static size_t plan_string(const char *ascii, hid_step_t *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = ascii; *p; p++) {
        hid_frames_t f;
        if (typeplan_char(&keymap_us, (uint32_t)(unsigned char)*p, &f) == KEYMAP_SKIPPED) {
            continue;
        }
        for (uint8_t i = 0; i < f.n && n < cap; i++) {
            out[n++] = f.frames[i];
        }
    }
    return n;
}

TEST_MAIN("typeplan", {
    hid_frames_t f;

    /* --- one character is press then release --- */
    CHECK_EQ_INT(typeplan_char(&keymap_us, 'a', &f), KEYMAP_EXACT);
    CHECK_EQ_INT(f.n, 2);
    CHECK_EQ_INT(f.frames[0].key, HID_KEY_A);
    CHECK_EQ_INT(f.frames[0].mod, HID_MOD_NONE);
    CHECK(typeplan_is_release(&f.frames[1]));

    /* The modifier rides the press frame and is cleared by the release. */
    CHECK_EQ_INT(typeplan_char(&keymap_us, 'A', &f), KEYMAP_EXACT);
    CHECK_EQ_INT(f.n, 2);
    CHECK_EQ_INT(f.frames[0].mod, HID_MOD_LSHIFT);
    CHECK(typeplan_is_release(&f.frames[1]));

    /* --- the bug this file exists to prevent ---
     * "aa" must be down, up, down, up. Without the release in the middle the
     * host sees no state change and types a single 'a'. */
    hid_step_t s[16];
    size_t n = plan_string("aa", s, 16);
    CHECK_EQ_INT(n, 4);
    CHECK_EQ_INT(s[0].key, HID_KEY_A);
    CHECK(typeplan_is_release(&s[1]));
    CHECK_EQ_INT(s[2].key, HID_KEY_A);
    CHECK(typeplan_is_release(&s[3]));

    /* No two consecutive frames may ever be identical, for any string. */
    const char *tricky[] = {"aa", "hello", "ssl", "mississippi", "  ", "AA", "aA", "..", "!!", ""};
    for (size_t t = 0; t < sizeof(tricky) / sizeof(tricky[0]); t++) {
        hid_step_t buf[128];
        const size_t m = plan_string(tricky[t], buf, 128);
        for (size_t i = 1; i < m; i++) {
            CHECK(!(buf[i].key == buf[i - 1].key && buf[i].mod == buf[i - 1].mod));
        }
        /* Every plan ends released, so no key is left held down. */
        if (m > 0) {
            CHECK(typeplan_is_release(&buf[m - 1]));
        }
    }

    /* "hello" is 5 characters, so 10 frames. */
    n = plan_string("hello", s, 16);
    CHECK_EQ_INT(n, 10);

    /* --- dead keys: two presses, so four frames --- */
    CHECK_EQ_INT(typeplan_char(&fake_layout, 0x00E1, &f), KEYMAP_EXACT);
    CHECK_EQ_INT(f.n, 4);
    CHECK_EQ_INT(f.frames[0].key, DEAD_ACUTE);
    CHECK(typeplan_is_release(&f.frames[1]));
    CHECK_EQ_INT(f.frames[2].key, HID_KEY_A);
    CHECK(typeplan_is_release(&f.frames[3]));
    CHECK(f.n <= TYPEPLAN_MAX_FRAMES);

    /* --- the fallback ladder is inherited from keymap_resolve --- */
    /* US cannot type á, so it de-accents to 'a': one press, two frames. */
    CHECK_EQ_INT(typeplan_char(&keymap_us, 0x00E1, &f), KEYMAP_DEACCENTED);
    CHECK_EQ_INT(f.n, 2);
    CHECK_EQ_INT(f.frames[0].key, HID_KEY_A);

    /* Ç keeps its shift when de-accented to 'C'. */
    CHECK_EQ_INT(typeplan_char(&keymap_us, 0x00C7, &f), KEYMAP_DEACCENTED);
    CHECK_EQ_INT(f.frames[0].mod, HID_MOD_LSHIFT);

    /* An emoji yields no frames at all -- never a '?' placeholder. */
    CHECK_EQ_INT(typeplan_char(&keymap_us, 0x1F600, &f), KEYMAP_SKIPPED);
    CHECK_EQ_INT(f.n, 0);
    CHECK_EQ_INT(f.frames[0].key, HID_KEY_NONE);

    /* Newline and tab are real keys, not skipped. */
    CHECK_EQ_INT(typeplan_char(&keymap_us, '\n', &f), KEYMAP_EXACT);
    CHECK_EQ_INT(f.frames[0].key, HID_KEY_ENTER);
    CHECK_EQ_INT(typeplan_char(&keymap_us, '\t', &f), KEYMAP_EXACT);
    CHECK_EQ_INT(f.frames[0].key, HID_KEY_TAB);

    /* A realistic transcript: every printable ASCII char plans cleanly and
     * ends released. */
    for (uint32_t c = 0x20; c < 0x7F; c++) {
        CHECK_EQ_INT(typeplan_char(&keymap_us, c, &f), KEYMAP_EXACT);
        CHECK_EQ_INT(f.n, 2);
        CHECK(typeplan_is_release(&f.frames[1]));
        CHECK(f.frames[0].key != HID_KEY_NONE);
    }

    /* Defensive. */
    CHECK_EQ_INT(typeplan_char(NULL, 'a', &f), KEYMAP_SKIPPED);
    CHECK_EQ_INT(f.n, 0);
})
