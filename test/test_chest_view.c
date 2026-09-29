/* The chest link's screen view — pure: builds chest_view_t from a parsed
 * status block plus the OATH browser model. "Bytes -> pixels, pinned end to
 * end" (plan docs/superpowers/plans/2026-09-29-chest-link-v3.md, Task 5):
 * V1/V9/V15/V16/L1/C1 come from chest_test_vectors.h — Niphar_chest
 * test/test_link_proto.c (k_vec_v1/v9/v15/v16 at commit cfd7b35,
 * k_vec_l1/k_vec_c1 at commit 440d79d, vectors identical to cfd7b35),
 * shared with test_memlcd_model.c's own end-to-end chain rather than a
 * third copy (see that header's own comment). */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_view.h"
#include "chest_test_vectors.h"

#define V1  CHEST_TV_V1
#define V9  CHEST_TV_V9
#define V15 CHEST_TV_V15
#define V16 CHEST_TV_V16
#define L1  CHEST_TV_L1
#define C1  CHEST_TV_C1

static chest_status_t parse(const uint8_t *raw, chest_block_t *blk)
{
    chest_status_t st;
    memset(&st, 0, sizeof st);
    *blk = chest_proto_parse(raw, 64, &st);
    return st;
}

static void test_view_ok_basic(void)
{
    chest_block_t blk;
    chest_status_t st = parse(V1, &blk);
    TEST_ASSERT_EQ(blk, CHEST_BLOCK_OK, "V1 parses");

    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, NULL, 1000);
    TEST_ASSERT_EQ(v.bits, CHEST_VIEW_PRESENT | 0x0F, "V1 bits: PRESENT + SD+USB+READY+TIME");
    TEST_ASSERT(v.bits & CHEST_STATE_TIME, "V1 TIME bit carried into the view");
    TEST_ASSERT_EQ(v.op, 7, "V1 op: OATH_CODE");
    TEST_ASSERT_EQ(v.op_count, 1, "V1 op_count");
    TEST_ASSERT(strcmp(v.label, "GITHUB") == 0, "V1 label");
    TEST_ASSERT_EQ(v.mode_active, CHEST_MODE_OATH, "V1 active mode oath");
    /* No oath model passed (NULL): browsing/code must not crash and stay off. */
    TEST_ASSERT(!v.browsing, "no oath model: never browsing");
    TEST_ASSERT(!v.code_visible, "no oath model: never a code");
}

static void test_view_absent_and_badversion(void)
{
    chest_view_t v;
    chest_status_t st = {0};
    chest_view_build(&v, CHEST_BLOCK_ABSENT, &st, 0, CHEST_MODE_ARRIVED, NULL, 0);
    TEST_ASSERT_EQ(v.bits, 0, "absent: no chest");
    TEST_ASSERT_EQ(v.op, 0, "absent: no op");

    chest_view_build(&v, CHEST_BLOCK_CORRUPT, &st, 0, CHEST_MODE_ARRIVED, NULL, 0);
    TEST_ASSERT_EQ(v.bits, 0, "corrupt: no chest either");

    chest_view_build(&v, CHEST_BLOCK_BAD_VERSION, &st, 0, CHEST_MODE_ARRIVED, NULL, 0);
    TEST_ASSERT_EQ(v.bits, CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER, "bad version: present, badver");
}

/* V16 — the RESET path: op_count says 12 in ONE byte, the label says so in
 * words too. Both must reach the view untouched (the bug this Task 5 note
 * exists for: the chest published op_count 1 here once, silently). */
static void test_view_v16_reset(void)
{
    chest_block_t blk;
    chest_status_t st = parse(V16, &blk);
    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, NULL, 0);
    TEST_ASSERT_EQ(v.op, 10, "V16 op: OATH_RESET");
    TEST_ASSERT_EQ(v.op_count, 12, "V16 op_count carried whole, not clamped to 1");
    TEST_ASSERT(strcmp(v.label, "12 COMPTES") == 0, "V16 label");
}

/* The prompt must show the CHEST's own label (register 0x14), never the
 * browser's cursor name — even when the two clearly differ AND an op is
 * pending at the same time (V1: op 7, label GITHUB, active mode oath).
 * V9 ALONE cannot prove this: it carries no label and no pending op, so a
 * "prompt shows the cursor name" bug would produce an empty prompt either
 * way — nothing to tell the two implementations apart. Only V1 (and V16)
 * actually exercise the discriminating path: an op pending WHILE the OATH
 * cursor sits on a different, named account. */
static void test_view_prompt_uses_chest_label_not_cursor_name(void)
{
    chest_list_t l;
    TEST_ASSERT(chest_list_decode(L1, sizeof L1, &l), "L1 decodes");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, +2);   /* cursor -> position 2 = "OVH:PRO" (index 2) */

    chest_block_t blk;
    chest_status_t st = parse(V1, &blk);
    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);

    TEST_ASSERT(strcmp(v.label, "GITHUB") == 0, "the CHEST's own label, unaffected by the cursor");
    TEST_ASSERT(strcmp(v.name, "OVH:PRO") == 0, "the browser's own cursor name is ALSO populated (priority is memlcd_vue_coffre's job, not this builder's)");
    TEST_ASSERT(v.browsing, "OATH active and a page cached: browsing true regardless of the pending op");
    TEST_ASSERT_EQ(v.op, 7, "the op is still there for the caller to prioritize");
}

/* Demonstrates the insufficiency of V9 alone for the same property: no op,
 * no label, no cached page relevant to it either way. */
static void test_view_v9_has_nothing_to_show_either_way(void)
{
    chest_block_t blk;
    chest_status_t st = parse(V9, &blk);
    chest_list_t l;
    chest_list_decode(L1, sizeof L1, &l);
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, +2);

    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    TEST_ASSERT_EQ(v.op, 0, "V9: nothing pending");
    TEST_ASSERT(v.label[0] == '\0', "V9: no label");
    /* active_mode is NONE on V9: browsing stays false whatever the OATH
     * model holds — a bug that always trusted the cursor would ALSO pass
     * this assertion, because there is no prompt line to compare against. */
    TEST_ASSERT(!v.browsing, "V9: active mode is none, not oath");
}

static void test_view_v15_no_time_while_browsing(void)
{
    chest_list_t l;
    chest_list_decode(L1, sizeof L1, &l);
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);

    chest_block_t blk;
    chest_status_t st = parse(V15, &blk);
    TEST_ASSERT(!(st.state & CHEST_STATE_TIME), "V15: time not posed");
    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    TEST_ASSERT(v.browsing, "V15: active oath, page cached");
    TEST_ASSERT(!(v.bits & CHEST_STATE_TIME), "TIME bit carried through as clear");

    /* Contrast: V1 has the SAME label/op/active-mode shape but TIME set. */
    chest_status_t st1 = parse(V1, &blk);
    chest_view_build(&v, blk, &st1, st1.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    TEST_ASSERT(v.bits & CHEST_STATE_TIME, "V1: time posed, bit set in the view");
}

/* A code is visible in the view only once chest_oath_code_requested() AND a
 * matching chest_oath_on_code() both happened (chest_oath's own gate —
 * proven again here through the view builder, end to end), and it is gone
 * once its deadline passes. Also proves the countdown ROUNDS UP: 12 s at
 * t=1000 reads 12, and 1 during the last second, never 0 while still shown. */
static void test_view_code_lifecycle(void)
{
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.first = 0;
    page.e[0].index = 5; strcpy(page.e[0].name, "WORK");

    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page);   /* cursor 0 -> the WORK / index 5 entry */

    chest_block_t blk;
    chest_status_t st = parse(V1, &blk);   /* active mode oath, whatever op/label */

    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 1000);
    TEST_ASSERT(!v.code_visible, "no request yet: no code");

    chest_oath_code_requested(&o, 5);
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 1000);
    TEST_ASSERT(!v.code_visible, "requested but not answered: still no code");

    chest_oath_on_code(&o, &c, 1000);
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 1000);
    TEST_ASSERT(v.code_visible, "requested AND answered, cursor on its account: visible");
    TEST_ASSERT(strcmp(v.code, "418902") == 0, "the code text");
    TEST_ASSERT_EQ(v.code_secs, 12, "12 s window, at t0: 12 (never 0 while shown)");

    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 12000);
    TEST_ASSERT(v.code_visible, "1 ms before the deadline: still visible");
    TEST_ASSERT_EQ(v.code_secs, 1, "last second reads 1, not 0: rounded UP");

    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 13000);
    TEST_ASSERT(!v.code_visible, "deadline passed: gone");
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 13001);
    TEST_ASSERT(!v.code_visible, "stays gone (chest_oath_code_visible latches false)");
}

/* review I2: a code shown on the panel could stay there past its deadline,
 * unbounded, whenever the transport stalls (rf_bus_lock kept failing,
 * chest_view_build() only being called on a round the link task actually
 * gets a fresh status for). chest_view_age() ages a SNAPSHOT in place, on
 * the display side's own clock read, independently of the transport. */
static void test_view_age_expires_and_rounds_up(void)
{
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.e[0].index = 5; strcpy(page.e[0].name, "WORK");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);   /* C1: 12 s window -> deadline 13000 */

    chest_block_t blk;
    chest_status_t st = parse(V1, &blk);
    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 1000);
    TEST_ASSERT(v.code_visible, "visible at t0");
    TEST_ASSERT_EQ(v.code_deadline_ms, 13000u, "the view carries the SAME deadline chest_oath computed");

    /* Age the FROZEN snapshot directly — no further chest_view_build call,
     * proving chest_view_age alone (not a fresh build) is what expires it. */
    chest_view_t v_500ms_left = v;
    chest_view_age(&v_500ms_left, 12500);
    TEST_ASSERT(v_500ms_left.code_visible, "aged at t=12500 (500 ms left): still visible");
    TEST_ASSERT_EQ(v_500ms_left.code_secs, 1, "500 ms rounds UP to 1 s, never 0 while still shown");

    chest_view_t v_at_deadline = v;
    chest_view_age(&v_at_deadline, 13000);
    TEST_ASSERT(!v_at_deadline.code_visible, "aged at t=13000 (the deadline): no code");
    TEST_ASSERT(v_at_deadline.code[0] == '\0', "the code text is cleared too, not just the flag");
    TEST_ASSERT_EQ(v_at_deadline.code_secs, 0, "the countdown is cleared too");
}

/* Same wrap-safety scrutiny as chest_oath_code_visible's own tests
 * (test_chest_oath.c): a naive `now_ms >= code_deadline_ms` comparison can
 * agree with the correct wrap-safe one on ONE side of a uint32 wrap by
 * accident — querying from the OTHER side (now_ms numerically large,
 * chronologically before a deadline that has already wrapped to a small
 * number) is what actually discriminates them. chest_view_age has its OWN
 * copy of the wrap-safe subtraction (it does not call chest_oath at all,
 * by design — see its header comment), so it needs its own proof. */
static void test_view_age_wraps_uint32(void)
{
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.e[0].index = 5; strcpy(page.e[0].name, "WORK");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 0xFFFFF000u);   /* deadline wraps to 0x1EE0, same vector as test_chest_oath's own wrap tests */

    chest_block_t blk;
    chest_status_t st = parse(V1, &blk);
    chest_view_t v;
    chest_view_build(&v, blk, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0xFFFFF000u);
    TEST_ASSERT(v.code_visible, "captured at arming time: visible");
    TEST_ASSERT_EQ(v.code_deadline_ms, 0x1EE0u, "the deadline snapshot itself wrapped, same as chest_oath's own");

    chest_view_t v1 = v;
    chest_view_age(&v1, 0x00001000u);
    TEST_ASSERT(v1.code_visible, "aged just after the wrap, before the wrapped deadline: still visible");

    chest_view_t v2 = v;
    chest_view_age(&v2, 0x00002000u);
    TEST_ASSERT(!v2.code_visible, "aged at/after the wrapped deadline: gone");

    /* From the OTHER side of the wrap: now_ms still numerically large
     * (0xFFFFFFF0), chronologically ~7.92 s before the (already-wrapped)
     * deadline — the case a naive comparison gets wrong. */
    chest_view_t v3 = v;
    chest_view_age(&v3, 0xFFFFFFF0u);
    TEST_ASSERT(v3.code_visible, "aged from BEFORE the wrap, numerically far past the wrapped deadline: still visible");
}

/* review M-d: a visible code must survive a CORRUPT or ABSENT round (a
 * transport hiccup, not a real navigation/expiry event) — it is armed on
 * the live chest_oath_t, which chest_view_build does not touch differently
 * depending on `blk`. It still ages out on time regardless (I2). */
static void test_view_code_survives_corrupt_round_then_ages_out(void)
{
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.e[0].index = 5; strcpy(page.e[0].name, "WORK");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);   /* deadline 13000 */

    chest_status_t bad = {0};
    chest_view_t v;
    chest_view_build(&v, CHEST_BLOCK_CORRUPT, &bad, 0, CHEST_MODE_ARRIVED, &o, 1500);
    TEST_ASSERT(v.code_visible, "code survives a CORRUPT round");
    TEST_ASSERT(strcmp(v.code, "418902") == 0, "code text intact through a CORRUPT round");

    chest_view_build(&v, CHEST_BLOCK_ABSENT, &bad, 0, CHEST_MODE_ARRIVED, &o, 2000);
    TEST_ASSERT(v.code_visible, "code survives an ABSENT round too");

    /* Still bounded: ages out on time via chest_view_age, whether or not
     * the transport ever reads the chest again. */
    chest_view_age(&v, 13000);
    TEST_ASSERT(!v.code_visible, "still ages out on time regardless of the transport (I2)");
}

void test_chest_view(void)
{
    TEST_SUITE("chest link screen view (S3 master, v3, bytes -> pixels)");
    TEST_RUN(test_view_ok_basic);
    TEST_RUN(test_view_absent_and_badversion);
    TEST_RUN(test_view_v16_reset);
    TEST_RUN(test_view_prompt_uses_chest_label_not_cursor_name);
    TEST_RUN(test_view_v9_has_nothing_to_show_either_way);
    TEST_RUN(test_view_v15_no_time_while_browsing);
    TEST_RUN(test_view_code_lifecycle);
    TEST_RUN(test_view_age_expires_and_rounds_up);
    TEST_RUN(test_view_age_wraps_uint32);
    TEST_RUN(test_view_code_survives_corrupt_round_then_ages_out);
}
