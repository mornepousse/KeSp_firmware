/*
 * test_espnow_peer_filter.c — Host tests for espnow_link is_known_peer() predicate.
 *
 * Contract:
 *   espnow_set_peers(macs, count): register up to 2 peer MACs.
 *   is_known_peer(mac): exact 6-byte memcmp against each registered MAC.
 *     Returns true on first match; false if count==0 or no match.
 *
 * Isolation guarantee: a foreign MAC (from a different KaSe set) is not in
 * the peer table → is_known_peer returns false → recv dropped.
 */
#define TEST_HOST
#include "test_framework.h"
#include "../main/comm/espnow/espnow_link.h"
#include <string.h>
#include <stdbool.h>

/* Helper: build a MAC from 6 individual bytes */
static void make_mac(uint8_t out[6], uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d, uint8_t e, uint8_t f)
{
    out[0]=a; out[1]=b; out[2]=c; out[3]=d; out[4]=e; out[5]=f;
}

void test_espnow_peer_filter(void)
{
    TEST_SUITE("espnow_peer_filter");

    uint8_t mac_left[6], mac_right[6], mac_foreign[6], mac_zero[6];
    make_mac(mac_left,    0xAA,0xBB,0xCC,0xDD,0xEE,0x01);
    make_mac(mac_right,   0xAA,0xBB,0xCC,0xDD,0xEE,0x02);
    make_mac(mac_foreign, 0x11,0x22,0x33,0x44,0x55,0x66);
    memset(mac_zero, 0x00, 6);

    /* ── Case 1: no peers configured → all MACs rejected ─────── */
    {
        espnow_set_peers(NULL, 0);
        TEST_ASSERT(!is_known_peer(mac_left),    "case1: left rejected (no peers)");
        TEST_ASSERT(!is_known_peer(mac_right),   "case1: right rejected (no peers)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case1: foreign rejected (no peers)");
    }

    /* ── Case 2: one peer (half role: mac_dongle only) ─────────── */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        TEST_ASSERT( is_known_peer(mac_left),    "case2: left accepted (1 peer)");
        TEST_ASSERT(!is_known_peer(mac_right),   "case2: right rejected (1 peer)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case2: foreign rejected (1 peer)");
    }

    /* ── Case 3: two peers (dongle role: mac_left + mac_right) ──── */
    {
        const uint8_t peers[2][6] = {
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 },
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x02 },
        };
        espnow_set_peers(peers, 2);
        TEST_ASSERT( is_known_peer(mac_left),    "case3: left accepted  (2 peers)");
        TEST_ASSERT( is_known_peer(mac_right),   "case3: right accepted (2 peers)");
        TEST_ASSERT(!is_known_peer(mac_foreign), "case3: foreign rejected (2 peers)");
    }

    /* ── Case 4: all-zero MAC is not a peer after valid registration ─ */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        TEST_ASSERT(!is_known_peer(mac_zero),    "case4: all-zero MAC not a peer");
    }

    /* ── Case 5: MAC with one-byte difference is not a peer ─────── */
    {
        const uint8_t peers[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers, 1);
        uint8_t almost[6];
        make_mac(almost, 0xAA,0xBB,0xCC,0xDD,0xEE,0x00);  /* last byte differs */
        TEST_ASSERT(!is_known_peer(almost),      "case5: off-by-one last byte rejected");
        make_mac(almost, 0xAB,0xBB,0xCC,0xDD,0xEE,0x01);  /* first byte differs */
        TEST_ASSERT(!is_known_peer(almost),      "case5: off-by-one first byte rejected");
    }

    /* ── Case 6: count > ESPNOW_MAX_PEERS is clamped safely ─────── */
    {
        /* Provide 3 MACs but limit is 2 — must not overflow. */
        const uint8_t peers[3][6] = {
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 },
            { 0xAA,0xBB,0xCC,0xDD,0xEE,0x02 },
            { 0x11,0x22,0x33,0x44,0x55,0x66 },   /* overflowing entry */
        };
        espnow_set_peers(peers, 3);
        TEST_ASSERT( is_known_peer(mac_left),    "case6: left accepted (clamped)");
        TEST_ASSERT( is_known_peer(mac_right),   "case6: right accepted (clamped)");
        /* Third entry should NOT be accepted (clamped at ESPNOW_MAX_PEERS=2) */
        TEST_ASSERT(!is_known_peer(mac_foreign), "case6: 3rd entry not accepted (clamped)");
    }

    /* ── Case 7: re-configure peers replaces previous list ───────── */
    {
        /* Register left only, then replace with foreign only. */
        const uint8_t peers_a[1][6] = {{ 0xAA,0xBB,0xCC,0xDD,0xEE,0x01 }};
        espnow_set_peers(peers_a, 1);
        TEST_ASSERT( is_known_peer(mac_left),    "case7a: left accepted before replace");

        const uint8_t peers_b[1][6] = {{ 0x11,0x22,0x33,0x44,0x55,0x66 }};
        espnow_set_peers(peers_b, 1);
        TEST_ASSERT(!is_known_peer(mac_left),    "case7b: left rejected after replace");
        TEST_ASSERT( is_known_peer(mac_foreign), "case7b: foreign accepted after replace");
    }

    /* ── Case 8: exact match all 6 bytes — canonical round-trip ─── */
    {
        uint8_t mac_a[6] = {0xDE,0xAD,0xBE,0xEF,0x00,0x01};
        const uint8_t peers[1][6] = {{0xDE,0xAD,0xBE,0xEF,0x00,0x01}};
        espnow_set_peers(peers, 1);
        TEST_ASSERT( is_known_peer(mac_a),       "case8: canonical 6-byte match accepted");
        mac_a[5] = 0x02;
        TEST_ASSERT(!is_known_peer(mac_a),       "case8: mutated byte rejected");
    }
}
