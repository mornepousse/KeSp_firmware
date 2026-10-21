/* Per-key debounce of the matrix driver — pure state machine
 * (components/keyboard_button/include/kbd_debounce.h).
 *
 * Two modes, chosen per board (CONFIG_KASE_DEBOUNCE_EAGER_PRESS):
 *  - symmetric (V1/V2/V2D, the historical driver): a change is reported after
 *    `ticks` consecutive scans that disagree with the reported level;
 *  - eager press / deferred release (Niphargus halves, QMK's
 *    asym_eager_defer_pk): a PRESS is reported on the FIRST scan that sees
 *    the key down, then the key ignores its contacts for `ticks` scans
 *    (lockout); a RELEASE is reported after `ticks` consecutive up scans,
 *    a down scan in between cancelling it (false alarm).
 * Failure mode of eager, by design: a single-scan glitch on a key at rest is
 * reported (a phantom tap of ~3 scans); the symmetric mode filters it. */
#include "test_framework.h"
#include "kbd_debounce.h"

#define T 2   /* Niphargus: 2 scans x 5 ms */

/* Feeds a raw sequence ("1" = down, "0" = up) and writes the event of each
 * scan into ev ('P', 'R' or '.'). */
static void run(const char *raw, uint32_t ticks, bool eager, char *ev)
{
    kbd_db_key_t k = {0};
    size_t i = 0;
    for (; raw[i]; i++) {
        kbd_db_event_t e = kbd_db_step(&k, raw[i] == '1', ticks, eager);
        ev[i] = e == KBD_DB_PRESS ? 'P' : e == KBD_DB_RELEASE ? 'R' : '.';
    }
    ev[i] = '\0';
}

#define EXPECT(raw, ticks, eager, want, msg) do { \
    char _ev[512]; run(raw, ticks, eager, _ev); \
    if (strcmp(_ev, want) != 0) printf("    raw  %s\n    got  %s\n    want %s\n", raw, _ev, want); \
    TEST_ASSERT(strcmp(_ev, want) == 0, msg); \
} while (0)

static void test_kbd_debounce_eager_clean_press(void)
{
    EXPECT("0011111100000", T, true,  "..P......R...", "eager: press on the first down scan, release after 2 up scans");
    EXPECT("0011111100000", T, false, "...P.....R...", "symmetric: press and release both after 2 scans");
}

static void test_kbd_debounce_eager_bouncing_press(void)
{
    /* Contact chatter right after the press: the lockout swallows it. */
    EXPECT("01010111111000", T, true, ".P..........R.", "eager: ONE press, the chatter is inside the lockout or cancels the release");
    EXPECT("01010111111000", T, false, "......P.....R.", "symmetric: waits for 2 stable scans");
}

static void test_kbd_debounce_eager_bouncing_release(void)
{
    /* Release chatter: each down scan cancels the pending release. */
    EXPECT("11111010100000", T, true,  "P.........R...", "eager: ONE release, after 2 consecutive up scans");
    EXPECT("11111010100000", T, false, ".P........R...", "symmetric: same release");
}

static void test_kbd_debounce_eager_glitch_at_rest(void)
{
    /* THE trade-off: a single-scan glitch while up. Eager reports it (press
     * at once, lockout, then the deferred release): a phantom tap held 3
     * scans (15 ms at 5 ms). QMK's asym_eager_defer_pk does the same — the
     * price of an eager press, which cannot know the next scan. Symmetric
     * filters it. On the halves the wake capture keeps its two-pass AND, so
     * a phantom WAKE still types nothing. */
    EXPECT("0001000000", T, true,  "...P..R...", "eager: a one-scan glitch is a phantom tap (documented)");
    EXPECT("0001000000", T, false, "..........", "symmetric: the glitch is filtered");
}

static void test_kbd_debounce_eager_short_tap(void)
{
    /* A tap shorter than the lockout: reported pressed until the lockout
     * ends, then released after 2 up scans — never lost, never doubled. */
    EXPECT("0100000", T, true, ".P..R..", "eager: a one-scan tap is still exactly one press + one release");
    EXPECT("011000000", T, true, ".P..R....", "eager: a two-scan tap");
}

static void test_kbd_debounce_eager_release_then_repress(void)
{
    /* No lockout after a release (asym_eager_defer_pk): a real re-press
     * right after the release is reported at once. A release bounce LONGER
     * than the release window (2 up scans = 10 ms) would therefore be a
     * double press — the switch's bounce is ~1-5 ms, and the dongle counts
     * re-presses < 30 ms (RF_STATUS[43..46]) to catch it on the bench. */
    EXPECT("111110010000", T, true, "P.....RP..R.", "eager: re-press after a reported release = a new press");
}

static void test_kbd_debounce_held_keys_never_wrap(void)
{
    /* No absolute scan counter: the per-key counter only runs inside a
     * window. 600 scans held / at rest (> 255, a uint8 wrap) report nothing. */
    char raw[700], want[700], ev[700];
    memset(raw, '1', 600); raw[600] = '\0';
    memset(want, '.', 600); want[600] = '\0'; want[0] = 'P';
    run(raw, T, true, ev);
    TEST_ASSERT(strcmp(ev, want) == 0, "eager: 600 scans held = one press, nothing after");
    want[0] = '.'; want[T - 1] = 'P';
    run(raw, T, false, ev);
    TEST_ASSERT(strcmp(ev, want) == 0, "symmetric: 600 scans held = one press");
    memset(raw, '0', 600);
    memset(want, '.', 600);
    run(raw, T, true, ev);
    TEST_ASSERT(strcmp(ev, want) == 0, "eager: 600 scans at rest = nothing");
}

/* The historical driver loop (keyboard_button.c before 2026-10-21), kept
 * as the oracle of the symmetric mode: V2/V2D must not change by one scan. */
static int legacy_step(uint8_t *level, uint8_t *cnt, int raw, uint32_t ticks)
{
    if (raw != *level) {
        if (++*cnt >= ticks) { *cnt = 0; *level = (uint8_t)raw; return raw ? 1 : 2; }
    } else {
        *cnt = 0;
    }
    return 0;
}

static uint32_t rng = 0x12345678u;
static uint32_t xs(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void test_kbd_debounce_symmetric_is_the_legacy_driver(void)
{
    int mismatches = 0;
    for (uint32_t ticks = 1; ticks <= 5; ticks++)
        for (int run_i = 0; run_i < 200; run_i++) {
            kbd_db_key_t k = {0};
            uint8_t lvl = 0, cnt = 0;
            int raw = 0;
            for (int s = 0; s < 400; s++) {
                if (xs() % 4 == 0) raw = !raw;   /* bouncy random input */
                int want = legacy_step(&lvl, &cnt, raw, ticks);
                kbd_db_event_t got = kbd_db_step(&k, raw, ticks, false);
                if ((int)got != want || k.level != lvl) mismatches++;
            }
        }
    TEST_ASSERT_EQ(mismatches, 0, "symmetric mode == the legacy driver, scan for scan (ticks 1..5, random bounce)");
}

static void test_kbd_debounce_eager_invariants(void)
{
    /* On random bouncy input, eager mode: events alternate P/R starting with
     * P; a P is only reported on a down scan; an R only after `ticks`
     * consecutive up scans; the reported level matches the events; and a
     * key reported pressed stays so at least 2*ticks-1 scans (lockout + release window, no flicker). */
    int bad = 0;
    for (uint32_t ticks = 1; ticks <= 5; ticks++)
        for (int run_i = 0; run_i < 200; run_i++) {
            kbd_db_key_t k = {0};
            int raw = 0, down = 0, ups = 0, since_p = 0;
            for (int s = 0; s < 400; s++) {
                if (xs() % 3 == 0) raw = !raw;
                ups = raw ? 0 : ups + 1;
                kbd_db_event_t e = kbd_db_step(&k, raw, ticks, true);
                since_p++;
                if (e == KBD_DB_PRESS) {
                    if (down || !raw) bad++;
                    down = 1; since_p = 0;
                } else if (e == KBD_DB_RELEASE) {
                    if (!down || raw || ups < (int)ticks || since_p < 2 * (int)ticks - 1) bad++;
                    down = 0;
                }
                if (k.level != down) bad++;
            }
        }
    TEST_ASSERT_EQ(bad, 0, "eager invariants hold on random bounce (ticks 1..5)");
}

/* Board choice: eager on the Niphargus halves only — V1/V2/V2D unchanged. */
static int defaults_has(const char *board, const char *line)
{
    char path[512], buf[256];
    snprintf(path, sizeof path, "%s/boards/%s/sdkconfig.defaults", KASE_REPO_ROOT, board);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int found = 0;
    while (fgets(buf, sizeof buf, f))
        if (strncmp(buf, line, strlen(line)) == 0) found = 1;
    fclose(f);
    return found;
}

static void test_kbd_debounce_eager_is_a_niphargus_choice(void)
{
    TEST_ASSERT_EQ(defaults_has("niphar_left",  "CONFIG_KASE_DEBOUNCE_EAGER_PRESS=y"), 1, "left half: eager press");
    TEST_ASSERT_EQ(defaults_has("niphar_right", "CONFIG_KASE_DEBOUNCE_EAGER_PRESS=y"), 1, "right half: eager press");
    TEST_ASSERT_EQ(defaults_has("kase_v2",       "CONFIG_KASE_DEBOUNCE_EAGER_PRESS"), 0, "V2: symmetric, unchanged");
    TEST_ASSERT_EQ(defaults_has("kase_v2_debug", "CONFIG_KASE_DEBOUNCE_EAGER_PRESS"), 0, "V2D: symmetric, unchanged");
    TEST_ASSERT_EQ(defaults_has("kase_v1",       "CONFIG_KASE_DEBOUNCE_EAGER_PRESS"), 0, "V1: symmetric, unchanged");
}

void test_kbd_debounce(void)
{
    printf("\n[kbd_debounce]\n");
    test_kbd_debounce_eager_clean_press();
    test_kbd_debounce_eager_bouncing_press();
    test_kbd_debounce_eager_bouncing_release();
    test_kbd_debounce_eager_glitch_at_rest();
    test_kbd_debounce_eager_short_tap();
    test_kbd_debounce_eager_release_then_repress();
    test_kbd_debounce_held_keys_never_wrap();
    test_kbd_debounce_symmetric_is_the_legacy_driver();
    test_kbd_debounce_eager_invariants();
    test_kbd_debounce_eager_is_a_niphargus_choice();
}
