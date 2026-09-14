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
#define MEMLCD_LINE_BYTES 9     /* ceil(68 / 8) */

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
    uint8_t batt_autre_dv, batt_autre_chg;
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
           a->batt_autre_dv != b->batt_autre_dv || a->batt_autre_chg != b->batt_autre_chg ||
           a->couche != b->couche || strcmp(a->nom, b->nom) != 0;
}
