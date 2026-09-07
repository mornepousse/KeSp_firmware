#include "half_link.h"
#include "board.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "rf_slot.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "half_link";

static rf_radio_t s_radio;
static half_state_t s_distant;   /* etat de la moitie d en face */
static volatile bool s_distant_change;   /* consomme par half_link_remote_changed */
static uint8_t    s_seq;

#if CONFIG_KASE_HALF_LINK_RX
/* Verrou de la PUCE, pas de l'état applicatif.
 *
 * Deux tâches parlent au même nRF24 par le même bus SPI : la tâche d'écoute
 * ci-dessous, qui interroge le FIFO toutes les 2 ms, et le relais HID, qui
 * demande une excursion PRX→PTX→PRX depuis un tout autre contexte (le moteur
 * clavier ou le timer de réémission). Sans exclusion, une excursion peut
 * reconfigurer le circuit en pleine lecture de trame — et le mutex de
 * kbd_relay_tx.c ne sert à rien ici, il ne connaît que ses propres appelants.
 *
 * C'est la même faute que les deux `static rf_radio_t` concurrents, d'un cran
 * plus fin : un seul propriétaire ne suffit pas s'il a deux bouches. */
static SemaphoreHandle_t s_radio_mux;

/* Réveil de la tâche d'écoute par la broche IRQ du nRF24.
 *
 * La tâche sondait le FIFO en boucle avec vTaskDelay(pdMS_TO_TICKS(2)). Or
 * CONFIG_FREERTOS_HZ vaut 100 : le tick fait 10 ms, donc 2 ms ARRONDIT À ZÉRO
 * et vTaskDelay(0) ne bloque pas — il cède la main aux tâches de priorité au
 * moins égale, jamais à l'IDLE. La tâche (priorité 4, épinglée au cœur 1)
 * affamait donc IDLE1, et le watchdog tombait toutes les 5 s. Constaté au banc
 * le 2026-09-07, backtrace dans rf_driver_rx_available.
 *
 * Plutôt que de ralentir le sondage — ce qui aurait ajouté 10 ms à chaque
 * frappe de la moitié droite — on ne sonde plus : le nRF24 a une broche IRQ,
 * active à l'état bas, et rf_driver laisse MASK_RX_DR à 0 (CONFIG = 0x3F). Elle
 * dit exactement ce qu'on passait notre temps à demander.
 *
 * Le repli à 10 ms reste : il fait tourner le contrôle de silence, et si l'IRQ
 * ne venait pas, le lien fonctionnerait encore — au rythme qu'aurait eu le
 * sondage ralenti, jamais pire. */
static TaskHandle_t s_rx_task;

static void IRAM_ATTR half_link_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t reveil = pdFALSE;
    vTaskNotifyGiveFromISR(s_rx_task, &reveil);
    portYIELD_FROM_ISR(reveil);
}
#endif

#if CONFIG_KASE_HALF_LINK_TX
/* Dernier état émis, et quand. Partagé entre DEUX contextes de tâche : le
 * callback du pilote keyboard_button (émission immédiate sur changement) et la
 * tâche de rafraîchissement ci-dessous. Quatre octets, mais une copie déchirée
 * enverrait une matrice qui n'a jamais existé — d'où le verrou, très court. */
static uint8_t  s_etat_local[RF_HALF_BITMAP_BYTES];
static uint32_t s_dernier_tx_ms;
static portMUX_TYPE s_etat_mux = portMUX_INITIALIZER_UNLOCKED;

/* Verrou de la PUCE côté émetteur. Jusqu'au 2026-09-07 un seul appelant
 * existait — le callback du pilote keyboard_button — et rf_driver_send pouvait
 * s'en passer. La tâche de rafraîchissement en ajoute un second.
 *
 * rf_driver_send pilote CSN à la main (spics_io_num = -1) : deux tâches qui
 * s'entrelacent entre csn_low() et csn_high() envoient une transaction avec CSN
 * dans le mauvais état — écriture de registre perdue en silence, ou impulsion CE
 * tronquée. La moitié gauche a reçu son verrou, la droite avait été oubliee. */
static SemaphoreHandle_t s_tx_radio_mux;
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
bool half_link_tx_init(void)
{
    s_tx_radio_mux = xSemaphoreCreateMutex();
    if (!s_tx_radio_mux) {
        ESP_LOGE(TAG, "mutex radio TX non cree — emission abandonnee");
        return false;
    }
    rf_radio_cfg_t cfg = half_link_cfg();
    esp_err_t e = rf_driver_init_tx(&s_radio, &cfg);
    if (e != ESP_OK || !s_radio.present) {
        ESP_LOGE(TAG, "TX init echouee (%d) — la moitie droite restera muette", (int)e);
        return false;
    }
    ESP_LOGI(TAG, "TX pret : ch=0x%02X addr=KaSe.%02X", cfg.channel, cfg.addr_suffix);

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

bool half_link_tx_matrix(const uint8_t *bitmap)
{
    if (!s_radio.present) return false;
    rf_heartbeat_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    h.seq = s_seq++;
    /* batt_dV et link_q restent a zero : la jauge est la brick B7, et la
     * qualite de lien se calculera quand le compteur de retransmissions aura
     * un sens (il faut un recepteur en face). */
    uint8_t buf[16];
    uint16_t n = rf_encode_heartbeat(buf, &h);
    /* Le verrou couvre TOUTE la transaction, CSN compris. 50 ms : une émission
     * ESB au pire cas (ARC=15, ARD=500 µs) tient en ~13 ms. */
    if (s_tx_radio_mux &&
        xSemaphoreTake(s_tx_radio_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "emission abandonnee : radio occupee");
        return false;
    }
    bool ack = rf_driver_send(&s_radio, buf, (uint8_t)n);
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
    taskENTER_CRITICAL(&s_etat_mux);
    if (change && bitmap) memcpy(s_etat_local, bitmap, RF_HALF_BITMAP_BYTES);
    memcpy(etat, s_etat_local, RF_HALF_BITMAP_BYTES);
    dernier = s_dernier_tx_ms;
    taskEXIT_CRITICAL(&s_etat_mux);

    bool tenu = false;
    for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++)
        if (etat[i]) { tenu = true; break; }

    if (!half_tx_doit_emettre(change, tenu, now, dernier, HALF_TX_REFRESH_MS))
        return;

    taskENTER_CRITICAL(&s_etat_mux);
    s_dernier_tx_ms = now;
    taskEXIT_CRITICAL(&s_etat_mux);

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
static void half_link_tx_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        half_link_tx_update(NULL, false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool half_link_tx_refresh_start(void)
{
    if (!s_radio.present) return false;
    BaseType_t r = xTaskCreate(half_link_tx_refresh_task, "half_tx_rfr",
                               3072, NULL, 4, NULL);
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

#if CONFIG_KASE_HALF_LINK_RX
static void half_link_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[32];
    uint32_t recus = 0, rejetes = 0, perdus = 0, excursions = 0;
    bool seq_amorce = false;
    uint8_t seq_attendu = 0;
#if CONFIG_KASE_HALF_LINK_R1
    /* Adresse du dongle, slot clavier — cible des excursions. */
    const uint8_t addr_dongle[5] = { 'K', 'a', 'S', 'e', 0x01 };
    const uint8_t addr_lien[5]   = { 'K', 'a', 'S', 'e', RF_ADDR_HALF_LINK };
    uint32_t derniere_excursion = 0;
#endif
    for (;;) {
#if CONFIG_KASE_HALF_LINK_R1
        /* ÉPREUVE R1. La gauche est sourde pendant qu'elle émet : on provoque
         * l'excursion à cadence fixe et on mesure ce qu'elle coûte en trous de
         * séquence. rf_driver_oob_tx fait le PRX→PTX→PRX complet, y compris la
         * restauration du canal d'écoute et le CE haut.
         *
         * 20 ms, soit 50 excursions/s : au-delà de ce qu'un clavier produit en
         * frappe rapide, donc un majorant honnête du coût. */
        uint32_t maintenant = (uint32_t)(esp_timer_get_time() / 1000);
        if (maintenant - derniere_excursion >= 20) {
            derniere_excursion = maintenant;
            uint8_t bidon[9] = { 0x50, 0, 0, 0, 0, 0, 0, 0, 0 };
            /* Par half_link_excursion_tx, PAS par rf_driver_oob_tx en direct :
             * l'appel direct contournait s_radio_mux, et rien n'interdit
             * d'activer R1 en même temps que le relais HID — les deux
             * reconfigureraient alors la puce sans exclusion. */
            (void)addr_lien;
            half_link_excursion_tx(RF_CH_KBD_DONGLE, addr_dongle,
                                   bidon, sizeof(bidon));
            excursions++;
        }
#endif
        /* Sondage ET lecture sous le même verrou : entre les deux, une
         * excursion basculerait le circuit en PTX et la lecture ne rendrait
         * plus rien de sensé. */
        uint16_t n = 0;
        if (xSemaphoreTake(s_radio_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (rf_driver_rx_available(&s_radio))
                n = rf_driver_read_rx(&s_radio, buf, sizeof(buf));
            xSemaphoreGive(s_radio_mux);
        }
        if (n) {
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                recus++;
                /* FUSION : l'etat recu devient celui de la moitie distante. Le
                 * moteur le lira via half_link_remote_pressed(). */
                {
                    uint8_t avant[RF_HALF_BITMAP_BYTES];
                    memcpy(avant, s_distant.bitmap, sizeof(avant));
                    half_state_recu(&s_distant, h.bitmap,
                                    (uint32_t)(esp_timer_get_time() / 1000));
                    /* Ne lever le drapeau que si l'ETAT a change : la droite
                     * emet sur changement, mais une retransmission ESB peut
                     * livrer deux fois la meme trame. */
                    if (memcmp(avant, s_distant.bitmap, sizeof(avant)))
                        s_distant_change = true;
                }
                /* Trous de séquence : le seul témoin de ce que l'excursion
                 * coûte. seq est un octet, l'écart se calcule donc modulo 256. */
                if (seq_amorce) {
                    uint8_t ecart = (uint8_t)(h.seq - seq_attendu);
                    if (ecart) perdus += ecart;
                }
                seq_amorce = true;
                seq_attendu = (uint8_t)(h.seq + 1);
                if ((recus % 20) == 0)
                    ESP_LOGW(TAG, "R1 : %u recus, %u perdus (%u%%), %u excursions",
                             (unsigned)recus, (unsigned)perdus,
                             (unsigned)(perdus * 100 / (recus + perdus)),
                             (unsigned)excursions);
                /* Journal de banc : on affiche les coordonnees pressees plutot
                 * que le bitmap brut, pour pouvoir comparer a ce qu'on presse
                 * physiquement sur la droite. */
                char pos[64]; int off = 0;
                for (int r = 0; r < RF_HALF_ROWS && off < (int)sizeof(pos) - 8; r++)
                    for (int c = 0; c < RF_HALF_COLS && off < (int)sizeof(pos) - 8; c++)
                        if (rf_bitmap_get(h.bitmap, (uint8_t)r, (uint8_t)c))
                            off += snprintf(pos + off, sizeof(pos) - off, "(%d,%d)", r, c);
                ESP_LOGW(TAG, "RX #%u seq=%u : %s", (unsigned)recus, h.seq,
                         off ? pos : "(rien enfonce)");
            } else {
                rejetes++;
                ESP_LOGW(TAG, "RX trame rejetee (len=%u, total %u)", n, (unsigned)rejetes);
            }
        }
        /* Repli sur silence (HALF_LINK_TIMEOUT_MS) : au-dela, une moitie qui
         * s est tue laisse
         * l hote sur son dernier etat — et si c etait « Maj enfoncee », il le
         * reste. On ne relache QUE ce que cette moitie tenait. */
        if (half_state_timeout(&s_distant,
                               (uint32_t)(esp_timer_get_time() / 1000),
                               HALF_LINK_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "lien silencieux > %u ms — touches de la droite relachees",
                     HALF_LINK_TIMEOUT_MS);
            s_distant_change = true;
        }

        /* Blocage réel : c'est ce qui rend la main à l'IDLE. Réveil immédiat
         * sur IRQ, ou au bout de 10 ms pour le contrôle de silence. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    }
}

/* Excursion PRX→PTX→PRX sur LA radio du lien.
 *
 * Le Niphargus n'a qu'une puce par moitié, et la gauche en a deux usages
 * concurrents : écouter la droite en PRX sur RF_CH_HALF_LINK, et parler au
 * dongle en PTX sur son canal. Chacun des deux modules avait jusqu'ici son
 * propre `static rf_radio_t s_radio` — deux propriétaires pour une seule puce,
 * dont le second écrasait silencieusement la configuration du premier. C'est
 * l'incident qui a fait écouter la gauche sur le canal du dongle, et il s'est
 * produit trois fois.
 *
 * La radio appartient désormais à ce module, seul à l'initialiser. Le relais
 * HID passe par ici : on sort vers le dongle, on émet, on rentre sur le canal
 * du lien. C'est exactement l'excursion que l'épreuve R1 a mesurée le
 * 2026-09-05 (0 perte, 0,4 retransmission/paquet) — à cadence bien plus élevée
 * que ce qu'une frappe produit. */
bool half_link_excursion_tx(uint8_t canal, const uint8_t addr[5],
                            const uint8_t *payload, uint8_t len)
{
    if (!s_radio.present || !s_radio_mux) return false;
    const uint8_t addr_lien[5] = { 'K', 'a', 'S', 'e', RF_ADDR_HALF_LINK };
    /* 20 ms : la tâche d'écoute ne garde le verrou que le temps d'une lecture
     * de FIFO. Au-delà, mieux vaut perdre ce rapport HID que bloquer le moteur
     * clavier — le relais réémet de lui-même (kbd_refresh_arm). */
    if (xSemaphoreTake(s_radio_mux, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "excursion abandonnee : radio occupee");
        return false;
    }
    bool ok = rf_driver_oob_tx(&s_radio, canal, addr, payload, len,
                               RF_CH_HALF_LINK, addr_lien);
    xSemaphoreGive(s_radio_mux);

    /* Bilan périodique. « La liaison n'est pas très bonne » ne se corrige pas
     * sans savoir LAQUELLE des trois issues domine : acquitté, MAX_RT (le
     * dongle n'entend pas), ou scrutin expiré (il ne répond pas à temps). */
    static uint32_t n;
    if ((++n % 50) == 0)
        ESP_LOGW(TAG, "excursions : %u ok, %u MAX_RT, %u expirees",
                 (unsigned)rf_oob_ok, (unsigned)rf_oob_maxrt,
                 (unsigned)rf_oob_timeout);
    return ok;
}

bool half_link_remote_pressed(uint8_t row, uint8_t col)
{
    return half_state_pressed(&s_distant, row, col);
}

bool half_link_remote_changed(void)
{
    if (!s_distant_change) return false;
    s_distant_change = false;
    return true;
}

bool half_link_rx_start(void)
{
    s_radio_mux = xSemaphoreCreateMutex();
    if (!s_radio_mux) {
        ESP_LOGE(TAG, "mutex radio non cree — lien abandonne");
        return false;
    }
    rf_radio_cfg_t cfg = half_link_cfg();
    esp_err_t e = rf_driver_init(&s_radio, &cfg);
    if (e != ESP_OK || !s_radio.present) {
        ESP_LOGE(TAG, "RX init echouee (%d) — la gauche n'entendra pas la droite", (int)e);
        return false;
    }
    ESP_LOGI(TAG, "RX a l'ecoute : ch=0x%02X addr=KaSe.%02X", cfg.channel, cfg.addr_suffix);
    xTaskCreatePinnedToCore(half_link_rx_task, "half_rx", 4096, NULL, 4,
                            &s_rx_task, 1);

    /* IRQ du nRF24 : active à l'état bas, donc front descendant. Le service
     * d'ISR peut déjà avoir été installé par un autre pilote (matrice, écran) —
     * ESP_ERR_INVALID_STATE veut dire « déjà là » et n'est pas une faute. */
    {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << BOARD_NRF_IRQ),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        /* Drapeau 0, pas ESP_INTR_FLAG_IRAM : le service est PARTAGÉ avec les
         * autres pilotes, et l'exiger en IRAM imposerait la contrainte à leurs
         * gestionnaires, pas seulement au nôtre. */
        esp_err_t e_isr = gpio_install_isr_service(0);
        if (e_isr != ESP_OK && e_isr != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "service ISR indisponible (%d) — repli sur le sondage "
                          "a 10 ms", (int)e_isr);
        if (gpio_isr_handler_add(BOARD_NRF_IRQ, half_link_irq_isr, NULL) == ESP_OK)
            ESP_LOGI(TAG, "ecoute reveillee par IRQ (GPIO%d)", BOARD_NRF_IRQ);
        else
            ESP_LOGW(TAG, "IRQ non armee — repli sur le sondage a 10 ms");
    }
    return true;
}
#endif /* CONFIG_KASE_HALF_LINK_RX */
