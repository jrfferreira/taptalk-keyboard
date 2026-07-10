# Pure C. Deliberately has no ESP-IDF dependencies (REQUIRES is empty), which
# is what lets test/host compile these same sources natively on a laptop.
idf_component_register(
    SRCS "wav.c" "textnorm.c" "keymap.c" "keymap_us.c" "keymap_uk.c" "keymap_usintl.c"
         "keymap_abnt2.c" "keymap_pt.c" "keymap_es.c" "keymap_esla.c" "keymap_fr.c"
         "keymap_de.c" "keymap_it.c"
         "multipart.c" "sm.c" "formdec.c" "dnsreply.c" "jsonesc.c" "typeplan.c"
    INCLUDE_DIRS "include"
)
