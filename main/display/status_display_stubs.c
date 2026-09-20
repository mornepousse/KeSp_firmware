/*
 * status_display_* stubs — keyboard role WITHOUT screen (Niphargus left,
 * KASE_SPLIT_MASTER: CONFIG_KASE_HAS_DISPLAY=n via its Kconfig override,
 * see main/Kconfig.projbuild).
 *
 * status_display_* calls are scattered across the keystroke path
 * (main/input/matrix_scan.c, main/input/keyboard_actions.c) and in
 * main/comm/cdc/cdc_binary_cmds.c, without a CONFIG_KASE_HAS_DISPLAY gate —
 * these three files are compiled for every KEYBOARD role (or for all
 * roles), screen or not. Sprinkling #if CONFIG_KASE_HAS_DISPLAY there
 * would leave permanent noise in hot code for a single board case. This
 * stub file centralizes the problem instead: it can be removed as a block
 * the day the display architecture changes (or if Niphargus left gains a
 * screen).
 *
 * Precedent in this repo: main/comm/cdc/cdc_dongle_stubs.c already stubs
 * status_display_update_layer_name() for CONFIG_KASE_DEVICE_ROLE_DONGLE,
 * for the same reason. KEYBOARD and DONGLE roles are mutually exclusive
 * (choice KASE_DEVICE_ROLE in main/Kconfig.projbuild): the two stub
 * files are never compiled together, so no double
 * definition is possible.
 *
 * Compiled only when CONFIG_KASE_DEVICE_ROLE_KEYBOARD &&
 * !CONFIG_KASE_HAS_DISPLAY (main/CMakeLists.txt). Silent stubs:
 * status_display_notify_keypress() is called on every keystroke from
 * matrix_scan.c (hot path) — no ESP_LOGx here.
 */

void status_display_update_layer_name(void) { }
void status_display_notify_keypress(void) { }
void status_display_refresh_all(void) { }
void status_display_notify_display_key(void) { }
