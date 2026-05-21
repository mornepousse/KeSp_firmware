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

#endif /* RF_RX_TASK_H */
