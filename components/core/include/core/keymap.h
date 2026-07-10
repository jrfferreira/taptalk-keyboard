/* Codepoint -> HID keystroke sequence.
 *
 * A character maps to a *sequence* of steps, not one step, because dead-key
 * layouts compose accents from two presses: on ABNT2, 'a' with an acute is
 * the dead-acute key followed by 'a'. Modelling this as a single
 * (modifier, keycode) pair cannot express it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/hid_codes.h"

/* Two is enough for every dead-key composition we intend to support.
 * A layout needing more must raise this and the tests must follow. */
#define KEYMAP_MAX_STEPS 2

typedef struct {
    hid_step_t steps[KEYMAP_MAX_STEPS];
    uint8_t n; /* 0 == this layout cannot produce the codepoint */
} hid_seq_t;

typedef struct {
    const char *name;
    /* Exact lookup only. No fallback logic here — that lives in
     * keymap_resolve() so every layout gets identical fallback behaviour. */
    bool (*lookup)(uint32_t codepoint, hid_seq_t *out);
} keymap_layout_t;

/* Storage for table-driven layouts: (codepoint -> sequence) entries, scanned
 * linearly. Order is free — no sortedness to silently get wrong — and speed
 * is irrelevant: every emitted frame already sleeps for milliseconds. */
typedef struct {
    uint32_t cp;
    hid_seq_t seq;
} keymap_entry_t;

bool keymap_table_lookup(const keymap_entry_t *entries, size_t count, uint32_t cp,
                         hid_seq_t *out);

extern const keymap_layout_t keymap_us;    /* US ANSI */
extern const keymap_layout_t keymap_abnt2; /* Portuguese (Brazil), ABNT2 */
extern const keymap_layout_t keymap_pt;    /* Portuguese (Portugal), ISO */

/* Every built-in layout, for tests and UIs that enumerate them. */
extern const keymap_layout_t *const keymap_layouts[];
extern const size_t keymap_layouts_count;

/* The layout whose name matches the stored settings token ("us", "abnt2",
 * ...), or NULL. Exact match only: the token is ours, so a near-miss is a bug
 * upstream, not something to paper over here. */
const keymap_layout_t *keymap_by_name(const char *name);

typedef enum {
    KEYMAP_EXACT,      /* layout produces the codepoint directly */
    KEYMAP_DEACCENTED, /* produced its ASCII base instead (e.g. 'á' -> 'a') */
    KEYMAP_SKIPPED,    /* cannot be typed; caller should drop it */
} keymap_result_t;

/* Resolve `codepoint` against `layout`, applying the shared fallback ladder:
 *   1. exact match in the layout
 *   2. strip the accent and retry ('á' -> 'a')
 *   3. give up
 *
 * On KEYMAP_SKIPPED, *out is zeroed (out->n == 0).
 *
 * We never substitute a placeholder such as '?'. A missing character is a
 * hole the reader can see; a wrong character is a lie they cannot. */
keymap_result_t keymap_resolve(const keymap_layout_t *layout, uint32_t codepoint, hid_seq_t *out);
