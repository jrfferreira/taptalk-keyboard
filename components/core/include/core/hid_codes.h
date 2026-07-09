/* USB HID Usage Table (page 0x07, keyboard) — the subset we emit.
 * Values are from the USB HID Usage Tables spec, not invented. */
#pragma once

#include <stdint.h>

/* Modifier bitmask, byte 0 of a boot-keyboard report. */
enum {
    HID_MOD_NONE   = 0x00,
    HID_MOD_LCTRL  = 0x01,
    HID_MOD_LSHIFT = 0x02,
    HID_MOD_LALT   = 0x04,
    HID_MOD_LGUI   = 0x08,
    HID_MOD_RCTRL  = 0x10,
    HID_MOD_RSHIFT = 0x20,
    HID_MOD_RALT   = 0x40, /* AltGr */
    HID_MOD_RGUI   = 0x80,
};

/* Usage IDs. HID_KEY_NONE in a report slot means "no key". */
enum {
    HID_KEY_NONE       = 0x00,

    HID_KEY_A          = 0x04, /* ..0x1D == Z, contiguous */
    HID_KEY_Z          = 0x1D,

    HID_KEY_1          = 0x1E, /* ..0x26 == 9, contiguous */
    HID_KEY_9          = 0x26,
    HID_KEY_0          = 0x27, /* zero sits after nine, not before one */

    HID_KEY_ENTER      = 0x28,
    HID_KEY_ESCAPE     = 0x29,
    HID_KEY_BACKSPACE  = 0x2A,
    HID_KEY_TAB        = 0x2B,
    HID_KEY_SPACE      = 0x2C,
    HID_KEY_MINUS      = 0x2D,
    HID_KEY_EQUAL      = 0x2E,
    HID_KEY_LBRACKET   = 0x2F,
    HID_KEY_RBRACKET   = 0x30,
    HID_KEY_BACKSLASH  = 0x31,
    HID_KEY_NONUS_HASH = 0x32,
    HID_KEY_SEMICOLON  = 0x33,
    HID_KEY_APOSTROPHE = 0x34,
    HID_KEY_GRAVE      = 0x35,
    HID_KEY_COMMA      = 0x36,
    HID_KEY_PERIOD     = 0x37,
    HID_KEY_SLASH      = 0x38,
    HID_KEY_CAPSLOCK   = 0x39,

    /* Needed by non-US layouts. Listed so a future layout table can use them
     * without reaching for magic numbers. */
    HID_KEY_NONUS_BSLASH = 0x64,
    HID_KEY_INTL1        = 0x87, /* ABNT2 '/?°' key */
    HID_KEY_INTL2        = 0x88,
    HID_KEY_INTL3        = 0x89,
    HID_KEY_KP_COMMA     = 0x85, /* ABNT2 numpad '.' */
};

/* One physical keypress: hold `mod`, strike `key`. */
typedef struct {
    uint8_t mod;
    uint8_t key;
} hid_step_t;
