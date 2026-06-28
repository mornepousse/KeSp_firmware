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

/* Light-sleep hooks: stop the refresh timer + power the NRF down (holding the TX
 * mutex) before sleep; power up + restart the timer on wake. */
void kbd_relay_sleep_prepare(void);
void kbd_relay_wake_restore(void);
