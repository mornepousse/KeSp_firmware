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
#include "radio_owner.h"   /* the chip has an owner: this module is a policy */
#if CONFIG_KASE_DONGLE_FUSION
#include "half_link.h"   /* excursion (RX); HALF_LINK_TIMEOUT_MS (fusion) */
#endif
#include "rf_packet.h"
#include "rf_slot.h"
#include "rf_pairing.h"
#include "usb_presence.h"   /* route poll + kbd_active_route (USB-first auto-switch) */
#include "keymap.h"         /* keymaps[], KEYMAP_BLOB_BYTES — config fingerprint */
#include "config_sync.h"    /* config_fp_crc32 — fingerprint announced in STATUS */
#if CONFIG_KASE_DONGLE_FUSION
#include "keymap_pull.h"    /* pull of the dongle's keymap via ACK payload */
#endif
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"     /* gauge: voltage + charge state in STATUS */
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
#include "veille_task.h"   /* radio hook, sync veto, HB suffix */
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

/* Repeats of the last report after a change, at KBD_RELAY_REFRESH_MS
 * intervals. 5 x 10 ms = 50 ms of self-healing: a key-up lost five times in
 * a row when each transmission already benefits from the 15 ESB
 * retransmissions does not happen in practice. Beyond that, the link goes
 * silent again — this is what makes the design's "event-driven emission" premise true. */
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

/* The chip (lock, PTX/PRX mode, target, sleep) lives in radio_owner.c. Here:
 * what to transmit, to whom, and listening to the right half in USB mode. */
#if CONFIG_KASE_DONGLE_FUSION
/* Dongle target (channel/address derived from set_id): wireless PTX; over
 * USB the chip switches to PRX on the link (KaSe.03) — radio_mode() says where we stand. */
static rf_radio_cfg_t s_kbd_cfg;
/* Last half-matrix of the RIGHT half received (re-emitted by the dongle) in
 * USB mode. The left half's engine reads it via kbd_relay_remote_pressed()
 * to merge it into the high columns — master path, step 4b. */
static uint8_t          s_remote_bm[RF_HALF_BITMAP_BYTES];
static volatile bool    s_remote_changed;
static uint32_t         s_remote_ms;
/* Last LOCAL half-matrix sent to the dongle + when. The refresh reaffirms
 * held keys (otherwise the dongle releases the left half on silence — the
 * same trap as half_link on the right side). */
static uint8_t          s_last_left_bm[RF_HALF_BITMAP_BYTES];
static uint32_t         s_last_left_ms;
/* Generation of the local state: +1 on every CHANGE, under s_left_mux,
 * BEFORE transmission. A repeat (bounded repair, 100 ms reaffirmation)
 * snapshots state + generation and is only transmitted by the owner if the
 * generation has not moved by the time it acquires the lock (radio_emettre
 * STALE). Without this: the timer would re-read the press while the scan
 * callback transmitted the release, wait for the lock behind it, then
 * transmit the stale press — P, R, P, R: a double press on a short tap,
 * which the dongle's transition queue faithfully replayed (bench 2026-09-20). */
static volatile uint32_t s_last_left_gen;
static portMUX_TYPE      s_left_mux = portMUX_INITIALIZER_UNLOCKED;
static bool gen_valide(void *ctx) { return s_last_left_gen == *(const uint32_t *)ctx; }
#endif
static bool s_paired = false;


/* Last keyboard report, for the periodic refresh. */
static uint8_t s_last_mod;
static uint8_t s_last_kb[6];
static kbd_refresh_t s_refresh;   /* bounded repeat — see kbd_relay_tx.h */
static esp_timer_handle_t s_refresh_timer;   /* periodic refresh; stopped during sleep */

/* Radio path tally. Without it, a keystroke lost in RF mode is
 * INDISTINGUISHABLE. Three outcomes: delivered (ACK), refused by the owner
 * (lock held under 20 ms, or chip in PRX / asleep — transmission never
 * happened), refused by the ESB (MAX_RT). */
static uint32_t s_tx_remis, s_tx_sans_mutex, s_tx_refuses;

/* Timestamp of the LAST transmission, of any kind, and the status frame
 * counter. A HID report keeps the link alive just as well as a supervision
 * frame: no point adding one while typing. */
static uint32_t s_derniere_emission_ms;
static uint8_t  s_sans_ack_ecran = 3;   /* consecutive transmissions without ACK: "dongle seen" for the screen (3 = not seen yet) */
static uint8_t  s_status_seq;

#if CONFIG_KASE_DONGLE_FUSION
/* The keymap pull via ACK payload lives in keymap_pull.c. */
/* Raw transmission of a half-matrix WITHOUT arming the re-emission — see
 * below. Returns true if the frame went out. */
static bool send_matrix_frame(uint8_t half, const uint8_t *bitmap, radio_valide_cb_t encore_valide, void *ctx);
#endif

/* Transmission to the dongle. `encore_valide` (or NULL): for a REPEAT, the
 * owner evaluates it under the lock and does not transmit a stale state.
 * Returns true if the frame WENT OUT (acknowledged or refused). */
static bool kbd_tx_emettre(const uint8_t *buf, uint8_t len, radio_valide_cb_t encore_valide, void *ctx)
{
    if (!radio_presente()) return false;
    if (radio_mode() != RADIO_PTX) {
        /* USB route: the chip is listening to the right half, the scan
         * callback normally does not transmit (route-gated). If we get here,
         * it's a route crossing: counted, not transmitted. */
        s_tx_sans_mutex++;
        return false;
    }
    {
        /* ACK payload return channel (auto keymap sync): retrieve what the
         * dongle slipped into the ACK. */
        uint8_t ack[32];
        uint8_t ack_n = 0;
        radio_tx_t r = radio_emettre(buf, len, ack, &ack_n, 20, encore_valide, ctx);
        if (r == RADIO_TX_PERIME) return false;   /* state changed during the wait: the new one already went out */
        if (r == RADIO_TX_INDISPO) {              /* nothing went out: lock held, chip asleep */
            s_tx_sans_mutex++;
            ESP_LOGW(TAG, "report ABANDONED (radio unavailable) — delivered %u, lost %u+%u",
                     (unsigned)s_tx_remis, (unsigned)s_tx_sans_mutex, (unsigned)s_tx_refuses);
            return false;
        }
        bool ok = (r == RADIO_TX_ACK);
#if CONFIG_KASE_DONGLE_FUSION
        keymap_pull_on_ack(ack, ack_n);   /* beacon or chunk slipped into the ACK */
#endif
        if (ok) s_tx_remis++; else s_tx_refuses++;
        s_derniere_emission_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (ok) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;
        /* Per-report trace, at DEBUG level: this is what showed, on
         * 2026-09-11, that the left half was correctly transmitting the
         * wake key — and that a captured Super thumb stayed stuck. One line
         * per send is too much for everyday use; re-enable via the log
         * level when a keystroke gets lost without knowing where. */
        if (buf[0] == (PKT_TYPE_HIDREPORT << 4) && buf[1] == RF_HID_SUB_KBD)
            ESP_LOGD(TAG, "TX kbd mod=%02X kc=%02X %02X -> %s", buf[2], buf[3], buf[4],
                     ok ? "ok" : "REFUSED");
        if (((s_tx_remis + s_tx_refuses) % 25) == 0)
            ESP_LOGW(TAG, "HID->dongle: %u delivered, %u without mutex, %u refused",
                     (unsigned)s_tx_remis, (unsigned)s_tx_sans_mutex,
                     (unsigned)s_tx_refuses);
        return true;
    }
}
static void kbd_tx_locked(const uint8_t *buf, uint8_t len) { (void)kbd_tx_emettre(buf, len, NULL, NULL); }

/* esp_timer callback (10 ms): the single route poller, and the idempotent live
 * keyboard-state refresh. Polling here keeps the debounce + cached route fresh
 * even when idle. Only transmits over RF when RF is the active path — when USB is
 * plugged we must NOT relay (the dongle would type a duplicate on its own host). */
/* Refresh timer period: 10 ms as long as there is something to repeat (held
 * key, bounded repair, sync pull), 100 ms at rest. At a permanent 10 ms, the
 * processor left idle 100 times per second for a memcmp and a route poll —
 * and each time the DFS raised the PLL back up. The route (50 ms debounce)
 * and the USB announcement (200 ms) hold fine at 100 ms. */
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
    sync = keymap_pull_en_cours();
    ecoute_usb = (radio_mode() == RADIO_PRX);   /* USB route: this tick drains the FIFO of right-half frames */
#else
    for (int i = 0; i < 6; i++) if (s_last_kb[i]) tenu = true;   /* held HID report (V2D) */
    if (s_last_mod) tenu = true;
#endif
    kbd_relay_timer_set(kbd_relay_cadence_ms(reparation, tenu, sync, ecoute_usb));
}
#if CONFIG_KASE_DONGLE_FUSION
/* The right half's half-matrix is written here (esp_timer task, via the
 * owner) and read by the scan callback and the keyboard task: the four
 * bytes and the flag travel together under s_left_mux — the flag must not
 * be visible before the bytes. */
static void remote_poser(const uint8_t *bm)
{
    taskENTER_CRITICAL(&s_left_mux);
    if (memcmp(s_remote_bm, bm, RF_HALF_BITMAP_BYTES) != 0) {
        memcpy(s_remote_bm, bm, RF_HALF_BITMAP_BYTES);
        s_remote_changed = true;
    }
    taskEXIT_CRITICAL(&s_left_mux);
}
/* Right half goes silent or listening stops: release what it was holding
 * (a mute half must not leave a stuck key); no effect if nothing was
 * held. */
static void remote_relacher(void)
{
    static const uint8_t rien[RF_HALF_BITMAP_BYTES];
    remote_poser(rien);
}
/* Consumer of the right half's frames (re-emitted by the dongle on
 * KaSe.03), called by the owner under its lock: periodic drain and drain
 * BEFORE each excursion. */
static void kbd_relay_rx_droite(const uint8_t *rb, uint16_t rn, void *ctx)
{
    uint32_t now = *(uint32_t *)ctx;
    rf_heartbeat_t h;
    if (!rf_decode_heartbeat(rb, rn, &h)) return;
    remote_poser(h.bitmap);
    s_remote_ms = now;
}
#endif

static void kbd_relay_refresh_body(void)
{
    usb_presence_poll(s_paired);
#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion phase 2: dynamic radio switch depending on the route.
     *  - USB: the left half types locally. It switches its radio to PRX on
     *    the link (KaSe.03) to LISTEN to the right half re-emitted by the
     *    dongle, and ANNOUNCES its mode to the dongle by excursion. (Listening
     *    is free on mains power.)
     *  - wireless: radio in PTX towards the dongle (battery: it doesn't listen).
     * The owner (radio_owner) holds the mode and the lock; here we only request
     * PRX(link) or PTX(dongle) depending on the route. The scan callback only
     * transmits in wireless mode (route-gated) — and in PRX the owner would refuse anyway. */
    if (kbd_active_route() == KBD_OUT_USB) {
        if (radio_mode() != RADIO_PRX) {
            /* The link uses the FIXED address 'KaSe'.03 (what the dongle
             * targets when re-emitting the right half), not the address
             * derived from set_id: before the owner existed, listening
             * started on the wrong address and the first excursion fixed
             * it by restoring 'KaSe'.03 — it worked by accident (found on 2026-09-19). */
            rf_radio_cfg_t link = s_kbd_cfg;
            memcpy(link.rx_addr, "KaSe", 4);
            link.channel     = RF_CH_HALF_LINK;
            link.addr_suffix = RF_ADDR_HALF_LINK;
            if (!radio_mode_set(RADIO_PRX, &link)) return;
            ESP_LOGW(TAG, "fusion USB: listening for the re-emitted right half (PRX ch=0x%02X KaSe.%02X)",
                     RF_CH_HALF_LINK, RF_ADDR_HALF_LINK);
        }
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        /* Right-half re-emissions (heartbeats): the owner reads the FIFO
         * and hands us the frames; we remember its half-matrix, the left
         * half's engine reads it via kbd_relay_remote_pressed (step 4b). */
        radio_rx_drain(kbd_relay_rx_droite, &now);
        /* Right half goes silent -> release what it was holding (same
         * caution as the dongle: a mute half must not leave a stuck key). */
        {
            if ((uint32_t)(now - s_remote_ms) >= HALF_LINK_TIMEOUT_MS) remote_relacher();
        }
        /* Announce the mode to the dongle by excursion: the owner DRAINS the
         * FIFO into our consumer BEFORE leaving (the excursion ends with a
         * FLUSH_RX) and comes back to listen on KaSe.03. */
        if ((uint32_t)(now - s_derniere_emission_ms) >= 200u) {
            /* config_fp stays 0 here: over USB the dongle is silent, engine
             * consistency is moot. Buffer at RF_STATUS_LEN regardless —
             * rf_encode_status writes 8 bytes in every case. */
            rf_status_t st = { .batt_dV = KBD_BATT_DV(), .half = RF_HALF_LEFT, .charging = KBD_BATT_CHG(), .link_q = 0, .seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED),
                               .mode_usb = true };
            uint8_t sb[RF_STATUS_LEN];
            uint16_t sn = rf_encode_status(sb, &st);
            uint8_t dst[5] = { s_kbd_cfg.rx_addr[0], s_kbd_cfg.rx_addr[1],
                               s_kbd_cfg.rx_addr[2], s_kbd_cfg.rx_addr[3],
                               s_kbd_cfg.addr_suffix };
            bool ok = radio_excursion_tx(s_kbd_cfg.channel, dst, sb, (uint8_t)sn, kbd_relay_rx_droite, &now);
            if (ok) s_sans_ack_ecran = 0; else if (s_sans_ack_ecran < 255) s_sans_ack_ecran++;                  /* "dongle seen" in USB mode too */
            s_derniere_emission_ms = now;
        }
        return;
    }
    /* Back to wireless mode: switch the radio back to PTX towards the dongle. */
    if (radio_mode() == RADIO_PRX) {
        if (radio_mode_set(RADIO_PTX, &s_kbd_cfg)) {
            /* Leaving listening mode: release the remote, otherwise a right
             * half key would stay frozen in the local fusion until the next USB return. */
            remote_relacher();
            ESP_LOGW(TAG, "fusion: back to PTX emission toward the dongle");
        }
    }
#endif
    if (kbd_active_route() != KBD_OUT_RF) return;
#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion, wireless mode: REAFFIRM the local matrix as long as a key is
     * held. matrix_scan only transmits on CHANGE; without this refresh a
     * held left key produces nothing more and the dongle releases the left
     * half after HALF_LINK_TIMEOUT_MS ("the left key releases itself",
     * bench 2026-09-13). Same rule as the right half (half_link_tx_refresh):
     * mute at rest (empty bitmap -> autonomy), kept alive while held. */
    {
        uint8_t bm[RF_HALF_BITMAP_BYTES]; uint32_t gen, dernier;
        taskENTER_CRITICAL(&s_left_mux);
        memcpy(bm, s_last_left_bm, RF_HALF_BITMAP_BYTES); gen = s_last_left_gen; dernier = s_last_left_ms;
        taskEXIT_CRITICAL(&s_left_mux);
        bool tenu = (bm[0] | bm[1] | bm[2] | bm[3]) != 0;
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (tenu && (uint32_t)(now - dernier) >= 100u) {
            /* Reaffirmation: a REPEAT — stale if the state changed between
             * this snapshot and the lock (the change already went out). */
            if (send_matrix_frame(RF_HALF_LEFT, bm, gen_valide, &gen)) {
                taskENTER_CRITICAL(&s_left_mux);
                if (s_last_left_gen == gen) s_last_left_ms = now;
                taskEXIT_CRITICAL(&s_left_mux);
            }
            return;
        }
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION
    /* Auto sync (phase 3), AFTER reaffirming held keys: a held key keeps
     * priority, the pull pauses during it and resumes after — never a key
     * wrongly released for a keymap (the 2026-09-13 outage). */
    if (keymap_pull_tick(kbd_tx_locked)) return;
#endif
    /* Bounded re-emission: with no recent change, we stay silent.
     * usb_presence_poll above is still called on every tick — it keeps
     * routing fresh, it must not depend on keyboard activity. */
    if (kbd_refresh_step(&s_refresh)) {
#if CONFIG_KASE_DONGLE_FUSION
        /* Fusion: what repeats is the LAST BITMAP — even empty, a lost
         * release is repaired this way too, without violating "mute at
         * rest" since it is bounded. Never a HID report here. Snapshot +
         * generation: stale if a change slips in between here and the lock. */
        uint8_t bm[RF_HALF_BITMAP_BYTES]; uint32_t gen;
        taskENTER_CRITICAL(&s_left_mux);
        memcpy(bm, s_last_left_bm, RF_HALF_BITMAP_BYTES); gen = s_last_left_gen;
        taskEXIT_CRITICAL(&s_left_mux);
        (void)send_matrix_frame(RF_HALF_LEFT, bm, gen_valide, &gen);
#else
        uint8_t buf[9];
        rf_encode_hidreport_kbd(buf, s_last_mod, s_last_kb);
        kbd_tx_locked(buf, 9);
#endif
        return;
    }

    /* Supervision. The dongle releases the keys of a mute slot after
     * RF_LINK_LOST_MS — protection against a vanished keyboard, without
     * which a key would stay stuck at the host. But the re-emission above
     * is BOUNDED: a simply HELD key produces no change, hence no more
     * reports, and the dongle would release it after ~2 s. Backspace kept
     * repeating on its own, found at the bench on 2026-09-08.
     *
     * We only announce ourselves if nothing else has gone out since
     * RF_STATUS_PERIOD_MS: while typing, the HID reports suffice, and at
     * rest it stays at one frame per second — negligible for R1. */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!rf_status_doit_emettre(now, s_derniere_emission_ms, RF_STATUS_PERIOD_MS))
        return;
    /* Sync guard (fusion): we announce the fingerprint of OUR keymap. The
     * dongle, which types wirelessly with ITS OWN, compares and flags a
     * divergence — otherwise two engines would type differently in
     * silence. Computed on the fly (1/s here): no cache, so never stale. */
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
/* Useless when the inter-half link holds the radio: active pairing requires
 * listening on the rendezvous channel, hence giving up listening to the
 * right half. Compiled out of that case, it would be a dead static function. */
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
    /* board.h's CE pin ALWAYS goes first: on a board whose pinout is
     * verified against the netlist, it is the right one, and the scan has
     * no reason to exist. It did not follow that rule, and the list below
     * did not even contain the Niphargus CE (GPIO15) — pairing could
     * therefore never succeed on that board. */
    static const int ce_cand[] = {
        BOARD_NRF_CE_GPIO,
#if CONFIG_KASE_RF_CE_SCAN
        47, 45, 38, 39, 40, 33, 34, 35, 36,
#endif
    };
    rf_pair_ack_t ack;
    bool acked = false;
    int win_ce = -1;

#if CONFIG_KASE_VEILLE
    /* Each round holds the chip ~150 ms and nobody types during pairing:
     * without a veto, at 15 s of inactivity radio_sleep would cut the chip out from under this task. */
    veille_veto(VEILLE_VETO_PAIR, true);
#endif
    for (unsigned ci = 0; ci < sizeof(ce_cand) / sizeof(ce_cand[0]) && !acked; ci++) {
        int ce = ce_cand[ci];
        ESP_LOGW(TAG, "pairing: trying CE=GPIO%d ...", ce);
        gpio_reset_pin(ce);
        gpio_set_direction(ce, GPIO_MODE_OUTPUT);
        gpio_set_level(ce, 0);
#if CONFIG_KASE_RF_CE_SCAN
        radio_ce_gpio(ce);   /* V2D bench: the CE pin is uncertain — try each candidate */
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
#if CONFIG_KASE_VEILLE
    veille_veto(VEILLE_VETO_PAIR, false);
#endif
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

#if CONFIG_KASE_VEILLE
    /* Sleep (B7): the refresh timer stops and restarts with the board; the
     * chip itself is on the owner's hook. Registered BEFORE the owner: hooks
     * wake up in reverse order, so the radio is up before the timer starts
     * again (otherwise its first tick could land on a still-sleeping chip:
     * "radio unavailable" for nothing). */
    static const veille_hook_t hook = { "relay", kbd_relay_sleep_prepare, kbd_relay_wake_restore };
    veille_hook_enregistrer(&hook);
#endif
    /* The owner initializes the chip in PTX towards the dongle and registers
     * its own sleep hook itself (power-down, lock kept; re-armed on wake). */
    if (!radio_owner_init(&nrf_cfg, NULL)) {
        ESP_LOGE(TAG, "NRF PTX init failed — wireless relay disabled");
        return;   /* s_paired stays false */
    }
#if CONFIG_KASE_DONGLE_FUSION
    s_kbd_cfg = nrf_cfg;   /* dongle target remembered to switch back to PTX (fusion phase 2) */
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
     * after a change (kbd_refresh_arm), for a bounded number of ticks: a
     * keyboard at rest is genuinely silent. */
    const esp_timer_create_args_t ta = {
        .callback = kbd_relay_refresh_cb, .name = "kbd_refresh",
    };
    if (esp_timer_create(&ta, &s_refresh_timer) == ESP_OK)
        kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);
}

#if CONFIG_KASE_VEILLE
/* Role suffix of the heartbeat (veille_task.h): the left half states its
 * route and the relay's state — the USB -> RF switch reads there. */
const char *veille_hb_suffixe(void)
{
    static char buf[32];
    snprintf(buf, sizeof buf, " route=%s relay=%s",
             (kbd_active_route() == KBD_OUT_RF) ? "RF" : "USB",
             kbd_relay_active() ? "active" : "inactive");
    return buf;
}
#endif

#if CONFIG_KASE_DONGLE_FUSION
/* Fusion phase 2 (4b): the left half's engine reads the right half's
 * half-matrix re-emitted by the dongle (received while listening over
 * USB) — equivalent to what the pre-fusion master did by listening to the right half directly. */
bool kbd_relay_remote_pressed(uint8_t row, uint8_t col)
{
    taskENTER_CRITICAL(&s_left_mux);
    bool p = rf_bitmap_get(s_remote_bm, row, col);
    taskEXIT_CRITICAL(&s_left_mux);
    return p;
}

/* Has the remote state changed since the last call? Consumes the flag. */
bool kbd_relay_remote_changed(void)
{
    taskENTER_CRITICAL(&s_left_mux);
    bool ch = s_remote_changed;
    s_remote_changed = false;
    taskEXIT_CRITICAL(&s_left_mux);
    return ch;
}
#endif

/* ── Sleep: the relay sleeps with the board ────────────────────────────── */

/* The chip is powered off and re-armed by the owner's hook (radio_owner);
 * here only the refresh timer: stopped on sleep — otherwise its ticks
 * would count "unavailable" while the chip sleeps and skew "dongle seen" —,
 * restarted at fast cadence on wake. */
void kbd_relay_sleep_prepare(void)
{
    if (s_refresh_timer) esp_timer_stop(s_refresh_timer);
}

void kbd_relay_wake_restore(void)
{
    s_periode_ms = 0;                              /* the timer was stopped: force a restart */
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);
}

bool kbd_relay_active(void)
{
    return s_paired;
}

bool kbd_relay_dongle_vu(void)
{
    /* Sticky (mute at rest), tolerant of ~1% ESB refusals: drops after
     * 3 consecutive transmissions without ACK, never on an isolated refusal. */
    return s_sans_ack_ecran < 3;
}

void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6])
{
    s_last_mod = modifier;
    memcpy(s_last_kb, kb, 6);
    kbd_refresh_arm(&s_refresh, KBD_RELAY_REPEATS);
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);   /* a change wakes the fast cadence */
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
/* Fusion: the half no longer sends a finished HID report, it transmits its
 * RAW half-matrix to the dongle, which merges the two halves and runs the
 * engine. Same transmission path as send_kbd (kbd_tx_locked: excursion or direct).
 *
 * ⚠ Reaffirmation of held keys: as everywhere in this chain, "emit on
 * change" does not compose with "release on silence" (see CLAUDE.md and
 * half_link). The dongle releases a mute half after HALF_LINK_TIMEOUT_MS, so
 * a held key must be re-emitted periodically. The matrix refresh cadence is
 * a BENCH setting (tuned against the dongle's real timeout) — see
 * docs/superpowers/plans/2026-09-13-dongle-fusion-runtime.md. */
static bool send_matrix_frame(uint8_t half, const uint8_t *bitmap, radio_valide_cb_t encore_valide, void *ctx)
{
    rf_matrix_t m;
    m.half = half;
    memcpy(m.bitmap, bitmap, RF_HALF_BITMAP_BYTES);
    m.seq = __atomic_fetch_add(&s_status_seq, 1, __ATOMIC_RELAXED);   /* reuses the relay's sequence counter */
    uint8_t buf[8];
    uint16_t n = rf_encode_matrix(buf, &m);
    uint32_t refus_avant = s_tx_refuses;
    bool partie = n && kbd_tx_emettre(buf, (uint8_t)n, encore_valide, ctx);
    /* Permanent diagnostic (rare, ~1% at the bench): WHICH frame the ESB
     * refused after its 15 retransmissions. This is the very frame the
     * bounded re-emission below repeats — without it, a brief press was lost. */
    if (s_tx_refuses != refus_avant)
        ESP_LOGW(TAG, "MATRIX refused bm=%02X%02X%02X%02X — repeated by the bounded re-emission",
                 bitmap[0], bitmap[1], bitmap[2], bitmap[3]);
    return partie;
}

void kbd_relay_send_matrix(uint8_t half, const uint8_t *bitmap)
{
    /* CHANGE: the state and its generation are set BEFORE transmission — a
     * repeat that reread the old state during this send will come out stale. */
    taskENTER_CRITICAL(&s_left_mux);
    memcpy(s_last_left_bm, bitmap, RF_HALF_BITMAP_BYTES);
    s_last_left_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_left_gen++;
    taskEXIT_CRITICAL(&s_left_mux);
    (void)send_matrix_frame(half, bitmap, NULL, NULL);
    /* BOUNDED re-emission armed on change — KBD_RELAY_REPEATS x 10 ms, then
     * silence. A CHANGE frame refused by the ESB had only one chance: the
     * 100 ms reaffirmation only covers held keys, so a lost brief press was
     * never repaired (Super held + Q: Q never reached the host, a frame
     * refused in that slot — bench 2026-09-13). Same mechanism as the
     * pre-fusion HID path (test_repos_ne_reemet_pas); the dongle deduplicates
     * by content, so repeats are free for it. Repeats go through
     * send_matrix_frame: they do not re-arm themselves. */
    kbd_refresh_arm(&s_refresh, KBD_RELAY_REPEATS);
    kbd_relay_timer_set(KBD_RELAY_REFRESH_MS);   /* a change wakes the fast cadence */
}
#endif
