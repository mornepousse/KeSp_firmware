/*
 * test_rf_pairing.c — Host tests for the pure per-set derivations (Plan RF-1).
 *
 * Contract under test (spec §2.2, §2.3, §2.4, §2.5, §10.1):
 *   - rf_derive_addr(set_id, slot, ...) → {'K','S',hi,lo}, suffix=slot,
 *     channel = 80 + 2*(set_id%20) for slot 0x01, +1 for slot 0x02.
 *   - rf_derive_addr returns false for set_id 0 and 0xFFFF (sentinel) and the
 *     caller's factory cfg is left UNCHANGED (backwards-compat invariant).
 *   - rf_derive_wifi_ch → {1,6,11}[set_id%3]; sentinel → 6.
 *   - crc16_ccitt pinned to a known vector.
 *
 * Run: cd test/build && cmake .. && make && ./test_runner
 */
#include "test_framework.h"
#include "rf_pairing.h"

/* Mirror of the firmware factory defaults (board_rf.h / board.h) used to prove
 * the set_id=0 fallback leaves a cfg byte-for-byte at factory values. */
typedef struct {
    uint8_t rx_addr[4];
    uint8_t addr_suffix;
    uint8_t channel;
} fake_cfg_t;

/* Local re-implementation of the rf_apply_set_id no-op contract using the pure
 * rf_derive_addr (the firmware rf_apply_set_id wraps exactly this). */
static void apply(fake_cfg_t *c, uint16_t set_id, uint8_t slot)
{
    uint8_t addr[4], suffix, ch;
    if (!rf_derive_addr(set_id, slot, addr, &suffix, &ch)) return; /* sentinel no-op */
    c->rx_addr[0] = addr[0]; c->rx_addr[1] = addr[1];
    c->rx_addr[2] = addr[2]; c->rx_addr[3] = addr[3];
    c->addr_suffix = suffix;
    c->channel = ch;
}

void test_rf_pairing(void)
{
    TEST_SUITE("rf_pairing");

    /* ── Case 1: set_id=0 → no-op; factory LEFT cfg unchanged ───── */
    fake_cfg_t l = { {'K','a','S','e'}, 0x01, 0x4C };
    apply(&l, 0x0000, 0x01);
    TEST_ASSERT_EQ(l.rx_addr[0], 'K',  "set_id0 L: rx_addr[0] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[1], 'a',  "set_id0 L: rx_addr[1] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[2], 'S',  "set_id0 L: rx_addr[2] unchanged");
    TEST_ASSERT_EQ(l.rx_addr[3], 'e',  "set_id0 L: rx_addr[3] unchanged");
    TEST_ASSERT_EQ(l.addr_suffix, 0x01, "set_id0 L: suffix stays 0x01");
    TEST_ASSERT_EQ(l.channel, 0x4C,    "set_id0 L: channel stays 76");

    /* ── Case 2: set_id=0 → no-op; factory RIGHT cfg unchanged ──── */
    fake_cfg_t r = { {'K','a','S','e'}, 0x02, 0x52 };
    apply(&r, 0x0000, 0x02);
    TEST_ASSERT_EQ(r.rx_addr[3], 'e',  "set_id0 R: rx_addr[3] unchanged");
    TEST_ASSERT_EQ(r.addr_suffix, 0x02, "set_id0 R: suffix stays 0x02");
    TEST_ASSERT_EQ(r.channel, 0x52,    "set_id0 R: channel stays 82");

    /* ── Case 3: set_id=0xFFFF → also a no-op (same sentinel) ───── */
    fake_cfg_t s = { {'K','a','S','e'}, 0x01, 0x4C };
    apply(&s, 0xFFFF, 0x01);
    TEST_ASSERT_EQ(s.channel, 0x4C,    "set_id 0xFFFF: channel unchanged");
    TEST_ASSERT_EQ(s.addr_suffix, 0x01, "set_id 0xFFFF: suffix unchanged");

    /* ── Case 4: known set_id 0x1234 LEFT → addr + channel 80 ──── */
    uint8_t addr[4], suf, ch;
    bool ok = rf_derive_addr(0x1234, 0x01, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "0x1234 L: derive returns true");
    TEST_ASSERT_EQ(addr[0], 0x4B,      "0x1234 L: addr[0]='K'");
    TEST_ASSERT_EQ(addr[1], 0x53,      "0x1234 L: addr[1]='S'");
    TEST_ASSERT_EQ(addr[2], 0x12,      "0x1234 L: addr[2]=set_id hi");
    TEST_ASSERT_EQ(addr[3], 0x34,      "0x1234 L: addr[3]=set_id lo");
    TEST_ASSERT_EQ(suf, 0x01,          "0x1234 L: suffix=0x01");
    TEST_ASSERT_EQ(ch, 80,             "0x1234 L: channel=80+2*(4660%20)=80");

    /* ── Case 5: known set_id 0x1234 RIGHT → channel base+1 ─────── */
    ok = rf_derive_addr(0x1234, 0x02, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "0x1234 R: derive returns true");
    TEST_ASSERT_EQ(suf, 0x02,          "0x1234 R: suffix=0x02");
    TEST_ASSERT_EQ(ch, 81,             "0x1234 R: channel=81");

    /* ── Case 6: set_id%20==19 → base_ch=118, R=119 ≤ 125 ──────── */
    ok = rf_derive_addr(0x0013 /*19*/, 0x02, addr, &suf, &ch);
    TEST_ASSERT(ok,                    "id19 R: derive returns true");
    TEST_ASSERT_EQ(ch, 119,            "id19 R: channel=118+1=119");
    TEST_ASSERT(ch <= 125,             "id19 R: channel within NRF24 max 125");

    /* ── Case 7: set_id%20==0 (id=20) → base_ch=80 ─────────────── */
    ok = rf_derive_addr(0x0014 /*20*/, 0x01, addr, &suf, &ch);
    TEST_ASSERT_EQ(ch, 80,             "id20 L: channel=80");

    /* ── Case 8: WiFi channel derivation {1,6,11}[id%3] ────────── */
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0003), 1,  "wifi: id%3==0 → 1");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0001), 6,  "wifi: id%3==1 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0002), 11, "wifi: id%3==2 → 11");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x1234), 6,  "wifi: 4660%3==1 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0x0000), 6,  "wifi: sentinel 0 → 6");
    TEST_ASSERT_EQ(rf_derive_wifi_ch(0xFFFF), 6,  "wifi: sentinel 0xFFFF → 6");

    /* ── Case 9: crc16_ccitt pinned vector ("123456789" → 0x29B1) ─ */
    /* Standard CRC-16/CCITT-FALSE check value per the algorithm catalogue. */
    const uint8_t check[9] = {'1','2','3','4','5','6','7','8','9'};
    TEST_ASSERT_EQ(crc16_ccitt(check, 9), 0x29B1, "crc16: check vector 0x29B1");

    /* ── Case 10: crc16 of a sample 6-byte MAC is deterministic ── */
    const uint8_t mac[6] = {0x24,0x6F,0x28,0x01,0x02,0x03};
    uint16_t a = crc16_ccitt(mac, 6);
    uint16_t b = crc16_ccitt(mac, 6);
    TEST_ASSERT_EQ(a, b,               "crc16: deterministic for same MAC");
}
