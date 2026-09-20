/*
 * Stubs specific to the mouse role (Conchodytes).
 *
 * cdc_binary_cmds.c unconditionally compiles `bin_cmd_monitor`, which
 * reports the keystroke count and words per minute. These two figures
 * come from input/key_stats.c and input/key_features.c, which a mouse
 * does not compile — it has no keys.
 *
 * The mouse already shares cdc_niphar_slave_stubs.c for everything missing
 * on both sides (display, BLE, keymap engine). These two symbols cannot
 * go there: the Niphargus slave has a matrix and defines them for real,
 * which would give a duplicate at link time. Hence this separate file,
 * compiled for the mouse role only.
 *
 * Modeled on comm/cdc/cdc_dongle_stubs.c: we only provide the symbols the
 * linker actually requested, added one by one.
 */

#include <stdint.h>

/* No keystroke is ever counted on a mouse. The CDC commands that
 * read this value will report zero — which is the truth, not a
 * stub that lies. */
uint32_t key_stats_total = 0;

/* Same here: no words per minute without keys. Same choice as the dongle,
 * which returns 0 from comm/rf/dongle_state.c. */
uint16_t wpm_get(void) { return 0; }
