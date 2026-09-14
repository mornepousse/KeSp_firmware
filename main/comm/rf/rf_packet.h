#ifndef RF_PACKET_H
#define RF_PACKET_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_pairing.h"   /* RF_DEV_DUMB_HALF, RF_DEV_SMART_KBD */

/* Packet type = high nibble of byte 0; flags = low nibble. */
#define PKT_TYPE_KEY        0x1
#define PKT_TYPE_HEARTBEAT  0x2
#define PKT_TYPE_TRACKPAD   0x3
#define PKT_TYPE_MATRIX     0x4   /* fusion : demi-matrice BRUTE half→dongle (identité de moitié) */
#define PKT_TYPE_HIDREPORT  0x5   /* keyboard-agnostic relay: final HID report */
#define PKT_TYPE_STATUS     0x6   /* supervision du lien : batterie + qualité, sans état */
#define PKT_TYPE_PAIR_ACK   0xE   /* dongle→half pairing ACK (RF-2) */
#define PKT_TYPE_PAIR_REQ   0xF   /* half→dongle pairing request (RF-2) */

/* Sub-types for PKT_TYPE_HIDREPORT (byte 1) */
#define RF_HID_SUB_KBD   0
#define RF_HID_SUB_MOUSE 1

/* Flags (low nibble of byte 0) */
#define PKT_FLAG_PRESSED    0x01   /* PKT_KEY: key is pressed (vs released) */
#define PKT_FLAG_IS_RETRY   0x02   /* application-level retransmit */

/* Géométrie de demi-matrice du protocole. DOIT correspondre aux dimensions des
 * board.h Niphargus — lien assuré par test/test_niphar_right_pins.c, qui inclut
 * les deux et casse à la moindre divergence.
 *
 * Valait 5 lignes jusqu'au 2026-09-05 : c'était la géométrie des anciennes
 * moitiés KaSe, retirées du dépôt au commit c107df77. Elle décrivait donc du
 * matériel qui n'existait plus, gaspillait un octet de bitmap par paquet, et
 * aurait laissé passer une ligne 4 inexistante sur une matrice 4×7.
 *
 * ⚠ Changer ces valeurs change un FORMAT DE TRAME : rf_encode_heartbeat passe
 * de 9 à 8 octets et LINK_PAYLOAD_MATRIX de 7 à 6. Les deux bouts doivent être
 * reflashés ensemble. */
#define RF_HALF_ROWS         4
#define RF_HALF_COLS         7
#define RF_HALF_BITMAP_BYTES 4     /* ceil(4*7 / 8) = 4 */

/* Identité de moitié pour PKT_TYPE_MATRIX (nibble bas de l'octet 0). Le dongle,
 * hub unique de la fusion, distingue ainsi les deux émetteurs sur le slot
 * clavier. Cf. docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md */
#define RF_HALF_LEFT   0
#define RF_HALF_RIGHT  1

typedef struct {
    uint8_t row;       /* 0..3 */
    uint8_t col;       /* 0..6 (local to the half) */
    bool    pressed;
    bool    is_retry;
    uint8_t seq;
} rf_key_event_t;

typedef struct {
    uint8_t bitmap[RF_HALF_BITMAP_BYTES];  /* MSB-first, row*7+col */
    uint8_t batt_dV;   /* 0..83 = 0..8.3V, 0 = unknown */
    uint8_t link_q;    /* cumulative retries since last heartbeat */
    uint8_t seq;
} rf_heartbeat_t;

/* Demi-matrice brute half→dongle pour la fusion (PKT_TYPE_MATRIX). État PUR :
 * pas de batterie ni de qualité de lien — la supervision reste sur
 * PKT_TYPE_STATUS. L'identité de moitié (RF_HALF_LEFT/RIGHT) permet au dongle de
 * fusionner deux émetteurs sur un même slot clavier. */
typedef struct {
    uint8_t half;                          /* RF_HALF_LEFT / RF_HALF_RIGHT */
    uint8_t bitmap[RF_HALF_BITMAP_BYTES];  /* MSB-first, row*7+col */
    uint8_t seq;
} rf_matrix_t;

/* Supervision du lien clavier → dongle.
 *
 * Le design du dongle (docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md,
 * §5) sépare deux fonctions que rf_heartbeat_t mélangeait :
 *
 *   - la RÉPARATION d'état passe par la réémission de PKT_TYPE_HIDREPORT
 *     lui-même, qui porte déjà l'état complet — inutile de l'emballer ailleurs ;
 *   - la SUPERVISION passe par cette trame, qui ne porte aucun état.
 *
 * Elle existe surtout pour une raison logique : le dongle ne peut pas distinguer
 * « elle ne tape pas » de « elle est morte » si elle se tait dans les deux cas.
 * Elle part au repos, ~1 fois par seconde, sur une moitié à batterie : sa taille
 * est une contrainte de conception. D'où 4 octets, contre 8 pour le heartbeat à
 * bitmap — lequel reste utilisé sur le lien droite → gauche, où il y a une vraie
 * matrice à réconcilier. */
typedef struct {
    uint8_t batt_dV;   /* 0..83 = 0..8,3 V ; 0 = inconnu */
    uint8_t link_q;    /* retransmissions cumulées depuis la dernière trame */
    uint8_t seq;
    bool    mode_usb;  /* fusion : la gauche annonce qu'un hôte USB la pilote →
                        * le dongle se tait et réémet la droite. Porté par le
                        * nibble bas de l'octet 0, rétrocompatible. */
    uint32_t config_fp;/* fusion phase 3 : empreinte CRC-32 de la keymap de
                        * l'émetteur, pour que le dongle détecte une divergence de
                        * config. 0 = absente (ancien émetteur, STATUS 4 octets). */
    uint8_t half;      /* jauge : RF_HALF_LEFT/RF_HALF_RIGHT — bit1 du nibble de
                        * flags. 0 = gauche, donc une trame ancienne reste « gauche »
                        * (les deux moitiés partagent le slot clavier en fusion). */
    uint8_t charging;  /* jauge : 0 inconnu, 1 en charge probable, 2 pleine — bits
                        * 2-3 du nibble. Déduit de la tension (pas de VBUS). */
} rf_status_t;

/* Nibble bas de l'octet 0 de STATUS : bit0 mode USB, bit1 identité de moitié,
 * bits 2-3 état de charge. Tous à 0 = trame historique (gauche, inconnu). */
#define PKT_STATUS_FLAG_MODE_USB    0x1
#define PKT_STATUS_FLAG_HALF_RIGHT  0x2
#define PKT_STATUS_CHG_SHIFT        2
#define PKT_STATUS_CHG_MASK         0x3

typedef struct {
    uint8_t ge0, ge1;
    uint8_t n_fingers;
    int16_t rel_x, rel_y;
    uint8_t seq;
} rf_trackpad_t;

typedef struct {
    uint16_t set_id;              /* host order; encoded big-endian on wire */
    uint8_t  dongle_wifi_mac[6];
    uint8_t  slot;               /* 0x01=left, 0x02=right */
} rf_pair_ack_t;

/* ── Encoders: write into buf, return byte count (0 on error) ── */

static inline uint16_t rf_encode_key(uint8_t *buf, const rf_key_event_t *e)
{
    if (e->row > 15 || e->col > 15) return 0;
    uint8_t flags = (e->pressed ? PKT_FLAG_PRESSED : 0) |
                    (e->is_retry ? PKT_FLAG_IS_RETRY : 0);
    buf[0] = (PKT_TYPE_KEY << 4) | (flags & 0x0F);
    buf[1] = (uint8_t)((e->row << 4) | (e->col & 0x0F));
    buf[2] = e->seq;
    return 3;
}

static inline uint16_t rf_encode_heartbeat(uint8_t *buf, const rf_heartbeat_t *h)
{
    buf[0] = (PKT_TYPE_HEARTBEAT << 4);
    memcpy(&buf[1], h->bitmap, RF_HALF_BITMAP_BYTES);
    buf[1 + RF_HALF_BITMAP_BYTES] = h->batt_dV;
    buf[2 + RF_HALF_BITMAP_BYTES] = h->link_q;
    buf[3 + RF_HALF_BITMAP_BYTES] = h->seq;
    return 4 + RF_HALF_BITMAP_BYTES;   /* 9 */
}

static inline uint16_t rf_encode_trackpad(uint8_t *buf, const rf_trackpad_t *t)
{
    buf[0] = (PKT_TYPE_TRACKPAD << 4);
    buf[1] = t->ge0; buf[2] = t->ge1; buf[3] = t->n_fingers;
    buf[4] = (uint8_t)((uint16_t)t->rel_x >> 8);
    buf[5] = (uint8_t)((uint16_t)t->rel_x & 0xFF);
    buf[6] = (uint8_t)((uint16_t)t->rel_y >> 8);
    buf[7] = (uint8_t)((uint16_t)t->rel_y & 0xFF);
    buf[8] = t->seq;
    return 9;
}

/* PKT_PAIR_REQ: 8 bytes — type 0xF, the half's 6-byte WiFi STA MAC, then the
 * half's declared slot (0x01=left / 0x02=right, board identity). slot=0 = unknown
 * (legacy 7-byte halves). */
static inline uint16_t rf_encode_pair_req(uint8_t *buf, const uint8_t mac[6], uint8_t slot)
{
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);
    buf[7] = slot;
    return 8;
}

/* PKT_STATUS: 8 octets — type 0x6 + flags, batterie, qualité de lien, seq, puis
 * l'empreinte CRC-32 de config en little-endian (buf[4..7]). ⚠ Dimensionner les
 * buffers d'émission avec RF_STATUS_LEN : la trame est passée de 4 à 8 octets
 * (empreinte, 4f5e2b09) et des buffers restés à 4 débordaient la pile. */
#define RF_STATUS_LEN 8u
static inline uint16_t rf_encode_status(uint8_t *buf, const rf_status_t *s)
{
    if (buf == NULL || s == NULL) return 0;
    buf[0] = (uint8_t)((PKT_TYPE_STATUS << 4) |
                       (s->mode_usb ? PKT_STATUS_FLAG_MODE_USB : 0) |
                       (s->half == RF_HALF_RIGHT ? PKT_STATUS_FLAG_HALF_RIGHT : 0) |
                       ((s->charging & PKT_STATUS_CHG_MASK) << PKT_STATUS_CHG_SHIFT));
    buf[1] = s->batt_dV;
    buf[2] = s->link_q;
    buf[3] = s->seq;
    buf[4] = (uint8_t)(s->config_fp);          /* empreinte CRC-32, little-endian */
    buf[5] = (uint8_t)(s->config_fp >> 8);
    buf[6] = (uint8_t)(s->config_fp >> 16);
    buf[7] = (uint8_t)(s->config_fp >> 24);
    return 8;
}

/* PKT_PAIR_ACK: 10 bytes — type 0xE, set_id big-endian, dongle MAC, slot. */
static inline uint16_t rf_encode_pair_ack(uint8_t *buf, const rf_pair_ack_t *a)
{
    buf[0] = (PKT_TYPE_PAIR_ACK << 4);
    buf[1] = (uint8_t)(a->set_id >> 8);
    buf[2] = (uint8_t)(a->set_id & 0xFF);
    memcpy(buf + 3, a->dongle_wifi_mac, 6);
    buf[9] = a->slot;
    return 10;
}

/* ── Decoder: returns type (0 on error/unknown), fills the matching struct ── */

static inline uint8_t rf_packet_type(const uint8_t *buf, uint16_t len)
{
    if (len < 1) return 0;
    return (buf[0] >> 4) & 0x0F;
}

static inline bool rf_decode_status(const uint8_t *buf, uint16_t len, rf_status_t *out)
{
    if (buf == NULL || out == NULL) return false;
    if (len < 4 || rf_packet_type(buf, len) != PKT_TYPE_STATUS) return false;
    out->batt_dV  = buf[1];
    out->link_q   = buf[2];
    out->seq      = buf[3];
    out->mode_usb = (buf[0] & PKT_STATUS_FLAG_MODE_USB) != 0;
    out->half     = (buf[0] & PKT_STATUS_FLAG_HALF_RIGHT) ? RF_HALF_RIGHT : RF_HALF_LEFT;
    out->charging = (uint8_t)((buf[0] >> PKT_STATUS_CHG_SHIFT) & PKT_STATUS_CHG_MASK);
    /* Empreinte : présente sur 8 octets, absente (0 = inconnue) sur un ancien
     * STATUS de 4 octets — rétrocompatible. */
    out->config_fp = (len >= 8)
        ? ((uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
           ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24))
        : 0u;
    return true;
}

/* PKT_HIDREPORT: relay final HID reports over NRF24.
 *   kbd:   [type<<4][SUB_KBD][modifier][keycodes×6]   → 9 bytes
 *   mouse: [type<<4][SUB_MOUSE][buttons][x][y][wheel] → 6 bytes
 */
static inline uint16_t rf_encode_hidreport_kbd(uint8_t *buf, uint8_t modifier, const uint8_t kb[6])
{
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_KBD; buf[2] = modifier;
    memcpy(buf + 3, kb, 6); return 9;
}

static inline uint16_t rf_encode_hidreport_mouse(uint8_t *buf, uint8_t buttons, int8_t x, int8_t y, int8_t wheel)
{
    buf[0] = (PKT_TYPE_HIDREPORT << 4); buf[1] = RF_HID_SUB_MOUSE;
    buf[2] = buttons; buf[3] = (uint8_t)x; buf[4] = (uint8_t)y; buf[5] = (uint8_t)wheel; return 6;
}

static inline bool rf_decode_hidreport(const uint8_t *buf, uint16_t len, uint8_t *sub,
        uint8_t *mod, uint8_t kb[6], uint8_t *btn, int8_t *x, int8_t *y, int8_t *wheel)
{
    if (len < 2 || rf_packet_type(buf, len) != PKT_TYPE_HIDREPORT) return false;
    *sub = buf[1];
    if (buf[1] == RF_HID_SUB_KBD)   { if (len < 9) return false; *mod = buf[2]; memcpy(kb, buf+3, 6); return true; }
    if (buf[1] == RF_HID_SUB_MOUSE) { if (len < 6) return false; *btn = buf[2];
        *x=(int8_t)buf[3]; *y=(int8_t)buf[4]; *wheel=(int8_t)buf[5]; return true; }
    return false;
}

static inline bool rf_decode_key(const uint8_t *buf, uint16_t len, rf_key_event_t *e)
{
    if (len < 3 || rf_packet_type(buf, len) != PKT_TYPE_KEY) return false;
    uint8_t flags = buf[0] & 0x0F;
    e->pressed  = (flags & PKT_FLAG_PRESSED) != 0;
    e->is_retry = (flags & PKT_FLAG_IS_RETRY) != 0;
    e->row = (buf[1] >> 4) & 0x0F;
    e->col = buf[1] & 0x0F;
    e->seq = buf[2];
    return true;
}

static inline bool rf_decode_heartbeat(const uint8_t *buf, uint16_t len, rf_heartbeat_t *h)
{
    /* Longueur derivee du symbole, pas ecrite en dur : elle valait 9 quand le
     * bitmap faisait 5 octets, et le passage a 4 l'a rendue fausse en silence
     * (le decodeur refusait toutes les trames que l'encodeur produisait). */
    if (len < 4 + RF_HALF_BITMAP_BYTES ||
        rf_packet_type(buf, len) != PKT_TYPE_HEARTBEAT) return false;
    memcpy(h->bitmap, &buf[1], RF_HALF_BITMAP_BYTES);
    h->batt_dV = buf[1 + RF_HALF_BITMAP_BYTES];
    h->link_q  = buf[2 + RF_HALF_BITMAP_BYTES];
    h->seq     = buf[3 + RF_HALF_BITMAP_BYTES];
    return true;
}

/* Demi-matrice brute half→dongle. 6 octets : type+identité, bitmap (4), seq. */
static inline uint16_t rf_encode_matrix(uint8_t *buf, const rf_matrix_t *m)
{
    if (m->half > 0x0F) return 0;
    buf[0] = (PKT_TYPE_MATRIX << 4) | (m->half & 0x0F);
    memcpy(&buf[1], m->bitmap, RF_HALF_BITMAP_BYTES);
    buf[1 + RF_HALF_BITMAP_BYTES] = m->seq;
    return 2 + RF_HALF_BITMAP_BYTES;   /* 6 */
}

static inline bool rf_decode_matrix(const uint8_t *buf, uint16_t len, rf_matrix_t *m)
{
    if (len < 2 + RF_HALF_BITMAP_BYTES ||
        rf_packet_type(buf, len) != PKT_TYPE_MATRIX) return false;
    m->half = buf[0] & 0x0F;
    memcpy(m->bitmap, &buf[1], RF_HALF_BITMAP_BYTES);
    m->seq  = buf[1 + RF_HALF_BITMAP_BYTES];
    return true;
}

static inline bool rf_decode_trackpad(const uint8_t *buf, uint16_t len, rf_trackpad_t *t)
{
    if (len < 9 || rf_packet_type(buf, len) != PKT_TYPE_TRACKPAD) return false;
    t->ge0 = buf[1]; t->ge1 = buf[2]; t->n_fingers = buf[3];
    t->rel_x = (int16_t)((uint16_t)(buf[4] << 8) | buf[5]);
    t->rel_y = (int16_t)((uint16_t)(buf[6] << 8) | buf[7]);
    t->seq = buf[8];
    return true;
}

static inline bool rf_decode_pair_req(const uint8_t *buf, uint16_t len,
                                      uint8_t mac_out[6], uint8_t *slot_out)
{
    if (len < 7 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_REQ) return false;
    memcpy(mac_out, buf + 1, 6);
    *slot_out = (len >= 8) ? buf[7] : 0;   /* legacy 7-byte req → slot unknown */
    return true;
}

/* PKT_PAIR_REQ v2: 9 bytes — type 0xF, mac[6], slot, devtype.
 * Use RF_DEV_DUMB_HALF or RF_DEV_SMART_KBD for devtype. */
static inline uint16_t rf_encode_pair_req2(uint8_t *buf, const uint8_t mac[6],
                                           uint8_t slot, uint8_t devtype)
{
    buf[0] = (PKT_TYPE_PAIR_REQ << 4);
    memcpy(buf + 1, mac, 6);
    buf[7] = slot;
    buf[8] = devtype;
    return 9;
}

/* Decodes both the 9-byte v2 (with devtype) and the legacy 8-byte (devtype=RF_DEV_DUMB_HALF). */
static inline bool rf_decode_pair_req2(const uint8_t *buf, uint16_t len,
                                       uint8_t mac_out[6], uint8_t *slot_out,
                                       uint8_t *devtype_out)
{
    if (len < 8 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_REQ) return false;
    memcpy(mac_out, buf + 1, 6);
    *slot_out    = buf[7];
    *devtype_out = (len >= 9) ? buf[8] : RF_DEV_DUMB_HALF;
    return true;
}

static inline bool rf_decode_pair_ack(const uint8_t *buf, uint16_t len, rf_pair_ack_t *a)
{
    if (len < 10 || rf_packet_type(buf, len) != PKT_TYPE_PAIR_ACK) return false;
    a->set_id = ((uint16_t)buf[1] << 8) | buf[2];
    memcpy(a->dongle_wifi_mac, buf + 3, 6);
    a->slot = buf[9];
    return true;
}

/* Bitmap helpers (row*7+col bit index, MSB-first in byte) */
static inline bool rf_bitmap_get(const uint8_t *bm, uint8_t row, uint8_t col)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    return (bm[idx >> 3] >> (7 - (idx & 7))) & 1;
}

static inline void rf_bitmap_set(uint8_t *bm, uint8_t row, uint8_t col, bool val)
{
    uint8_t idx = row * RF_HALF_COLS + col;
    uint8_t mask = 1 << (7 - (idx & 7));
    if (val) bm[idx >> 3] |= mask;
    else     bm[idx >> 3] &= ~mask;
}

/* Empaquette une matrice locale row-major (rows×cols, cols == RF_HALF_COLS) en
 * bitmap demi-matrice. Une seule implémentation testée (test_matrix_bitmap.c) ;
 * les deux moitiés l'utilisent pour émettre leur brut. Efface d'abord bm. */
static inline void rf_matrix_to_bitmap(const uint8_t *state, uint8_t rows,
                                       uint8_t cols, uint8_t *bm)
{
    memset(bm, 0, RF_HALF_BITMAP_BYTES);
    for (uint8_t r = 0; r < rows; r++)
        for (uint8_t c = 0; c < cols; c++)
            if (state[(uint16_t)r * cols + c]) rf_bitmap_set(bm, r, c, true);
}

/* ── Sync auto de la keymap dongle→gauche par ACK payload (fusion, phase 3) ─────
 *
 * La gauche est sourde en sans-fil (PTX pur, autonomie) : le dongle ne peut pas
 * lui « pousser » une keymap. Mais chaque trame de la gauche reçoit un ACK
 * matériel, et le nRF24 sait y glisser une charge utile (EN_ACK_PAY). Le dongle
 * distille donc la keymap dans les ACK des émissions normales de la gauche.
 *
 * Pull piloté par la gauche : elle demande le prochain chunk manquant (REQ, en
 * uplink) ; le dongle répond BEACON (« j'ai une keymap d'empreinte fp_target en
 * n_chunks ») ou CHUNK (un morceau) dans l'ACK. Tout tient dans ≤ 32 o.
 * 40 chunks × 28 o = 1120 o = KEYMAP_BLOB_BYTES, sans chunk partiel.
 *
 * Testées host dans test/test_keymap_sync_frames.c. Design :
 * docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
#define PKT_TYPE_SYNC_BEACON 0x7   /* dongle→gauche (ACK) : une keymap est disponible */
#define PKT_TYPE_SYNC_CHUNK  0x8   /* dongle→gauche (ACK) : un morceau de keymap */
#define PKT_TYPE_SYNC_REQ    0x9   /* gauche→dongle (uplink) : prochain chunk voulu */
#define SYNC_CHUNK_BYTES     28
#define SYNC_N_CHUNKS        40    /* 1120 / 28 */

typedef struct { uint32_t fp_target; uint8_t n_chunks; } rf_sync_beacon_t;
typedef struct { uint8_t idx; uint8_t data[SYNC_CHUNK_BYTES]; } rf_sync_chunk_t;
typedef struct { uint8_t next; } rf_sync_req_t;

/* BEACON : 6 octets — type, empreinte LE, nombre de chunks. */
static inline uint16_t rf_encode_sync_beacon(uint8_t *buf, const rf_sync_beacon_t *b)
{
    if (buf == NULL || b == NULL) return 0;
    buf[0] = (PKT_TYPE_SYNC_BEACON << 4);
    buf[1] = (uint8_t)(b->fp_target);
    buf[2] = (uint8_t)(b->fp_target >> 8);
    buf[3] = (uint8_t)(b->fp_target >> 16);
    buf[4] = (uint8_t)(b->fp_target >> 24);
    buf[5] = b->n_chunks;
    return 6;
}
static inline bool rf_decode_sync_beacon(const uint8_t *buf, uint16_t len, rf_sync_beacon_t *o)
{
    if (buf == NULL || o == NULL) return false;
    if (len < 6 || rf_packet_type(buf, len) != PKT_TYPE_SYNC_BEACON) return false;
    o->fp_target = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8) |
                   ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
    o->n_chunks = buf[5];
    return true;
}

/* CHUNK : 30 octets — type, index, 28 octets de données. */
static inline uint16_t rf_encode_sync_chunk(uint8_t *buf, const rf_sync_chunk_t *c)
{
    if (buf == NULL || c == NULL) return 0;
    buf[0] = (PKT_TYPE_SYNC_CHUNK << 4);
    buf[1] = c->idx;
    memcpy(&buf[2], c->data, SYNC_CHUNK_BYTES);
    return 2 + SYNC_CHUNK_BYTES;
}
static inline bool rf_decode_sync_chunk(const uint8_t *buf, uint16_t len, rf_sync_chunk_t *o)
{
    if (buf == NULL || o == NULL) return false;
    if (len < 2 + SYNC_CHUNK_BYTES || rf_packet_type(buf, len) != PKT_TYPE_SYNC_CHUNK)
        return false;
    o->idx = buf[1];
    memcpy(o->data, &buf[2], SYNC_CHUNK_BYTES);
    return true;
}

/* REQ : 2 octets — type, prochain chunk voulu. */
static inline uint16_t rf_encode_sync_req(uint8_t *buf, const rf_sync_req_t *q)
{
    if (buf == NULL || q == NULL) return 0;
    buf[0] = (PKT_TYPE_SYNC_REQ << 4);
    buf[1] = q->next;
    return 2;
}
static inline bool rf_decode_sync_req(const uint8_t *buf, uint16_t len, rf_sync_req_t *o)
{
    if (buf == NULL || o == NULL) return false;
    if (len < 2 || rf_packet_type(buf, len) != PKT_TYPE_SYNC_REQ) return false;
    o->next = buf[1];
    return true;
}

/* ── Trame d'affichage dongle→moitié, glissée dans l'ACK payload ─────────────
 *
 * Les moitiés n'écoutent pas (PTX, autonomie) : ce que le dongle veut leur
 * montrer — la couche STATIQUE courante, la batterie de l'AUTRE moitié — voyage
 * dans l'ACK de leurs propres émissions, comme la sync keymap (prioritaire).
 * 4 octets : [type<<4 | flags][couche][batt autre dV][chg & 0x3 | dongle_ok<<2].
 * flags bit0 = destinée à la DROITE (les deux moitiés partagent le slot).
 * Testée host dans test/test_rf_packet.c. Spec : ecrans-memlcd-design.md §4. */
#define PKT_TYPE_DISPLAY          0xA
#define PKT_DISPLAY_FLAG_TO_RIGHT 0x1

typedef struct {
    uint8_t to_right;        /* 1 = pour la droite, 0 = pour la gauche */
    uint8_t couche;          /* couche statique (TO / base), jamais un MO tenu */
    uint8_t batt_autre_dv;   /* tension de l'AUTRE moitié, 0 = inconnue */
    uint8_t batt_autre_chg;  /* 0 inconnu, 1 en charge probable, 2 pleine */
    uint8_t dongle_ok;       /* 1 = le dongle parle (toujours 1 quand reçu…) */
} rf_display_t;

static inline uint16_t rf_encode_display(uint8_t *buf, const rf_display_t *d)
{
    if (buf == NULL || d == NULL) return 0;
    buf[0] = (uint8_t)((PKT_TYPE_DISPLAY << 4) | (d->to_right ? PKT_DISPLAY_FLAG_TO_RIGHT : 0));
    buf[1] = d->couche;
    buf[2] = d->batt_autre_dv;
    buf[3] = (uint8_t)((d->batt_autre_chg & 0x3) | (d->dongle_ok ? 0x4 : 0));
    return 4;
}
static inline bool rf_decode_display(const uint8_t *buf, uint16_t len, rf_display_t *o)
{
    if (buf == NULL || o == NULL) return false;
    if (len < 4 || rf_packet_type(buf, len) != PKT_TYPE_DISPLAY) return false;
    o->to_right       = (buf[0] & PKT_DISPLAY_FLAG_TO_RIGHT) != 0;
    o->couche         = buf[1];
    o->batt_autre_dv  = buf[2];
    o->batt_autre_chg = buf[3] & 0x3;
    o->dongle_ok      = (buf[3] & 0x4) != 0;
    return true;
}

#endif /* RF_PACKET_H */
