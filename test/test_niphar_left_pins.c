/* Contrat de brochage — Niphargus moitié GAUCHE (U6).
 *
 * Recopié à la main de docs/NIPHARGUS_V2_HARDWARE.md (netlist vérifiée le
 * 2026-08-06). Les deux moitiés ont des tables DIFFÉRENTES : ce sont des
 * permutations de routage, pas une symétrie. Aucune compilation ne détecte une
 * inversion, et les cartes ne sont pas arrivées — ce test est la seule barrière
 * avant le banc. Si le contrat change, c'est CE fichier qu'on met à jour en
 * premier, puis board.h.
 */
#include "test_framework.h"

/* ESP-IDF définit GPIO_NUM_* comme un enum ; sur host on les stube. */
#ifndef GPIO_NUM_0
#define GPIO_NUM_0  0
#define GPIO_NUM_1  1
#define GPIO_NUM_2  2
#define GPIO_NUM_4  4
#define GPIO_NUM_5  5
#define GPIO_NUM_6  6
#define GPIO_NUM_7  7
#define GPIO_NUM_8  8
#define GPIO_NUM_9  9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_21 21
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define GPIO_NUM_NC (-1)
#define SPI2_HOST   1
#endif

#include "../boards/niphar_left/board.h"
#include "../main/comm/rf/rf_slot.h"

/* Garde de compilation : la gauche a LE MÊME écran que la droite depuis le
 * 2026-09-14 (Sharp LS011B7DH03 soudé sur J12, portrait 68×160, CS actif haut
 * sur GPIO14, SPI partagé avec le nRF24). CMakeLists.txt lit le TEXTE de board.h
 * pour choisir le backend : un BOARD_DISPLAY_BACKEND_ROUND/OLED égaré ici
 * changerait le build sans qu'aucun test bronche — on fait planter la
 * compilation. (Jusqu'au 2026-09-14 ce test interdisait TOUTE macro écran :
 * J12 était déclaré vide ; le matériel a changé, le contrat le suit.) */
#ifndef BOARD_DISPLAY_BACKEND_MEMLCD
#error "la gauche a un écran Sharp memory-LCD (J12 soudé) : BOARD_DISPLAY_BACKEND_MEMLCD manque dans boards/niphar_left/board.h"
#endif
#ifdef BOARD_DISPLAY_BACKEND_ROUND
#error "la gauche n'a pas d'écran rond : BOARD_DISPLAY_BACKEND_ROUND changerait le backend choisi par CMakeLists.txt"
#endif
#ifdef BOARD_DISPLAY_BACKEND_OLED
#error "la gauche n'a pas d'OLED : BOARD_DISPLAY_BACKEND_OLED changerait le backend choisi par CMakeLists.txt"
#endif

/* GPIO non câblés : strapping et PSRAM octale. Aucun pin du board ne doit
 * tomber dedans. */
static int is_forbidden(int gpio)
{
    return gpio == 3 || gpio == 45 || gpio == 46 ||
           gpio == 35 || gpio == 36 || gpio == 37;
}

/* GPIO déjà engagés à autre chose que la matrice/périphériques du board :
 * USB D-/D+ (19/20, NIPHARGUS_V2_HARDWARE.md:41) et le connecteur de prog
 * (0, 43, 44, :47). La gauche existe POUR l'USB — une colonne posée sur
 * GPIO19 passerait test_left_no_forbidden_gpio (pas dans la liste des non
 * câblés) sans ce second contrôle. */
static int is_reserved(int gpio)
{
    return gpio == 19 || gpio == 20 ||
           gpio == 0 || gpio == 43 || gpio == 44;
}

static void test_left_matrix_table(void)
{
    /* Table GAUCHE du contrat — NE PAS recopier depuis la table droite. */
    TEST_ASSERT_EQ(ROWS0, 1,  "gauche row0 = GPIO1");
    TEST_ASSERT_EQ(ROWS1, 2,  "gauche row1 = GPIO2");
    TEST_ASSERT_EQ(ROWS2, 8,  "gauche row2 = GPIO8");
    TEST_ASSERT_EQ(ROWS3, 6,  "gauche row3 = GPIO6");

    TEST_ASSERT_EQ(COLS0, 4,  "gauche col0 = GPIO4");
    TEST_ASSERT_EQ(COLS1, 5,  "gauche col1 = GPIO5");
    TEST_ASSERT_EQ(COLS2, 7,  "gauche col2 = GPIO7");
    TEST_ASSERT_EQ(COLS3, 9,  "gauche col3 = GPIO9");
    TEST_ASSERT_EQ(COLS4, 10, "gauche col4 = GPIO10");
    TEST_ASSERT_EQ(COLS5, 11, "gauche col5 = GPIO11");
    TEST_ASSERT_EQ(COLS6, 12, "gauche col6 = GPIO12");
}

static void test_left_matrix_geometry(void)
{
    TEST_ASSERT_EQ(MATRIX_ROWS, 4, "4 rangées");
    TEST_ASSERT_EQ(MATRIX_COLS, 7, "7 colonnes");
}

static void test_left_peripheral_pins(void)
{
    TEST_ASSERT_EQ(BOARD_NRF_SCK,  38, "SPI SCK partagé");
    TEST_ASSERT_EQ(BOARD_NRF_MISO, 39, "SPI MISO partagé");
    TEST_ASSERT_EQ(BOARD_NRF_MOSI, 40, "SPI MOSI partagé");
    TEST_ASSERT_EQ(BOARD_NRF_CE,   15, "nRF24 CE");
    TEST_ASSERT_EQ(BOARD_NRF_CSN,  16, "nRF24 CSN");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ,  41, "nRF24 IRQ");

    TEST_ASSERT_EQ(BOARD_LINK_TX,    17, "TRRS TX (UART1)");
    TEST_ASSERT_EQ(BOARD_LINK_RX,    18, "TRRS RX (UART1)");
    TEST_ASSERT_EQ(BOARD_LINK_5V_EN, 21, "ON du SiP32431");

    TEST_ASSERT_EQ(BOARD_VBAT_SENSE_GPIO, 13, "jauge ADC2_CH2");

    /* Trackpad : gauche uniquement. */
    TEST_ASSERT_EQ(BOARD_TRACK_SDA_GPIO, 47, "trackpad SDA");
    TEST_ASSERT_EQ(BOARD_TRACK_SCL_GPIO, 48, "trackpad SCL");
    TEST_ASSERT_EQ(BOARD_TRACK_RDY_GPIO, 42, "trackpad RDY");
    TEST_ASSERT_EQ(BOARD_HAS_TRACKPAD_LOCAL, 1, "le trackpad est sur la gauche");
}

static void test_left_no_forbidden_gpio(void)
{
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_forbidden(pins[i]), "aucun pin sur un GPIO non câblé");
}

static void test_left_no_reserved_gpio(void)
{
    /* USB D-/D+ et connecteur de prog : engagés ailleurs, pas dans la liste
     * des non-câblés donc invisibles à test_left_no_forbidden_gpio. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        TEST_ASSERT(!is_reserved(pins[i]), "aucun pin sur l'USB natif ou le connecteur de prog");
}

static void test_left_no_pin_used_twice(void)
{
    /* Une permutation ratée produit typiquement un doublon — sur TOUS les
     * pins du board (matrice + SPI + nRF + lien + jauge + trackpad), pas
     * seulement la matrice : à droite, COLS6/ROWS0 sont à un chiffre des
     * numéros de radio, une faute de frappe qui poserait BOARD_NRF_CE sur un
     * pin matrice ne serait pas vue si on ne regardait que la matrice. */
    const int pins[] = {
        ROWS0, ROWS1, ROWS2, ROWS3,
        COLS0, COLS1, COLS2, COLS3, COLS4, COLS5, COLS6,
        BOARD_NRF_SCK, BOARD_NRF_MISO, BOARD_NRF_MOSI,
        BOARD_NRF_CE, BOARD_NRF_CSN, BOARD_NRF_IRQ,
        BOARD_LINK_TX, BOARD_LINK_RX, BOARD_LINK_5V_EN,
        BOARD_VBAT_SENSE_GPIO,
        BOARD_TRACK_SDA_GPIO, BOARD_TRACK_SCL_GPIO, BOARD_TRACK_RDY_GPIO,
    };
    const unsigned n = sizeof(pins) / sizeof(pins[0]);
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = i + 1; j < n; j++)
            TEST_ASSERT(pins[i] != pins[j], "aucun GPIO en double sur tout le board");
}

static void test_left_swaps_the_link_uart(void)
{
    /* Câble droit : TX arrive sur TX. UNE moitié doit échanger TXD/RXD via la
     * matrice GPIO — on a choisi la gauche. La droite ne doit PAS swapper
     * (vérifié dans test_niphar_right_pins.c). */
    TEST_ASSERT_EQ(BOARD_LINK_SWAP_TX_RX, 1, "la gauche swappe TX/RX");
}

/* Les consommateurs de la pile RF (comm/rf/kbd_relay_tx.c) construisent leur
 * config depuis BOARD_NRF_SPI_SCK, BOARD_NRF_CSN_GPIO... Ces noms different de
 * ceux du contrat materiel (BOARD_NRF_SCK, BOARD_NRF_CSN), et kbd_relay_tx.c
 * fournit un bloc de repli — GPIO 35/36/37, interdits ici — sous
 * `#ifndef BOARD_NRF_SPI_HOST`.
 *
 * Les board.h Niphargus DEFINISSENT BOARD_NRF_SPI_HOST : le garde est donc
 * faux, le bloc de repli entier est saute, et les alias ne sont definis nulle
 * part. Le mode de defaillance n'est pas un mauvais brochage silencieux mais
 * une erreur de compilation, le jour ou un consommateur de la pile RF sera
 * compile pour une moitie. C'est moins grave, ca reste a fermer.
 *
 * boards/conchodytes/board.h a du ajouter ce bloc d'alias pour la meme raison,
 * en documentant le piege. Les deux moities l'avaient oublie. Ce test
 * verrouille l'equivalence : un alias absent ne compile pas, un alias qui
 * derive echoue ici — pas au banc, six mois plus tard.
 *
 * On n'asserte volontairement NI canal NI suffixe d'adresse : contrairement a
 * la souris qui n'a qu'un lien, une moitie en a deux (PRX vers l'autre moitie,
 * PTX vers le dongle). Le choix des canaux appartient a B3/B4. */
static void test_left_radio_pin_aliases(void)
{
    TEST_ASSERT_EQ(BOARD_NRF_SPI_SCK,  BOARD_NRF_SCK,  "alias SCK  == pin brute");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MISO, BOARD_NRF_MISO, "alias MISO == pin brute");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_MOSI, BOARD_NRF_MOSI, "alias MOSI == pin brute");
    TEST_ASSERT_EQ(BOARD_NRF_CSN_GPIO, BOARD_NRF_CSN,  "alias CSN  == pin brute");
    TEST_ASSERT_EQ(BOARD_NRF_CE_GPIO,  BOARD_NRF_CE,   "alias CE   == pin brute");
    TEST_ASSERT_EQ(BOARD_NRF_IRQ_GPIO, BOARD_NRF_IRQ,  "alias IRQ  == pin brute");

    /* Lien maitre -> dongle (B4). comm/rf/kbd_relay_tx.c consomme ces deux
     * macros pour s'adresser au dongle ; elles doivent s'accorder avec
     * boards/kase_dongle/board_rf.h (radio 1) ou le lien ne s'etablit jamais.
     * Ce n'est PAS le lien droite -> gauche, qui aura ses propres macros a B3. */
    /* Ecrit en dur dans board.h : on le relie au plan de canaux, sinon les deux
     * derivent en silence et le lien se tait sans qu'aucun test ne bronche. */
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL, RF_CH_KBD_DONGLE, "canal == plan de canaux");
    TEST_ASSERT_EQ(BOARD_NRF_CHANNEL,     0x4C, "canal du slot clavier, cf. board_rf.h du dongle");
    TEST_ASSERT_EQ(BOARD_NRF_ADDR_SUFFIX, 0x01, "suffixe d'adresse = slot clavier (rf_slot.h)");
    TEST_ASSERT_EQ(BOARD_NRF_SPI_CLOCK_HZ, BOARD_NRF_CLOCK_HZ, "alias horloge SPI");
}

/* L'écran de la gauche est le MÊME module que celui de la droite, au même CS :
 * test_niphar_right_pins.c vérifie 14 / actif haut de son côté ; les deux
 * moitiés doivent rester alignées (même pilote, même bus partagé). */
static void test_ecran_memlcd_gauche(void)
{
    TEST_ASSERT_EQ(BOARD_LCD_CS_GPIO, 14, "CS de l'écran Sharp (comme la droite)");
    TEST_ASSERT_EQ(BOARD_LCD_CS_ACTIVE_HIGH, 1, "CS actif HAUT, pas bas");
    TEST_ASSERT_EQ(BOARD_DISPLAY_WIDTH, 68, "portrait : 68 px de large");
    TEST_ASSERT_EQ(BOARD_DISPLAY_HEIGHT, 160, "portrait : 160 px de haut");
    TEST_ASSERT(BOARD_LCD_CS_GPIO != BOARD_NRF_CSN && BOARD_LCD_CS_GPIO != BOARD_NRF_CE,
                "le CS écran n'est ni le CSN ni le CE de la radio (bus partagé)");
}

void test_niphar_left_pins(void)
{
    printf("\n-- brochage Niphargus GAUCHE (contrat netlist 2026-08-06) --\n");
    test_left_matrix_table();
    test_left_matrix_geometry();
    test_left_peripheral_pins();
    test_left_no_forbidden_gpio();
    test_left_no_reserved_gpio();
    test_left_no_pin_used_twice();
    test_left_swaps_the_link_uart();
    test_left_radio_pin_aliases();
    test_ecran_memlcd_gauche();
}
