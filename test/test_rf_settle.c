/*
 * test_rf_settle.c — the dongle's RX settle (rf_settle.h) and the bench-knob
 * guard (rf_bench.h).
 *
 * Settle: on the clone nRF24 modules an SPI access to the PRX while its
 * auto-ACK goes out makes the ACK miss (rf_scope stress, 2026-10-21). The rx
 * task therefore waits, before each SPI access sequence on a radio, only what
 * remains of settle_us since THAT radio's last IRQ — zero when the IRQ is
 * older, zero for a pass the 1 ms timer woke with no recent IRQ, and never
 * for the other radio's IRQ. Commit 88d49728 waited 500 us unconditionally
 * on every pass of both radios: ~50 % of core 0 at rest (review C1).
 *
 * Bench guard: the three nRF24 knobs (FEATURE, SETUP_RETR, RX settle) are
 * bench experiments; a build_<board>/sdkconfig keeps whatever value was set there
 * (no default tracking), so a non-shipped value must be visible (boot log,
 * HB, trace periodic) and refused by scripts/build_release.sh. The shipped
 * values live in rf_bench.h and are pinned here against main/Kconfig.projbuild.
 */
#include "test_framework.h"
#include "rf_settle.h"
#include "rf_bench.h"
#include <stdio.h>
#include <string.h>

/* ── rf_settle_remaining_us ─────────────────────────────────────────── */

static void test_rf_settle_remaining(void)
{
    TEST_ASSERT_EQ(rf_settle_remaining_us(1000, 1000, 500), 500, "IRQ just now: the whole settle");
    TEST_ASSERT_EQ(rf_settle_remaining_us(1100, 1000, 500), 400, "100 us after the IRQ: 400 left");
    TEST_ASSERT_EQ(rf_settle_remaining_us(1499, 1000, 500), 1, "1 us left");
    TEST_ASSERT_EQ(rf_settle_remaining_us(1500, 1000, 500), 0, "settle elapsed exactly: no wait");
    TEST_ASSERT_EQ(rf_settle_remaining_us(5000, 1000, 500), 0, "IRQ long ago: no wait");
    /* The 32-bit microsecond clock wraps every 71.6 min. */
    TEST_ASSERT_EQ(rf_settle_remaining_us(0x00000064u, 0xFFFFFF00u, 500), 500 - 0x164,
                   "IRQ before the wrap, now after it: 356 us elapsed, 144 left");
    TEST_ASSERT_EQ(rf_settle_remaining_us(0x00001000u, 0xFFFFFF00u, 500), 0,
                   "IRQ before the wrap, long ago: no wait");
    TEST_ASSERT_EQ(rf_settle_remaining_us(1000, 1000, 0), 0, "settle 0 (halves, old behaviour): never waits");
    /* No IRQ since boot: the stamp is 0, the clock far ahead. */
    TEST_ASSERT_EQ(rf_settle_remaining_us(3000000u, 0, 500), 0, "never an IRQ: no wait");
}

/* ── rf_settle_gate with a fake clock ───────────────────────────────── */

typedef struct {
    uint32_t now;
    uint32_t waited;    /* sum of the waits asked */
    unsigned calls;     /* number of waits asked */
} fake_clk_t;

static uint32_t fake_now(void *ctx) { return ((fake_clk_t *)ctx)->now; }
static void fake_wait(void *ctx, uint32_t us)
{
    fake_clk_t *c = ctx;
    c->now += us;
    c->waited += us;
    c->calls++;
}

static void test_rf_settle_gate_waits_the_remainder(void)
{
    rf_settle_t r; rf_settle_init(&r);
    fake_clk_t c = { .now = 10000 };
    rf_settle_note_irq(&r, 9900);                         /* the IRQ, 100 us ago */
    TEST_ASSERT_EQ(rf_settle_gate(&r, 500, fake_now, fake_wait, &c), 400, "gate returns the wait");
    TEST_ASSERT_EQ(c.waited, 400, "waits only the remaining 400 us");
    TEST_ASSERT_EQ(c.now, 10400, "SPI may start 500 us after the IRQ");
    /* The next SPI sequence of the same pass, no new IRQ: no second wait. */
    TEST_ASSERT_EQ(rf_settle_gate(&r, 500, fake_now, fake_wait, &c), 0, "already settled: no wait");
    TEST_ASSERT_EQ(c.calls, 1, "the waiter is not even called when nothing remains");
}

static void test_rf_settle_gate_timer_pass_waits_nothing(void)
{
    /* The 1 ms timer wakes the task on both radios with no IRQ: the review's
     * C1 — 2 × 500 us per ms burnt at rest. */
    rf_settle_t kbd, mouse; rf_settle_init(&kbd); rf_settle_init(&mouse);
    fake_clk_t c = { .now = 7000000 };
    for (int pass = 0; pass < 1000; pass++) {
        rf_settle_gate(&kbd, 500, fake_now, fake_wait, &c);
        rf_settle_gate(&mouse, 500, fake_now, fake_wait, &c);
        c.now += 1000;
    }
    TEST_ASSERT_EQ(c.waited, 0, "a second of timer passes without IRQ waits 0 us");
    TEST_ASSERT_EQ(c.calls, 0, "and never calls the waiter");

    /* An old IRQ (> settle) does not count either. */
    rf_settle_note_irq(&kbd, c.now - 501);
    TEST_ASSERT_EQ(rf_settle_gate(&kbd, 500, fake_now, fake_wait, &c), 0, "IRQ 501 us ago: no wait");
}

static void test_rf_settle_gate_never_waits_for_the_other_radio(void)
{
    rf_settle_t kbd, mouse; rf_settle_init(&kbd); rf_settle_init(&mouse);
    fake_clk_t c = { .now = 50000 };
    rf_settle_note_irq(&mouse, 50000);                    /* the mouse's IRQ, now */
    TEST_ASSERT_EQ(rf_settle_gate(&kbd, 500, fake_now, fake_wait, &c), 0,
                   "keyboard radio: the mouse's IRQ is not its business");
    TEST_ASSERT_EQ(rf_settle_gate(&mouse, 500, fake_now, fake_wait, &c), 500,
                   "mouse radio: waits its own settle");
    /* And the other way round, interleaved: each pays only its own. */
    rf_settle_note_irq(&kbd, c.now);
    c.now += 200;
    TEST_ASSERT_EQ(rf_settle_gate(&mouse, 500, fake_now, fake_wait, &c), 0,
                   "mouse: its IRQ is 700 us old, the keyboard's is not counted");
    TEST_ASSERT_EQ(rf_settle_gate(&kbd, 500, fake_now, fake_wait, &c), 300,
                   "keyboard: 300 us left of its own");
}

static void test_rf_settle_gate_follows_a_new_irq_mid_pass(void)
{
    /* A frame landing during the pass (after the pass cleared RX_DR, so it
     * raises a NEW edge): the next SPI sequence waits for ITS ACK. */
    rf_settle_t r; rf_settle_init(&r);
    fake_clk_t c = { .now = 1000 };
    rf_settle_note_irq(&r, 1000);
    rf_settle_gate(&r, 500, fake_now, fake_wait, &c);     /* 1500 */
    c.now += 80;                                           /* reading frame A */
    rf_settle_note_irq(&r, c.now - 30);                   /* B arrived 30 us ago */
    TEST_ASSERT_EQ(rf_settle_gate(&r, 500, fake_now, fake_wait, &c), 470, "waits for B's ACK");
}

static void test_rf_settle_gate_settle_zero(void)
{
    rf_settle_t r; rf_settle_init(&r);
    fake_clk_t c = { .now = 1000 };
    rf_settle_note_irq(&r, 1000);
    TEST_ASSERT_EQ(rf_settle_gate(&r, 0, fake_now, fake_wait, &c), 0, "settle 0: the gate is open");
    TEST_ASSERT_EQ(c.calls, 0, "no waiter call");
}

/* ── rf_bench ───────────────────────────────────────────────────────── */

static void test_rf_bench_flags(void)
{
    TEST_ASSERT_EQ(rf_bench_flags(RF_SHIPPED_FEATURE, RF_SHIPPED_SETUP_RETR, 500, 500), 0,
                   "shipped values: nothing to flag");
    TEST_ASSERT_EQ(rf_bench_flags(0x04, RF_SHIPPED_SETUP_RETR, 500, 500), RF_BENCH_FEATURE,
                   "FEATURE 0x04 (no ACK payload): flagged");
    TEST_ASSERT_EQ(rf_bench_flags(RF_SHIPPED_FEATURE, 0x2F, 500, 500), RF_BENCH_RETR,
                   "SETUP_RETR 0x2F: flagged");
    TEST_ASSERT_EQ(rf_bench_flags(RF_SHIPPED_FEATURE, RF_SHIPPED_SETUP_RETR, 200, 500), RF_BENCH_SETTLE,
                   "settle 200 us on the dongle: flagged");
    TEST_ASSERT_EQ(rf_bench_flags(0x07, 0x3F, 0, 500), RF_BENCH_FEATURE | RF_BENCH_RETR | RF_BENCH_SETTLE,
                   "all three");
    TEST_ASSERT_EQ(rf_bench_flags(RF_SHIPPED_FEATURE, RF_SHIPPED_SETUP_RETR, 0, 0), 0,
                   "a half: no settle knob, 0 == 0");
}

static void test_rf_bench_describe(void)
{
    char b[64];
    rf_bench_describe(0, RF_SHIPPED_FEATURE, RF_SHIPPED_SETUP_RETR, 500, b, sizeof b);
    TEST_ASSERT(strcmp(b, "") == 0, "shipped: empty suffix (the HB stays as it was)");
    rf_bench_describe(RF_BENCH_FEATURE | RF_BENCH_SETTLE, 0x04, RF_SHIPPED_SETUP_RETR, 200, b, sizeof b);
    TEST_ASSERT(strcmp(b, " BENCH-RF(feature=0x04 settle=200us)") == 0, "names the knobs that differ, and only those");
    rf_bench_describe(RF_BENCH_RETR, RF_SHIPPED_FEATURE, 0x2F, 500, b, sizeof b);
    TEST_ASSERT(strcmp(b, " BENCH-RF(retr=0x2F)") == 0, "retr alone");
    char small[8];
    rf_bench_describe(RF_BENCH_RETR, RF_SHIPPED_FEATURE, 0x2F, 500, small, sizeof small);
    TEST_ASSERT(strlen(small) < sizeof small, "truncates, never overflows");
}

/* The shipped values in rf_bench.h are the Kconfig defaults: change one
 * without the other and the guard flags a release build — or worse, stops
 * flagging a bench one. Also M1: FEATURE's range keeps EN_DPL (bit 2). */
static const char *kconfig_block(const char *text, const char *sym, char *out, size_t cap)
{
    char key[96];
    snprintf(key, sizeof key, "config %s\n", sym);
    const char *p = strstr(text, key);
    if (!p) return NULL;
    const char *end = strstr(p + strlen(key), "\nconfig ");
    size_t n = end ? (size_t)(end - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
    return out;
}

static void test_rf_bench_shipped_match_kconfig(void)
{
    FILE *f = fopen(KCONFIG_PROJBUILD, "rb");
    TEST_ASSERT(f != NULL, "main/Kconfig.projbuild readable");
    if (!f) return;
    static char text[256 * 1024];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    char blk[4096], want[64];

    TEST_ASSERT(kconfig_block(text, "KASE_RF_FEATURE", blk, sizeof blk) != NULL, "KASE_RF_FEATURE exists");
    snprintf(want, sizeof want, "default 0x%02X\n", RF_SHIPPED_FEATURE);
    TEST_ASSERT(strstr(blk, want) != NULL, "KASE_RF_FEATURE default == RF_SHIPPED_FEATURE");
    TEST_ASSERT(strstr(blk, "range 0x04 0x07\n") != NULL, "KASE_RF_FEATURE range keeps EN_DPL (bit 2) set");

    TEST_ASSERT(kconfig_block(text, "KASE_RF_SETUP_RETR", blk, sizeof blk) != NULL, "KASE_RF_SETUP_RETR exists");
    snprintf(want, sizeof want, "default 0x%02X\n", RF_SHIPPED_SETUP_RETR);
    TEST_ASSERT(strstr(blk, want) != NULL, "KASE_RF_SETUP_RETR default == RF_SHIPPED_SETUP_RETR");

    TEST_ASSERT(kconfig_block(text, "KASE_RF_RX_SETTLE_US", blk, sizeof blk) != NULL, "KASE_RF_RX_SETTLE_US exists");
    snprintf(want, sizeof want, "default %u\n", (unsigned)RF_SHIPPED_RX_SETTLE_US);
    TEST_ASSERT(strstr(blk, want) != NULL, "KASE_RF_RX_SETTLE_US default == RF_SHIPPED_RX_SETTLE_US");
    TEST_ASSERT(strstr(blk, "depends on KASE_DEVICE_ROLE_DONGLE\n") != NULL,
                "KASE_RF_RX_SETTLE_US exists on the dongle only (M2)");
}

void test_rf_settle(void)
{
    TEST_SUITE("rf_settle (dongle RX settle) + rf_bench (bench-knob guard)");
    TEST_RUN(test_rf_settle_remaining);
    TEST_RUN(test_rf_settle_gate_waits_the_remainder);
    TEST_RUN(test_rf_settle_gate_timer_pass_waits_nothing);
    TEST_RUN(test_rf_settle_gate_never_waits_for_the_other_radio);
    TEST_RUN(test_rf_settle_gate_follows_a_new_irq_mid_pass);
    TEST_RUN(test_rf_settle_gate_settle_zero);
    TEST_RUN(test_rf_bench_flags);
    TEST_RUN(test_rf_bench_describe);
    TEST_RUN(test_rf_bench_shipped_match_kconfig);
}
