/* Keyboard task: main coordinator loop.
   Waits for matrix ISR notification, delegates to key_processor and hid_report. */
#include "keyboard_task.h"
#if CONFIG_KASE_VEILLE
#include "veille.h"
#if CONFIG_KASE_LINK_WIRE
#include "link_uart.h"
#endif
#include "tinyusb.h"
#endif
#include "key_processor.h"
#include "hid_report.h"
#include "keyboard_actions.h"
#include "matrix_scan.h"
#if CONFIG_KASE_HALF_LINK_RX
#include "half_link.h"
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
#if CONFIG_KASE_KBD_WIRELESS
#include "usb_presence.h"   /* kbd_active_route */
#if CONFIG_KASE_HAS_DISPLAY
#include "v2d_sleep.h"      /* voir la garde du CMakeLists : depend de l'ecran */
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

        /* 10ms loop for responsive tap/hold and tap dance timing */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        /* Tick timers — even without matrix change */
        tap_hold_tick();
        tap_dance_tick();

#if CONFIG_KASE_HALF_LINK_RX
        /* Fusion de la moitié distante. On n'agit QUE si l'état distant a
         * changé : le chemin local a sa propre émission, et rebâtir le rapport
         * à chaque cycle écrasait la frappe de la moitié gauche.
         *
         * Le callback de scan ne tourne que sur activité locale — un appui sur
         * la seule moitié droite n'en produit aucune — d'où cet appel ici. */
        if (half_link_remote_changed()) {
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
         * Le drapeau est consommé AVANT la lecture de l'état (matrix_flag_take
         * fait le test et l'effacement ensemble). Le code effaçait après, si bien
         * qu'un callback de scan tombant pendant build_keycode_report() voyait son
         * front écrasé et perdu — audit F3. Voir matrix_flag.h pour le
         * raisonnement complet. */
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

#if CONFIG_KASE_VEILLE
        /* Veille hybride (B7). Le seuil léger est court, le profond se compte
         * en heures : à 240 µA l'étage léger coûte 1 mAh sur quatre heures,
         * donc autant le tenir longtemps et éviter le redémarrage de 683 ms.
         * Bloquée tant que l'USB est énuméré — la carte est alors alimentée et
         * l'hôte attend un clavier. */
        {
            uint32_t inactif = (uint32_t)(esp_timer_get_time() / 1000)
                             - get_last_activity_time_ms();
            /* tud_ready(), PAS tud_mounted() : sur l'ESP32-S3, mounted reste vrai
             * après un débranchement à chaud — aucun événement de déconnexion —
             * et la veille restait bloquée jusqu'au prochain redémarrage. ready
             * retombe dès que le bus se suspend (~3 ms après le débranchement),
             * c'est le signal qu'utilise déjà le routage USB/RF. Contrepartie
             * assumée : un hôte qui s'endort câble branché laisse aussi le
             * clavier dormir ; il se ré-énumère au réveil. Constaté au banc le
             * 2026-09-11 : sept minutes sur batterie sans jamais dormir. */
            bool usb  = tud_ready();
            bool lien = false;
#if CONFIG_KASE_LINK_WIRE
            /* Une moitié en charge par le TRRS reste éveillée : endormie, elle
             * cesserait de répondre aux sondes et le pair rouvrirait son 5 V. */
            lien = link_uart_active();
#endif
            veille_diag(inactif, usb, lien);
            veille_pas(inactif, usb || lien);
        }
#endif

#if CONFIG_KASE_KBD_WIRELESS && CONFIG_KASE_HAS_DISPLAY
        /* RF-mode idle → light-sleep (USB stays awake). v2d_sleep_enter() blocks
         * until a keypress wakes it, then restores everything.
         * Ecran obligatoire : v2d_sleep_enter() eteint l'OLED. Une moitie
         * Niphargus n'a pas d'ecran et aura son propre sommeil en B7. */
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
