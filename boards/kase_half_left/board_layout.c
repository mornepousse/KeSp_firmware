/*
 * Layout names for kase_half_left.
 * board_layout_json[] is provided by cdc_half_stubs.c (compiled when
 * CONFIG_KASE_DEVICE_ROLE_HALF=y). This file only provides default_layout_names[].
 */

#include "keyboard_config.h"

/* Matches dongle convention — half never uses layout names locally. */
char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "Base",
    "L1", "L2", "L3", "L4", "L5", "L6", "L7", "L8", "L9",
};
