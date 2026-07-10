#include "core/keymap.h"

#include <string.h>

#include "core/textnorm.h"

bool keymap_table_lookup(const keymap_entry_t *entries, size_t count, uint32_t cp, hid_seq_t *out)
{
    for (size_t i = 0; i < count; i++) {
        if (entries[i].cp == cp) {
            *out = entries[i].seq;
            return true;
        }
    }
    return false;
}

const keymap_layout_t *const keymap_layouts[] = {
    &keymap_us,  &keymap_uk, &keymap_usintl, &keymap_abnt2, &keymap_pt,
    &keymap_es,  &keymap_esla, &keymap_fr,   &keymap_de,    &keymap_it,
};
const size_t keymap_layouts_count = sizeof(keymap_layouts) / sizeof(keymap_layouts[0]);

const keymap_layout_t *keymap_by_name(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < keymap_layouts_count; i++) {
        if (strcmp(keymap_layouts[i]->name, name) == 0) {
            return keymap_layouts[i];
        }
    }
    return NULL;
}

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
