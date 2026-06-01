/*
 * cdc_dongle_cmds.c — CDC binary commands specific to the dongle role.
 *
 * Compiled only when CONFIG_KASE_DEVICE_ROLE_DONGLE=y. Registers a second
 * command table with the binary protocol parser (the standard table lives in
 * cdc_binary_cmds.c).
 *
 * Commands provided here (see cdc_binary_protocol.h for IDs):
 *   - KS_CMD_RF_STATUS      → 27-byte link snapshot for both halves
 *   - KS_CMD_RF_PAIR_LIST   → 13 bytes: paired_count + mac_left + mac_right
 *   - KS_CMD_RF_PAIR_RESET  → 1 byte: paired_count after reset (always 0)
 *   - KS_CMD_BATTERY        → 14 bytes: dV/soc/charging/age_ms per half
 *
 * All multi-byte integers are little-endian.
 */

#include "cdc_binary_protocol.h"
#include "rf_rx_task.h"
#include "rf_pairing.h"

#include <string.h>
#include <stdint.h>

/* From dongle_engine_state.c — battery cache accessor */
extern void dongle_cache_get_battery(uint8_t slot,
                                     uint8_t *batt_dV, uint8_t *soc_pct,
                                     uint8_t *charging, uint32_t *age_ms_out);

/* ── Little-endian packers ──────────────────────────────────────── */
static inline void put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)( v        & 0xFF);
    p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ── KS_CMD_RF_STATUS ───────────────────────────────────────────────
 * Request: no payload.
 * Response (27 bytes):
 *   [0]    flags         bit0=link_left_up, bit1=link_right_up, bits2-7=rsvd
 *   [1]    sig_left      rf_signal_q255(link_up,age,link_q) — 0..255, 0=down
 *   [2]    sig_right     idem for the right half
 *   [3..6] hb_age_left   u32 LE ms since last heartbeat from left
 *   [7..10] hb_age_right u32 LE ms since last heartbeat from right
 *   [11..14] pkt_rx_left  u32 LE total accepted packets from left
 *   [15..18] pkt_rx_right idem right
 *   [19..22] pkt_dup_left u32 LE dropped duplicates from left
 *   [23..26] pkt_dup_right idem right
 */
static void bin_cmd_rf_status(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;

    rf_link_status_t st;
    rf_rx_get_status(&st);

    uint8_t buf[27];
    buf[0] = (uint8_t)((st.link_left  ? 0x01 : 0) |
                       (st.link_right ? 0x02 : 0));
    buf[1] = rf_signal_q255(st.link_left,  st.hb_age_left_ms,  st.link_q_left);
    buf[2] = rf_signal_q255(st.link_right, st.hb_age_right_ms, st.link_q_right);
    put_u32_le(&buf[3],  st.hb_age_left_ms);
    put_u32_le(&buf[7],  st.hb_age_right_ms);
    put_u32_le(&buf[11], st.pkt_rx_left);
    put_u32_le(&buf[15], st.pkt_rx_right);
    put_u32_le(&buf[19], st.pkt_dup_left);
    put_u32_le(&buf[23], st.pkt_dup_right);

    ks_respond(cmd, KS_STATUS_OK, buf, sizeof(buf));
}

/* ── KS_CMD_RF_PAIR_LIST ────────────────────────────────────────────
 * Request: no payload.
 * Response (13 bytes):
 *   [0]      paired_count  0..2
 *   [1..6]   mac_left      6 bytes (00:00:00:00:00:00 if not paired)
 *   [7..12]  mac_right     6 bytes (00:00:00:00:00:00 if not paired)
 */
static void bin_cmd_rf_pair_list(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;

    uint8_t mac_left[6]  = {0};
    uint8_t mac_right[6] = {0};
    uint8_t paired = 0;
    rf_pairing_load_peers_dongle(mac_left, mac_right, &paired);

    uint8_t buf[13];
    buf[0] = paired;
    memcpy(&buf[1], mac_left,  6);
    memcpy(&buf[7], mac_right, 6);

    ks_respond(cmd, KS_STATUS_OK, buf, sizeof(buf));
}

/* ── KS_CMD_RF_PAIR_RESET ───────────────────────────────────────────
 * Request: no payload.
 * Response (1 byte): paired_count after reset (always 0 on success).
 * Clears all pairing state from NVS namespace "rf". The dongle keeps
 * running on the current radio config; new halves can be paired via
 * KS_CMD_RF_PAIR_START. Existing halves will time out their heartbeat
 * but cannot reconnect until they re-pair. */
static void bin_cmd_rf_pair_reset(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;

    if (rf_pairing_reset_dongle() != 0) {
        ks_respond_err(cmd, KS_STATUS_ERR_UNKNOWN);
        return;
    }
    uint8_t paired = 0;
    ks_respond(cmd, KS_STATUS_OK, &paired, 1);
}

/* ── KS_CMD_BATTERY ─────────────────────────────────────────────────
 * Request: no payload.
 * Response (14 bytes): two records, slot 0=LEFT then slot 1=RIGHT.
 *   per slot (7 bytes):
 *     [0]    batt_dV      voltage × 10 (0xFF = never reported)
 *     [1]    soc_pct      0..100 (0xFF = unknown)
 *     [2]    charging     0=no, 1=yes (0xFF = unknown)
 *     [3..6] age_ms       u32 LE ms since the sample was received
 *                          (0xFFFFFFFF = never received)
 *
 * The controller should treat any 0xFF in the first three fields, or
 * age_ms above ~30 s, as "stale". Halves emit battery only when the
 * value changes (rate-limited), so age can grow legitimately. */
static void bin_cmd_battery(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;

    uint8_t buf[14];
    for (uint8_t slot = 0; slot < 2; slot++) {
        uint8_t  dV, soc, chg;
        uint32_t age;
        dongle_cache_get_battery(slot, &dV, &soc, &chg, &age);
        uint8_t *r = &buf[slot * 7];
        r[0] = dV;
        r[1] = soc;
        r[2] = chg;
        put_u32_le(&r[3], age);
    }
    ks_respond(cmd, KS_STATUS_OK, buf, sizeof(buf));
}

/* ── Command table + init ──────────────────────────────────────── */

static const ks_bin_cmd_entry_t dongle_cmd_table[] = {
    { KS_CMD_RF_STATUS,      bin_cmd_rf_status      },
    { KS_CMD_RF_PAIR_LIST,   bin_cmd_rf_pair_list   },
    { KS_CMD_RF_PAIR_RESET,  bin_cmd_rf_pair_reset  },
    { KS_CMD_BATTERY,        bin_cmd_battery        },
};

void cdc_dongle_cmds_init(void)
{
    ks_register_binary_commands(dongle_cmd_table,
                                sizeof(dongle_cmd_table) / sizeof(dongle_cmd_table[0]));
}

/* Silence unused warnings on the put_u16_le helper — reserved for future cmds. */
static inline void __attribute__((unused)) cdc_dongle_keep_helpers(void)
{
    uint8_t tmp[2]; put_u16_le(tmp, 0);
}
