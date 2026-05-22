/*
 * Layout names for kase_dongle.
 * 2D char array consumed by status display / controller app.
 * Sized [LAYERS][MAX_LAYOUT_NAME_LENGTH] (declared extern in keyboard_config.h).
 */

#include "keyboard_config.h"

char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "BASE",
    "LOWER",
    "RAISE",
    "L3",
    "L4",
    "L5",
    "L6",
    "L7",
    "L8",
    "L9",
};

/* Physical key layout JSON (consumed by the controller app for remap UI). */
#define _STR(x) #x
#define STR(x)  _STR(x)

#include "../kase_dongle_layout.inc"
