/* main/security/ccid.h — minimal TinyUSB CCID class driver (dongle only, Phase 0)
 *
 * Carries APDUs between the host (scdaemon/gpg) and the OpenPGP applet
 * (openpgp_card.c) over a USB CCID smartcard interface.
 *
 * The driver is registered with TinyUSB via the weak usbd_app_driver_get_cb()
 * hook, which is implemented in ccid.c. TinyUSB picks it up at
 * tinyusb_driver_install() time and invokes the driver init() callback, which
 * wires the OpenPGP applet (openpgp_card_init).
 *
 * Compiled for the dongle role only (see main/CMakeLists.txt).
 */
#pragma once

/* ccid_init() — call once from force-linked USB init (kase_tinyusb_init).
 *
 * The weak usbd_app_driver_get_cb() override only takes effect if ccid.c.obj is
 * actually linked. Since the object exports no other referenced symbol, the
 * linker would otherwise drop it and the empty weak default in libtinyusb.a
 * would win — no CCID app driver registered, and SET_CONFIGURATION asserts
 * because no driver claims the CCID interface. Calling ccid_init() forces the
 * object into the link. See the comment on ccid_init() in ccid.c. Do not
 * remove the call. */
void ccid_init(void);
