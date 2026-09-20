/* HID relay for the Conchodytes mouse to dongle slot 2.
 *
 * The protocol needs no inventing: `rf_encode_hidreport_mouse()` already
 * exists in rf_packet.h, the dongle already decodes it (rf_rx_task.c) and
 * calls `hid_send_mouse()`. On link loss it releases ONLY the buttons, never
 * the keypress in progress (rf_slot.h). This module only transmits.
 *
 * ⚠ Why a file separate from kbd_relay_tx.c, which already carries a
 * `kbd_relay_send_mouse()`: that one is welded to the keyboard — it polls
 * `kbd_active_route()`, calls `usb_presence_poll()` and periodically
 * refreshes the last keyboard report. A mouse has none of that, and its
 * movement is RELATIVE, hence not idempotent: re-emitting it in a loop
 * would make the cursor drift. What is genuinely common — the radio driver
 * and pairing — is reused, not copied.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initializes the radio in PTX and restores pairing from NVS.
 *
 * ⚠ Does NOT initialize the SPI bus: the sensor already did so in
 * `pmw3389_init()`, which runs first. The bus is shared, and two calls to
 * `spi_bus_initialize` cannot coexist. Order matters, therefore, and
 * `mouse_task_start()` guarantees it.
 *
 * Returns ESP_OK even if the board is not paired — pairing is a separate
 * step, see `mouse_relay_pair()`. Returns an error only if the radio
 * itself does not respond. */
esp_err_t mouse_relay_init(void);

/* true when the radio responds AND pairing is loaded. False as long as the
 * mouse has not been paired: reports are then simply dropped. */
bool mouse_relay_active(void);

/* Sends a mouse HID report (6 bytes, PKT_TYPE_HIDREPORT / RF_HID_SUB_MOUSE).
 *
 * ⚠ x, y and wheel are `int8_t`: ±127 per report. Measured at the bench on
 * 2026-08-25, the sensor produces up to 5373 counts over 200 ms — about 27
 * per millisecond. At an 8 ms cadence that would be 215, a clear overflow;
 * at 1 ms it fits. **The choice of cadence and encoding is the same
 * choice**, and it is not settled: see the spec
 * docs/superpowers/specs/2026-08-25-conchodytes-firmware-design.md §7.
 * The caller is responsible for clamping or accumulation.
 *
 * ⚠ RETURNS the radio acknowledgment (TX_DS), AND THE CALLER MUST CHECK IT.
 * Without retransmission (see mouse_relay_init), an unacknowledged frame is
 * LOST. Since the movement is RELATIVE, dropping it erases that bit of the
 * gesture: the cursor travels less than the hand. That said, the frame must
 * not be re-emitted as-is — replaying a relative delta advances it twice —
 * but the counts must be put back into the accumulator, so the next frame
 * carries the sum. This is correct by construction: a sum of movements is a movement. */
bool mouse_relay_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

/* Transmission counters since startup: frames sent, and among them those
 * ACKNOWLEDGED at the radio level (nRF24 TX_DS).
 *
 * An acknowledgment says someone is listening on this address and channel —
 * not that the dongle understood the frame. But its ABSENCE is unambiguous:
 * nobody is out there. That is the difference between "the dongle isn't
 * decoding" and "the mouse is talking into the void", and without this
 * counter that distinction cannot be made. `rf_driver_send()` already
 * returns the information; it was simply being discarded. */
void mouse_relay_stats(uint32_t *envoyes, uint32_t *acquittes);

/* Starts the pairing exchange: sends PKT_PAIR_REQ on the rendezvous
 * (channel 0x28, address "KSPR\xFF") declaring slot 0x02 and type
 * RF_DEV_MOUSE, then waits for the dongle's PKT_PAIR_ACK.
 *
 * The dongle's pairing window must be OPEN — CDC command
 * KS_CMD_RF_PAIR_START (0xB2). Without it the dongle ignores requests.
 *
 * On success, stores the dongle's set_id / slot / MAC in NVS and returns
 * ESP_OK; pairing only takes effect on the next restart, as for the
 * keyboard halves. */
esp_err_t mouse_relay_pair(void);
