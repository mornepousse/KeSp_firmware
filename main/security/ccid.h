/* main/security/ccid.h — minimal TinyUSB CCID class driver (dongle only, Phase 0)
 *
 * Carries APDUs between the host (scdaemon/gpg) and the OpenPGP applet
 * (openpgp_card.c) over a USB CCID smartcard interface.
 *
 * The driver is registered with TinyUSB via the weak usbd_app_driver_get_cb()
 * hook, which is implemented in ccid.c. No explicit registration call is
 * needed; TinyUSB picks it up at tinyusb_driver_install() time and invokes the
 * driver init() callback, which wires the OpenPGP applet (openpgp_card_init).
 *
 * Compiled for the dongle role only (see main/CMakeLists.txt).
 */
#pragma once

/* No public surface: the driver self-registers through usbd_app_driver_get_cb()
 * and initialises the applet from its own init() callback. This header exists
 * so the source is discoverable and to document the registration mechanism. */
