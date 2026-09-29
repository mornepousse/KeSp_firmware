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
 * index independently from its cursor position — the browser model does
 * not care where the bytes came from, only what chest_list_decode produced,
 * so this is a faithful stand-in. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_oath.h"
#include "../main/comm/chest/chest_proto.h"

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

static void test_chest_oath_reset(void)
{
    chest_oath_t o;
    memset(&o, 0xAA, sizeof o);
    chest_oath_reset(&o);
    TEST_ASSERT(!o.have_page, "reset: no page cached");
    TEST_ASSERT_EQ(o.cursor, 0, "reset: cursor 0");
    TEST_ASSERT(!o.code_shown, "reset: no code shown");
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
}

/* Plan deviation (docs/superpowers/plans/2026-09-29-chest-link-v3.md, Task 3
 * Step 1): the plan's illustrative text says "nav -4 from 0 wraps to 11",
 * but that is arithmetically wrong for a single nav(-4) over total 12: full
 * modulo wraparound gives (0 - 4) mod 12 == 8 (11 is only what a single
 * nav(-1) would give). Implemented and tested against the correct value —
 * see the report for this deviation. */
static void test_chest_oath_page_needed_wrap(void)
{
    chest_oath_t o;
    chest_list_t l = decode_l1();
    uint8_t first = 0xFF;

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, -4);
    TEST_ASSERT_EQ(o.cursor, 8, "nav -4 from 0 over 12 accounts wraps to 8 (0 - 4 mod 12)");
    TEST_ASSERT(chest_oath_page_needed(&o, &first), "position 8 is outside the cached page [0,3)");
    TEST_ASSERT_EQ(first, 8, "first is the wrapped cursor's own position");
}

/* The plan's own illustrative number (11) IS correct for a single-step
 * PREV: nav(-1) from cursor 0 over total 12 wraps to total-1 == 11. Kept as
 * its own test so the single-step boundary the plan actually describes
 * elsewhere in the flow (K_OATH_PREV, one press at a time) is still pinned
 * literally. */
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

static void test_chest_oath_code_lifecycle(void)
{
    chest_oath_t o;
    chest_list_t l5;
    chest_code_t c = decode_c1();   /* index 5, seconds 12 */
    uint8_t secs;

    /* A single-entry page whose account index matches C1's (5), so the
     * cursor's cached entry matches the code's index and on_code is
     * accepted. */
    memset(&l5, 0, sizeof l5);
    l5.total = 1;
    l5.count = 1;
    l5.first = 0;
    l5.e[0].index = 5;
    strcpy(l5.e[0].name, "SOMEACC");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "no code shown before on_code");

    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible right after on_code");
    TEST_ASSERT_EQ(secs, 12, "12 s left at on_code time (rounded up)");

    TEST_ASSERT(chest_oath_code_visible(&o, 12000, &secs), "code still visible near the end of the window");
    TEST_ASSERT_EQ(secs, 1, "1 s left at 12000 ms (deadline 13000)");

    TEST_ASSERT(!chest_oath_code_visible(&o, 13000, &secs), "code hidden exactly at its deadline");
    TEST_ASSERT(!chest_oath_code_visible(&o, 13001, &secs), "code stays hidden after its deadline, asked again");
}

static void test_chest_oath_code_visible_no_resurrection(void)
{
    chest_oath_t o;
    chest_list_t l5;
    chest_code_t c = decode_c1();
    uint8_t secs;

    memset(&l5, 0, sizeof l5);
    l5.total = 1;
    l5.count = 1;
    l5.first = 0;
    l5.e[0].index = 5;
    strcpy(l5.e[0].name, "SOMEACC");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_on_code(&o, &c, 1000);   /* deadline 13000 */

    TEST_ASSERT(!chest_oath_code_visible(&o, 13500, &secs), "code hidden past its deadline");
    /* Bite proof (Task 3, plan): a later call with an EARLIER now_ms must
     * NOT resurrect it — once hidden, hidden for good. */
    TEST_ASSERT(!chest_oath_code_visible(&o, 1500, &secs),
                "a later call with an earlier now_ms does not resurrect an already-hidden code");
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
    chest_oath_on_code(&o, &c, 1000);
    TEST_ASSERT(chest_oath_code_visible(&o, 1000, &secs), "code visible before navigating");

    chest_oath_nav(&o, +1);
    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "navigation hides the code, still well within its window");
}

static void test_chest_oath_on_code_ignored_off_cursor(void)
{
    chest_oath_t o;
    chest_list_t l5;
    chest_code_t c = decode_c1();   /* index 5 */
    uint8_t secs;

    memset(&l5, 0, sizeof l5);
    l5.total = 1;
    l5.count = 1;
    l5.first = 0;
    l5.e[0].index = 9;   /* NOT the code's index 5: the owner navigated away before the answer arrived */
    strcpy(l5.e[0].name, "NOTIT");

    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l5);
    chest_oath_on_code(&o, &c, 1000);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs),
                "an answer for an index other than the cursor's cached entry is ignored");
}

static void test_chest_oath_on_code_ignored_no_cached_entry(void)
{
    chest_oath_t o;
    chest_code_t c = decode_c1();
    uint8_t secs;

    chest_oath_reset(&o);
    /* No on_list at all: nothing cached under the cursor. */
    chest_oath_on_code(&o, &c, 1000);

    TEST_ASSERT(!chest_oath_code_visible(&o, 1000, &secs), "on_code with nothing cached under the cursor is ignored");
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

void test_chest_oath(void)
{
    TEST_SUITE("chest_oath");
    TEST_RUN(test_chest_oath_reset);
    TEST_RUN(test_chest_oath_on_list_cursor_and_entry);
    TEST_RUN(test_chest_oath_page_needed_forward);
    TEST_RUN(test_chest_oath_page_needed_wrap);
    TEST_RUN(test_chest_oath_page_needed_wrap_single_step);
    TEST_RUN(test_chest_oath_total_zero);
    TEST_RUN(test_chest_oath_code_lifecycle);
    TEST_RUN(test_chest_oath_code_visible_no_resurrection);
    TEST_RUN(test_chest_oath_nav_hides_code);
    TEST_RUN(test_chest_oath_on_code_ignored_off_cursor);
    TEST_RUN(test_chest_oath_on_code_ignored_no_cached_entry);
    TEST_RUN(test_chest_oath_may_request_code);
    TEST_RUN(test_chest_oath_may_request_code_index_not_cursor_position);
    TEST_RUN(test_chest_oath_may_request_code_no_cached_entry);
}
