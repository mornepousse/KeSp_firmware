#ifndef RF_RX_TASK_H
#define RF_RX_TASK_H

#include <stdint.h>
#include <stdbool.h>

/* Start RF radios + rx task. Returns false if neither radio is present. */
bool rf_rx_start(void);

/* Link diagnostic exposed over CDC.
 *
 * The two slots are no longer two halves of the same keyboard: the first carries
 * the keyboard (Niphargus master half), the second the Conchodytes mouse. See
 * comm/rf/rf_slot.h. */
typedef struct {
    bool link_kbd, link_mouse;
    /* Age of the last packet received, whatever its kind — not only heartbeats:
     * an active link no longer sends any. */
    uint32_t age_kbd_ms, age_mouse_ms;
    uint32_t pkt_rx_kbd, pkt_rx_mouse;
    uint32_t pkt_dup_kbd, pkt_dup_mouse;
    /* Last link_q announced by each slot. 0 if nothing received yet
     * (conservative = best retransmission score). */
    uint8_t link_q_kbd;
    uint8_t link_q_mouse;
    /* PHYSICAL presence of the two nRF24 modules, as established by the SPI
     * probe at startup. Distinct from link state: a present radio
     * may have no peer, but an ABSENT radio will never listen to anything.
     *
     * Without this information, a dongle whose radio 2 is not mounted is
     * indistinguishable from a dongle whose mouse is out of range — and the
     * mouse, for its part, transmits into the void with nothing to say so. */
    bool radio_kbd_present;
    bool radio_mouse_present;
} rf_link_status_t;

void rf_rx_get_status(rf_link_status_t *out);

/* Copies the paired WiFi MACs of both slots (live copy: loaded at boot,
 * refreshed on every successful pairing), so as never to depend on a stale
 * NVS cache. An all-zero MAC = this slot is not paired. */
void rf_rx_copy_peer_macs(uint8_t mac_kbd[6], uint8_t mac_mouse[6]);

/* Signal quality derivation — pure function, host-testable.
 * Returns 0..255 link quality (255 = best, 0 = link down/timed out).
 * See rf_rx_task.c for the age/retry mapping. */
uint8_t rf_signal_q255(bool link_up, uint32_t hb_age_ms, uint8_t link_q);

/* Begin a pairing window (called from the CDC KS_CMD_RF_PAIR_START handler).
 * reset=1 → first clears the paired MACs and paired_count in NVS. Switches
 * radio 1 to the pairing rendezvous (RF_PAIR_ADDR/RF_PAIR_CHANNEL) in PRX and
 * opens a window driven by rf_rx_task. Returns the computed set_id and the
 * current paired_count. */
bool rf_rx_pair_start(uint8_t reset, uint16_t *set_id_out, uint8_t *paired_count_out);

#endif /* RF_RX_TASK_H */
