/* HID transport abstraction: USB/BLE routing.
 * usb_bl_state is the user-controlled mode (0=USB, 1=BLE), persisted via
 * save_io_mode in NVS. No per-report fallback: a press going to one transport
 * while the release goes to another would leave the host with a stuck key. */
#include "hid_transport.h"
#include "keyboard_task.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "hid_bluetooth_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us — tick-independent short wait */
#include <string.h>

static const char *TAG_HTX = "HID_TX";

#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

/* ── USB endpoint arbitration (audit F1) ─────────────────────────────────────
 * The HID IN endpoint is an interrupt EP polled every 1 ms (see usb_hid.c), and
 * the sender task drains its queue in a tight loop: a burst of >= 2 reports
 * finds the EP still busy on the second one. Dropping it there loses whatever
 * that report carried — and when it is the "everything released" report, the
 * host keeps the modifier down forever (stuck Super after a shortcut), because
 * nothing ever retransmits it.
 *
 * So: wait for the endpoint instead of dropping, and tell the caller whether the
 * report actually left. A few ms is invisible to a typist and bounded, so a dead
 * host (unplugged, suspended) cannot wedge the sender.
 *
 * The wait is a short busy-poll, not vTaskDelay: the FreeRTOS tick is 10 ms
 * (CONFIG_FREERTOS_HZ=100) so pdMS_TO_TICKS(1) rounds to 0 and a tick-based wait
 * would either not wait at all or cost a full 10 ms per burst. The USB task runs
 * at a higher priority than the sender, so it still preempts this poll and
 * services the endpoint.
 *
 * The mutex serialises the TinyUSB calls: hid_send_* is reached both from the
 * sender task and directly from the keyboard task (send_tap for tap-dance /
 * leader / macros), and the TinyUSB device API is not reentrant. */
#define USB_HID_TX_WAIT_US 2500   /* > 2 full-speed frames (1 ms each) */

static void usb_resume_if_suspended(void);   /* défini plus bas */
#define USB_HID_TX_POLL_US 100

static SemaphoreHandle_t s_usb_tx_mutex = NULL;

void hid_transport_init(void)
{
    if (s_usb_tx_mutex == NULL) s_usb_tx_mutex = xSemaphoreCreateMutex();
}

static inline bool usb_tx_lock(void)
{
    if (s_usb_tx_mutex == NULL) return true;   /* pre-init: single caller anyway */
    return xSemaphoreTake(s_usb_tx_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static inline void usb_tx_unlock(void)
{
    if (s_usb_tx_mutex != NULL) xSemaphoreGive(s_usb_tx_mutex);
}

/* Wait until the HID endpoint can take a report, or give up after
 * USB_HID_TX_WAIT_US. Must be called with the tx mutex held. */
static bool usb_hid_wait_ready(void)
{
    for (int waited = 0; waited < USB_HID_TX_WAIT_US; waited += USB_HID_TX_POLL_US) {
        if (tud_hid_ready()) return true;   /* EP frees on the next SOF (<= 1 ms) */
        esp_rom_delay_us(USB_HID_TX_POLL_US);
    }
    return tud_hid_ready();
}

static bool send_usb_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                              uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    usb_resume_if_suspended();
    if (!usb_tx_lock()) return false;
    bool ok = usb_hid_wait_ready();
    if (ok) {
        ok = tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kb);
        if ((buttons || x || y || wheel) && usb_hid_wait_ready())
            tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
    }
    usb_tx_unlock();
    if (!ok) ESP_LOGW(TAG_HTX, "kb+mouse report not sent (EP busy %d us)", USB_HID_TX_WAIT_US);
    return ok;
}

/* Réveiller l'hôte si le bus est suspendu, et attendre qu'il reprenne.
 *
 * Après une minute sans trafic HID, l'hôte suspend le bus USB. Le rapport
 * suivant tombait alors sur un point d'accès muet : usb_hid_wait_ready()
 * expirait au bout de 2,5 ms et le rapport était JETÉ avec un simple
 * avertissement. La tentative échouée, ou la souris, finissait par réveiller
 * l'hôte — et les touches suivantes passaient. Constaté au banc le
 * 2026-09-11 sur le dongle : « la première touche est perdue » après chaque
 * veille du clavier, quel que soit ce que faisait la moitié gauche.
 *
 * usb_try_remote_wakeup() existait, mais n'était appelé que par la tâche
 * clavier — un clavier USB direct. Le dongle relaie par rf_rx_task et n'y
 * passait jamais. usb_hid.c raconte qu'un V2D filaire avait déjà perdu cette
 * fonction de la même façon : la troisième fois, elle vit au seul endroit
 * que tous les chemins traversent, l'émission elle-même.
 *
 * Une reprise USB dure au moins 20 ms (resume signaling, USB 2.0 §7.1.7.7) :
 * on attend jusqu'à 100 ms que tud_suspended() retombe. tud_remote_wakeup()
 * ne fait rien si l'hôte n'a pas autorisé le réveil distant — la chaîne
 * échoue alors comme avant, mais on aura essayé. */
static void usb_resume_if_suspended(void)
{
    if (!tud_mounted() || !tud_suspended()) return;
    tud_remote_wakeup();
    for (int i = 0; i < 10 && tud_suspended(); i++)
        vTaskDelay(1);   /* 1 tick = 10 ms (CONFIG_FREERTOS_HZ=100) */
}

static bool send_usb_keyboard(uint8_t modifier, const uint8_t kb[6])
{
    usb_resume_if_suspended();
    if (!usb_tx_lock()) return false;
    bool ok = usb_hid_wait_ready() &&
              tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifier, kb);
    usb_tx_unlock();
    if (!ok) ESP_LOGW(TAG_HTX, "kb report not sent (EP busy %d us)", USB_HID_TX_WAIT_US);
    return ok;
}

static bool send_usb_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    usb_resume_if_suspended();
    if (!usb_tx_lock()) return false;
    bool ok = usb_hid_wait_ready() &&
              tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, 0);
    usb_tx_unlock();
    return ok;
}

static inline bool bt_ready(void)
{
    return hid_bluetooth_is_initialized() && hid_bluetooth_is_connected();
}

bool hid_send_kb_mouse(uint8_t modifier, const uint8_t kb[6],
                       uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (usb_bl_state == 0) {
        return send_usb_kb_mouse(modifier, kb, buttons, x, y, wheel);
    } else if (bt_ready()) {
        send_hid_bl_key(modifier, kb);
        send_hid_bl_mouse(buttons, x, y, wheel);
        return true;
    }
    return false;   /* BLE selected but not connected: the report is lost */
}

bool hid_send_keyboard(uint8_t modifier, const uint8_t kb[6])
{
    if (usb_bl_state == 0) {
        return send_usb_keyboard(modifier, kb);
    } else if (bt_ready()) {
        send_hid_bl_key(modifier, kb);
        return true;
    }
    return false;
}

bool hid_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    if (usb_bl_state == 0) {
        return send_usb_mouse(buttons, x, y, wheel);
    } else if (bt_ready()) {
        send_hid_bl_mouse(buttons, x, y, wheel);
        return true;
    }
    return false;
}
