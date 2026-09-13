/*
 * Tâche RF du dongle : elle possède les deux radios NRF24 et rien d'autre.
 *
 * Ce ne sont plus deux moitiés d'un même clavier. Le slot 1 porte le clavier —
 * la moitié maître du Niphargus, qui fait tourner son moteur keymap chez elle et
 * n'envoie ici que du HID déjà fini — et le slot 2 porte la souris Conchodytes.
 * Les deux appareils sont indépendants ; ce que rf_slot.h rend impossible à
 * oublier, c'est que la perte de l'un ne doit rien relâcher de l'autre.
 *
 * Il n'y a donc plus ni matrice, ni réconciliation de bitmap, ni cycle moteur
 * dans ce fichier : le dongle relaie et supervise.
 */

/*
 * rf_signal_q255() — derive a 0..255 link-quality value for one slot.
 *
 * 255 = best, 0 = link down / timed out.
 * Pure function: no globals, no I/O. Host-testable (outside TEST_HOST guard).
 * Place before the #ifndef TEST_HOST block so it compiles in both contexts.
 */
#include "rf_rx_task.h"
#include <stdint.h>
#include <stdbool.h>

uint8_t rf_signal_q255(bool link_up, uint32_t hb_age_ms, uint8_t link_q)
{
    /* Link is down if rf_rx_task flagged it, OR if the heartbeat age exceeds 3×
     * the nominal heartbeat interval (500 ms → 1500 ms). 3× = two missed beats. */
    if (!link_up || hb_age_ms >= 1500u) return 0;

    /* Age factor: 255 when fresh, linear down to 0 at the 1500 ms timeout. */
    uint32_t age_factor = 255u * (1500u - hb_age_ms) / 1500u;

    /* Retry factor: link_q is a retransmit PERCENTAGE (0..100) from OBSERVE_TX
     * ARC_CNT (Σ retries × 100 / (tx_count × 3)). 255 at 0 % (pristine), linear
     * down to 0 at 100 % (every packet maxing all 3 ARC retries). */
    uint8_t  lq = (link_q > 100u) ? 100u : link_q;
    uint32_t retry_factor = 255u * (100u - lq) / 100u;

    /* Both dimensions must be good — take the worse of the two. */
    return (uint8_t)((age_factor < retry_factor) ? age_factor : retry_factor);
}

#ifndef TEST_HOST

#include "rf_driver.h"
#include "rf_packet.h"
#include "rf_slot.h"
#include "dongle_engine.h"   /* fusion : moteur keymap embarqué (gardé en interne) */
#include "board_rf.h"
#include "rf_pairing.h"   /* rf_pairing_load_set_id_dongle, rf_apply_set_id */
#if CONFIG_KASE_NRF_LINE_TEST
#include "rf_line_test.h"
#endif
#include "keyboard_config.h"
#include "hid_transport.h"
#include "nvs.h"            /* nvs_open, nvs_get_blob, nvs_close */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"      /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "rf_rx";

/* Radio 1 → slot clavier, radio 2 → slot souris. Le nom dit le rôle ; la
 * configuration matérielle (broches, adresse, canal) reste nommée par la radio
 * physique dans board_rf.h, parce que c'est ce qui est sérigraphié sur la carte. */
static rf_radio_t s_kbd, s_mouse;

/* Présence de chaque slot : date du dernier paquet reçu, quel qu'il soit —
 * battement, trame d'état ou rapport HID. C'est aussi ce que lit le chien de
 * garde radio : se fier à l'âge du battement réarmerait une radio parfaitement
 * saine dès que la moitié cesse d'en émettre (cf. §7 bis du design du dongle). */
static rf_slot_link_t s_link[RF_SLOT_COUNT];

/* Current per-radio config (set_id-derived) — kept so the radio watchdog can
 * re-arm a wedged radio with the live address/channel. Updated at init and on
 * every pairing hot-switch. */
static rf_radio_cfg_t s_kbd_cfg, s_mouse_cfg;
static SemaphoreHandle_t s_evt_sem;

/* Dernier link_q annoncé par chaque slot (battement ou trame d'état).
 * Lu par rf_rx_get_status() → CDC RF_STATUS. */
static uint8_t s_link_q[RF_SLOT_COUNT];

/* ── Pairing window state (driven inside rf_rx_task) ── */
static volatile bool s_pairing_mode = false;
static uint32_t s_pair_deadline_ms = 0;
static uint8_t  s_pair_paired_count = 0;
/* Les clés NVS d'appairage gardent leurs noms d'origine : les renommer
 * désapparierait le matériel déjà appairé pour un gain purement cosmétique.
 * `left` y désigne le slot 0x01 (clavier), `right` le slot 0x02 (souris). */
static uint8_t  s_pair_mac_left[6]  = {0};
static uint8_t  s_pair_mac_right[6] = {0};
#define RF_PAIR_WINDOW_MS 120000   /* 2 min — relaxed envelope for the manual BOOT-hold dance */

/* ── IRQ ISR (shared sem; task polls both radios) ── */
static void IRAM_ATTR nrf_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_evt_sem, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

/* Le dongle ne fait plus tourner de moteur : il reçoit du HID déjà fini et le
 * repousse à l'hôte. rebuild_press_arrays(), les callbacks de réconciliation et
 * run_engine_cycle() ont été retirés avec la matrice — voir
 * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md
 *
 * Ce qui reste à surveiller : le silence d'un slot. Sans matrice à relâcher, le
 * repli se ramène à un rapport vide — sinon le dernier rapport reçu resterait
 * appliqué sur l'hôte indéfiniment.
 *
 * Et il porte sur ce slot-là uniquement : une souris qui sort de portée ne doit
 * pas effacer la frappe en cours. C'est rf_slot_link_check() qui décide. */
static void apply_safe_action(rf_safe_action_t action)
{
    static const uint8_t none[6] = {0};
    if (action == RF_SAFE_RELEASE_KEYS)         hid_send_keyboard(0, none);
    else if (action == RF_SAFE_RELEASE_BUTTONS) hid_send_mouse(0, 0, 0, 0);
}

/* Le cache batterie n'avait plus personne pour l'alimenter depuis que la
 * réconciliation des heartbeats a été retirée : la commande CDC BATTERY
 * répondait « inconnu » en permanence. La trame d'état le remplit à nouveau.
 * Elle ne porte que la tension — l'état de charge et la charge en cours restent
 * inconnus plutôt que devinés, parce que quatre octets étaient une contrainte
 * de conception et non un oubli. */
extern void dongle_cache_set_battery(uint8_t slot, uint8_t batt_dV,
                                     uint8_t soc_pct, uint8_t charging);

static void cache_battery(uint8_t slot, uint8_t batt_dV)
{
    dongle_cache_set_battery(slot, batt_dV, 0xFF, 0xFF);
}

/* ── Vider les paquets en attente sur une radio ── */
static void drain_radio(rf_radio_t *radio, uint8_t slot)
{
    uint8_t buf[32];
    while (rf_driver_rx_available(radio)) {
        uint16_t n = rf_driver_read_rx(radio, buf, sizeof(buf));
        if (n == 0) break;
        /* Tout paquet vaut preuve de vie, pas seulement les battements. */
        rf_slot_link_rx(&s_link[slot], (uint32_t)(esp_timer_get_time() / 1000));
        uint8_t type = rf_packet_type(buf, n);
        /* PKT_TYPE_KEY (matrice brute) et PKT_TYPE_TRACKPAD (gestuelle brute) ne
         * sont plus traités : plus personne ne les émet depuis le retrait des
         * anciennes moitiés, et le Niphargus envoie du HID déjà fini. Le dongle
         * ne décode plus aucune matrice — c'est ce qui rend impossible
         * l'existence de deux moteurs keymap dans le système. */
        if (type == PKT_TYPE_STATUS) {
            /* Trame de repos : batterie et qualité du lien, aucun état de touche. */
            rf_status_t st;
            if (rf_decode_status(buf, n, &st)) {
                s_link_q[slot] = st.link_q;
                cache_battery(slot, st.batt_dV);
            }
        } else if (type == PKT_TYPE_HEARTBEAT) {
            /* Ancien format, conservé le temps que le Niphargus le remplace :
             * son bitmap n'est plus lu, il n'y a plus de matrice ici. */
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                s_link_q[slot] = h.link_q;
                cache_battery(slot, h.batt_dV);
            }
        } else if (type == PKT_TYPE_HIDREPORT) {
            /* Le clavier a déjà fait tourner son moteur : on pousse tel quel.
             * En mode fusion le CLAVIER n'émet plus de HID fini (il envoie sa
             * matrice brute, voir PKT_TYPE_MATRIX) — mais la SOURIS Conchodytes,
             * elle, continue d'émettre du HID fini sur son slot. Ce chemin reste
             * donc nécessaire dans les deux modes. */
            uint8_t sub, mod, kb[6], btn; int8_t x, y, w;
            if (rf_decode_hidreport(buf, n, &sub, &mod, kb, &btn, &x, &y, &w)) {
                if (sub == RF_HID_SUB_KBD)        hid_send_keyboard(mod, kb);
                else if (sub == RF_HID_SUB_MOUSE) hid_send_mouse(btn, x, y, w);
            }
        }
#if CONFIG_KASE_DONGLE_FUSION
        else if (type == PKT_TYPE_MATRIX) {
            /* Fusion : demi-matrice brute d'une moitié. On la remet au moteur
             * embarqué, qui fusionne les deux moitiés et sort le HID. */
            rf_matrix_t m;
            if (rf_decode_matrix(buf, n, &m))
                dongle_engine_on_matrix(&m);
        }
#endif
    }
}

bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out)
{
    if (!s_kbd.present) return false;   /* la radio 1 porte le rendez-vous d'appairage */

    if (reset) {
        rf_pairing_reset_dongle();
    }
    rf_pairing_load_peers_dongle(s_pair_mac_left, s_pair_mac_right, &s_pair_paired_count);

    /* Switch radio L to the pairing rendezvous PRX. */
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    rf_driver_set_channel(&s_kbd, RF_PAIR_CHANNEL);
    rf_driver_set_rx_address(&s_kbd, pair_addr);

    s_pair_deadline_ms = (uint32_t)(esp_timer_get_time() / 1000) + RF_PAIR_WINDOW_MS;
    s_pairing_mode = true;

    if (set_id_out)       *set_id_out = rf_compute_set_id();
    if (paired_count_out) *paired_count_out = s_pair_paired_count;
    ESP_LOGI(TAG, "pairing window open (reset=%u, paired_count=%u)", reset, s_pair_paired_count);
    return true;
}

/* Reprogram both radios to the derived per-set address+channel (or factory if
 * paired_count==0). Hot-switch — no reboot (USB stays up). */
static void rf_rx_apply_paired_config(void)
{
    uint16_t set_id = (s_pair_paired_count > 0) ? rf_compute_set_id() : 0;

    rf_radio_cfg_t kcfg = board_rf_radio1_cfg();
    rf_radio_cfg_t mcfg = board_rf_radio2_cfg();
    rf_apply_set_id(&kcfg, set_id, 0x01);
    rf_apply_set_id(&mcfg, set_id, 0x02);
    s_kbd_cfg = kcfg; s_mouse_cfg = mcfg;   /* keep live config for the radio watchdog */

    uint8_t kaddr[5] = { kcfg.rx_addr[0], kcfg.rx_addr[1], kcfg.rx_addr[2],
                         kcfg.rx_addr[3], kcfg.addr_suffix };
    uint8_t maddr[5] = { mcfg.rx_addr[0], mcfg.rx_addr[1], mcfg.rx_addr[2],
                         mcfg.rx_addr[3], mcfg.addr_suffix };
    rf_driver_set_channel(&s_kbd,  kcfg.channel);
    rf_driver_set_rx_address(&s_kbd, kaddr);
    if (s_mouse.present) {
        rf_driver_set_channel(&s_mouse,  mcfg.channel);
        rf_driver_set_rx_address(&s_mouse, maddr);
    }
    ESP_LOGI(TAG, "hot-switch: set_id=0x%04X clavier ch=%u souris ch=%u",
             set_id, kcfg.channel, mcfg.channel);
}

/* Process the pairing rendezvous on radio L. Returns true while still pairing. */
static bool rf_rx_pairing_service(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Window timeout or both halves paired → close + hot-switch. */
    if (now >= s_pair_deadline_ms || s_pair_paired_count >= 2) {
        rf_rx_apply_paired_config();
        s_pairing_mode = false;
        ESP_LOGI(TAG, "pairing window closed (paired_count=%u)", s_pair_paired_count);
        return false;
    }

    /* Drain any PKT_PAIR_REQ on radio 1. */
    uint8_t buf[32];
    while (rf_driver_rx_available(&s_kbd)) {
        uint16_t n = rf_driver_read_rx(&s_kbd, buf, sizeof(buf));
        if (n == 0) break;
        uint8_t mac[6];
        uint8_t declared_slot = 0;
        if (!rf_decode_pair_req(buf, n, mac, &declared_slot)) continue;

        uint8_t slot = 0;
        bool is_dup = rf_pairing_match_slot(mac, s_pair_mac_left, s_pair_mac_right, &slot);
        if (!is_dup) {
            /* Le périphérique déclare son propre slot (identité de carte) →
             * l'ordre d'appairage n'a plus d'importance. slot=0 → repli positionnel. */
            if (!rf_pairing_resolve_slot(declared_slot, s_pair_paired_count, &slot)) continue; /* full */
        }

        /* Persist (new pairings only bump count).
         *
         * ⚠ NE PAS ACQUITTER UN APPAIRAGE QU'ON N'A PAS SU ENREGISTRER.
         *
         * Le retour de rf_pairing_save_peer_dongle() était ignoré ici. Constaté
         * au banc le 2026-08-26 avec la souris Conchodytes : la NVS du dongle
         * refusait ses écritures, l'ACK partait quand même, et le périphérique
         * repartait convaincu d'être appairé — set_id et slot enregistrés de son
         * côté — pendant que le dongle n'en gardait aucune trace et continuait
         * d'écouter le rendez-vous. Les deux émettaient sur des adresses
         * différentes, zéro paquet passait, et RIEN ne le signalait.
         *
         * Un échec bruyant vaut mieux qu'un appairage fantôme : sans ACK, le
         * périphérique réessaie puis abandonne en le disant. */
        if (!is_dup) {
            uint8_t new_count = s_pair_paired_count + 1;
            esp_err_t err = rf_pairing_save_peer_dongle(slot, mac, new_count);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "appairage NON enregistre (slot=0x%02X) : %s "
                              "— pas d'ACK, la NVS du dongle est en echec",
                         slot, esp_err_to_name(err));
                continue;   /* pas d'ACK : voir le commentaire ci-dessus */
            }
            if (slot == 0x01) memcpy(s_pair_mac_left,  mac, 6);
            else              memcpy(s_pair_mac_right, mac, 6);
            s_pair_paired_count = new_count;
        }

        /* Send PKT_PAIR_ACK out-of-band on radio L, then restore PAIR PRX. */
        uint8_t dmac[6];
        esp_read_mac(dmac, ESP_MAC_WIFI_STA);
        rf_pair_ack_t ack = { .set_id = rf_compute_set_id(), .slot = slot };
        memcpy(ack.dongle_wifi_mac, dmac, 6);
        uint8_t ackbuf[10];
        rf_encode_pair_ack(ackbuf, &ack);

        static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
        rf_driver_oob_tx(&s_kbd, RF_PAIR_CHANNEL, pair_addr, ackbuf, 10,
                         RF_PAIR_CHANNEL, pair_addr);   /* restore to PAIR PRX */
        ESP_LOGI(TAG, "ACK sent slot=0x%02X (dup=%d, paired_count=%u)",
                 slot, is_dup, s_pair_paired_count);
    }
    return true;
}

/* ── Chien de garde radio — réarmer une radio muette trop longtemps ──────────
 * Les modules NRF24 (clones) se figent avec le temps : ils cessent d'acquitter
 * et de recevoir alors que le SPI répond toujours (observé : ack% → 0, seul un
 * redémarrage du dongle rétablissait le lien). Au-delà de RF_REARM_SILENCE_MS
 * sans le moindre paquet, on réécrit la configuration RX — pas de redémarrage.
 * Limité en cadence, et sans effet si le périphérique est simplement éteint. */
/* Constantes déplacées dans rf_slot.h : c'est un contrat avec le clavier,
 * qui doit connaître le budget de silence qu'il ne faut pas dépasser. */



static void rearm_if_silent(rf_radio_t *radio, const rf_radio_cfg_t *cfg,
                            uint8_t slot, uint32_t *last_rearm_ms,
                            uint32_t now, const char *name)
{
    if (!radio->present) return;
    if ((uint32_t)(now - s_link[slot].last_rx_ms) <= RF_REARM_SILENCE_MS) return;
    if ((uint32_t)(now - *last_rearm_ms) <= RF_REARM_SILENCE_MS) return;
    rf_driver_rearm_rx(radio, cfg);
    *last_rearm_ms = now;
    ESP_LOGW(TAG, "chien de garde : radio %s réarmée (rien reçu depuis %lu ms)",
             name, (unsigned long)(now - s_link[slot].last_rx_ms));
}

static void rf_rx_watchdog(uint32_t now)
{
    static uint32_t s_kbd_rearm_ms = 0, s_mouse_rearm_ms = 0;
    rearm_if_silent(&s_kbd,   &s_kbd_cfg,   RF_SLOT_KBD,   &s_kbd_rearm_ms,   now, "clavier");
    rearm_if_silent(&s_mouse, &s_mouse_cfg, RF_SLOT_MOUSE, &s_mouse_rearm_ms, now, "souris");
}

static void rf_rx_task(void *arg)
{
    (void)arg;
    /* ⚠ ÉTAIT À 10 ms. Ce n'est en principe qu'un repli — l'IRQ de chaque radio
     * réveille `s_evt_sem` (voir nrf_irq_isr) — mais dans les faits c'est lui qui
     * gouvernait, et il PLAFONNE LE DÉBIT : la FIFO de réception d'un nRF24 ne
     * tient que 3 paquets, et un paquet arrivant FIFO pleine n'est pas acquitté,
     * donc perdu. Trois paquets par réveil, c'est 300/s au mieux.
     *
     * Invisible avec un clavier, qui produit quelques événements par seconde.
     * Une souris émet à 1 kHz PENDANT LES GESTES. Mesuré au banc le 2026-08-26 :
     * 8356 trames émises, 3135 acceptées — 37,5 %, très exactement les 3 sur 10
     * que laisse passer une fenêtre de 10 ms.
     *
     * 1 ms porte le plafond à ~3000/s. Le dongle est alimenté par l'USB : un
     * réveil par milliseconde ne coûte rien ici, contrairement au côté souris. */
    const TickType_t tick_period = pdMS_TO_TICKS(1);
    for (;;) {
        xSemaphoreTake(s_evt_sem, tick_period);

        if (s_pairing_mode) {
            rf_rx_pairing_service();
            continue;   /* skip normal RX/engine while pairing */
        }

        if (s_kbd.present)   drain_radio(&s_kbd,   RF_SLOT_KBD);
        if (s_mouse.present) drain_radio(&s_mouse, RF_SLOT_MOUSE);

        uint32_t now = esp_timer_get_time() / 1000;

        /* Perte de lien → repli, sur ce slot seulement. hb_check_timeout() ne
         * convenait plus : il parcourait un bitmap de matrice qui n'existe plus
         * ici, et n'aurait donc jamais rien relâché. */
        rf_safe_action_t a_kbd =
            rf_slot_link_check(&s_link[RF_SLOT_KBD], RF_SLOT_KBD, now, RF_LINK_LOST_MS);
        if (a_kbd != RF_SAFE_NONE) ESP_LOGW(TAG, "lien clavier perdu → touches relâchées");
        apply_safe_action(a_kbd);

        rf_safe_action_t a_mouse =
            rf_slot_link_check(&s_link[RF_SLOT_MOUSE], RF_SLOT_MOUSE, now, RF_LINK_LOST_MS);
        if (a_mouse != RF_SAFE_NONE) ESP_LOGW(TAG, "lien souris perdu → boutons relâchés");
        apply_safe_action(a_mouse);

        rf_rx_watchdog(now);   /* réparer une radio figée, sans redémarrage */
    }
}

bool rf_rx_start(void)
{
#if CONFIG_KASE_NRF_LINE_TEST
    rf_line_test_run();   /* bring-up: detect NRF line solder bridges (see Kconfig) */
#endif
    s_evt_sem = xSemaphoreCreateBinary();

    rf_radio_cfg_t kcfg = board_rf_radio1_cfg();
    rf_radio_cfg_t mcfg = board_rf_radio2_cfg();

    /* Per-set addressing (Plan RF-1): if this dongle is paired (NVS rf.paired_count
     * > 0), derive a unique address+channel from its own WiFi MAC. If unpaired,
     * rf_pairing_load_set_id_dongle() returns 0 and rf_apply_set_id is a no-op,
     * so lcfg/rcfg keep the board factory defaults (KaSe.01/.02, ch 76/82). */
    uint16_t set_id = rf_pairing_load_set_id_dongle();
    if (set_id == 0) {
        /* No NVS pairs — fall back to the set_id COMPUTED from this dongle's
         * own WiFi MAC. Halves that were paired in a previous lifetime stored
         * exactly this same id (derived from the dongle's MAC during the
         * pairing handshake), so reusing it here makes their TX addresses
         * match our RX addresses without any new pairing exchange.
         * Effect: as long as the dongle's MAC stays stable, NVS-erased dongles
         * still recover their bond with previously-paired halves. */
        set_id = rf_compute_set_id();
        ESP_LOGW(TAG, "no NVS pairs — using computed set_id 0x%04X for RX", set_id);
    }
    rf_apply_set_id(&kcfg, set_id, 0x01);   /* clavier → slot 0x01 */
    rf_apply_set_id(&mcfg, set_id, 0x02);   /* souris  → slot 0x02 */
    s_kbd_cfg = kcfg; s_mouse_cfg = mcfg;   /* keep live config for the radio watchdog */

    /* Load paired peer MACs from NVS at boot: rf_rx_pair_start() alone used to
     * populate them, so a plain reboot left them zeroed until a re-pairing.
     * s_pair_mac_* is the single source of truth, refreshed on each successful
     * pairing and reported over CDC (RF_PAIR_LIST). */
    rf_pairing_load_peers_dongle(s_pair_mac_left, s_pair_mac_right, &s_pair_paired_count);

    /* Park BOTH CSN HIGH before initialising either radio. The dongle shares
     * one SPI bus between NRF1 (csn=13) and NRF2 (csn=1 — a UART0 strap pin
     * that floats LOW at reset). If NRF2's CSN is still floating during
     * rf_driver_init(NRF1), NRF2 will silently latch NRF1's SPI traffic in
     * parallel, and the writes meant for NRF1 get corrupted by the parasitic
     * activity on the bus. Observed symptom: NRF1 boots with CONFIG=0x3E,
     * EN_AA=0, EN_RXADDR=0 while NRF2 looks fine — verify_rx FAILs on NRF1.
     * Pre-driving both CSN HIGH guarantees only one radio sees each command. */
    gpio_config_t csn_park = {
        .pin_bit_mask = (1ULL << kcfg.pin_csn) | (1ULL << mcfg.pin_csn),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&csn_park);
    gpio_set_level(kcfg.pin_csn, 1);
    gpio_set_level(mcfg.pin_csn, 1);

    rf_driver_init(&s_kbd, &kcfg);
    rf_driver_verify_rx(&s_kbd, &kcfg);    /* read-back config check (logs OK / per-reg FAIL) */
    rf_driver_init(&s_mouse, &mcfg);
    rf_driver_verify_rx(&s_mouse, &mcfg);

    if (!s_kbd.present && !s_mouse.present) {
        ESP_LOGE(TAG, "aucune radio NRF présente — RF désactivée");
        return false;
    }

    gpio_install_isr_service(0);
    if (s_kbd.present) {
        gpio_set_intr_type(kcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(kcfg.pin_irq, nrf_irq_isr, &s_kbd);
        rf_radio_set_irq_sem(&s_kbd, s_evt_sem);
    }
    if (s_mouse.present) {
        gpio_set_intr_type(mcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(mcfg.pin_irq, nrf_irq_isr, &s_mouse);
        rf_radio_set_irq_sem(&s_mouse, s_evt_sem);
    }

    xTaskCreatePinnedToCore(rf_rx_task, "rf_rx", 8192, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "RF RX démarrée (clavier=%d souris=%d)", s_kbd.present, s_mouse.present);

#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion : démarrer le moteur keymap embarqué. drain_radio lui remet les
     * demi-matrices ; il fusionne et sort le HID. */
    dongle_engine_start();
#endif

    return true;
}

void rf_rx_get_status(rf_link_status_t *out)
{
    /* L'âge rapporté est celui du dernier paquet reçu, pas du dernier battement :
     * c'est ce que la tâche suit désormais, et un lien actif n'envoie plus de
     * battements du tout (cf. la cadence adaptative, §5 du design du dongle). */
    uint32_t now = esp_timer_get_time() / 1000;
    out->link_kbd   = s_link[RF_SLOT_KBD].up;
    out->link_mouse = s_link[RF_SLOT_MOUSE].up;
    out->age_kbd_ms   = now - s_link[RF_SLOT_KBD].last_rx_ms;
    out->age_mouse_ms = now - s_link[RF_SLOT_MOUSE].last_rx_ms;
    out->pkt_rx_kbd   = s_kbd.pkt_rx;
    out->pkt_rx_mouse = s_mouse.pkt_rx;
    out->pkt_dup_kbd   = s_kbd.pkt_dup;
    out->pkt_dup_mouse = s_mouse.pkt_dup;
    out->link_q_kbd   = s_link_q[RF_SLOT_KBD];
    out->link_q_mouse = s_link_q[RF_SLOT_MOUSE];
    out->radio_kbd_present   = s_kbd.present;
    out->radio_mouse_present = s_mouse.present;
}

void rf_rx_copy_peer_macs(uint8_t mac_kbd[6], uint8_t mac_mouse[6])
{
    /* Live copy maintained by this task (loaded at boot, refreshed on pairing). */
    memcpy(mac_kbd,   s_pair_mac_left,  6);
    memcpy(mac_mouse, s_pair_mac_right, 6);
}

#endif /* TEST_HOST */
