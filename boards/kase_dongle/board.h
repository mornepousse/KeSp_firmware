#ifndef BOARD_H
#define BOARD_H

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "sdkconfig.h"     /* CONFIG_KASE_DONGLE_FUSION — voir les dims plus bas */
#endif

/* ── Product info ──────────────────────────────────────────── */
#define GATTS_TAG           "KaSe_Dongle"
#define MANUFACTURER_NAME   "KaSe"
#define PRODUCT_NAME        "KaSe Dongle"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0xD0   /* dongle, distinct des halves 0x01/0x02 */

/* ── Pas de matrice ─────────────────────────────────────────────
 *
 * Cette carte déclarait une matrice 5×14, un brochage complet en GPIO_NUM_NC et
 * une keymap de 70 touches. Rien de tout cela n'existe : le dongle n'a aucun
 * interrupteur. C'était la trace de l'architecture A, où il fusionnait les
 * demi-matrices des anciennes moitiés et appliquait la keymap lui-même.
 *
 * Le Niphargus lui envoie du HID déjà fini, alors la déclaration ne décrivait
 * plus qu'un clavier imaginaire — et un `board.h` qui ment sur son matériel finit
 * par tromper quelqu'un. Le moteur d'entrée n'est plus compilé pour ce rôle
 * (main/CMakeLists.txt), le bloc CLAVIER du protocole CDC non plus, donc plus
 * rien n'en a besoin.
 */

/* ── Dimensions de fusion (KASE_DONGLE_FUSION uniquement) ────────────────────
 *
 * En mode fusion, le dongle RETROUVE le moteur keymap : il reçoit les DEUX
 * demi-matrices brutes des moitiés, les fusionne et sort le HID. Il partage donc
 * la géométrie et la keymap par défaut de la moitié GAUCHE (boards/niphar_left) —
 * mêmes dimensions, un seul fichier de keymap redirigé (board_keymap.c / .c ici
 * ne font qu'inclure ceux de la gauche). La NVS répliquée reste l'autorité
 * runtime (phase 3). Design : docs/superpowers/specs/2026-09-12-dongle-fusion-*.
 *
 * Ces macros n'existent PAS hors fusion : sans moteur, personne ne les lit, et un
 * board.h ne doit pas décrire une matrice que la carte n'a pas. C'est aussi
 * pourquoi elles sont gardées et non déclarées en dur. */
#if defined(CONFIG_KASE_DONGLE_FUSION)
#define MATRIX_ROWS  4
#define MATRIX_COLS  7
#define KEYMAP_COLS  14
/* Les deux moitiés sont le même PCB retourné : la droite se range en colonnes
 * hautes à l'envers. La conversion appartient au moteur (half_col_to_keymap). */
#define BOARD_REMOTE_COLS_MIRRORED  1
#endif

/* ── NRF24L01+ pinout (extracted from dongle.kicad_sch netlist) ─ */
#define BOARD_NRF_SPI_HOST       SPI2_HOST
#define BOARD_NRF_SPI_MOSI       GPIO_NUM_5
#define BOARD_NRF_SPI_MISO       GPIO_NUM_6
#define BOARD_NRF_SPI_SCK        GPIO_NUM_7
#define BOARD_NRF_SPI_CLOCK_HZ   (10 * 1000 * 1000)   /* 10 MHz, NRF24 datasheet max */

/* NRF#1 = slot clavier (moitié maître Niphargus), canal 0x4C par défaut */
#define BOARD_NRF1_CSN_GPIO      GPIO_NUM_13
#define BOARD_NRF1_CE_GPIO       GPIO_NUM_14
#define BOARD_NRF1_IRQ_GPIO      GPIO_NUM_8

/* NRF#2 = slot souris (Conchodytes), canal 0x52 par défaut */
#define BOARD_NRF2_CSN_GPIO      GPIO_NUM_1
#define BOARD_NRF2_CE_GPIO       GPIO_NUM_4
#define BOARD_NRF2_IRQ_GPIO      GPIO_NUM_2

/* ── No display backend on dongle ──────────────────────────── */
/* DELIBERATELY NO #define BOARD_DISPLAY_BACKEND_* here so root CMakeLists
 * detects the absence and skips display sources. */

/* ── Display sleep / deep sleep — no display, no batt → both 0 ─ */
#define BOARD_DISPLAY_SLEEP_MS    0
#define BOARD_SLEEP_MINS          0

/* ── No LED strip on dongle ────────────────────────────────── */
#define BOARD_HAS_LED_STRIP       0

/* ── USB identification ────────────────────────────────────── */
/* Dev VID/PID until first public release.
 * Migrate to pid.codes (VID 0x1209) before v4.0 release. */
#define BOARD_USB_VID             0x303A
#define BOARD_USB_PID             0x4001

#endif /* BOARD_H */
