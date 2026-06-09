#include "test_framework.h"
#include "otp_proto.h"
#include "cr_crc16.h"
#include <string.h>

static uint8_t  g_fake_resp[20];
static int      g_confirm = 0;     /* 0 pending, 1 authorized */
static uint8_t  g_armed_slot = 0xFF;

static bool fake_hmac(uint8_t slot, const uint8_t ch[64], uint8_t out[20])
{ (void)slot; (void)ch; memcpy(out, g_fake_resp, 20); return true; }
static int  fake_confirm(void) { return g_confirm; }
static void fake_arm(uint8_t slot) { g_armed_slot = slot; }

static void feed_frame(uint8_t slot, const uint8_t challenge[64])
{
    uint8_t frame[OTP_FRAME_SIZE];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, challenge, 64);
    frame[64] = slot;
    uint16_t crc = cr_crc16(frame, 65);          /* CRC over payload+slot */
    frame[65] = (uint8_t)(crc & 0xFF);
    frame[66] = (uint8_t)(crc >> 8);
    /* 70 bytes -> 10 reports of 7 data bytes + seq/flag */
    for (uint8_t seq = 0; seq < 10; seq++) {
        uint8_t r[OTP_FEATURE_RPT_SIZE] = {0};
        memcpy(r, &frame[seq * 7], (seq < 9) ? 7 : (70 - 9 * 7));
        r[7] = (seq & 0x1F) | OTP_SLOT_WRITE_FLAG;
        otp_proto_on_write(r);
    }
}

static void test_full_cr_with_confirm(void)
{
    otp_proto_hooks_t h = { fake_hmac, fake_confirm, fake_arm };
    otp_proto_init(&h);
    otp_proto_reset();
    for (int i = 0; i < 20; i++) g_fake_resp[i] = (uint8_t)(0xA0 + i);
    g_confirm = 0; g_armed_slot = 0xFF;

    uint8_t challenge[64];
    for (int i = 0; i < 64; i++) challenge[i] = (uint8_t)i;
    feed_frame(OTP_SLOT_CHAL_HMAC1, challenge);
    TEST_ASSERT_EQ(g_armed_slot, OTP_SLOT_CHAL_HMAC1, "valid frame armed confirm with slot");

    uint8_t r[OTP_FEATURE_RPT_SIZE];
    otp_proto_on_read(r);
    TEST_ASSERT(r[7] & OTP_RESP_TIMEOUT_WAIT_FLAG, "pending confirm -> RESP_TIMEOUT_WAIT");

    g_confirm = 1;  /* user pressed K_SEC_CONFIRM */
    /* read 3 response reports */
    uint8_t got[21]; int n = 0;
    for (int k = 0; k < 3; k++) {
        otp_proto_on_read(r);
        TEST_ASSERT(r[7] & OTP_RESP_PENDING_FLAG, "authorized -> RESP_PENDING");
        memcpy(&got[n], r, 7); n += 7;
    }
    TEST_ASSERT(memcmp(got, g_fake_resp, 20) == 0, "response bytes match HMAC output");
}

static void test_bad_crc_rejected(void)
{
    otp_proto_hooks_t h = { fake_hmac, fake_confirm, fake_arm };
    otp_proto_init(&h);
    otp_proto_reset();
    g_armed_slot = 0xFF;
    uint8_t challenge[64] = {0};
    /* Feed a frame then corrupt: feed_frame computes valid CRC; instead feed manually with bad CRC */
    uint8_t frame[OTP_FRAME_SIZE]; memset(frame, 0, sizeof(frame));
    memcpy(frame, challenge, 64); frame[64] = OTP_SLOT_CHAL_HMAC1;
    frame[65] = 0xDE; frame[66] = 0xAD;           /* wrong CRC */
    for (uint8_t seq = 0; seq < 10; seq++) {
        uint8_t r[OTP_FEATURE_RPT_SIZE] = {0};
        memcpy(r, &frame[seq * 7], (seq < 9) ? 7 : 7);
        r[7] = (seq & 0x1F) | OTP_SLOT_WRITE_FLAG;
        otp_proto_on_write(r);
    }
    TEST_ASSERT_EQ(g_armed_slot, 0xFF, "bad-CRC frame did NOT arm confirm");
}

void test_otp_proto(void)
{
    TEST_SUITE("otp_proto state machine");
    TEST_RUN(test_full_cr_with_confirm);
    TEST_RUN(test_bad_crc_rejected);
}
