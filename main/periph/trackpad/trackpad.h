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

/* Pause/restart the trackpad RDY ISR + task so it neither draws nor wakes the
 * CPU during half light-sleep. */
void trackpad_suspend(void);
void trackpad_resume(void);

/* ── Gesture state held across calls to trackpad_map ────────────────
 * The caller (trackpad_task / dongle RX) owns one instance and resets it to
 * zeros at startup. The map function mutates it as gestures progress. */
typedef struct {
    bool    pending_release;   /* true → emit a button-release packet next call */
    bool    drag_active;       /* true → left button held due to press-and-hold */
    uint8_t peak_fingers;      /* highest n_fingers seen in current touch session */
} trackpad_state_t;

/* ── HID mouse output from trackpad_map ─────────────────────────────
 * Produced by the dongle (approach B) from raw NRF fields. */
typedef struct {
    int8_t  dx, dy;
    uint8_t buttons;     /* 0x01 L, 0x02 R, 0x04 M */
    int8_t  scroll_v, scroll_h;
} trackpad_out_t;

/* ── Acceleration / sensitivity config for trackpad_map ─────────────
 * Task 1 default: base=100, accel=0, gain_max=100 → gain 1.0 (behaviour-preserving).
 * Task 2 will tune the curve; Task 3 will expose it via CDC/NVS. */
#define TRACKPAD_CFG_FMT   0x01
typedef struct {
    uint8_t  fmt;
    uint16_t base;       /* gain ×100 (100 = 1.00x) */
    uint16_t accel;
    uint16_t gain_max;
} trackpad_cfg_t;
#define TRACKPAD_ACCEL_DEN   100

/* ── Pure gesture-to-HID mapping function — host-testable ──────────
 *
 * Maps raw IQS5xx fields to trackpad_out_t output fields.
 * No I/O, no FreeRTOS calls, no global state reads.
 *
 * Supported gestures (v2):
 *   - 1-finger cursor             → dx, dy (accel curve applied)
 *   - 1-finger single tap         → left click pulse  (button 0x01)
 *   - 2-finger tap                → right click pulse (button 0x02)
 *   - 3-finger tap                → middle click pulse (button 0x04)
 *     (detected via peak_fingers tracked across the touch session)
 *   - 2-finger swipe (Scroll evt) → scroll_v / scroll_h
 *   - Press-and-hold              → hold left button while moving;
 *     released when fingers leave the surface.
 *
 * Parameters:
 *   ge0              GestureEvents0 byte (data[0] from 9-byte read block)
 *   ge1              GestureEvents1 byte (data[1])
 *   n_fingers        NumberOfFingers byte (data[4])
 *   rel_x            RelativeX signed 16-bit (data[5..6] big-endian decoded)
 *   rel_y            RelativeY signed 16-bit (data[7..8] big-endian decoded)
 *   cfg              Acceleration/sensitivity config (neutral default → gain 1.0)
 *   state            In/out gesture state (see trackpad_state_t)
 *   out              Output: filled with dx, dy, buttons, scroll_v, scroll_h.
 *
 * Returns true if a packet should be sent (activity gate passed).
 * Returns false if all output fields are zero and no button event — caller drops.
 */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  const trackpad_cfg_t *cfg,
                  trackpad_state_t *state, trackpad_out_t *out);
