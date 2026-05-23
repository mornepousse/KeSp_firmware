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
} rf_link_status_t;

void rf_rx_get_status(rf_link_status_t *out);

/* Begin a pairing window (called from the CDC KS_CMD_RF_PAIR_START handler).
 * reset=1 → clear NVS mac_left/mac_right/paired_count first. Switches radio L to
 * the pairing rendezvous (RF_PAIR_ADDR/RF_PAIR_CHANNEL) PRX and starts a 30 s
 * window driven by rf_rx_task. Returns set_id (computed) and current paired_count. */
bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out);

#endif /* RF_RX_TASK_H */
