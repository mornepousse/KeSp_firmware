#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Écran Sharp memory-LCD des moitiés — logique pure, testée host
 * (test/test_memlcd_model.c). Panneau LS011B7DH03 (module nice!view) monté en
 * PORTRAIT : 68 px de large × 160 de haut, 1 bit par pixel.
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#define MEMLCD_W          68
#define MEMLCD_H          160
#define MEMLCD_LINE_BYTES 9     /* ceil(68 / 8) : une RANGÉE du tampon portrait */

/* Géométrie PHYSIQUE du panneau : 68 lignes de grille de 160 pixels (catalogue
 * Sharp, lemia doc 6844 p. 5 : « LS011B7DH03 160 × 68 », H = sens des données,
 * comme le LS013B7DH05 144 × 168 = 18 octets × 168 lignes). Le portrait 68 × 160
 * est donc une ROTATION de 90° : une colonne du tampon devient une ligne du
 * panneau. Adresses de ligne 1..68, 20 octets de pixels par ligne. */
#define MEMLCD_PANEL_LINES      68
#define MEMLCD_PANEL_LINE_BYTES 20

/* Le panneau lit LSB-first ; le maître SPI de l'ESP32 émet MSB-first. On
 * inverse donc les bits des octets de COMMANDE et d'ADRESSE avant émission
 * (les octets de pixels sont posés directement bit 0 = pixel 0). Trois
 * permutations (nibbles, paires, bits) : involution, testée sur les 256 valeurs. */
static inline uint8_t memlcd_rev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* Transpose le tampon portrait (MEMLCD_H rangées × MEMLCD_LINE_BYTES, bit 7 de
 * l'octet 0 = x 0, 1 = ENCRE) en lignes du panneau (MEMLCD_PANEL_LINES × 20
 * octets, bit 7 = D1 émis en premier par le SPI MSB-first, 1 = BLANC — app note
 * lemia doc 6845 p. 10 : D(n) = L → noir). Rotation 90° : le pixel portrait
 * (x, y) tombe ligne x, colonne 159 - y ; rot180 retourne l'image (ligne 67 - x,
 * colonne y) pour un module monté tête en bas. */
static inline void memlcd_fb_to_panel(const uint8_t *fb, uint8_t *panel, bool rot180)
{
    memset(panel, 0xFF, (size_t)MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES);
    for (int y = 0; y < MEMLCD_H; y++) {
        const uint8_t *row = fb + (size_t)y * MEMLCD_LINE_BYTES;
        for (int x = 0; x < MEMLCD_W; x++) {
            if (!(row[x >> 3] & (0x80 >> (x & 7)))) continue;   /* pas d'encre */
            int ligne = rot180 ? (MEMLCD_PANEL_LINES - 1 - x) : x;
            int col   = rot180 ? y : (MEMLCD_H - 1 - y);
            panel[ligne * MEMLCD_PANEL_LINE_BYTES + (col >> 3)] &= (uint8_t)~(0x80 >> (col & 7));
        }
    }
}

/* Tension affichée. L'ADC oscille entre deux dV voisins et chaque changement
 * est une réécriture du panneau ; mais une hystérésis « ±1 autour de l'AFFICHÉ »
 * a figé 4,2 V toute une nuit pendant que la batterie perdait 0,1 V (banc
 * 2026-09-15). Règle : une valeur différente de l'affichée s'affiche quand elle
 * a TENU hold_ms d'affilée — l'oscillation ne tient jamais, la dérive finit par
 * tenir. Inconnue (0xFF) et le retour d'inconnue passent sans délai. */
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

/* Nom de couche sur 68 px en Montserrat 14 : 4 caractères par ligne, 3 lignes
 * au plus, et « … » sur la dernière si le nom dépasse 12 caractères.
 * L'utilisateur a préféré des lignes lisibles à un texte tourné de 90°.
 * MEMLCD_NOM_BUF loge 4 caractères + l'ellipse UTF-8 (3 octets) + NUL. */
#define MEMLCD_NOM_COLS   4
#define MEMLCD_NOM_LIGNES 3
#define MEMLCD_NOM_BUF    (MEMLCD_NOM_COLS + 4)

static inline uint8_t memlcd_couper_nom(const char *nom,
                                        char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF])
{
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lignes[i][0] = '\0';
    size_t n = nom ? strlen(nom) : 0;
    if (n == 0) return 1;                       /* une ligne vide, jamais zéro ligne */
    const size_t max = (size_t)MEMLCD_NOM_COLS * MEMLCD_NOM_LIGNES;
    uint8_t nl = 0;
    for (size_t off = 0; off < n && nl < MEMLCD_NOM_LIGNES; off += MEMLCD_NOM_COLS, nl++) {
        size_t len = n - off;
        if (len > MEMLCD_NOM_COLS) len = MEMLCD_NOM_COLS;
        memcpy(lignes[nl], nom + off, len);
        lignes[nl][len] = '\0';
    }
    if (n > max)                                /* tronqué : 3 lettres + … */
        memcpy(lignes[MEMLCD_NOM_LIGNES - 1] + 3, "\xE2\x80\xA6", 4);
    return nl;
}

/* Tout ce que l'écran montre. is_left choisit la zone centrale (couche ou logo)
 * mais n'est pas une DONNÉE qui bouge : il n'entre pas dans le diff. */
typedef struct {
    uint8_t route_rf, dongle_vu;
    uint8_t batt_local_dv, batt_local_chg;
    uint8_t couche;
    char    nom[16];
    uint8_t is_left;
} memlcd_model_t;

/* Faut-il redessiner ? Chaque redessin est une transaction sur le bus SPI que
 * l'écran PARTAGE avec la radio : on ne le fait que si un champ affiché a changé. */
static inline bool memlcd_model_diff(const memlcd_model_t *a, const memlcd_model_t *b)
{
    return a->route_rf != b->route_rf || a->dongle_vu != b->dongle_vu ||
           a->batt_local_dv != b->batt_local_dv || a->batt_local_chg != b->batt_local_chg ||
           a->couche != b->couche || strcmp(a->nom, b->nom) != 0;
}
