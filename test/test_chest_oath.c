/* The chest link's OATH browser model, pure — cursor over the accounts,
 * page fetch decision, a code shown only after a press and only until the
 * end of its window or the first navigation key. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §4 (flows);
 * IHM engagements: Niphar_chest docs/LINK_CONTRACT.md §13 (32e8257) — never
 * a code without a preceding press, the code disappears at the end of its
 * window or at the first navigation key, never refreshed automatically,
 * names browse freely.
 *
 * L1 and C1 below are the SAME vectors as test_chest_dma.c (Niphar_chest
 * main/link/link_proto.h and test/test_link_proto.c at commit 440d79d,
 * vectors identical to cfd7b35), duplicated here per the plan's own note
 * ("include them via a shared test header or duplicate the bytes with the
 * same provenance comment") rather than shared via a header. Decoded
 * through the real chest_list_decode/chest_code_decode (Task 2) instead of
 * hand-built chest_list_t/chest_code_t structs, so this suite also proves
 * the two tasks compose end to end. Some tests below use hand-built
 * chest_list_t pages (not from any chest vector) to control an account's
 * index independently from its cursor position, or a hand-built LIST byte
 * buffer (review round 1, item 5) to exercise a malformed-but-CRC-valid
 * page the real chest is not expected to ever emit — the browser model
 * does not care where the bytes came from, only what chest_list_decode
 * produced, so these are faithful stand-ins.
 *
 * Since review round 1: every accepted chest_oath_on_code call below is
 * preceded by chest_oath_code_requested() for the same index — the request
 * lock (item 4) is now the sole gate on_code checks; a call without it is
 * always a no-op. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_oath.h"
#include "../main/comm/chest/chest_proto.h"
#include "../main/security/cr_crc16.h"

/* L1 — one LIST page: twelve accounts total, three in this page from index
 * 0, the "more" flag set. Niphar_chest test/test_link_proto.c, 440d79d. */
static const uint8_t k_vec_l1[34] = {
    0x0C, 0x03, 0x00, 0x01, 0x00, 0x06, 0x47, 0x49, 0x54, 0x48,
    0x55, 0x42, 0x01, 0x09, 0x4F, 0x56, 0x48, 0x3A, 0x50, 0x45,
    0x52, 0x53, 0x4F, 0x02, 0x07, 0x4F, 0x56, 0x48, 0x3A, 0x50,
    0x52, 0x4F, 0xE5, 0xD7,
};
/* C1 — one CODE answer: account 5, six digits, 418902, twelve seconds
 * left. Niphar_chest test/test_link_proto.c, 440d79d. */
static const uint8_t k_vec_c1[14] = {
    0x05, 0x06, 0x30, 0x30, 0x34, 0x31, 0x38, 0x39, 0x30, 0x32,
    0x0C, 0x00, 0x8F, 0x9B,
};

static chest_list_t decode_l1(void)
{
    chest_list_t l;
    TEST_ASSERT(chest_list_decode(k_vec_l1, sizeof k_vec_l1, &l), "L1 decodes (setup)");
    return l;
}

static chest_code_t decode_c1(void)
{
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(k_vec_c1, sizeof k_vec_c1, &c), "C1 decodes (setup)");
    return c;
}

/* A single-entry page whose account index is `idx`, cursor lands on it at
 * position 0. Used everywhere below a code needs a cached entry to be
 * requested/answered for, without depending on L1's own index layout. */
static chest_list_t one_entry_page(uint8_t idx, const char *name)
{
    chest_list_t l;
    memset(&l, 0, sizeof l);
    l.total = 1;
    l.count = 1;
    l.first = 0;
    l.e[0].index = idx;
    strcpy(l.e[0].name, name);
    return l;
}

static void test_chest_oath_reset(void)
{
    chest_oath_t o;
    memset(&o, 0xAA, sizeof o);
    chest_oath_reset(&o);
    TEST_ASSERT(!o.have_page, "reset: no page cached");
    TEST_ASSERT_EQ(o.cursor, 0, "reset: cursor 0");
    TEST_ASSERT(!o.code_shown, "reset: no code shown");
    TEST_ASSERT(!o.code_requested, "reset: no pending code request");
}

static void test_chest_oath_on_list_cursor_and_entry(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    const chest_list_entry_t *e;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    TEST_ASSERT_EQ(o.cursor, 0, "on_list: cursor starts at 0");
    TEST_ASSERT(chest_oath_cursor_entry(&o, &e), "on_list: entry cached under the cursor");
    TEST_ASSERT(strcmp(e->name, "GITHUB") == 0, "on_list: cursor 0 is GITHUB");

    chest_oath_nav(&o, +1);
    TEST_ASSERT_EQ(o.cursor, 1, "nav +1: cursor 1");
    TEST_ASSERT(chest_oath_cursor_entry(&o, &e), "nav +1: entry cached");
    TEST_ASSERT(strcmp(e->name, "OVH:PERSO") == 0, "nav +1: OVH:PERSO");
}

/* Review round 1, item 6 (plan text corrected: "nav -4 from 3 (after the
 * three +1) wraps to 11" — a continuation of the SAME scenario, not a
 * fresh nav(-4) from cursor 0). Continues in place: three nav(+1) reach
 * position 3 (outside L1's cached [0,3)), then nav(-4) from there wraps to
 * 11 (3 - 4 == -1, mod 12 == 11) — the literal sequence the plan
 * describes, asserted end to end in one place. */
static void test_chest_oath_page_needed_forward(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    uint8_t first = 0xFF;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    TEST_ASSERT(!chest_oath_page_needed(&o, &first), "cursor 0 is within the cached page [0,3): no fetch needed");

    chest_oath_nav(&o, +1);   /* cursor 1 */
    chest_oath_nav(&o, +1);   /* cursor 2 */
    chest_oath_nav(&o, +1);   /* cursor 3 */
    TEST_ASSERT_EQ(o.cursor, 3, "three navigations land on position 3");
    TEST_ASSERT(chest_oath_page_needed(&o, &first), "position 3 is outside [0,3): a fetch is needed");
    TEST_ASSERT_EQ(first, 3, "page_needed starts the request at the cursor's own position");

    /* Plan's literal continuation (item 6): nav(-4) from position 3. */
    chest_oath_nav(&o, -4);
    TEST_ASSERT_EQ(o.cursor, 11, "nav -4 from position 3 wraps to 11 (3 - 4 == -1, mod 12 == 11)");
    TEST_ASSERT(chest_oath_page_needed(&o, &first), "position 11 is outside [0,3) too");
    TEST_ASSERT_EQ(first, 11, "first is the wrapped cursor's own position");
}

/* The general wraparound rule, pinned independently of the plan's specific
 * continuation above: a single nav(-1) from a fresh cursor 0 wraps to
 * total - 1 == 11. */
static void test_chest_oath_page_needed_wrap_single_step(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    uint8_t first = 0xFF;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, -1);
    TEST_ASSERT_EQ(o.cursor, 11, "nav -1 from 0 over 12 accounts wraps to 11 (total - 1)");
    TEST_ASSERT(chest_oath_page_needed(&o, &first), "position 11 is outside the cached page [0,3)");
    TEST_ASSERT_EQ(first, 11, "first is the wrapped cursor's own position");
}

/* The general wraparound rule for a multi-step negative delta applied in
 * ONE call, from a fresh cursor 0 (not the plan's specific continuation,
 * which starts from cursor 3 — see test_chest_oath_page_needed_forward):
 * full modulo wraparound, (0 - 4) mod 12 == 8. */
static void test_chest_oath_nav_wraps_negative_from_zero(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, -4);
    TEST_ASSERT_EQ(o.cursor, 8, "nav -4 from cursor 0 over 12 accounts wraps to 8 (0 - 4 mod 12)");
}

static void test_chest_oath_total_zero(void)
{
    chest_oath_t o;
    chest_list_t empty;
    const chest_list_entry_t *e;

    memset(&empty, 0, sizeof empty);
    empty.total = 0;
    empty.count = 0;
    empty.first = 0;
    empty.more = false;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &empty);
    TEST_ASSERT_EQ(o.cursor, 0, "total 0: cursor 0 after on_list");

    chest_oath_nav(&o, +1);
    TEST_ASSERT_EQ(o.cursor, 0, "total 0: nav keeps the cursor at 0");
    chest_oath_nav(&o, -3);
    TEST_ASSERT_EQ(o.cursor, 0, "total 0: nav the other way still keeps the cursor at 0");
    TEST_ASSERT(!chest_oath_cursor_entry(&o, &e), "total 0: no entry under the cursor");
}

/* Review round 1, item 1 — bite proof done separately (see report): a REAL
 * empty chest (a well-formed empty page, total == count == first == 0,
 * "an empty list is still published" per contract §13) must need no
 * further fetch, or the transport would loop on LIST(0) forever. */
static void test_chest_oath_page_needed_empty_chest(void)
{
    chest_oath_t o;
    chest_list_t empty;
    memset(&empty, 0, sizeof empty);

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &empty);
    TEST_ASSERT(!chest_oath_page_needed(&o, NULL), "a real empty chest (total 0) needs no fetch, ever");
}

/* Review round 1, item 2 (page_needed with no page): after reset, with
 * nothing cached at all, a fetch is needed starting at position 0. */
static void test_chest_oath_page_needed_no_page_yet(void)
{
    chest_oath_t o;
    uint8_t first = 0xFF;

    chest_oath_reset(&o);
    TEST_ASSERT(chest_oath_page_needed(&o, &first), "no page cached at all: a fetch is needed");
    TEST_ASSERT_EQ(first, 0, "first is the cursor's starting position, 0");
}

/* Review round 1, item 2 (shrinking clamp): on_list(L1) [total 12], a
 * single nav(-1) reaches 11; a LATER on_list with a SMALLER total (5) must
 * clamp the cursor to total - 1 == 4, not leave it dangling at 11. */
static void test_chest_oath_on_list_shrinks_cursor(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    chest_list_t small;

    memset(&small, 0, sizeof small);
    small.total = 5;
    small.count = 1;
    small.first = 4;
    small.e[0].index = 40;
    strcpy(small.e[0].name, "FIFTH");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, -1);
    TEST_ASSERT_EQ(o.cursor, 11, "cursor 11 before the shrinking LIST");

    chest_oath_on_list(&o, &small);
    TEST_ASSERT_EQ(o.cursor, 4, "a LIST with a smaller total (5) clamps the cursor to total - 1 == 4");
}

static void test_chest_oath_code_lifecycle(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "no code shown before on_code");

    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible right after on_code");
    TEST_ASSERT_EQ(secs, 12, "12 s left at on_code time (rounded up)");

    TEST_ASSERT(chest_oath_code_visible(&o, 12000, &secs), "code still visible near the end of the window");
    TEST_ASSERT_EQ(secs, 1, "1 s left at 12000 ms (deadline 13000)");

    /* Review round 1, item 2 (rounding up): deadline 13000, now 12500 ->
     * 500 ms == 0.5 s left, must round UP to 1, never truncate to 0. Bite
     * proof done separately (see report): a `remaining_ms / 1000` mutant
     * (floor instead of the `(remaining_ms + 999) / 1000` ceiling) gives 0
     * here, not 1. */
    TEST_ASSERT(chest_oath_code_visible(&o, 12500, &secs), "code still visible 500 ms before its deadline");
    TEST_ASSERT_EQ(secs, 1, "500 ms (0.5 s) left rounds UP to 1, not down to 0");

    TEST_ASSERT(!chest_oath_code_visible(&o, 13000, &secs), "code hidden exactly at its deadline");
    TEST_ASSERT(!chest_oath_code_visible(&o, 13001, &secs), "code stays hidden after its deadline, asked again");
}

static void test_chest_oath_code_visible_no_resurrection(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);   /* deadline 13000 */

    TEST_ASSERT(!chest_oath_code_visible(&o, 13500, &secs), "code hidden past its deadline");
    /* Bite proof (Task 3, plan): a later call with an EARLIER now_ms must
     * NOT resurrect it — once hidden, hidden for good. */
    TEST_ASSERT(!chest_oath_code_visible(&o, 1500, &secs),
                "a later call with an earlier now_ms does not resurrect an already-hidden code");
}

/* Review round 1, item 2 (2^32 wrap): on_code at a now_ms close to
 * UINT32_MAX with a 12 s window makes code_deadline_ms wrap past 0 (uint32
 * arithmetic: 0xFFFFF000 + 12000, mod 2^32, == 0x1EE0). The code must
 * still read as visible while the (also wrapped) query time is before that
 * wrapped deadline, and hidden once it is at or past it. */
static void test_chest_oath_code_visible_wraps_uint32(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 0xFFFFF000u);   /* deadline wraps to 0x1EE0 */

    TEST_ASSERT(chest_oath_code_visible(&o, 0x00001000u, &secs), "visible just after the uint32 wrap, before the wrapped deadline");
    TEST_ASSERT(!chest_oath_code_visible(&o, 0x00002000u, &secs), "hidden at/after the wrapped deadline");
}

/* The check points above (0x1000, 0x2000) do NOT actually discriminate a
 * `now_ms >= code_deadline_ms` (naive, non-wrap-safe) mutant from the
 * correct implementation: because code_deadline_ms itself already wrapped
 * via ordinary uint32 ADDITION at on_code time (0xFFFFF000 + 12000, mod
 * 2^32, == 0x1EE0), both query points here and the stored deadline sit in
 * the SAME "post-wrap" small range, where plain unsigned comparison
 * happens to agree with the wrap-safe subtraction. Verified empirically
 * (bite proof round 2, reported separately): that mutant survives the test
 * above. This second test queries from the OTHER side of the wrap — a
 * `now_ms` still numerically large (close to UINT32_MAX, chronologically
 * BEFORE the wrap and therefore before the deadline) — which IS where a
 * naive `now_ms >= deadline` gets it backwards: numerically
 * 0xFFFFFFF0 > 0x1EE0, so the naive check reads "already past deadline"
 * even though only ~7.9 s of the 12 s window have elapsed. */
static void test_chest_oath_code_visible_wraps_uint32_from_the_other_side(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 0xFFFFF000u);   /* deadline wraps to 0x1EE0 */

    /* Chronologically ~7.92 s before the deadline: 0xFFFFFFF0 is 0x10
     * (16 ms) short of the uint32 wrap, plus the 0x1EE0 (7904 ms) still to
     * go after it — about 7920 ms remaining, well inside the 12 s window. */
    TEST_ASSERT(chest_oath_code_visible(&o, 0xFFFFFFF0u, &secs),
                "still visible when queried from BEFORE the wrap, numerically far past the (already-wrapped) deadline");
}

static void test_chest_oath_nav_hides_code(void)
{
    chest_oath_t o;
    chest_list_t l5;
    chest_code_t c = decode_c1();
    uint8_t secs;

    memset(&l5, 0, sizeof l5);
    l5.total = 2;
    l5.count = 2;
    l5.first = 0;
    l5.e[0].index = 5;
    strcpy(l5.e[0].name, "SOMEACC");
    l5.e[1].index = 6;
    strcpy(l5.e[1].name, "OTHERACC");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible before navigating");

    chest_oath_nav(&o, +1);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "navigation hides the code, still well within its window");
}

/* Review round 1, item 2 (nav(0) hides the code): the mutant under test
 * elsewhere guards the hide behind `if (delta)`, which nav(0) would skip.
 * Bite proof done separately (see report). */
static void test_chest_oath_nav_zero_hides_code(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible before nav(0)");

    chest_oath_nav(&o, 0);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "nav(0) still hides the code: the key itself is the trigger");
}

/* Review round 2, item 2 (L2): nav(0) must retract a PENDING request too,
 * not only hide an already-shown code — this exercises the case where NO
 * code has been shown yet (unlike test_chest_oath_nav_zero_hides_code,
 * which starts from a shown code). Mutant to kill: gating the
 * code_requested clear behind `if (delta)` in chest_oath_nav. */
static void test_chest_oath_nav_zero_retracts_pending_request(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);

    chest_oath_nav(&o, 0);

    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs),
                "nav(0) retracted the pending request: the later answer is ignored");
}

/* Review round 1, item 3: a LIST refresh that moves a DIFFERENT account
 * under the cursor must hide a code already shown for the account that
 * used to be there. Positive control alongside: a refresh that leaves the
 * SAME account under the cursor must NOT hide it. */
static void test_chest_oath_on_list_moves_cursor_hides_code(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible right after on_code");

    /* Positive control: a LIST refresh that leaves the SAME index (5) at
     * the cursor's position must not hide the code. */
    chest_list_t same = one_entry_page(5, "SOMEACC-RENAMED");
    chest_oath_on_list(&o, &same);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "same account still under the cursor: code stays shown");

    /* A LIST refresh that puts a DIFFERENT account under the cursor must
     * hide the code — it would otherwise sit next to the wrong account. */
    chest_list_t other = one_entry_page(9, "DIFFERENT");
    chest_oath_on_list(&o, &other);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "a different account slid under the cursor: code hidden");
}

/* A LIST refresh where the cursor's position no longer has ANY cached
 * entry (not just a different one) must also hide a shown code. */
static void test_chest_oath_on_list_entry_gone_hides_code(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible right after on_code");

    chest_list_t empty;
    memset(&empty, 0, sizeof empty);
    chest_oath_on_list(&o, &empty);   /* total 0: nothing under the cursor any more */
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "the account vanished from the list: code hidden");
}

/* Review round 1, item 4: on_code is now gated on a pending request for
 * the exact index — calling it without ever requesting is a no-op. */
static void test_chest_oath_on_code_ignored_without_request(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    /* No chest_oath_code_requested call at all. */
    chest_oath_on_code(&o, &c, 1000);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "on_code without a pending request is ignored");
}

/* A request pending for a DIFFERENT index than the answer's must also be
 * ignored — the lock is exact, not "any request will do". Review round 2,
 * item 3 (L6): a refused answer must leave the pending request INTACT —
 * pinned here by following the decoy refusal with the real answer for the
 * account that was actually requested (and is still under the cursor, to
 * satisfy chest_oath_on_code's cursor gate from item 1), which must still
 * be accepted. */
static void test_chest_oath_on_code_ignored_wrong_requested_index(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");   /* cursor -> index 5 */
    chest_code_t right = decode_c1();                 /* index 5: matches both the request and the cursor */
    chest_code_t wrong = right;
    wrong.index = 9;                                  /* a decoy answer for an index never requested */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);

    chest_oath_on_code(&o, &wrong, 1000);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "an answer for an index other than the one requested is ignored");

    chest_oath_on_code(&o, &right, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs),
                "the refused decoy did NOT clear the pending request: the right answer is still accepted afterwards");
}

/* Review round 2, item 1 (Important) — the exact reproduction reported: a
 * code answering an OLD request must not surface once the chest's LIST has
 * moved a DIFFERENT account under the cursor in between. Both gates in
 * chest_oath_on_code (the request lock AND the cursor-entry check) close
 * this; chest_oath_on_list's own request-retraction (see its header) means
 * code_requested is already false by the time on_code is even called, but
 * the cursor gate is what actually enforces it — this test does not rely
 * on which one fires. */
static void test_chest_oath_on_code_after_list_changes_account_under_cursor(void)
{
    chest_oath_t o;
    chest_list_t page2 = one_entry_page(2, "ACC2");
    chest_list_t page7 = one_entry_page(7, "ACC7");
    chest_code_t c = decode_c1();
    c.index = 2;   /* an answer for the account that WAS under the cursor at request time */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page2);      /* cursor -> index 2 */
    chest_oath_code_requested(&o, 2);    /* request lock armed for 2 */

    chest_oath_on_list(&o, &page7);      /* the chest now serves index 7 at the SAME cursor position */

    chest_oath_on_code(&o, &c, 1000);    /* the (late/racing) answer for index 2 */
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs),
                "a code for the account that WAS under the cursor, after a different one slid there, must not show");
}

/* Review round 2, item 1: a request for an index that was never under the
 * cursor in the first place (a caller bug, or a race the owner did not
 * guard against) must never lead to a visible code, even though the
 * request lock alone would have matched. Also pins item 3 (L6) for THIS
 * refusal path specifically (not just the request-mismatch path
 * test_chest_oath_on_code_ignored_wrong_requested_index covers): the
 * cursor-gate refusal must not clear the pending request either — once
 * the cursor legitimately catches up to the requested account, the SAME
 * answer, replayed, is accepted. */
static void test_chest_oath_code_requested_index_not_under_cursor_never_visible(void)
{
    chest_oath_t o;
    chest_list_t page2 = one_entry_page(2, "ACC2");   /* cursor is on index 2 */
    chest_list_t page5 = one_entry_page(5, "ACC5");   /* a later refresh puts index 5 at the cursor */
    chest_code_t c = decode_c1();
    c.index = 5;   /* a code for an index that was never under the cursor, at first */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &page2);
    chest_oath_code_requested(&o, 5);   /* lock armed for 5, but the cursor's real entry is 2 */
    chest_oath_on_code(&o, &c, 1000);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs),
                "a request for an index not under the cursor never leads to a visible code");

    /* The refusal above must not have cleared the pending request: once a
     * LIST refresh legitimately brings the requested account under the
     * cursor, the SAME (replayed) answer is now accepted. */
    chest_oath_on_list(&o, &page5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs),
                "the cursor-gate refusal did not clear the request: it is accepted once the cursor catches up");
}

/* Review round 1, item 4: CODE requested, nav away, nav back to the SAME
 * position — the late answer must still be ignored, because nav ALWAYS
 * retracts the pending request (chest_oath.h), even a nav that returns to
 * where it started. This is exactly the case the request lock exists to
 * cover: a plain cursor-position check (the pre-review-round-1 design)
 * would have wrongly accepted this. */
static void test_chest_oath_on_code_nav_away_and_back_ignores_late_answer(void)
{
    chest_oath_t o;
    chest_list_t l5;
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    memset(&l5, 0, sizeof l5);
    l5.total = 2;
    l5.count = 2;
    l5.first = 0;
    l5.e[0].index = 5;
    strcpy(l5.e[0].name, "SOMEACC");
    l5.e[1].index = 6;
    strcpy(l5.e[1].name, "OTHERACC");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);

    chest_oath_nav(&o, +1);   /* away: retracts the request */
    chest_oath_nav(&o, -1);   /* back to the SAME position (index 5) */
    TEST_ASSERT_EQ(o.cursor, 0, "cursor is back on the original account");

    chest_oath_on_code(&o, &c, 1000);   /* the late answer for index 5 */
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs),
                "a late answer after nav-away-and-back is ignored: the request was retracted, not just the cursor moved");
}

/* Review round 1, item 4: once consumed, a second (duplicate) answer for
 * the same index is ignored — one code per request, never refreshed. */
static void test_chest_oath_on_code_duplicate_after_success_ignored(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    chest_code_t c2 = c;
    uint8_t secs;

    c2.seconds = 30;   /* a distinctly different window, to prove a duplicate is not even re-applied identically */

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);

    chest_oath_on_code(&o, &c, 1000);   /* accepted: deadline 1000 + 12000 == 13000 */
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "first answer accepted");
    TEST_ASSERT_EQ(secs, 12, "12 s window from the first (accepted) answer");

    chest_oath_on_code(&o, &c2, 1000);   /* duplicate, same index, request already consumed */
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "still visible (the first answer's state)");
    TEST_ASSERT_EQ(secs, 12, "the duplicate's different seconds (30) did NOT overwrite the original window");

    TEST_ASSERT(!chest_oath_code_visible(&o, 13000, &secs),
                "hidden at the ORIGINAL deadline (13000), proving the duplicate never re-armed a fresh 30 s window");
}

/* Review round 2, item 4: chest_oath_code_cancel retracts a pending
 * request — an answer arriving afterwards is ignored, same as any other
 * retraction path (nav, on_list, consumption). */
static void test_chest_oath_code_cancel(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    TEST_ASSERT(o.code_requested, "request armed before cancel");

    chest_oath_code_cancel(&o);
    TEST_ASSERT(!o.code_requested, "cancel clears the pending request");

    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "an answer after cancel is ignored");
}

/* Review round 2, item 4: cancel must NEVER touch an already-shown code —
 * only the pending-request lock. */
static void test_chest_oath_code_cancel_does_not_touch_shown_code(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);   /* accepted: code_shown true, request already consumed */
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible before cancel");

    chest_oath_code_cancel(&o);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "cancel does not hide an already-shown code");
    TEST_ASSERT_EQ(secs, 12, "cancel does not touch the code's countdown either");
}

/* chest_oath_code_cancel on nothing pending: a harmless no-op. */
static void test_chest_oath_code_cancel_nothing_pending(void)
{
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_code_cancel(&o);
    TEST_ASSERT(!o.code_requested, "cancel with nothing pending: still false, no crash");
}

/* Review round 1, item 5: a malformed-but-CRC-valid LIST page — total 0
 * (no account exists) yet count 1 with a real entry present in the bytes.
 * cursor_entry (and, through it, may_request_code) must never surface a
 * phantom account just because the bytes happen to carry one: total is
 * authoritative. Built as real wire bytes through chest_list_decode, like
 * chest_dma.c's own "well-formed but pathological" vectors, not a
 * hand-built chest_list_t — the CRC must be real for chest_list_decode to
 * accept it at all. */
static void test_chest_oath_cursor_entry_malformed_total_zero(void)
{
    uint8_t b[8];
    chest_list_t l;
    chest_oath_t o;
    const chest_list_entry_t *e;
    uint8_t index = 0xFF;

    b[CHEST_LIST_OFF_TOTAL] = 0;    /* NO account, despite what follows */
    b[CHEST_LIST_OFF_COUNT] = 1;
    b[CHEST_LIST_OFF_FIRST] = 0;
    b[CHEST_LIST_OFF_FLAGS] = 0;
    b[4] = 7;    /* entry index */
    b[5] = 0;    /* empty name */
    uint16_t crc = cr_crc16(b, 6);
    b[6] = (uint8_t)(crc & 0xFF);
    b[7] = (uint8_t)(crc >> 8);

    TEST_ASSERT(chest_list_decode(b, sizeof b, &l), "malformed page (total 0, count 1) still decodes: valid CRC, in-bounds entry");
    TEST_ASSERT_EQ(l.total, 0, "total really is 0");
    TEST_ASSERT_EQ(l.count, 1, "count really is 1: the malformation chest_list_decode does not itself reject");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    TEST_ASSERT_EQ(o.cursor, 0, "cursor clamped to 0 (total 0)");

    TEST_ASSERT(!chest_oath_cursor_entry(&o, &e), "cursor_entry refuses the phantom entry: total 0 means no account");
    TEST_ASSERT(!chest_oath_may_request_code(&o, (uint8_t)(CHEST_STATE_READY | CHEST_STATE_TIME), &index),
                "may_request_code is false against the same malformed page");
}

static void test_chest_oath_may_request_code(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    uint8_t index = 0xFF;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);   /* cursor 0 -> entry index 0, "GITHUB" */

    TEST_ASSERT(!chest_oath_may_request_code(&o, 0x00, &index), "not READY, not TIME_VALID: refused");
    TEST_ASSERT(!chest_oath_may_request_code(&o, CHEST_STATE_READY, &index),
                "READY alone, TIME_VALID clear (V15-like state): refused");
    TEST_ASSERT(!chest_oath_may_request_code(&o, CHEST_STATE_TIME, &index), "TIME_VALID alone, not READY: refused");

    TEST_ASSERT(chest_oath_may_request_code(&o, (uint8_t)(CHEST_STATE_READY | CHEST_STATE_TIME), &index),
                "READY and TIME_VALID both set, cached entry under the cursor: allowed");
    TEST_ASSERT_EQ(index, 0, "index is the entry's CHEST index (0 for GITHUB)");

    chest_oath_nav(&o, +1);   /* cursor 1, still within the cached page [0,3) */
    TEST_ASSERT(chest_oath_may_request_code(&o, (uint8_t)(CHEST_STATE_READY | CHEST_STATE_TIME), &index),
                "cursor 1 still within the cached page");
    TEST_ASSERT_EQ(index, 1, "index follows the cursor's entry, index 1 for OVH:PERSO");
}

static void test_chest_oath_may_request_code_index_not_cursor_position(void)
{
    chest_oath_t o;
    chest_list_t l;
    uint8_t index = 0xFF;

    /* A page whose entry's CHEST index (42) differs from its cursor
     * position (5) — pins that may_request_code reports entry.index, not
     * the browser's 0-based position. */
    memset(&l, 0, sizeof l);
    l.total = 6;
    l.count = 1;
    l.first = 5;
    l.e[0].index = 42;
    strcpy(l.e[0].name, "SCRAMBLED");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, 5);
    TEST_ASSERT_EQ(o.cursor, 5, "cursor moved to position 5, exactly on the cached entry");

    TEST_ASSERT(chest_oath_may_request_code(&o, (uint8_t)(CHEST_STATE_READY | CHEST_STATE_TIME), &index),
                "cached entry at cursor position 5");
    TEST_ASSERT_EQ(index, 42, "index is the entry's CHEST index (42), not the cursor's position (5)");
}

static void test_chest_oath_may_request_code_no_cached_entry(void)
{
    chest_oath_t o;
    uint8_t index = 0xFF;

    chest_oath_reset(&o);
    /* No page cached at all. */
    TEST_ASSERT(!chest_oath_may_request_code(&o, (uint8_t)(CHEST_STATE_READY | CHEST_STATE_TIME), &index),
                "READY and TIME_VALID set but nothing cached under the cursor: refused");
}

/* Task 6 review m3 (2026-09-29): a hidden code does not linger in RAM.
 * The digits are zeroed the moment the code stops being visible — on a
 * navigation key, on its deadline, on a LIST that moves another account
 * under the cursor — not left in chest_oath_t until the next code
 * overwrites them. */
static bool code_bytes_zero(const chest_oath_t *o)
{
    const uint8_t *b = (const uint8_t *)&o->code;
    for (size_t i = 0; i < sizeof o->code; i++) if (b[i]) return false;
    return true;
}

static void test_chest_oath_hidden_code_is_wiped(void)
{
    chest_oath_t o;
    chest_list_t l5 = one_entry_page(5, "SOMEACC");
    chest_code_t c = decode_c1();   /* index 5, "418902", 12 s */
    uint8_t secs;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(!code_bytes_zero(&o), "shown: the code is held");
    chest_oath_nav(&o, 0);
    TEST_ASSERT(code_bytes_zero(&o), "hidden by a navigation key: digits wiped");

    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 2000);
    TEST_ASSERT(chest_oath_code_visible(&o, 13999, &secs), "visible until its deadline");
    TEST_ASSERT(!code_bytes_zero(&o), "still held while visible");
    TEST_ASSERT(!chest_oath_code_visible(&o, 14000, &secs), "deadline reached");
    TEST_ASSERT(code_bytes_zero(&o), "expired: digits wiped");

    chest_list_t other = one_entry_page(6, "OTHERACC");
    chest_oath_code_requested(&o, 5);
    chest_oath_on_code(&o, &c, 20000);
    chest_oath_on_list(&o, &other);
    TEST_ASSERT(code_bytes_zero(&o), "hidden by a LIST moving another account under the cursor: wiped");
}

void test_chest_oath(void)
{
    TEST_SUITE("chest_oath");
    TEST_RUN(test_chest_oath_reset);
    TEST_RUN(test_chest_oath_on_list_cursor_and_entry);
    TEST_RUN(test_chest_oath_page_needed_forward);
    TEST_RUN(test_chest_oath_page_needed_wrap_single_step);
    TEST_RUN(test_chest_oath_nav_wraps_negative_from_zero);
    TEST_RUN(test_chest_oath_total_zero);
    TEST_RUN(test_chest_oath_page_needed_empty_chest);
    TEST_RUN(test_chest_oath_page_needed_no_page_yet);
    TEST_RUN(test_chest_oath_on_list_shrinks_cursor);
    TEST_RUN(test_chest_oath_code_lifecycle);
    TEST_RUN(test_chest_oath_code_visible_no_resurrection);
    TEST_RUN(test_chest_oath_code_visible_wraps_uint32);
    TEST_RUN(test_chest_oath_code_visible_wraps_uint32_from_the_other_side);
    TEST_RUN(test_chest_oath_nav_hides_code);
    TEST_RUN(test_chest_oath_hidden_code_is_wiped);
    TEST_RUN(test_chest_oath_nav_zero_hides_code);
    TEST_RUN(test_chest_oath_nav_zero_retracts_pending_request);
    TEST_RUN(test_chest_oath_on_list_moves_cursor_hides_code);
    TEST_RUN(test_chest_oath_on_list_entry_gone_hides_code);
    TEST_RUN(test_chest_oath_on_code_ignored_without_request);
    TEST_RUN(test_chest_oath_on_code_ignored_wrong_requested_index);
    TEST_RUN(test_chest_oath_on_code_after_list_changes_account_under_cursor);
    TEST_RUN(test_chest_oath_code_requested_index_not_under_cursor_never_visible);
    TEST_RUN(test_chest_oath_on_code_nav_away_and_back_ignores_late_answer);
    TEST_RUN(test_chest_oath_on_code_duplicate_after_success_ignored);
    TEST_RUN(test_chest_oath_code_cancel);
    TEST_RUN(test_chest_oath_code_cancel_does_not_touch_shown_code);
    TEST_RUN(test_chest_oath_code_cancel_nothing_pending);
    TEST_RUN(test_chest_oath_cursor_entry_malformed_total_zero);
    TEST_RUN(test_chest_oath_may_request_code);
    TEST_RUN(test_chest_oath_may_request_code_index_not_cursor_position);
    TEST_RUN(test_chest_oath_may_request_code_no_cached_entry);
}
