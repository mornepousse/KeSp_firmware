/*
 * kbd_relay_tx.c — Smart keyboard NRF24 HID relay (PTX to dongle).
 *
 * Mirrors the radio-init + pairing-restore flow from half_scan_task.c, adapted
 * for a standalone keyboard that sends final HID reports (PKT_TYPE_HIDREPORT)
 * instead of raw matrix events.  No shared-bus lock needed: the keyboard has no
 * other device sharing the NRF SPI bus.
 *
 * Compiled only when CONFIG_KASE_KBD_WIRELESS=y (CMakeLists.txt guard).
 */

#include "kbd_relay_tx.h"
#include "rf_driver.h"
#if CONFIG_KASE_HALF_LINK_RX || CONFIG_KASE_DONGLE_FUSION
#include "half_link.h"   /* excursion (RX) ; HALF_LINK_TIMEOUT_MS (fusion) */
#endif
#include "rf_packet.h"
#include "rf_slot.h"
#include "rf_pairing.h"
#include "usb_presence.h"   /* route poll + kbd_active_route (USB-first auto-switch) */
#include "keymap.h"         /* keymaps[], KEYMAP_BLOB_BYTES — empreinte de config */
#include "config_sync.h"    /* config_fp_crc32 — garde-fou de sync (fusion) */
#include "keymap_sync.h"    /* keymap_rx_* — réassemblage de la keymap reçue par ACK */
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"     /* jauge : tension + état de charge dans STATUS */
#define KBD_BATT_DV()  batt_sense_dv()
#define KBD_BATT_CHG() batt_sense_charging()
#else
#define KBD_BATT_DV()  0
#define KBD_BATT_CHG() 0
#endif
#include "board.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"        /* esp_read_mac */
#include "esp_system.h"     /* esp_restart */
#include "esp_timer.h"
#if CONFIG_KASE_DISPLAY_MEMLCD
#include "memlcd_backend.h"   /* trame DISPLAY reçue dans l'ACK → écran */
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"    /* CE-pin scan during pairing */

static const char *TAG = "kbd_relay";

/* Refresh the current keyboard report every 25 ms so a lost key-up self-heals
 * (the NRF link is lossy + the HIDREPORT relay has no heartbeat reconciliation).
 * HID keyboard reports are idempotent, so resending the live state is harmless;
 * mouse is relative (non-idempotent) so it is NOT refreshed. */
#define KBD_RELAY_REFRESH_MS  10

/* Répétitions du dernier rapport après un changement, à KBD_RELAY_REFRESH_MS
 * d'intervalle. 5 × 10 ms = 50 ms d'auto-réparation : un key-up perdu cinq fois
 * de suite alors que chaque émission bénéficie déjà des 15 retransmissions ESB
 * n'arrive pas en pratique. Au-delà, le lien redevient silencieux — c'est ce qui
 * rend vraie la prémisse « émissions événementielles » du design. */
#define KBD_RELAY_REPEATS     5

/* ── Fallback NRF pin config ────────────────────────────────────────────────
 * Keyboard boards (V2, V2D) do not define BOARD_NRF_* — those are on the half
 * and dongle board headers.  The fallbacks here let the code compile and link
 * on any board; a real wireless-keyboard board.h would override them.
 * GPIO numbers are chosen from the free pool on the V2 PCB (no conflicts with
 * matrix, I2C display, or USB).
 * ─────────────────────────────────────────────────────────────────────────── */
#ifndef BOARD_NRF_SPI_HOST
#define BOARD_NRF_SPI_HOST      SPI2_HOST
#define BOARD_NRF_SPI_MOSI      GPIO_NUM_35
#define BOARD_NRF_SPI_MISO      GPIO_NUM_37
#define BOARD_NRF_SPI_SCK       GPIO_NUM_36
#define BOARD_NRF_SPI_CLOCK_HZ  (8 * 1000 * 1000)
#define BOARD_NRF_CSN_GPIO      GPIO_NUM_34
#define BOARD_NRF_CE_GPIO       GPIO_NUM_33
#define BOARD_NRF_IRQ_GPIO      GPIO_NUM_38
#define BOARD_NRF_CHANNEL       76
/* Slot 0x03 distinguishes the smart keyboard from half-left (0x01) / half-right (0x02). */
#define BOARD_NRF_ADDR_SUFFIX   0x03
#endif

/* ── Module state ───────────────────────────────────────────────────────── */

#if !CONFIG_KASE_HALF_LINK_RX
/* La radio de CE module. Sous HALF_LINK_RX elle n'existe pas : la puce
 * appartient à half_link, qui nous prête une excursion. */
static rf_radio_t s_radio;
#if CONFIG_KASE_DONGLE_FUSION
/* Cible dongle mémorisée (canal/adresse dérivés du set_id), pour rebasculer la
 * radio en PTX après une écoute USB. Fusion phase 2 : bascule dynamique. */
static rf_radio_cfg_t s_kbd_cfg;
static bool s_usb_listening = false;   /* la radio est-elle en PRX (mode USB) ? */
/* Dernière demi-matrice de la DROITE reçue (réémise par le dongle) en mode USB.
 * Le moteur de la gauche la lit via kbd_relay_remote_pressed() pour la fusionner
 * dans les colonnes hautes — chemin maître, étape 4b. */
static uint8_t          s_remote_bm[RF_HALF_BITMAP_BYTES];
static volatile bool    s_remote_changed;
static uint32_t         s_remote_ms;
/* Dernière demi-matrice LOCALE émise au dongle + quand. Le rafraîchissement
 * réaffirme les maintiens (sinon le dongle relâche la gauche sur silence — même
 * piège que half_link côté droite). */
static uint8_t          s_last_left_bm[RF_HALF_BITMAP_BYTES];
static uint32_t         s_last_left_ms;
#endif
#endif
static bool s_paired = false;

#if CONFIG_KASE_HALF_LINK_RX
/* Moitié gauche du Niphargus : la radio ne nous appartient PAS. Elle écoute la
 * droite en PRX, et half_link nous prête une excursion pour parler au dongle.
 * On mémorise donc la cible plutôt qu'une configuration de puce. */
static uint8_t s_dongle_ch;
static uint8_t s_dongle_addr[5];
#endif

/* TX serialization (engine send + refresh timer share the single radio) +
 * last keyboard report for the periodic refresh. */
static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_last_mod;
static uint8_t s_last_kb[6];
static kbd_refresh_t s_refresh;   /* répétition bornée — voir kbd_relay_tx.h */
static esp_timer_handle_t s_refresh_timer;   /* periodic refresh; stopped during sleep */

/* Bilan du chemin radio. Sans lui, une frappe perdue en mode RF est
 * INDISCERNABLE : kbd_tx_locked abandonnait le rapport en silence quand le
 * mutex n'était pas libre sous 20 ms, sans compteur ni journal. On distingue
 * désormais les trois issues — remis à la radio, abandonné faute de mutex,
 * refusé par l'excursion. */
static uint32_t s_tx_remis, s_tx_sans_mutex, s_tx_refuses;

/* Date de la DERNIÈRE émission, tous types confondus, et compteur de la trame
 * d'état. Un rapport HID entretient le lien aussi bien qu'une trame de
 * supervision : inutile d'en ajouter pendant la frappe. */
static uint32_t s_derniere_emission_ms;
static uint32_t s_dernier_ack_ms;       /* dernier ACK du dongle : « dongle vu » pour l'écran */
static uint8_t  s_status_seq;

#if CONFIG_KASE_DONGLE_FUSION
/* Sync auto de la keymap reçue par ACK payload (phase 3). La gauche PILOTE : à
 * la BEACON (une keymap d'empreinte ≠ la nôtre nous attend) elle entame un
 * pull et demande le prochain chunk manquant (SYNC_REQ) à chaque tick ; chaque
 * chunk arrive dans l'ACK de son émission suivante. Quand les 40 sont là, la
 * sauvegarde NVS se fait dans refresh_cb — hors du chemin TX et de son mutex. */
static keymap_rx_t   s_krx;
static bool          s_syncing;
static uint32_t      s_sync_target_fp;
static volatile bool s_sync_done;   /* 40/40 reçus : à enregistrer (refresh_cb) */
/* Émission brute d'une demi-matrice SANS armer la réémission — voir plus bas. */
static void send_matrix_frame(uint8_t half, const uint8_t *bitmap);
#endif

#if !CONFIG_KASE_HALF_LINK_RX
/* Prêt du bus SPI à l'écran (rf_bus.h) : le même mutex que kbd_tx_locked. */
#include "rf_bus.h"
bool rf_bus_lock(uint32_t timeout_ms)
{
    return s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void rf_bus_unlock(void) { if (s_tx_mutex) xSemaphoreGive(s_tx_mutex); }
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
#endif

static void kbd_tx_locked(const uint8_t *buf, uint8_t len)
{
    if (!s_tx_mutex) return;
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_tx_sans_mutex++;
        ESP_LOGW(TAG, "rapport ABANDONNE (mutex occupe) — remis %u, perdus %u+%u",
                 (unsigned)s_tx_remis, (unsigned)s_tx_sans_mutex,
                 (unsigned)s_tx_refuses);
        return;
    }
    {
#if CONFIG_KASE_HALF_LINK_RX
        /* La radio écoute la droite : on ne peut pas simplement émettre, il
         * faut en sortir et y revenir. half_link possède la puce et restaure
         * lui-même le canal du lien. */
        bool ok = half_link_excursion_tx(s_dongle_ch, s_dongle_addr, buf, len);
#else
        /* Canal retour ACK payload (sync auto keymap) : on récupère ce que le
         * dongle a glissé dans l'ACK. Loggé au banc pour le go/no-go (Task 3) ;
         * la Task 5 consommera ces octets (balise / chunk). */
        uint8_t ack[32];
        uint8_t ack_n = 0;
        bool ok = rf_driver_send_ap(&s_radio, buf, len, ack, &ack_n);
#if CONFIG_KASE_DONGLE_FUSION
        if (ack_n && !s_sync_done) {
            rf_sync_beacon_t b;
            rf_sync_chunk_t  c;
            if (rf_decode_sync_beacon(ack, ack_n, &b)) {
                /* Une keymap nous attend. On (re)part de zéro si c'est une autre
                 * empreinte que celle qu'on tirait déjà — le dongle a rechangé. */
                uint32_t own = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
                if (b.fp_target != own && (!s_syncing || b.fp_target != s_sync_target_fp)) {
                    keymap_rx_reset(&s_krx);
                    s_syncing = true;
                    s_sync_target_fp = b.fp_target;
                    ESP_LOGW(TAG, "sync keymap : balise fp=0x%08X (la nôtre 0x%08X), %u chunks — pull",
                             (unsigned)b.fp_target, (unsigned)own, (unsigned)b.n_chunks);
                }
            } else if (s_syncing && rf_decode_sync_chunk(ack, ack_n, &c)) {
                if (keymap_rx_chunk(&s_krx, c.idx, c.data) && keymap_rx_complete(&s_krx)) {
                    s_syncing  = false;
                    s_sync_done = true;   /* refresh_cb enregistre hors du chemin TX */
                }
            }
        }
#if CONFIG_KASE_DISPLAY_MEMLCD
        /* Trame DISPLAY (quand la sync n'a rien à dire) : la batterie de la
         * DROITE pour notre pied d'écran. La couche affichée à gauche est la
         * locale (elle a le moteur) : celle du dongle n'est qu'un écho. */
        rf_display_t d;
        if (ack_n && rf_decode_display(ack, ack_n, &d)) {
            uint8_t dv, chg; rf_display_autre(&d, RF_HALF_LEFT, &dv, &chg);
            memlcd_backend_set_remote(d.couche, dv ? dv : 0xFF, chg, d.dongle_ok);
        }
#endif
#endif
#endif
        if (ok) s_tx_remis++; else s_tx_refuses++;
        s_derniere_emission_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (ok) s_dernier_ack_ms = s_derniere_emission_ms;
        /* Trace par rapport, en DEBUG : c'est elle qui a montré, le 2026-09-11,
         * que la gauche émettait correctement la touche de réveil — et qu'un
         * pouce Super capturé restait collé. Une ligne par envoi est trop pour
         * l'usage courant ; à réactiver par le niveau de log quand une frappe
         * se perd sans qu'on sache où. */
        if (buf[0] == (PKT_TYPE_HIDREPORT << 4) && buf[1] == RF_HID_SUB_KBD)
            ESP_LOGD(TAG, "TX kbd mod=%02X kc=%02X %02X -> %s", buf[2], buf[3], buf[4],
                     ok ? "ok" : "REFUSE");
        xSemaphoreGive(s_tx_mutex);
        if (((s_tx_remis + s_tx_refuses) % 25) == 0)
            ESP_LOGW(TAG, "HID->dongle : %u remis, %u sans mutex, %u refuses",
                     (unsigned)s_tx_remis, (unsigned)s_tx_sans_mutex,
                     (unsigned)s_tx_refuses);
    }
}

/* esp_timer callback (10 ms): the single route poller, and the idempotent live
 * keyboard-state refresh. Polling here keeps the debounce + cached route fresh
 * even when idle. Only transmits over RF when RF is the active path — when USB is
 * plugged we must NOT relay (the dongle would type a duplicate on its own host). */
static void kbd_relay_refresh_cb(void *arg)
{
    (void)arg;
    usb_presence_poll(s_paired);
#if CONFIG_KASE_DONGLE_FUSION && !CONFIG_KASE_HALF_LINK_RX
    /* Fusion phase 2 : bascule dynamique de la radio selon la route.
     *  - USB : la gauche tape en local. Elle passe sa radio en PRX sur le lien
     *    (KaSe.03) pour ÉCOUTER la droite réémise par le dongle, et ANNONCE son
     *    mode au dongle par excursion. (Sur secteur : écouter est gratuit.)
     *  - sans-fil : radio en PTX vers le dongle (autonomie : elle n'écoute pas).
     * Un seul propriétaire (kbd_relay), pas de handoff : on écrit REG_CONFIG via
     * rearm_rx / set_ptx. Tout sous s_tx_mutex (le callback de scan n'émet qu'en
     * mode sans-fil, route-gated). */
    if (kbd_active_route() == KBD_OUT_USB) {
        if (!s_tx_mutex || xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
        if (!s_usb_listening) {
            rf_radio_cfg_t link = s_kbd_cfg;
            link.channel     = RF_CH_HALF_LINK;
            link.addr_suffix = RF_ADDR_HALF_LINK;
            rf_driver_rearm_rx(&s_radio, &link);
            s_usb_listening = true;
            ESP_LOGW(TAG, "fusion USB : ecoute la droite reemise (PRX ch=0x%02X KaSe.%02X)",
                     RF_CH_HALF_LINK, RF_ADDR_HALF_LINK);
        }
        /* Réémissions de la droite (heartbeats) : on mémorise sa demi-matrice ;
         * le moteur de la gauche la lit via kbd_relay_remote_pressed (étape 4b). */
        uint8_t rb[32];
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        while (rf_driver_rx_available(&s_radio)) {
            uint16_t rn = rf_driver_read_rx(&s_radio, rb, sizeof(rb));
            if (rn == 0) break;
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(rb, rn, &h)) {
                if (memcmp(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES) != 0) {
                    memcpy(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES);
                    s_remote_changed = true;
                }
                s_remote_ms = now;
            }
        }
        /* Silence de la droite → relâcher ce qu'elle tenait (même prudence que le
         * dongle : une moitié muette ne laisse pas une touche collée). */
        {
            bool held = (s_remote_bm[0] | s_remote_bm[1] | s_remote_bm[2] | s_remote_bm[3]) != 0;
            if (held && (uint32_t)(now - s_remote_ms) >= HALF_LINK_TIMEOUT_MS) {
                memset(s_remote_bm, 0, RF_HALF_BITMAP_BYTES);
                s_remote_changed = true;
            }
        }
        /* Annonce du mode au dongle par excursion (retour PRX KaSe.03). */
        if ((uint32_t)(now - s_derniere_emission_ms) >= 200u) {
            /* config_fp reste 0 ici : en USB le dongle se tait, la cohérence
             * des moteurs est sans objet. Buffer à RF_STATUS_LEN quand même —
             * rf_encode_status écrit 8 octets dans tous les cas. */
            rf_status_t st = { .batt_dV = KBD_BATT_DV(), .half = RF_HALF_LEFT, .charging = KBD_BATT_CHG(), .link_q = 0, .seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED),
                               .mode_usb = true };
            uint8_t sb[RF_STATUS_LEN];
            uint16_t sn = rf_encode_status(sb, &st);
            static const uint8_t link_addr[5] = { 'K','a','S','e', RF_ADDR_HALF_LINK };
            uint8_t dst[5] = { s_kbd_cfg.rx_addr[0], s_kbd_cfg.rx_addr[1],
                               s_kbd_cfg.rx_addr[2], s_kbd_cfg.rx_addr[3],
                               s_kbd_cfg.addr_suffix };
#if CONFIG_KASE_DISPLAY_MEMLCD
            /* L'ACK de l'annonce porte la trame DISPLAY (batterie de la droite)
             * : en USB c'est notre seule émission, donc notre seul canal descendant. */
            uint8_t ackp[32]; uint8_t ackn = 0;
            bool ok = rf_driver_oob_tx_ap(&s_radio, s_kbd_cfg.channel, dst, sb, (uint8_t)sn,
                                          RF_CH_HALF_LINK, link_addr, ackp, &ackn);
            rf_display_t d;
            if (ackn && rf_decode_display(ackp, ackn, &d)) {
                uint8_t dv, chg; rf_display_autre(&d, RF_HALF_LEFT, &dv, &chg);
                memlcd_backend_set_remote(d.couche, dv ? dv : 0xFF, chg, d.dongle_ok);
            }
#else
            bool ok = rf_driver_oob_tx(&s_radio, s_kbd_cfg.channel, dst, sb, (uint8_t)sn,
                                       RF_CH_HALF_LINK, link_addr);
#endif
            if (ok) s_dernier_ack_ms = now;         /* « dongle vu » aussi en mode USB */
            s_derniere_emission_ms = now;
        }
        xSemaphoreGive(s_tx_mutex);
        return;
    }
    /* Retour au mode sans-fil : rebasculer la radio en PTX vers le dongle. */
    if (s_usb_listening) {
        if (s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            rf_driver_set_ptx(&s_radio, &s_kbd_cfg);
            s_usb_listening = false;
            /* On quitte l'écoute : relâcher le distant, sinon une touche de la
             * droite resterait figée dans la fusion locale jusqu'au retour USB. */
            memset(s_remote_bm, 0, RF_HALF_BITMAP_BYTES);
            s_remote_changed = true;
            ESP_LOGW(TAG, "fusion : retour emission PTX vers le dongle");
            xSemaphoreGive(s_tx_mutex);
        }
    }
#endif
    if (kbd_active_route() != KBD_OUT_RF) return;
#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion, mode sans-fil : RÉAFFIRMER la matrice locale tant qu'une touche est
     * tenue. matrix_scan n'émet que sur CHANGEMENT ; sans ce rafraîchissement une
     * touche gauche tenue ne produit plus rien et le dongle relâche la moitié
     * gauche après HALF_LINK_TIMEOUT_MS (« la touche gauche se relâche seule »,
     * banc 2026-09-13). Même règle que la droite (half_link_tx_refresh) : muet au
     * repos (bitmap vide → autonomie), entretenu sur maintien. */
    {
        bool tenu = (s_last_left_bm[0] | s_last_left_bm[1] |
                     s_last_left_bm[2] | s_last_left_bm[3]) != 0;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (tenu && (uint32_t)(now - s_last_left_ms) >= 100u) {
            uint8_t bm[RF_HALF_BITMAP_BYTES];
            memcpy(bm, s_last_left_bm, RF_HALF_BITMAP_BYTES);
            kbd_relay_send_matrix(RF_HALF_LEFT, bm);   /* réaffirme + met à jour l'horodatage */
            return;
        }
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION
    /* Sync auto (phase 3), APRÈS la réaffirmation des maintiens : un maintien
     * garde la priorité, le pull se met en pause pendant et reprend après —
     * jamais une touche relâchée à tort pour une keymap (la panne du 2026-09-13). */
    {
        /* 40/40 reçus : la keymap complète est dans s_krx.buf. Le drapeau et le
         * buffer sont ÉCRITS sous s_tx_mutex (kbd_tx_locked, depuis la tâche de
         * scan) : on les CONSOMME sous le même mutex, sinon la copie pourrait
         * lire un buffer pas encore entièrement visible (revue 2026-09-13). La
         * NVS (ms) se fait ensuite HORS mutex — kbd_tx_locked le reprend plus
         * bas et il n'est pas récursif. Le STATUS suivant annoncera la nouvelle
         * empreinte → le dongle coupera la balise. */
        bool a_enregistrer = false;
        uint32_t cible = 0;
        if (s_sync_done && s_tx_mutex &&
            xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_sync_done) {
                memcpy((uint8_t *)keymaps, s_krx.buf, KEYMAP_BLOB_BYTES);
                cible = s_sync_target_fp;
                s_sync_done = false;
                a_enregistrer = true;
            }
            xSemaphoreGive(s_tx_mutex);
        }
        if (a_enregistrer) {
            bool saved = save_keymaps((uint16_t *)keymaps, KEYMAP_BLOB_BYTES);
            uint32_t fp = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
            ESP_LOGW(TAG, "sync keymap : 40/40 recus, fp=0x%08X %s (cible 0x%08X) — %s",
                     (unsigned)fp, fp == cible ? "= cible" : "!= CIBLE", (unsigned)cible,
                     saved ? "enregistree en NVS" : "ECHEC NVS");
        }
    }
    if (s_syncing) {
        /* Un REQ toutes les 100 ms (gate par horodatage : le timer tourne à
         * KBD_RELAY_REFRESH_MS = 10 ms, il ne faut PAS un REQ par tick — la
         * revue du 2026-09-13 a relevé 100 REQ/s réels contre 10 documentés).
         * Chaque ACK rapporte le chunk demandé → ~10 chunks/s, 40 en ~4 s.
         * Trafic borné, seulement tant qu'on diverge. */
        static uint32_t s_dernier_req_ms;
        uint32_t maintenant = (uint32_t)(esp_timer_get_time() / 1000);
        if ((uint32_t)(maintenant - s_dernier_req_ms) >= 100u) {
            s_dernier_req_ms = maintenant;
            rf_sync_req_t q = { .next = keymap_rx_next(&s_krx) };
            uint8_t rb[4];
            uint16_t rn = rf_encode_sync_req(rb, &q);
            kbd_tx_locked(rb, (uint8_t)rn);
        }
        return;
    }
#endif
    /* Réémission bornée : sans changement récent, on se tait. usb_presence_poll
     * ci-dessus reste appelé à chaque tick — c'est lui qui garde le routage
     * frais, il ne doit pas dépendre de l'activité clavier. */
    if (kbd_refresh_step(&s_refresh)) {
#if CONFIG_KASE_DONGLE_FUSION
        /* Fusion : ce qui se répète, c'est le DERNIER BITMAP — même vide, un
         * relâchement perdu se répare ainsi aussi, sans violer « muet au repos »
         * puisque c'est borné. Jamais un rapport HID ici. */
        send_matrix_frame(RF_HALF_LEFT, s_last_left_bm);
#else
        uint8_t buf[9];
        rf_encode_hidreport_kbd(buf, s_last_mod, s_last_kb);
        kbd_tx_locked(buf, 9);
#endif
        return;
    }

    /* Supervision. Le dongle relâche les touches d'un slot muet depuis
     * RF_LINK_LOST_MS — protection contre un clavier disparu, sans quoi une
     * touche resterait collée chez l'hôte. Or la réémission ci-dessus est
     * BORNÉE : une touche simplement MAINTENUE ne produit aucun changement,
     * donc plus aucun rapport, et le dongle la relâchait au bout de ~2 s.
     * Backspace remontait toute seule, constaté au banc le 2026-09-08.
     *
     * On ne s'annonce que si rien d'autre n'est parti depuis RF_STATUS_PERIOD_MS :
     * pendant la frappe, les rapports HID suffisent, et le repos reste à une
     * seule trame par seconde — négligeable pour R1. */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!rf_status_doit_emettre(now, s_derniere_emission_ms, RF_STATUS_PERIOD_MS))
        return;
    /* Garde-fou de sync (fusion) : on annonce l'empreinte de NOTRE keymap. Le
     * dongle, qui tape en sans-fil avec la SIENNE, compare et signale une
     * divergence — sinon deux moteurs taperaient différemment en silence.
     * Calculée à la volée (1/s ici) : pas de cache, donc jamais périmée. */
    rf_status_t st = { .batt_dV = KBD_BATT_DV(), .half = RF_HALF_LEFT, .charging = KBD_BATT_CHG(), .link_q = 0, .seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED),
                       .config_fp = config_fp_crc32((const uint8_t *)keymaps,
                                                    KEYMAP_BLOB_BYTES) };
    uint8_t buf[RF_STATUS_LEN];
    uint16_t n = rf_encode_status(buf, &st);
    kbd_tx_locked(buf, (uint8_t)n);
}

/* ── Radio config helper (mirrors board_nrf_cfg in half_scan_task.c) ─────── */

static rf_radio_cfg_t kbd_nrf_cfg(void)
{
    rf_radio_cfg_t c = {
        .spi_host         = BOARD_NRF_SPI_HOST,
        .pin_mosi         = BOARD_NRF_SPI_MOSI,
        .pin_miso         = BOARD_NRF_SPI_MISO,
        .pin_sck          = BOARD_NRF_SPI_SCK,
        .clock_hz         = BOARD_NRF_SPI_CLOCK_HZ,
        .pin_csn          = BOARD_NRF_CSN_GPIO,
        .pin_ce           = BOARD_NRF_CE_GPIO,
        .pin_irq          = BOARD_NRF_IRQ_GPIO,
        .channel          = BOARD_NRF_CHANNEL,
        .rx_addr          = { 'K', 'a', 'S', 'e' },   /* base address (4 bytes) */
        .addr_suffix      = BOARD_NRF_ADDR_SUFFIX,
        .shares_bus_first = true,   /* keyboard has exactly one NRF radio */
    };
    return c;
}

/* ── Active pairing task (mirrors half_pairing_task; no SPI-bus lock) ──────
 * Sends PKT_PAIR_REQ on the rendezvous declaring this device as a smart
 * keyboard, awaits PKT_PAIR_ACK, saves the assigned set_id/slot to NVS, then
 * reboots so kbd_relay_init() comes up paired (relay active). Runs only while
 * unpaired; the dongle's pairing window must be open (KS_CMD_RF_PAIR_START). */
#if !CONFIG_KASE_HALF_LINK_RX
/* Inutile quand le lien inter-moitiés tient la radio : l'appairage actif
 * suppose d'écouter le canal de rendez-vous, donc d'abandonner l'écoute de la
 * droite. Compilée hors de ce cas, elle serait une fonction statique morte. */
static void kbd_pairing_task(void *arg)
{
    (void)arg;
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    uint8_t req[8];
    rf_encode_pair_req(req, my_mac, BOARD_NRF_ADDR_SUFFIX);
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;

    /* CE-pin scan: the register-read probe confirms SPI (CSN/SCK/MOSI/MISO) but
     * NOT CE (the TX trigger). On the bodged V2D the CE wire is uncertain, so try
     * each candidate GPIO as CE: the one that lets a REQ reach the dongle (ACK
     * comes back) is the real CE. Logs the winner so it can be set in board.h.
     * Only cfg.pin_ce changes (a GPIO toggled directly) — no SPI re-init. */
    /* La broche CE du board.h passe TOUJOURS en premier : sur une carte dont le
     * brochage est verifie a la netlist, c'est la bonne, et le balayage n'a
     * aucune raison d'etre. Il ne suivait pas cette regle et la liste ci-dessous
     * ne contient meme pas le CE du Niphargus (GPIO15) — l'appairage ne pouvait
     * donc jamais aboutir sur cette carte. */
    static const int ce_cand[] = {
        BOARD_NRF_CE_GPIO,
#if CONFIG_KASE_RF_CE_SCAN
        47, 45, 38, 39, 40, 33, 34, 35, 36,
#endif
    };
    rf_pair_ack_t ack;
    bool acked = false;
    int win_ce = -1;

    for (unsigned ci = 0; ci < sizeof(ce_cand) / sizeof(ce_cand[0]) && !acked; ci++) {
        int ce = ce_cand[ci];
        ESP_LOGW(TAG, "pairing: trying CE=GPIO%d ...", ce);
        gpio_reset_pin(ce);
        gpio_set_direction(ce, GPIO_MODE_OUTPUT);
        gpio_set_level(ce, 0);
        s_radio.cfg.pin_ce = ce;
        for (int i = 0; i < 12 && !acked; i++) {        /* ~3 s per candidate */
            rf_driver_set_tx_address(&s_radio, pair_addr);
            rf_driver_set_channel(&s_radio, RF_PAIR_CHANNEL);
            rf_driver_send(&s_radio, req, 8);
            uint8_t rxb[32];
            uint16_t n = rf_driver_pair_listen(&s_radio, RF_PAIR_CHANNEL, pair_addr,
                                               rxb, sizeof(rxb), 150);
            if (n && rf_decode_pair_ack(rxb, n, &ack)) { acked = true; win_ce = ce; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (acked) {
        ESP_LOGW(TAG, "pairing: ACK on CE=GPIO%d! set_id=0x%04X slot=0x%02X — saving + reboot",
                 win_ce, ack.set_id, ack.slot);
        rf_pairing_save_half(ack.set_id, ack.slot, ack.dongle_wifi_mac);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "pairing: no CE candidate worked — REQ never reached the dongle "
                  "(check CE wiring / dongle window)");
    vTaskDelete(NULL);
}
#endif /* !CONFIG_KASE_HALF_LINK_RX */

/* ── Public API ─────────────────────────────────────────────────────────── */

void kbd_relay_init(void)
{
    s_paired = false;

    usb_presence_init();   /* VBUS sense GPIO for the USB-first auto-switch */

    rf_radio_cfg_t nrf_cfg = kbd_nrf_cfg();

    /* Load pairing state from NVS (mirrors half_scan_task.c init).
     * rf_pairing_load_set_id_half returns 0 if no NVS entry (unpaired);
     * rf_apply_set_id is a no-op for set_id 0/0xFFFF → cfg keeps board defaults. */
    uint8_t slot = BOARD_NRF_ADDR_SUFFIX;
    uint16_t set_id = rf_pairing_load_set_id_half(BOARD_NRF_ADDR_SUFFIX, &slot);
    rf_apply_set_id(&nrf_cfg, set_id, slot);

#if CONFIG_KASE_HALF_LINK_RX
    /* PAS d'init : half_link_rx_start() a déjà configuré la puce en PRX sur le
     * canal du lien, et une seconde init l'écraserait — c'est très exactement
     * la panne qui a fait écouter cette moitié sur le canal du dongle. On ne
     * retient que la cible de l'excursion.
     *
     * Conséquence assumée : l'appairage actif ne peut pas se faire ici, car il
     * suppose d'écouter le canal de rendez-vous, donc d'abandonner l'écoute de
     * la droite. Une moitié non appairée le reste, en le disant. */
    s_dongle_ch = nrf_cfg.channel;
    memcpy(s_dongle_addr, nrf_cfg.rx_addr, 4);
    s_dongle_addr[4] = nrf_cfg.addr_suffix;
    if (set_id == 0 || set_id == 0xFFFF) {
        ESP_LOGE(TAG, "kbd_relay: NON APPAIRE et le lien inter-moities tient la "
                      "radio — appairer d'abord (dongle + KS_CMD_RF_PAIR_START) "
                      "sur un build sans HALF_LINK_RX");
        return;   /* s_paired reste false : pas de relais, mais le lien vit */
    }
#else
    esp_err_t err = rf_driver_init_tx(&s_radio, &nrf_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF PTX init failed (%d) — wireless relay disabled", err);
        return;   /* s_paired stays false */
    }
#if CONFIG_KASE_DONGLE_FUSION
    s_kbd_cfg = nrf_cfg;   /* cible dongle mémorisée pour rebasculer en PTX (fusion phase 2) */
#endif
#endif

#if !CONFIG_KASE_HALF_LINK_RX
    if (set_id == 0 || set_id == 0xFFFF) {
        /* Unpaired: spawn the active pairing task. It REQs on the rendezvous and,
         * on ACK, saves NVS + reboots (relay comes up active). Open the dongle's
         * pairing window (KS_CMD_RF_PAIR_START) to complete it. */
        ESP_LOGW(TAG, "kbd_relay: unpaired — starting pairing task (open the dongle window)");
        xTaskCreate(kbd_pairing_task, "kbd_pair", 4096, NULL, 5, NULL);
        return;   /* s_paired becomes true after the post-pairing reboot */
    }
#endif

    ESP_LOGI(TAG, "kbd_relay: paired set_id=0x%04X slot=0x%02X — relay active",
             set_id, slot);
    s_tx_mutex = xSemaphoreCreateMutex();
    s_paired = true;

    /* Periodic keyboard-state refresh: self-heals lost key-ups over the lossy
     * link (no heartbeat reconciliation on the HIDREPORT path). Streams only
     * après un changement seulement (kbd_refresh_arm), pour un nombre borné de
     * ticks : un clavier au repos est réellement silencieux. */
    const esp_timer_create_args_t ta = {
        .callback = kbd_relay_refresh_cb, .name = "kbd_refresh",
    };
    if (esp_timer_create(&ta, &s_refresh_timer) == ESP_OK)
        esp_timer_start_periodic(s_refresh_timer, (uint64_t)KBD_RELAY_REFRESH_MS * 1000);
}

#if CONFIG_KASE_DONGLE_FUSION && !CONFIG_KASE_HALF_LINK_RX
/* Fusion phase 2 (4b) : le moteur de la gauche lit la demi-matrice de la droite
 * réémise par le dongle (reçue en écoute USB) — équivalent de
 * half_link_remote_pressed sur le maître pré-fusion. */
bool kbd_relay_remote_pressed(uint8_t row, uint8_t col)
{
    return rf_bitmap_get(s_remote_bm, row, col);
}

/* L'état distant a-t-il changé depuis le dernier appel ? Consomme le drapeau. */
bool kbd_relay_remote_changed(void)
{
    bool ch = s_remote_changed;
    s_remote_changed = false;
    return ch;
}
#endif

/* ── Light-sleep hooks (V2D wireless) ─────────────────────────────────────── */

void kbd_relay_sleep_prepare(void)
{
    /* Stop the refresh timer (no NRF access during sleep), then take the TX mutex
     * and power the NRF down. Hold the mutex across sleep so nothing transmits. */
    if (s_refresh_timer) esp_timer_stop(s_refresh_timer);
    if (s_tx_mutex) xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(50));
#if !CONFIG_KASE_HALF_LINK_RX
    rf_driver_power_down(&s_radio);   /* radio d'autrui sous HALF_LINK_RX */
#endif
}

void kbd_relay_wake_restore(void)
{
#if !CONFIG_KASE_HALF_LINK_RX
    rf_driver_power_up(&s_radio);
#endif
    if (s_tx_mutex) xSemaphoreGive(s_tx_mutex);
    if (s_refresh_timer) esp_timer_start_periodic(s_refresh_timer,
                                                  (uint64_t)KBD_RELAY_REFRESH_MS * 1000);
}

bool kbd_relay_active(void)
{
    return s_paired;
}

bool kbd_relay_dongle_vu(void)
{
    if (!s_dernier_ack_ms) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (now - s_dernier_ack_ms) <= RF_LINK_LOST_MS;
}

void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6])
{
    s_last_mod = modifier;
    memcpy(s_last_kb, kb, 6);
    kbd_refresh_arm(&s_refresh, KBD_RELAY_REPEATS);
    uint8_t buf[9];
    rf_encode_hidreport_kbd(buf, modifier, kb);
    kbd_tx_locked(buf, 9);
}

void kbd_relay_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    uint8_t buf[6];
    rf_encode_hidreport_mouse(buf, buttons, x, y, wheel);
    kbd_tx_locked(buf, 6);   /* relative — never refreshed */
}

#if CONFIG_KASE_DONGLE_FUSION
/* Fusion : la moitié n'envoie plus de HID fini, elle émet sa demi-matrice BRUTE
 * au dongle, qui fusionne les deux moitiés et fait tourner le moteur. Même
 * chemin d'émission que send_kbd (kbd_tx_locked : excursion ou direct).
 *
 * ⚠ Réaffirmation des maintiens : comme partout dans cette chaîne, « émettre sur
 * changement » ne compose pas avec « relâcher sur silence » (cf. CLAUDE.md et
 * half_link). Le dongle relâche une moitié muette après HALF_LINK_TIMEOUT_MS, donc
 * un maintien doit être ré-émis périodiquement. La cadence de rafraîchissement de
 * la matrice est une pièce du BANC (elle se règle contre le timeout réel du
 * dongle) — voir docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md. */
static void send_matrix_frame(uint8_t half, const uint8_t *bitmap)
{
    rf_matrix_t m;
    m.half = half;
    memcpy(m.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    m.seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED);   /* réutilise le compteur de séquence du relais */
    uint8_t buf[8];
    uint16_t n = rf_encode_matrix(buf, &m);
    uint32_t refus_avant = s_tx_refuses;
    if (n) kbd_tx_locked(buf, (uint8_t)n);
    /* Diagnostic permanent (rare, ~1 % au banc) : QUELLE trame l'ESB a refusée
     * après ses 15 retransmissions. C'est cette trame-là que la réémission
     * bornée ci-dessous répète — sans elle, un appui bref était perdu. */
    if (s_tx_refuses != refus_avant)
        ESP_LOGW(TAG, "MATRIX refusee bm=%02X%02X%02X%02X — repetee par la reemission bornee",
                 bitmap[0], bitmap[1], bitmap[2], bitmap[3]);
    /* Mémorise l'état local pour la réaffirmation des maintiens (refresh_cb). */
    memcpy(s_last_left_bm, bitmap, RF_HALF_BITMAP_BYTES);
    s_last_left_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

void kbd_relay_send_matrix(uint8_t half, const uint8_t *bitmap)
{
    send_matrix_frame(half, bitmap);
    /* Réémission BORNÉE armée au changement — KBD_RELAY_REPEATS × 10 ms, puis
     * silence. Une trame de CHANGEMENT refusée par l'ESB n'avait qu'une seule
     * chance : la réaffirmation à 100 ms ne couvre que les maintiens, donc un
     * appui bref perdu n'était jamais réparé (Super tenu + Q : Q jamais arrivé
     * à l'hôte, une trame refusée sur le créneau — banc 2026-09-13). Même
     * mécanisme que le chemin HID pré-fusion (test_repos_ne_reemet_pas) ; le
     * dongle déduplique par contenu, les répétitions sont gratuites pour lui.
     * Les répétitions passent par send_matrix_frame : elles ne se réarment pas. */
    kbd_refresh_arm(&s_refresh, KBD_RELAY_REPEATS);
}
#endif
