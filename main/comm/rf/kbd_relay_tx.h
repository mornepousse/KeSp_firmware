#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * kbd_relay_tx.h — Smart keyboard NRF24 HID relay (PTX to dongle).
 *
 * When CONFIG_KASE_KBD_WIRELESS=y, a standalone keyboard relays its final HID
 * reports over NRF24 to the M.2 dongle (PKT_TYPE_HIDREPORT packets) instead of
 * delivering them locally over USB/BLE.
 *
 * This module owns the NRF radio handle (PTX), the pairing NVS state, and the
 * two low-level TX helpers called by hid_report.c under the CONFIG guard.
 */

/* ── Répétition bornée du dernier rapport (logique pure, testée host) ──────
 *
 * Le chemin HIDREPORT n'a pas de réconciliation : un key-up perdu sur un lien
 * radio bruité laisserait une touche coincée. Le relais réémet donc le dernier
 * rapport après chaque changement — mais un nombre BORNÉ de fois, puis se tait.
 *
 * L'implémentation d'origine réémettait indéfiniment dès la première frappe de
 * la vie de la carte (`s_have_last` n'était jamais remis à faux). Mesuré au
 * banc le 2026-09-05 : 100 paquets/s clavier au repos. Deux conséquences —
 * la moitié gauche du Niphargus, sourde pendant qu'elle émet, l'était en
 * permanence, ce qui ruine le pari R1 du design ; et le budget < 50 µA de B7
 * devenait inatteignable.
 *
 * Pure, donc host-testable (test/test_kbd_refresh.c), sur le modèle de
 * kbd_route_target/vbus_debounce_step dans comm/usb/usb_presence.h. */
typedef struct {
    uint8_t left;          /* répétitions restantes */
} kbd_refresh_t;

/* Changement d'état HID : réarme le compteur de répétitions. */
static inline void kbd_refresh_arm(kbd_refresh_t *r, uint8_t repeats)
{
    r->left = repeats;
}

/* Un tick du timer de rafraîchissement : faut-il réémettre maintenant ?
 * Consomme une répétition quand la réponse est oui. */
static inline bool kbd_refresh_step(kbd_refresh_t *r)
{
    if (r->left == 0) return false;
    r->left--;
    return true;
}

/* Init NRF radio in PTX mode and restore (or discover) the dongle pairing from
 * NVS, declaring device type RF_DEV_SMART_KBD. Sets the internal s_paired flag.
 * Safe to call even if the board has no NRF hardware — the flag stays false. */
void kbd_relay_init(void);

/* Returns true when wireless mode is active AND the radio is paired to a dongle.
 * False ⇒ hid_report.c falls through to the local USB/BLE path. */
bool kbd_relay_active(void);

/* Encode + transmit a keyboard HID report (PKT_TYPE_HIDREPORT / RF_HID_SUB_KBD).
 * modifier: standard HID modifier byte. kb[6]: keycodes (modifiers already
 * extracted by hid_report.c's extract_modifiers before this is called). */
void kbd_relay_send_kbd(uint8_t modifier, const uint8_t kb[6]);

/* Encode + transmit a mouse HID report (PKT_TYPE_HIDREPORT / RF_HID_SUB_MOUSE). */
void kbd_relay_send_mouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);

/* Fusion (KASE_DONGLE_FUSION) : émettre la demi-matrice BRUTE au dongle
 * (PKT_TYPE_MATRIX), au lieu du HID fini. `half` = RF_HALF_LEFT/RIGHT ;
 * `bitmap` = RF_HALF_BITMAP_BYTES octets. Passe par le même chemin d'émission
 * que kbd_relay_send_kbd (excursion si la radio écoute, sinon direct). */
void kbd_relay_send_matrix(uint8_t half, const uint8_t *bitmap);

/* Fusion phase 2 (4b) : état de la demi-matrice DISTANTE (droite) réémise par le
 * dongle et reçue en écoute USB. Le moteur de la gauche les lit pour fusionner la
 * droite dans les colonnes hautes (matrix_apply_remote). Miroir de
 * half_link_remote_pressed/changed. */
bool kbd_relay_remote_pressed(uint8_t row, uint8_t col);
bool kbd_relay_remote_changed(void);

/* Light-sleep hooks: stop the refresh timer + power the NRF down (holding the TX
 * mutex) before sleep; power up + restart the timer on wake. */
void kbd_relay_sleep_prepare(void);
void kbd_relay_wake_restore(void);
