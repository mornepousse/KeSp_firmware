#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../../comm/chest/chest_proto.h"

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
#define MEMLCD_COFFRE_BUF     5        /* 4 UNSCII 8 characters in the 35 px zone + NUL */
_Static_assert(MEMLCD_COFFRE_BUF >= CHEST_MODE_LABEL_BUF, "MEMLCD_COFFRE_BUF must fit chest_mode_label's output");

/* Chest status under the logo: "P4" ready ("P4.." booting, "P4?" unknown
 * protocol version), then "SD", then the USB mode line (chest_mode_label:
 * ACTIVE upper case once ARRIVED, WANTED lower case while PENDING, "ERR" on
 * FAULT — v2 rule, kept as-is in v3: 2026-09-29). Lines packed upwards: a
 * line only used when the previous one was. */
static inline void memlcd_lignes_coffre(const memlcd_model_t *m, char l[3][MEMLCD_COFFRE_BUF])
{
    for (int i = 0; i < 3; i++) l[i][0] = '\0';
    if (!(m->coffre & MEMLCD_COFFRE_PRESENT)) return;
    if (m->coffre & MEMLCD_COFFRE_BADVER) { strcpy(l[0], "P4?"); return; }
    if (!(m->coffre & CHEST_STATE_READY)) { strcpy(l[0], "P4.."); return; }
    int n = 0;
    strcpy(l[n++], "P4");
    if (m->coffre & CHEST_STATE_SD) strcpy(l[n++], "SD");
    char mode[CHEST_MODE_LABEL_BUF];
    chest_mode_label((chest_mode_state_t)m->coffre_mode_state, m->coffre_mode_active, m->coffre_mode_wanted, mode);
    if (mode[0] && n < 3) strcpy(l[n++], mode);
}

/* Cuts `s` into up to `n` UNSCII lines of up to 8 characters (hard cut at
 * column 8 — sanitized/untrusted text, not prose: no space-awareness like
 * memlcd_couper_nom above). If text remains once the n-th line is full, that
 * line's LAST character is replaced with '~' (UNSCII has no ellipsis
 * glyph — "cut into lines of 8, with ~ marking a cut", plan Task 5): 7
 * characters of real content plus the marker, so a cut is never silent.
 * Returns the number of lines written, 1..n (never 0: an empty string still
 * produces one empty line, like memlcd_couper_nom). */
#define MEMLCD_COUP8_COLS 8
static inline uint8_t memlcd_couper_8(const char *s, uint8_t n, char lines[][MEMLCD_ETAT_BUF])
{
    for (uint8_t i = 0; i < n; i++) lines[i][0] = '\0';
    if (n == 0) return 0;
    size_t len = s ? strlen(s) : 0;
    if (len == 0) return 1;
    size_t pos = 0;
    uint8_t nl = 0;
    while (pos < len && nl < n) {
        bool last_slot = (nl == (uint8_t)(n - 1));
        size_t remain = len - pos;
        if (!last_slot || remain <= MEMLCD_COUP8_COLS) {
            size_t take = remain < MEMLCD_COUP8_COLS ? remain : MEMLCD_COUP8_COLS;
            memcpy(lines[nl], s + pos, take);
            lines[nl][take] = '\0';
            pos += take;
        } else {
            memcpy(lines[nl], s + pos, MEMLCD_COUP8_COLS - 1);
            lines[nl][MEMLCD_COUP8_COLS - 1] = '~';
            lines[nl][MEMLCD_COUP8_COLS] = '\0';
            pos = len;   /* the rest is dropped — the '~' says so */
        }
        nl++;
    }
    return nl ? nl : 1;
}

#define MEMLCD_BAS_LIGNES 6

/* The bottom area's six UNSCII lines (spec §5 / plan Task 5), priority
 * order prompt > code visible > browsing > (nothing — the caller falls back
 * to the layer/status widgets). Returns true when one of the first three
 * cases applies (the caller must show these lines instead of the layer). */
static inline bool memlcd_bas_coffre(const memlcd_model_t *m, char lines[MEMLCD_BAS_LIGNES][MEMLCD_ETAT_BUF])
{
    for (int i = 0; i < MEMLCD_BAS_LIGNES; i++) lines[i][0] = '\0';

    if (m->coffre_op) {
        chest_op_label(m->coffre_op, lines[0]);   /* CHEST_LABEL_BUF (8) fits MEMLCD_ETAT_BUF (9) */
        char lbl[4][MEMLCD_ETAT_BUF];
        uint8_t nl = memlcd_couper_8(m->coffre_label, 4, lbl);
        for (uint8_t i = 0; i < nl; i++) strcpy(lines[1 + i], lbl[i]);
        if (m->coffre_op_count > 1) {
            char cpt[MEMLCD_ETAT_BUF];
            snprintf(cpt, sizeof cpt, "%u CPT", (unsigned)m->coffre_op_count);
            uint8_t idx = (nl >= 4) ? 4 : (uint8_t)(1 + nl);   /* room left: appended; full: replaces the last label line */
            strncpy(lines[idx], cpt, MEMLCD_ETAT_BUF - 1);
            lines[idx][MEMLCD_ETAT_BUF - 1] = '\0';
        }
        strcpy(lines[5], "OK ?");
        return true;
    }
    if (m->coffre_code_visible) {
        strncpy(lines[0], m->coffre_nom, MEMLCD_ETAT_BUF - 1); lines[0][MEMLCD_ETAT_BUF - 1] = '\0';
        /* line 1 stays empty. digits is 6 or 8 only (chest_code_decode's own
         * contract) — the code string's length says which. */
        if (strlen(m->coffre_code) == 8) {
            memcpy(lines[2], m->coffre_code, 4); lines[2][4] = '\0';
            strcpy(lines[3], m->coffre_code + 4);
        } else {
            strcpy(lines[2], m->coffre_code);
        }
        snprintf(lines[5], MEMLCD_ETAT_BUF, "  %2u s", (unsigned)m->coffre_code_secs);
        return true;
    }
    if (m->coffre_browsing) {
        snprintf(lines[0], MEMLCD_ETAT_BUF, "%u/%u", (unsigned)m->coffre_pos + 1, (unsigned)m->coffre_total);
        char nm[4][MEMLCD_ETAT_BUF];
        uint8_t nl = memlcd_couper_8(m->coffre_nom, 4, nm);
        for (uint8_t i = 0; i < nl; i++) strcpy(lines[1 + i], nm[i]);
        if (!(m->coffre & CHEST_STATE_TIME)) strcpy(lines[5], "NO TIME");
        return true;
    }
    return false;
}

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
