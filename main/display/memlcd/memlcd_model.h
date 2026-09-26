#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

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

/* Layer name, full width under the icon column (2026-09-26): Montserrat 14
 * on 68 px = 6 characters per line, 2 lines. A line is cut at the last space
 * that fits (skipped), else hard at 6; the last line ends in "…" when the
 * name does not fit, never after a space. (4 x 3 until 2026-09-25, 4 x 2 for
 * one day under the big layer number.) MEMLCD_NOM_BUF holds 6 characters +
 * NUL, or 5 + the UTF-8 ellipsis (3 bytes) + NUL. */
#define MEMLCD_NOM_COLS   6
#define MEMLCD_NOM_LIGNES 2
#define MEMLCD_NOM_BUF    (MEMLCD_NOM_COLS + 3)

static inline uint8_t memlcd_couper_nom(const char *nom,
                                        char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF])
{
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lignes[i][0] = '\0';
    size_t n = nom ? strlen(nom) : 0;
    if (n == 0) return 1;                       /* one empty line, never zero lines */
    const char *p = nom;
    uint8_t nl = 0;
    while (*p && nl < MEMLCD_NOM_LIGNES) {
        while (*p == ' ') p++;                  /* a line never starts with a space */
        size_t reste = strlen(p);
        if (reste == 0) break;
        bool derniere = (nl == MEMLCD_NOM_LIGNES - 1);
        if (reste <= MEMLCD_NOM_COLS) {         /* fits: take it all */
            memcpy(lignes[nl], p, reste); lignes[nl][reste] = '\0';
            nl++; break;
        }
        if (derniere) {                         /* overflow: 5 characters + … */
            size_t len = MEMLCD_NOM_COLS - 1;
            while (len > 0 && p[len - 1] == ' ') len--;
            memcpy(lignes[nl], p, len);
            memcpy(lignes[nl] + len, "\xE2\x80\xA6", 4);
            nl++; break;
        }
        size_t len = MEMLCD_NOM_COLS;           /* cut at the last space that fits */
        for (size_t i = MEMLCD_NOM_COLS; i > 0; i--) if (p[i] == ' ') { len = i; break; }
        memcpy(lignes[nl], p, len); lignes[nl][len] = '\0';
        p += len; nl++;
    }
    return nl ? nl : 1;
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
    uint8_t is_left;
} memlcd_model_t;

/* The two status lines under the layer name (see test_ligne_etat). */
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
           a->veille != b->veille;
}
