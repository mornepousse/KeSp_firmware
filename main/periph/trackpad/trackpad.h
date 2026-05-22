#pragma once

/*
 * trackpad.h — IQS5xx trackpad API for KaSe half firmware.
 *
 * Hardware: TPS43-201A-S (Azoteq IQS5xx-compatible) on I2C.
 * GPIO assignments from board.h:
 *   SDA=BOARD_TRACK_SDA_GPIO (GPIO40), SCL=BOARD_TRACK_SCL_GPIO (GPIO38)
 *   RST=BOARD_TRACK_RST_GPIO (GPIO13), RDY=BOARD_TRACK_RDY_GPIO (GPIO14)
 *
 * Both halves compile this module (PCB is reversible — presence detected at runtime
 * by I2C ACK probe). Only call trackpad_start() if trackpad_init() returns true.
 */

#include <stdbool.h>
#include <stdint.h>
#include "rf_packet.h"    /* rf_trackpad_t */

/* Initialize trackpad hardware:
 *   - I2C master init on SDA=GPIO40, SCL=GPIO38 at 400 kHz
 *   - Pulse RST=GPIO13 low 10 ms then high; settle 50 ms
 *   - Configure RDY=GPIO14 as input with ISR on NEGEDGE (→ binary semaphore)
 *   - I2C ACK probe at BOARD_TRACK_I2C_ADDR
 * Returns true if trackpad is physically present (ACK received).
 * Returns false if no ACK (trackpad not mounted on this half).
 * Not available in TEST_HOST builds — guarded in trackpad.c. */
bool trackpad_init(void);

/* Start the trackpad FreeRTOS task (prio 6, stack 3072, core 0).
 * Only call if trackpad_init() returned true.
 * The task waits on the RDY semaphore, reads IQS5xx touch data, and
 * transmits PKT_TRACKPAD over NRF24 via half_spi_lock + rf_driver_send.
 * Not available in TEST_HOST builds — guarded in trackpad.c. */
void trackpad_start(void);

/* ── Pure gesture-to-HID mapping function — host-testable ──────────
 *
 * Maps raw IQS5xx fields to rf_trackpad_t output fields.
 * No I/O, no FreeRTOS calls, no global state reads.
 *
 * Parameters:
 *   ge0              GestureEvents0 byte (data[0] from 9-byte read block)
 *   ge1              GestureEvents1 byte (data[1])
 *   n_fingers        NumberOfFingers byte (data[4]) — unused in v1 logic
 *   rel_x            RelativeX signed 16-bit (data[5..6] big-endian decoded)
 *   rel_y            RelativeY signed 16-bit (data[7..8] big-endian decoded)
 *   pending_release_io  In/out: current state of the tap-release state machine.
 *                       Set to true by the function when a tap press is emitted.
 *                       Cleared to false when the release packet is emitted.
 *                       The caller (trackpad_task) holds this as a static bool.
 *   out              Output: filled with dx, dy, buttons, scroll_v, scroll_h.
 *                    seq is NOT set here — caller sets out->seq = s_seq++ after return.
 *                    scroll_h is always set to 0 (out of v1 scope).
 *
 * Returns true if a packet should be sent (activity gate passed).
 * Returns false if all output fields are zero and no button event — caller drops.
 *
 * Precedence: scroll gesture overrides cursor movement.
 * Tap detection: buttons=0x01 on tap event; pending_release_io set to true.
 * Release: buttons=0x00 on next call when pending_release_io is true; force-send.
 */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  bool *pending_release_io, rf_trackpad_t *out);
