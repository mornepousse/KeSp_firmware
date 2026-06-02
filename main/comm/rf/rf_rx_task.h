#ifndef RF_RX_TASK_H
#define RF_RX_TASK_H

#include <stdint.h>
#include <stdbool.h>

/* Start RF radios + rx task. Returns false if neither radio is present. */
bool rf_rx_start(void);

/* Link diagnostics for CDC (Plan 4). */
typedef struct {
    bool link_left, link_right;
    uint32_t hb_age_left_ms, hb_age_right_ms;
    uint32_t pkt_rx_left, pkt_rx_right;
    uint32_t pkt_dup_left, pkt_dup_right;
    /* New: last link_q reported by each half in PKT_HEARTBEAT.
     * 0 if no heartbeat received yet (conservative = best retry score). */
    uint8_t link_q_left;
    uint8_t link_q_right;
} rf_link_status_t;

void rf_rx_get_status(rf_link_status_t *out);

/* Copy the current paired-half WiFi MACs (live copy: loaded at boot and
 * refreshed on every successful pairing). For ESP-NOW senders (status push,
 * layer/state push) so they never rely on a stale one-shot NVS cache.
 * All-zero MAC = that half is not paired. */
void rf_rx_copy_peer_macs(uint8_t mac_left[6], uint8_t mac_right[6]);

/* Signal quality derivation — pure function, host-testable.
 * Returns 0..255 link quality (255 = best, 0 = link down/timed out).
 * See rf_rx_task.c for the age/retry mapping. */
uint8_t rf_signal_q255(bool link_up, uint32_t hb_age_ms, uint8_t link_q);

/* Begin a pairing window (called from the CDC KS_CMD_RF_PAIR_START handler).
 * reset=1 → clear NVS mac_left/mac_right/paired_count first. Switches radio L to
 * the pairing rendezvous (RF_PAIR_ADDR/RF_PAIR_CHANNEL) PRX and starts a 30 s
 * window driven by rf_rx_task. Returns set_id (computed) and current paired_count. */
bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out);

#endif /* RF_RX_TASK_H */
