#include "core/keymap.h"

#include <string.h>

#include "core/textnorm.h"

keymap_result_t keymap_resolve(const keymap_layout_t *layout, uint32_t codepoint, hid_seq_t *out)
{
    memset(out, 0, sizeof(*out));
    if (layout == NULL || layout->lookup == NULL) {
        return KEYMAP_SKIPPED;
    }

    if (layout->lookup(codepoint, out)) {
        return KEYMAP_EXACT;
    }

    const uint32_t base = textnorm_deaccent(codepoint);
    if (base != 0 && layout->lookup(base, out)) {
        return KEYMAP_DEACCENTED;
    }

    memset(out, 0, sizeof(*out));
    return KEYMAP_SKIPPED;
}
