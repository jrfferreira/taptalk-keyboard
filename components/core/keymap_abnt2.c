/* Portuguese (Brazil), ABNT2.
 *
 * Transcribed from Microsoft's KBDBR tables (kbdbr.dll 10.0.25393.1, via
 * kbdlayout.info), with Windows scancodes translated to HID usages — no
 * position in here is from memory. Accented letters are dead-key
 * compositions: the accent key, then the letter. A standalone accent (or the
 * ASCII '`', '~', '^', which live on dead keys here) is the accent key, then
 * space.
 *
 * The AltGr column matches Windows and Linux, which both treat Right Alt as
 * AltGr for this layout. macOS builds its Option combinations differently;
 * on a macOS host those entries may come out wrong until validated. */
#include "core/keymap.h"

#define S HID_MOD_LSHIFT
#define G HID_MOD_RALT

#define ONE(cp, m, k) {(cp), {{{(m), (k)}}, 1}}
#define TWO(cp, dead, m, k) {(cp), {{{dead}, {(m), (k)}}, 2}}
/* One dead-key accent, lowercase and uppercase of the same letter key.
 * Spelled out rather than via TWO(): the DEAD_* argument expands to
 * "mod, key" before substitution, which TWO would read as an extra arg. */
#define ACCENT(dead, key, lower, upper) \
    {(lower), {{{dead}, {0, (key)}}, 2}}, {(upper), {{{dead}, {S, (key)}}, 2}}

/* The five ABNT2 dead keys, as (modifier, usage) pairs. */
#define DEAD_ACUTE 0, HID_KEY_LBRACKET     /* ´ */
#define DEAD_GRAVE S, HID_KEY_LBRACKET     /* ` */
#define DEAD_TILDE 0, HID_KEY_APOSTROPHE   /* ~ */
#define DEAD_CIRC  S, HID_KEY_APOSTROPHE   /* ^ */
#define DEAD_TREMA S, 0x23                 /* ¨ rides Shift+6 */

static const keymap_entry_t abnt2_entries[] = {
    ONE('\t', 0, HID_KEY_TAB),
    ONE('\n', 0, HID_KEY_ENTER),
    ONE(' ', 0, HID_KEY_SPACE),

    /* Letters sit where US QWERTY puts them. */
    ONE('a', 0, HID_KEY_A), ONE('b', 0, 0x05), ONE('c', 0, 0x06), ONE('d', 0, 0x07),
    ONE('e', 0, 0x08), ONE('f', 0, 0x09), ONE('g', 0, 0x0A), ONE('h', 0, 0x0B),
    ONE('i', 0, 0x0C), ONE('j', 0, 0x0D), ONE('k', 0, 0x0E), ONE('l', 0, 0x0F),
    ONE('m', 0, 0x10), ONE('n', 0, 0x11), ONE('o', 0, 0x12), ONE('p', 0, 0x13),
    ONE('q', 0, 0x14), ONE('r', 0, 0x15), ONE('s', 0, 0x16), ONE('t', 0, 0x17),
    ONE('u', 0, 0x18), ONE('v', 0, 0x19), ONE('w', 0, 0x1A), ONE('x', 0, 0x1B),
    ONE('y', 0, 0x1C), ONE('z', 0, HID_KEY_Z),

    ONE('A', S, HID_KEY_A), ONE('B', S, 0x05), ONE('C', S, 0x06), ONE('D', S, 0x07),
    ONE('E', S, 0x08), ONE('F', S, 0x09), ONE('G', S, 0x0A), ONE('H', S, 0x0B),
    ONE('I', S, 0x0C), ONE('J', S, 0x0D), ONE('K', S, 0x0E), ONE('L', S, 0x0F),
    ONE('M', S, 0x10), ONE('N', S, 0x11), ONE('O', S, 0x12), ONE('P', S, 0x13),
    ONE('Q', S, 0x14), ONE('R', S, 0x15), ONE('S', S, 0x16), ONE('T', S, 0x17),
    ONE('U', S, 0x18), ONE('V', S, 0x19), ONE('W', S, 0x1A), ONE('X', S, 0x1B),
    ONE('Y', S, 0x1C), ONE('Z', S, HID_KEY_Z),

    ONE('1', 0, HID_KEY_1), ONE('2', 0, 0x1F), ONE('3', 0, 0x20), ONE('4', 0, 0x21),
    ONE('5', 0, 0x22), ONE('6', 0, 0x23), ONE('7', 0, 0x24), ONE('8', 0, 0x25),
    ONE('9', 0, HID_KEY_9), ONE('0', 0, HID_KEY_0),

    /* Shift+6 is the trema dead key, so '^' does NOT live on the digit row. */
    ONE('!', S, HID_KEY_1), ONE('@', S, 0x1F), ONE('#', S, 0x20), ONE('$', S, 0x21),
    ONE('%', S, 0x22), ONE('&', S, 0x24), ONE('*', S, 0x25),
    ONE('(', S, HID_KEY_9), ONE(')', S, HID_KEY_0),

    ONE('-', 0, HID_KEY_MINUS), ONE('_', S, HID_KEY_MINUS),
    ONE('=', 0, HID_KEY_EQUAL), ONE('+', S, HID_KEY_EQUAL),
    ONE('[', 0, HID_KEY_RBRACKET), ONE('{', S, HID_KEY_RBRACKET),
    ONE(']', 0, HID_KEY_NONUS_HASH), ONE('}', S, HID_KEY_NONUS_HASH),
    ONE('\\', 0, HID_KEY_NONUS_BSLASH), ONE('|', S, HID_KEY_NONUS_BSLASH),
    ONE('\'', 0, HID_KEY_GRAVE), ONE('"', S, HID_KEY_GRAVE),
    ONE(',', 0, HID_KEY_COMMA), ONE('<', S, HID_KEY_COMMA),
    ONE('.', 0, HID_KEY_PERIOD), ONE('>', S, HID_KEY_PERIOD),
    ONE(';', 0, HID_KEY_SLASH), ONE(':', S, HID_KEY_SLASH),
    ONE('/', 0, HID_KEY_INTL1), ONE('?', S, HID_KEY_INTL1),

    /* ASCII characters that only exist as dead keys: accent, then space. */
    TWO('`', DEAD_GRAVE, 0, HID_KEY_SPACE),
    TWO('~', DEAD_TILDE, 0, HID_KEY_SPACE),
    TWO('^', DEAD_CIRC, 0, HID_KEY_SPACE),

    ONE(0x00E7, 0, HID_KEY_SEMICOLON), /* ç has its own key */
    ONE(0x00C7, S, HID_KEY_SEMICOLON), /* Ç */

    ACCENT(DEAD_ACUTE, HID_KEY_A, 0x00E1, 0x00C1), /* á Á */
    ACCENT(DEAD_ACUTE, 0x08, 0x00E9, 0x00C9),      /* é É */
    ACCENT(DEAD_ACUTE, 0x0C, 0x00ED, 0x00CD),      /* í Í */
    ACCENT(DEAD_ACUTE, 0x12, 0x00F3, 0x00D3),      /* ó Ó */
    ACCENT(DEAD_ACUTE, 0x18, 0x00FA, 0x00DA),      /* ú Ú */
    ACCENT(DEAD_ACUTE, 0x1C, 0x00FD, 0x00DD),      /* ý Ý */

    ACCENT(DEAD_GRAVE, HID_KEY_A, 0x00E0, 0x00C0), /* à À */
    ACCENT(DEAD_GRAVE, 0x08, 0x00E8, 0x00C8),      /* è È */
    ACCENT(DEAD_GRAVE, 0x0C, 0x00EC, 0x00CC),      /* ì Ì */
    ACCENT(DEAD_GRAVE, 0x12, 0x00F2, 0x00D2),      /* ò Ò */
    ACCENT(DEAD_GRAVE, 0x18, 0x00F9, 0x00D9),      /* ù Ù */

    ACCENT(DEAD_TILDE, HID_KEY_A, 0x00E3, 0x00C3), /* ã Ã */
    ACCENT(DEAD_TILDE, 0x12, 0x00F5, 0x00D5),      /* õ Õ */
    ACCENT(DEAD_TILDE, 0x11, 0x00F1, 0x00D1),      /* ñ Ñ */

    ACCENT(DEAD_CIRC, HID_KEY_A, 0x00E2, 0x00C2), /* â Â */
    ACCENT(DEAD_CIRC, 0x08, 0x00EA, 0x00CA),      /* ê Ê */
    ACCENT(DEAD_CIRC, 0x0C, 0x00EE, 0x00CE),      /* î Î */
    ACCENT(DEAD_CIRC, 0x12, 0x00F4, 0x00D4),      /* ô Ô */
    ACCENT(DEAD_CIRC, 0x18, 0x00FB, 0x00DB),      /* û Û */

    ACCENT(DEAD_TREMA, HID_KEY_A, 0x00E4, 0x00C4), /* ä Ä */
    ACCENT(DEAD_TREMA, 0x08, 0x00EB, 0x00CB),      /* ë Ë */
    ACCENT(DEAD_TREMA, 0x0C, 0x00EF, 0x00CF),      /* ï Ï */
    ACCENT(DEAD_TREMA, 0x12, 0x00F6, 0x00D6),      /* ö Ö */
    ACCENT(DEAD_TREMA, 0x18, 0x00FC, 0x00DC),      /* ü Ü */
    TWO(0x00FF, DEAD_TREMA, 0, 0x1C),              /* ÿ — KBDBR has no Ÿ */

    TWO(0x00B4, DEAD_ACUTE, 0, HID_KEY_SPACE), /* standalone ´ */
    TWO(0x00A8, DEAD_TREMA, 0, HID_KEY_SPACE), /* standalone ¨ */

    /* AltGr row. Correct for Windows and Linux hosts; see the header note. */
    ONE(0x00B9, G, HID_KEY_1),    /* ¹ */
    ONE(0x00B2, G, 0x1F),         /* ² */
    ONE(0x00B3, G, 0x20),         /* ³ */
    ONE(0x00A3, G, 0x21),         /* £ */
    ONE(0x00A2, G, 0x22),         /* ¢ */
    ONE(0x00AC, G, 0x23),         /* ¬ */
    ONE(0x00A7, G, HID_KEY_EQUAL),      /* § */
    ONE(0x00AA, G, HID_KEY_RBRACKET),   /* ª */
    ONE(0x00BA, G, HID_KEY_NONUS_HASH), /* º */
    ONE(0x00B0, G, 0x08),               /* ° on AltGr+E */
};

static bool abnt2_lookup(uint32_t cp, hid_seq_t *out)
{
    return keymap_table_lookup(abnt2_entries, sizeof(abnt2_entries) / sizeof(abnt2_entries[0]),
                               cp, out);
}

const keymap_layout_t keymap_abnt2 = {
    .name   = "abnt2",
    .lookup = abnt2_lookup,
};
