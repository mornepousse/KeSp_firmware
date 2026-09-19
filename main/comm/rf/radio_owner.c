/* La puce nRF24 d'une moitié du Niphargus — et rien d'autre. Voir radio_owner.h. */
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
/* Host : le verrou est un booléen — la sémantique « tenu pendant le sommeil »
 * se teste sans FreeRTOS. */
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
static uint32_t       s_ok, s_refus;

#ifndef TEST_HOST
static const radio_hw_t radio_hw_defaut = {
    .init_tx = rf_driver_init_tx,   .set_ptx = rf_driver_set_ptx,       .rearm_rx = rf_driver_rearm_rx,
    .send = rf_driver_send,         .send_ap = rf_driver_send_ap,       .oob_tx = rf_driver_oob_tx,
    .rx_available = rf_driver_rx_available, .read_rx = rf_driver_read_rx,
    .power_down = rf_driver_power_down,     .power_up = rf_driver_power_up,
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
    s_mode = RADIO_ETEINTE; s_ok = s_refus = 0;
    lock_create();
    if (s_hw.init_tx(&s_radio, cible) != ESP_OK || !s_radio.present) return false;
    s_cible = *cible; s_mode = RADIO_PTX;
#if !defined(TEST_HOST) && CONFIG_KASE_VEILLE
    static const veille_hook_t hook = { "radio", radio_sleep, radio_wake };
    veille_hook_enregistrer(&hook);
#endif
    return true;
}
bool radio_presente(void) { return s_radio.present; }
bool radio_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void radio_unlock(void) { lock_give(); }
#ifndef TEST_HOST
/* Prêt du bus SPI à l'écran (rf_bus.h) : le même verrou. */
bool rf_bus_lock(uint32_t timeout_ms) { return lock_take(timeout_ms); }
void rf_bus_unlock(void) { lock_give(); }
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
#endif

static bool meme_cible(const rf_radio_cfg_t *a, const rf_radio_cfg_t *b)
{
    return a->channel == b->channel && a->addr_suffix == b->addr_suffix
        && memcmp(a->rx_addr, b->rx_addr, sizeof a->rx_addr) == 0;
}

static void appliquer(radio_mode_t mode, const rf_radio_cfg_t *cfg)   /* verrou tenu */
{
    if (mode == RADIO_PTX) s_hw.set_ptx(&s_radio, cfg);
    else                   s_hw.rearm_rx(&s_radio, cfg);
    s_mode = mode; s_cible = *cfg;
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
const rf_radio_cfg_t *radio_cible(void) { return &s_cible; }

bool radio_send(const uint8_t *buf, uint8_t len, uint32_t timeout_ms)
{
    if (!s_radio.present || s_mode != RADIO_PTX) return false;   /* en PRX : excursion */
    if (!lock_take(timeout_ms)) return false;
    bool ok = s_hw.send(&s_radio, buf, len);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}
bool radio_send_ap(const uint8_t *buf, uint8_t len, uint8_t *ack, uint8_t *ack_len, uint32_t timeout_ms)
{
    *ack_len = 0;
    if (!s_radio.present || s_mode != RADIO_PTX) return false;
    if (!lock_take(timeout_ms)) return false;
    bool ok = s_hw.send_ap(&s_radio, buf, len, ack, ack_len);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}

static void vider(radio_rx_cb_t cb, void *ctx)   /* verrou tenu, mode PRX */
{
    uint8_t b[32]; uint32_t perdues = 0;
    while (s_hw.rx_available(&s_radio)) {
        uint16_t n = s_hw.read_rx(&s_radio, b, sizeof b);
        if (!n) break;
        if (cb) cb(b, n, ctx); else perdues++;
    }
    if (perdues) LOGW("%lu trame(s) lue(s) sans consommateur avant l'excursion", (unsigned long)perdues);
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
    if (!s_radio.present || s_mode != RADIO_PRX) return false;   /* depuis PTX : radio_send */
    if (!lock_take(20)) return false;
    vider(cb, ctx);                                               /* AVANT : l'excursion finit par FLUSH_RX */
    uint8_t retour[5] = { s_cible.rx_addr[0], s_cible.rx_addr[1], s_cible.rx_addr[2], s_cible.rx_addr[3],
                          s_cible.addr_suffix };
    bool ok = s_hw.oob_tx(&s_radio, canal, addr, buf, len, s_cible.channel, retour);
    if (ok) s_ok++; else s_refus++;
    lock_give();
    return ok;
}

void radio_sleep(void)
{
    if (!s_radio.present) return;
    (void)lock_take(50);              /* GARDÉ pendant tout le sommeil */
    s_hw.power_down(&s_radio);
}
void radio_wake(void)
{
    if (!s_radio.present) return;
    s_hw.power_up(&s_radio);
    if (s_mode != RADIO_ETEINTE) appliquer(s_mode, &s_cible);   /* power_up ne touche pas à CE */
    lock_give();
}
void radio_stats(uint32_t *ok, uint32_t *refus) { if (ok) *ok = s_ok; if (refus) *refus = s_refus; }
