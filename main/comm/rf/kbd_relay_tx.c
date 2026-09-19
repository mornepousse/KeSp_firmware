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
#include "radio_owner.h"   /* la puce a un propriétaire : ce module est une politique */
#if CONFIG_KASE_DONGLE_FUSION
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
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* hook radio, veto sync, suffixe HB */
#endif
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"    /* CE-pin scan during pairing */

static const char *TAG = "kbd_relay";

/* Refresh the current keyboard report every 25 ms so a lost key-up self-heals
 * (the NRF link is lossy + the HIDREPORT relay has no heartbeat reconciliation).
 * HID keyboard reports are idempotent, so resending the live state is harmless;
 * mouse is relative (non-idempotent) so it is NOT refreshed.
 * KBD_RELAY_REFRESH_MS / KBD_RELAY_REPOS_MS : kbd_relay_tx.h (cadence pure). */

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

/* La puce (verrou, mode PTX/PRX, cible, sommeil) est à radio_owner.c. Ici :
 * quoi émettre, vers qui, et l'écoute de la droite en mode USB. */
#if CONFIG_KASE_DONGLE_FUSION
/* Cible dongle (canal/adresse dérivés du set_id) : PTX sans fil ; en USB la
 * puce passe en PRX sur le lien (KaSe.03) — radio_mode() dit où on en est. */
static rf_radio_cfg_t s_kbd_cfg;
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
static bool s_paired = false;


/* Dernier rapport clavier, pour le rafraîchissement périodique. */
static uint8_t s_last_mod;
static uint8_t s_last_kb[6];
static kbd_refresh_t s_refresh;   /* répétition bornée — voir kbd_relay_tx.h */
static esp_timer_handle_t s_refresh_timer;   /* periodic refresh; stopped during sleep */

/* Bilan du chemin radio. Sans lui, une frappe perdue en mode RF est
 * INDISCERNABLE. Trois issues : remis (ACK), refusé par le propriétaire (verrou
 * pris sous 20 ms, ou puce en PRX / endormie — l'émission n'a pas eu lieu),
 * refusé par l'ESB (MAX_RT). */
static uint32_t s_tx_remis, s_tx_sans_mutex, s_tx_refuses;

/* Date de la DERNIÈRE émission, tous types confondus, et compteur de la trame
 * d'état. Un rapport HID entretient le lien aussi bien qu'une trame de
 * supervision : inutile d'en ajouter pendant la frappe. */
static uint32_t s_derniere_emission_ms;
static uint8_t  s_sans_ack_ecran = 3;   /* émissions consécutives sans ACK : « dongle vu » pour l'écran (3 = pas encore vu) */
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

static void kbd_tx_locked(const uint8_t *buf, uint8_t len)
{
    if (!radio_presente()) return;
    if (radio_mode() != RADIO_PTX) {
        /* Route USB : la puce écoute la droite, le callback de scan n'émet pas
         * en principe (route-gated). Si on arrive ici, c'est un croisement de
         * route : compté, pas émis. */
        s_tx_sans_mutex++;
        return;
    }
    {
        /* Canal retour ACK payload (sync auto keymap) : on récupère ce que le
         * dongle a glissé dans l'ACK. */
        uint8_t ack[32];
        uint8_t ack_n = 0;
        uint32_t indispo_avant, indispo_apres;
        radio_stats(NULL, NULL, &indispo_avant);
        bool ok = radio_send_ap(buf, len, ack, &ack_n, 20);
        radio_stats(NULL, NULL, &indispo_apres);
        if (indispo_apres != indispo_avant) {   /* rien n'est parti : verrou pris, puce endormie */
            s_tx_sans_mutex++;
            ESP_LOGW(TAG, "rapport ABANDONNE (radio indisponible) — remis %u, perdus %u+%u",
                     (unsigned)s_tx_remis, (unsigned)s_tx_sans_mutex, (unsigned)s_tx_refuses);
            return;
        }
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
#if CONFIG_KASE_VEILLE
                    veille_veto(VEILLE_VETO_SYNC, true);   /* pas de veille en plein tirage */
#endif
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
#endif
        if (ok) s_tx_remis++; else s_tx_refuses++;
        s_derniere_emission_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (ok) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;
        /* Trace par rapport, en DEBUG : c'est elle qui a montré, le 2026-09-11,
         * que la gauche émettait correctement la touche de réveil — et qu'un
         * pouce Super capturé restait collé. Une ligne par envoi est trop pour
         * l'usage courant ; à réactiver par le niveau de log quand une frappe
         * se perd sans qu'on sache où. */
        if (buf[0] == (PKT_TYPE_HIDREPORT << 4) && buf[1] == RF_HID_SUB_KBD)
            ESP_LOGD(TAG, "TX kbd mod=%02X kc=%02X %02X -> %s", buf[2], buf[3], buf[4],
                     ok ? "ok" : "REFUSE");
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
/* Période du timer de rafraîchissement : 10 ms tant qu'il y a quelque chose à
 * répéter (touche tenue, réparation bornée, pull de sync), 100 ms au repos.
 * À 10 ms permanents, le processeur sortait d'oisiveté 100 fois par seconde
 * pour un memcmp et un poll de route — et à chaque fois le DFS remontait la PLL.
 * La route (débounce 50 ms) et l'annonce USB (200 ms) tiennent à 100 ms. */
static uint32_t s_periode_ms;
static void kbd_relay_timer_set(uint32_t ms)
{
    if (!s_refresh_timer || s_periode_ms == ms) return;
    esp_timer_stop(s_refresh_timer);
    esp_timer_start_periodic(s_refresh_timer, (uint64_t)ms * 1000);
    s_periode_ms = ms;
}
static void kbd_relay_refresh_body(void);
static void kbd_relay_refresh_cb(void *arg)
{
    (void)arg;
    kbd_relay_refresh_body();
    bool reparation = s_refresh.left != 0, tenu = false, sync = false, ecoute_usb = false;
#if CONFIG_KASE_DONGLE_FUSION
    tenu = (s_last_left_bm[0] | s_last_left_bm[1] | s_last_left_bm[2] | s_last_left_bm[3]) != 0;
    sync = s_syncing || s_sync_done;
    ecoute_usb = (radio_mode() == RADIO_PRX);   /* route USB : ce tick vide la FIFO des trames de la droite */
#else
    for (int i = 0; i < 6; i++) if (s_last_kb[i]) tenu = true;   /* rapport HID tenu (V2D) */
    if (s_last_mod) tenu = true;
#endif
    kbd_relay_timer_set(kbd_relay_cadence_ms(reparation, tenu, sync, ecoute_usb));
}
#if CONFIG_KASE_DONGLE_FUSION
/* Consommateur des trames de la droite (réémises par le dongle sur KaSe.03),
 * appelé par le propriétaire sous son verrou : vidange périodique et vidange
 * AVANT chaque excursion. */
static void kbd_relay_rx_droite(const uint8_t *rb, uint16_t rn, void *ctx)
{
    uint32_t now = *(uint32_t *)ctx;
    rf_heartbeat_t h;
    if (!rf_decode_heartbeat(rb, rn, &h)) return;
    if (memcmp(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES) != 0) {
        memcpy(s_remote_bm, h.bitmap, RF_HALF_BITMAP_BYTES);
        s_remote_changed = true;
    }
    s_remote_ms = now;
}
#endif

static void kbd_relay_refresh_body(void)
{
    usb_presence_poll(s_paired);
#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion phase 2 : bascule dynamique de la radio selon la route.
     *  - USB : la gauche tape en local. Elle passe sa radio en PRX sur le lien
     *    (KaSe.03) pour ÉCOUTER la droite réémise par le dongle, et ANNONCE son
     *    mode au dongle par excursion. (Sur secteur : écouter est gratuit.)
     *  - sans-fil : radio en PTX vers le dongle (autonomie : elle n'écoute pas).
     * Le propriétaire (radio_owner) tient le mode et le verrou ; ici on ne fait
     * que demander PRX(lien) ou PTX(dongle) selon la route. Le callback de scan
     * n'émet qu'en mode sans-fil (route-gated) — et en PRX le propriétaire
     * refuserait de toute façon. */
    if (kbd_active_route() == KBD_OUT_USB) {
        if (radio_mode() != RADIO_PRX) {
            /* Le lien est à adresse FIXE 'KaSe'.03 (c'est ce que le dongle vise
             * en réémettant la droite), pas l'adresse dérivée du set_id : avant
             * le propriétaire, l'écoute partait sur la mauvaise adresse et la
             * première excursion la « corrigeait » en restaurant 'KaSe'.03 —
             * ça marchait par accident (constaté le 2026-09-19). */
            rf_radio_cfg_t link = s_kbd_cfg;
            memcpy(link.rx_addr, "KaSe", 4);
            link.channel     = RF_CH_HALF_LINK;
            link.addr_suffix = RF_ADDR_HALF_LINK;
            if (!radio_mode_set(RADIO_PRX, &link)) return;
            ESP_LOGW(TAG, "fusion USB : ecoute la droite reemise (PRX ch=0x%02X KaSe.%02X)",
                     RF_CH_HALF_LINK, RF_ADDR_HALF_LINK);
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        /* Réémissions de la droite (heartbeats) : le propriétaire lit la FIFO et
         * nous livre les trames ; on mémorise sa demi-matrice, le moteur de la
         * gauche la lit via kbd_relay_remote_pressed (étape 4b). */
        radio_rx_drain(kbd_relay_rx_droite, &now);
        /* Silence de la droite → relâcher ce qu'elle tenait (même prudence que le
         * dongle : une moitié muette ne laisse pas une touche collée). */
        {
            bool held = (s_remote_bm[0] | s_remote_bm[1] | s_remote_bm[2] | s_remote_bm[3]) != 0;
            if (held && (uint32_t)(now - s_remote_ms) >= HALF_LINK_TIMEOUT_MS) {
                memset(s_remote_bm, 0, RF_HALF_BITMAP_BYTES);
                s_remote_changed = true;
            }
        }
        /* Annonce du mode au dongle par excursion : le propriétaire VIDE la FIFO
         * dans notre consommateur AVANT de partir (l'excursion finit par un
         * FLUSH_RX) et revient écouter KaSe.03. */
        if ((uint32_t)(now - s_derniere_emission_ms) >= 200u) {
            /* config_fp reste 0 ici : en USB le dongle se tait, la cohérence
             * des moteurs est sans objet. Buffer à RF_STATUS_LEN quand même —
             * rf_encode_status écrit 8 octets dans tous les cas. */
            rf_status_t st = { .batt_dV = KBD_BATT_DV(), .half = RF_HALF_LEFT, .charging = KBD_BATT_CHG(), .link_q = 0, .seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED),
                               .mode_usb = true };
            uint8_t sb[RF_STATUS_LEN];
            uint16_t sn = rf_encode_status(sb, &st);
            uint8_t dst[5] = { s_kbd_cfg.rx_addr[0], s_kbd_cfg.rx_addr[1],
                               s_kbd_cfg.rx_addr[2], s_kbd_cfg.rx_addr[3],
                               s_kbd_cfg.addr_suffix };
            bool ok = radio_excursion_tx(s_kbd_cfg.channel, dst, sb, (uint8_t)sn, kbd_relay_rx_droite, &now);
            if (ok) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;                  /* « dongle vu » aussi en mode USB */
            s_derniere_emission_ms = now;
        }
        return;
    }
    /* Retour au mode sans-fil : rebasculer la radio en PTX vers le dongle. */
    if (radio_mode() == RADIO_PRX) {
        if (radio_mode_set(RADIO_PTX, &s_kbd_cfg)) {
            /* On quitte l'écoute : relâcher le distant, sinon une touche de la
             * droite resterait figée dans la fusion locale jusqu'au retour USB. */
            memset(s_remote_bm, 0, RF_HALF_BITMAP_BYTES);
            s_remote_changed = true;
            ESP_LOGW(TAG, "fusion : retour emission PTX vers le dongle");
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
         * buffer sont ÉCRITS pendant une émission (kbd_tx_locked, sous le verrou
         * du propriétaire, depuis la tâche de scan) : on les CONSOMME sous ce
         * même verrou (radio_lock), sinon la copie pourrait lire un buffer pas
         * encore entièrement visible (revue 2026-09-13). La NVS (ms) se fait
         * ensuite HORS verrou. Le STATUS suivant annoncera la nouvelle
         * empreinte → le dongle coupera la balise. */
        bool a_enregistrer = false;
        uint32_t cible = 0;
        if (s_sync_done && radio_lock(20)) {
            if (s_sync_done) {
                memcpy((uint8_t *)keymaps, s_krx.buf, KEYMAP_BLOB_BYTES);
                cible = s_sync_target_fp;
                s_sync_done = false;
                a_enregistrer = true;
            }
            radio_unlock();
        }
        if (a_enregistrer) {
            bool saved = save_keymaps((uint16_t *)keymaps, KEYMAP_BLOB_BYTES);
            uint32_t fp = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
            ESP_LOGW(TAG, "sync keymap : 40/40 recus, fp=0x%08X %s (cible 0x%08X) — %s",
                     (unsigned)fp, fp == cible ? "= cible" : "!= CIBLE", (unsigned)cible,
                     saved ? "enregistree en NVS" : "ECHEC NVS");
#if CONFIG_KASE_VEILLE
            veille_veto(VEILLE_VETO_SYNC, false);   /* tirage fini et enregistré */
#endif
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
#if CONFIG_KASE_RF_CE_SCAN
        radio_ce_gpio(ce);   /* banc V2D : la broche CE incertaine — essayer chaque candidate */
#endif
        for (int i = 0; i < 12 && !acked; i++) {        /* ~3 s per candidate */
            uint8_t rxb[32]; uint16_t n = 0;
            radio_pair_round(pair_addr, RF_PAIR_CHANNEL, req, 8, rxb, sizeof rxb, 150, &n);
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

    /* Le propriétaire initialise la puce en PTX vers le dongle et enregistre
     * lui-même son hook de veille (power-down, verrou gardé ; réveil réarmé). */
    if (!radio_owner_init(&nrf_cfg, NULL)) {
        ESP_LOGE(TAG, "NRF PTX init failed — wireless relay disabled");
        return;   /* s_paired stays false */
    }
#if CONFIG_KASE_DONGLE_FUSION
    s_kbd_cfg = nrf_cfg;   /* cible dongle mémorisée pour rebasculer en PTX (fusion phase 2) */
#endif

    if (set_id == 0 || set_id == 0xFFFF) {
        /* Unpaired: spawn the active pairing task. It REQs on the rendezvous and,
         * on ACK, saves NVS + reboots (relay comes up active). Open the dongle's
         * pairing window (KS_CMD_RF_PAIR_START) to complete it. */
        ESP_LOGW(TAG, "kbd_relay: unpaired — starting pairing task (open the dongle window)");
        xTaskCreate(kbd_pairing_task, "kbd_pair", 4096, NULL, 5, NULL);
        return;   /* s_paired becomes true after the post-pairing reboot */
    }

    ESP_LOGI(TAG, "kbd_relay: paired set_id=0x%04X slot=0x%02X — relay active",
             set_id, slot);
    s_paired = true;

    /* Periodic keyboard-state refresh: self-heals lost key-ups over the lossy
     * link (no heartbeat reconciliation on the HIDREPORT path). Streams only
     * après un changement seulement (kbd_refresh_arm), pour un nombre borné de
     * ticks : un clavier au repos est réellement silencieux. */
    const esp_timer_create_args_t ta = {
        .callback = kbd_relay_refresh_cb, .name = "kbd_refresh",
    };
    if (esp_timer_create(&ta, &s_refresh_timer) == ESP_OK)
        kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);
#if CONFIG_KASE_VEILLE
    /* Veille (B7) : le timer de rafraîchissement s'arrête et repart avec la
     * carte ; la puce elle-même est au hook du propriétaire. */
    static const veille_hook_t hook = { "relais", kbd_relay_sleep_prepare, kbd_relay_wake_restore };
    veille_hook_enregistrer(&hook);
#endif
}

#if CONFIG_KASE_VEILLE
/* Suffixe de rôle du battement de coeur (veille_task.h) : la gauche dit sa
 * route et l'état du relais — la bascule USB → RF se lit là. */
const char *veille_hb_suffixe(void)
{
    static char buf[32];
    snprintf(buf, sizeof buf, " route=%s relais=%s",
             (kbd_active_route() == KBD_OUT_RF) ? "RF" : "USB",
             kbd_relay_active() ? "actif" : "inactif");
    return buf;
}
#endif

#if CONFIG_KASE_DONGLE_FUSION
/* Fusion phase 2 (4b) : le moteur de la gauche lit la demi-matrice de la droite
 * réémise par le dongle (reçue en écoute USB) — équivalent de
 * ce que faisait le maître pré-fusion en écoutant la droite en direct. */
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

/* ── Veille : le relais dort avec la carte ───────────────────────────────── */

/* La puce est éteinte et réarmée par le hook du propriétaire (radio_owner) ;
 * ici seulement le timer de rafraîchissement : arrêté au sommeil — sinon ses
 * ticks compteraient des « indisponibles » pendant que la puce dort et
 * fausseraient « dongle vu » —, relancé en cadence rapide au réveil. */
void kbd_relay_sleep_prepare(void)
{
    if (s_refresh_timer) esp_timer_stop(s_refresh_timer);
}

void kbd_relay_wake_restore(void)
{
    s_periode_ms = 0;                              /* le timer a été arrêté : forcer le redémarrage */
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);
}

bool kbd_relay_active(void)
{
    return s_paired;
}

bool kbd_relay_dongle_vu(void)
{
    /* Collant (muette au repos), tolérant aux ~1 % de refus ESB : tombe après
     * 3 émissions consécutives sans ACK, jamais sur un refus isolé. */
    return s_sans_ack_ecran < 3;
}

void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6])
{
    s_last_mod = modifier;
    memcpy(s_last_kb, kb, 6);
    kbd_refresh_arm(&s_refresh, KBD_RELAY_REPEATS);
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);   /* un changement réveille la cadence rapide */
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
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);   /* un changement réveille la cadence rapide */
}
#endif
