#include "core/keymap.h"
#include "test_util.h"

/* A synthetic dead-key layout. Its only job is to prove the engine can carry
 * a two-step composition, which is what ABNT2 needs for 'á' (dead acute, then
 * 'a'). Deliberately NOT a real ABNT2 table — those keycodes need verifying
 * against hardware before anyone types with them. */
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

static hid_seq_t resolve(const keymap_layout_t *l, uint32_t cp, keymap_result_t *r)
{
    hid_seq_t s;
    *r = keymap_resolve(l, cp, &s);
    return s;
}

TEST_MAIN("keymap", {
    keymap_result_t r;
    hid_seq_t s;

    /* --- US layout, single-step --- */
    s = resolve(&keymap_us, 'a', &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_A);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);

    s = resolve(&keymap_us, 'A', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_A);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* '0' sits after '9' in the usage table, a classic off-by-one trap. */
    s = resolve(&keymap_us, '1', &r); CHECK_EQ_INT(s.steps[0].key, 0x1E);
    s = resolve(&keymap_us, '9', &r); CHECK_EQ_INT(s.steps[0].key, 0x26);
    s = resolve(&keymap_us, '0', &r); CHECK_EQ_INT(s.steps[0].key, 0x27);

    /* Shifted symbols ride the digit keys. */
    s = resolve(&keymap_us, '!', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1E); CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_us, ')', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x27); CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    s = resolve(&keymap_us, ' ', &r);  CHECK_EQ_INT(s.steps[0].key, HID_KEY_SPACE);
    s = resolve(&keymap_us, '\n', &r); CHECK_EQ_INT(s.steps[0].key, HID_KEY_ENTER);
    s = resolve(&keymap_us, '\t', &r); CHECK_EQ_INT(s.steps[0].key, HID_KEY_TAB);
    s = resolve(&keymap_us, '?', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SLASH); CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* Every printable ASCII char must be reachable on US. */
    for (uint32_t c = 0x20; c < 0x7F; c++) {
        (void)resolve(&keymap_us, c, &r);
        CHECK_EQ_INT(r, KEYMAP_EXACT);
    }

    /* --- fallback ladder --- */
    /* US cannot type á, so it de-accents to 'a'. */
    s = resolve(&keymap_us, 0x00E1, &r);
    CHECK_EQ_INT(r, KEYMAP_DEACCENTED);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_A);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);

    /* Ç de-accents to 'C', keeping the shift. */
    s = resolve(&keymap_us, 0x00C7, &r);
    CHECK_EQ_INT(r, KEYMAP_DEACCENTED);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* Curly quote becomes a straight one. */
    s = resolve(&keymap_us, 0x2019, &r);
    CHECK_EQ_INT(r, KEYMAP_DEACCENTED);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);

    /* Emoji is skipped, and the sequence is zeroed — never a '?' placeholder. */
    s = resolve(&keymap_us, 0x1F600, &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);
    CHECK_EQ_INT(s.n, 0);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONE);

    /* CJK: no ASCII base, so skipped rather than mangled. */
    s = resolve(&keymap_us, 0x4E2D, &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);

    /* A NUL is not typeable. */
    s = resolve(&keymap_us, 0, &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);

    /* --- dead-key composition --- */
    s = resolve(&fake_layout, 0x00E1, &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, DEAD_ACUTE);
    CHECK_EQ_INT(s.steps[1].key, HID_KEY_A);

    /* The layout has 'a' but not 'b'; 'b' has no accent to strip, so skip. */
    s = resolve(&fake_layout, 'b', &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);

    /* An exact multi-step hit beats de-accenting: we must not silently
     * downgrade 'á' to 'a' on a layout that can actually compose it. */
    s = resolve(&fake_layout, 0x00E1, &r);
    CHECK_EQ_INT(s.n, 2);

    /* Defensive: a null layout resolves to SKIPPED rather than crashing. */
    s = resolve(NULL, 'a', &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);
})
