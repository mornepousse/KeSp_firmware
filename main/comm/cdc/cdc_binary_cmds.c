/* Binary command handlers for all KaSe CDC commands */
#include "cdc_binary_cmds.h"
#include "cdc_internal.h"
#include "keyboard_config.h"
#include "keyboard_task.h"
#include "keymap.h"
#include "key_stats.h"
#include "key_definitions.h"
#include "dfu_manager.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "tama_engine.h"
#include "key_features.h"
#include "hid_bluetooth_manager.h"
#include "status_display.h"
#include "version.h"
#include "esp_app_desc.h"

#if BOARD_HAS_POSITION_MAP
#include "board_position_map.h"
#endif

/* ── System ─────────────────────────────────────────────────────── */

static void bin_cmd_ping(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    ks_respond_ok(cmd);
}

static void bin_cmd_version(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    const char *ver = esp_app_get_description()->version;
    ks_respond(cmd, KS_STATUS_OK, (const uint8_t *)ver, (uint16_t)strlen(ver));
}

static void bin_cmd_features(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    static const char feat[] = "MT,LT,OSM,OSL,CAPS_WORD,REPEAT,TAP_DANCE,COMBO,LEADER,GESC,LAYER_LOCK,WPM,TRI_LAYER";
    ks_respond(cmd, KS_STATUS_OK, (const uint8_t *)feat, (uint16_t)strlen(feat));
}

static void bin_cmd_dfu(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    ks_respond_ok(cmd);
    vTaskDelay(pdMS_TO_TICKS(100));
    reboot_to_dfu();
}

/* ── Keymap ─────────────────────────────────────────────────────── */

/* LAYER_INDEX: no payload → response: [current_layer:u8] */
static void bin_cmd_layer_index(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t layer = (uint8_t)current_layout;
    ks_respond(cmd, KS_STATUS_OK, &layer, 1);
}

/* KEYMAP_CURRENT: no payload → returns keymap of active layer */
/* KEYMAP_GET: payload [layer:u8] → returns keymap of that layer */
static void bin_cmd_keymap_get(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    uint8_t layer;
    if (cmd == KS_CMD_KEYMAP_CURRENT)
        layer = (uint8_t)current_layout;
    else if (l >= 1)
        layer = p[0];
    else {
        ks_respond_err(cmd, KS_STATUS_ERR_INVALID);
        return;
    }

    if (layer >= LAYERS) {
        ks_respond_err(cmd, KS_STATUS_ERR_RANGE);
        return;
    }

    uint16_t total = 1 + MATRIX_ROWS * MATRIX_COLS * 2; /* layer_idx + keycodes */
    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&layer, 1);

    for (int r = 0; r < MATRIX_ROWS; r++) {
        for (int c = 0; c < MATRIX_COLS; c++) {
#if BOARD_HAS_POSITION_MAP
            int ir, ic;
            v2_to_v1_pos(r, c, &ir, &ic);
            uint16_t code = keymaps[layer][ir][ic];
#else
            uint16_t code = keymaps[layer][r][c];
#endif
            uint8_t b[2];
            pack_u16_le(b, code);
            ks_respond_write(b, 2);
        }
    }
    ks_respond_end();
}

/* SETKEY: payload [layer:u8][row:u8][col:u8][value:u16 LE] */
static void bin_cmd_setkey(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 5) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t layer = p[0], row = p[1], col = p[2];
    uint16_t value = p[3] | ((uint16_t)p[4] << 8);

    if (layer >= LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        ks_respond_err(cmd, KS_STATUS_ERR_RANGE);
        return;
    }

#if BOARD_HAS_POSITION_MAP
    int ir, ic;
    v2_to_v1_pos(row, col, &ir, &ic);
#else
    int ir = row, ic = col;
#endif
    keymaps[layer][ir][ic] = value;
    save_keymaps((uint16_t *)keymaps,
                 (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
    ks_respond_ok(cmd);
}

/* SETLAYER: payload [layer:u8][keycodes: ROWS*COLS * u16 LE] */
static void bin_cmd_setlayer(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    size_t expected = 1 + MATRIX_ROWS * MATRIX_COLS * 2;
    if (l < expected) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }

    uint8_t layer = p[0];
    if (layer >= LAYERS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }

    const uint8_t *data = p + 1;
    for (int r = 0; r < MATRIX_ROWS; r++) {
        for (int c = 0; c < MATRIX_COLS; c++) {
            size_t off = (r * MATRIX_COLS + c) * 2;
            uint16_t val = data[off] | ((uint16_t)data[off + 1] << 8);
#if BOARD_HAS_POSITION_MAP
            int ir, ic;
            v2_to_v1_pos(r, c, &ir, &ic);
            keymaps[layer][ir][ic] = val;
#else
            keymaps[layer][r][c] = val;
#endif
        }
    }
    save_keymaps((uint16_t *)keymaps,
                 (size_t)LAYERS * MATRIX_ROWS * MATRIX_COLS * sizeof(uint16_t));
    if (layer == current_layout)
        status_display_update_layer_name();
    ks_respond_ok(cmd);
}

/* LAYER_NAME: payload [layer:u8] → response [layer:u8][name bytes] */
static void bin_cmd_layer_name(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 1) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t layer = p[0];
    if (layer >= LAYERS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }

    const char *name = default_layout_names[layer];
    uint16_t nlen = (uint16_t)strlen(name);
    ks_respond_begin(cmd, KS_STATUS_OK, 1 + nlen);
    ks_respond_write(&layer, 1);
    ks_respond_write((const uint8_t *)name, nlen);
    ks_respond_end();
}

/* ── Layout names ───────────────────────────────────────────────── */

/* LIST_LAYOUTS: → [count:u8][{idx:u8, name_len:u8, name[]}...] */
static void bin_cmd_list_layouts(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint16_t total = 1;
    for (int i = 0; i < LAYERS; i++)
        total += 2 + (uint16_t)strlen(default_layout_names[i]);

    ks_respond_begin(cmd, KS_STATUS_OK, total);
    uint8_t count = (uint8_t)LAYERS;
    ks_respond_write(&count, 1);

    for (int i = 0; i < LAYERS; i++) {
        uint8_t idx = (uint8_t)i;
        const char *name = default_layout_names[i];
        uint8_t nlen = (uint8_t)strlen(name);
        ks_respond_write(&idx, 1);
        ks_respond_write(&nlen, 1);
        ks_respond_write((const uint8_t *)name, nlen);
    }
    ks_respond_end();
}

/* SET_LAYOUT_NAME: payload [layer:u8][name bytes...] */
static void bin_cmd_set_layout_name(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 2) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t layer = p[0];
    if (layer >= LAYERS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }

    uint16_t nlen = l - 1;
    if (nlen >= MAX_LAYOUT_NAME_LENGTH) nlen = MAX_LAYOUT_NAME_LENGTH - 1;
    memcpy(default_layout_names[layer], p + 1, nlen);
    default_layout_names[layer][nlen] = '\0';
    save_layout_names(default_layout_names, LAYERS);

    if (layer == current_layout)
        status_display_update_layer_name();
    ks_respond_ok(cmd);
}

/* ── Bluetooth ──────────────────────────────────────────────────── */

/* BT_QUERY: → [slot:u8][init:u8][conn:u8][pairing:u8][{slot_info}...] */
static void bin_cmd_bt_query(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    /* Header: 4 bytes + per-slot: 1+1+6+name_len per slot */
    uint16_t total = 4;
    for (int i = 0; i < BT_MAX_DEVICES; i++) {
        total += 8; /* idx(1) + valid(1) + addr(6) */
        const bt_device_slot_t *s = bt_get_slot(i);
        total += (s && s->valid && s->name[0]) ? (uint16_t)strlen(s->name) : 0;
        total += 1; /* name_len */
    }

    ks_respond_begin(cmd, KS_STATUS_OK, total);
    uint8_t hdr[4] = {
        bt_get_active_slot(),
        hid_bluetooth_is_initialized() ? 1 : 0,
        hid_bluetooth_is_connected() ? 1 : 0,
        hid_bluetooth_is_pairing() ? 1 : 0,
    };
    ks_respond_write(hdr, 4);

    for (int i = 0; i < BT_MAX_DEVICES; i++) {
        uint8_t idx = (uint8_t)i;
        const bt_device_slot_t *s = bt_get_slot(i);
        uint8_t valid = (s && s->valid) ? 1 : 0;
        ks_respond_write(&idx, 1);
        ks_respond_write(&valid, 1);
        if (valid) {
            ks_respond_write(s->addr, 6);
            uint8_t nlen = s->name[0] ? (uint8_t)strlen(s->name) : 0;
            ks_respond_write(&nlen, 1);
            if (nlen) ks_respond_write((const uint8_t *)s->name, nlen);
        } else {
            uint8_t zeroes[7] = {0}; /* 6 addr + 0 name_len */
            ks_respond_write(zeroes, 7);
        }
    }
    ks_respond_end();
}

static void bin_cmd_bt_switch(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 1) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    if (p[0] >= BT_MAX_DEVICES) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    bt_switch_slot(p[0]);
    ks_respond_ok(cmd);
}

static void bin_cmd_bt_pair(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    bt_start_pairing();
    ks_respond_ok(cmd);
}

static void bin_cmd_bt_disconnect(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    bt_disconnect();
    ks_respond_ok(cmd);
}

static void bin_cmd_bt_next(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    bt_next_device();
    ks_respond_ok(cmd);
}

static void bin_cmd_bt_prev(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    bt_prev_device();
    ks_respond_ok(cmd);
}

/* ── Tamagotchi ─────────────────────────────────────────────────── */

/* TAMA_QUERY: → packed stats */
static void bin_cmd_tama_query(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    const tama2_stats_t *s = tama_engine_get_stats();
    uint8_t buf[24];
    buf[0] = tama_engine_is_enabled() ? 1 : 0;
    buf[1] = (uint8_t)tama_engine_get_state();
    pack_u16_le(buf + 2, s->hunger);
    pack_u16_le(buf + 4, s->happiness);
    pack_u16_le(buf + 6, s->energy);
    pack_u16_le(buf + 8, s->health);
    pack_u16_le(buf + 10, s->level);
    pack_u16_le(buf + 12, s->xp);
    pack_u32_le(buf + 14, s->total_keys);
    pack_u32_le(buf + 18, s->max_kpm);
    ks_respond(cmd, KS_STATUS_OK, buf, 22);
}

static void bin_cmd_tama_enable(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_set_enabled(true); ks_respond_ok(cmd); }

static void bin_cmd_tama_disable(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_set_enabled(false); ks_respond_ok(cmd); }

static void bin_cmd_tama_feed(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_action(TAMA2_ACTION_FEED); ks_respond_ok(cmd); }

static void bin_cmd_tama_play(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_action(TAMA2_ACTION_PLAY); ks_respond_ok(cmd); }

static void bin_cmd_tama_sleep(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_action(TAMA2_ACTION_SLEEP); ks_respond_ok(cmd); }

static void bin_cmd_tama_medicine(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_action(TAMA2_ACTION_MEDICINE); ks_respond_ok(cmd); }

static void bin_cmd_tama_save(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; tama_engine_save(); ks_respond_ok(cmd); }

/* ── Features ───────────────────────────────────────────────────── */

static void bin_cmd_autoshift(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    auto_shift_toggle();
    uint8_t state = auto_shift_is_enabled() ? 1 : 0;
    ks_respond(cmd, KS_STATUS_OK, &state, 1);
}

static void bin_cmd_wpm(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint16_t w = wpm_get();
    uint8_t buf[2];
    pack_u16_le(buf, w);
    ks_respond(cmd, KS_STATUS_OK, buf, 2);
}

/* TRILAYER: payload [l1:u8][l2:u8][result:u8] */
static void bin_cmd_trilayer(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 3) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    tri_layer_set(p[0], p[1], p[2]);
    ks_respond_ok(cmd);
}

/* ── Stats ──────────────────────────────────────────────────────── */

static void bin_cmd_keystats_reset(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; reset_key_stats(); ks_respond_ok(cmd); }

static void bin_cmd_bigrams_reset(uint8_t cmd, const uint8_t *p, uint16_t l)
{ (void)p; (void)l; reset_bigram_stats(); ks_respond_ok(cmd); }

/* KEYSTATS_BIN: → raw key_stats[MATRIX_ROWS][MATRIX_COLS] as u32 LE */
static void bin_cmd_keystats_bin(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint16_t total = 2 + MATRIX_ROWS * MATRIX_COLS * 4; /* rows,cols + data */
    ks_respond_begin(cmd, KS_STATUS_OK, total);
    uint8_t hdr[2] = { (uint8_t)MATRIX_ROWS, (uint8_t)MATRIX_COLS };
    ks_respond_write(hdr, 2);

    for (int r = 0; r < MATRIX_ROWS; r++) {
        for (int c = 0; c < MATRIX_COLS; c++) {
            uint8_t b[4];
            pack_u32_le(b, key_stats[r][c]);
            ks_respond_write(b, 4);
        }
    }
    ks_respond_end();
}

/* ── Tap Dance ──────────────────────────────────────────────────── */

/* TD_SET: payload [index:u8][a1:u8][a2:u8][a3:u8][a4:u8] */
static void bin_cmd_td_set(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 5) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    if (p[0] >= TAP_DANCE_MAX_SLOTS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    uint8_t actions[4] = { p[1], p[2], p[3], p[4] };
    tap_dance_set(p[0], actions);
    tap_dance_save();
    ks_respond_ok(cmd);
}

/* TD_LIST: → [count:u8][{idx:u8, a1,a2,a3,a4}...] */
static void bin_cmd_td_list(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t count = 0;
    for (int i = 0; i < TAP_DANCE_MAX_SLOTS; i++) {
        const tap_dance_config_t *td = tap_dance_get(i);
        if (td && (td->actions[0] || td->actions[1] || td->actions[2] || td->actions[3]))
            count++;
    }

    uint16_t total = 1 + count * 5;
    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&count, 1);

    for (int i = 0; i < TAP_DANCE_MAX_SLOTS; i++) {
        const tap_dance_config_t *td = tap_dance_get(i);
        if (!td || (!td->actions[0] && !td->actions[1] && !td->actions[2] && !td->actions[3]))
            continue;
        uint8_t entry[5] = { (uint8_t)i, td->actions[0], td->actions[1], td->actions[2], td->actions[3] };
        ks_respond_write(entry, 5);
    }
    ks_respond_end();
}

/* ── Combos ─────────────────────────────────────────────────────── */

/* COMBO_SET: payload [index:u8][r1:u8][c1:u8][r2:u8][c2:u8][result:u8] */
static void bin_cmd_combo_set(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 6) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    if (p[0] >= COMBO_MAX_SLOTS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    combo_config_t cfg = { .row1 = p[1], .col1 = p[2], .row2 = p[3], .col2 = p[4], .result = p[5] };
    combo_set(p[0], &cfg);
    combo_save();
    ks_respond_ok(cmd);
}

/* COMBO_LIST: → [count:u8][{idx:u8, r1,c1,r2,c2,result}...] */
static void bin_cmd_combo_list(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t count = 0;
    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        const combo_config_t *cb = combo_get(i);
        if (cb && cb->result) count++;
    }

    uint16_t total = 1 + count * 6;
    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&count, 1);

    for (int i = 0; i < COMBO_MAX_SLOTS; i++) {
        const combo_config_t *cb = combo_get(i);
        if (!cb || !cb->result) continue;
        uint8_t entry[6] = { (uint8_t)i, cb->row1, cb->col1, cb->row2, cb->col2, cb->result };
        ks_respond_write(entry, 6);
    }
    ks_respond_end();
}

/* ── Leader ─────────────────────────────────────────────────────── */

/* LEADER_SET: payload [index:u8][seq_len:u8][seq...][result:u8][result_mod:u8] */
static void bin_cmd_leader_set(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 4) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t idx = p[0];
    uint8_t seq_len = p[1];
    if (idx >= LEADER_MAX_ENTRIES || l < (uint16_t)(2 + seq_len + 2)) {
        ks_respond_err(cmd, KS_STATUS_ERR_RANGE);
        return;
    }
    leader_entry_t entry = {0};
    for (uint8_t i = 0; i < seq_len && i < LEADER_MAX_SEQ_LEN; i++)
        entry.sequence[i] = p[2 + i];
    entry.result = p[2 + seq_len];
    entry.result_mod = p[2 + seq_len + 1];
    leader_set(idx, &entry);
    leader_save();
    ks_respond_ok(cmd);
}

/* LEADER_LIST: → [count:u8][{idx:u8, seq_len:u8, seq..., result:u8, result_mod:u8}...] */
static void bin_cmd_leader_list(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t count = 0;
    uint16_t total = 1;
    for (int i = 0; i < LEADER_MAX_ENTRIES; i++) {
        const leader_entry_t *ld = leader_get(i);
        if (ld && ld->result) {
            count++;
            /* count seq length (terminated by 0) */
            uint8_t slen = 0;
            for (int j = 0; j < LEADER_MAX_SEQ_LEN && ld->sequence[j]; j++) slen++;
            total += 2 + slen + 2;
        }
    }

    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&count, 1);

    for (int i = 0; i < LEADER_MAX_ENTRIES; i++) {
        const leader_entry_t *ld = leader_get(i);
        if (!ld || !ld->result) continue;
        uint8_t slen = 0;
        for (int j = 0; j < LEADER_MAX_SEQ_LEN && ld->sequence[j]; j++) slen++;
        uint8_t hdr[2] = { (uint8_t)i, slen };
        ks_respond_write(hdr, 2);
        ks_respond_write(ld->sequence, slen);
        uint8_t tail[2] = { ld->result, ld->result_mod };
        ks_respond_write(tail, 2);
    }
    ks_respond_end();
}

/* ── Key Override ───────────────────────────────────────────────── */

/* KO_SET: payload [index:u8][trig_key:u8][trig_mod:u8][res_key:u8][res_mod:u8] */
static void bin_cmd_ko_set(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 5) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    if (p[0] >= KEY_OVERRIDE_MAX_SLOTS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    key_override_t ko = { .trigger_key = p[1], .trigger_mod = p[2],
                           .result_key = p[3], .result_mod = p[4] };
    key_override_set(p[0], &ko);
    key_override_save();
    ks_respond_ok(cmd);
}

/* KO_LIST: → [count:u8][{idx:u8, trig_key, trig_mod, res_key, res_mod}...] */
static void bin_cmd_ko_list(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t count = 0;
    for (int i = 0; i < KEY_OVERRIDE_MAX_SLOTS; i++) {
        const key_override_t *ko = key_override_get(i);
        if (ko && ko->trigger_key) count++;
    }

    uint16_t total = 1 + count * 5;
    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&count, 1);

    for (int i = 0; i < KEY_OVERRIDE_MAX_SLOTS; i++) {
        const key_override_t *ko = key_override_get(i);
        if (!ko || !ko->trigger_key) continue;
        uint8_t entry[5] = { (uint8_t)i, ko->trigger_key, ko->trigger_mod,
                              ko->result_key, ko->result_mod };
        ks_respond_write(entry, 5);
    }
    ks_respond_end();
}

/* ── Macros ─────────────────────────────────────────────────────── */

static uint16_t macro_kc_from_index(size_t idx)
{
    return (uint16_t)(MACRO_1 + (idx * 0x0100));
}

/* LIST_MACROS: → [count:u8][{idx:u8, kc:u16 LE, name_len:u8, name[], keys_len:u8, keys[],
 *                            step_count:u8, steps[]{kc:u8,mod:u8}}...] */
static void bin_cmd_list_macros(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    /* Pre-compute total size */
    uint8_t count = 0;
    uint16_t total = 1; /* count byte */
    for (int i = 0; i < MAX_MACROS; i++) {
        if (macros_list[i].name[0] == '\0') continue;
        count++;
        uint8_t nlen = (uint8_t)strlen(macros_list[i].name);
        uint8_t klen = 0;
        for (int k = 0; k < 6; k++) if (macros_list[i].keys[k]) klen++;
        uint8_t scount = 0;
        for (int s = 0; s < MACRO_MAX_STEPS; s++) if (macros_list[i].steps[s].keycode) scount++;
        total += 1 + 2 + 1 + nlen + 1 + klen + 1 + scount * 2; /* idx+kc+nlen+name+klen+keys+scount+steps */
    }

    ks_respond_begin(cmd, KS_STATUS_OK, total);
    ks_respond_write(&count, 1);

    for (int i = 0; i < MAX_MACROS; i++) {
        if (macros_list[i].name[0] == '\0') continue;
        uint8_t idx = (uint8_t)i;
        uint16_t kc = macro_kc_from_index(i);
        uint8_t kc_bytes[2]; pack_u16_le(kc_bytes, kc);
        uint8_t nlen = (uint8_t)strlen(macros_list[i].name);
        ks_respond_write(&idx, 1);
        ks_respond_write(kc_bytes, 2);
        ks_respond_write(&nlen, 1);
        ks_respond_write((const uint8_t *)macros_list[i].name, nlen);

        /* Legacy keys */
        uint8_t klen = 0;
        uint8_t keys_compact[6];
        for (int k = 0; k < 6; k++)
            if (macros_list[i].keys[k]) keys_compact[klen++] = macros_list[i].keys[k];
        ks_respond_write(&klen, 1);
        if (klen) ks_respond_write(keys_compact, klen);

        /* Sequence steps */
        uint8_t scount = 0;
        for (int s = 0; s < MACRO_MAX_STEPS; s++)
            if (macros_list[i].steps[s].keycode) scount++;
        ks_respond_write(&scount, 1);
        for (int s = 0; s < MACRO_MAX_STEPS && macros_list[i].steps[s].keycode; s++) {
            uint8_t step[2] = { macros_list[i].steps[s].keycode, macros_list[i].steps[s].modifier };
            ks_respond_write(step, 2);
        }
    }
    ks_respond_end();
}

/* MACRO_ADD: payload [slot:u8][name_len:u8][name...][keys: 6 bytes] */
static void bin_cmd_macro_add(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 9) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; } /* min: slot+nlen+1char+6keys */
    uint8_t slot = p[0];
    if (slot >= MAX_MACROS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    uint8_t nlen = p[1];
    if (l < (uint16_t)(2 + nlen + 6)) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }

    if (nlen >= MAX_MACRO_NAME_LENGTH) nlen = MAX_MACRO_NAME_LENGTH - 1;
    memcpy(macros_list[slot].name, p + 2, nlen);
    macros_list[slot].name[nlen] = '\0';
    memcpy(macros_list[slot].keys, p + 2 + nlen, 6);
    memset(macros_list[slot].steps, 0, sizeof(macros_list[slot].steps));
    macros_list[slot].key_definition = macro_kc_from_index(slot);
    if ((size_t)(slot + 1) > macros_count) macros_count = slot + 1;
    save_macros(macros_list, macros_count);
    ks_respond_ok(cmd);
}

/* MACRO_ADD_SEQ: payload [slot:u8][name_len:u8][name...][step_count:u8][steps: count*2 (kc,mod)] */
static void bin_cmd_macro_add_seq(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 4) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t slot = p[0];
    if (slot >= MAX_MACROS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    uint8_t nlen = p[1];
    if (l < (uint16_t)(2 + nlen + 1)) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }

    uint8_t step_count = p[2 + nlen];
    if (step_count > MACRO_MAX_STEPS) step_count = MACRO_MAX_STEPS;
    if (l < (uint16_t)(2 + nlen + 1 + step_count * 2)) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }

    if (nlen >= MAX_MACRO_NAME_LENGTH) nlen = MAX_MACRO_NAME_LENGTH - 1;
    memcpy(macros_list[slot].name, p + 2, nlen);
    macros_list[slot].name[nlen] = '\0';
    memset(macros_list[slot].keys, 0, sizeof(macros_list[slot].keys));
    memset(macros_list[slot].steps, 0, sizeof(macros_list[slot].steps));

    const uint8_t *steps_data = p + 2 + nlen + 1;
    for (uint8_t i = 0; i < step_count; i++) {
        macros_list[slot].steps[i].keycode = steps_data[i * 2];
        macros_list[slot].steps[i].modifier = steps_data[i * 2 + 1];
    }

    macros_list[slot].key_definition = macro_kc_from_index(slot);
    if ((size_t)(slot + 1) > macros_count) macros_count = slot + 1;
    save_macros(macros_list, macros_count);
    ks_respond_ok(cmd);
}

/* MACRO_DELETE: payload [slot:u8] */
static void bin_cmd_macro_delete(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 1) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    uint8_t slot = p[0];
    if (slot >= MAX_MACROS) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    macros_list[slot].name[0] = '\0';
    memset(macros_list[slot].keys, 0, sizeof(macros_list[slot].keys));
    memset(macros_list[slot].steps, 0, sizeof(macros_list[slot].steps));
    macros_list[slot].key_definition = macro_kc_from_index(slot);
    recalc_macros_count();
    save_macros(macros_list, macros_count);
    ks_respond_ok(cmd);
}

/* ── Bigrams ────────────────────────────────────────────────────── */

/* BIGRAMS_BIN: → [module_id:u8][num_keys:u8][total:u32 LE][max:u16 LE][n_entries:u16 LE]
 *               [{prev:u8, curr:u8, count:u16 LE}...] (top 256, sorted desc) */
static void bin_cmd_bigrams_bin(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    typedef struct { uint8_t prev; uint8_t curr; uint16_t count; } bg_t;
    #define BG_MAX 256
    bg_t *entries = malloc(BG_MAX * sizeof(bg_t));
    if (!entries) { ks_respond_err(cmd, KS_STATUS_ERR_BUSY); return; }

    uint16_t n = 0;
    uint16_t min_in = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        for (int j = 0; j < NUM_KEYS; j++) {
            if (bigram_stats[i][j] == 0) continue;
            if (n < BG_MAX) {
                entries[n].prev = (uint8_t)i; entries[n].curr = (uint8_t)j;
                entries[n].count = bigram_stats[i][j]; n++;
                if (bigram_stats[i][j] < min_in || min_in == 0) min_in = bigram_stats[i][j];
            } else if (bigram_stats[i][j] > min_in) {
                uint16_t mi = 0;
                for (uint16_t k = 1; k < BG_MAX; k++) if (entries[k].count < entries[mi].count) mi = k;
                entries[mi].prev = (uint8_t)i; entries[mi].curr = (uint8_t)j;
                entries[mi].count = bigram_stats[i][j];
                min_in = entries[0].count;
                for (uint16_t k = 1; k < BG_MAX; k++) if (entries[k].count < min_in) min_in = entries[k].count;
            }
        }
    }
    /* Sort descending */
    for (uint16_t i = 1; i < n; i++) {
        bg_t tmp = entries[i]; int j = i - 1;
        while (j >= 0 && entries[j].count < tmp.count) { entries[j+1] = entries[j]; j--; }
        entries[j+1] = tmp;
    }

    uint16_t total_payload = 8 + n * 4;
    ks_respond_begin(cmd, KS_STATUS_OK, total_payload);
    uint8_t hdr[8];
    hdr[0] = MODULE_ID;
    hdr[1] = (uint8_t)NUM_KEYS;
    pack_u32_le(hdr + 2, bigram_total);
    pack_u16_le(hdr + 6, get_bigram_stats_max());
    ks_respond_write(hdr, 8);

    for (uint16_t i = 0; i < n; i++) {
        uint8_t e[4] = { entries[i].prev, entries[i].curr, 0, 0 };
        pack_u16_le(e + 2, entries[i].count);
        ks_respond_write(e, 4);
    }
    ks_respond_end();
    free(entries);
    #undef BG_MAX
}

/* ── Layout JSON ────────────────────────────────────────────────── */

static void bin_cmd_layout_json(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    extern const char board_layout_json[];
    uint16_t jlen = (uint16_t)strlen(board_layout_json);
    ks_respond(cmd, KS_STATUS_OK, (const uint8_t *)board_layout_json, jlen);
}

/* ── Command table ──────────────────────────────────────────────── */

static const ks_bin_cmd_entry_t bin_cmd_table[] = {
    /* System */
    { KS_CMD_PING,              bin_cmd_ping },
    { KS_CMD_VERSION,           bin_cmd_version },
    { KS_CMD_FEATURES,          bin_cmd_features },
    { KS_CMD_DFU,               bin_cmd_dfu },
    /* Keymap */
    { KS_CMD_LAYER_INDEX,       bin_cmd_layer_index },
    { KS_CMD_KEYMAP_CURRENT,    bin_cmd_keymap_get },
    { KS_CMD_KEYMAP_GET,        bin_cmd_keymap_get },
    { KS_CMD_SETKEY,            bin_cmd_setkey },
    { KS_CMD_SETLAYER,          bin_cmd_setlayer },
    { KS_CMD_LAYER_NAME,        bin_cmd_layer_name },
    /* Layout */
    { KS_CMD_LIST_LAYOUTS,      bin_cmd_list_layouts },
    { KS_CMD_SET_LAYOUT_NAME,   bin_cmd_set_layout_name },
    { KS_CMD_GET_LAYOUT_JSON,   bin_cmd_layout_json },
    /* Bluetooth */
    { KS_CMD_BT_QUERY,          bin_cmd_bt_query },
    { KS_CMD_BT_SWITCH,         bin_cmd_bt_switch },
    { KS_CMD_BT_PAIR,           bin_cmd_bt_pair },
    { KS_CMD_BT_DISCONNECT,     bin_cmd_bt_disconnect },
    { KS_CMD_BT_NEXT,           bin_cmd_bt_next },
    { KS_CMD_BT_PREV,           bin_cmd_bt_prev },
    /* Tamagotchi */
    { KS_CMD_TAMA_QUERY,        bin_cmd_tama_query },
    { KS_CMD_TAMA_ENABLE,       bin_cmd_tama_enable },
    { KS_CMD_TAMA_DISABLE,      bin_cmd_tama_disable },
    { KS_CMD_TAMA_FEED,         bin_cmd_tama_feed },
    { KS_CMD_TAMA_PLAY,         bin_cmd_tama_play },
    { KS_CMD_TAMA_SLEEP,        bin_cmd_tama_sleep },
    { KS_CMD_TAMA_MEDICINE,     bin_cmd_tama_medicine },
    { KS_CMD_TAMA_SAVE,         bin_cmd_tama_save },
    /* Features */
    { KS_CMD_AUTOSHIFT_TOGGLE,  bin_cmd_autoshift },
    { KS_CMD_WPM_QUERY,         bin_cmd_wpm },
    { KS_CMD_TRILAYER_SET,      bin_cmd_trilayer },
    /* Macros */
    { KS_CMD_LIST_MACROS,       bin_cmd_list_macros },
    { KS_CMD_MACRO_ADD,         bin_cmd_macro_add },
    { KS_CMD_MACRO_ADD_SEQ,     bin_cmd_macro_add_seq },
    { KS_CMD_MACRO_DELETE,      bin_cmd_macro_delete },
    /* Stats */
    { KS_CMD_KEYSTATS_BIN,      bin_cmd_keystats_bin },
    { KS_CMD_KEYSTATS_RESET,    bin_cmd_keystats_reset },
    { KS_CMD_BIGRAMS_BIN,       bin_cmd_bigrams_bin },
    { KS_CMD_BIGRAMS_RESET,     bin_cmd_bigrams_reset },
    /* Tap Dance */
    { KS_CMD_TD_SET,            bin_cmd_td_set },
    { KS_CMD_TD_LIST,           bin_cmd_td_list },
    /* Combos */
    { KS_CMD_COMBO_SET,         bin_cmd_combo_set },
    { KS_CMD_COMBO_LIST,        bin_cmd_combo_list },
    /* Leader */
    { KS_CMD_LEADER_SET,        bin_cmd_leader_set },
    { KS_CMD_LEADER_LIST,       bin_cmd_leader_list },
    /* Key Override */
    { KS_CMD_KO_SET,            bin_cmd_ko_set },
    { KS_CMD_KO_LIST,           bin_cmd_ko_list },
};

void cdc_binary_cmds_init(void)
{
    ks_register_binary_commands(bin_cmd_table,
                                sizeof(bin_cmd_table) / sizeof(bin_cmd_table[0]));
}
