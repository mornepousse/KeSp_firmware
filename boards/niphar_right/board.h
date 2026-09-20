#ifndef BOARD_H
#define BOARD_H

/* Niphargus — moitié DROITE (U5), l'ESCLAVE.
 *
 * Brochage : docs/NIPHARGUS_V2_HARDWARE.md, vérifié à la netlist le 2026-08-06.
 * ⚠ La table de la moitié GAUCHE est différente (permutations de routage) —
 * ne jamais recopier l'une depuis l'autre.
 * Verrouillé par test/test_niphar_right_pins.c.
 */

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "driver/spi_master.h"
#endif

/* ── Identité produit ── */
#define GATTS_TAG           "Niphargus_R"
#define MANUFACTURER_NAME   "Mae"
#define PRODUCT_NAME        "Niphargus Right"
#define SERIAL_NUMBER       "N/A"
#define MODULE_ID           0x11

/* ── Matrice : COL → switch → diode → ROW ──────────────────────
 * Le scan PILOTE les colonnes et LIT les rows (BOARD_MATRIX_COL2ROW).
 * Table DROITE — PAS un miroir de la gauche : sur les 11 pins de la
 * matrice, une seule coïncide entre les deux moitiés (col3 = GPIO9,
 * routage commun) ; les 10 autres (row0..row3, col0..col2, col4..col6)
 * diffèrent. Ne jamais recopier l'une depuis l'autre. */
#define ROWS0  GPIO_NUM_2
#define ROWS1  GPIO_NUM_12
#define ROWS2  GPIO_NUM_4
#define ROWS3  GPIO_NUM_5

#define COLS0  GPIO_NUM_6
#define COLS1  GPIO_NUM_7
#define COLS2  GPIO_NUM_8
#define COLS3  GPIO_NUM_9
#define COLS4  GPIO_NUM_11
#define COLS5  GPIO_NUM_10
#define COLS6  GPIO_NUM_1

/* main/input/matrix_scan.c hardcode un initialiseur fixe COLS0..COLS12 /
 * ROWS0..ROWS4 (forme KaSe 5×13) quelle que soit la géométrie réelle du
 * board : matrix_arm_key_wake()/matrix_disarm_key_wake()/matrix_setup()
 * bornent leurs boucles sur MATRIX_COLS/MATRIX_ROWS (7/4 ici, corrects), mais
 * l'initialiseur C lui-même doit nommer 13 COLS et 5 ROWS pour compiler — les
 * valeurs en trop (COLS7..COLS12, ROWS4) sont des "excess elements"
 * silencieusement ignorés par le compilateur, jamais lues à l'exécution.
 * GPIO_NUM_NC : aucun pin réel n'existe au-delà de COLS6/ROWS3 sur cette
 * moitié, donc pas de numéro à inventer. Généraliser matrix_scan.c à une
 * géométrie arbitraire est hors scope de cette tâche (board.h uniquement). */
#define COLS7   GPIO_NUM_NC
#define COLS8   GPIO_NUM_NC
#define COLS9   GPIO_NUM_NC
#define COLS10  GPIO_NUM_NC
#define COLS11  GPIO_NUM_NC
#define COLS12  GPIO_NUM_NC
#define ROWS4   GPIO_NUM_NC

/* 26 touches, rangées de 7/7/6/6 : deux positions de la grille sont vides. */
#define MATRIX_ROWS  4
#define MATRIX_COLS  7

/* ── Radio nRF24L01+ (SPI2, partagé avec l'écran) ──────────────── */
#define BOARD_NRF_SPI_HOST   SPI2_HOST
#define BOARD_NRF_SCK        GPIO_NUM_38
#define BOARD_NRF_MISO       GPIO_NUM_39
#define BOARD_NRF_MOSI       GPIO_NUM_40
#define BOARD_NRF_CE         GPIO_NUM_15
#define BOARD_NRF_CSN        GPIO_NUM_16
#define BOARD_NRF_IRQ        GPIO_NUM_41
/* Alias attendus par la pile RF. comm/rf/kbd_relay_tx.c construit sa config
 * depuis BOARD_NRF_SPI_SCK, BOARD_NRF_CSN_GPIO... et son bloc de repli est
 * garde par `#ifndef BOARD_NRF_SPI_HOST` — que nous definissons ci-dessus.
 * Le repli est donc saute et ces alias doivent exister ici, sans quoi tout
 * consommateur de la pile RF cesse de compiler pour cette moitie.
 * Verrouilles par test/test_niphar_right_pins.c.
 *
 * Ni BOARD_NRF_CHANNEL ni BOARD_NRF_ADDR_SUFFIX : une moitie porte DEUX liens
 * (PRX vers l'autre moitie, PTX vers le dongle), un canal unique n'aurait pas
 * de sens. Leur choix appartient a B3/B4 — cf.
 * docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md. */
#define BOARD_NRF_SPI_SCK       BOARD_NRF_SCK
#define BOARD_NRF_SPI_MISO      BOARD_NRF_MISO
#define BOARD_NRF_SPI_MOSI      BOARD_NRF_MOSI
#define BOARD_NRF_CSN_GPIO      BOARD_NRF_CSN
#define BOARD_NRF_CE_GPIO       BOARD_NRF_CE
#define BOARD_NRF_IRQ_GPIO      BOARD_NRF_IRQ


/* ── Lien inter-moitiés (TRRS, UART1) ──────────────────────────
 * Câble droit : TX arrive sur TX. UNE moitié doit échanger TXD/RXD via la
 * matrice GPIO — c'est la GAUCHE. La droite ne swappe PAS : piloter les deux
 * TX sans swap d'un des deux côtés mettrait deux sorties en conflit sur le
 * même fil. */
#define BOARD_LINK_UART_NUM    1
#define BOARD_LINK_TX          GPIO_NUM_17
#define BOARD_LINK_RX          GPIO_NUM_18
#define BOARD_LINK_SWAP_TX_RX  0
/* ON du SiP32431, pull-down 100 k : le 5 V est MORT par défaut. Ne lever
 * qu'après une poignée de main aboutie (link_handshake.h). */
#define BOARD_LINK_5V_EN       GPIO_NUM_21

/* ── Jauge batterie ────────────────────────────────────────────
 * ADC2_CH2, diviseur 1M/1M + 100 nF. ADC2 est utilisable parce qu'il n'y a
 * pas de WiFi ; batterie pleine ≈ 4,15 V ÷ 2. */
#define BOARD_VBAT_SENSE_GPIO  GPIO_NUM_13

/* ── Pas de trackpad sur la droite (gauche uniquement) ── */

/* ── Écran Sharp LS011B7DH03 (module type nice!view, J4) ──────
 * Même module que la gauche (J12), même bus : partage le SPI du nRF24
 * (write-only, pas de MISO), CS ACTIF HAUT sur GPIO14, monté en PORTRAIT
 * (68 de large × 160 de haut). Pilote : display/memlcd, prouvé au banc le
 * 2026-09-14. Le CS est tenu BAS dès le boot et tiré bas en veille pour que
 * l'écran n'écoute jamais le trafic radio du bus partagé. */
#define BOARD_DISPLAY_BACKEND_MEMLCD
#define BOARD_LCD_CS_GPIO         GPIO_NUM_14
#define BOARD_LCD_CS_ACTIVE_HIGH  1
#define BOARD_LCD_ROTATE_180      0
#define BOARD_DISPLAY_WIDTH       68    /* PORTRAIT : 68 de large × 160 de haut */
#define BOARD_DISPLAY_HEIGHT      160
#define BOARD_DISPLAY_SLEEP_MS    60000

#define BOARD_HAS_LED_STRIP  0

/* ── Scan matrice ──────────────────────────────────────────────
 * SETTLING/RECOVERY à 0 reprend le réglage des boards KaSe. Suspecté de
 * participer aux frappes ratées (docs/DONGLE_ARCHI_ET_HALF_TYPING_2026-07-13.md,
 * bug #2) — à mesurer au banc, ne pas régler à l'aveugle. */
#define BOARD_MATRIX_COL2ROW
#define BOARD_MATRIX_SCAN_INTERVAL_US  1000
#define BOARD_MATRIX_SETTLING_US       0
#define BOARD_MATRIX_RECOVERY_US       0
#define BOARD_DEBOUNCE_TICKS           5   /* 5 ms : le dongle rejoue chaque transition, il ne masque plus un rebond de 3-5 ms (2026-09-20) */

/* ── USB ──
 * Pas d'USB HID utile en production sur la droite (elle n'a ni moteur keymap
 * ni sortie HID) ; VID/PID gardés pour cohérence si le port de prog est
 * énuméré au bring-up. */
#define BOARD_USB_VID  0xCafe
#define BOARD_USB_PID  0x4004

#endif /* BOARD_H */
