/* main/security/otp_hid.h — YubiKey OTP HID transport (dongle only).
 *
 * Wires otp_proto to the real sec_store/sec_confirm/cr_hmac hooks,
 * and provides the USB descriptor additions used by usb_hid.c.
 */
#pragma once

/* Initialize: injects real hooks into otp_proto.
 * Call once at dongle init, after sec_store_init(). */
void otp_hid_init(void);
