#include "matrix_scan.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* TEST veto */
#endif
#include "matrix_flag.h"

#include "keyboard_task.h"
#include "key_stats.h"
#include "keyboard_config.h"
#include "wake_grace.h"         /* wake_grace_ms — driver grace period on wake (all boards) */
#include "cdc_binary_protocol.h"
#if CONFIG_KASE_HALF_LINK_TX
#include "half_link.h"
#include "rf_packet.h"
#endif
#if CONFIG_KASE_DONGLE_FUSION
#include "rf_packet.h"          /* rf_matrix_to_bitmap, RF_HALF_LEFT */
#include "fusion_route.h"       /* fusion_left_emits_raw (rule 3) */
#include "half_link.h"          /* half_col_to_keymap (remote fusion 4b) */
#if CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"       /* kbd_relay_send_matrix + remote_pressed/changed */
#include "usb_presence.h"       /* kbd_active_route / KBD_OUT_USB */
#endif
#endif
#include <esp_log.h>
#include <stdint.h>
#include <string.h>
#include "keyboard_button.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KASE_HAS_DISPLAY
#include "status_display.h"
#endif
#include "driver/gpio.h"
#include "esp_sleep.h"

#define TAG "MATRIX_SCAN"

/* ── Matrix test mode ──────────────────────────────────────────── */
volatile bool matrix_test_mode = false;
volatile uint32_t matrix_test_last_activity_ms = 0;
#define MATRIX_TEST_TIMEOUT_MS 30000  /* 30s auto-exit */

// Define variables expected by other code
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE, &SLAVE_MATRIX_STATE };
#define MAX_REPORT_KEYS  6       /* HID boot protocol: max 6 simultaneous keys */

uint8_t keycodes[MAX_REPORT_KEYS];
uint8_t current_press_row[MAX_REPORT_KEYS];
uint8_t current_press_col[MAX_REPORT_KEYS];
uint8_t current_press_stat[MAX_REPORT_KEYS];

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
/* Ordering of the remote columns — a property of the wiring, declared by
 * the master's board.h. 0 = simple shift, 1 = mirror. */
#ifndef BOARD_REMOTE_COLS_MIRRORED
#define BOARD_REMOTE_COLS_MIRRORED 0
#endif

/* Source of the remote half-matrix depending on mode:
 *  - fusion, left on USB → kbd_relay_remote_pressed (right re-emitted by the
 *    dongle and received while listening on USB). */
#define KASE_REMOTE_PRESSED(r, c) kbd_relay_remote_pressed((r), (c))

/* Boundary between the entries of the LOCAL scan and those received by radio.
 * Everything beyond it belongs to the remote half and is rebuilt on every
 * call to matrix_apply_remote(). */
static uint8_t s_filled_local;

/* Fusion of the two halves — IDEMPOTENT, so it can be called every cycle.
 *
 * It has to be: the scan callback only runs on LOCAL activity, and a
 * keypress on the right half alone produces none. Without a periodic call
 * from the keyboard task, keys received by radio would never reach the
 * engine — that's the bug that made the right half type nothing while its
 * link was acknowledged.
 *
 * Remote keys occupy columns 7-13, this half occupies 0-6. The engine does
 * not know where they come from, it just indexes keymaps[layer][row][col]
 * and nothing else. The ORDERING depends on the wiring and goes through
 * half_col_to_keymap(): since both halves are the same PCB flipped over,
 * the right's column 0 is its rightmost key.
 *
 * The MAX_REPORT_KEYS ceiling is respected, and local keys keep priority
 * since they occupy the start of the array. */
void matrix_apply_remote(void)
{
    uint8_t filled = s_filled_local;
    for (uint8_t i = filled; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    for (uint8_t r = 0; r < MATRIX_ROWS && filled < MAX_REPORT_KEYS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (KASE_REMOTE_PRESSED(r, c)) {
                current_press_row[filled]  = r;
                current_press_col[filled]  = half_col_to_keymap(
                        c, MATRIX_COLS, BOARD_REMOTE_COLS_MIRRORED);
                current_press_stat[filled] = 1;
                filled++;
            }
}
#endif
volatile uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
uint8_t current_layout = 0;
volatile uint8_t is_layer_changed = 0;
volatile uint32_t last_activity_time_ms = 0;

static keyboard_btn_handle_t s_kbd = NULL;
static uint8_t prev_matrix_state[MATRIX_ROWS][MATRIX_COLS];  /* For KPM: track new keypresses */
/* Matrix state captured on sleep wake, by manual scan before the driver
 * gets recreated. matrix_setup() turns it into the new driver's initial
 * prev_matrix_state, then clears it: an ordinary boot starts again from
 * "nothing pressed" as before. */
static uint8_t s_wake_state[MATRIX_ROWS][MATRIX_COLS];
/* Has the recreated driver emitted an event? It only signals CHANGES
 * relative to its own state, which starts from "nothing pressed": a key
 * captured on wake and then released before its first scan therefore
 * produces NO event, neither press nor release. This flag lets us
 * know. */
static volatile bool s_cb_since_setup;
static bool s_wake_had_keys;


static void keyboard_btn_cb(keyboard_btn_handle_t kbd_handle, keyboard_btn_report_t kbd_report, void *user_data)
{
    s_cb_since_setup = true;
    /* Build current matrix state from report */
    uint8_t new_state[MATRIX_ROWS][MATRIX_COLS];
    memset(new_state, 0, sizeof(new_state));
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS)
                new_state[in_idx][out_idx] = 1;
        }
    }

#if CONFIG_KASE_MATRIX_LOG_CONSOLE
    /* Bench log: each change spelled out on the console, before
     * prev_matrix_state gets overwritten by one path or the other. Does not
     * depend on CDC, unlike the test mode below. */
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            if (new_state[r][c] != prev_matrix_state[r][c])
                ESP_LOGW(TAG, "MTX r=%d c=%d %s", r, c,
                         new_state[r][c] ? "pressed" : "released");
#endif

#if CONFIG_KASE_HALF_LINK_TX
    /* Right half: emit the half-matrix on every change. Event-driven,
     * not periodic — that's premise §2.3 of the design, and the only one
     * that makes the R1 bet tenable. Task context (the callback comes from
     * the keyboard_button driver, not an ISR), so rf_driver_send can block
     * there for the duration of its retransmissions. */
    {
        bool change = false;
        for (int r = 0; r < MATRIX_ROWS && !change; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                if (new_state[r][c] != prev_matrix_state[r][c]) { change = true; break; }
        if (change) {
            uint8_t bm[RF_HALF_BITMAP_BYTES];
            rf_matrix_to_bitmap(&new_state[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
            half_link_tx_update(bm, true);
        }
    }
#endif

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, LEFT side: this half no longer runs the engine nor sends
     * finished HID (the dongle handles that) — it emits its RAW half-matrix
     * to the dongle, on change, the same way the right does towards the dongle.
     * KBD_WIRELESS identifies the left (the right is NIPHAR_SLAVE and will earn
     * its own path at the bench). The mirror is NOT applied here: each half
     * emits its physical coordinates, the dongle sorts them out (half_col_to_keymap). */
    {
        bool change = false;
        for (int r = 0; r < MATRIX_ROWS && !change; r++)
            for (int c = 0; c < MATRIX_COLS; c++)
                if (new_state[r][c] != prev_matrix_state[r][c]) { change = true; break; }
        /* Rule 3: we emit the raw data to the dongle ONLY outside USB mode.
         * If a USB host is plugged into the left, it's ITS engine that types
         * locally — feeding the dongle too would type twice. At rest/on
         * battery (route != USB), we emit, the dongle fuses and types. */
        bool usb = (kbd_active_route() == KBD_OUT_USB);
        if (change && fusion_left_emits_raw(usb)) {
            uint8_t bm[RF_HALF_BITMAP_BYTES];
            rf_matrix_to_bitmap(&new_state[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
            kbd_relay_send_matrix(RF_HALF_LEFT, bm);
        }
        /* Outside USB, the LOCAL engine is useless: it's the dongle that
         * types. Running it anyway (report, tap-hold, combos, muted HID)
         * cost time per keystroke for a discarded result. We stop here:
         * state stored, activity noted, nothing else. USB replugged -> the
         * route switches and the engine resumes on the next scan ("only load
         * the local keymap with USB", 2026-09-16 — it's the execution we're
         * gating, the code stays here). Matrix test mode keeps control. */
        if (!fusion_left_types_local(usb) && !matrix_test_mode) {
            memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
            last_activity_time_ms = esp_timer_get_time() / 1000;
            return;
        }
    }
#endif

    /* ── Test mode: send change events, skip HID ── */
    if (matrix_test_mode) {
        uint32_t now = esp_timer_get_time() / 1000;
        /* Auto-exit if no CDC activity or USB disconnected for > 30s */
        if (now - matrix_test_last_activity_ms > MATRIX_TEST_TIMEOUT_MS) {
            matrix_test_mode = false;
#if CONFIG_KASE_VEILLE
            veille_veto(VEILLE_VETO_TEST, false);
#endif
            ESP_LOGW(TAG, "matrix test mode timeout — auto-exit");
        } else {
            for (int r = 0; r < MATRIX_ROWS; r++) {
                for (int c = 0; c < MATRIX_COLS; c++) {
                    if (new_state[r][c] != prev_matrix_state[r][c]) {
                        uint8_t evt[3] = { (uint8_t)r, (uint8_t)c, new_state[r][c] };
                        matrix_test_last_activity_ms = now;
                        ks_respond(KS_CMD_MATRIX_TEST, KS_STATUS_OK, evt, 3);
                    }
                }
            }
            memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
            last_activity_time_ms = now;
            return;
        }
        /* Fallthrough to normal mode if just auto-exited */
    }

    /* ── Normal mode ── */
    memcpy(MATRIX_STATE, new_state, sizeof(MATRIX_STATE));

    /* ⚠ DO NOT reset keycodes[] to zero here. This array belongs to the
     * report producer: build_keycode_report() walks the six slots and
     * writes 0 into each of the empty ones
     * (key_processor.c, "Step 4"), so it fully determines it — clearing it
     * here brought nothing.
     *
     * But this callback runs in the keyboard_button driver task, at
     * priority 5, while vTaskKeyboard may be BETWEEN build_keycode_report()
     * and send_hid_key() — a window that contains all of
     * process_matrix_changes(). The reset then landed right before sending:
     * the report went out EMPTY, and the next cycle rebuilt from a state
     * where the key was already released. The press was never transmitted.
     *
     * Observed at the bench on 2026-09-08: on fast typing on the left
     * half, a key skipped. Matrix test mode (KS_CMD_MATRIX_TEST) showed
     * 45 events without a single gap — the scan saw everything, the loss
     * was here. */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i] = INVALID_KEY_POS;
        current_press_col[i] = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }

    uint8_t filled = 0;
    uint8_t new_keypresses = 0;
    if (kbd_report.key_pressed_num > 0 && kbd_report.key_data) {
        for (uint32_t i = 0; i < kbd_report.key_pressed_num && filled < MAX_REPORT_KEYS; i++) {
            uint8_t out_idx = kbd_report.key_data[i].output_index;
            uint8_t in_idx = kbd_report.key_data[i].input_index;
            if (in_idx < MATRIX_ROWS && out_idx < MATRIX_COLS) {
                if (!prev_matrix_state[in_idx][out_idx]) {
                    new_keypresses++;
                    key_stats_record_press(in_idx, out_idx);
                }
                current_press_row[filled] = in_idx;
                current_press_col[filled] = out_idx;
                current_press_stat[filled] = 1;
                filled++;
            }
        }
    }

    /* ⚠ The #if surrounds the WHOLE loop, braces included. Applying it
     * only to the body of a brace-less `for` would turn the next statement
     * into that body — here the memcpy of prev_matrix_state, which would
     * then stop being executed once a cycle counts no new keypress.
     * Observed at the bench on the right half: every release reported twice.
     *
     * The Niphargus right half scans without a screen (module not compiled):
     * the link failed on this one symbol alone. Same case as v2d_sleep.c. */
    /* ⚠ FUSION HAS NOTHING TO DO WITH THE SCREEN — never nest it back under
     * CONFIG_KASE_HAS_DISPLAY. It ended up nested there by accident on
     * 2026-09-07, while fixing the brace-less `for` trap above, and since
     * the left half has no screen, these two lines were NOT compiled:
     * s_filled_local stayed at zero, so matrix_apply_remote() restarted from
     * index 0 and OVERWROTE the local keys, while the local path never
     * called fusion and erased the remote ones. Each half typed alone, and
     * NO combination between the two ever went through — Shift on the left
     * + a letter on the right, in particular. */
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    s_filled_local = filled;   /* local/remote boundary, for fusion */
    matrix_apply_remote();
#endif

#if CONFIG_KASE_HAS_DISPLAY
    for (uint8_t k = 0; k < new_keypresses; k++)
        status_display_notify_keypress();
#else
    (void)new_keypresses;
#endif

    memcpy(prev_matrix_state, new_state, sizeof(prev_matrix_state));
    matrix_flag_signal(&stat_matrix_changed);
    last_activity_time_ms = esp_timer_get_time() / 1000;

    if (keyboard_task_handle != NULL)
        xTaskNotifyGive(keyboard_task_handle);
}

/* The power-saving driver (enable_power_save) SETS a gpio_hold_en on the
 * columns at rest and only lifts it at the start of its own scan;
 * keyboard_button_delete (gpio_reset_pin) does NOT lift it. Anything that
 * drives the columns outside the driver — wake capture, sleep arming,
 * recreation — must release them first, otherwise the seven columns stay
 * high together and a held key reads across its whole row. */
static void matrix_cols_unhold(void)
{
    const int cols[] = BOARD_COL_PINS;
    for (int c = 0; c < MATRIX_COLS; c++) gpio_hold_dis(cols[c]);
}

void rtc_matrix_deinit(void)
{
    ESP_LOGD(TAG, "rtc_matrix_deinit");
    if (s_kbd) {
        keyboard_button_delete(s_kbd);
        s_kbd = NULL;
    }
    matrix_cols_unhold();
}

/* ── Light-sleep matrix key-wake (V2D wireless) ──────────────────────────────
 * The scan driver drives COLS (outputs) and reads ROWS (inputs). For a static
 * wake, drive every COL HIGH and arm each ROW as a HIGH-level wake source: a
 * keypress ties a HIGH col to its row → row goes HIGH → wakes light-sleep.
 * gpio_sleep_sel_dis keeps this config live during sleep (else IDF isolates the
 * pads and no key can wake). Call rtc_matrix_deinit() first (driver released).
 * BENCH-TUNE: active level is HIGH here; flip if the matrix is active-low. */
void matrix_arm_key_wake(void)
{
    /* The board's own tables (BOARD_ROW_PINS/BOARD_COL_PINS, exactly
     * MATRIX_ROWS/MATRIX_COLS entries — test_board_pin_tables_match_the_geometry):
     * the core has no matrix shape of its own since 2026-09-20. */
    const int cols[] = BOARD_COL_PINS;
    const int rows[] = BOARD_ROW_PINS;

    for (int i = 0; i < MATRIX_COLS; i++) {
        gpio_set_direction(cols[i], GPIO_MODE_OUTPUT);
        gpio_set_level(cols[i], 1);
        gpio_sleep_sel_dis(cols[i]);
    }
    for (int i = 0; i < MATRIX_ROWS; i++) {
        gpio_set_direction(rows[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(rows[i], GPIO_PULLDOWN_ONLY);
        gpio_wakeup_enable(rows[i], GPIO_INTR_HIGH_LEVEL);
        gpio_sleep_sel_dis(rows[i]);
    }
    esp_sleep_enable_gpio_wakeup();
}

void matrix_disarm_key_wake(void)
{
    const int rows[] = BOARD_ROW_PINS;
    for (int i = 0; i < MATRIX_ROWS; i++) {
        gpio_wakeup_disable(rows[i]);
    }
    /* matrix_setup() re-takes the pins for the scan driver. */
}

void matrix_setup(void)
{
    /* ⚠ matrix_setup() is on the WAKE PATH. Each log line costs ~3.5 ms at
     * 115,200 baud, and the pinout table used to print fourteen of them: the
     * first scan only happened 60 ms after wake. A brief keystroke was
     * already released — the key that woke the board was lost. Observed at
     * the bench on 2026-09-11.
     *
     * The table is useful for bring-up, not on every wake: it moves to
     * ESP_LOGD, only one line stays at INFO. A doubt about the pinout is
     * resolved by raising the log level, not by slowing down every wake. */
    ESP_LOGI(TAG, "matrix_setup");
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(SLAVE_MATRIX_STATE, 0, sizeof(SLAVE_MATRIX_STATE));
    /* The driver is (re)created: the previous state no longer means
     * anything. Without this clearing, the first scan after a light sleep
     * wake compares against the state from BEFORE sleep and manufactures
     * phantom presses and releases. Restarting it from "nothing pressed"
     * instead makes the key that woke the board be seen as a fresh press,
     * which is exactly what we want. */
    s_cb_since_setup = false;
    memcpy(prev_matrix_state, s_wake_state, sizeof(prev_matrix_state));
    memset(s_wake_state, 0, sizeof(s_wake_state));
    /* If matrix_wake_capture() has just published a key, the driver's first
     * scan already finds it in prev: still held -> nothing new, no
     * duplicate; released -> a release, and the host lets it go. Without
     * this, a key captured then released before this scan would stay STUCK
     * on the host — the driver would never have seen either the press or
     * the release. */

    // Build gpio arrays from keyboard_config defines
    static int output_gpios[MATRIX_COLS];
    static int input_gpios[MATRIX_ROWS];
#if defined(BOARD_MATRIX_COL2ROW)
    const int cols_map[] = BOARD_COL_PINS;
    const int rows_map[] = BOARD_ROW_PINS;

    /* Reset all matrix GPIOs to detach any function set by ROM bootloader
       (UART0 on GPIO43/44, SPI on GPIO37, etc.) */
    matrix_cols_unhold();   /* a hold would survive gpio_reset_pin */
    for (int i = 0; i < MATRIX_COLS; i++) gpio_reset_pin(cols_map[i]);
    for (int i = 0; i < MATRIX_ROWS; i++) gpio_reset_pin(rows_map[i]);
#else
    const int cols_map[] = BOARD_COL_PINS;
    const int rows_map[] = BOARD_ROW_PINS;
#endif
    ESP_LOGD(TAG, "Cols (outputs): ");
    for (int i = 0; i < MATRIX_COLS; i++) {
        output_gpios[i] = cols_map[i];
        ESP_LOGD(TAG, "  COL%d = GPIO%d", i, output_gpios[i]);
    }
    ESP_LOGD(TAG, "Rows (inputs): ");
    for (int i = 0; i < MATRIX_ROWS; i++) {
        input_gpios[i] = rows_map[i];
        ESP_LOGD(TAG, "  ROW%d = GPIO%d", i, input_gpios[i]);
    }

    keyboard_btn_config_t cfg = {0};
    cfg.output_gpios = output_gpios;
    cfg.input_gpios = input_gpios;
    cfg.output_gpio_num = MATRIX_COLS;
    cfg.input_gpio_num = MATRIX_ROWS;
    cfg.active_level = 1; // Active HIGH 
    cfg.debounce_ticks = BOARD_DEBOUNCE_TICKS;
    cfg.ticks_interval = BOARD_MATRIX_SCAN_INTERVAL_US;
    /* Driver power saving: with no key pressed, the 1 ms gptimer STOPS, the
     * columns are held high, and an interrupt on the rows restarts it on the
     * first press. Without this the processor came out of idle 1000 times
     * per second at rest — each time relighting the PLL for 160 MHz — and
     * the DFS (power/pm_dfs.c) never really went down. The first scan
     * follows the press in < 1 ms, as before. */
    cfg.enable_power_save = true;
    cfg.priority = 5;
    cfg.core_id = 0;

    esp_err_t res = keyboard_button_create(&cfg, &s_kbd);
    if (res == ESP_OK && s_kbd != NULL) {
        ESP_LOGD(TAG, "keyboard_button created: handle=%p", s_kbd);
        
        // KBD_EVENT_PRESSED is called for all changes (press AND release)
        keyboard_btn_cb_config_t cb_pressed = {0};
        cb_pressed.event = KBD_EVENT_PRESSED;
        cb_pressed.callback = keyboard_btn_cb;
        cb_pressed.user_data = NULL;
        esp_err_t r1 = keyboard_button_register_cb(s_kbd, cb_pressed, NULL);
        
        if (r1 == ESP_OK) {
            ESP_LOGD(TAG, "keyboard_button callback registered");
        } else {
            ESP_LOGW(TAG, "keyboard_button_register_cb failed: %d", r1);
        }
    } else {
        ESP_LOGW(TAG, "keyboard_button_create failed: %d", res);
    }
}

void layer_changed(void)
{
    is_layer_changed = 1;
}

uint32_t get_last_activity_time_ms(void)
{
    return last_activity_time_ms;
}

/* A wake IS activity. Without this bump, the keyboard loop re-reads a 60 s
 * inactivity right on the next round — the recreated driver hasn't scanned
 * yet — and sends the board back to sleep 10 ms after its wake. The key
 * still pressed wakes it right back up, and the cycle restarts, 80 ms per
 * round, until a scan lands inside the window. Observed at the bench on
 * 2026-09-11: "very slow before being able to type", and keystrokes lost if
 * released too early. v2d_sleep.c did this bump, it had been lost. */
/* Capture the key that woke the board — BEFORE recreating the driver.
 *
 * The GPIO wake only happens because a key is pressed AT THIS INSTANT:
 * it's the most reliable information we'll get. Recreating the driver,
 * waiting for its first scan and its debounce, relighting the radio, takes
 * ~90 ms — a brief keystroke is released before that, and the key that
 * woke the keyboard was LOST. Observed at the bench on 2026-09-11, and
 * removing the logs from the path hadn't been enough.
 *
 * Here we scan once by hand, in a few tens of microseconds, and publish
 * exactly what the callback would have published. The duration of the wake
 * path stops mattering.
 *
 * Preconditions: wake configuration in place (matrix_arm_key_wake) —
 * columns as outputs, rows as inputs with pull-down. We lower all columns,
 * raise them one by one, read the rows, restore. Same electrical chain as
 * the scan: COL -> switch -> diode -> ROW. */
void matrix_wake_capture(void)
{
    const int cols[] = BOARD_COL_PINS;
    const int rows[] = BOARD_ROW_PINS;
    /* TWO scans, we only keep what holds on both. The GPIO wake triggers on
     * a simple edge: a glitching line (capacitive coupling from neighboring
     * columns held high, ESD, a line at threshold) wakes the board and
     * reads "pressed" on a single scan. It does not survive two reads 1 ms
     * apart; a real keypress does.
     *
     * Without this filter, every phantom wake (cause=7, ~3 per 10 min
     * measured on 2026-09-12) did two kinds of damage: it bumped 60 s of
     * activity — radio on, 0.2 V lost over a night — AND the capture
     * PUBLISHED the key, which went on to type a stray character to the dongle. */
    uint8_t st[MATRIX_ROWS][MATRIX_COLS];
    uint8_t st2[MATRIX_ROWS][MATRIX_COLS];
    memset(st, 0, sizeof(st));
    memset(st2, 0, sizeof(st2));
    matrix_cols_unhold();   /* the destroyed driver may have left its holds */

    for (int pass = 0; pass < 2; pass++) {
        uint8_t (*dst)[MATRIX_COLS] = (pass == 0) ? st : st2;
        for (int c = 0; c < MATRIX_COLS; c++) gpio_set_level(cols[c], 0);
        for (int c = 0; c < MATRIX_COLS; c++) {
            gpio_set_level(cols[c], 1);
            esp_rom_delay_us(20);             /* RC of the 100 Ohm series + capacitance */
            for (int r = 0; r < MATRIX_ROWS; r++)
                dst[r][c] = (uint8_t)gpio_get_level(rows[r]);
            gpio_set_level(cols[c], 0);
        }
        if (pass == 0) esp_rom_delay_us(1000);   /* 1 ms between the two passes */
    }
    for (int c = 0; c < MATRIX_COLS; c++) gpio_set_level(cols[c], 1);

    /* AND of the two passes: a transient phantom drops out, a real press stays. */
    uint8_t st1[MATRIX_ROWS][MATRIX_COLS];
    memcpy(st1, st, sizeof(st1));   /* raw pass 1, for the diagnostic below */
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS; c++)
            st[r][c] = st[r][c] && st2[r][c];

    /* Publish like the callback: local report, boundary, fusion, flag. */
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    uint8_t filled = 0;
    for (int r = 0; r < MATRIX_ROWS; r++)
        for (int c = 0; c < MATRIX_COLS && filled < MAX_REPORT_KEYS; c++)
            if (st[r][c]) {
                current_press_row[filled]  = (uint8_t)r;
                current_press_col[filled]  = (uint8_t)c;
                current_press_stat[filled] = 1;
                filled++;
            }
    memcpy(MATRIX_STATE, st, sizeof(MATRIX_STATE));
    memcpy(s_wake_state, st, sizeof(s_wake_state));   /* for matrix_setup */
    s_wake_had_keys = (filled != 0);
    memcpy(prev_matrix_state, st, sizeof(prev_matrix_state));
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    s_filled_local = filled;
    matrix_apply_remote();
#endif
#if CONFIG_KASE_HALF_LINK_TX
    /* The right half has no keyboard task: it's the callback that emits.
     * So here we emit what it would have emitted. */
    if (filled) {   /* phantom wake (nothing pressed): mute, nothing to report */
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        rf_matrix_to_bitmap(&st[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
        half_link_tx_update(bm, true);
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, LEFT side: the same gap as the right, closed just above. The
     * left's only raw emitter is the callback on CHANGE — but the capture
     * sets prev_matrix_state = st, so the recreated scanner sees the key
     * held WITHOUT a change and emits nothing; only the release used to go
     * out. The first key after light sleep was swallowed (bench 2026-09-13;
     * the veille.c sequence used to emit on the pre-fusion path (removed on
     * 2026-09-18), compiled out here). So we emit the captured press right
     * away, outside USB (rule 3), with the same emitter as the callback —
     * the 100 ms reaffirmation then takes over as long as the key is held. */
    if (filled && fusion_left_emits_raw(kbd_active_route() == KBD_OUT_USB)) {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        rf_matrix_to_bitmap(&st[0][0], MATRIX_ROWS, MATRIX_COLS, bm);
        kbd_relay_send_matrix(RF_HALF_LEFT, bm);
    }
#endif
    if (filled) {
        matrix_flag_signal(&stat_matrix_changed);
        if (keyboard_task_handle != NULL) xTaskNotifyGive(keyboard_task_handle);
        /* A KEY is activity. A wake without a key — a glitch on a line, TRRS
         * cable coupling, noise — is NOT: bumping it would buy 60 s of radio
         * on for every parasite. One night on 2026-09-12: 1.2 h awake out of
         * 7 h for ~70 phantom wakes, 0.2 V lost. Without a bump, the loop
         * re-reads an old inactivity and sends it back to sleep in ~15 ms —
         * exactly what a glitch deserves. A real key that the capture would
         * have missed would be seen by the driver within those 15 ms and
         * would get bumped by the callback. */
        last_activity_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    /* One line per wake: what the capture found. It's this line that
     * proved, on 2026-09-11, that the left did see the wake key. */
    ESP_LOGI(TAG, "wake: %u key(s) captured", filled);
#if CONFIG_KASE_VEILLE_DIAG
    if (filled == 0) {
        /* Empty capture on a GPIO wake: report what EACH pass read, to
         * distinguish a bounce (pass 1 full, pass 2 empty or vice versa)
         * from a pre-contact or a phantom (both empty). Bench 2026-09-16:
         * "wake key lost on the left, since forever". */
        char l1[MATRIX_ROWS * MATRIX_COLS + 1], l2[MATRIX_ROWS * MATRIX_COLS + 1];
        int k = 0;
        for (int r = 0; r < MATRIX_ROWS; r++)
            for (int c = 0; c < MATRIX_COLS; c++, k++) { l1[k] = st1[r][c] ? '1' : '.'; l2[k] = st2[r][c] ? '1' : '.'; }
        l1[k] = l2[k] = '\0';
        ESP_LOGW(TAG, "  empty capture: pass1=%s pass2=%s", l1, l2);
    }
#endif
    for (uint8_t i = 0; i < filled; i++)
        ESP_LOGI(TAG, "  (%u,%u)", current_press_row[i], current_press_col[i]);
}

/* To be called ~10 ms after matrix_setup(), once the driver has had time
 * to do its first scan and its debounce.
 *
 * If the wake capture had found a key and the driver has signaled NOTHING
 * since, the key was released between the two: the driver, starting from
 * "nothing pressed", saw "nothing pressed" and kept quiet. Without this
 * reconciliation the key would stay in the report until the next event
 * from THIS half — keystrokes from the other half do not dislodge it,
 * fusion preserves local keys. A Super thumb key captured on wake would
 * then turn all of the right's typing into shortcuts. Observed at the
 * bench on 2026-09-11.
 *
 * Returns true if a release was published: the caller must then
 * emit it. */
bool matrix_wake_had_keys(void) { return s_wake_had_keys; }

void matrix_wake_wait_first_scan(void)
{
    /* CONDITIONAL wait, not a guessed delay: we exit as soon as the driver
     * has called back (held key confirmed — the capture already emitted it,
     * nothing to do), otherwise we wait the grace deduced from its debounce
     * before concluding a release. vTaskDelay(1) in the loop hands control
     * back to the driver (prio 5 task) without starving it; time is measured with esp_timer. */
    const uint32_t grace = wake_grace_ms(BOARD_DEBOUNCE_TICKS, BOARD_MATRIX_SCAN_INTERVAL_US);
    const uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (!s_cb_since_setup &&
           (uint32_t)((uint32_t)(esp_timer_get_time() / 1000) - t0) < grace)
        vTaskDelay(1);
}

bool matrix_wake_reconcile(void)
{
    if (!s_wake_had_keys || s_cb_since_setup) return false;
    s_wake_had_keys = false;
    for (int i = 0; i < MAX_REPORT_KEYS; i++) {
        current_press_row[i]  = INVALID_KEY_POS;
        current_press_col[i]  = INVALID_KEY_POS;
        current_press_stat[i] = 0;
    }
    memset(MATRIX_STATE, 0, sizeof(MATRIX_STATE));
    memset(prev_matrix_state, 0, sizeof(prev_matrix_state));
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    s_filled_local = 0;
    matrix_apply_remote();
#endif
#if CONFIG_KASE_HALF_LINK_TX
    {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        memset(bm, 0, sizeof(bm));
        half_link_tx_update(bm, true);
    }
#endif
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
    /* Fusion, left: the wake key was emitted at capture time; released
     * before the first scan, the callback will never say so — we emit the
     * release, otherwise it stays stuck at the dongle. */
    if (fusion_left_emits_raw(kbd_active_route() == KBD_OUT_USB)) {
        uint8_t bm[RF_HALF_BITMAP_BYTES];
        memset(bm, 0, sizeof(bm));
        kbd_relay_send_matrix(RF_HALF_LEFT, bm);
    }
#endif
    matrix_flag_signal(&stat_matrix_changed);
    ESP_LOGW(TAG, "wake: key released before the first scan, release published");
    return true;
}

void matrix_mark_activity(void)
{
    last_activity_time_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

