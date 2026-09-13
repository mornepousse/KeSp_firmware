/* Tests for the NRF24 RF packet codec (dongle plan 2) */
#include "test_framework.h"
#include "../main/comm/rf/rf_packet.h"
#include "../main/comm/rf/rf_pairing.h"

static void test_rf_key_roundtrip(void)
{
    uint8_t buf[32];
    rf_key_event_t e = { .row = 3, .col = 5, .pressed = true, .is_retry = false, .seq = 42 };
    uint16_t n = rf_encode_key(buf, &e);
    TEST_ASSERT_EQ(n, 3, "key encode length");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_KEY, "key type");

    rf_key_event_t d;
    TEST_ASSERT(rf_decode_key(buf, n, &d), "key decode ok");
    TEST_ASSERT_EQ(d.row, 3, "key row");
    TEST_ASSERT_EQ(d.col, 5, "key col");
    TEST_ASSERT(d.pressed, "key pressed");
    TEST_ASSERT(!d.is_retry, "key not retry");
    TEST_ASSERT_EQ(d.seq, 42, "key seq");
}

static void test_rf_key_flags(void)
{
    uint8_t buf[32];
    rf_key_event_t e = { .row = 0, .col = 0, .pressed = false, .is_retry = true, .seq = 255 };
    uint16_t n = rf_encode_key(buf, &e);
    rf_key_event_t d;
    rf_decode_key(buf, n, &d);
    TEST_ASSERT(!d.pressed, "released flag");
    TEST_ASSERT(d.is_retry, "retry flag");
    TEST_ASSERT_EQ(d.seq, 255, "seq 255");
}

static void test_rf_heartbeat_roundtrip(void)
{
    uint8_t buf[32];
    rf_heartbeat_t h;
    memset(&h, 0, sizeof(h));
    rf_bitmap_set(h.bitmap, 3, 6, true);   /* derniere touche de la moitie (4x7) */
    rf_bitmap_set(h.bitmap, 0, 0, true);
    h.batt_dV = 74; h.link_q = 2; h.seq = 7;

    uint16_t n = rf_encode_heartbeat(buf, &h);
    TEST_ASSERT_EQ(n, 8, "heartbeat encode length");   /* 4 + bitmap 4 octets */

    rf_heartbeat_t hd;
    TEST_ASSERT(rf_decode_heartbeat(buf, n, &hd), "hb decode ok");
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 3, 6), "hb bit 3,6");
    TEST_ASSERT(rf_bitmap_get(hd.bitmap, 0, 0), "hb bit 0,0");
    TEST_ASSERT(!rf_bitmap_get(hd.bitmap, 2, 3), "hb bit 2,3 clear");
    TEST_ASSERT_EQ(hd.batt_dV, 74, "hb batt");
    TEST_ASSERT_EQ(hd.link_q, 2, "hb link_q");
    TEST_ASSERT_EQ(hd.seq, 7, "hb seq");
}

static void test_rf_trackpad_roundtrip(void)
{
    uint8_t buf[16];
    rf_trackpad_t t = { .ge0=0x01, .ge1=0x02, .n_fingers=2, .rel_x=-300, .rel_y=1234, .seq=9 };
    uint16_t n = rf_encode_trackpad(buf, &t);
    TEST_ASSERT_EQ(n, 9, "trackpad encode length");
    rf_trackpad_t td;
    TEST_ASSERT(rf_decode_trackpad(buf, n, &td), "tp decode ok");
    TEST_ASSERT_EQ(td.ge0, 0x01, "tp ge0");
    TEST_ASSERT_EQ(td.ge1, 0x02, "tp ge1");
    TEST_ASSERT_EQ(td.n_fingers, 2, "tp nfingers");
    TEST_ASSERT_EQ(td.rel_x, -300, "tp rel_x BE signed");
    TEST_ASSERT_EQ(td.rel_y, 1234, "tp rel_y BE");
    TEST_ASSERT_EQ(td.seq, 9, "tp seq");
    TEST_ASSERT(!rf_decode_trackpad(buf, 8, &td), "tp short rejected");
}

/* ── Trame d'état — supervision du lien clavier → dongle ────────────────────
 *
 * Le §5 du design du dongle sépare deux fonctions que l'ancien heartbeat
 * mélangeait : la réparation d'état et la supervision.
 *
 * La réparation passe par la réémission du PKT_TYPE_HIDREPORT lui-même, qui
 * porte déjà l'état complet — inutile de l'emballer ailleurs. Reste la
 * supervision : batterie, qualité de lien, et surtout un signe de vie, parce que
 * le dongle ne peut pas distinguer « elle ne tape pas » de « elle est morte » si
 * elle se tait dans les deux cas.
 *
 * D'où cette trame, volontairement minuscule : elle part au repos, environ une
 * fois par seconde, et chaque octet y coûte du temps d'antenne et de la
 * batterie.
 */
static void test_rf_status_roundtrip(void)
{
    uint8_t buf[16];
    rf_status_t in = { .batt_dV = 41, .link_q = 3, .seq = 200 };

    uint16_t n = rf_encode_status(buf, &in);
    TEST_ASSERT_EQ(n, 4, "la trame d'état tient en 4 octets");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_STATUS, "type STATUS");

    rf_status_t out = {0};
    TEST_ASSERT(rf_decode_status(buf, n, &out), "décodée");
    TEST_ASSERT_EQ(out.batt_dV, 41, "batterie conservée");
    TEST_ASSERT_EQ(out.link_q, 3,  "qualité de lien conservée");
    TEST_ASSERT_EQ(out.seq, 200,   "seq conservé");
}

/* Fusion phase 2 : la gauche annonce son mode (USB vs sans-fil) au dongle via un
 * flag dans le nibble bas de l'octet 0 de STATUS (libre, rétrocompatible : un
 * décodeur qui l'ignore lit quand même batt/link_q/seq). */
static void test_rf_status_mode_usb_flag(void)
{
    uint8_t buf[16];
    rf_status_t in = { .batt_dV = 40, .link_q = 1, .seq = 5, .mode_usb = true };
    uint16_t n = rf_encode_status(buf, &in);
    TEST_ASSERT_EQ(n, 4, "STATUS reste 4 octets avec le flag");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_STATUS, "type STATUS malgré le flag");
    TEST_ASSERT((buf[0] & 0x0F) != 0, "flag mode dans le nibble bas");

    rf_status_t out = {0};
    TEST_ASSERT(rf_decode_status(buf, n, &out), "décodée");
    TEST_ASSERT(out.mode_usb, "mode_usb round-trip");
    TEST_ASSERT_EQ(out.batt_dV, 40, "batterie conservée avec le flag");
    TEST_ASSERT_EQ(out.seq, 5, "seq conservé avec le flag");

    /* Sans-fil : flag à zéro. */
    rf_status_t in2 = { .batt_dV = 40, .link_q = 0, .seq = 6, .mode_usb = false };
    rf_encode_status(buf, &in2);
    rf_status_t out2 = {0};
    rf_decode_status(buf, 4, &out2);
    TEST_ASSERT(!out2.mode_usb, "sans-fil → mode_usb false");
}

static void test_rf_status_rejects_short_and_wrong_type(void)
{
    uint8_t buf[16];
    rf_status_t in = { .batt_dV = 40, .link_q = 0, .seq = 1 };
    uint16_t n = rf_encode_status(buf, &in);
    rf_status_t out = {0};

    for (uint16_t cut = 0; cut < n; cut++)
        TEST_ASSERT(!rf_decode_status(buf, cut, &out), "trame tronquée → rejet");

    buf[0] = (PKT_TYPE_HEARTBEAT << 4);
    TEST_ASSERT(!rf_decode_status(buf, n, &out), "type étranger → rejet");
}

static void test_rf_status_is_smaller_than_a_heartbeat(void)
{
    /* Elle part au repos, en continu, sur une moitié à batterie : sa taille est
     * une contrainte de conception, pas un détail. L'ancien heartbeat traînait un
     * bitmap de matrice dont le dongle n'a plus l'usage. */
    uint8_t a[16], b[16];
    rf_status_t st = { .batt_dV = 40, .link_q = 0, .seq = 0 };
    rf_heartbeat_t hb = { .bitmap = {0}, .batt_dV = 40, .link_q = 0, .seq = 0 };
    TEST_ASSERT(rf_encode_status(a, &st) < rf_encode_heartbeat(b, &hb),
                "la trame d'état est plus courte que le heartbeat à bitmap");
}

static void test_rf_decode_rejects(void)
{
    uint8_t buf[32];
    rf_trackpad_t t = { .ge0 = 0, .ge1 = 0, .n_fingers = 1, .rel_x = 1, .rel_y = 0, .seq = 1 };
    uint16_t n = rf_encode_trackpad(buf, &t);

    rf_key_event_t d;
    TEST_ASSERT(!rf_decode_key(buf, n, &d), "reject wrong type");

    rf_heartbeat_t hd;
    TEST_ASSERT(!rf_decode_heartbeat(buf, 2, &hd), "reject short buffer");

    TEST_ASSERT_EQ(rf_packet_type(buf, 0), 0, "type of empty buffer");
}

static void test_rf_bitmap_all_positions(void)
{
    uint8_t bm[RF_HALF_BITMAP_BYTES];
    memset(bm, 0, sizeof(bm));
    /* set every position, verify only that one reads back */
    for (uint8_t r = 0; r < RF_HALF_ROWS; r++) {
        for (uint8_t c = 0; c < RF_HALF_COLS; c++) {
            memset(bm, 0, sizeof(bm));
            rf_bitmap_set(bm, r, c, true);
            TEST_ASSERT(rf_bitmap_get(bm, r, c), "bit set reads back");
            int count = 0;
            for (uint8_t rr = 0; rr < RF_HALF_ROWS; rr++)
                for (uint8_t cc = 0; cc < RF_HALF_COLS; cc++)
                    if (rf_bitmap_get(bm, rr, cc)) count++;
            TEST_ASSERT_EQ(count, 1, "exactly one bit set");
        }
    }
}

static void test_rf_pair_roundtrip(void)
{
    /* ── PKT_PAIR_REQ round-trip ──────────────────────────────── */
    {
        const uint8_t mac[6] = {0x24,0x6F,0x28,0xAA,0xBB,0xCC};
        uint8_t buf[8];
        uint16_t n = rf_encode_pair_req(buf, mac, 0x01);
        TEST_ASSERT_EQ(n, 8,                "pair_req: encodes 8 bytes");
        TEST_ASSERT_EQ(buf[0], 0xF0,        "pair_req: byte0 = 0xF0");
        TEST_ASSERT_EQ(rf_packet_type(buf,n), PKT_TYPE_PAIR_REQ, "pair_req: type 0xF");
        uint8_t out[6] = {0};
        uint8_t slot = 0xFF;
        TEST_ASSERT(rf_decode_pair_req(buf, n, out, &slot), "pair_req: decode ok");
        TEST_ASSERT_EQ(memcmp(out, mac, 6), 0,       "pair_req: mac round-trips");
        TEST_ASSERT_EQ(slot, 0x01,          "pair_req: slot round-trips");
    }

    /* ── PKT_PAIR_ACK round-trip (big-endian set_id) ──────────── */
    {
        rf_pair_ack_t a = { .set_id = 0x1234,
                            .dongle_wifi_mac = {0x10,0x20,0x30,0x40,0x50,0x60},
                            .slot = 0x02 };
        uint8_t buf[10];
        uint16_t n = rf_encode_pair_ack(buf, &a);
        TEST_ASSERT_EQ(n, 10,               "pair_ack: encodes 10 bytes");
        TEST_ASSERT_EQ(buf[0], 0xE0,        "pair_ack: byte0 = 0xE0");
        TEST_ASSERT_EQ(buf[1], 0x12,        "pair_ack: set_id hi first (BE)");
        TEST_ASSERT_EQ(buf[2], 0x34,        "pair_ack: set_id lo second");
        TEST_ASSERT_EQ(buf[9], 0x02,        "pair_ack: slot in byte9");
        rf_pair_ack_t d;
        TEST_ASSERT(rf_decode_pair_ack(buf, n, &d), "pair_ack: decode ok");
        TEST_ASSERT_EQ(d.set_id, 0x1234,    "pair_ack: set_id round-trips");
        TEST_ASSERT_EQ(d.slot, 0x02,        "pair_ack: slot round-trips");
        TEST_ASSERT_EQ(memcmp(d.dongle_wifi_mac, a.dongle_wifi_mac, 6), 0,
                                            "pair_ack: dongle mac round-trips");
    }

    /* ── Negative: wrong type / short buffer ──────────────────── */
    {
        uint8_t bad[10] = {0x10};   /* type 0x1 (KEY), not pair */
        uint8_t out[6];
        uint8_t slot = 0;
        rf_pair_ack_t d;
        TEST_ASSERT(!rf_decode_pair_req(bad, 7, out, &slot), "pair_req: wrong type rejected");
        TEST_ASSERT(!rf_decode_pair_ack(bad, 10, &d), "pair_ack: wrong type rejected");
        uint8_t shortbuf[5] = {0xF0};
        TEST_ASSERT(!rf_decode_pair_req(shortbuf, 5, out, &slot), "pair_req: short buf rejected");
        uint8_t shortack[9] = {0xE0};
        TEST_ASSERT(!rf_decode_pair_ack(shortack, 9, &d),  "pair_ack: short buf rejected");
    }
}

/* ── PKT_PAIR_REQ declared-slot API (RF-declared-slot spec) ────────────────
 * Régression de l'API désormais implémentée dans rf_packet.h :
 *   rf_encode_pair_req(buf, mac, slot) → 8 octets
 *   rf_decode_pair_req(buf, len, mac_out, slot_out) → bool, 4 paramètres
 */

/* Roundtrip slot=0x01 : encode 8 octets, decode préserve mac et slot. */
static void test_rf_pair_req_slot_left_roundtrip(void)
{
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x11, 0x22, 0x33};
    uint8_t buf[8];
    /* Nouvelle signature : rf_encode_pair_req(buf, mac, slot) → 8 */
    uint16_t n = rf_encode_pair_req(buf, mac, 0x01);
    TEST_ASSERT_EQ(n, 8, "pair_req slot=0x01: encode retourne 8 octets");
    TEST_ASSERT_EQ(buf[7], 0x01, "pair_req slot=0x01: byte7 = slot");

    uint8_t mac_out[6] = {0};
    uint8_t slot_out = 0xFF;
    /* Nouvelle signature : rf_decode_pair_req(buf, len, mac_out, slot_out) → bool */
    bool ok = rf_decode_pair_req(buf, n, mac_out, &slot_out);
    TEST_ASSERT(ok, "pair_req slot=0x01: decode ok");
    TEST_ASSERT_EQ(memcmp(mac_out, mac, 6), 0, "pair_req slot=0x01: mac preservée");
    TEST_ASSERT_EQ(slot_out, 0x01, "pair_req slot=0x01: slot preservé");
}

/* Roundtrip slot=0x02 : même contrat pour la moitié droite. */
static void test_rf_pair_req_slot_right_roundtrip(void)
{
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t buf[8];
    uint16_t n = rf_encode_pair_req(buf, mac, 0x02);
    TEST_ASSERT_EQ(n, 8, "pair_req slot=0x02: encode retourne 8 octets");
    TEST_ASSERT_EQ(buf[7], 0x02, "pair_req slot=0x02: byte7 = slot");

    uint8_t mac_out[6] = {0};
    uint8_t slot_out = 0xFF;
    bool ok = rf_decode_pair_req(buf, n, mac_out, &slot_out);
    TEST_ASSERT(ok, "pair_req slot=0x02: decode ok");
    TEST_ASSERT_EQ(memcmp(mac_out, mac, 6), 0, "pair_req slot=0x02: mac preservée");
    TEST_ASSERT_EQ(slot_out, 0x02, "pair_req slot=0x02: slot preservé");
}

/* Rétrocompatibilité : buffer 7 octets (ancienne moitié sans slot byte).
 * Le decode doit réussir, mac préservée, slot_out = 0 (inconnu). */
static void test_rf_pair_req_legacy_7bytes(void)
{
    const uint8_t mac[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    /* Construction manuelle : byte0 = type 0xF (PKT_TYPE_PAIR_REQ << 4 = 0xF0),
     * bytes 1..6 = mac — pas de slot byte (ancien firmware). */
    uint8_t buf[7];
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);

    uint8_t mac_out[6] = {0};
    uint8_t slot_out = 0xFF;
    bool ok = rf_decode_pair_req(buf, 7, mac_out, &slot_out);
    TEST_ASSERT(ok, "pair_req legacy 7B: decode ok");
    TEST_ASSERT_EQ(memcmp(mac_out, mac, 6), 0, "pair_req legacy 7B: mac preservée");
    TEST_ASSERT_EQ(slot_out, 0, "pair_req legacy 7B: slot_out = 0 (inconnu)");
}

/* Buffer trop court (len < 7) : decode doit retourner false. */
static void test_rf_pair_req_too_short(void)
{
    uint8_t buf[6];
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memset(buf + 1, 0xAA, 5);

    uint8_t mac_out[6];
    uint8_t slot_out = 0xFF;
    bool ok = rf_decode_pair_req(buf, 6, mac_out, &slot_out);
    TEST_ASSERT(!ok, "pair_req len<7: retourne false");
}

static void test_rf_pkt_hidreport_roundtrip(void)
{
    uint8_t kb[6] = {0x04,0x05,0,0,0,0};
    uint8_t buf[32];
    uint16_t n = rf_encode_hidreport_kbd(buf, 0x02, kb);
    TEST_ASSERT_EQ(n, 9, "kbd hidreport = 1+1+1+6");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_HIDREPORT, "type 0x5");
    uint8_t mod, kbo[6]; uint8_t btn; int8_t x,y,w; uint8_t sub;
    TEST_ASSERT(rf_decode_hidreport(buf, n, &sub, &mod, kbo, &btn, &x, &y, &w), "decode kbd");
    TEST_ASSERT(sub==0 && mod==0x02 && kbo[0]==0x04 && kbo[1]==0x05, "kbd fields");
    n = rf_encode_hidreport_mouse(buf, 0x01, 5, -3, 1);
    TEST_ASSERT_EQ(n, 6, "mouse hidreport = 1+1+1+1+1+1");
    TEST_ASSERT(rf_decode_hidreport(buf, n, &sub, &mod, kbo, &btn, &x, &y, &w), "decode mouse");
    TEST_ASSERT(sub==1 && btn==0x01 && x==5 && y==-3 && w==1, "mouse fields");
    TEST_ASSERT(!rf_decode_hidreport(buf, 2, &sub, &mod, kbo, &btn, &x, &y, &w), "runt rejected");
}

/* ── PKT_TYPE_MATRIX — demi-matrice brute pour la fusion au dongle ───────────
 *
 * Phase 1 de la fusion : les deux moitiés émettent leur matrice BRUTE au dongle,
 * qui fusionne et fait tourner le moteur. Le dongle doit savoir de QUELLE moitié
 * vient chaque bitmap — l'identité était implicite sur l'ancien lien direct
 * droite→gauche (un seul pair), elle ne l'est plus au dongle (deux pairs sur le
 * slot clavier). D'où l'octet d'identité dans le nibble bas de l'octet 0. État
 * pur : ni batterie ni qualité de lien, qui restent sur PKT_TYPE_STATUS. */
static void test_rf_matrix_roundtrip(void)
{
    uint8_t buf[16];
    rf_matrix_t m; memset(&m, 0, sizeof(m));
    m.half = RF_HALF_RIGHT;
    rf_bitmap_set(m.bitmap, 3, 6, true);   /* derniere touche de la moitie (4x7) */
    rf_bitmap_set(m.bitmap, 0, 0, true);
    m.seq = 17;

    uint16_t n = rf_encode_matrix(buf, &m);
    TEST_ASSERT_EQ(n, 6, "matrix encode = 1 + bitmap 4 + seq");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_MATRIX, "type MATRIX");

    rf_matrix_t d; memset(&d, 0, sizeof(d));
    TEST_ASSERT(rf_decode_matrix(buf, n, &d), "matrix decode ok");
    TEST_ASSERT_EQ(d.half, RF_HALF_RIGHT, "half id preservé");
    TEST_ASSERT(rf_bitmap_get(d.bitmap, 3, 6), "bit 3,6");
    TEST_ASSERT(rf_bitmap_get(d.bitmap, 0, 0), "bit 0,0");
    TEST_ASSERT(!rf_bitmap_get(d.bitmap, 2, 3), "bit 2,3 clear");
    TEST_ASSERT_EQ(d.seq, 17, "seq preservé");
}

static void test_rf_matrix_left_half(void)
{
    uint8_t buf[16];
    rf_matrix_t m; memset(&m, 0, sizeof(m));
    m.half = RF_HALF_LEFT; m.seq = 1;
    uint16_t n = rf_encode_matrix(buf, &m);
    TEST_ASSERT_EQ(buf[0], (PKT_TYPE_MATRIX << 4) | RF_HALF_LEFT, "left dans le nibble bas");
    rf_matrix_t d;
    TEST_ASSERT(rf_decode_matrix(buf, n, &d), "decode ok");
    TEST_ASSERT_EQ(d.half, RF_HALF_LEFT, "left half preservé");
}

static void test_rf_matrix_rejects(void)
{
    uint8_t buf[16];
    rf_matrix_t m; memset(&m, 0, sizeof(m)); m.half = RF_HALF_LEFT;
    uint16_t n = rf_encode_matrix(buf, &m);
    rf_matrix_t d;
    for (uint16_t cut = 0; cut < n; cut++)
        TEST_ASSERT(!rf_decode_matrix(buf, cut, &d), "trame tronquée → rejet");
    buf[0] = (PKT_TYPE_STATUS << 4);
    TEST_ASSERT(!rf_decode_matrix(buf, n, &d), "type étranger → rejet");
}

static void test_rf_pair_devtype(void) {
    uint8_t buf[16]; uint8_t mac[6]={1,2,3,4,5,6};
    uint16_t n = rf_encode_pair_req2(buf, mac, 0x01, RF_DEV_SMART_KBD);
    uint8_t mo[6], slot, dev;
    TEST_ASSERT(rf_decode_pair_req2(buf, n, mo, &slot, &dev), "decode v2 pair req");
    TEST_ASSERT(dev == RF_DEV_SMART_KBD && slot == 0x01, "devtype + slot");
    TEST_ASSERT(mo[0]==1 && mo[5]==6, "mac roundtrip");
    /* legacy 8-byte pair-req decodes as DUMB_HALF via the v2 decoder */
    uint8_t legacy[8]; rf_encode_pair_req(legacy, mac, 0x02);
    TEST_ASSERT(rf_decode_pair_req2(legacy, 8, mo, &slot, &dev), "legacy decodes");
    TEST_ASSERT(dev == RF_DEV_DUMB_HALF && slot == 0x02, "legacy = dumb half");
}

void test_rf_packet(void)
{
    TEST_SUITE("RF packet codec");
    test_rf_key_roundtrip();
    test_rf_key_flags();
    test_rf_heartbeat_roundtrip();
    test_rf_trackpad_roundtrip();
    test_rf_decode_rejects();

    /* Supervision du lien clavier → dongle (design dongle §5) */
    test_rf_status_roundtrip();
    test_rf_status_mode_usb_flag();
    test_rf_status_rejects_short_and_wrong_type();
    test_rf_status_is_smaller_than_a_heartbeat();
    test_rf_bitmap_all_positions();
    test_rf_pair_roundtrip();

    /* TDD red: nouveaux tests déclaré-slot — échouent tant que l'API n'est pas mise à jour */
    test_rf_pair_req_slot_left_roundtrip();
    test_rf_pair_req_slot_right_roundtrip();
    test_rf_pair_req_legacy_7bytes();
    test_rf_pair_req_too_short();

    /* TDD: PKT_TYPE_HIDREPORT encode/decode */
    test_rf_pkt_hidreport_roundtrip();

    /* TDD: v2 pairing request with device-type byte */
    test_rf_pair_devtype();

    /* TDD: PKT_TYPE_MATRIX — demi-matrice brute portant l'identité de moitié */
    test_rf_matrix_roundtrip();
    test_rf_matrix_left_half();
    test_rf_matrix_rejects();
}
