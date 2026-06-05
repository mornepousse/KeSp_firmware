/*
 * Dongle RF RX task: owns both NRF24 radios, per-half heartbeat state, and
 * the engine drive loop (mirrors keyboard_task.c but fed by RF instead of a
 * local matrix scan).
 */

/*
 * rf_signal_q255() — derive a 0..255 link-quality value for one half.
 *
 * 255 = best, 0 = link down / timed out. Displayed raw on the e-ink as "235/255".
 * Pure function: no globals, no I/O. Host-testable (outside TEST_HOST guard).
 * Place before the #ifndef TEST_HOST block so it compiles in both contexts.
 */
#include "rf_rx_task.h"
#include <stdint.h>
#include <stdbool.h>

uint8_t rf_signal_q255(bool link_up, uint32_t hb_age_ms, uint8_t link_q)
{
    /* Link is down if rf_rx_task flagged it, OR if the heartbeat age exceeds 3×
     * the nominal heartbeat interval (500 ms → 1500 ms). 3× = two missed HBs. */
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
#include "trackpad.h"
#include "heartbeat.h"
#include "board_rf.h"
#include "rf_pairing.h"   /* rf_pairing_load_set_id_dongle, rf_apply_set_id */
#include "keyboard_config.h"
#include "hid_transport.h"
#include "cdc_binary_protocol.h"   /* ks_respond, KS_CMD_MATRIX_TEST */
#include "espnow_link.h"    /* espnow_send() */
#include "espnow_msg.h"     /* en_status_t, EN_INFO_STATUS */
#include "nvs.h"            /* nvs_open, nvs_get_blob, nvs_close */
#include "tusb.h"           /* tud_ready() */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"      /* esp_read_mac, ESP_MAC_WIFI_STA */
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "rf_rx";

/* Engine globals (dongle_engine_state.c) */
extern uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
extern uint8_t current_press_row[];
extern uint8_t current_press_col[];
extern uint8_t current_press_stat[];
extern uint8_t keycodes[];
extern volatile uint8_t stat_matrix_changed;

/* Matrix-test mode (toggled by KS_CMD_MATRIX_TEST in cdc_binary_cmds.c).
 * Globals live in dongle_engine_state.c. When on, RF key events are streamed
 * raw to the controller and the engine/HID is skipped. */
extern volatile bool matrix_test_mode;
extern volatile uint32_t matrix_test_last_activity_ms;
#define MATRIX_TEST_TIMEOUT_MS 30000

#define MAX_REPORT_KEYS 6
#define INVALID_KEY_POS 0xFF
#define HALF_R_COL_OFFSET 7

/* Engine entry points */
extern void build_keycode_report(void);
extern void process_matrix_changes(void);
extern void send_hid_key(void);
extern bool key_processor_has_tap(void);
extern void key_processor_clear_taps(void);
extern void tap_hold_tick(void);
extern void tap_dance_tick(void);

static rf_radio_t s_left, s_right;

/* Current per-radio config (set_id-derived) — kept so the radio watchdog can
 * re-arm a wedged radio with the live address/channel. Updated at init and on
 * every pairing hot-switch. */
static rf_radio_cfg_t s_lcfg, s_rcfg;
static hb_half_state_t s_hb_left, s_hb_right;
static SemaphoreHandle_t s_evt_sem;

/* Last link_q received from each half (PKT_HEARTBEAT.link_q).
 * Read by rf_rx_get_status() → status_push_cb → rf_signal_bars(). */
static uint8_t s_link_q_left  = 0;
static uint8_t s_link_q_right = 0;

/* 5 s periodic status push timer handle */
static esp_timer_handle_t s_status_push_timer = NULL;

/* Forward declaration — defined after rf_rx_start() */
static void status_push_cb(void *arg);

/* ── Pairing window state (driven inside rf_rx_task) ── */
static volatile bool s_pairing_mode = false;
static uint32_t s_pair_deadline_ms = 0;
static uint8_t  s_pair_paired_count = 0;
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

/* ── Rebuild compact current_press_* arrays from MATRIX_STATE ── */
static void rebuild_press_arrays(void)
{
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
        keycodes[i] = 0;
    }
    uint8_t filled = 0;
    for (uint8_t r = 0; r < MATRIX_ROWS && filled < MAX_REPORT_KEYS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (MATRIX_STATE[r][c]) {
                current_press_row[filled] = r;
                current_press_col[filled] = c;
                current_press_stat[filled] = 1;
                filled++;
            }
    stat_matrix_changed = 1;
}

/* ── Engine callbacks for heartbeat reconciliation ── */
static void on_force_press(void *ctx, uint8_t half, uint8_t row, uint8_t col)
{
    (void)ctx;
    uint8_t gcol = (half == HB_HALF_RIGHT) ? col + HALF_R_COL_OFFSET : col;
    if (row < MATRIX_ROWS && gcol < MATRIX_COLS) MATRIX_STATE[row][gcol] = 1;
}
static void on_force_release(void *ctx, uint8_t half, uint8_t row, uint8_t col)
{
    (void)ctx;
    uint8_t gcol = (half == HB_HALF_RIGHT) ? col + HALF_R_COL_OFFSET : col;
    if (row < MATRIX_ROWS && gcol < MATRIX_COLS) MATRIX_STATE[row][gcol] = 0;
}
static const hb_callbacks_t s_cb = { on_force_press, on_force_release, NULL };

/* ── Run one engine cycle (mirrors keyboard_task.c matrix-changed branch) ── */
static void run_engine_cycle(void)
{
    rebuild_press_arrays();
    build_keycode_report();
    process_matrix_changes();
    if (key_processor_has_tap()) {
        send_hid_key();
        vTaskDelay(pdMS_TO_TICKS(10));
        key_processor_clear_taps();
        send_hid_key();
    } else {
        send_hid_key();
    }
}

/* ── Process all pending packets from one radio ── */
static bool drain_radio(rf_radio_t *radio, hb_half_state_t *hb, uint8_t half)
{
    bool changed = false;
    uint8_t buf[32];
    while (rf_driver_rx_available(radio)) {
        uint16_t n = rf_driver_read_rx(radio, buf, sizeof(buf));
        if (n == 0) break;
        uint8_t type = rf_packet_type(buf, n);
        if (type == PKT_TYPE_KEY) {
            rf_key_event_t e;
            if (rf_decode_key(buf, n, &e) && hb_apply_key(hb, &e)) {
                uint8_t gcol = (half == HB_HALF_RIGHT) ? e.col + HALF_R_COL_OFFSET : e.col;
                if (e.row < MATRIX_ROWS && gcol < MATRIX_COLS) {
                    if (matrix_test_mode) {
                        /* Stream raw (row,col,pressed) to the controller; no engine/HID.
                         * Mirrors matrix_scan.c so the soft's matrix test works on the dongle. */
                        uint8_t evt[3] = { e.row, gcol, (uint8_t)(e.pressed ? 1 : 0) };
                        matrix_test_last_activity_ms = esp_timer_get_time() / 1000;
                        ks_respond(KS_CMD_MATRIX_TEST, KS_STATUS_OK, evt, 3);
                    } else {
                        MATRIX_STATE[e.row][gcol] = e.pressed ? 1 : 0;
                        changed = true;
                    }
                }
            } else {
                radio->pkt_dup++;
            }
        } else if (type == PKT_TYPE_HEARTBEAT) {
            rf_heartbeat_t h;
            if (rf_decode_heartbeat(buf, n, &h)) {
                uint32_t now = esp_timer_get_time() / 1000;
                hb_reconcile(hb, half, &h, &s_cb, now);
                /* Update last link_q for signal quality derivation */
                if (half == HB_HALF_LEFT)  s_link_q_left  = h.link_q;
                else                        s_link_q_right = h.link_q;
                changed = true;   /* reconciliation may have changed MATRIX_STATE */
            }
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                static trackpad_state_t s_tp_state;
                static const trackpad_cfg_t s_tp_cfg = {
                    .fmt = TRACKPAD_CFG_FMT, .base = 100, .accel = 0, .gain_max = 100,
                };
                trackpad_out_t out;
                if (trackpad_map(tp.ge0, tp.ge1, tp.n_fingers, tp.rel_x, tp.rel_y,
                                 &s_tp_cfg, &s_tp_state, &out)) {
                    hid_send_mouse(out.buttons, out.dx, out.dy, out.scroll_v);
                }
            }
        }
    }
    return changed;
}

bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out)
{
    if (!s_left.present) return false;   /* need radio L for the rendezvous */

    if (reset) {
        rf_pairing_reset_dongle();
    }
    rf_pairing_load_peers_dongle(s_pair_mac_left, s_pair_mac_right, &s_pair_paired_count);

    /* Switch radio L to the pairing rendezvous PRX. */
    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    rf_driver_set_channel(&s_left, RF_PAIR_CHANNEL);
    rf_driver_set_rx_address(&s_left, pair_addr);

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

    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();
    rf_apply_set_id(&lcfg, set_id, 0x01);
    rf_apply_set_id(&rcfg, set_id, 0x02);
    s_lcfg = lcfg; s_rcfg = rcfg;   /* keep live config for the radio watchdog */

    uint8_t laddr[5] = { lcfg.rx_addr[0], lcfg.rx_addr[1], lcfg.rx_addr[2],
                         lcfg.rx_addr[3], lcfg.addr_suffix };
    uint8_t raddr[5] = { rcfg.rx_addr[0], rcfg.rx_addr[1], rcfg.rx_addr[2],
                         rcfg.rx_addr[3], rcfg.addr_suffix };
    rf_driver_set_channel(&s_left,  lcfg.channel);
    rf_driver_set_rx_address(&s_left, laddr);
    if (s_right.present) {
        rf_driver_set_channel(&s_right,  rcfg.channel);
        rf_driver_set_rx_address(&s_right, raddr);
    }
    ESP_LOGI(TAG, "hot-switch: set_id=0x%04X L ch=%u R ch=%u", set_id, lcfg.channel, rcfg.channel);

    /* Re-register ESP-NOW peers from the freshly-saved NVS pairing (+ re-derive
     * the WiFi channel if set_id changed). Without this a newly paired half is
     * not an ESP-NOW peer until a dongle reboot — its e-ink dashboard gets no
     * status pushes. */
    espnow_reload_peers();
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

    /* Drain any PKT_PAIR_REQ on radio L. */
    uint8_t buf[32];
    while (rf_driver_rx_available(&s_left)) {
        uint16_t n = rf_driver_read_rx(&s_left, buf, sizeof(buf));
        if (n == 0) break;
        uint8_t mac[6];
        uint8_t declared_slot = 0;
        if (!rf_decode_pair_req(buf, n, mac, &declared_slot)) continue;

        uint8_t slot = 0;
        bool is_dup = rf_pairing_match_slot(mac, s_pair_mac_left, s_pair_mac_right, &slot);
        if (!is_dup) {
            /* Half declares its own slot (board identity) → order-independent,
             * no L/R swap. Legacy halves send slot=0 → positional fallback. */
            if (!rf_pairing_resolve_slot(declared_slot, s_pair_paired_count, &slot)) continue; /* full */
        }

        /* Persist (new pairings only bump count). */
        if (!is_dup) {
            uint8_t new_count = s_pair_paired_count + 1;
            rf_pairing_save_peer_dongle(slot, mac, new_count);
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
        rf_driver_oob_tx(&s_left, RF_PAIR_CHANNEL, pair_addr, ackbuf, 10,
                         RF_PAIR_CHANNEL, pair_addr);   /* restore to PAIR PRX */
        ESP_LOGI(TAG, "ACK sent slot=0x%02X (dup=%d, paired_count=%u)",
                 slot, is_dup, s_pair_paired_count);
    }
    return true;
}

/* ── Radio watchdog — re-arm a radio that's been silent too long ─────────────
 * NRF24 (clone) modules can wedge over time: stop ACKing/receiving the half
 * even though SPI is fine (observed: left ack% → 0, only a dongle reboot fixed
 * it). If a present radio gets NO heartbeat for > RF_REARM_SILENCE_MS, re-assert
 * its RX config to self-heal — no reboot. Rate-limited; harmless if the half is
 * simply off (re-arm just rewrites registers, nothing to receive). */
#define RF_REARM_SILENCE_MS 2000u
static void rf_rx_watchdog(uint32_t now)
{
    static uint32_t s_left_rearm_ms = 0, s_right_rearm_ms = 0;
    if (s_left.present &&
        (now - s_hb_left.last_hb_ms) > RF_REARM_SILENCE_MS &&
        (now - s_left_rearm_ms)      > RF_REARM_SILENCE_MS) {
        rf_driver_rearm_rx(&s_left, &s_lcfg);
        s_left_rearm_ms = now;
        ESP_LOGW(TAG, "watchdog: re-armed LEFT radio (silent %lu ms)",
                 (unsigned long)(now - s_hb_left.last_hb_ms));
    }
    if (s_right.present &&
        (now - s_hb_right.last_hb_ms) > RF_REARM_SILENCE_MS &&
        (now - s_right_rearm_ms)      > RF_REARM_SILENCE_MS) {
        rf_driver_rearm_rx(&s_right, &s_rcfg);
        s_right_rearm_ms = now;
        ESP_LOGW(TAG, "watchdog: re-armed RIGHT radio (silent %lu ms)",
                 (unsigned long)(now - s_hb_right.last_hb_ms));
    }
}

static void rf_rx_task(void *arg)
{
    (void)arg;
    const TickType_t tick_period = pdMS_TO_TICKS(10);
    for (;;) {
        xSemaphoreTake(s_evt_sem, tick_period);

        if (s_pairing_mode) {
            rf_rx_pairing_service();
            continue;   /* skip normal RX/engine while pairing */
        }

        bool changed = false;
        if (s_left.present)  changed |= drain_radio(&s_left,  &s_hb_left,  HB_HALF_LEFT);
        if (s_right.present) changed |= drain_radio(&s_right, &s_hb_right, HB_HALF_RIGHT);

        uint32_t now = esp_timer_get_time() / 1000;
        hb_check_timeout(&s_hb_left,  HB_HALF_LEFT,  &s_cb, now, 250);
        hb_check_timeout(&s_hb_right, HB_HALF_RIGHT, &s_cb, now, 250);
        rf_rx_watchdog(now);   /* self-heal a wedged NRF radio (no reboot) */

        if (matrix_test_mode) {
            /* Test events are streamed from drain_radio; skip the engine so no HID
             * is emitted. Auto-exit after 30 s without CDC activity (matches keyboards). */
            if (now - matrix_test_last_activity_ms > MATRIX_TEST_TIMEOUT_MS) {
                matrix_test_mode = false;
                ESP_LOGW(TAG, "matrix test mode timeout — auto-exit");
            }
            continue;
        }

        tap_hold_tick();
        tap_dance_tick();

        if (changed)
            run_engine_cycle();
    }
}

bool rf_rx_start(void)
{
    s_evt_sem = xSemaphoreCreateBinary();

    rf_radio_cfg_t lcfg = board_rf_left_cfg();
    rf_radio_cfg_t rcfg = board_rf_right_cfg();

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
    rf_apply_set_id(&lcfg, set_id, 0x01);   /* left  → slot 0x01 */
    rf_apply_set_id(&rcfg, set_id, 0x02);   /* right → slot 0x02 */
    s_lcfg = lcfg; s_rcfg = rcfg;           /* keep live config for the radio watchdog */

    /* Load paired half MACs from NVS at boot so status_push_cb (and layer/state
     * push) can send EN_INFO_STATUS immediately — without waiting for a pairing
     * session. These were previously only populated in rf_rx_pair_start(), so a
     * plain reboot left them zeroed and the ESP-NOW status link stayed down
     * until a re-pairing. s_pair_mac_* is the single source of truth, also
     * refreshed on each successful pairing. */
    rf_pairing_load_peers_dongle(s_pair_mac_left, s_pair_mac_right, &s_pair_paired_count);

    /* Park BOTH CSN HIGH before initialising either radio. The dongle shares
     * one SPI bus between NRF1 (csn=13) and NRF2 (csn=1 — a UART0 strap pin
     * that floats LOW at reset). If RIGHT's CSN is still floating during
     * rf_driver_init(LEFT), NRF2 will silently latch LEFT's SPI traffic in
     * parallel, and the writes meant for NRF1 get corrupted by the parasitic
     * activity on the bus. Observed symptom: NRF1 boots with CONFIG=0x3E,
     * EN_AA=0, EN_RXADDR=0 while NRF2 looks fine — verify_rx FAILs on LEFT.
     * Pre-driving both CSN HIGH guarantees only one radio sees each command. */
    gpio_config_t csn_park = {
        .pin_bit_mask = (1ULL << lcfg.pin_csn) | (1ULL << rcfg.pin_csn),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&csn_park);
    gpio_set_level(lcfg.pin_csn, 1);
    gpio_set_level(rcfg.pin_csn, 1);

    rf_driver_init(&s_left, &lcfg);
    rf_driver_verify_rx(&s_left, &lcfg);    /* read-back config check (logs OK / per-reg FAIL) */
    rf_driver_init(&s_right, &rcfg);
    rf_driver_verify_rx(&s_right, &rcfg);

    if (!s_left.present && !s_right.present) {
        ESP_LOGE(TAG, "no NRF radios present - RF disabled");
        return false;
    }

    gpio_install_isr_service(0);
    if (s_left.present) {
        gpio_set_intr_type(lcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(lcfg.pin_irq, nrf_irq_isr, &s_left);
        rf_radio_set_irq_sem(&s_left, s_evt_sem);
    }
    if (s_right.present) {
        gpio_set_intr_type(rcfg.pin_irq, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(rcfg.pin_irq, nrf_irq_isr, &s_right);
        rf_radio_set_irq_sem(&s_right, s_evt_sem);
    }

    xTaskCreatePinnedToCore(rf_rx_task, "rf_rx", 8192, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "RF RX started (L=%d R=%d)", s_left.present, s_right.present);

    /* ── 5 s periodic status push to paired halves ───────────────
     * NB: rf_rx_start() runs just BEFORE espnow_link_init() in main.c, so the
     * timer is created before ESP-NOW is up — but the first tick fires at +5 s,
     * by which time espnow_link_init() (synchronous, ms) has completed, so
     * status_push_cb's espnow_send() is safe. The callback is safe from the
     * esp_timer task context: no SPI, no portMAX_DELAY mutex (see
     * status_push_cb comment). */
    const esp_timer_create_args_t status_args = {
        .callback = status_push_cb,
        .name     = "status_push_tick",
    };
    if (esp_timer_create(&status_args, &s_status_push_timer) == ESP_OK) {
        esp_timer_start_periodic(s_status_push_timer, 5 * 1000 * 1000ULL);  /* 5 s in µs */
        ESP_LOGI(TAG, "status push timer started (5 s period)");
    } else {
        ESP_LOGW(TAG, "status push timer create failed -- status push disabled");
    }

    return true;
}

void rf_rx_get_status(rf_link_status_t *out)
{
    uint32_t now = esp_timer_get_time() / 1000;
    out->link_left  = s_hb_left.link_up;
    out->link_right = s_hb_right.link_up;
    out->hb_age_left_ms  = now - s_hb_left.last_hb_ms;
    out->hb_age_right_ms = now - s_hb_right.last_hb_ms;
    out->pkt_rx_left  = s_left.pkt_rx;
    out->pkt_rx_right = s_right.pkt_rx;
    out->pkt_dup_left  = s_left.pkt_dup;
    out->pkt_dup_right = s_right.pkt_dup;
    out->link_q_left  = s_link_q_left;
    out->link_q_right = s_link_q_right;
}

void rf_rx_copy_peer_macs(uint8_t mac_left[6], uint8_t mac_right[6])
{
    /* Live copy maintained by this task (loaded at boot, refreshed on pairing). */
    memcpy(mac_left,  s_pair_mac_left,  6);
    memcpy(mac_right, s_pair_mac_right, 6);
}

/*
 * status_push_cb — periodic esp_timer callback (5 s period).
 *
 * Context: esp_timer task (NOT rf_rx_task).
 * Safe to call: rf_rx_get_status() (no SPI, copies atomically),
 *               espnow_send() (thread-safe per ESP-NOW spec),
 *               tud_ready() (TinyUSB, thread-safe read).
 * Must NOT call: any rf_driver_* (SPI), xSemaphoreTake with portMAX_DELAY.
 */
static void status_push_cb(void *arg)
{
    (void)arg;

    /* Use the live paired-MAC copy maintained by the RX task: loaded from NVS at
     * boot (rf_pairing_load_peers_dongle) and refreshed on every successful
     * pairing. A previous one-shot static cache here required a dongle reboot
     * after re-pairing before the status push would resume — fixed by reading
     * the live copy each tick. */
    const uint8_t *mac_left  = s_pair_mac_left;
    const uint8_t *mac_right = s_pair_mac_right;

    bool has_left  = mac_left[0]  | mac_left[1]  | mac_left[2]  |
                     mac_left[3]  | mac_left[4]  | mac_left[5];
    bool has_right = mac_right[0] | mac_right[1] | mac_right[2] |
                     mac_right[3] | mac_right[4] | mac_right[5];

    if (!has_left && !has_right) {
        ESP_LOGD(TAG, "status_push_cb: no paired halves -- skip");
        return;
    }

    /* Get current RF link diagnostics */
    rf_link_status_t st;
    rf_rx_get_status(&st);

    /* Build EN_INFO_STATUS payload */
    en_status_t msg;
    msg.sig_left  = rf_signal_q255(st.link_left,  st.hb_age_left_ms,  st.link_q_left);
    msg.sig_right = rf_signal_q255(st.link_right, st.hb_age_right_ms, st.link_q_right);
    msg.flags = 0;
    if (st.link_left)  msg.flags |= (1u << 0);
    if (st.link_right) msg.flags |= (1u << 1);
    if (tud_ready())   msg.flags |= (1u << 2);   /* USB active */

    ESP_LOGD(TAG, "status_push: flags=0x%02x sig_l=%u sig_r=%u",
             msg.flags, msg.sig_left, msg.sig_right);

    /* Unicast to each paired half */
    if (has_left)  espnow_send(mac_left,  EN_INFO_STATUS, &msg, sizeof(msg));
    if (has_right) espnow_send(mac_right, EN_INFO_STATUS, &msg, sizeof(msg));
}

#endif /* TEST_HOST */
