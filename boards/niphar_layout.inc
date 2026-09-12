/*
 * Niphargus — physical key layout, GENERE par scripts/gen_layout.py depuis
 * hardware/pcb/niphar.kicad_pcb du depot Niphargus. NE PAS EDITER A LA MAIN :
 * corriger le PCB ou le script, puis regenerer.
 *
 * Moitie gauche = feuille /s3/, 4 rows x 7 cols electriques, 26 touches ;
 * (row2,col6) et (row3,col6) n'existent pas. Chaque touche est positionnee
 * individuellement avec sa rotation reelle : les colonnes 0-1 sont evasees de
 * 10 deg, la colonne 2 de 5 deg, les autres droites ; l'arc de pouces est un
 * cluster compact cote interieur qui s'evente de 0 a 45 deg. L'ordre
 * electrique des pouces (col 3,4,5,2,0,1 de l'interieur vers l'exterieur) ne
 * suit pas l'ordre physique — c'est ce qui rendait l'ancien dessin faux.
 *
 * Unites : 1 = 19,05 mm ; x,y = coin haut-gauche ; r > 0 = sens horaire.
 * Requires: PRODUCT_NAME, MATRIX_ROWS, MATRIX_COLS, STR() defined.
 */
#ifndef KEYMAP_COLS
#define KEYMAP_COLS MATRIX_COLS
#endif

/* Moitie droite : miroir de la gauche, x' = 2W + G - x - w avec W = 8.53,
 * G = 1 ; rotations opposees ; la colonne physique c porte la colonne
 * keymap 13 - c (cf. half_col_to_keymap). Ajoutee seulement sur le maitre,
 * ou KEYMAP_COLS vaut 14. */

const char board_layout_json[] =
"{"
  "\"name\":\"" PRODUCT_NAME "\","
  "\"rows\":" STR(MATRIX_ROWS) ","
  "\"cols\":" STR(KEYMAP_COLS) ","
  "\"keys\":["
    "{\"row\":0,\"col\":0,\"x\":0.00,\"y\":0.83,\"r\":-10.0},"
    "{\"row\":0,\"col\":1,\"x\":0.98,\"y\":0.66,\"r\":-10.0},"
    "{\"row\":0,\"col\":2,\"x\":2.13,\"y\":0.29,\"r\":-5.0},"
    "{\"row\":0,\"col\":3,\"x\":3.29,\"y\":0.00},"
    "{\"row\":0,\"col\":4,\"x\":4.28,\"y\":0.12},"
    "{\"row\":0,\"col\":5,\"x\":5.28,\"y\":0.25},"
    "{\"row\":0,\"col\":6,\"x\":6.28,\"y\":0.25},"
    "{\"row\":1,\"col\":0,\"x\":0.17,\"y\":1.81,\"r\":-10.0},"
    "{\"row\":1,\"col\":1,\"x\":1.16,\"y\":1.64,\"r\":-10.0},"
    "{\"row\":1,\"col\":2,\"x\":2.22,\"y\":1.28,\"r\":-5.0},"
    "{\"row\":1,\"col\":3,\"x\":3.29,\"y\":1.00},"
    "{\"row\":1,\"col\":4,\"x\":4.28,\"y\":1.12},"
    "{\"row\":1,\"col\":5,\"x\":5.28,\"y\":1.25},"
    "{\"row\":1,\"col\":6,\"x\":6.28,\"y\":1.25},"
    "{\"row\":2,\"col\":0,\"x\":0.35,\"y\":2.79,\"r\":-10.0},"
    "{\"row\":2,\"col\":1,\"x\":1.33,\"y\":2.62,\"r\":-10.0},"
    "{\"row\":2,\"col\":2,\"x\":2.30,\"y\":2.28,\"r\":-5.0},"
    "{\"row\":2,\"col\":3,\"x\":3.29,\"y\":1.99},"
    "{\"row\":2,\"col\":4,\"x\":4.28,\"y\":2.12},"
    "{\"row\":2,\"col\":5,\"x\":5.28,\"y\":2.24},"
    "{\"row\":3,\"col\":0,\"x\":6.81,\"y\":4.37,\"r\":45.0},"
    "{\"row\":3,\"col\":1,\"x\":7.53,\"y\":3.66,\"r\":45.0},"
    "{\"row\":3,\"col\":2,\"x\":6.44,\"y\":2.83,\"r\":30.0},"
    "{\"row\":3,\"col\":3,\"x\":3.78,\"y\":3.15},"
    "{\"row\":3,\"col\":4,\"x\":4.89,\"y\":3.29,\"r\":15.0},"
    "{\"row\":3,\"col\":5,\"x\":5.93,\"y\":3.70,\"r\":30.0}"
#if KEYMAP_COLS > MATRIX_COLS
    ",{\"row\":0,\"col\":13,\"x\":17.06,\"y\":0.83,\"r\":10.0},"
    "{\"row\":0,\"col\":12,\"x\":16.08,\"y\":0.66,\"r\":10.0},"
    "{\"row\":0,\"col\":11,\"x\":14.93,\"y\":0.29,\"r\":5.0},"
    "{\"row\":0,\"col\":10,\"x\":13.77,\"y\":0.00},"
    "{\"row\":0,\"col\":9,\"x\":12.78,\"y\":0.12},"
    "{\"row\":0,\"col\":8,\"x\":11.78,\"y\":0.25},"
    "{\"row\":0,\"col\":7,\"x\":10.78,\"y\":0.25},"
    "{\"row\":1,\"col\":13,\"x\":16.89,\"y\":1.81,\"r\":10.0},"
    "{\"row\":1,\"col\":12,\"x\":15.90,\"y\":1.64,\"r\":10.0},"
    "{\"row\":1,\"col\":11,\"x\":14.84,\"y\":1.28,\"r\":5.0},"
    "{\"row\":1,\"col\":10,\"x\":13.77,\"y\":1.00},"
    "{\"row\":1,\"col\":9,\"x\":12.78,\"y\":1.12},"
    "{\"row\":1,\"col\":8,\"x\":11.78,\"y\":1.25},"
    "{\"row\":1,\"col\":7,\"x\":10.78,\"y\":1.25},"
    "{\"row\":2,\"col\":13,\"x\":16.71,\"y\":2.79,\"r\":10.0},"
    "{\"row\":2,\"col\":12,\"x\":15.73,\"y\":2.62,\"r\":10.0},"
    "{\"row\":2,\"col\":11,\"x\":14.76,\"y\":2.28,\"r\":5.0},"
    "{\"row\":2,\"col\":10,\"x\":13.77,\"y\":1.99},"
    "{\"row\":2,\"col\":9,\"x\":12.78,\"y\":2.12},"
    "{\"row\":2,\"col\":8,\"x\":11.78,\"y\":2.24},"
    "{\"row\":3,\"col\":13,\"x\":10.25,\"y\":4.37,\"r\":-45.0},"
    "{\"row\":3,\"col\":12,\"x\":9.53,\"y\":3.66,\"r\":-45.0},"
    "{\"row\":3,\"col\":11,\"x\":10.62,\"y\":2.83,\"r\":-30.0},"
    "{\"row\":3,\"col\":10,\"x\":13.28,\"y\":3.15},"
    "{\"row\":3,\"col\":9,\"x\":12.17,\"y\":3.29,\"r\":-15.0},"
    "{\"row\":3,\"col\":8,\"x\":11.13,\"y\":3.70,\"r\":-30.0}"
#endif
  "],"
  "\"groups\":[]"
"}"
;
