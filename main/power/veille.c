/* Veille du Niphargus — brick B7. Voir veille.h pour le budget énergétique et
 * la raison pour laquelle la radio est éteinte dès l'étage léger. */
#include "veille.h"
#include "board.h"
#include "matrix_scan.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
#include "half_link.h"
#endif

static const char *TAG = "veille";

/* Seuils reglables au banc : eprouver le reveil EXT1 avec le defaut de 4 h
 * demanderait d'attendre quatre heures. Les valeurs de veille.h restent la
 * documentation et l'ancrage du test host. */
#ifndef CONFIG_KASE_VEILLE_LEGERE_S
#define CONFIG_KASE_VEILLE_LEGERE_S   60
#endif
#ifndef CONFIG_KASE_VEILLE_PROFONDE_S
#define CONFIG_KASE_VEILLE_PROFONDE_S 14400
#endif

/* Les tables de brochage sont dimensionnées par leur initialiseur, pas par
 * MATRIX_COLS/ROWS : les board.h complètent la queue avec GPIO_NUM_NC pour les
 * cartes plus petites (Niphargus 7×4). Les boucles s'arrêtent aux dimensions
 * réelles, donc ces entrées existent sans jamais être lues. Même raison que
 * dans matrix_arm_key_wake(). */
static const int s_cols[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                              COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
static const int s_rows[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

/* À appeler au DÉMARRAGE, avant matrix_setup().
 *
 * rtc_gpio_hold_en() survit délibérément au sommeil profond — c'est ce qui
 * maintient les colonnes hautes pendant que le domaine numérique est coupé, et
 * donc ce qui rend le réveil EXT1 possible. Mais le maintien survit AUSSI au
 * redémarrage : sans cette libération les colonnes restent figées, le pilote de
 * scan croit les piloter alors qu'elles ne bougent plus, et toute la matrice
 * lit n'importe quoi. gpio_reset_pin() ne suffit pas — il ne défait pas un
 * maintien posé par le multiplexeur RTC.
 *
 * Constaté au banc le 2026-09-08 : après un réveil EXT1, les touches étaient
 * complètement fausses. Inoffensif si aucun maintien n'est posé, donc appelé
 * sans condition. */
void veille_liberer_gpio(void)
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        rtc_gpio_hold_dis(s_cols[i]);
        rtc_gpio_deinit(s_cols[i]);
    }
    for (int i = 0; i < MATRIX_ROWS; i++) {
        rtc_gpio_hold_dis(s_rows[i]);
        rtc_gpio_deinit(s_rows[i]);
    }
}

void veille_legere_entrer(void)
{
    ESP_LOGI(TAG, "light sleep");

#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
    half_link_radio_sleep();      /* 900 nA au lieu de 26 µA en standby-I */
#endif
    rtc_matrix_deinit();          /* rendre les GPIO au réveil statique */
    matrix_arm_key_wake();

    esp_light_sleep_start();      /* bloque ici jusqu'à une touche */

    matrix_disarm_key_wake();
    /* AVANT tout : un réveil est une activité. Sinon la boucle clavier relit
     * 60 s d'inactivité au tour suivant et renvoie la carte dormir 10 ms après
     * son réveil — en boucle, 80 ms par cycle, tant que la touche est tenue. */
    matrix_mark_activity();
    matrix_setup();               /* recrée le pilote, relit les touches tenues */
#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
    half_link_radio_wake();
#endif
    ESP_LOGI(TAG, "reveil");
}

void veille_profonde_entrer(void)
{
    ESP_LOGW(TAG, "deep sleep — le reveil sera un redemarrage");

#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
    half_link_radio_sleep();
#endif
    rtc_matrix_deinit();

    /* Même montage électrique que le réveil léger : COL → interrupteur →
     * diode → ROW, donc on tient toutes les colonnes HAUT et on se réveille sur
     * n'importe quelle ligne qui monte. Aucun balayage n'est nécessaire, et
     * c'est ce qui rend la cible sous 50 µA atteignable — l'ULP coûterait à lui
     * seul 170 µA.
     *
     * En sommeil profond le domaine numérique est coupé : sans maintien RTC les
     * colonnes retomberaient et plus aucune touche ne pourrait réveiller la
     * carte. rtc_gpio_hold_en fige la broche telle qu'elle est. */
    for (int i = 0; i < MATRIX_COLS; i++) {
        rtc_gpio_init(s_cols[i]);
        rtc_gpio_set_direction(s_cols[i], RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(s_cols[i], 1);
        rtc_gpio_hold_en(s_cols[i]);
    }

    uint64_t masque = 0;
    for (int i = 0; i < MATRIX_ROWS; i++) {
        rtc_gpio_init(s_rows[i]);
        rtc_gpio_set_direction(s_rows[i], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(s_rows[i]);
        rtc_gpio_pulldown_en(s_rows[i]);
        masque |= 1ULL << s_rows[i];
    }

    esp_sleep_enable_ext1_wakeup_io(masque, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();       /* ne revient jamais : le réveil rebootera */
}

void veille_pas(uint32_t inactif_ms, bool bloque)
{
    switch (veille_niveau(inactif_ms, bloque,
                          (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u,
                          (uint32_t)CONFIG_KASE_VEILLE_PROFONDE_S * 1000u)) {
    case VEILLE_PROFONDE: veille_profonde_entrer(); break;   /* ne revient pas */
    case VEILLE_LEGERE:   veille_legere_entrer();   break;
    case VEILLE_AUCUNE:   break;
    }
}
