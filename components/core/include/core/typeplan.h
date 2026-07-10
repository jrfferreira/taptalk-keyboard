/* Codepoint -> the exact sequence of HID reports to put on the wire.
 *
 * HID is state-based: the host reacts to a report *changing*. Two identical
 * consecutive reports read as ONE keypress, so "aa" typed without an
 * intervening all-zero release report arrives as "a". That failure looks like
 * flaky hardware, not a bug, which is why this lives here and is tested rather
 * than being three lines inside a FreeRTOS task. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "core/keymap.h"

/* A dead-key composition is 2 steps, and every step is press + release. */
#define TYPEPLAN_MAX_FRAMES (KEYMAP_MAX_STEPS * 2)

typedef struct {
    hid_step_t frames[TYPEPLAN_MAX_FRAMES];
    uint8_t n; /* 0 when the codepoint cannot be typed */
} hid_frames_t;

/* Fills `out` with the report sequence for `codepoint` on `layout`, applying
 * the same fallback ladder as keymap_resolve(): exact, then de-accented, then
 * give up. Every press frame is followed by a release frame. */
keymap_result_t typeplan_char(const keymap_layout_t *layout, uint32_t codepoint,
                              hid_frames_t *out);

/* True for an all-zero (release) frame. */
bool typeplan_is_release(const hid_step_t *frame);
