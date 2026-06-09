#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Status byte flags (YubiKey OTP HID protocol) */
#define OTP_SLOT_WRITE_FLAG        0x80
#define OTP_RESP_PENDING_FLAG      0x40
#define OTP_RESP_TIMEOUT_WAIT_FLAG 0x20

#define OTP_FEATURE_RPT_SIZE 8     /* 7 data + 1 seq/flag */
#define OTP_FRAME_SIZE       70    /* 64 payload + slot + crc16 + 3 filler */
#define OTP_SLOT_CHAL_HMAC1  0x30
#define OTP_SLOT_CHAL_HMAC2  0x38
#define OTP_RESP_LEN         20    /* HMAC-SHA1 digest */

/* Injected dependencies (keeps otp_proto pure + host-testable):
 *  - compute_hmac(slot, challenge[64], out20) -> true if a configured slot
 *    answered (false = no such slot). On target: looks up sec_store +
 *    cr_hmac_sha1. In tests: a fake.
 *  - confirm_state() -> 0 = no confirm yet (keep waiting), 1 = authorized,
 *    2 = denied/timeout. On target: drives sec_confirm. In tests: a fake. */
typedef struct {
    bool (*compute_hmac)(uint8_t slot, const uint8_t challenge[64], uint8_t out20[20]);
    int  (*confirm_state)(void);
    void (*confirm_arm)(uint8_t slot);   /* called when a valid challenge arrives */
} otp_proto_hooks_t;

void otp_proto_init(const otp_proto_hooks_t *hooks);
void otp_proto_reset(void);
/* Host wrote one 8-byte feature report (SET_REPORT). */
void otp_proto_on_write(const uint8_t report[OTP_FEATURE_RPT_SIZE]);
/* Host reads one 8-byte feature report (GET_REPORT): fill it, advancing state. */
void otp_proto_on_read(uint8_t report[OTP_FEATURE_RPT_SIZE]);
