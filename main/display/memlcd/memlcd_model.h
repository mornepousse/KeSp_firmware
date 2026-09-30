#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../../comm/chest/chest_proto.h"
#include "../../comm/chest/chest_view.h"

/* Halves' Sharp memory-LCD screen — pure logic, tested on host
 * (test/test_memlcd_model.c). LS011B7DH03 panel (nice!view module) mounted in
 * PORTRAIT: 68 px wide × 160 tall, 1 bit per pixel.
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#define MEMLCD_W          68
#define MEMLCD_H          160
#define MEMLCD_LINE_BYTES 9     /* ceil(68 / 8): one ROW of the portrait buffer */

/* PHYSICAL geometry of the panel: 68 grid lines of 160 pixels (Sharp
 * catalog, lemia doc 6844 p. 5: "LS011B7DH03 160 × 68", H = data direction,
 * like the LS013B7DH05 144 × 168 = 18 bytes × 168 lines). The 68 × 160
 * portrait is therefore a 90° ROTATION: a column of the buffer becomes a line
 * of the panel. Line addresses 1..68, 20 bytes of pixels per line. */
#define MEMLCD_PANEL_LINES      68
#define MEMLCD_PANEL_LINE_BYTES 20

/* The panel reads LSB-first; the ESP32's SPI master transmits MSB-first. So
 * the bits of the COMMAND and ADDRESS bytes are reversed before transmission
 * (the pixel bytes are laid out directly, bit 0 = pixel 0). Three
 * permutations (nibbles, pairs, bits): an involution, tested over all 256 values. */
static inline uint8_t memlcd_rev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* Transposes the portrait buffer (MEMLCD_H rows × MEMLCD_LINE_BYTES, bit 7 of
 * byte 0 = x 0, 1 = INK) into panel lines (MEMLCD_PANEL_LINES × 20
 * bytes, bit 7 = D1 sent first by MSB-first SPI, 1 = WHITE — app note
 * lemia doc 6845 p. 10: D(n) = L → black). 90° rotation: the portrait pixel
 * (x, y) lands on line x, column 159 - y; rot180 flips the image (line 67 - x,
 * column y) for a module mounted upside down. */
static inline void memlcd_fb_to_panel(const uint8_t *fb, uint8_t *panel, bool rot180)
{
    memset(panel, 0xFF, (size_t)MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES);
    for (int y = 0; y < MEMLCD_H; y++) {
        const uint8_t *row = fb + (size_t)y * MEMLCD_LINE_BYTES;
        for (int x = 0; x < MEMLCD_W; x++) {
            if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;   /* no ink */
            int ligne = rot180 ? (MEMLCD_PANEL_LINES - 1 - x) : x;
            int col   = rot180 ? y : (MEMLCD_H - 1 - y);
            panel[ligne * MEMLCD_PANEL_LINE_BYTES + (col >> 3)] &= (uint8_t)~(0x80 >> (col & 7));
        }
    }
}

/* Displayed voltage. The ADC oscillates between two neighboring dV values and
 * each change is a panel rewrite; but a "±1 around the DISPLAYED value"
 * hysteresis froze 4.2 V for a whole night while the battery lost 0.1 V (bench
 * 2026-09-15). Rule: a value different from the displayed one is shown once it
 * has HELD for hold_ms in a row — oscillation never holds, drift eventually
 * holds. Unknown (0xFF) and the return from unknown pass with no delay. */
typedef struct { uint8_t aff, cand; uint32_t cand_ms; bool vide; } memlcd_batt_aff_t;
static inline void memlcd_batt_aff_init(memlcd_batt_aff_t *b) { b->aff = 0xFF; b->cand = 0xFF; b->cand_ms = 0; b->vide = true; }
static inline uint8_t memlcd_batt_aff_step(memlcd_batt_aff_t *b, uint8_t dv, uint32_t now_ms, uint32_t hold_ms)
{
    if (b->vide || dv == 0xFF || b->aff == 0xFF) { b->vide = false; b->aff = dv; b->cand = dv; return b->aff; }
    if (dv == b->aff) { b->cand = dv; return b->aff; }
    if (dv != b->cand) { b->cand = dv; b->cand_ms = now_ms; return b->aff; }
    if ((uint32_t)(now_ms - b->cand_ms) >= hold_ms) b->aff = dv;
    return b->aff;
}

/* The layer the screen shows: the STABLE one — base, TO or Layer Lock — which
 * is the engine's last_layer; a held MO/LT/LM (current_layout differing) is
 * not shown. In RF the left does not hear the right, so it cannot know a MO
 * held on the right, and a 1 s status screen cannot follow a momentary layer
 * anyway (Mae, 2026-09-26). */
static inline uint8_t memlcd_couche_affichee(uint8_t courante, uint8_t derniere)
{
    (void)courante;
    return derniere;
}

#define MEMLCD_OSL_AUCUNE 0xFF   /* no one-shot layer armed */
#define MEMLCD_ETAT_BUF   9      /* 8 UNSCII 8 columns on 68 px + NUL */

/* Everything the screen shows. is_left picks the central zone (layer or logo)
 * but is not DATA that changes: it does not enter the diff. */
typedef struct {
    uint8_t route_rf, dongle_vu;
    uint8_t lien_5v;                   /* TRRS handshake UP: the 5 V is closed on our side */
    uint8_t batt_local_dv, batt_local_chg;
    uint8_t batt_niveau;               /* 0 normal, 1 low, 2 critical: thickened gauge border */
    uint8_t couche;
    char    nom[16];
    uint8_t caps_lock;                 /* host LED — known over USB only */
    uint8_t caps_word;
    uint8_t osm;                       /* armed one-shot modifiers, HID mask */
    uint8_t osl;                       /* armed one-shot layer, MEMLCD_OSL_AUCUNE if none */
    uint8_t veille;                    /* last image before sleep: zZ */
    uint8_t  coffre;                   /* MEMLCD_COFFRE_* | CHEST_STATE_* (incl. TIME); 0 = no chest */
    uint16_t coffre_op;                /* chest op awaiting confirmation, 0 = none */
    uint8_t  coffre_op_count;          /* accounts targeted by that op: 0 none, 1, N (RESET) */
    char     coffre_label[CHEST_LABEL_MAX + 1];   /* the CHEST's own label for the pending op */
    uint8_t  coffre_mode_active, coffre_mode_wanted, coffre_mode_state;
    uint8_t  coffre_browsing;          /* OATH active mode AND a LIST page cached */
    uint8_t  coffre_pos, coffre_total; /* cursor position (0-based), cached page's total */
    char     coffre_nom[CHEST_LABEL_MAX + 1];     /* entry under the cursor, "" if not cached */
    uint8_t  coffre_code_visible;
    char     coffre_code[9];           /* 6 or 8 digits, NUL-terminated */
    uint8_t  coffre_code_secs;         /* whole seconds left, rounded up */
    uint8_t is_left;
} memlcd_model_t;

#define MEMLCD_COFFRE_PRESENT 0x80
#define MEMLCD_COFFRE_BADVER  0x40

/* chest_view_t (main/comm/chest/chest_view.h) -> the model's coffre_*
 * fields — the SAME mapping used by both memlcd_backend.c's lire_modele()
 * and the host tests (review M-c: a single pure function instead of the
 * mapping being hand-duplicated in the backend and reinvented in the test).
 * Only the coffre_* fields are touched: the caller is expected to have
 * already zeroed/filled the rest of *m (route_rf, battery, layer…). */
static inline void memlcd_model_set_coffre(memlcd_model_t *m, const chest_view_t *v)
{
    m->coffre = v->bits; m->coffre_op = v->op; m->coffre_op_count = v->op_count;
    strncpy(m->coffre_label, v->label, sizeof m->coffre_label - 1);
    m->coffre_label[sizeof m->coffre_label - 1] = '\0';
    m->coffre_mode_active = v->mode_active; m->coffre_mode_wanted = v->mode_wanted; m->coffre_mode_state = v->mode_state;
    m->coffre_browsing = v->browsing; m->coffre_pos = v->pos; m->coffre_total = v->total;
    strncpy(m->coffre_nom, v->name, sizeof m->coffre_nom - 1);
    m->coffre_nom[sizeof m->coffre_nom - 1] = '\0';
    m->coffre_code_visible = v->code_visible;
    strncpy(m->coffre_code, v->code, sizeof m->coffre_code - 1);
    m->coffre_code[sizeof m->coffre_code - 1] = '\0';
    m->coffre_code_secs = v->code_secs;
}

/* ── Geometry of the RIGHT half's screen (memlcd_backend.c) ─────────
 * Y_SEP is the separator between its top part (icon column) and its 60 px
 * logo. The left half draws the cave (memlcd_cave.h) and has no separator. */
#define MEMLCD_Y_SEP          92

/* ── Width oracle ──────────────────────────────────────────────────────
 * The left screen is laid out by PURE code (memlcd_cave.h), tested on host
 * where LVGL does not run: text is measured with memlcd_font_widths.h,
 * generated from the LVGL font files by scripts/gen_memlcd_font_widths.py
 * (the font-widths tripwire brick fails if it drifts), kerning included:
 * the host measures what LVGL draws. A line still keeps 2 px of its box free
 * (a glyph's ink may overhang its advance by a pixel).
 * MEMLCD_F_* is the cave's Montserrat ladder, smallest first (Montserrat 12
 * is the text floor: below it the panel's 1-bit threshold erases glyphs —
 * tools/memlcd_sim/check_glyph_ink.c), then UNSCII 8, a bitmap font kept
 * ONLY as the last resort that keeps a chest label whole on a prompt. */
#include "memlcd_font_widths.h"

typedef enum {
    MEMLCD_F_M12, MEMLCD_F_M14, MEMLCD_F_M16, MEMLCD_F_M18,
    MEMLCD_F_M20, MEMLCD_F_M24, MEMLCD_F_M28, MEMLCD_F_M32,
    MEMLCD_F_U8,
    MEMLCD_F_N
} memlcd_font_t;
#define MEMLCD_W_BUDGET   66                      /* a full-width line: 68 - 2 px of air */
#define MEMLCD_VC_TXT     (CHEST_LABEL_MAX + 1)   /* a line can never hold more than the whole text */
#define MEMLCD_CODE_FENETRE_S 30                  /* TOTP window: the countdown's 100 % */

/* Line pitch of each font: LVGL's line_height; UNSCII 8 is 9 high, set at
 * 11 for air between the lines. */
static inline uint8_t memlcd_font_pas(uint8_t font)
{
    switch (font) {
    case MEMLCD_F_M12: return MEMLCD_FW_M12_LINE_H;
    case MEMLCD_F_M14: return MEMLCD_FW_M14_LINE_H;
    case MEMLCD_F_M16: return MEMLCD_FW_M16_LINE_H;
    case MEMLCD_F_M18: return MEMLCD_FW_M18_LINE_H;
    case MEMLCD_F_M20: return MEMLCD_FW_M20_LINE_H;
    case MEMLCD_F_M24: return MEMLCD_FW_M24_LINE_H;
    case MEMLCD_F_M28: return MEMLCD_FW_M28_LINE_H;
    case MEMLCD_F_M32: return MEMLCD_FW_M32_LINE_H;
    default:           return 11;
    }
}

/* Kerning of the pair (a, b) in 1/16 px — kern_scale is 16 for every size
 * (checked by the generator), so the class value is used as is. */
static inline int memlcd_kern(uint8_t font, char a, char b)
{
    static const int8_t *const kv[] = {
        memlcd_fw_m12_kv, memlcd_fw_m14_kv, memlcd_fw_m16_kv, memlcd_fw_m18_kv,
        memlcd_fw_m20_kv, memlcd_fw_m24_kv, memlcd_fw_m28_kv, memlcd_fw_m32_kv,
    };
    unsigned ca = (unsigned char)a, cb = (unsigned char)b;
    if (font >= MEMLCD_F_U8) return 0;
    if (ca < MEMLCD_FW_FIRST || ca > MEMLCD_FW_LAST || cb < MEMLCD_FW_FIRST || cb > MEMLCD_FW_LAST) return 0;
    uint8_t l = memlcd_fw_kl[ca - MEMLCD_FW_FIRST], r = memlcd_fw_kr[cb - MEMLCD_FW_FIRST];
    if (!l || !r) return 0;
    return kv[font][(l - 1) * MEMLCD_FW_KERN_RC + (r - 1)];
}

/* Advance of ch followed by next ('\0' at the end of a line), as LVGL
 * computes it: the pair's kerning added, then rounded glyph by glyph. */
static inline uint8_t memlcd_glyph_w2(uint8_t font, char ch, char next)
{
    static const uint16_t *const tables[] = {
        memlcd_fw_m12, memlcd_fw_m14, memlcd_fw_m16, memlcd_fw_m18,
        memlcd_fw_m20, memlcd_fw_m24, memlcd_fw_m28, memlcd_fw_m32,
    };
    if (font >= MEMLCD_F_U8) return 8;
    unsigned c = (unsigned char)ch;
    /* Outside 0x20..0x7E (never after chest_proto's sanitizing): count the
     * widest glyph, unkerned, so an unknown character can only make a line
     * SHORTER. */
    if (c < MEMLCD_FW_FIRST || c > MEMLCD_FW_LAST) return (uint8_t)((tables[font]['W' - MEMLCD_FW_FIRST] + 8) >> 4);
    int adv = tables[font][c - MEMLCD_FW_FIRST] + memlcd_kern(font, ch, next);
    return (uint8_t)((adv + 8) >> 4);
}
static inline uint8_t memlcd_glyph_w(uint8_t font, char ch) { return memlcd_glyph_w2(font, ch, '\0'); }

/* Pixel width of the first n bytes of s in font, drawn as a line on its own
 * (the last glyph has no kerning partner), stopping at a NUL — exactly what
 * lv_txt_get_width gives for that line. */
static inline uint16_t memlcd_text_width_n(uint8_t font, const char *s, size_t n)
{
    uint16_t w = 0;
    if (!s) return 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        char next = (i + 1 < n) ? s[i + 1] : '\0';
        w = (uint16_t)(w + memlcd_glyph_w2(font, s[i], next));
    }
    return w;
}
static inline uint16_t memlcd_text_width(uint8_t font, const char *s)
{
    return memlcd_text_width_n(font, s, MEMLCD_VC_TXT);
}

/* The status flags under the layer name (see test_ligne_etat). */
static inline void memlcd_ligne_etat(const memlcd_model_t *m,
                                     char l1[MEMLCD_ETAT_BUF], char l2[MEMLCD_ETAT_BUF])
{
    l1[0] = '\0';
    if (m->caps_lock) strcat(l1, "CAPS");
    if (m->caps_word) strcat(l1, l1[0] ? " CW" : "CW");
    size_t n = 0;
    uint8_t mods = (uint8_t)(m->osm | (m->osm >> 4));   /* right mods onto the left bits */
    static const char lettres[4] = { 'C', 'S', 'A', 'G' };
    for (int i = 0; i < 4; i++) if (mods & (1u << i)) l2[n++] = lettres[i];
    l2[n] = '\0';
    if (m->osl != MEMLCD_OSL_AUCUNE && m->osl < 10) {
        if (n) l2[n++] = ' ';
        l2[n++] = 'L'; l2[n++] = (char)('0' + m->osl); l2[n] = '\0';
    }
}

/* Should it redraw? Each redraw is a transaction on the SPI bus that
 * the screen SHARES with the radio: it only happens if a displayed field changed. */
static inline bool memlcd_model_diff(const memlcd_model_t *a, const memlcd_model_t *b)
{
    return a->route_rf != b->route_rf || a->dongle_vu != b->dongle_vu ||
           a->lien_5v != b->lien_5v ||
           a->batt_local_dv != b->batt_local_dv || a->batt_local_chg != b->batt_local_chg ||
           a->batt_niveau != b->batt_niveau ||
           a->couche != b->couche || strcmp(a->nom, b->nom) != 0 ||
           a->caps_lock != b->caps_lock ||
           a->caps_word != b->caps_word || a->osm != b->osm || a->osl != b->osl ||
           a->veille != b->veille ||
           a->coffre != b->coffre || a->coffre_op != b->coffre_op ||
           a->coffre_op_count != b->coffre_op_count || strcmp(a->coffre_label, b->coffre_label) != 0 ||
           a->coffre_mode_active != b->coffre_mode_active ||
           a->coffre_mode_wanted != b->coffre_mode_wanted || a->coffre_mode_state != b->coffre_mode_state ||
           a->coffre_browsing != b->coffre_browsing ||
           a->coffre_pos != b->coffre_pos || a->coffre_total != b->coffre_total ||
           strcmp(a->coffre_nom, b->coffre_nom) != 0 ||
           a->coffre_code_visible != b->coffre_code_visible ||
           strcmp(a->coffre_code, b->coffre_code) != 0 || a->coffre_code_secs != b->coffre_code_secs;
}
