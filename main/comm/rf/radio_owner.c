/* The nRF24 chip of a Niphargus half — and nothing else. See radio_owner.h. */
#include "radio_owner.h"
#include <string.h>
#include <stdio.h>

#ifndef TEST_HOST
#include "rf_bus.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"
#endif
static const char *TAG = "radio";
static SemaphoreHandle_t s_mux;
static bool lock_take(uint32_t ms) { return s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void lock_give(void)        { if (s_mux) xSemaphoreGive(s_mux); }
static void lock_create(void)      { if (!s_mux) s_mux = xSemaphoreCreateMutex(); }
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
/* Host: the lock is a boolean — the "held during sleep" semantics
 * is testable without FreeRTOS. */
static bool s_tenu;
static bool lock_take(uint32_t ms) { (void)ms; if (s_tenu) return false; s_tenu = true; return true; }
static void lock_give(void)        { s_tenu = false; }
static void lock_create(void)      { s_tenu = false; }
#define LOGW(...) ((void)0)
#endif

static rf_radio_t     s_radio;
static radio_hw_t     s_hw;
static radio_mode_t   s_mode;
static rf_radio_cfg_t s_cible;
static uint32_t       s_ok, s_refus, s_indispo, s_perimes;
static bool           s_endormie, s_verrou_sommeil;

/* ── rf-scope: one TX record per transmission ─────────────────────────────
 * Built here because the owner is the only module that sees every
 * transmission, under the lock, with the frame in hand (seq, kind) and the
 * driver's verdict (STATUS, OBSERVE_TX). Tracing off costs ONE load of
 * g_rf_trace_session and a branch per transmission; the excursion's extra
 * OBSERVE_TX read happens only while tracing. Any frame sent through here —
 * a future stress frame included — gets its record the same way. */
#if defined(TEST_HOST) || CONFIG_KASE_RF_TRACE_HALF
#define RADIO_TRACE 1
#include "rf_trace_half.h"
#include "rf_trace_uart.h"
static rf_trace_plos_t s_plos;
static rf_plos_reset_t s_plos_reset;   /* RF_CH written since the last traced read (zeroes PLOS_CNT) */
#define RF_CH_ECRIT() (s_plos_reset = RF_PLOS_RESET_SINCE)

static void tracer_tx(uint32_t session, const uint8_t *buf, uint8_t len, bool ok,
                      uint8_t canal, bool excursion)   /* lock held */
{
    rf_trace_tx_t r;
    memset(&r, 0, sizeof r);
    rf_trace_ident_t id;                   /* the dongle's own reading of a frame (rf_trace_ctl.c) */
    rf_trace_frame_ident(buf, len, false, &id);
    r.pkt_kind = id.pkt_kind; r.seq = id.seq;
    if (!id.has_seq) r.flags |= RF_TRACE_TXF_NOSEQ;
    if (excursion) r.flags |= RF_TRACE_TXF_EXCURSION;
    r.payload_len = len;
    r.result = rf_trace_tx_result(ok, s_radio.tx_status);
    /* An excursion wrote RF_CH just before its packet (the far channel): its
     * PLOS_CNT, read before the return, is this packet's alone. */
    r.plos_delta = rf_trace_observe_step(&s_plos, s_radio.tx_observe, session,
                                         excursion ? RF_PLOS_RESET_NOW : s_plos_reset, &r.arc);
    s_plos_reset = excursion ? RF_PLOS_RESET_SINCE : RF_PLOS_NO_RESET;   /* the return wrote RF_CH again */
    r.channel = canal;
    rf_trace_uart_tx(&r);   /* stamps t_us and the emitter, copies, never blocks */
}
#else
#define RADIO_TRACE 0
#define RF_CH_ECRIT() ((void)0)
#endif

#ifndef TEST_HOST
static const radio_hw_t radio_hw_defaut = {
    .init_tx = rf_driver_init_tx,   .set_ptx = rf_driver_set_ptx,       .rearm_rx = rf_driver_rearm_rx,
    .send = rf_driver_send,         .send_ap = rf_driver_send_ap,       .oob_tx = rf_driver_oob_tx,
    .rx_available = rf_driver_rx_available, .read_rx = rf_driver_read_rx,
    .power_down = rf_driver_power_down,     .power_up = rf_driver_power_up,
    .set_tx_address = rf_driver_set_tx_address, .set_channel = rf_driver_set_channel,
    .pair_listen = rf_driver_pair_listen,
};
#endif

bool radio_owner_init(const rf_radio_cfg_t *cible, const radio_hw_t *hw)
{
#ifndef TEST_HOST
    s_hw = hw ? *hw : radio_hw_defaut;
#else
    s_hw = *hw;
#endif
    memset(&s_radio, 0, sizeof s_radio);
    s_mode = RADIO_ETEINTE; s_ok = s_refus = s_indispo = s_perimes = 0;
    s_endormie = s_verrou_sommeil = false;
#if RADIO_TRACE
    memset(&s_plos, 0, sizeof s_plos);
#endif
    RF_CH_ECRIT();
    lock_create();
    if (s_hw.init_tx(&s_radio, cible) != ESP_OK || !s_radio.present) return false;
    s_cible = *cible; s_mode = RADIO_PTX;
#if !defined(TEST_HOST) && CONFIG_KASE_VEILLE
    static const veille_hook_t hook = { "radio", radio_sleep, radio_wake, NULL };
    veille_hook_enregistrer(&hook);
#endif
#if !defined(TEST_HOST) && CONFIG_KASE_RF_TRACE_HALF
    rf_trace_uart_init();   /* AFTER the radio's hook: SLEEPING follows the power-down */
#endif
    return true;
}
bool radio_presente(void) { return s_radio.present; }
bool radio_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void radio_unlock(void) { lock_give(); }
#ifndef TEST_HOST
/* Lending the SPI bus to the display (rf_bus.h): the same lock. */
bool rf_bus_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void rf_bus_unlock(void) { lock_give(); }
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
#endif

static bool meme_cible(const rf_radio_cfg_t *a, const rf_radio_cfg_t *b)
{
    return a->channel == b->channel && a->addr_suffix == b->addr_suffix
        && memcmp(a->rx_addr, b->rx_addr, sizeof a->rx_addr) == 0;
}

static void appliquer(radio_mode_t mode, const rf_radio_cfg_t *cfg)   /* lock held */
{
    if (mode == RADIO_PTX) s_hw.set_ptx(&s_radio, cfg);
    else                   s_hw.rearm_rx(&s_radio, cfg);
    s_mode = mode; s_cible = *cfg;
    RF_CH_ECRIT();
}

bool radio_mode_set(radio_mode_t mode, const rf_radio_cfg_t *cfg)
{
    if (!s_radio.present || mode == RADIO_ETEINTE) return false;
    if (!lock_take(50)) return false;
    if (!(mode == s_mode && meme_cible(cfg, &s_cible))) appliquer(mode, cfg);
    lock_give();
    return true;
}
bool radio_rearmer(void)
{
    if (!s_radio.present || s_mode == RADIO_ETEINTE) return false;
    if (!lock_take(50)) return false;
    appliquer(s_mode, &s_cible);
    lock_give();
    return true;
}
radio_mode_t          radio_mode(void)  { return s_mode; }
const rf_radio_cfg_t *radio_cible(void) { return &s_cible; }   /* tests / diagnostics only */

radio_tx_t radio_emettre(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len,
                         uint32_t timeout_ms, radio_valide_cb_t encore_valide, void *ctx)
{
    if (ack_len) *ack_len = 0;
    if (!s_radio.present || s_mode != RADIO_PTX) { s_indispo++; return RADIO_TX_INDISPO; }   /* in PRX: excursion */
    if (!lock_take(timeout_ms)) { s_indispo++; return RADIO_TX_INDISPO; }
    if (encore_valide && !encore_valide(ctx)) {   /* UNDER the lock: was the state overtaken while waiting? */
        lock_give(); s_perimes++; return RADIO_TX_PERIME;
    }
    bool ok = (ack && ack_len) ? s_hw.send_ap(&s_radio, buf, len, ack, ack_len)
                               : s_hw.send(&s_radio, buf, len);
    if (ok) s_ok++; else s_refus++;
#if RADIO_TRACE
    uint32_t session = g_rf_trace_session;
    if (session) tracer_tx(session, buf, len, ok, s_cible.channel, false);
#endif
    lock_give();
    return ok ? RADIO_TX_ACK : RADIO_TX_REFUS;
}
bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{ return radio_emettre(buf, len, NULL, NULL, timeout_ms, NULL, NULL) == RADIO_TX_ACK; }
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms)
{ return radio_emettre(buf, len, ack, ack_len, timeout_ms, NULL, NULL) == RADIO_TX_ACK; }

static void vider(radio_rx_cb_t cb, void *ctx)   /* lock held, PRX mode */
{
    uint8_t b[32]; uint32_t perdues = 0;
    while (s_hw.rx_available(&s_radio)) {
        uint16_t n = s_hw.read_rx(&s_radio, b, sizeof b);
        if (!n) break;
        if (cb) cb(b, n, ctx); else perdues++;
    }
    if (perdues) LOGW("%lu frame(s) read with no consumer before the excursion", (unsigned long)perdues);
}
void radio_rx_drain(radio_rx_cb_t cb, void *ctx)
{
    if (!s_radio.present || s_mode != RADIO_PRX || !lock_take(20)) return;
    vider(cb, ctx);
    lock_give();
}
bool radio_excursion_tx(uint8_t canal, const uint8_t addr[5], const uint8_t *buf, uint8_t len,
                        radio_rx_cb_t cb, void *ctx)
{
    if (!s_radio.present || s_mode != RADIO_PRX) return false;   /* from PTX: radio_send */
    if (!lock_take(20)) return false;
    vider(cb, ctx);                                               /* BEFORE: the excursion ends with FLUSH_RX */
    uint8_t retour[5] = { s_cible.rx_addr[0], s_cible.rx_addr[1], s_cible.rx_addr[2], s_cible.rx_addr[3],
                          s_cible.addr_suffix };
#if RADIO_TRACE
    uint32_t session = g_rf_trace_session;
    s_radio.trace_obs = session != 0;   /* the driver reads OBSERVE_TX only while tracing */
#endif
    bool ok = s_hw.oob_tx(&s_radio, canal, addr, buf, len, s_cible.channel, retour);
    if (ok) s_ok++; else s_refus++;
#if RADIO_TRACE
    if (session) tracer_tx(session, buf, len, ok, canal, true);
    else         RF_CH_ECRIT();
#endif
    lock_give();
    return ok;
}

bool radio_pair_round(const uint8_t rdv_addr[5], uint8_t rdv_ch, const uint8_t *req, uint8_t n,
                      uint8_t *rx, uint16_t rx_max, uint32_t listen_ms, uint16_t *rx_n)
{
    *rx_n = 0;
    if (!s_radio.present || s_mode != RADIO_PTX) return false;
    if (!lock_take(200)) return false;
    s_hw.set_tx_address(&s_radio, rdv_addr);
    s_hw.set_channel(&s_radio, rdv_ch);
    s_hw.send(&s_radio, req, n);      /* pairing: not traced (not the link); appliquer() marks RF_CH */
    *rx_n = s_hw.pair_listen(&s_radio, rdv_ch, rdv_addr, rx, rx_max, listen_ms);
    appliquer(s_mode, &s_cible);      /* RETURN to the target, ACK or not */
    lock_give();
    return true;
}

#if CONFIG_KASE_RF_CE_SCAN
void radio_ce_gpio(int gpio) { if (lock_take(50)) { s_radio.cfg.pin_ce = gpio; s_cible.pin_ce = gpio; lock_give(); } }
#endif

void radio_sleep(void)
{
    if (!s_radio.present || s_endormie) return;   /* deep sleep calls the hooks again after light sleep */
    s_verrou_sommeil = lock_take(50);             /* HELD for the whole sleep */
    if (!s_verrou_sommeil) LOGW("sleep without the lock: a send lasting more than 50 ms was holding it");
    s_hw.power_down(&s_radio);
    s_endormie = true;
}
void radio_wake(void)
{
    if (!s_radio.present || !s_endormie) return;
    s_hw.power_up(&s_radio);
    if (s_mode != RADIO_ETEINTE) appliquer(s_mode, &s_cible);   /* power_up does not touch CE */
    if (s_verrou_sommeil) lock_give();            /* never release a lock we did not take */
    s_verrou_sommeil = false; s_endormie = false;
}
void radio_stats(uint32_t *ok, uint32_t *refus, uint32_t *indispo, uint32_t *perimes)
{ if (ok) *ok = s_ok; if (refus) *refus = s_refus; if (indispo) *indispo = s_indispo; if (perimes) *perimes = s_perimes; }
