/* Keymap par défaut — Niphargus, LES DEUX moitiés.
 *
 * La gauche est le seul moteur keymap du clavier : elle porte les keycodes des
 * 52 touches alors qu'elle n'en balaie que 26. D'où KEYMAP_COLS = 14 quand
 * MATRIX_COLS vaut 7 — colonnes 0-6 pour cette moitié, 7-13 pour la droite,
 * dont la coordonnée reçue par radio se décale de MATRIX_COLS.
 *
 * 26 touches par moitié, en rangées de 7/7/6/6 : les positions manquantes des
 * deux dernières rangées sont K_NO, de chaque côté.
 *
 * ⚠ CÔTÉ DROIT, CES TROUS SONT EN COLONNE 7, PAS EN COLONNE 13. Les deux
 * moitiés sont le même PCB retourné : la colonne courte de la droite est sa
 * colonne PHYSIQUE 6, qui porte la colonne keymap 13 - 6 = 7
 * (half_col_to_keymap). Jusqu'au 2026-09-07 les K_NO étaient en colonne 13 et
 * les rangées 2 et 3 décalées d'un cran : K_N était injoignable, et la touche
 * la plus extérieure de la droite ne produisait rien. Les rangées 0 et 1,
 * pleines sur les sept colonnes, ne trahissaient rien — d'où une panne qui ne
 * se voyait que sur la rangée du bas.
 *
 * Deux positions de pouce à droite (colonnes 12 et 13) restent sans affectation
 * d'usine : elles existent physiquement, à provisionner par USB. La vraie keymap est
 * provisionnée par USB (KS_CMD_*) ; celle-ci n'est que le repli d'usine. */
#include "keymap.h"
#include "key_definitions.h"
#include "keyboard_config.h"

char default_layout_names[LAYERS][MAX_LAYOUT_NAME_LENGTH] = {
    "MAIN", "NAV", "LAYER 2", "LAYER 3", "LAYER 4",
    "LAYER 5", "LAYER 6", "LAYER 7", "LAYER 8", "LAYER 9",
};

#define _______ K_TRNS
#define XXXXXXX K_NO

uint16_t keymaps[LAYERS][MATRIX_ROWS][KEYMAP_COLS] = {
    {   /* 0 — MAIN            ── gauche (0-6) ──          ── droite (7-13) ── */
        {K_TAB,   K_Q, K_W, K_E, K_R, K_T, K_LBRC,   K_Y, K_U, K_I, K_O, K_P,    K_BSPC,   K_DEL},
        {K_ESC,   K_A, K_S, K_D, K_F, K_G, K_RBRC,   K_H, K_J, K_K, K_L, K_SCLN, K_QUOT,   K_ENT},
        {K_LSHIFT,K_Z, K_X, K_C, K_V, K_B, XXXXXXX,  XXXXXXX, K_N, K_M, K_COMM, K_DOT, K_SLSH, K_RSHIFT},
        {K_LCTRL, K_LWIN, K_LALT, MO_L1, K_SPACE, K_ENT, XXXXXXX,
                                                    XXXXXXX, K_SPACE, MO_L1, K_RALT, K_RCTRL, XXXXXXX, XXXXXXX},
    },
    {   /* 1 — NAV */
        {_______, K_1, K_2, K_3, K_4, K_5, XXXXXXX,  K_6, K_7, K_8, K_9, K_0, _______, _______},
        {_______, K_F1, K_HOME, K_UP, K_END, XXXXXXX, XXXXXXX,
                                                    XXXXXXX, K_LEFT, K_DOWN, K_UP, K_RIGHT, XXXXXXX, XXXXXXX},
        {_______, XXXXXXX, K_LEFT, K_DOWN, K_RIGHT, XXXXXXX, XXXXXXX,
                                                    XXXXXXX, K_HOME, K_END, XXXXXXX, XXXXXXX, XXXXXXX, XXXXXXX},
        {_______, _______, _______, _______, _______, XXXXXXX, XXXXXXX,
                                                    _______, _______, _______, _______, XXXXXXX, XXXXXXX, XXXXXXX},
    },
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
    {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}}, {{XXXXXXX}},
};
