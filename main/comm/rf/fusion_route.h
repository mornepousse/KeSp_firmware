#pragma once
#include <stdbool.h>

/* Fusion routing — which engine is active (rule 3 of the two-engine design).
 *
 * Only one engine runs at a time, chosen by the presence of a USB host on the
 * LEFT half:
 *   - USB present → the left half is on mains power, it can listen to the right half,
 *     runs ITS OWN engine and outputs HID via its own USB; the dongle stays quiet
 *     and merely re-emits the right half's half-matrix to the left half.
 *   - no USB → the left half emits its RAW matrix to the dongle (it no longer listens,
 *     autonomy mode); the dongle merges and types.
 *
 * These predicates are the single source of "who does what". They take a
 * boolean (USB present?) rather than the ESP route type, to stay host-testable
 * and decoupled from usb_presence.h. The caller converts
 * (kbd_active_route()==KBD_OUT_USB).
 *
 * Tested host-side (test/test_fusion_route.c), central invariant included:
 * the left half and the dongle never type at the same time.
 * Design: docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */

/* LEFT side. */
static inline bool fusion_left_types_local(bool usb_present) { return usb_present; }
static inline bool fusion_left_emits_raw(bool usb_present)   { return !usb_present; }

/* DONGLE side, depending on the left half's USB mode (which it announces to it —
 * RF announcement to come; by default the left half is assumed wireless and the dongle types). */
static inline bool fusion_dongle_types(bool left_usb_present)   { return !left_usb_present; }
static inline bool fusion_dongle_reemits(bool left_usb_present) { return left_usb_present; }
