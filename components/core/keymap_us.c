/* US ANSI layout. Every printable ASCII character is one keystroke, so no
 * entry here uses more than one step — but the sequence type is what lets a
 * dead-key layout (ABNT2) drop in later without touching the engine. */
#include "core/keymap.h"

#include <string.h>

#define S HID_MOD_LSHIFT

/* Indexed by ASCII codepoint. key == HID_KEY_NONE means unmappable. */
static const hid_step_t us_ascii[128] = {
    ['\t'] = {0, HID_KEY_TAB},
    ['\n'] = {0, HID_KEY_ENTER},
    [' ']  = {0, HID_KEY_SPACE},

    ['a'] = {0, HID_KEY_A}, ['b'] = {0, 0x05}, ['c'] = {0, 0x06}, ['d'] = {0, 0x07},
    ['e'] = {0, 0x08}, ['f'] = {0, 0x09}, ['g'] = {0, 0x0A}, ['h'] = {0, 0x0B},
    ['i'] = {0, 0x0C}, ['j'] = {0, 0x0D}, ['k'] = {0, 0x0E}, ['l'] = {0, 0x0F},
    ['m'] = {0, 0x10}, ['n'] = {0, 0x11}, ['o'] = {0, 0x12}, ['p'] = {0, 0x13},
    ['q'] = {0, 0x14}, ['r'] = {0, 0x15}, ['s'] = {0, 0x16}, ['t'] = {0, 0x17},
    ['u'] = {0, 0x18}, ['v'] = {0, 0x19}, ['w'] = {0, 0x1A}, ['x'] = {0, 0x1B},
    ['y'] = {0, 0x1C}, ['z'] = {0, HID_KEY_Z},

    ['A'] = {S, HID_KEY_A}, ['B'] = {S, 0x05}, ['C'] = {S, 0x06}, ['D'] = {S, 0x07},
    ['E'] = {S, 0x08}, ['F'] = {S, 0x09}, ['G'] = {S, 0x0A}, ['H'] = {S, 0x0B},
    ['I'] = {S, 0x0C}, ['J'] = {S, 0x0D}, ['K'] = {S, 0x0E}, ['L'] = {S, 0x0F},
    ['M'] = {S, 0x10}, ['N'] = {S, 0x11}, ['O'] = {S, 0x12}, ['P'] = {S, 0x13},
    ['Q'] = {S, 0x14}, ['R'] = {S, 0x15}, ['S'] = {S, 0x16}, ['T'] = {S, 0x17},
    ['U'] = {S, 0x18}, ['V'] = {S, 0x19}, ['W'] = {S, 0x1A}, ['X'] = {S, 0x1B},
    ['Y'] = {S, 0x1C}, ['Z'] = {S, HID_KEY_Z},

    ['1'] = {0, HID_KEY_1}, ['2'] = {0, 0x1F}, ['3'] = {0, 0x20}, ['4'] = {0, 0x21},
    ['5'] = {0, 0x22}, ['6'] = {0, 0x23}, ['7'] = {0, 0x24}, ['8'] = {0, 0x25},
    ['9'] = {0, HID_KEY_9}, ['0'] = {0, HID_KEY_0},

    ['!'] = {S, HID_KEY_1}, ['@'] = {S, 0x1F}, ['#'] = {S, 0x20}, ['$'] = {S, 0x21},
    ['%'] = {S, 0x22}, ['^'] = {S, 0x23}, ['&'] = {S, 0x24}, ['*'] = {S, 0x25},
    ['('] = {S, HID_KEY_9}, [')'] = {S, HID_KEY_0},

    ['-']  = {0, HID_KEY_MINUS},      ['_'] = {S, HID_KEY_MINUS},
    ['=']  = {0, HID_KEY_EQUAL},      ['+'] = {S, HID_KEY_EQUAL},
    ['[']  = {0, HID_KEY_LBRACKET},   ['{'] = {S, HID_KEY_LBRACKET},
    [']']  = {0, HID_KEY_RBRACKET},   ['}'] = {S, HID_KEY_RBRACKET},
    ['\\'] = {0, HID_KEY_BACKSLASH},  ['|'] = {S, HID_KEY_BACKSLASH},
    [';']  = {0, HID_KEY_SEMICOLON},  [':'] = {S, HID_KEY_SEMICOLON},
    ['\''] = {0, HID_KEY_APOSTROPHE}, ['"'] = {S, HID_KEY_APOSTROPHE},
    ['`']  = {0, HID_KEY_GRAVE},      ['~'] = {S, HID_KEY_GRAVE},
    [',']  = {0, HID_KEY_COMMA},      ['<'] = {S, HID_KEY_COMMA},
    ['.']  = {0, HID_KEY_PERIOD},     ['>'] = {S, HID_KEY_PERIOD},
    ['/']  = {0, HID_KEY_SLASH},      ['?'] = {S, HID_KEY_SLASH},
};

#undef S

static bool us_lookup(uint32_t cp, hid_seq_t *out)
{
    if (cp >= 128) {
        return false;
    }
    const hid_step_t step = us_ascii[cp];
    if (step.key == HID_KEY_NONE) {
        return false;
    }
    out->steps[0] = step;
    out->n        = 1;
    return true;
}

const keymap_layout_t keymap_us = {
    .name   = "us",
    .lookup = us_lookup,
};
