#pragma once
/* radio_owner — the nRF24 chip of a Niphargus half has ONE owner.
 *
 * Three "wrong channel in silence" failures and one FIFO flushed on return
 * from an excursion (CLAUDE.md "One chip, one owner", "An ESB acknowledgment
 * does not prove software reception"): all modules that wrote
 * the chip's config each on their own. Here there is one state (mode + target),
 * one lock, and the invariants are functions:
 *   - one mode at a time: PTX toward a target, PRX listening to a target,
 *     off; changing mode is idempotent;
 *   - sending while in PRX is REFUSED — that's what the excursion is for;
 *   - an excursion EMPTIES the receive FIFO into the consumer BEFORE
 *     leaving (it ends with a FLUSH_RX) and comes back to listen to the
 *     previous target;
 *   - waking up RE-ARMS the current mode (rf_driver_power_up does not touch
 *     CE: without that a PRX comes back deaf);
 *   - the lock is held throughout sleep.
 * The module talks to the hardware through an operations table (radio_hw_t):
 * rf_driver_* in production, a fake recorder in host tests — the
 * SEQUENCE of calls is the oracle (test/test_radio_owner.c).
 *
 * This module knows neither frames nor policies: half_link.c (the
 * right) and kbd_relay_tx.c (the left) decide WHAT to send and TO WHOM;
 * it guarantees HOW the chip is touched. */
#include <stdbool.h>
#include <stdint.h>
#include "rf_driver.h"

typedef enum { RADIO_ETEINTE = 0, RADIO_PTX, RADIO_PRX } radio_mode_t;

/* Hardware operations table. NULL at init → rf_driver_*. */
typedef struct {
    esp_err_t (*init_tx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*set_ptx)(rf_radio_t *, const rf_radio_cfg_t *);
    void      (*rearm_rx)(rf_radio_t *, const rf_radio_cfg_t *);
    bool      (*send)(rf_radio_t *, const uint8_t *, uint8_t);
    bool      (*send_ap)(rf_radio_t *, const uint8_t *, uint8_t, uint8_t *, uint8_t *);
    bool      (*oob_tx)(rf_radio_t *, uint8_t, const uint8_t[5], const uint8_t *, uint8_t,
                        uint8_t, const uint8_t[5]);
    bool      (*rx_available)(rf_radio_t *);
    uint16_t  (*read_rx)(rf_radio_t *, uint8_t *, uint16_t);
    void      (*power_down)(rf_radio_t *);
    void      (*power_up)(rf_radio_t *);
    void      (*set_tx_address)(rf_radio_t *, const uint8_t[5]);
    void      (*set_channel)(rf_radio_t *, uint8_t);
    uint16_t  (*pair_listen)(rf_radio_t *, uint8_t, const uint8_t[5], uint8_t *, uint16_t, uint32_t);
} radio_hw_t;

typedef void (*radio_rx_cb_t)(const uint8_t *trame, uint16_t n, void *ctx);

/* Init: the chip in PTX toward `cible`. Registers the "radio" sleep hook
 * (sleep: power-down, lock kept; wake: power-up + re-arm).
 * Returns false if the chip is absent (probe): everything else then refuses. */
bool radio_owner_init(const rf_radio_cfg_t *cible, const radio_hw_t *hw);
bool radio_presente(void);

/* Chip lock: EVERY transaction, CSN included. It is also the loan of the
 * SPI bus to the screen — rf_bus_lock/unlock/host (rf_bus.h) are implemented here,
 * once for both halves. */
bool radio_lock(uint32_t timeout_ms);
void radio_unlock(void);

/* Mode. PTX toward `cfg` (channel/address) or PRX listening to `cfg`. Idempotent
 * if already in that mode toward that target. Takes the lock (50 ms). */
bool radio_mode_set(radio_mode_t mode, const rf_radio_cfg_t *cfg);
/* Rewrites the current mode's config even if nothing changed: the watchdog
 * for a frozen chip (nRF24 clone that stops acknowledging anything until reset). */
bool radio_rearmer(void);
radio_mode_t          radio_mode(void);
/* Oracle for host tests and diagnostics: NON-atomic read of the live state.
 * A policy does not decide based on it — it knows what it asked for. */
const rf_radio_cfg_t *radio_cible(void);

/* Transmission — PTX ONLY (refused in PRX: go through the excursion).
 *   ACK      : sent and acknowledged;
 *   REFUS    : sent, the ESB refused it (MAX_RT);
 *   INDISPO  : nothing was sent — lock taken under timeout_ms, wrong mode,
 *              chip asleep or absent;
 *   PERIME   : nothing was sent — `encore_valide(ctx)`, evaluated ONCE THE
 *              LOCK IS ACQUIRED, said the state to send is no longer current.
 * It's this last case that closes the race "a key-repeat sent after
 * the release" (double press on a short press, bench 2026-09-20): a
 * repeat snapshots the state and its generation, then waits for the lock behind
 * the release's transmission; without this check it would send the stale press.
 * `ack`/`ack_len` NULL → no ACK payload; otherwise EN_ACK_PAY, *ack_len = 0 if
 * the ACK was bare or nothing was sent. `encore_valide` NULL → always valid. */
typedef enum { RADIO_TX_ACK = 0, RADIO_TX_REFUS, RADIO_TX_INDISPO, RADIO_TX_PERIME } radio_tx_t;
typedef bool (*radio_valide_cb_t)(void *ctx);
radio_tx_t radio_emettre(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len,
                         uint32_t timeout_ms, radio_valide_cb_t encore_valide, void *ctx);
/* Shortcuts: true if ACK. */
bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms);
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms);

/* PRX: read everything pending and deliver it to `cb`. Nothing in PTX. */
void radio_rx_drain(radio_rx_cb_t cb, void *ctx);

/* PRX: transmit ELSEWHERE (channel/address) then come back to listen to the
 * current target. Empties the FIFO into `cb` BEFORE leaving (cb NULL = frames lost,
 * and it says so). Refused in PTX (radio_send is enough). */
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx);

/* One pairing round: aim at the rendezvous (address + channel), transmit `req`,
 * listen `listen_ms` for the response (returned in rx/rx_n), then COME BACK to the
 * current target — no matter what. Under lock. PTX only. */
bool radio_pair_round(const uint8_t rdv_addr[5], uint8_t rdv_ch, const uint8_t *req, uint8_t n,
                      uint8_t *rx, uint16_t rx_max, uint32_t listen_ms, uint16_t *rx_n);

#if CONFIG_KASE_RF_CE_SCAN
/* V2D bench only: try another CE pin (uncertain wiring). */
void radio_ce_gpio(int gpio);
#endif

/* Sleep: called by the hook registered at init (public for tests).
 * Idempotent (deep sleep calls the hooks again after light sleep); the
 * lock is only returned on wake if it was taken at sleep. */
void radio_sleep(void);
void radio_wake(void);

/* Since boot: transmissions acknowledged, refused by the ESB (MAX_RT),
 * UNAVAILABLE (lock taken, wrong mode, chip asleep: nothing was sent)
 * and STALE (state outdated at lock time: nothing was sent). NULL ok. */
void radio_stats(uint32_t *ok, uint32_t *refus, uint32_t *indispo, uint32_t *perimes);
