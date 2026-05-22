/*
 * Dongle RF RX task: owns both NRF24 radios, per-half heartbeat state, and
 * the engine drive loop (mirrors keyboard_task.c but fed by RF instead of a
 * local matrix scan).
 */
#include "rf_rx_task.h"
#include "rf_driver.h"
#include "rf_packet.h"
#include "heartbeat.h"
#include "board_rf.h"
#include "keyboard_config.h"
#include "hid_transport.h"
#include "cdc_binary_protocol.h"   /* ks_respond, KS_CMD_MATRIX_TEST */
#include "esp_log.h"
#include "esp_timer.h"
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
static hb_half_state_t s_hb_left, s_hb_right;
static SemaphoreHandle_t s_evt_sem;

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
                changed = true;   /* reconciliation may have changed MATRIX_STATE */
            }
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                /* Forward mouse data directly — bypasses keyboard engine cycle.
                 * hid_send_mouse signature: (buttons, x, y, wheel).
                 * scroll_h is always 0 (out of v1 scope; no horizontal wheel arg). */
                hid_send_mouse(tp.buttons, tp.dx, tp.dy, tp.scroll_v);
            }
        }
    }
    return changed;
}

static void rf_rx_task(void *arg)
{
    (void)arg;
    const TickType_t tick_period = pdMS_TO_TICKS(10);
    for (;;) {
        xSemaphoreTake(s_evt_sem, tick_period);

        bool changed = false;
        if (s_left.present)  changed |= drain_radio(&s_left,  &s_hb_left,  HB_HALF_LEFT);
        if (s_right.present) changed |= drain_radio(&s_right, &s_hb_right, HB_HALF_RIGHT);

        uint32_t now = esp_timer_get_time() / 1000;
        hb_check_timeout(&s_hb_left,  HB_HALF_LEFT,  &s_cb, now, 250);
        hb_check_timeout(&s_hb_right, HB_HALF_RIGHT, &s_cb, now, 250);

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
    rf_driver_init(&s_left, &lcfg);
    rf_driver_init(&s_right, &rcfg);

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
}
