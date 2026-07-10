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

    /* --- layout registry --- */
    CHECK(keymap_by_name("us") == &keymap_us);
    CHECK(keymap_by_name("abnt2") == &keymap_abnt2);
    CHECK(keymap_by_name("pt") == &keymap_pt);
    CHECK(keymap_by_name("US") == NULL); /* exact match only */
    CHECK(keymap_by_name("qwerty") == NULL);
    CHECK(keymap_by_name("") == NULL);
    CHECK(keymap_by_name(NULL) == NULL);
    CHECK_EQ_INT(keymap_layouts_count, 10);

    /* --- invariants every built-in layout must hold --- */
    for (size_t li = 0; li < keymap_layouts_count; li++) {
        const keymap_layout_t *l = keymap_layouts[li];
        CHECK(keymap_by_name(l->name) == l);

        /* Every printable ASCII char is reachable, except the few a layout
         * genuinely lacks (Italian has no ` or ~; Italians use Alt-codes).
         * Those must SKIP — a visible hole, never a wrong character. A hole
         * appearing on any other layout is a table bug. */
        const char *missing = strcmp(l->name, "it") == 0 ? "`~" : "";
        for (uint32_t c = 0x20; c < 0x7F; c++) {
            s = resolve(l, c, &r);
            CHECK_EQ_INT(r, strchr(missing, (int)c) ? KEYMAP_SKIPPED : KEYMAP_EXACT);
        }

        /* Whatever resolves must be well-formed: a plausible step count,
         * no all-zero press inside the sequence, zeroed steps after it, and
         * only Shift/AltGr as modifiers — this firmware never chords Ctrl or
         * GUI, where a stray bit becomes a hotkey on the host. */
        for (uint32_t c = 0x01; c <= 0x2100; c++) {
            s = resolve(l, c, &r);
            if (r == KEYMAP_SKIPPED) {
                continue;
            }
            CHECK(s.n >= 1 && s.n <= KEYMAP_MAX_STEPS);
            for (uint8_t i = 0; i < KEYMAP_MAX_STEPS; i++) {
                if (i < s.n) {
                    CHECK(s.steps[i].key != HID_KEY_NONE);
                    CHECK_EQ_INT(s.steps[i].mod & ~(HID_MOD_LSHIFT | HID_MOD_RALT), 0);
                } else {
                    CHECK_EQ_INT(s.steps[i].key, HID_KEY_NONE);
                    CHECK_EQ_INT(s.steps[i].mod, 0);
                }
            }
        }

        /* Emoji and CJK skip everywhere; no layout gets to invent them. */
        s = resolve(l, 0x1F600, &r);
        CHECK_EQ_INT(r, KEYMAP_SKIPPED);
        s = resolve(l, 0x4E2D, &r);
        CHECK_EQ_INT(r, KEYMAP_SKIPPED);
    }

    /* --- ABNT2 spot checks, against the KBDBR tables --- */
    /* ç has its own key; no composition and no de-accent fallback. */
    s = resolve(&keymap_abnt2, 0x00E7, &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SEMICOLON);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_abnt2, 0x00C7, &r); /* Ç */
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* á: dead acute (the key at the US-[ position), then the letter. */
    s = resolve(&keymap_abnt2, 0x00E1, &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    CHECK_EQ_INT(s.steps[1].key, HID_KEY_A);
    CHECK_EQ_INT(s.steps[1].mod, HID_MOD_NONE);

    /* Á: same dead key, shifted letter. */
    s = resolve(&keymap_abnt2, 0x00C1, &r);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    CHECK_EQ_INT(s.steps[1].mod, HID_MOD_LSHIFT);

    /* ã / ê / à / ü pick the right dead key. */
    s = resolve(&keymap_abnt2, 0x00E3, &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_abnt2, 0x00EA, &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_abnt2, 0x00E0, &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_abnt2, 0x00FC, &r); /* ü: trema rides Shift+6 */
    CHECK_EQ_INT(s.steps[0].key, 0x23);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* Punctuation moved off its US positions. */
    s = resolve(&keymap_abnt2, '/', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_INTL1);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_abnt2, '?', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_INTL1);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_abnt2, ';', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SLASH);
    s = resolve(&keymap_abnt2, '\'', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_GRAVE);
    s = resolve(&keymap_abnt2, '"', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_GRAVE);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_abnt2, ']', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONUS_HASH);
    s = resolve(&keymap_abnt2, '\\', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONUS_BSLASH);

    /* ASCII '~', '^', '`' only exist as dead key + space. */
    s = resolve(&keymap_abnt2, '~', &r);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[1].key, HID_KEY_SPACE);
    s = resolve(&keymap_abnt2, '^', &r);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_abnt2, '`', &r);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* Shift+6 is the trema dead key, NOT '^'; '&' rides Shift+7. */
    s = resolve(&keymap_abnt2, '&', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x24);
    s = resolve(&keymap_abnt2, '@', &r); /* @ stays on Shift+2, unlike pt */
    CHECK_EQ_INT(s.steps[0].key, 0x1F);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* --- Portuguese (Portugal) spot checks, against the KBDPO tables --- */
    s = resolve(&keymap_pt, 0x00E7, &r); /* ç */
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SEMICOLON);

    /* á/à/ã: dead keys sit one key to the right of their ABNT2 homes. */
    s = resolve(&keymap_pt, 0x00E1, &r);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_RBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_pt, 0x00E0, &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_RBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_pt, 0x00E3, &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONUS_HASH);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_pt, 0x00FC, &r); /* ü: trema rides AltGr on +* */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);

    /* º and ª are direct keys here, not compositions. */
    s = resolve(&keymap_pt, 0x00BA, &r);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    s = resolve(&keymap_pt, 0x00AA, &r);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* Guillemets are a shifted key of their own. */
    s = resolve(&keymap_pt, 0x00AB, &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_EQUAL);
    s = resolve(&keymap_pt, 0x00BB, &r);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* The shifted digit row is nothing like US. */
    s = resolve(&keymap_pt, '"', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1F); /* Shift+2 */
    s = resolve(&keymap_pt, '/', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x24); /* Shift+7 */
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_pt, '=', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_0); /* Shift+0 */
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    s = resolve(&keymap_pt, '\'', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_MINUS);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_pt, '?', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_MINUS);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_pt, '-', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SLASH);
    s = resolve(&keymap_pt, '+', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);

    /* Brackets and @ live on AltGr. */
    s = resolve(&keymap_pt, '@', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1F);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);
    s = resolve(&keymap_pt, '[', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x25);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);
    s = resolve(&keymap_pt, '}', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_0);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);

    /* € types as AltGr+E. */
    s = resolve(&keymap_pt, 0x20AC, &r);
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.steps[0].key, 0x08);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);

    /* --- generated layouts: spot checks against their KBD* tables --- */
    /* The full-coverage invariants above run on all of these; the checks
     * here pin the load-bearing keys each layout is known for, so a
     * regenerated table cannot silently swap conventions. */

    /* UK: @ and " swap places relative to US; £ rides Shift+3. */
    CHECK(keymap_by_name("uk") == &keymap_uk);
    s = resolve(&keymap_uk, '@', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_uk, '"', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1F);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_uk, 0x00A3, &r); /* £ */
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.steps[0].key, 0x20);
    s = resolve(&keymap_uk, '#', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONUS_HASH);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);

    /* US International: ' ` " ~ ^ are dead keys (composed with space);
     * common accents also sit directly on AltGr. */
    CHECK(keymap_by_name("us-intl") == &keymap_usintl);
    s = resolve(&keymap_usintl, '\'', &r);
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[1].key, HID_KEY_SPACE);
    s = resolve(&keymap_usintl, 0x00E1, &r); /* á prefers 1-step AltGr+A */
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_A);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);
    s = resolve(&keymap_usintl, 0x00F1, &r); /* ñ */
    CHECK_EQ_INT(s.steps[0].key, 0x11);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);

    /* Spanish (Spain): ñ has its own key; ¡¿ direct; é composes on ´. */
    CHECK(keymap_by_name("es") == &keymap_es);
    s = resolve(&keymap_es, 0x00F1, &r); /* ñ */
    CHECK_EQ_INT(r, KEYMAP_EXACT);
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SEMICOLON);
    s = resolve(&keymap_es, 0x00BF, &r); /* ¿ = Shift+¡ key */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_EQUAL);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_es, 0x00E9, &r); /* é */
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    CHECK_EQ_INT(s.steps[1].key, 0x08);
    s = resolve(&keymap_es, 0x00E7, &r); /* ç still has a key in Spain */
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_NONUS_HASH);

    /* Latin American Spanish: @ famously rides AltGr+Q; ¿ is unshifted. */
    CHECK(keymap_by_name("es-la") == &keymap_esla);
    s = resolve(&keymap_esla, '@', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x14);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);
    s = resolve(&keymap_esla, 0x00BF, &r); /* ¿ */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_EQUAL);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_esla, 0x00F1, &r); /* ñ */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SEMICOLON);

    /* French AZERTY: letters move (a<->q, z<->w, m), digits need Shift,
     * and the common accents are direct keys on the digit row. */
    CHECK(keymap_by_name("fr") == &keymap_fr);
    s = resolve(&keymap_fr, 'a', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x14); /* the physical Q key */
    s = resolve(&keymap_fr, 'q', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_A);
    s = resolve(&keymap_fr, 'm', &r);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_SEMICOLON);
    s = resolve(&keymap_fr, '1', &r);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_fr, 0x00E9, &r); /* é: direct, digit row */
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, 0x1F);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_fr, 0x00EA, &r); /* ê: dead ^ then e */
    CHECK_EQ_INT(s.n, 2);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[1].key, 0x08);
    /* Legacy AZERTY cannot type Ç; the ladder de-accents it to C. */
    s = resolve(&keymap_fr, 0x00C7, &r);
    CHECK_EQ_INT(r, KEYMAP_DEACCENTED);
    CHECK_EQ_INT(s.steps[0].key, 0x06);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);

    /* German QWERTZ: y and z swap; umlauts and ß are direct keys. */
    CHECK(keymap_by_name("de") == &keymap_de);
    s = resolve(&keymap_de, 'z', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1C); /* the physical Y key */
    s = resolve(&keymap_de, 'y', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x1D);
    s = resolve(&keymap_de, 0x00FC, &r); /* ü */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    s = resolve(&keymap_de, 0x00DF, &r); /* ß */
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_MINUS);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_de, '@', &r);
    CHECK_EQ_INT(s.steps[0].key, 0x14);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_RALT);

    /* Italian: accented vowels are direct keys; ` and ~ do not exist. */
    CHECK(keymap_by_name("it") == &keymap_it);
    s = resolve(&keymap_it, 0x00E8, &r); /* è */
    CHECK_EQ_INT(s.n, 1);
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_LBRACKET);
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_NONE);
    s = resolve(&keymap_it, 0x00E9, &r); /* é */
    CHECK_EQ_INT(s.steps[0].mod, HID_MOD_LSHIFT);
    s = resolve(&keymap_it, 0x00E0, &r); /* à */
    CHECK_EQ_INT(s.steps[0].key, HID_KEY_APOSTROPHE);
    s = resolve(&keymap_it, '`', &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);
    s = resolve(&keymap_it, '~', &r);
    CHECK_EQ_INT(r, KEYMAP_SKIPPED);
})
