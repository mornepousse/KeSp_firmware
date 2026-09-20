#include "half_link.h"
#include "board.h"
#include "rf_driver.h"
#include "radio_owner.h"   /* the chip: one owner, this module is only a policy */
#include "rf_packet.h"
#include "rf_slot.h"
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_pairing.h"   /* fusion: the right addresses the dongle's keyboard slot */
#include "esp_mac.h"      /* esp_read_mac — pairing REQ */
#include "esp_system.h"   /* esp_restart — after pairing */
#endif
#include "driver/gpio.h"
#include "esp_log.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* radio hook + heartbeat suffix */
#if CONFIG_KASE_LINK_WIRE
#include "link_uart.h"     /* HB suffix: link */
#endif
#endif
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"    /* HB suffix: batt */
#endif
#include "esp_attr.h"
#include "esp_timer.h"
#include "cadence.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "half_link";

static uint8_t    s_seq;


#if CONFIG_KASE_HALF_LINK_TX
/* Last state emitted, and when. Shared between TWO task contexts: the
 * keyboard_button driver callback (immediate emission on change) and the
 * refresh task below. Four bytes, but a torn copy would send a matrix
 * that never existed — hence the lock, very short. */
static uint8_t  s_etat_local[RF_HALF_BITMAP_BYTES];
static uint32_t s_dernier_tx_ms;
/* Generation of the local state (+1 per CHANGE, under s_etat_mux): a
 * reaffirmation snapshots state + generation, the owner does not emit a
 * stale state (same race as the left, shorter window: snapshot -> lock). */
static volatile uint32_t s_etat_gen;
static bool etat_valide(void *ctx) { return s_etat_gen == *(const uint32_t *)ctx; }
static portMUX_TYPE s_etat_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_refresh_task;   /* notified on change: fast cadence without waiting */

#if CONFIG_KASE_VEILLE && CONFIG_KASE_BATT_SENSE
static void half_link_apres_reveil(void);
#endif
/* The chip (lock, mode, target, sleep) belongs to radio_owner.c: this module
 * only decides frames and target. Two tasks call tx_frame
 * (scan callback, refresh): the fallback FSM is under s_etat_mux. */
static uint8_t        s_sans_ack_ecran = 3;   /* consecutive emissions without ACK: "dongle seen" for the screen (3 = not seen yet) */
#if CONFIG_KASE_DONGLE_FUSION
/* Fallback without dongle: two emission targets. The right targets the dongle
 * by default (s_cfg_dongle, MATRIX on KaSe.01); if the dongle disappears, it
 * switches to the left-USB (s_cfg_left, HEARTBEAT on KaSe.03, the pre-fusion
 * protocol that kbd_relay decodes). half_link.h carries the pure FSM. */
static rf_radio_cfg_t s_cfg_dongle;   /* KaSe.01, set_id — the dongle types */
static rf_radio_cfg_t s_cfg_left;     /* KaSe.03 fixed — the left-USB types */
static half_tx_fsm_t  s_tx_fsm = { HALF_TX_TO_DONGLE, 0 };
#endif
#endif

/* Config common to both ends: same channel, same address, otherwise nothing
 * gets through (nRF24L01+ PS §6.3: "You must program a transmitter and a
 * receiver with the same RF channel frequency to communicate with each other"). */
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
/* Active pairing of the RIGHT to the dongle (fusion). Replicates the proven flow
 * of kbd_pairing_task (kbd_relay_tx.c): REQ on the rendezvous declaring the
 * KEYBOARD SLOT (0x01, the right shares the left's address), waiting for
 * the ACK carrying the set_id, NVS save, restart — on reboot,
 * half_link_tx_init loads the set_id and targets the right address.
 *
 * The right declares 0x01: rf_pairing_resolve_slot honors the declared slot, so
 * the dongle assigns it to the keyboard without touching the mouse slot (0x02).
 * Both halves end up on the same address, distinguished by the half identity
 * in PKT_TYPE_MATRIX.
 *
 * ⚠ The dongle must have its pairing window OPEN (KS_CMD_RF_PAIR_START).
 * Goes through radio_pair_round: aim at the rendezvous then RETURN to the target. */
static void half_fusion_pairing_task(void *arg)
{
    (void)arg;
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    uint8_t req[8];
    rf_encode_pair_req(req, my_mac, RF_ADDR_KBD_DONGLE);   /* declares the keyboard slot */
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;

    rf_pair_ack_t ack;
    bool acked = false;
#if CONFIG_KASE_VEILLE
    /* 40 s of rounds holding the chip 150 ms each, without typing: without a veto,
     * at 15 s of inactivity radio_sleep was cutting the chip out from under this task. */
    veille_veto(VEILLE_VETO_PAIR, true);
#endif
    /* ~30 s of attempts: leaves time to open the dongle's window. */
    for (int i = 0; i < 200 && !acked; i++) {
        uint8_t rxb[32]; uint16_t n = 0;
        radio_pair_round(pair_addr, RF_PAIR_CHANNEL, req, 8, rxb, sizeof rxb, 150, &n);
        if (n && rf_decode_pair_ack(rxb, n, &ack)) { acked = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (acked) {
        /* We ALWAYS save the keyboard slot: the right shares the left's
         * address, whatever slot the dongle returns. */
        ESP_LOGW(TAG, "fusion appairage : ACK set_id=0x%04X — sauvegarde + reboot",
                 ack.set_id);
        rf_pairing_save_half(ack.set_id, RF_ADDR_KBD_DONGLE, ack.dongle_wifi_mac);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "fusion appairage : pas d'ACK — ouvrir la fenêtre du dongle "
                  "(KS_CMD_RF_PAIR_START) puis redémarrer la droite");
#if CONFIG_KASE_VEILLE
    veille_veto(VEILLE_VETO_PAIR, false);
#endif
    vTaskDelete(NULL);
}
#endif /* CONFIG_KASE_DONGLE_FUSION */

bool half_link_tx_init(void)
{
    rf_radio_cfg_t cfg = half_link_cfg();
#if CONFIG_KASE_DONGLE_FUSION
    /* The base config (KaSe.03 fixed, link channel) IS the "left
     * direct" target: it's exactly what the left-USB listens to. It is memorized
     * BEFORE retargeting toward the dongle, for the fallback without a dongle. */
    s_cfg_left = cfg;
    /* Fusion: the right no longer talks to the left but to the DONGLE'S KEYBOARD
     * SLOT, like the left (same address, distinguished by the half identity in
     * PKT_TYPE_MATRIX). Channel and suffix of the keyboard slot, address derived
     * from the pairing set_id. Not paired -> active pairing is launched (below). */
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
#if CONFIG_KASE_DONGLE_FUSION
    s_cfg_dongle    = cfg;                 /* default target: the dongle */
    s_tx_fsm.cible  = HALF_TX_TO_DONGLE;   /* at boot, we target the dongle */
    s_tx_fsm.sans_ack = 0;
#endif
    /* The owner initializes the chip in PTX toward the target and registers
     * its own sleep hook itself (power-down, lock kept; re-armed on wake). */
    if (!radio_owner_init(&cfg, NULL)) {
        ESP_LOGE(TAG, "TX init echouee — la moitie droite restera muette");
        return false;
    }
    ESP_LOGI(TAG, "TX pret : ch=0x%02X addr=KaSe.%02X", cfg.channel, cfg.addr_suffix);
#if CONFIG_KASE_VEILLE && CONFIG_KASE_BATT_SENSE
    /* On wake: a STATUS right away, the voltage may have moved. */
    static const veille_hook_t hook_status = { "status", NULL, half_link_apres_reveil };
    veille_hook_enregistrer(&hook_status);
#endif

#if CONFIG_KASE_DONGLE_FUSION
    if (fusion_unpaired) {
        /* No probe or normal emission as long as there is no set_id: we would
         * target the factory address and the dongle would not acknowledge. We
         * launch active pairing, which restarts the board once the ACK is received. */
        ESP_LOGW(TAG, "fusion : appairage actif au dongle — ouvrir sa fenêtre");
        xTaskCreate(half_fusion_pairing_task, "half_pair", 4096, NULL, 5, NULL);
        return true;
    }
#endif

    /* Startup probe burst. Without it, knowing whether the link carries
     * would depend on someone pressing a key WHILE watching the
     * console — an inconvenient synchronization between two operators. Here a
     * simple reset is enough to get the verdict.
     *
     * The acknowledgment is HARDWARE: the nRF24 on the other side answers on its
     * own if channel and address match, without its software getting involved. A
     * high rate therefore proves that the left's radio listens on the right channel,
     * even if its application layer had a problem elsewhere. */
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
            /* THE measurement of R1. The acknowledgment alone says nothing about deafness:
             * the ESB retransmits up to 15 times, so a packet gets through even if the
             * left was deaf on the first try. What betrays the deafness
             * is the NUMBER OF RETRANSMISSIONS — every PRX->PTX excursion of
             * the left costs the right one lost attempt.
             *
             * A ratio close to zero means the switch is not noticeable;
             * a high ratio measures exactly what the bet costs. */
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
static bool half_link_tx_frame_si(const uint8_t *buf, uint8_t n, radio_valide_cb_t encore_valide, void *ctx);
bool half_link_tx_matrix_si(const uint8_t *bitmap, radio_valide_cb_t encore_valide, void *ctx)
{
    if (!radio_presente()) return false;
    uint8_t buf[16];
    uint16_t n;
#if CONFIG_KASE_DONGLE_FUSION
    /* The FORMAT depends on the current target (fallback without dongle):
     *  - dongle target: RAW half-matrix (PKT_TYPE_MATRIX + half identity).
     *    The dongle merges the two halves and runs the engine.
     *  - left target: pre-fusion HEARTBEAT (KaSe.03), the ONLY format that
     *    kbd_relay decodes on the left-USB side. Each listener speaks its own protocol. */
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
    /* seq is only consumed once the lock is taken: an abandoned send
     * (radio asleep, lock held) was incrementing it anyway, and the left
     * counted every abandonment as a LOST frame — 48% of "losses"
     * read on the bench on 2026-09-11 while the right was falling asleep, without a
     * single packet actually vanishing in the air. */
    h.seq = s_seq;
    /* batt_dV and link_q stay at zero: the gauge is brick B7, and the
     * link quality will be computed once the retransmission counter makes
     * sense (a receiver on the other end is needed). */
    n = rf_encode_heartbeat(buf, &h);
#endif
    return half_link_tx_frame_si(buf, (uint8_t)n, encore_valide, ctx);
}

/* Emission of ONE frame toward the current target, under the radio lock: send,
 * watchdog / target switch (fusion), bench instrumentation. Shared by
 * the matrix (half_link_tx_matrix) and the gauge's slow STATUS
 * (half_link_tx_status) — a single emission path, hence a single owner
 * of the chip and a single FSM. */
#if CONFIG_KASE_HALF_LINK_TX
bool half_link_tx_dongle_vu(void)
{
#if CONFIG_KASE_DONGLE_FUSION
    if (s_tx_fsm.cible != HALF_TX_TO_DONGLE) return false;   /* fallback to the left: no dongle */
#endif
    /* Sticky, not timestamped: a half is SILENT at rest by construction (a
     * STATUS every 30 s), a "seen less than 2.5 s ago" would flicker at
     * every STATUS. The indicator only drops if an emission is NOT
     * acknowledged. */
    return s_sans_ack_ecran < 3;   /* tolerates the ~1% of isolated ESB refusals */
}
#endif

bool half_link_tx_matrix(const uint8_t *bitmap) { return half_link_tx_matrix_si(bitmap, NULL, NULL); }

static bool half_link_tx_frame_si(const uint8_t *buf, uint8_t n, radio_valide_cb_t encore_valide, void *ctx)
{
    if (!radio_presente()) return false;
    /* 50 ms: an ESB emission in the worst case (ARC=15, ARD=500 us) fits in ~13 ms.
     * The owner holds the lock over the whole transaction, CSN included. */
    radio_tx_t r = radio_emettre(buf, n, NULL, NULL, 50, encore_valide, ctx);
    /* STALE (state outdated) and UNAVAILABLE (lock taken, chip asleep): NOTHING
     * left — neither a refusal nor a send. The fallback FSM must not learn about it:
     * eight missed locks in a row were switching the target to the left without
     * a single frame having been refused by the dongle (2026-09-20 review). */
    if (r == RADIO_TX_PERIME || r == RADIO_TX_INDISPO) return false;
    s_seq++;                                  /* the frame left: this number is consumed */
    bool ack = (r == RADIO_TX_ACK);
    if (ack) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;

    /* Radio watchdog. An nRF24 (clone) FREEZES — under a storm of
     * retransmissions or a glitch (observed on the bench on 2026-09-13: when the
     * TRRS link activated in USB mode, the right's radio froze and stopped
     * acknowledging ANYTHING, even once USB was unplugged, until reset). After N
     * consecutive sends without an ACK (not a simple ESB loss), the chip is RE-ARMED
     * (rewriting the PTX config), without a restart. This function is
     * called by TWO tasks (scan callback, refresh): the
     * decision is made under s_etat_mux, the owner applies it under its own. */
#if CONFIG_KASE_DONGLE_FUSION
    /* In fusion, re-arming also DOUBLES as fallback: it switches to the other
     * listener (dongle ↔ direct left-USB). Pure and tested decision
     * (half_tx_target_step, test/test_half_tx_target.c). */
    bool bascule; half_tx_target_t avant, apres;
    taskENTER_CRITICAL(&s_etat_mux);
    avant   = s_tx_fsm.cible;
    bascule = half_tx_target_step(&s_tx_fsm, ack, HALF_TX_SWITCH_FAILS);
    apres   = s_tx_fsm.cible;
    taskEXIT_CRITICAL(&s_etat_mux);
    if (bascule) {
        /* Same target as before (chip frozen): radio_mode_set would be idempotent,
         * radio_rearmer rewrites anyway. The decision is made on the snapshot
         * taken under s_etat_mux — not by re-reading the owner's live state. */
        if (apres == avant) radio_rearmer();
        else                radio_mode_set(RADIO_PTX, apres == HALF_TX_TO_LEFT ? &s_cfg_left : &s_cfg_dongle);
        ESP_LOGW(TAG, "repli : bascule TX -> %s (rearme, %u sans ACK)",
                 apres == HALF_TX_TO_LEFT ? "GAUCHE KaSe.03 (heartbeat)" : "DONGLE KaSe.01 (matrix)",
                 (unsigned)HALF_TX_SWITCH_FAILS);
    }
#else
    static uint16_t s_sans_ack;
    if (ack) {
        s_sans_ack = 0;
    } else if (++s_sans_ack >= 30) {
        s_sans_ack = 0;
        radio_rearmer();
        ESP_LOGW(TAG, "chien de garde : radio TX rearmee (30 envois sans ACK)");
    }
#endif

    /* Bench instrument: without it, one cannot tell "packets leave
     * and get acknowledged" from "they leave into the void". Summarizes every ten
     * sends rather than one line per packet — while typing, one line per packet
     * would flood the console and skew the timing. */
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
/* THE ONLY decision point for emission — both paths go through here.
 *
 * `change` distinguishes the two callers: the scan callback knows there is
 * something new and wants to leave without waiting; the refresh task knows
 * nothing and lets the rule decide. The rule itself is pure and tested host-side
 * (half_tx_doit_emettre, test/test_half_tx_cadence.c): silent at rest,
 * refreshed as long as a key is held. */
void half_link_tx_update(const uint8_t *bitmap, bool change)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t  etat[RF_HALF_BITMAP_BYTES];
    uint32_t dernier, gen;

    /* The refresh task passes bitmap = NULL: it has nothing new
     * to announce, it reaffirms what is already there. This is deliberate. If it
     * supplied the state it read on the previous round, it would REWRITE it
     * here, and a release published in the meantime by the scan callback would be
     * resurrected — the key would stay pressed until the next change.
     * A single writer, therefore: the callback. */
    /* Bounded repair: a change arms HALF_TX_REPEATS repetitions
     * (half_tx_doit_emettre_repare, tested). The counter lives under the same
     * spinlock as the state: the scan callback and the refresh task both
     * access it. The decision is made INSIDE the critical section (pure,
     * non-blocking) so that the timestamp and the counter move as one block. */
    static half_tx_repeat_t s_rep;
    bool emettre;
    taskENTER_CRITICAL(&s_etat_mux);
    if (change && bitmap) { memcpy(s_etat_local, bitmap, RF_HALF_BITMAP_BYTES); s_etat_gen++; }
    memcpy(etat, s_etat_local, RF_HALF_BITMAP_BYTES);
    gen = s_etat_gen;
    dernier = s_dernier_tx_ms;
    bool tenu = false;
    for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++)
        if (etat[i]) { tenu = true; break; }
    emettre = half_tx_doit_emettre_repare(&s_rep, change, tenu, now, dernier, HALF_TX_REFRESH_MS);
    if (emettre) s_dernier_tx_ms = now;
    taskEXIT_CRITICAL(&s_etat_mux);

    if (change && s_refresh_task) xTaskNotifyGive(s_refresh_task);   /* wake the fast cadence */
    if (!emettre) return;
    half_link_tx_matrix_si(etat, etat_valide, &gen);   /* stale if a change slips in before the lock */
}

/* Refresh of holds.
 *
 * The keyboard_button driver callback is registered on KBD_EVENT_PRESSED,
 * which fires ONLY on change (cf. matrix_scan.c). A held key therefore
 * produces nothing more after it is pressed — and the left, which releases
 * after HALF_LINK_TIMEOUT_MS of silence, was releasing it while it was
 * physically pressed. A periodic context is needed, and there was none
 * on this half: here it is.
 *
 * It only wakes the radio if something is pressed. At rest the task
 * runs idle for the price of a memcmp every 20 ms — the link stays
 * free, which is the premise of R1. */
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
/* Gauge: the right is silent at rest, so its voltage must leave on its
 * own initiative — a STATUS every RF_BATT_PERIOD_MS (contract in
 * rf_slot.h), plus one on wake. It carries the half identity: the two
 * halves share the dongle's keyboard slot in fusion. It leaves toward the
 * CURRENT target of the FSM; when fallen back to the left, that one ignores
 * it — acceptable, the dongle is absent anyway then. This is NOT keyboard
 * activity: it does not buffer sleep, the right falls asleep as before. */
static uint32_t s_dernier_status_ms;   /* 0 = force on next tick (boot, wake) */

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
        half_link_batt_tick();   /* gauge's slow STATUS; not an activity */
#endif
        /* Sleep (B7) is no longer evaluated here: power/veille_task.c, one
         * task shared by both halves; this module has registered its radio hook. */
        /* 20 ms as long as a key is held (reaffirmation at 100 ms, bounded
         * repair), 100 ms at rest: at a permanent 20 ms this task pulled the
         * processor out of idle 50 times per second for a memcmp — and with
         * DFS, every exit relights the PLL. Sleep (15 s threshold), the gauge
         * (30 s) and the HB (10 s) accommodate this. */
        bool tenu = false;
        taskENTER_CRITICAL(&s_etat_mux);
        for (int i = 0; i < RF_HALF_BITMAP_BYTES; i++) if (s_etat_local[i]) { tenu = true; break; }
        taskEXIT_CRITICAL(&s_etat_mux);
        /* A change (scan callback) notifies the task: the bounded
         * repair leaves right away, not at the next 100 ms tick. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tenu ? HALF_TX_TENU_MS : HALF_TX_REPOS_MS));
    }
}

bool half_link_tx_refresh_start(void)
{
    if (!radio_presente()) return false;
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


#if CONFIG_KASE_VEILLE && CONFIG_KASE_BATT_SENSE
static void half_link_apres_reveil(void) { s_dernier_status_ms = 0; }   /* STATUS forced on the next tick */
#endif

#if CONFIG_KASE_VEILLE
/* Role suffix of the heartbeat (veille_task.h): the right reports whether the
 * TRRS link is active and its voltage. */
const char *veille_hb_suffixe(void)
{
    static char buf[32];
    int lien = 0;
#if CONFIG_KASE_LINK_WIRE
    lien = link_uart_active();
#endif
    unsigned batt = 0;
#if CONFIG_KASE_BATT_SENSE
    batt = batt_sense_dv();
#endif
    snprintf(buf, sizeof buf, " lien=%d batt=%u dV", lien, batt);
    return buf;
}
#endif

static bool half_link_tx_frame(const uint8_t *buf, uint8_t n) { return half_link_tx_frame_si(buf, n, NULL, NULL); }
