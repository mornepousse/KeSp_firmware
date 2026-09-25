/* Keyboard task: main coordinator loop.
   Waits for matrix ISR notification, delegates to key_processor and hid_report. */
#include "keyboard_task.h"
#include "keyboard_cadence.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* TEST veto (matrix test mode) */
#endif
#include "tinyusb.h"
#include "key_processor.h"
#include "hid_report.h"
#include "keyboard_actions.h"
#include "matrix_scan.h"
#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
#include "kbd_relay_tx.h"   /* kbd_relay_remote_changed (fusion 4b) */
#endif
#include "matrix_flag.h"   /* test-and-clear du drapeau — audit F3 */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "key_features.h"
#include "keymap.h"
#include "matrix_scan.h"
#include "hid_transport.h"
#include "usb_hid.h"        /* usb_try_remote_wakeup */
#include "usb_presence.h"   /* usb_presence_cable (cadence); kbd_active_route under KBD_WIRELESS */
#if CONFIG_KASE_KBD_WIRELESS
#if CONFIG_KASE_HAS_DISPLAY && !CONFIG_KASE_VEILLE
#include "v2d_sleep.h"      /* see the CMakeLists guard: depends on the screen, excluded with B7 */
#endif
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "KB_TASK";

TaskHandle_t keyboard_task_handle = NULL;
uint8_t usb_bl_state = 0;

/* Send a keycode as a quick press+release tap */
static void send_tap(uint8_t kc, uint8_t mod)
{
    uint8_t buf[6] = { kc };
    hid_send_keyboard(mod, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(buf, 0, 6);
    hid_send_keyboard(0, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
}

void vTaskKeyboard(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        if (keyboard_task_handle == NULL)
            keyboard_task_handle = xTaskGetCurrentTaskHandle();

        /* 10 ms only while a timer IS running (tap-hold, tap-dance, leader,
         * macro, test mode, USB), 100 ms otherwise: see keyboard_cadence.h. A
         * matrix change notifies the task, the first keystroke doesn't
         * wait. */
        {
            extern volatile bool matrix_test_mode;
            bool minuteur = tap_hold_pending() || tap_dance_pending() || leader_is_active()
                         || key_processor_has_pending_macro();   /* the tick's four consumers */
            uint32_t attente = kbd_cadence_attente_ms(usb_presence_cable(), matrix_test_mode,   /* same USB rule as routing, link, sleep */
                                                      minuteur);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(attente));
        }

        /* Tick timers — even without matrix change */
        tap_hold_tick();
        tap_dance_tick();

#if CONFIG_KASE_DONGLE_FUSION && CONFIG_KASE_KBD_WIRELESS
        /* Fusion, left over USB (phase 2 4b): the right is re-transmitted by the
         * dongle and received by kbd_relay listening over USB. Same logic as the
         * pre-fusion master — on remote change, re-merge and type locally. */
        if (kbd_relay_remote_changed()) {
            matrix_apply_remote();
            build_keycode_report();
            send_hid_key();
        }
#endif

        /* Matrix test mode auto-exit (runs even if no key pressed) */
        extern volatile bool matrix_test_mode;
        extern volatile uint32_t matrix_test_last_activity_ms;
        if (matrix_test_mode) {
            uint32_t now = esp_timer_get_time() / 1000;
            if (now - matrix_test_last_activity_ms > 30000) {
                matrix_test_mode = false;
#if CONFIG_KASE_VEILLE
                veille_veto(VEILLE_VETO_TEST, false);
#endif
                ESP_LOGW(TAG, "matrix test mode timeout — auto-exit");
            }
        }

        /* Hold just activated → rebuild and send */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Tap dance resolved → send tap */
        if (tap_dance_just_resolved()) {
            uint8_t td_kc = tap_dance_consume();
            if (td_kc != 0)
                send_tap(td_kc, 0);
        }

        /* Leader timeout check + result */
        if (leader_tick()) {
            uint8_t mod = 0;
            uint8_t kc = leader_consume(&mod);
            if (kc != 0)
                send_tap(kc, mod);
        }

        /* Macro sequence pending → play */
        if (key_processor_has_pending_macro()) {
            int16_t idx = key_processor_consume_macro();
            if (idx >= 0 && idx < MAX_MACROS) {
                const macro_step_t *steps = macros_list[idx].steps;

                /* Sequential playback (only macros with delays reach here).
                   Fold modifier keycodes (0xE0-0xE7) into the next step's mod
                   so [LSFT, ;] plays as Shift+; instead of two separate taps. */
                uint8_t pending_mod = 0;
                for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
                    uint8_t kc  = steps[s].keycode;
                    uint8_t mod = steps[s].modifier;

                    if (kc == MACRO_DELAY_MARKER) {
                        vTaskDelay(pdMS_TO_TICKS(mod * 10));
                        continue;
                    }
                    if (kc >= 0xE0 && kc <= 0xE7) {
                        pending_mod |= (1 << (kc - 0xE0));
                        continue;
                    }
                    send_tap(kc, mod | pending_mod);
                    pending_mod = 0;
                }
            }
        }

        /* Matrix changed → full processing cycle.
         *
         * The flag is consumed BEFORE the state is read (matrix_flag_take
         * does the test and the clear together). The code used to clear it after, so
         * that a scan callback landing during build_keycode_report() would see its
         * edge overwritten and lost — audit F3. See matrix_flag.h for the
         * full reasoning. */
        if (matrix_flag_take(&stat_matrix_changed)) {
            /* A keypress while the USB host is suspended (PC asleep, cable in) →
             * remote-wakeup so the key wakes the PC. Not gated on KBD_WIRELESS:
             * a wired keyboard is precisely the case where the user expects a
             * keypress to wake the machine. No-op when not suspended. */
            usb_try_remote_wakeup();
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

        /* Sleep (B7) is no longer evaluated here: power/veille_task.c, a single
         * task shared by both halves, with vetos (USB, link, sync, test) and hooks. */

#if CONFIG_KASE_KBD_WIRELESS && CONFIG_KASE_HAS_DISPLAY && !CONFIG_KASE_VEILLE
        /* RF-mode idle → light-sleep (USB stays awake). v2d_sleep_enter() blocks
         * until a keypress wakes it, then restores everything.
         * Screen required: v2d_sleep_enter() turns off the OLED. ⚠ Excluded from
         * boards with B7 sleep (Niphargus): two sleeps in the same loop used to
         * step on each other and swallow the wakeup keystroke (2026-09-16). */
        {
            uint32_t idle = (uint32_t)(esp_timer_get_time() / 1000)
                          - get_last_activity_time_ms();
            if (v2d_should_sleep(kbd_active_route() == KBD_OUT_RF, idle, 60000)
                && !usb_sleep_blocked())   /* never sleep while USB is present */
                v2d_sleep_enter();
        }
#endif
    }
}

void keyboard_manager_init(void)
{
    ESP_LOGI(TAG, "Keyboard manager initialized");
    tap_hold_init();
    tap_dance_init();
    combo_init();
    leader_init();
    key_override_init();
    hid_report_init();
    keyboard_worker_init();
}
