/* Advanced key features implementation */
#include "key_features.h"
#include "key_definitions.h"
#include <string.h>

/* ── One-Shot Modifier ───────────────────────────────────────────── */

static uint8_t osm_pending_mods = 0;

void osm_arm(uint8_t mod_mask)   { osm_pending_mods |= mod_mask; }
bool osm_is_active(void)         { return osm_pending_mods != 0; }

uint8_t osm_consume(void)
{
    uint8_t mods = osm_pending_mods;
    osm_pending_mods = 0;
    return mods;
}

/* ── One-Shot Layer ──────────────────────────────────────────────── */

static int8_t osl_pending_layer = -1;

void osl_arm(uint8_t layer)      { osl_pending_layer = (int8_t)layer; }
int8_t osl_get_layer(void)       { return osl_pending_layer; }

void osl_consume(void)
{
    osl_pending_layer = -1;
}

/* ── Caps Word ───────────────────────────────────────────────────── */

static bool caps_word_active = false;

void caps_word_toggle(void)
{
    caps_word_active = !caps_word_active;
}

bool caps_word_is_active(void)
{
    return caps_word_active;
}

void caps_word_process(uint8_t *keycode, uint8_t *modifier)
{
    if (!caps_word_active || *keycode == 0) return;

    /* Letters A-Z (HID 0x04-0x1D): add shift */
    if (*keycode >= 0x04 && *keycode <= 0x1D) {
        *modifier |= MOD_LSFT;
        return;
    }

    /* Numbers, minus, underscore: keep caps word active but don't shift */
    if ((*keycode >= 0x1E && *keycode <= 0x27) || /* 1-0 */
        *keycode == 0x2D ||                         /* - */
        *keycode == 0x2E) {                         /* = */
        return;
    }

    /* Backspace: keep active (allow corrections) */
    if (*keycode == 0x2A) return;

    /* Any other key: deactivate caps word */
    caps_word_active = false;
}

/* ── Repeat Key ──────────────────────────────────────────────────── */

static uint8_t last_keycode = 0;

void repeat_key_record(uint8_t keycode)
{
    if (keycode != 0 && keycode < HID_KEY_CONTROL_LEFT)
        last_keycode = keycode;
}

uint8_t repeat_key_get(void)
{
    return last_keycode;
}
