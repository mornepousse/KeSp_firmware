/* radio_owner: the invariants that broke three times in silence, verified
 * against the SEQUENCE of calls to the hardware (fake recorder). */
#include "test_framework.h"
#include "../main/comm/rf/radio_owner.h"
#include <string.h>
#include <stdio.h>

static char s_trace[512];
static int  s_pending_rx;          /* frames pending in the fake FIFO */
static bool s_ack = true;
#define T(s) strncat(s_trace, s, sizeof s_trace - strlen(s_trace) - 1)

static esp_err_t f_init_tx(rf_radio_t *r, const rf_radio_cfg_t *c) { (void)c; r->present = true; T("init_tx;"); return ESP_OK; }
static void f_set_ptx(rf_radio_t *r, const rf_radio_cfg_t *c)   { (void)r; char b[24]; snprintf(b, sizeof b, "ptx(%02X);", c->channel); T(b); }
static void f_rearm_rx(rf_radio_t *r, const rf_radio_cfg_t *c)  { (void)r; char b[24]; snprintf(b, sizeof b, "prx(%02X);", c->channel); T(b); }
static bool f_send(rf_radio_t *r, const uint8_t *b, uint8_t n)  { (void)r; (void)b; (void)n; T("send;"); return s_ack; }
static bool f_send_ap(rf_radio_t *r, const uint8_t *b, uint8_t n, uint8_t *a, uint8_t *al) { (void)r; (void)b; (void)n; (void)a; *al = 0; T("send_ap;"); return s_ack; }
static bool f_oob(rf_radio_t *r, uint8_t ch, const uint8_t a[5], const uint8_t *b, uint8_t n, uint8_t rch, const uint8_t ra[5])
{ (void)r; (void)a; (void)b; (void)n; (void)ra; char t[32]; snprintf(t, sizeof t, "oob(%02X->%02X);", ch, rch); T(t); return s_ack; }
static bool f_rx_avail(rf_radio_t *r) { (void)r; return s_pending_rx > 0; }
static uint16_t f_read_rx(rf_radio_t *r, uint8_t *b, uint16_t n) { (void)r; (void)n; if (!s_pending_rx) return 0; s_pending_rx--; b[0] = 0xAB; T("read;"); return 1; }
static void f_pd(rf_radio_t *r) { (void)r; T("pd;"); }
static void f_pu(rf_radio_t *r) { (void)r; T("pu;"); }
static void f_set_tx_address(rf_radio_t *r, const uint8_t a[5]) { (void)r; (void)a; T("addr;"); }
static void f_set_channel(rf_radio_t *r, uint8_t ch) { (void)r; char b[16]; snprintf(b, sizeof b, "ch(%02X);", ch); T(b); }
static uint16_t f_pair_listen(rf_radio_t *r, uint8_t ch, const uint8_t a[5], uint8_t *b, uint16_t n, uint32_t ms)
{ (void)r; (void)ch; (void)a; (void)n; (void)ms; T("listen;"); b[0] = 0x42; return s_pending_rx ? 1 : 0; }
static const radio_hw_t FAKE = { f_init_tx, f_set_ptx, f_rearm_rx, f_send, f_send_ap, f_oob, f_rx_avail, f_read_rx, f_pd, f_pu,
                                 f_set_tx_address, f_set_channel, f_pair_listen };

static rf_radio_cfg_t cfg(uint8_t ch) { rf_radio_cfg_t c; memset(&c, 0, sizeof c); c.channel = ch; c.addr_suffix = 0x01; return c; }
static void reset(void) { s_trace[0] = 0; s_pending_rx = 0; s_ack = true; }
static int s_livrees; static void compte(const uint8_t *t, uint16_t n, void *ctx) { (void)t; (void)n; (void)ctx; s_livrees++; }

static void test_init_est_ptx_vers_la_cible(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68);
    TEST_ASSERT(radio_owner_init(&d, &FAKE), "chip present");
    TEST_ASSERT(radio_mode() == RADIO_PTX, "PTX at start");
    TEST_ASSERT(radio_cible()->channel == 0x68, "towards the target");
    TEST_ASSERT(strcmp(s_trace, "init_tx;") == 0, s_trace);
}

static void test_un_seul_mode_a_la_fois_et_idempotent(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    TEST_ASSERT(radio_mode_set(RADIO_PRX, &l), "PRX");
    TEST_ASSERT(radio_mode() == RADIO_PRX && radio_cible()->channel == 0x4F, "listening on 4F");
    TEST_ASSERT(radio_mode_set(RADIO_PRX, &l), "PRX again");
    TEST_ASSERT(strcmp(s_trace, "prx(4F);") == 0, "idempotent: a single write");
    TEST_ASSERT(radio_mode_set(RADIO_PTX, &d), "back to PTX");
    TEST_ASSERT(strcmp(s_trace, "prx(4F);ptx(68);") == 0, s_trace);
}

static void test_rearmer_reecrit_le_mode_courant(void)
{
    /* Watchdog: a frozen chip re-arms by rewriting the SAME config —
     * something radio_mode_set's idempotence would refuse. */
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    TEST_ASSERT(radio_rearmer(), "re-armed");
    TEST_ASSERT(strcmp(s_trace, "ptx(68);") == 0, s_trace);
}

static void test_emettre_en_prx_est_refuse(void)
{
    /* The incident: transmitting while listening silently overwrites the PRX
     * config. The owner refuses; the caller must go through the excursion. */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0;
    uint8_t b[4] = {0}; uint8_t ack[32]; uint8_t al = 9;
    TEST_ASSERT(!radio_send(b, sizeof b, 20), "send refused in PRX");
    TEST_ASSERT(!radio_send_ap(b, sizeof b, ack, &al, 20) && al == 0, "send_ap refused in PRX, empty ack");
    TEST_ASSERT(strcmp(s_trace, "") == 0, "nothing touched the chip");
}

static void test_excursion_vide_la_fifo_AVANT_et_revient(void)
{
    /* The 2026-09-08 incident: the excursion used to end with FLUSH_RX and
     * destroyed already-ACKED frames. Required order: read* then oob(there->back). */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0; s_pending_rx = 2; s_livrees = 0;
    uint8_t b[4] = {0}; uint8_t addr[5] = {'K','a','S','e',0x01};
    TEST_ASSERT(radio_excursion_tx(0x68, addr, b, sizeof b, compte, NULL), "excursion ok");
    TEST_ASSERT(s_livrees == 2, "both frames delivered");
    TEST_ASSERT(strcmp(s_trace, "read;read;oob(68->4F);") == 0, s_trace);
    TEST_ASSERT(radio_mode() == RADIO_PRX && radio_cible()->channel == 0x4F, "still listening afterwards");
}

static void test_excursion_en_ptx_est_refusee(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    uint8_t b[4] = {0}; uint8_t addr[5] = {0};
    TEST_ASSERT(!radio_excursion_tx(0x4F, addr, b, sizeof b, NULL, NULL), "no excursion from PTX: send is enough");
    TEST_ASSERT(strcmp(s_trace, "") == 0, "nothing");
}

static void test_drain_livre_en_prx_seulement(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    s_pending_rx = 3; s_livrees = 0;
    radio_rx_drain(compte, NULL);
    TEST_ASSERT(s_livrees == 0 && s_pending_rx == 3, "in PTX: nothing to read, nothing read");
    radio_mode_set(RADIO_PRX, &l);
    radio_rx_drain(compte, NULL);
    TEST_ASSERT(s_livrees == 3 && s_pending_rx == 0, "in PRX: everything delivered");
}

static void test_reveil_rearme_le_mode(void)
{
    /* CLAUDE.md: power_up does not touch CE — a PRX used to wake up deaf. */
    reset(); rf_radio_cfg_t d = cfg(0x68), l = cfg(0x4F); radio_owner_init(&d, &FAKE);
    radio_mode_set(RADIO_PRX, &l); s_trace[0] = 0;
    radio_sleep(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;prx(4F);") == 0, s_trace);
    radio_mode_set(RADIO_PTX, &d); s_trace[0] = 0;
    radio_sleep(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;ptx(68);") == 0, "PTX also re-arms");
}

static void test_le_verrou_est_tenu_pendant_le_sommeil(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE);
    radio_sleep();
    TEST_ASSERT(!radio_lock(0), "no one transmits on a powered-off chip");
    uint8_t b[4] = {0};
    TEST_ASSERT(!radio_send(b, 4, 0), "send refused during sleep");
    radio_wake();
    TEST_ASSERT(radio_lock(0), "free on wake"); radio_unlock();
}

static void test_compteurs(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE);
    uint8_t b[4] = {0}; uint32_t ok, refus, indispo;
    radio_send(b, 4, 20); s_ack = false; radio_send(b, 4, 20); radio_send(b, 4, 20);
    radio_sleep(); radio_send(b, 4, 0); radio_wake();          /* asleep: unavailable, not a refusal */
    radio_stats(&ok, &refus, &indispo, NULL);
    TEST_ASSERT(ok == 1 && refus == 2 && indispo == 1, "1 ok, 2 ESB refusals, 1 unavailable");
}

static esp_err_t f_init_absent(rf_radio_t *r, const rf_radio_cfg_t *c) { (void)r; (void)c; T("init_tx;"); return ESP_OK; }
static void test_puce_absente(void)
{
    reset(); rf_radio_cfg_t d = cfg(0x68);
    radio_hw_t sans = FAKE; sans.init_tx = f_init_absent;   /* the probe got no reply: present stays false */
    TEST_ASSERT(!radio_owner_init(&d, &sans), "init says no");
    TEST_ASSERT(!radio_presente() && radio_mode() == RADIO_ETEINTE, "off");
    uint8_t b[4] = {0};
    TEST_ASSERT(!radio_send(b, 4, 20) && !radio_mode_set(RADIO_PTX, &d), "everything refused, without touching the chip");
    TEST_ASSERT(strcmp(s_trace, "init_tx;") == 0, s_trace);
}

static void test_pair_round_revient_a_la_cible(void)
{
    /* A pairing round targets the rendezvous then RETURNS to the current
     * target: a board that stayed on the rendezvous channel would no longer ack anything. */
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    uint8_t rdv[5] = {'P','A','I','R',0}; uint8_t req[8] = {0}; uint8_t rx[32]; uint16_t rxn = 99;
    s_pending_rx = 1;
    TEST_ASSERT(radio_pair_round(rdv, 0x4C, req, sizeof req, rx, sizeof rx, 150, &rxn), "round ok");
    TEST_ASSERT(rxn == 1 && rx[0] == 0x42, "response returned");
    TEST_ASSERT(strcmp(s_trace, "addr;ch(4C);send;listen;ptx(68);") == 0, s_trace);
    TEST_ASSERT(radio_mode() == RADIO_PTX && radio_cible()->channel == 0x68, "target restored");
}

static bool pred_faux_sous_verrou(void *ctx)
{
    /* The predicate must be evaluated AFTER taking the lock: otherwise a state
     * that changes between evaluation and transmission would still go through. */
    *(bool *)ctx = !radio_lock(0);
    return false;
}
static bool pred_vrai(void *ctx) { (void)ctx; return true; }

static void test_emission_perimee_n_est_pas_emise(void)
{
    /* The 2026-09-20 double press: a REPEAT of the last state (press) used to
     * fire after the release — the half had re-read the state before the
     * release overwrote it, then waited on the lock behind it. A transmission
     * whose state is stale by the time the lock is acquired must NOT touch
     * the chip. */
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    uint8_t b[4] = {0}; bool verrou_tenu = false; uint32_t ok, refus, indispo, perimes;
    TEST_ASSERT(radio_emettre(b, 4, NULL, NULL, 20, pred_faux_sous_verrou, &verrou_tenu) == RADIO_TX_PERIME, "stale");
    TEST_ASSERT(verrou_tenu, "predicate evaluated under the lock");
    TEST_ASSERT(strcmp(s_trace, "") == 0, "nothing touched the chip");
    TEST_ASSERT(radio_lock(0), "lock returned"); radio_unlock();
    TEST_ASSERT(radio_emettre(b, 4, NULL, NULL, 20, pred_vrai, NULL) == RADIO_TX_ACK, "valid: sent");
    TEST_ASSERT(strcmp(s_trace, "send;") == 0, s_trace);
    radio_stats(&ok, &refus, &indispo, &perimes);
    TEST_ASSERT(ok == 1 && refus == 0 && indispo == 0 && perimes == 1, "one stale, one acked");
}

static void test_sommeil_idempotent_et_verrou_rendu(void)
{
    /* Deep sleep re-calls the hooks after light sleep: sleeping twice must
     * neither block for 50 ms nor release a lock that was never taken. */
    reset(); rf_radio_cfg_t d = cfg(0x68); radio_owner_init(&d, &FAKE); s_trace[0] = 0;
    radio_sleep(); radio_sleep(); radio_wake(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;ptx(68);") == 0, s_trace);
    TEST_ASSERT(radio_lock(0), "lock free afterwards"); radio_unlock();
    /* Lock already held by someone else (a long transmission) when going to
     * sleep: we power off anyway, and waking does NOT release that lock. */
    s_trace[0] = 0;
    TEST_ASSERT(radio_lock(0), "someone else holds the lock");
    radio_sleep(); radio_wake();
    TEST_ASSERT(strcmp(s_trace, "pd;pu;ptx(68);") == 0, s_trace);
    TEST_ASSERT(!radio_lock(0), "the other one's lock is still held"); radio_unlock();
}

void test_radio_owner(void)
{
    TEST_SUITE("radio_owner: one chip, one owner");
    TEST_RUN(test_init_est_ptx_vers_la_cible);
    TEST_RUN(test_un_seul_mode_a_la_fois_et_idempotent);
    TEST_RUN(test_rearmer_reecrit_le_mode_courant);
    TEST_RUN(test_emettre_en_prx_est_refuse);
    TEST_RUN(test_excursion_vide_la_fifo_AVANT_et_revient);
    TEST_RUN(test_excursion_en_ptx_est_refusee);
    TEST_RUN(test_drain_livre_en_prx_seulement);
    TEST_RUN(test_reveil_rearme_le_mode);
    TEST_RUN(test_le_verrou_est_tenu_pendant_le_sommeil);
    TEST_RUN(test_compteurs);
    TEST_RUN(test_puce_absente);
    TEST_RUN(test_pair_round_revient_a_la_cible);
    TEST_RUN(test_emission_perimee_n_est_pas_emise);
    TEST_RUN(test_sommeil_idempotent_et_verrou_rendu);
}
