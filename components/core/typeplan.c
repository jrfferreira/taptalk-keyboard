#include "core/typeplan.h"

#include <string.h>

bool typeplan_is_release(const hid_step_t *frame)
{
    return frame->mod == HID_MOD_NONE && frame->key == HID_KEY_NONE;
}

keymap_result_t typeplan_char(const keymap_layout_t *layout, uint32_t codepoint, hid_frames_t *out)
{
    memset(out, 0, sizeof(*out));

    hid_seq_t seq;
    const keymap_result_t r = keymap_resolve(layout, codepoint, &seq);
    if (r == KEYMAP_SKIPPED) {
        return r;
    }

    for (uint8_t i = 0; i < seq.n; i++) {
        out->frames[out->n++] = seq.steps[i];
        /* The release. Not optional, and not merely tidiness: without it the
         * host sees no state change on the next identical press. */
        out->frames[out->n++] = (hid_step_t){HID_MOD_NONE, HID_KEY_NONE};
    }
    return r;
}
