#include "otp_proto.h"
#include "cr_crc16.h"
#include <string.h>

typedef enum {
    ST_IDLE = 0,
    ST_WAIT,
    ST_RESP
} otp_state_t;

static const otp_proto_hooks_t *s_hooks;
static otp_state_t  s_state;
static uint8_t      s_frame[OTP_FRAME_SIZE];
static uint8_t      s_challenge[64];
static uint8_t      s_resp[OTP_RESP_LEN];
static uint8_t      s_slot;
static uint8_t      s_resp_seq;

void otp_proto_init(const otp_proto_hooks_t *hooks)
{
    s_hooks = hooks;
    otp_proto_reset();
}

void otp_proto_reset(void)
{
    s_state    = ST_IDLE;
    s_resp_seq = 0;
    memset(s_frame, 0, sizeof(s_frame));
}

void otp_proto_on_write(const uint8_t report[OTP_FEATURE_RPT_SIZE])
{
    /* Only accept new writes when idle (not awaiting confirm or sending resp) */
    if (s_state != ST_IDLE) return;

    uint8_t byte7 = report[7];
    if (!(byte7 & OTP_SLOT_WRITE_FLAG)) return;

    uint8_t seq = byte7 & 0x1F;
    if (seq >= 10) return;

    /* seq=0 starts a fresh frame */
    if (seq == 0) memset(s_frame, 0, sizeof(s_frame));

    memcpy(&s_frame[seq * 7], report, 7);

    if (seq == 9) {
        /* All 10 reports received — validate CRC over payload+slot (65 bytes) */
        uint16_t computed = cr_crc16(s_frame, 65);
        uint16_t stored   = (uint16_t)s_frame[65] | ((uint16_t)s_frame[66] << 8);
        if (computed != stored) {
            otp_proto_reset();
            return;
        }
        uint8_t s = s_frame[64];
        if (s != OTP_SLOT_CHAL_HMAC1 && s != OTP_SLOT_CHAL_HMAC2) {
            otp_proto_reset();
            return;
        }
        memcpy(s_challenge, s_frame, 64);
        s_slot = s;
        s_hooks->confirm_arm(s_slot);
        s_state = ST_WAIT;
    }
}

void otp_proto_on_read(uint8_t report[OTP_FEATURE_RPT_SIZE])
{
    memset(report, 0, OTP_FEATURE_RPT_SIZE);

    if (s_state == ST_WAIT) {
        int cs = s_hooks->confirm_state();
        if (cs == 2) {
            /* Denied / timed out */
            otp_proto_reset();
            return;  /* report stays all-zeros */
        } else if (cs == 1) {
            /* Authorized: compute HMAC, fall through to ST_RESP handling */
            s_hooks->compute_hmac(s_slot, s_challenge, s_resp);
            s_resp_seq = 0;
            s_state    = ST_RESP;
        } else {
            /* Still waiting for physical confirm */
            report[7] = OTP_RESP_TIMEOUT_WAIT_FLAG;
            return;
        }
    }

    if (s_state == ST_RESP) {
        uint8_t offset = s_resp_seq * 7;
        /* Reports 0+1: 7 bytes each; report 2: remaining 6 bytes (resp[14..19]) */
        uint8_t bytes = (s_resp_seq < 2) ? 7 : (OTP_RESP_LEN - 14);
        memcpy(report, &s_resp[offset], bytes);
        report[7] = OTP_RESP_PENDING_FLAG | s_resp_seq;
        s_resp_seq++;
        if (s_resp_seq >= 3) otp_proto_reset();
    }
}
