#include "half_link.h"
#include "board.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "rf_slot.h"
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_pairing.h"   /* fusion : la droite s'adresse au slot clavier du dongle */
#include "esp_mac.h"      /* esp_read_mac — REQ d'appairage */
#include "esp_system.h"   /* esp_restart — après appairage */
#endif
#include "driver/gpio.h"
#include "esp_log.h"
#if CONFIG_KASE_VEILLE
#include "veille.h"
#if CONFIG_KASE_LINK_WIRE
#include "link_uart.h"
#endif
#include "matrix_scan.h"
#include "tinyusb.h"
#endif
#include "esp_attr.h"
#include "esp_timer.h"
#if CONFIG_PM_PROFILING
#include "esp_pm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "half_link";

static rf_radio_t s_radio;
static uint8_t    s_seq;


#if CONFIG_KASE_HALF_LINK_TX
/* Dernier état émis, et quand. Partagé entre DEUX contextes de tâche : le
 * callback du pilote keyboard_button (émission immédiate sur changement) et la
 * tâche de rafraîchissement ci-dessous. Quatre octets, mais une copie déchirée
 * enverrait une matrice qui n'a jamais existé — d'où le verrou, très court. */
static uint8_t  s_etat_local[RF_HALF_BITMAP_BYTES];
static uint32_t s_dernier_tx_ms;
static portMUX_TYPE s_etat_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_refresh_task;   /* notifiée sur changement : cadence rapide sans attendre */

/* Verrou de la PUCE côté émetteur. Jusqu'au 2026-09-07 un seul appelant
 * existait — le callback du pilote keyboard_button — et rf_driver_send pouvait
 * s'en passer. La tâche de rafraîchissement en ajoute un second.
 *
 * rf_driver_send pilote CSN à la main (spics_io_num = -1) : deux tâches qui
 * s'entrelacent entre csn_low() et csn_high() envoient une transaction avec CSN
 * dans le mauvais état — écriture de registre perdue en silence, ou impulsion CE
 * tronquée. La moitié gauche a reçu son verrou, la droite avait été oubliee. */
static SemaphoreHandle_t s_tx_radio_mux;
/* Config TX vivante, pour le chien de garde radio (réarmer une puce figée). */
static rf_radio_cfg_t s_tx_cfg;
static uint8_t        s_sans_ack_ecran = 3;   /* émissions consécutives sans ACK : « dongle vu » pour l'écran (3 = pas encore vu) */
#if CONFIG_KASE_DONGLE_FUSION
/* Repli sans dongle : deux cibles d'émission. La droite vise le dongle par
 * défaut (s_cfg_dongle, MATRIX sur KaSe.01) ; si le dongle disparaît, elle
 * bascule vers la gauche-USB (s_cfg_left, HEARTBEAT sur KaSe.03, protocole
 * pré-fusion que kbd_relay décode). half_link.h porte la FSM pure. */
static rf_radio_cfg_t s_cfg_dongle;   /* KaSe.01, set_id — le dongle tape */
static rf_radio_cfg_t s_cfg_left;     /* KaSe.03 fixe — la gauche-USB tape */
static half_tx_fsm_t  s_tx_fsm = { HALF_TX_TO_DONGLE, 0 };
#endif
#endif

/* Config commune aux deux bouts : même canal, même adresse, sinon rien ne
 * passe (nRF24L01+ PS §6.3 : « You must program a transmitter and a receiver
 * with the same RF channel frequency to communicate with each other »). */
static rf_radio_cfg_t half_link_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host         = BOARD_NRF_SPI_HOST,
        .pin_mosi         = BOARD_NRF_MOSI,
        .pin_miso         = BOARD_NRF_MISO,
        .pin_sck          = BOARD_NRF_SCK,
        .clock_hz         = 8 * 1000 * 1000,
        .pin_csn          = BOARD_NRF_CSN,
        .pin_ce           = BOARD_NRF_CE,
        .pin_irq          = BOARD_NRF_IRQ,
        .channel          = RF_CH_HALF_LINK,
        .rx_addr          = { 'K', 'a', 'S', 'e' },
        .addr_suffix      = RF_ADDR_HALF_LINK,
        .shares_bus_first = true,
    };
    return c;
}

#if CONFIG_KASE_HAS_RF_TX

#if CONFIG_KASE_DONGLE_FUSION
/* Appairage actif de la DROITE au dongle (fusion). Réplique le flux prouvé de
 * kbd_pairing_task (kbd_relay_tx.c) : REQ sur le rendez-vous en déclarant le
 * SLOT CLAVIER (0x01, la droite partage l'adresse de la gauche), attente de
 * l'ACK qui porte le set_id, sauvegarde NVS, redémarrage — au reboot,
 * half_link_tx_init charge le set_id et vise la bonne adresse.
 *
 * La droite déclare 0x01 : rf_pairing_resolve_slot honore le slot déclaré, donc
 * le dongle l'assigne au clavier sans toucher au slot souris (0x02). Les deux
 * moitiés finissent sur la même adresse, distinguées par l'identité de moitié
 * dans PKT_TYPE_MATRIX.
 *
 * ⚠ Le dongle doit avoir sa fenêtre d'appairage OUVERTE (KS_CMD_RF_PAIR_START).
 * Utilise s_radio, déjà initialisée en PTX par half_link_tx_init. */
static void half_fusion_pairing_task(void *arg)
{
    (void)arg;
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    uint8_t req[8];
    rf_encode_pair_req(req, my_mac, RF_ADDR_KBD_DONGLE);   /* déclare le slot clavier */
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;

    rf_pair_ack_t ack;
    bool acked = false;
    /* ~30 s de tentatives : laisse le temps d'ouvrir la fenêtre du dongle. */
    for (int i = 0; i < 200 && !acked; i++) {
        rf_driver_set_tx_address(&s_radio, pair_addr);
        rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
        rf_driver_send(&s_radio, req, 8);
        uint8_t rxb[32];
        uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                           rxb, sizeof(rxb), 150);
        if (n && rf_decode_pair_ack(rxb, n, &ack)) { acked = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (acked) {
        /* On sauvegarde TOUJOURS le slot clavier : la droite partage l'adresse de
         * la gauche, quel que soit le slot renvoyé par le dongle. */
        ESP_LOGW(TAG, "fusion appairage : ACK set_id=0x%04X — sauvegarde + reboot",
                 ack.set_id);
        rf_pairing_save_half(ack.set_id, RF_ADDR_KBD_DONGLE, ack.dongle_wifi_mac);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "fusion appairage : pas d'ACK — ouvrir la fenêtre du dongle "
                  "(KS_CMD_RF_PAIR_START) puis redémarrer la droite");
    vTaskDelete(NULL);
}
#endif /* CONFIG_KASE_DONGLE_FUSION */

bool half_link_tx_init(void)
{
    s_tx_radio_mux = xSemaphoreCreateMutex();
    if (!s_tx_radio_mux) {
        ESP_LOGE(TAG, "mutex radio TX non cree — emission abandonnee");
        return false;
    }
    rf_radio_cfg_t cfg = half_link_cfg();
#if CONFIG_KASE_DONGLE_FUSION
    /* La config de base (KaSe.03 fixe, canal du lien) EST la cible « gauche
     * directe » : c'est exactement ce que la gauche-USB écoute. On la mémorise
     * AVANT de retargeter vers le dongle, pour le repli sans dongle. */
    s_cfg_left = cfg;
    /* Fusion : la droite ne parle plus à la gauche mais au SLOT CLAVIER DU DONGLE,
     * comme la gauche (même adresse, distinction par l'identité de moitié dans
     * PKT_TYPE_MATRIX). Canal et suffixe du slot clavier, adresse dérivée du
     * set_id d'appairage. Non appairée → on lance l'appairage actif (plus bas). */
    bool fusion_unpaired = false;
    {
        uint8_t slot = RF_ADDR_KBD_DONGLE;
        uint16_t set_id = rf_pairing_load_set_id_half(RF_ADDR_KBD_DONGLE, &slot);
        cfg.channel     = RF_CH_KBD_DONGLE;
        cfg.addr_suffix = RF_ADDR_KBD_DONGLE;
        rf_apply_set_id(&cfg, set_id, RF_ADDR_KBD_DONGLE);
        fusion_unpaired = (set_id == 0 || set_id == 0xFFFF);
        ESP_LOGW(TAG, "fusion : TX vers le dongle ch=0x%02X suffixe=0x%02X set_id=0x%04X%s",
                 cfg.channel, cfg.addr_suffix, set_id,
                 fusion_unpaired ? " (NON APPAIRE — appairage actif)" : "");
    }
#endif
    s_tx_cfg = cfg;   /* mémorisé pour le chien de garde radio */
#if CONFIG_KASE_DONGLE_FUSION
    s_cfg_dongle    = cfg;                 /* cible par défaut : le dongle */
    s_tx_fsm.cible  = HALF_TX_TO_DONGLE;   /* au boot, on vise le dongle */
    s_tx_fsm.sans_ack = 0;
#endif
    esp_err_t e = rf_driver_init_tx(&s_radio, &cfg);
    if (e != ESP_OK || !s_radio.present) {
        ESP_LOGE(TAG, "TX init echouee (%d) — la moitie droite restera muette", (int)e);
        return false;
    }
    ESP_LOGI(TAG, "TX pret : ch=0x%02X addr=KaSe.%02X", cfg.channel, cfg.addr_suffix);

#if CONFIG_KASE_DONGLE_FUSION
    if (fusion_unpaired) {
        /* Pas d'épreuve ni d'émission normale tant qu'on n'a pas de set_id : on
         * viserait l'adresse d'usine et le dongle n'acquitterait pas. On lance
         * l'appairage actif, qui redémarre la carte une fois l'ACK reçu. */
        ESP_LOGW(TAG, "fusion : appairage actif au dongle — ouvrir sa fenêtre");
        xTaskCreate(half_fusion_pairing_task, "half_pair", 4096, NULL, 5, NULL);
        return true;
    }
#endif

    /* Rafale d'epreuve au demarrage. Sans elle, savoir si le lien porte
     * dependrait de quelqu'un appuyant sur une touche PENDANT qu'on ecoute la
     * console — synchronisation peu commode entre deux operateurs. Ici un
     * simple reset suffit a obtenir le verdict.
     *
     * L'acquittement est MATERIEL : le nRF24 d'en face repond de lui-meme si
     * canal et adresse concordent, sans que son logiciel intervienne. Un taux
     * eleve prouve donc que la radio de la gauche ecoute sur le bon canal,
     * meme si sa couche applicative avait un probleme par ailleurs. */
    {
        const int N = 10;
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        memset(bm, 0, sizeof(bm));
        int ok = 0;
        for (int i = 0; i < N; i++) {
            if (half_link_tx_matrix(bm)) ok++;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (ok == 0) {
            ESP_LOGE(TAG, "epreuve : 0/%d acquittes — PERSONNE N'ECOUTE sur ch=0x%02X",
                     N, cfg.channel);
        } else {
            ESP_LOGW(TAG, "epreuve : %d/%d acquittes — LA GAUCHE ECOUTE", ok, N);
            /* LA mesure de R1. L'acquittement seul ne dit rien de la surdite :
             * l'ESB retransmet jusqu'a 15 fois, donc un paquet passe meme si la
             * gauche etait sourde au premier essai. Ce qui trahit la surdite,
             * c'est le NOMBRE DE RETRANSMISSIONS — chaque excursion PRX->PTX de
             * la gauche coute un essai perdu a la droite.
             *
             * Un ratio proche de zero veut dire que la bascule ne se voit pas ;
             * un ratio eleve mesure exactement ce que le pari coute. */
            if (rf_tx_count)
                ESP_LOGW(TAG, "R1 : %u retransmissions pour %u paquets = %u.%02u par paquet",
                         (unsigned)rf_tx_retr_sum, (unsigned)rf_tx_count,
                         (unsigned)(rf_tx_retr_sum / rf_tx_count),
                         (unsigned)((rf_tx_retr_sum * 100 / rf_tx_count) % 100));
        }
    }
    return true;
}

static bool half_link_tx_frame(const uint8_t *buf, uint8_t n);

#if CONFIG_KASE_HALF_LINK_TX
/* Prêt du bus SPI à l'écran (rf_bus.h) : le même mutex que half_link_tx_frame. */
#include "rf_bus.h"
bool rf_bus_lock(uint32_t timeout_ms)
{
    return s_tx_radio_mux && xSemaphoreTake(s_tx_radio_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void rf_bus_unlock(void) { if (s_tx_radio_mux) xSemaphoreGive(s_tx_radio_mux); }
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
#endif

bool half_link_tx_matrix(const uint8_t *bitmap)
{
    if (!s_radio.present) return false;
    uint8_t buf[16];
    uint16_t n;
#if CONFIG_KASE_DONGLE_FUSION
    /* Le FORMAT dépend de la cible courante (repli sans dongle) :
     *  - cible dongle : demi-matrice BRUTE (PKT_TYPE_MATRIX + identité de moitié).
     *    Le dongle fusionne les deux moitiés et fait tourner le moteur.
     *  - cible gauche : HEARTBEAT pré-fusion (KaSe.03), le SEUL format que
     *    kbd_relay décode côté gauche-USB. Chaque auditeur parle son protocole. */
    if (s_tx_fsm.cible == HALF_TX_TO_LEFT) {
        rf_heartbeat_t h;
        memset(&h, 0, sizeof(h));
        memcpy(h.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
        h.seq = s_seq;
        n = rf_encode_heartbeat(buf, &h);
    } else {
        rf_matrix_t m;
        m.half = RF_HALF_RIGHT;
        memcpy(m.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
        m.seq = s_seq;
        n = rf_encode_matrix(buf, &m);
    }
#else
    rf_heartbeat_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    /* seq n'est consommé qu'une fois le verrou pris : un envoi abandonné
     * (radio endormie, verrou tenu) l'incrémentait quand même, et la gauche
     * comptait chaque abandon comme une trame PERDUE — 48 % de « pertes »
     * lues au banc le 2026-09-11 pendant que la droite s'endormait, sans qu'un
     * seul paquet ait disparu en l'air. */
    h.seq = s_seq;
    /* batt_dV et link_q restent a zero : la jauge est la brick B7, et la
     * qualite de lien se calculera quand le compteur de retransmissions aura
     * un sens (il faut un recepteur en face). */
    n = rf_encode_heartbeat(buf, &h);
#endif
    return half_link_tx_frame(buf, (uint8_t)n);
}

/* Émission d'UNE trame vers la cible courante, sous le verrou radio : envoi,
 * chien de garde / bascule de cible (fusion), instrument de banc. Partagée par
 * la matrice (half_link_tx_matrix) et le STATUS lent de la jauge
 * (half_link_tx_status) — un seul chemin d'émission, donc un seul propriétaire
 * de la puce et une seule FSM. */
#if CONFIG_KASE_HALF_LINK_TX
bool half_link_tx_dongle_vu(void)
{
#if CONFIG_KASE_DONGLE_FUSION
    if (s_tx_fsm.cible != HALF_TX_TO_DONGLE) return false;   /* repli sur la gauche : pas de dongle */
#endif
    /* Collant, pas daté : une moitié est MUETTE au repos par construction (un
     * STATUS toutes les 30 s), un « vu depuis moins de 2,5 s » clignoterait à
     * chaque STATUS. L'indicateur ne tombe que si une émission n'est PAS
     * acquittée. */
    return s_sans_ack_ecran < 3;   /* tolère les ~1 % de refus ESB isolés */
}
#endif

static bool half_link_tx_frame(const uint8_t *buf, uint8_t n)
{
    if (!s_radio.present) return false;
    /* Le verrou couvre TOUTE la transaction, CSN compris. 50 ms : une émission
     * ESB au pire cas (ARC=15, ARD=500 µs) tient en ~13 ms. */
    if (s_tx_radio_mux &&
        xSemaphoreTake(s_tx_radio_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "emission abandonnee : radio occupee");
        return false;
    }
    s_seq++;                       /* la trame part : ce numéro est consommé */
    bool ack = rf_driver_send(&s_radio, buf, n);
    if (ack) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;
    /* Le verrou reste TENU jusqu'après le chien de garde : la décision de
     * bascule (s_tx_fsm) et le réarmement doivent être sérialisés avec l'envoi.
     * Cette fonction est appelée par DEUX tâches (callback de scan sur
     * changement, tâche de rafraîchissement des maintiens) — un compteur
     * read-modify-write hors verrou perdait des incréments ou basculait deux
     * fois (revue 2026-09-13 : « un seul propriétaire ne suffit pas s'il a deux
     * bouches »). */

    /* Chien de garde radio. Un nRF24 (clone) se FIGE — sous un orage de
     * retransmissions ou un glitch (constaté au banc 2026-09-13 : à l'activation
     * du lien TRRS en mode USB, la radio de la droite gelait et n'acquittait plus
     * RIEN, même une fois l'USB retiré, jusqu'au reset). Le dongle a un chien de
     * garde ; la droite n'en avait pas → mort permanente. Après N envois
     * consécutifs sans ACK (pas une simple perte ESB), on RÉARME la puce
     * (réécriture de la config PTX), sans redémarrage. */
#if CONFIG_KASE_DONGLE_FUSION
    /* En fusion, le réarmement DOUBLE comme repli : il bascule vers l'autre
     * auditeur (dongle ↔ gauche-USB directe). Décision pure et testée
     * (half_tx_target_step, test/test_half_tx_target.c). La bascule réécrit la
     * config PTX de la nouvelle cible — elle réarme donc aussi une puce figée. */
    if (half_tx_target_step(&s_tx_fsm, ack, HALF_TX_SWITCH_FAILS)) {
        const rf_radio_cfg_t *tgt = (s_tx_fsm.cible == HALF_TX_TO_LEFT)
                                        ? &s_cfg_left : &s_cfg_dongle;
        rf_driver_set_ptx(&s_radio, tgt);   /* verrou déjà tenu */
        s_tx_cfg = *tgt;                    /* la config vivante suit la cible */
        ESP_LOGW(TAG, "repli : bascule TX -> %s (rearme, %u sans ACK)",
                 s_tx_fsm.cible == HALF_TX_TO_LEFT ? "GAUCHE KaSe.03 (heartbeat)"
                                                   : "DONGLE KaSe.01 (matrix)",
                 (unsigned)HALF_TX_SWITCH_FAILS);
    }
#else
    static uint16_t s_sans_ack;
    if (ack) {
        s_sans_ack = 0;
    } else if (++s_sans_ack >= 30) {
        s_sans_ack = 0;
        rf_driver_set_ptx(&s_radio, &s_tx_cfg);   /* verrou déjà tenu */
        ESP_LOGW(TAG, "chien de garde : radio TX rearmee (30 envois sans ACK)");
    }
#endif
    if (s_tx_radio_mux) xSemaphoreGive(s_tx_radio_mux);

    /* Instrument de banc : sans lui, on ne distingue pas « les paquets partent
     * et sont acquittes » de « ils partent dans le vide ». Resume tous les dix
     * envois plutot qu'une ligne par paquet — a la frappe, une ligne par paquet
     * noierait la console et fausserait le timing. */
    static uint32_t envois, acquittes;
    envois++;
    if (ack) acquittes++;
    if ((envois % 10) == 0)
        ESP_LOGW(TAG, "TX %u envois, %u acquittes (%u%%)",
                 (unsigned)envois, (unsigned)acquittes,
                 (unsigned)(acquittes * 100 / envois));
    return ack;
}

#if CONFIG_KASE_HALF_LINK_TX
/* SEUL point de décision de l'émission — les deux chemins passent par ici.
 *
 * `change` distingue les deux appelants : le callback de scan sait qu'il y a du
 * neuf et veut partir sans attendre ; la tâche de rafraîchissement ne sait rien
 * et laisse la règle trancher. La règle elle-même est pure et testée host
 * (half_tx_doit_emettre, test/test_half_tx_cadence.c) : muet au repos,
 * rafraîchi tant qu'une touche est tenue. */
void half_link_tx_update(const uint8_t *bitmap, bool change)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t  etat[RF_HALF_BITMAP_BYTES];
    uint32_t dernier;

    /* La tâche de rafraîchissement passe bitmap = NULL : elle n'a rien de neuf
     * à annoncer, elle réaffirme ce qui est déjà là. C'est délibéré. Si elle
     * fournissait l'état qu'elle a lu au tour précédent, elle le RÉÉCRIRAIT
     * ici, et un relâchement publié entre-temps par le callback de scan serait
     * ressuscité — la touche resterait enfoncée jusqu'au prochain changement.
     * Un seul écrivain, donc : le callback. */
    /* Réparation bornée : un changement arme HALF_TX_REPEATS répétitions
     * (half_tx_doit_emettre_repare, testée). Le compteur vit sous le même
     * spinlock que l'état : callback de scan et tâche de rafraîchissement y
     * accèdent tous deux. La décision est prise DANS la section critique (pure,
     * sans blocage) pour que l'horodatage et le compteur bougent d'un bloc. */
    static half_tx_repeat_t s_rep;
    bool emettre;
    taskENTER_CRITICAL(&s_etat_mux);
    if (change && bitmap) memcpy(s_etat_local, bitmap, RF_HALF_BITMAP_BYTES);
    memcpy(etat, s_etat_local, RF_HALF_BITMAP_BYTES);
    dernier = s_dernier_tx_ms;
    bool tenu = false;
    for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++)
        if (etat[i]) { tenu = true; break; }
    emettre = half_tx_doit_emettre_repare(&s_rep, change, tenu, now, dernier, HALF_TX_REFRESH_MS);
    if (emettre) s_dernier_tx_ms = now;
    taskEXIT_CRITICAL(&s_etat_mux);

    if (change && s_refresh_task) xTaskNotifyGive(s_refresh_task);   /* réveiller la cadence rapide */
    if (!emettre) return;
    half_link_tx_matrix(etat);
}

/* Rafraîchissement des maintiens.
 *
 * Le callback du pilote keyboard_button est enregistré sur KBD_EVENT_PRESSED,
 * qui ne se déclenche QUE sur changement (cf. matrix_scan.c). Une touche tenue
 * n'y produit donc plus rien après son appui — et la gauche, qui relâche au
 * bout de HALF_LINK_TIMEOUT_MS de silence, la lâchait alors qu'elle était
 * physiquement enfoncée. Il faut un contexte périodique, et il n'y en avait
 * aucun sur cette moitié : le voici.
 *
 * Il ne réveille la radio que si quelque chose est enfoncé. Au repos la tâche
 * tourne à vide pour le prix d'un memcmp toutes les 20 ms — le lien reste
 * gratuit, ce qui est la prémisse de R1. */
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
/* Jauge : la droite est muette au repos, donc sa tension doit partir de sa
 * propre initiative — un STATUS toutes les RF_BATT_PERIOD_MS (contrat dans
 * rf_slot.h), plus un au réveil. Il porte l'identité de moitié : les deux
 * moitiés partagent le slot clavier du dongle en fusion. Il part vers la cible
 * COURANTE de la FSM ; replié sur la gauche, celle-ci l'ignore — acceptable, le
 * dongle est alors absent de toute façon. Ce n'est PAS une activité clavier :
 * il ne tamponne pas la veille, la droite s'endort comme avant. */
static uint32_t s_dernier_status_ms;   /* 0 = forcer au prochain tick (boot, réveil) */

bool half_link_tx_status(void)
{
    rf_status_t st = { .batt_dV = batt_sense_dv(), .link_q = 0, .seq = s_seq,
                       .mode_usb = false, .half = RF_HALF_RIGHT,
                       .charging = batt_sense_charging(), .config_fp = 0 };
    uint8_t buf[RF_STATUS_LEN];
    uint16_t n = rf_encode_status(buf, &st);
    return half_link_tx_frame(buf, (uint8_t)n);
}

static void half_link_batt_tick(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_dernier_status_ms == 0 || (uint32_t)(now - s_dernier_status_ms) >= RF_BATT_PERIOD_MS) {
        s_dernier_status_ms = now ? now : 1;
        half_link_tx_status();
    }
}
#endif

static void half_link_tx_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        half_link_tx_update(NULL, false);
#if CONFIG_KASE_BATT_SENSE
        half_link_batt_tick();   /* STATUS lent de la jauge ; pas une activité */
#endif
#if CONFIG_KASE_VEILLE
        /* La moitié droite n'a pas de tâche clavier : cette tâche, qui tourne
         * déjà à 20 ms, porte aussi sa veille. Elle dort indépendamment de la
         * gauche et se réveille sur SA matrice — la radio étant éteinte, aucune
         * des deux ne peut réveiller l'autre. */
        {
            uint32_t inactif = (uint32_t)(esp_timer_get_time() / 1000)
                             - get_last_activity_time_ms();
            /* tud_ready(), PAS tud_mounted() : sur l'ESP32-S3, mounted reste vrai
             * après un débranchement à chaud — aucun événement de déconnexion —
             * et la veille restait bloquée jusqu'au prochain redémarrage. ready
             * retombe dès que le bus se suspend (~3 ms après le débranchement),
             * c'est le signal qu'utilise déjà le routage USB/RF. Contrepartie
             * assumée : un hôte qui s'endort câble branché laisse aussi le
             * clavier dormir ; il se ré-énumère au réveil. Constaté au banc le
             * 2026-09-11 : sept minutes sur batterie sans jamais dormir. */
            /* PAS de verrou USB sur la moitié droite, contrairement à la gauche.
             *
             * Son USB n'est qu'un port CDC que personne n'ouvre : Linux le met
             * en autosuspend après deux secondes, et tud_ready() retombe à
             * faux. La droite se croyait alors sur batterie, s'endormait à 60 s
             * d'inactivité, coupait sa PHY USB — l'hôte voyait un débranchement
             * — et sa radio avec : « je perds le clavier droit si je le branche
             * en USB ». Constaté au banc le 2026-09-11, journal noyau à l'appui
             * (énumération, puis disconnect 66 s plus tard).
             *
             * tud_mounted() ne vaut pas mieux : il reste vrai après un vrai
             * débranchement. Il n'y a pas de bon signal sans pont VBUS. Mais
             * ici il n'en faut pas : rien sur ce port ne justifie de rester
             * éveillé, dormir ne coûte qu'un port CDC dont personne ne se sert,
             * et le réveil sur la matrice est le même que sur batterie.
             *
             * La gauche garde son verrou : elle EST le clavier HID, et un HID
             * n'est pas autosuspendu. */
            bool lien = false;
#if CONFIG_KASE_LINK_WIRE
            /* Une moitié en charge par le TRRS reste éveillée : endormie, elle
             * cesserait de répondre aux sondes et le pair rouvrirait son 5 V. */
            lien = link_uart_active();
#endif
            veille_diag(inactif, false, lien);
            /* Battement de coeur de la DROITE (elle n'a pas celui de main.c) :
             * toutes les 10 s, inactivité et bilan de sommeil. Une nuit à 0,2 V
             * perdus (2026-09-15, droite, 4,02 V au voltmètre) n'a laissé
             * aucune trace faute de ce chiffre. */
            {
                static uint32_t dernier_hb_ms;
                uint32_t now_hb = (uint32_t)(esp_timer_get_time() / 1000);
                if ((uint32_t)(now_hb - dernier_hb_ms) >= 10000u) {
                    dernier_hb_ms = now_hb;
                    uint32_t dodo_n = 0, dodo_ms = 0;
                    veille_bilan(&dodo_n, &dodo_ms);
#if CONFIG_PM_PROFILING
                    esp_pm_dump_locks(stdout);   /* banc : sommeils automatiques, temps par mode */
                    esp_timer_dump(stdout);      /* banc : qui arme des alarmes trop rapprochées */
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
                    { static char stats[1024]; vTaskGetRunTimeStats(stats); printf("Run time stats:\n%s", stats); }
#endif
#endif
                    ESP_LOGW(TAG, "HB up=%lus inactif=%lus dormi=%lus/%lu lien=%d batt=%u dV",
                             (unsigned long)(now_hb / 1000), (unsigned long)(inactif / 1000),
                             (unsigned long)(dodo_ms / 1000), (unsigned long)dodo_n, (int)lien,
#if CONFIG_KASE_BATT_SENSE
                             (unsigned)batt_sense_dv());
#else
                             0u);
#endif
                }
            }
            veille_pas(inactif, lien);
        }
#endif
        /* 20 ms tant qu'une touche est tenue (réaffirmation à 100 ms, réparation
         * bornée), 100 ms au repos : à 20 ms permanents cette tâche sortait le
         * processeur d'oisiveté 50 fois par seconde pour un memcmp — et avec le
         * DFS, chaque sortie rallume la PLL. La veille (seuil 15 s), la jauge
         * (30 s) et le HB (10 s) s'en accommodent. */
        bool tenu = false;
        taskENTER_CRITICAL(&s_etat_mux);
        for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++) if (s_etat_local[i]) { tenu = true; break; }
        taskEXIT_CRITICAL(&s_etat_mux);
        /* Un changement (callback de scan) notifie la tâche : la réparation
         * bornée part dans la foulée, pas au prochain tick de 100 ms. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tenu ? 20 : 100));
    }
}

bool half_link_tx_refresh_start(void)
{
    if (!s_radio.present) return false;
    BaseType_t r = xTaskCreate(half_link_tx_refresh_task, "half_tx_rfr",
                               3072, NULL, 4, &s_refresh_task);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "tache de rafraichissement non creee — les maintiens de "
                      "plus de %u ms seront relaches a tort", HALF_LINK_TIMEOUT_MS);
        return false;
    }
    ESP_LOGI(TAG, "rafraichissement des maintiens : %u ms (relache a %u ms)",
             HALF_TX_REFRESH_MS, HALF_LINK_TIMEOUT_MS);
    return true;
}
#endif /* CONFIG_KASE_HALF_LINK_TX */

#endif /* CONFIG_KASE_HAS_RF_TX */


/* ── Veille : éteindre et rallumer la radio (brick B7) ──────────────────────
 *
 * Écouter coûte 13,1 mA (nRF24L01+ PS v1.0, table 4, p. 14) contre 900 nA en
 * power-down : la radio doit être coupée dès l'étage léger, sans quoi le budget
 * de veille n'a aucun sens. Les verrous sont PRIS et GARDÉS pendant tout le
 * sommeil — rien ne doit tenter d'émettre sur une puce éteinte. */
void half_link_radio_sleep(void)
{
    if (!s_radio.present) return;
    if (s_tx_radio_mux) xSemaphoreTake(s_tx_radio_mux, pdMS_TO_TICKS(50));
    rf_driver_power_down(&s_radio);
}

void half_link_radio_wake(void)
{
    if (!s_radio.present) return;
    rf_driver_power_up(&s_radio);
#if CONFIG_KASE_BATT_SENSE
    s_dernier_status_ms = 0;   /* un STATUS au réveil : la tension a pu bouger */
#endif
    if (s_tx_radio_mux) xSemaphoreGive(s_tx_radio_mux);
}
