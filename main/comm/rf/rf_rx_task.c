/*
 * Dongle RF task: it owns both NRF24 radios and nothing else.
 *
 * These are no longer two halves of the same keyboard. Slot 1 carries the
 * keyboard — the Niphargus master half, which runs its own keymap engine at
 * home and only sends already-finished HID here — and slot 2 carries the
 * Conchodytes mouse. The two devices are independent; what rf_slot.h makes
 * impossible to forget is that losing one must release nothing of the other.
 *
 * There is therefore no more matrix, bitmap reconciliation, or engine cycle
 * in this file: the dongle relays and supervises.
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
#include "dongle_engine.h"   /* fusion: embedded keymap engine (kept internal) */
#include "batt_calc.h"      /* batt_soc_pct — SoC derived from voltage, dongle side */
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

/* Radio 1 → keyboard slot, radio 2 → mouse slot. The name states the role; the
 * hardware config (pins, address, channel) stays named by the physical radio
 * in board_rf.h, because that is what is silkscreened on the board. */
static rf_radio_t s_kbd, s_mouse;

/* Presence of each slot: timestamp of the last packet received, whatever it
 * was — heartbeat, status frame, or HID report. This is also what the radio
 * watchdog reads: trusting the heartbeat's age would re-arm a perfectly
 * healthy radio as soon as the half stops sending them (cf. §7 bis of the dongle design). */
static rf_slot_link_t s_link[RF_SLOT_COUNT];

/* Current per-radio config (set_id-derived) — kept so the radio watchdog can
 * re-arm a wedged radio with the live address/channel. Updated at init and on
 * every pairing hot-switch. */
static rf_radio_cfg_t s_kbd_cfg, s_mouse_cfg;
static SemaphoreHandle_t s_evt_sem;

/* Last link_q announced by each slot (heartbeat or status frame).
 * Read by rf_rx_get_status() → CDC RF_STATUS. */
static uint8_t s_link_q[RF_SLOT_COUNT];

/* ── Pairing window state (driven inside rf_rx_task) ── */
static volatile bool s_pairing_mode = false;
static uint32_t s_pair_deadline_ms = 0;
static uint8_t  s_pair_paired_count = 0;
/* The pairing NVS keys keep their original names: renaming them would
 * unpair already-paired hardware for a purely cosmetic gain.
 * `left` here designates slot 0x01 (keyboard), `right` slot 0x02 (mouse). */
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

/* The dongle no longer runs an engine: it receives already-finished HID and
 * pushes it on to the host. rebuild_press_arrays(), the reconciliation
 * callbacks and run_engine_cycle() were removed along with the matrix — see
 * docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md
 *
 * What remains to watch: a slot's silence. With no matrix to release, the
 * fallback comes down to an empty report — otherwise the last report received
 * would stay applied on the host indefinitely.
 *
 * And it applies to that slot alone: a mouse going out of range must not
 * clear the ongoing keystroke. rf_slot_link_check() is what decides. */
static void apply_safe_action(rf_safe_action_t action)
{
    static const uint8_t none[6] = {0};
    if (action == RF_SAFE_RELEASE_KEYS)         hid_send_keyboard(0, none);
    else if (action == RF_SAFE_RELEASE_BUTTONS) hid_send_mouse(0, 0, 0, 0);
}

/* The battery cache had no one left to feed it since heartbeat reconciliation
 * was removed: the CDC BATTERY command replied "unknown" permanently. The
 * status frame fills it again. It only carries the voltage — the charge
 * state and ongoing charging stay unknown rather than being guessed, because
 * four bytes was a design constraint and not an oversight.
 */
extern void dongle_cache_set_battery(uint8_t slot, uint8_t batt_dV,
                                     uint8_t soc_pct, uint8_t charging);

/* Battery cache indexed by HALF (0 = left, 1 = right), NOT by slot: under
 * fusion the two halves share the keyboard slot and are distinguished by
 * STATUS's identity flag. Two conventions meet here: the radio says "0 =
 * unknown" (batt_dV), the cache and the CDC say "0xFF = unknown" — the
 * translation happens at this boundary and nowhere else. The SoC is DERIVED
 * from the voltage on the dongle side (batt_soc_pct, pure), not transported. */
static void cache_battery_half(uint8_t half, uint8_t batt_dV, uint8_t charging)
{
    uint8_t idx = (half == RF_HALF_RIGHT) ? 1 : 0;
    if (batt_dV == 0) { dongle_cache_set_battery(idx, 0xFF, 0xFF, 0xFF); return; }
    dongle_cache_set_battery(idx, batt_dV, batt_soc_pct(batt_dV), charging);
}

/* ── Drain pending packets on a radio ── */
static void drain_radio(rf_radio_t *radio, uint8_t slot)
{
    uint8_t buf[32];
    while (rf_driver_rx_available(radio)) {
        uint16_t n = rf_driver_read_rx(radio, buf, sizeof(buf));
        if (n == 0) break;
        /* Every packet counts as proof of life, not just heartbeats. */
        rf_slot_link_rx(&s_link[slot], (uint32_t)(esp_timer_get_time() / 1000));
        uint8_t type = rf_packet_type(buf, n);
#if CONFIG_KASE_DONGLE_FUSION
        /* Auto keymap sync via ACK payload (phase 3). On a known divergence,
         * we load the payload that will go out in the ACK of the NEXT frame
         * from the left half: the CHUNK it just requested (SYNC_REQ), else the
         * BEACON telling it a keymap is waiting for it. Reloaded on EVERY
         * frame — an oob_tx excursion empties the PRX's TX FIFO, hence any
         * pending payload; and the nRF24 only keeps three. In sync → nothing
         * is loaded: the ACK goes out bare, zero cost.
         * Go/no-go proven on the bench on 2026-09-13 (return channel alive, 11/11). */
        /* ⚠ Only after a frame FROM THE LEFT half. The right half shares the
         * keyboard slot (same address KaSe.01): a payload loaded after ITS
         * frame would go out in ITS ACK, discarded by it — a chunk lost per
         * right-half frame, and the left half's pull stalls under bilateral
         * typing (review 2026-09-13). STATUS and SYNC_REQ only come from the
         * left half; MATRIX carries the half identity. */
        rf_matrix_t lm;
        bool de_la_gauche = (type == PKT_TYPE_STATUS) || (type == PKT_TYPE_SYNC_REQ) ||
                            (type == PKT_TYPE_MATRIX && rf_decode_matrix(buf, n, &lm) &&
                             lm.half == RF_HALF_LEFT);
        /* The ACK payload is BUILT here but LOADED at the end of the pass: an
         * oob_tx excursion (right→left re-emission) empties the PRX's TX
         * FIFO, and a payload loaded before it goes to waste (bench
         * 2026-09-14, seen with a screen frame since removed). */
        uint8_t  ap[32];
        uint16_t apl = 0;
        if (slot == RF_SLOT_KBD && de_la_gauche && dongle_sync_active()) {
            uint8_t req_next = SYNC_N_CHUNKS;   /* default: beacon */
            rf_sync_req_t q;
            if (type == PKT_TYPE_SYNC_REQ && rf_decode_sync_req(buf, n, &q)) req_next = q.next;
            apl = dongle_sync_ack_for(req_next, ap);
        }
#endif
        /* PKT_TYPE_KEY (raw matrix) and PKT_TYPE_TRACKPAD (raw gesture) are no
         * longer handled: nothing emits them any more since the old halves
         * were removed, and the Niphargus sends already-finished HID. The
         * dongle no longer decodes any matrix — which is what makes the
         * existence of two keymap engines in the system impossible. */
        if (type == PKT_TYPE_STATUS) {
            /* Idle frame: battery and link quality, no key state. */
            rf_status_t st;
            if (rf_decode_status(buf, n, &st)) {
                s_link_q[slot] = st.link_q;
                cache_battery_half(st.half, st.batt_dV, st.charging);
#if CONFIG_KASE_DONGLE_FUSION
                /* The left half announces its mode AND its keymap's fingerprint
                 * via STATUS on the keyboard slot. Sync safeguard: the dongle
                 * compares it to its own (config_fp=0 = not announced, in USB mode). */
                if (slot == RF_SLOT_KBD) {
                    dongle_engine_set_left_usb(st.mode_usb);
                    if (st.config_fp != 0) dongle_engine_note_left_fp(st.config_fp);
                }
#endif
            }
        } else if (type == PKT_TYPE_HEARTBEAT) {
            /* Old format, kept until the Niphargus replaces it: its bitmap is
             * no longer read, there is no more matrix here. */
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                s_link_q[slot] = h.link_q;
                cache_battery_half(RF_HALF_LEFT, h.batt_dV, 0);   /* old format: left half only */
            }
        } else if (type == PKT_TYPE_HIDREPORT) {
            /* The keyboard has already run its engine: pushed on as-is.
             * Under fusion mode the KEYBOARD no longer emits finished HID (it
             * sends its raw matrix, see PKT_TYPE_MATRIX) — but the
             * Conchodytes MOUSE keeps emitting finished HID on its slot. This
             * path therefore stays necessary in both modes. */
            uint8_t sub, mod, kb[6], btn; int8_t x, y, w;
            if (rf_decode_hidreport(buf, n, &sub, &mod, kb, &btn, &x, &y, &w)) {
                if (sub == RF_HID_SUB_KBD)        hid_send_keyboard(mod, kb);
                else if (sub == RF_HID_SUB_MOUSE) hid_send_mouse(btn, x, y, w);
            }
        }
#if CONFIG_KASE_DONGLE_FUSION
        else if (type == PKT_TYPE_MATRIX) {
            /* Fusion: a half's raw half-matrix. We hand it to the embedded
             * engine, which merges the two halves and outputs the HID. */
            rf_matrix_t m;
            if (rf_decode_matrix(buf, n, &m)) {
                /* A left half emitting RAW is in wireless mode: the dongle types.
                 * (In USB mode it emits no matrix, it announces via STATUS.) */
                if (m.half == RF_HALF_LEFT) dongle_engine_set_left_usb(false);
                dongle_engine_on_matrix(&m);

                /* Left-USB mode: re-emit the RIGHT half's half-matrix toward
                 * the left half (RF_CH_HALF_LINK / KaSe.03), as a heartbeat that
                 * the left half's listening decodes. PRX→PTX→PRX excursion on the keyboard radio.
                 *
                 * ⚠ RE-EMIT ONLY ON CHANGE — not every frame.
                 * Re-emitting every frame (the right half reaffirms its holds
                 * ~10/s AND retransmits) monopolized the dongle's radio in
                 * excursions: it stopped listening, hence stopped ACKing the
                 * right half → the right half kept retransmitting in a loop →
                 * spiral, ACK collapsed (bench bug 2026-09-13, "the right half
                 * dies on every USB plug-in"). So we only re-emit on a bitmap
                 * CHANGE, plus a BOUNDED refresh (100 ms) as long as a key is
                 * held — the dongle stays listening most of the time. Same
                 * rule as half_link: silent at rest, kept alive on hold. */
                if (m.half == RF_HALF_RIGHT && dongle_engine_left_usb()) {
                    static uint8_t  s_reemit_last[RF_HALF_BITMAP_BYTES];
                    static uint32_t s_reemit_ms;
                    uint32_t now2 = (uint32_t)(esp_timer_get_time() / 1000);
                    bool change = memcmp(s_reemit_last, m.bitmap, RF_HALF_BITMAP_BYTES) != 0;
                    bool tenu   = (m.bitmap[0] | m.bitmap[1] | m.bitmap[2] | m.bitmap[3]) != 0;
                    if (change || (tenu && (uint32_t)(now2 - s_reemit_ms) >= 100u)) {
                        rf_heartbeat_t h;
                        memset(&h, 0, sizeof(h));
                        memcpy(h.bitmap, m.bitmap, RF_HALF_BITMAP_BYTES);
                        h.seq = m.seq;
                        uint8_t hb[16];
                        uint16_t hn = rf_encode_heartbeat(hb, &h);
                        static const uint8_t left_addr[5] = { 'K','a','S','e', RF_ADDR_HALF_LINK };
                        uint8_t restore_addr[5] = { s_kbd_cfg.rx_addr[0], s_kbd_cfg.rx_addr[1],
                                                    s_kbd_cfg.rx_addr[2], s_kbd_cfg.rx_addr[3],
                                                    s_kbd_cfg.addr_suffix };
                        rf_driver_oob_tx(&s_kbd, RF_CH_HALF_LINK, left_addr, hb, (uint8_t)hn,
                                         s_kbd_cfg.channel, restore_addr);
                        memcpy(s_reemit_last, m.bitmap, RF_HALF_BITMAP_BYTES);
                        s_reemit_ms = now2;
                    }
                }
            }
        }
        /* After any excursion: the ACK payload (sync) for the NEXT frame from
         * the left half, see above. */
        if (apl) rf_driver_load_ack_payload(radio, 0, ap, (uint8_t)apl);
#endif
    }
}

bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out)
{
    if (!s_kbd.present) return false;   /* radio 1 carries the pairing rendezvous */

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
    ESP_LOGI(TAG, "hot-switch: set_id=0x%04X keyboard ch=%u mouse ch=%u",
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
            /* The device declares its own slot (board identity) → the
             * pairing order no longer matters. slot=0 → positional fallback. */
            if (!rf_pairing_resolve_slot(declared_slot, s_pair_paired_count, &slot)) continue; /* full */
        }

        /* Persist (new pairings only bump count).
         *
         * ⚠ DO NOT ACK A PAIRING WE FAILED TO RECORD.
         *
         * The return value of rf_pairing_save_peer_dongle() used to be
         * ignored here. Observed on the bench on 2026-08-26 with the
         * Conchodytes mouse: the dongle's NVS refused its writes, the ACK
         * went out anyway, and the device went off convinced it was paired —
         * set_id and slot recorded on its side — while the dongle kept no
         * trace of it and kept listening for the rendezvous. Both transmitted
         * on different addresses, zero packets got through, and NOTHING signaled it.
         *
         * A loud failure is better than a phantom pairing: without an ACK,
         * the device retries then gives up, saying so. */
        if (!is_dup) {
            uint8_t new_count = s_pair_paired_count + 1;
            esp_err_t err = rf_pairing_save_peer_dongle(slot, mac, new_count);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "pairing NOT recorded (slot=0x%02X): %s "
                              "— no ACK, the dongle's NVS is failing",
                         slot, esp_err_to_name(err));
                continue;   /* no ACK: see the comment above */
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

/* ── Radio watchdog — re-arm a radio silent too long ──────────────────────
 * NRF24 modules (clones) freeze over time: they stop ACKing and receiving
 * while the SPI still responds (observed: ack% → 0, only a dongle reboot
 * restored the link). Past RF_REARM_SILENCE_MS without a single packet, we
 * rewrite the RX config — no reboot. Rate-limited, and has no effect if the
 * device is simply switched off. */
/* Constants moved into rf_slot.h: it is a contract with the keyboard,
 * which must know the silence budget it must not exceed. */



static void rearm_if_silent(rf_radio_t *radio, const rf_radio_cfg_t *cfg,
                            uint8_t slot, uint32_t *last_rearm_ms,
                            uint32_t now, const char *name)
{
    if (!radio->present) return;
    if ((uint32_t)(now - s_link[slot].last_rx_ms) <= RF_REARM_SILENCE_MS) return;
    if ((uint32_t)(now - *last_rearm_ms) <= RF_REARM_SILENCE_MS) return;
    rf_driver_rearm_rx(radio, cfg);
    *last_rearm_ms = now;
    ESP_LOGW(TAG, "watchdog: radio %s rearmed (nothing received in %lu ms)",
             name, (unsigned long)(now - s_link[slot].last_rx_ms));
}

static void rf_rx_watchdog(uint32_t now)
{
    static uint32_t s_kbd_rearm_ms = 0, s_mouse_rearm_ms = 0;
    rearm_if_silent(&s_kbd,   &s_kbd_cfg,   RF_SLOT_KBD,   &s_kbd_rearm_ms,   now, "keyboard");
    rearm_if_silent(&s_mouse, &s_mouse_cfg, RF_SLOT_MOUSE, &s_mouse_rearm_ms, now, "mouse");
}

static void rf_rx_task(void *arg)
{
    (void)arg;
    /* ⚠ USED TO BE 10 ms. In principle this is only a fallback — each radio's
     * IRQ wakes `s_evt_sem` (see nrf_irq_isr) — but in practice it was the one
     * governing, and it CAPS THE THROUGHPUT: an nRF24's receive FIFO only
     * holds 3 packets, and a packet arriving with a full FIFO is not ACKed,
     * hence lost. Three packets per wake-up is 300/s at best.
     *
     * Invisible with a keyboard, which produces a few events per second.
     * A mouse transmits at 1 kHz DURING GESTURES. Measured on the bench on
     * 2026-08-26: 8356 frames sent, 3135 accepted — 37.5%, exactly the 3 out
     * of 10 that a 10 ms window lets through.
     *
     * 1 ms raises the cap to ~3000/s. The dongle is USB-powered: a wake-up
     * every millisecond costs nothing here, unlike on the mouse side. */
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

        /* Link lost → fallback, on that slot only. hb_check_timeout() no
         * longer fit: it walked a matrix bitmap that no longer exists here,
         * and would therefore never have released anything. */
        rf_safe_action_t a_kbd =
            rf_slot_link_check(&s_link[RF_SLOT_KBD], RF_SLOT_KBD, now, RF_LINK_LOST_MS);
        if (a_kbd != RF_SAFE_NONE) ESP_LOGW(TAG, "keyboard link lost → keys released");
        apply_safe_action(a_kbd);

        rf_safe_action_t a_mouse =
            rf_slot_link_check(&s_link[RF_SLOT_MOUSE], RF_SLOT_MOUSE, now, RF_LINK_LOST_MS);
        if (a_mouse != RF_SAFE_NONE) ESP_LOGW(TAG, "mouse link lost → buttons released");
        apply_safe_action(a_mouse);

        rf_rx_watchdog(now);   /* fix a frozen radio, without a reboot */
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
    rf_apply_set_id(&kcfg, set_id, 0x01);   /* keyboard → slot 0x01 */
    rf_apply_set_id(&mcfg, set_id, 0x02);   /* mouse    → slot 0x02 */
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
        ESP_LOGE(TAG, "no NRF radio present — RF disabled");
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
    ESP_LOGI(TAG, "RF RX started (keyboard=%d mouse=%d)", s_kbd.present, s_mouse.present);

#if CONFIG_KASE_DONGLE_FUSION
    /* Fusion: start the embedded keymap engine. drain_radio hands it the
     * half-matrices; it merges them and outputs the HID. */
    dongle_engine_start();
#endif

    return true;
}

void rf_rx_get_status(rf_link_status_t *out)
{
    /* The reported age is that of the last packet received, not the last
     * heartbeat: that is what the task now tracks, and an active link no
     * longer sends any heartbeats at all (cf. the adaptive cadence, §5 of the dongle design). */
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
