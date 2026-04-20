/* Advanced key features implementation */
#include "key_features.h"
#include "key_definitions.h"
#include "keyboard_config.h"
#include "nvs_utils.h"
#include "esp_log.h"
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

/* ── Grave Escape ───────────────────────────────────────────────── */

uint8_t grave_esc_resolve(uint8_t active_mods)
{
    /* If Shift or GUI is held, send grave (`) instead of ESC */
    if (active_mods & (MOD_LSFT | MOD_RSFT | MOD_LGUI | MOD_RGUI))
        return 0x35; /* HID_KEY_GRAVE */
    return 0x29; /* HID_KEY_ESCAPE */
}

/* ── Layer Lock ─────────────────────────────────────────────────── */

static int8_t locked_layer = -1;

void layer_lock_toggle(void)
{
    extern uint8_t current_layout;
    extern uint8_t last_layer;
    extern void layer_changed(void);

    if (locked_layer >= 0) {
        /* Unlock: return to base layer */
        locked_layer = -1;
        current_layout = 0;
        last_layer = 0;
        layer_changed();
    } else if (current_layout != 0) {
        /* Lock: keep current MO layer active */
        locked_layer = (int8_t)current_layout;
        last_layer = current_layout;
        layer_changed();
    }
}

bool layer_lock_is_locked(void)  { return locked_layer >= 0; }
int8_t layer_lock_get(void)      { return locked_layer; }

/* ── WPM Counter ────────────────────────────────────────────────── */

#define WPM_WINDOW_SIZE 10   /* samples in rolling window */
#define WPM_SAMPLE_MS   1000 /* 1 second per sample */
#define WPM_CHARS_PER_WORD 5

static uint16_t wpm_counts[WPM_WINDOW_SIZE] = {0};
static uint8_t wpm_index = 0;
static uint16_t wpm_current_count = 0;

void wpm_record_keypress(void)
{
    wpm_current_count++;
}

void wpm_tick(void)
{
    /* Called every ~1 second — store current count and advance window */
    wpm_counts[wpm_index] = wpm_current_count;
    wpm_index = (wpm_index + 1) % WPM_WINDOW_SIZE;
    wpm_current_count = 0;
}

uint16_t wpm_get(void)
{
    uint32_t total = 0;
    for (int i = 0; i < WPM_WINDOW_SIZE; i++)
        total += wpm_counts[i];
    /* total chars in WPM_WINDOW_SIZE seconds → WPM */
    return (uint16_t)(total * 60 / (WPM_WINDOW_SIZE * WPM_CHARS_PER_WORD));
}

/* ── Double-Tap Shift → Caps Lock ──────────────────────────────── */

#define SHIFT_DTAP_TIMEOUT_TICKS  20  /* 20 × 10ms = 200ms window */

static uint8_t sdt_ticks = 0;
static bool    sdt_first_released = false;  /* first tap released, window active */
static bool    sdt_first_pressed = false;   /* first tap press seen, not yet released */
static bool    sdt_caps_pending = false;

bool shift_double_tap_press(void)
{
    /* If first tap was released and we're still in the window → this is tap 2 */
    if (sdt_first_released) {
        sdt_first_released = false;
        sdt_ticks = 0;
        sdt_caps_pending = true;
        return true;
    }
    /* Otherwise, this is tap 1 — track until release */
    sdt_first_pressed = true;
    return false;
}

void shift_double_tap_release(void)
{
    if (sdt_first_pressed) {
        sdt_first_pressed = false;
        sdt_first_released = true;
        sdt_ticks = 0;
    }
}

void shift_double_tap_tick(void)
{
    if (sdt_first_released) {
        sdt_ticks++;
        if (sdt_ticks >= SHIFT_DTAP_TIMEOUT_TICKS)
            sdt_first_released = false;
    }
}

bool shift_double_tap_consume(void)
{
    if (!sdt_caps_pending) return false;
    sdt_caps_pending = false;
    return true;
}

/* ── Key Override / Mod-Morph ───────────────────────────────────── */

static key_override_t overrides[KEY_OVERRIDE_MAX_SLOTS];

void key_override_init(void) {
    memset(overrides, 0, sizeof(overrides));
}

void key_override_set(uint8_t index, const key_override_t *cfg) {
    if (index < KEY_OVERRIDE_MAX_SLOTS)
        overrides[index] = *cfg;
}

const key_override_t *key_override_get(uint8_t index) {
    if (index >= KEY_OVERRIDE_MAX_SLOTS) return NULL;
    return &overrides[index];
}

uint8_t key_override_check(uint8_t keycode, uint8_t active_mods, uint8_t *out_mod)
{
    for (int i = 0; i < KEY_OVERRIDE_MAX_SLOTS; i++) {
        if (overrides[i].trigger_key == 0) continue;
        if (overrides[i].trigger_key == keycode &&
            (active_mods & overrides[i].trigger_mod) == overrides[i].trigger_mod) {
            if (out_mod) *out_mod = overrides[i].result_mod;
            return overrides[i].result_key;
        }
    }
    return 0;
}

void key_override_save(void) {
    uint8_t count = 0;
    for (int i = 0; i < KEY_OVERRIDE_MAX_SLOTS; i++)
        if (overrides[i].trigger_key != 0) count = i + 1;
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "ko_cfg", overrides,
                                              sizeof(overrides), "ko_cnt", count);
    if (err != ESP_OK) ESP_LOGE("KEY_OVERRIDE", "Save failed: %s", esp_err_to_name(err));
}

void key_override_load(void) {
    uint32_t count = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "ko_cfg", overrides,
                              sizeof(overrides), "ko_cnt", &count);
}

/* ── Tri-Layer ──────────────────────────────────────────────────── */

static uint8_t tri_l1 = 0, tri_l2 = 0, tri_result = 0;

void tri_layer_set(uint8_t layer1, uint8_t layer2, uint8_t result_layer)
{
    tri_l1 = layer1;
    tri_l2 = layer2;
    tri_result = result_layer;
}

int8_t tri_layer_check(uint8_t active_layer, uint8_t last_layer)
{
    if (tri_result == 0) return -1; /* disabled */
    /* If both trigger layers are "involved" (one active, one as last), activate result */
    if ((active_layer == tri_l1 && last_layer == tri_l2) ||
        (active_layer == tri_l2 && last_layer == tri_l1))
        return (int8_t)tri_result;
    return -1;
}
