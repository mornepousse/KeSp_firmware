/* Veille du Niphargus — brick B7. Voir veille.h pour le budget énergétique et
 * la raison pour laquelle la radio est éteinte dès l'étage léger. */
#include "veille.h"
#include "board.h"
#include "matrix_scan.h"
#include "matrix_flag.h"
#if CONFIG_KASE_HALF_LINK_RX
#include "key_processor.h"
#include "hid_report.h"
#endif
#include "esp_log.h"
#include "esp_sleep.h"
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#endif
#include "esp_timer.h"
#include "tinyusb.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
#include "half_link.h"
#endif

static const char *TAG = "veille";

/* Bilan de sommeil depuis le boot, pour lire une nuit : veille_bilan(). */
static uint32_t s_sommeils, s_dormi_ms;
static uint32_t s_dernier_reveil_ms;   /* pour veille_en_grace (0 = jamais) */
void veille_bilan(uint32_t *sommeils, uint32_t *dormi_ms)
{
    if (sommeils) *sommeils = s_sommeils;
    if (dormi_ms) *dormi_ms = s_dormi_ms;
}

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

    /* Se retirer du bus USB AVANT de dormir. Le light sleep coupe la PHY : vu
     * de l'hôte c'est un débranchement brutal, et au réveil TinyUSB retrouve
     * un contrôleur OTG dans un état indéfini — la moitié droite ne se
     * réveillait plus tant qu'elle était branchée, seul un reset la ramenait.
     * Constaté au banc le 2026-09-11 : 65 s de fonctionnement branchée, puis
     * silence complet au premier sommeil. Sur batterie, le réveil marchait.
     *
     * tud_disconnect() relâche le pull-up D+ proprement : l'hôte voit un
     * appareil qui se retire, pas un qui disparaît. tud_connect() au réveil
     * ré-énumère en ~1 s. Un appareil qui dort n'a rien à faire sur le bus. */
    bool etait_connecte = tud_mounted();
    if (etait_connecte) tud_disconnect();

    /* Le sommeil PROFOND était inatteignable : l'inactivité n'est évaluée
     * qu'éveillé, et la carte reste bloquée ici jusqu'à une touche — qui remet
     * le compteur à zéro. Un réveil par timer au seuil profond (moins ce qui
     * est déjà écoulé) bascule en deep sleep sans passer par une frappe
     * (l'utilisateur, 2026-09-15 : « il ne part jamais en deep sleep »). */
    uint64_t reste_us = (uint64_t)(CONFIG_KASE_VEILLE_PROFONDE_S - CONFIG_KASE_VEILLE_LEGERE_S) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(reste_us);
    uint64_t avant_us = (uint64_t)esp_timer_get_time();   /* esp_timer suit le RTC : seul témoin fiable */
    esp_light_sleep_start();      /* bloque ici jusqu'à une touche, ou le timer */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    uint32_t dormi_ms = (uint32_t)(((uint64_t)esp_timer_get_time() - avant_us) / 1000);
    s_sommeils++; s_dormi_ms += dormi_ms;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGW(TAG, "%lu s de sommeil leger sans une touche : sommeil profond", (unsigned long)(dormi_ms / 1000));
        veille_profonde_entrer();   /* ne revient pas ; la radio est déjà éteinte */
    }
    { uint32_t t = (uint32_t)(esp_timer_get_time() / 1000); s_dernier_reveil_ms = t ? t : 1; }
    /* AVANT tout : prouver le réveil ET le nommer. cause=7 est ESP_SLEEP_WAKEUP_GPIO
     * et le masque dit quelle ligne ; toute autre cause est un réveil qu'on n'a
     * pas demandé. Et dire COMBIEN on a dormi : une nuit à 0,2 V perdus
     * (= ~20 mA) est indiscernable d'une nuit à 244 µA sans ce chiffre. */
    ESP_LOGW(TAG, "reveil apres %lu s de sommeil (cause=%d) — cumul : %lu sommeils, %lu s dormies sur %lu s",
             (unsigned long)(dormi_ms / 1000), (int)esp_sleep_get_wakeup_cause(),
             (unsigned long)s_sommeils, (unsigned long)(s_dormi_ms / 1000),
             (unsigned long)(esp_timer_get_time() / 1000000));
#if CONFIG_KASE_HALF_LINK_RX
    half_link_note_wake();
#endif

    if (etait_connecte) tud_connect();

    /* L'ordre est le cœur du correctif, chaque étape a sa raison :
     *
     * 1. Radio d'abord (~7 ms) : tout ce qui suit émet, et une émission sur une
     *    puce éteinte est un rapport perdu — la droite envoie directement, la
     *    gauche par excursion.
     * 2. Capture : la touche qui a réveillé la carte est enfoncée MAINTENANT ;
     *    recréer le pilote prend des dizaines de ms, une frappe brève serait
     *    relâchée avant. Elle tamponne aussi l'activité, sans quoi la boucle
     *    clavier renverrait la carte dormir 10 ms plus tard.
     * 3. Émettre l'appui TOUT DE SUITE (gauche) : le tampon de rapport est
     *    unique, un relâchement publié ensuite l'écraserait avant l'envoi.
     * 4. Recréer le pilote, puis lui laisser 10 ms pour un premier balayage.
     * 5. Réconcilier : s'il n'a rien dit, la touche a été relâchée entre-temps
     *    et il ne le dira jamais — publier et émettre le relâchement, sinon
     *    elle reste collée jusqu'au prochain événement de cette moitié. */
#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX
    half_link_radio_wake();
#endif
    matrix_wake_capture();
    /* Premier front dans le REBOND du contact : la capture 2 passes (1 ms) lit
     * alors 0 touche, la carte conclut « fantôme », se rendort 10-50 ms et se
     * re-réveille sur la touche encore tenue — vu deux fois au banc le
     * 2026-09-13. Un tap bref relâché pendant ce rendormissement est PERDU (un
     * relâchement ne réveille pas). Si le réveil vient bien d'un GPIO et que la
     * capture est vide, on relit UNE fois 5 ms plus tard avant de trancher : un
     * vrai glitch donne deux captures vides (filtre intact), un vrai appui est
     * rattrapé sans double réveil. */
    if (!matrix_wake_had_keys() && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        esp_rom_delay_us(5000);
        matrix_wake_capture();
    }
    /* BANC 2026-09-16 (gauche) : la touche qui réveille n'est vue ni par les deux
     * captures ni par le pilote — 5 appuis, 4 vus, le premier manque. Soit le
     * réveil arrive ~150 ms après l'appui (touche déjà relâchée), soit la
     * lecture des lignes est fausse pendant les premières ms après le réveil
     * (la droite lit 7 ms plus tard, après sa radio, et capture presque
     * toujours). On relit toutes les 10 ms pendant 150 ms et on note À QUEL
     * DÉLAI une touche apparaît : « jamais » = réveil tardif ou glitch ;
     * « à 10-20 ms » = lecture précoce fausse ; « à 100 ms+ » = appui suivant. */
    if (!matrix_wake_had_keys() && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        int trouve_ms = -1;
        for (int d = 10; d <= 150; d += 10) {
            esp_rom_delay_us(10000);
            matrix_wake_capture();
            if (matrix_wake_had_keys()) { trouve_ms = d + 6; break; }
        }
        ESP_LOGW(TAG, "capture vide au reveil : touche %s", trouve_ms < 0 ? "JAMAIS vue en 156 ms" : "vue plus tard");
        if (trouve_ms >= 0) ESP_LOGW(TAG, "  apparue a +%d ms apres le reveil", trouve_ms);
    }
#if CONFIG_KASE_BATT_SENSE
    batt_sense_sample_now();   /* une mesure au réveil : le timer était gelé */
#endif
#if CONFIG_KASE_HALF_LINK_RX
    if (matrix_flag_take(&stat_matrix_changed)) {
        build_keycode_report();
        send_hid_key();
    }
#endif
    matrix_disarm_key_wake();
    matrix_setup();
    /* ⚠ Pas de vTaskDelay(1) ici : un tick nu attend jusqu'à la PROCHAINE
     * frontière de tick, entre ~0 et 10 ms — pas « 10 ms ». Quand la phase
     * tombait mal, le pilote n'avait pas fini son anti-rebond, son silence
     * passait pour un relâchement, et une touche TENUE était relâchée au dongle
     * 20 ms après le réveil (Super tenu → tap de Super → lanceur → Super+F
     * perdu, banc 2026-09-13). On attend son premier événement, ou la grâce
     * déduite de son anti-rebond (wake_grace.h). */
    matrix_wake_wait_first_scan();
    if (matrix_wake_reconcile()) {
#if CONFIG_KASE_HALF_LINK_RX
        (void)matrix_flag_take(&stat_matrix_changed);
        build_keycode_report();
        send_hid_key();
#endif
    }
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

void veille_diag(uint32_t inactif_ms, bool usb, bool lien)
{
    static uint32_t dernier_ms;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (inactif_ms < (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u) return;
    if (!usb && !lien) return;                       /* rien ne bloque : on va dormir */
    if ((uint32_t)(now - dernier_ms) < 30000u) return;
    dernier_ms = now;
    ESP_LOGW(TAG, "veille REFUSEE depuis %lu s : usb=%d lien=%d",
             (unsigned long)(inactif_ms / 1000), (int)usb, (int)lien);
}

void veille_pas(uint32_t inactif_ms, bool bloque)
{
    /* Grâce après réveil (veille.h) : la touche qui a réveillé la carte peut
     * n'avoir pas encore été vue (pré-contact lent, capture vide) ; on laisse
     * au pilote recréé le temps de la voir et de l'émettre avant de dormir. */
    if (veille_en_grace((uint32_t)(esp_timer_get_time() / 1000), s_dernier_reveil_ms, VEILLE_GRACE_REVEIL_MS))
        return;
    veille_t niveau = veille_niveau(inactif_ms, bloque,
                                    (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u,
                                    (uint32_t)CONFIG_KASE_VEILLE_PROFONDE_S * 1000u);

#if CONFIG_KASE_HALF_LINK_TX
    /* Moitié droite : si l'USB a été monté, PROFOND plutôt que LÉGER.
     *
     * En light sleep les horloges de la PHY USB sont gelées ; la doc ESP-IDF
     * (USB Serial/JTAG Console, « Sleep Mode Considerations ») prévient que
     * l'hôte peut déclarer l'appareil en erreur, qu'il peut ne pas le
     * ré-énumérer à la sortie, et qu'ESP-IDF ne refuse pas l'entrée en sommeil
     * câble branché. Constaté au banc le 2026-09-11 : la droite branchée
     * s'endormait puis ne se réveillait plus, seul un reset la ramenait.
     *
     * Il n'y a pas de bon signal de présence sans pont VBUS : tud_mounted()
     * reste vrai après un débranchement à chaud, tud_ready() suit l'autosuspend
     * de l'hôte (un CDC que personne n'ouvre est suspendu en 2 s). Alors on
     * contourne : le deep sleep éteint la PHY proprement — déconnexion et
     * reconnexion normales, dit la même doc — et son réveil EXT1 est un
     * REDÉMARRAGE, qui remet tud_mounted() à faux. Après un débranchement à
     * chaud, un seul réveil coûte 700 ms, puis la carte revient au light sleep
     * d'elle-même. Dégradation gracieuse et auto-réparante, plutôt qu'un
     * clavier mort ou une batterie vidée.
     *
     * La gauche n'est pas concernée : clavier HID, l'hôte ne la suspend pas,
     * elle ne dort jamais branchée. */
    if (niveau == VEILLE_LEGERE && tud_mounted()) {
        ESP_LOGW(TAG, "USB monte : sommeil profond plutot que leger (PHY USB)");
        niveau = VEILLE_PROFONDE;
    }
#endif

    switch (niveau) {
    case VEILLE_PROFONDE: veille_profonde_entrer(); break;   /* ne revient pas */
    case VEILLE_LEGERE:   veille_legere_entrer();   break;
    case VEILLE_AUCUNE:   break;
    }
}
