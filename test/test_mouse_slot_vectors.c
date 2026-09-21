/* The mouse-slot contract, byte for byte — docs/DONGLE_MOUSE_CONTRACT.md.
 *
 * A firmware that shares no code with this one (the Conchodytes rewrite) is
 * built against docs/contracts/mouse_slot_vectors.json. Those bytes come from
 * the encoders below; this test pins them so that a change to a wire byte, a
 * channel or a timing cannot slip through without touching the document, the
 * generator and this file in the same commit. */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"
#include "../main/comm/rf/rf_pairing.h"
#include "../main/comm/rf/rf_slot.h"

static bool bytes_eq(const uint8_t *got, unsigned n, const uint8_t *exp, unsigned m)
{
    if (n != m) return false;
    return memcmp(got, exp, n) == 0;
}

static void test_hid_report_bytes(void)
{
    uint8_t b[32];
    static const uint8_t v1[] = { 0x50, 0x01, 0x01, 0x05, 0xfd, 0x00 };
    static const uint8_t v2[] = { 0x50, 0x01, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t v3[] = { 0x50, 0x01, 0x07, 0x7f, 0x80, 0x01 };
    unsigned n = rf_encode_hidreport_mouse(b, 0x01, 5, -3, 0);
    TEST_ASSERT(bytes_eq(b, n, v1, sizeof v1), "HID mouse: left, dx+5, dy-3 = 50 01 01 05 fd 00");
    n = rf_encode_hidreport_mouse(b, 0x00, 0, 0, 0);
    TEST_ASSERT(bytes_eq(b, n, v2, sizeof v2), "HID mouse: idle = 50 01 00 00 00 00");
    n = rf_encode_hidreport_mouse(b, 0x07, 127, -128, 1);
    TEST_ASSERT(bytes_eq(b, n, v3, sizeof v3), "HID mouse: all buttons, dx127, dy-128, wheel1");
}

static void test_pairing_bytes(void)
{
    uint8_t b[32];
    static const uint8_t mac[6] = { 0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33 };
    static const uint8_t req[] = { 0xf0, 0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33, 0x02, 0x02 };
    unsigned n = rf_encode_pair_req2(b, mac, 0x02, RF_DEV_MOUSE);
    TEST_ASSERT(bytes_eq(b, n, req, sizeof req), "PAIR_REQ v2: f0 mac slot=02 devtype=02");
    TEST_ASSERT_EQ(RF_DEV_MOUSE, 2, "RF_DEV_MOUSE is 2 on the wire");

    rf_pair_ack_t a = { .set_id = 0xBEEF, .slot = 0x02 };
    static const uint8_t dm[6] = { 0xac, 0xa7, 0x04, 0x18, 0x82, 0x24 };
    memcpy(a.dongle_wifi_mac, dm, 6);
    static const uint8_t ack[] = { 0xe0, 0xbe, 0xef, 0xac, 0xa7, 0x04, 0x18, 0x82, 0x24, 0x02 };
    n = rf_encode_pair_ack(b, &a);
    TEST_ASSERT(bytes_eq(b, n, ack, sizeof ack), "PAIR_ACK: e0 set_id(BE) dongle_mac slot");

    /* The mouse decodes what the dongle sends: same bytes, back to fields. */
    rf_pair_ack_t d;
    TEST_ASSERT(rf_decode_pair_ack(ack, sizeof ack, &d), "PAIR_ACK decodes");
    TEST_ASSERT_EQ(d.set_id, 0xBEEF, "set_id is big-endian on the wire");
    TEST_ASSERT_EQ(d.slot, 0x02, "slot 2");
}

static void test_addresses_and_channels(void)
{
    static const uint8_t dm[6] = { 0xac, 0xa7, 0x04, 0x18, 0x82, 0x24 };   /* bench dongle */
    TEST_ASSERT_EQ(crc16_ccitt((const uint8_t *)"123456789", 9), 0x29B1, "CRC-16/CCITT-FALSE check value");
    uint16_t id = crc16_ccitt(dm, 6);
    TEST_ASSERT_EQ(id, 0x9044, "set_id of the bench dongle");
    uint8_t addr[4], suf, ch;
    TEST_ASSERT(rf_derive_addr(id, 0x02, addr, &suf, &ch), "derivation applies to a real set_id");
    TEST_ASSERT_EQ(addr[0], 'K', "derived address byte 0");
    TEST_ASSERT_EQ(addr[1], 'S', "derived address byte 1");
    TEST_ASSERT_EQ(addr[2], 0x90, "derived address byte 2 = set_id >> 8");
    TEST_ASSERT_EQ(addr[3], 0x44, "derived address byte 3 = set_id & 0xff");
    TEST_ASSERT_EQ(suf, 0x02, "5th byte = slot");
    TEST_ASSERT_EQ(ch, 0x69, "mouse channel = 80 + 2*(set_id %% 20) + 1");
    TEST_ASSERT(!rf_derive_addr(0x0000, 0x02, addr, &suf, &ch), "0x0000 is the unpaired sentinel");
    TEST_ASSERT(!rf_derive_addr(0xFFFF, 0x02, addr, &suf, &ch), "0xFFFF is the unpaired sentinel");

    static const uint8_t pair_addr[5] = RF_PAIR_ADDR;
    static const uint8_t pair_exp[5] = { 0x4b, 0x53, 0x50, 0x52, 0xff };
    TEST_ASSERT(memcmp(pair_addr, pair_exp, 5) == 0, "rendezvous address KSPR\\xff");
    TEST_ASSERT_EQ(RF_PAIR_CHANNEL, 0x28, "rendezvous channel 0x28");
    TEST_ASSERT_EQ(RF_CH_MOUSE_DONGLE, 0x52, "factory mouse channel 0x52");
}

static void test_timings(void)
{
    TEST_ASSERT_EQ(RF_STATUS_PERIOD_MS, 1000u, "keep-alive period while a button is held");
    TEST_ASSERT_EQ(RF_REARM_SILENCE_MS, 2000u, "dongle re-arms its radio after 2 s of silence");
    TEST_ASSERT_EQ(RF_LINK_LOST_MS, 2500u, "dongle releases the buttons after 2.5 s of silence");
    /* The safe action of the mouse slot is a zeroed mouse report, never a
     * keyboard release: losing the mouse must not wipe a keystroke. */
    rf_slot_link_t l = { .last_rx_ms = 1000, .up = true };
    TEST_ASSERT_EQ(rf_slot_link_check(&l, RF_SLOT_MOUSE, 1000 + RF_LINK_LOST_MS, RF_LINK_LOST_MS),
                   RF_SAFE_RELEASE_BUTTONS, "mouse slot lost → release buttons");
}

void test_mouse_slot_vectors(void)
{
    printf("\n-- Mouse slot contract (docs/DONGLE_MOUSE_CONTRACT.md) --\n");
    test_hid_report_bytes();
    test_pairing_bytes();
    test_addresses_and_channels();
    test_timings();
}
