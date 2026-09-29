/* The chest link's DMA channel bookkeeping, pure (main/comm/chest/chest_round.h):
 * when the transport reads a segment, sends LIST or CODE, abandons a code
 * request, drops the browser. Contract: Niphar_chest docs/LINK_CONTRACT.md
 * §13 (46499d6) — read a segment only when 0x11 CHANGED, never on 0x10
 * alone; doorbell after WR_END; a CODE arms a confirmation, its segment only
 * comes after the press. Review ruling 4 (2026-09-29): at most one LIST per
 * 0x11 change or per decision, never one per tick.
 *
 * Status blocks here are hand-built chest_status_t (the parser is pinned
 * elsewhere, test_chest_proto.c): this suite is about the decisions. The
 * OATH model is fed with the chest's own L1 page through the real
 * chest_list_decode, so an account's index is the chest's. */
#include "test_framework.h"
#include <string.h>
#include "../main/comm/chest/chest_round.h"
#include "../main/comm/chest/chest_oath.h"
#include "../main/comm/chest/chest_dma.h"
#include "chest_test_vectors.h"

#define ST_OATH_OK  (CHEST_STATE_SD | CHEST_STATE_USB | CHEST_STATE_READY | CHEST_STATE_TIME)

static chest_status_t oath_status(uint8_t seq)
{
    chest_status_t st;
    memset(&st, 0, sizeof st);
    st.version = CHEST_PROTO_VERSION;
    st.state = ST_OATH_OK;
    st.active_mode = CHEST_MODE_OATH;
    st.instance = 3;
    st.dma_seq = seq;
    return st;
}

/* A model holding L1 (12 accounts, page 0..2), cursor 0. */
static void model_with_l1(chest_oath_t *o)
{
    static chest_list_t l;     /* ~1.1 KB: off the stack, as the transport does */
    chest_oath_reset(o);
    TEST_ASSERT(chest_list_decode(CHEST_TV_L1, sizeof CHEST_TV_L1, &l), "L1 decodes");
    chest_oath_on_list(o, &l);
}

/* Enter OATH with L1 cached and the entry LIST already answered: returns the
 * round state ready for code tests (seq reference 1, nothing in flight). */
static void round_in_oath_with_page(chest_round_t *r, chest_oath_t *o, uint32_t now)
{
    chest_round_plan_t p;
    chest_round_reset(r);
    chest_oath_reset(o);
    chest_status_t st = oath_status(0);
    chest_round_plan(r, &st, 0, o, false, false, now, &p);       /* first contact, LIST(0) */
    chest_round_sent(r, true, p.cmd, p.arg, st.instance, now);
    model_with_l1(o);
    st = oath_status(1); st.dma_kind = CHEST_DMA_LIST; st.dma_len = sizeof CHEST_TV_L1;
    chest_round_plan(r, &st, 0, o, false, false, now + 250, &p);
    chest_round_segment_read(r, true, 1);
}

static void test_first_contact_takes_the_reference_reads_nothing(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(7);
    st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;   /* a segment announced from before */
    chest_round_plan(&r, &st, 0x41, &o, false, false, 1000, &p);
    TEST_ASSERT(!p.read_segment, "first contact: 0x11 is a reference, not a change");
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), 0x42,
                   "doorbell seeded from the read-back, never re-sends what the chest served");
    chest_round_plan(&r, &st, 0x41, &o, false, false, 1250, &p);
    TEST_ASSERT(!p.read_segment, "0x10 non-zero alone never triggers a read");
}

static void test_segment_read_only_on_a_change_and_only_when_readable(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, false, 0, &p);
    st.dma_seq = 2; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, 250, &p);
    TEST_ASSERT(p.read_segment, "0x11 changed, readable: read");
    chest_round_segment_read(&r, false, 2);                         /* bus busy */
    chest_round_plan(&r, &st, 0, &o, false, false, 500, &p);
    TEST_ASSERT(p.read_segment, "a failed read is retried next round");
    chest_round_segment_read(&r, true, 2);
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(!p.read_segment, "consumed: never read twice");

    st.dma_seq = 3; st.dma_len = 0;
    chest_round_plan(&r, &st, 0, &o, false, false, 1000, &p);
    TEST_ASSERT(!p.read_segment, "length 0: not readable");
    st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    TEST_ASSERT(!p.read_segment, "an unreadable announcement is consumed, not retried when it heals");
    st.dma_seq = 4; st.dma_len = CHEST_DMA_MAX + 1;
    chest_round_plan(&r, &st, 0, &o, false, false, 1500, &p);
    TEST_ASSERT(!p.read_segment, "length > 512: not readable");
    st.dma_seq = 5; st.dma_len = 34; st.dma_kind = 3;
    chest_round_plan(&r, &st, 0, &o, false, false, 1750, &p);
    TEST_ASSERT(!p.read_segment, "unknown kind: not readable");
}

static void test_entering_oath_sends_one_list_not_one_per_tick(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(0);
    chest_round_plan(&r, &st, 0, &o, false, false, 0, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 0, "entering OATH: LIST(0)");
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 0);
    int lists = 0;
    for (uint32_t t = 250; t < 2000; t += 250) {
        chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        if (p.send) lists++;
    }
    TEST_ASSERT_EQ(lists, 0, "no second LIST while the first is unanswered (< 2 s)");
}

static void test_a_dead_channel_gets_two_retries_then_silence(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(0);
    int lists = 0;
    for (uint32_t t = 0; t < 30000; t += 250) {
        chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        if (p.send) { lists++; chest_round_sent(&r, true, p.cmd, p.arg, st.instance, t); }
    }
    TEST_ASSERT_EQ(lists, 1 + CHEST_LIST_RETRIES, "entry + CHEST_LIST_RETRIES, then wait for a decision");
    chest_round_plan(&r, &st, 0, &o, true, false, 30000, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST, "a navigation key is a decision: one more LIST");
}

static void test_a_reply_not_covering_the_cursor_does_not_loop(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 5);                                        /* cursor 5, page holds 0..2 */
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, true, false, 500, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 5, "navigation out of the page: LIST(5)");
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 500);
    int lists = 0;
    for (uint8_t s = 2; s < 12; s++) {   /* the chest keeps answering a page that misses 5 */
        st = oath_status(s); st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
        chest_round_plan(&r, &st, 0, &o, false, false, 500 + s * 250u, &p);
        if (p.read_segment) chest_round_segment_read(&r, true, s);
        if (p.send) { lists++; chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 500 + s * 250u); }
    }
    TEST_ASSERT_EQ(lists, 0, "the same first is not re-sent on every 0x11 change");
    chest_oath_nav(&o, 1);                                        /* cursor 6 */
    chest_round_plan(&r, &st, 0, &o, true, false, 4000, &p);
    TEST_ASSERT(p.send && p.arg == 6, "a new decision for a new first: LIST(6)");
}

static void test_an_empty_chest_asks_nothing(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_list_t empty; memset(&empty, 0, sizeof empty);
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_oath_on_list(&o, &empty);
    chest_status_t st = oath_status(0);
    for (uint32_t t = 0; t < 10000; t += 250) {
        chest_round_plan(&r, &st, 0, &o, (t % 1000) == 0, false, t, &p);
        TEST_ASSERT(!p.send, "total 0: never a LIST, even on navigation");
    }
}

static void test_nothing_is_sent_outside_oath_ready(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(0);
    st.active_mode = CHEST_MODE_PGP;
    chest_round_plan(&r, &st, 0, &o, true, true, 0, &p);
    TEST_ASSERT(!p.send, "PGP mode: nothing");
    TEST_ASSERT(p.code_dropped, "a code press outside OATH is dropped, not kept");
    st.active_mode = CHEST_MODE_OATH; st.state = CHEST_STATE_USB | CHEST_STATE_TIME;   /* not READY */
    chest_round_plan(&r, &st, 0, &o, true, true, 250, &p);
    TEST_ASSERT(!p.send, "OATH but not READY: nothing");
    st.active_mode = CHEST_MODE_IN_FLIGHT; st.state = ST_OATH_OK & ~CHEST_STATE_USB;
    chest_round_plan(&r, &st, 0, &o, true, true, 500, &p);
    TEST_ASSERT(!p.send, "switch in flight: nothing");
}

static void test_code_press_without_time_sends_nothing(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    st.state &= (uint8_t)~CHEST_STATE_TIME;                       /* V15: TIME_VALID clear */
    chest_round_plan(&r, &st, 0, &o, false, true, 500, &p);
    TEST_ASSERT(!p.send, "NO TIME: K_OATH_CODE sends nothing");
    TEST_ASSERT(p.code_dropped, "and the press is not kept for when the time comes");
    st.state |= CHEST_STATE_TIME;
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(!p.send, "time set later: still no code without a new press");
}

static void test_code_press_sends_code_for_the_entry_index(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 2);                                        /* cursor 2: OVH:PRO, chest index 2 */
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, true, false, 500, &p);
    TEST_ASSERT(!p.send, "cursor inside the cached page: no LIST");
    chest_round_plan(&r, &st, 0, &o, false, true, 750, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE && p.arg == 2, "CODE(the entry's chest index)");
    TEST_ASSERT(!p.code_dropped, "served");
}

static void test_code_not_armed_within_2s_is_cancelled(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    chest_round_plan(&r, &st, 0, &o, false, false, 2999, &p);
    TEST_ASSERT(!p.cancel_code, "1.999 s: still waiting for the arming");
    chest_round_plan(&r, &st, 0, &o, false, false, 3000, &p);
    TEST_ASSERT(p.cancel_code, "2 s without arming: the chest refused, cancel");
    chest_round_plan(&r, &st, 0, &o, false, true, 3250, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE, "the channel is free again for a new press");
}

static void test_armed_code_is_never_cancelled_by_the_wire_timeout(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    st.pending_op = 7; st.instance = 4;                           /* armed: new instance */
    for (uint32_t t = 1250; t <= 16000; t += 250) {
        chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        TEST_ASSERT(!p.cancel_code, "armed: waiting for the human press, however long");
    }
}

static void test_armed_then_confirmed_with_segment_keeps_the_request(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    st.pending_op = 7; st.instance = 4;
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    st.pending_op = 0; st.dma_seq = 2; st.dma_kind = CHEST_DMA_CODE; st.dma_len = CHEST_CODE_SIZE;
    chest_round_plan(&r, &st, 0, &o, false, false, 5000, &p);
    TEST_ASSERT(!p.cancel_code, "op cleared WITH a CODE segment in the same block: the code is there");
    TEST_ASSERT(p.read_segment, "and it is read");
}

static void test_armed_then_op_gone_without_segment_is_cancelled(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    st.pending_op = 7; st.instance = 4;
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    st.pending_op = 0;                                            /* expired, no segment */
    chest_round_plan(&r, &st, 0, &o, false, false, 16000, &p);
    TEST_ASSERT(p.cancel_code, "prompt gone with no segment: refused or expired, cancel");

    round_in_oath_with_page(&r, &o, 0);
    st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    st.pending_op = 7; st.instance = 4;
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    st.pending_op = 0; st.dma_seq = 2; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, 1500, &p);
    TEST_ASSERT(p.cancel_code, "a LIST segment is not the code: cancel");
}

static void test_armed_then_instance_changes_is_cancelled(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    st.pending_op = 7; st.instance = 4;
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    TEST_ASSERT(!p.cancel_code, "armed");
    st.instance = 5;                                              /* another arming replaced ours */
    chest_round_plan(&r, &st, 0, &o, false, false, 1500, &p);
    TEST_ASSERT(p.cancel_code, "instance changed while our request was pending: cancel");
}

static void test_leaving_oath_drops_the_browser_and_cancels(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, false, 500, &p);
    TEST_ASSERT(!p.leave_oath, "still in OATH");
    st.active_mode = CHEST_MODE_IN_FLIGHT; st.state &= (uint8_t)~CHEST_STATE_USB;
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(p.leave_oath, "leaving OATH: the browser is reset");
    TEST_ASSERT(p.cancel_code, "and any code request cancelled");
    chest_round_plan(&r, &st, 0, &o, false, false, 1000, &p);
    TEST_ASSERT(!p.leave_oath, "once");
}

static void test_code_press_while_prompt_pending_or_busy_is_dropped(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    st.pending_op = 1; st.instance = 9;                          /* a CCID signature waits */
    chest_round_plan(&r, &st, 0, &o, false, true, 500, &p);
    TEST_ASSERT(!p.send && p.code_dropped, "a prompt is up: the chest would refuse, send nothing");
    st.pending_op = 0;
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(!p.send, "the dropped press is not replayed once the prompt clears");

    /* a LIST in flight: the press is dropped, not queued behind it */
    chest_oath_nav(&o, 5);
    chest_round_plan(&r, &st, 0, &o, true, false, 1000, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST, "LIST(5) in flight");
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    chest_oath_nav(&o, -5);                                       /* back on a cached entry */
    chest_round_plan(&r, &st, 0, &o, true, true, 1250, &p);
    TEST_ASSERT(!p.send && p.code_dropped, "busy: the press is dropped");
    st.dma_seq = 2; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, 1500, &p);
    TEST_ASSERT(!(p.send && p.cmd == CHEST_REQ_CODE), "never a CODE without a press of its own");
}

static void test_code_send_failure_gets_one_retry(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 500, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE, "CODE planned");
    uint8_t bell = chest_round_next_doorbell(&r);
    chest_round_sent(&r, false, p.cmd, p.arg, st.instance, 500);  /* bus busy */
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), bell, "a failed send does not advance the doorbell");
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE, "the press gets its request next round");
    chest_round_sent(&r, false, p.cmd, p.arg, st.instance, 750);
    chest_round_plan(&r, &st, 0, &o, false, false, 1000, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE, "still the same press while the bus is busy");
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), (uint8_t)(bell + 1), "sent: the doorbell advanced once");
    chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
    TEST_ASSERT(!p.send, "one press, one request");

    round_in_oath_with_page(&r, &o, 0);
    st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, false, true, 500, &p);
    chest_round_sent(&r, false, p.cmd, p.arg, st.instance, 500);  /* bus busy */
    chest_oath_nav(&o, 1);
    chest_round_plan(&r, &st, 0, &o, true, false, 750, &p);
    TEST_ASSERT(!(p.send && p.cmd == CHEST_REQ_CODE),
                "a navigation key drops the retried press: it was for the account left behind");
}

static void test_doorbell_wraps_and_reset_forgets_the_chest(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    chest_round_reset(&r); chest_oath_reset(&o);
    chest_status_t st = oath_status(0);
    chest_round_plan(&r, &st, 0xFF, &o, false, false, 0, &p);
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), 0x00, "0xFF + 1 wraps to 0x00");
    chest_round_sent(&r, true, CHEST_REQ_LIST, 0, 3, 0);
    chest_round_plan(&r, &st, 0x10, &o, false, false, 250, &p);
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), 0x01, "seeded once per session, not every round");
    chest_round_reset(&r);
    st.dma_seq = 9; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0x10, &o, false, false, 500, &p);
    TEST_ASSERT(!p.read_segment, "after a reset the next chest is a first contact");
    TEST_ASSERT_EQ(chest_round_next_doorbell(&r), 0x11, "and its doorbell is seeded again");
}

/* Review round 1, I1: once a code has been served, the transport must go
 * back to accepting K_OATH_CODE — the prompt is over whichever way the op
 * cleared. Two shapes of the real chest (contract §13 at 6219516: 0x11
 * moves BEFORE the op clears, the op clears one chest tick later): our
 * 250 ms read sees both changes in ONE block, or sees them in two. */
static void test_a_second_code_after_a_served_code(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    for (int shape = 0; shape < 2; shape++) {
        round_in_oath_with_page(&r, &o, 0);
        chest_status_t st = oath_status(1);
        chest_round_plan(&r, &st, 0, &o, false, true, 1000, &p);
        TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE, "first CODE");
        chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 1000);
        st.pending_op = 7; st.instance = 4;                       /* armed */
        chest_round_plan(&r, &st, 0, &o, false, false, 1250, &p);
        st.dma_seq = 2; st.dma_kind = CHEST_DMA_CODE; st.dma_len = CHEST_CODE_SIZE;
        if (shape == 1) {                                         /* 0x11 first, op still set */
            chest_round_plan(&r, &st, 0, &o, false, false, 1500, &p);
            TEST_ASSERT(p.read_segment, "the CODE segment is read");
            chest_round_segment_read(&r, true, 2);
        }
        st.pending_op = 0;                                        /* the op clears */
        chest_round_plan(&r, &st, 0, &o, false, false, 1750, &p);
        if (p.read_segment) chest_round_segment_read(&r, true, 2);
        chest_round_plan(&r, &st, 0, &o, false, true, 2000, &p);
        TEST_ASSERT(p.send && p.cmd == CHEST_REQ_CODE,
                    shape ? "a second K_OATH_CODE is served (op cleared a round after 0x11)"
                          : "a second K_OATH_CODE is served (0x11 and op seen in one block)");
    }
}

/* Review round 1, I2 + M15: leaving OATH resets the browser (the transport
 * calls chest_oath_reset), so re-entering must ask LIST(0) again at once —
 * even though LIST(0) was the last LIST sent, and even if a LIST was still
 * in flight when OATH was left. */
static void test_reentering_oath_lists_again(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);                          /* LIST(0) was sent and answered */
    chest_status_t st = oath_status(1);
    st.active_mode = CHEST_MODE_PGP;
    chest_round_plan(&r, &st, 0, &o, false, false, 500, &p);
    TEST_ASSERT(p.leave_oath, "left");
    chest_oath_reset(&o);
    st.active_mode = CHEST_MODE_OATH;
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 0, "re-entering OATH: LIST(0) again");

    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 5);
    st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, true, false, 500, &p);
    TEST_ASSERT(p.send && p.arg == 5, "LIST(5) in flight");
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 500);
    st.active_mode = CHEST_MODE_PGP;
    chest_round_plan(&r, &st, 0, &o, false, false, 750, &p);
    chest_oath_reset(&o);
    st.active_mode = CHEST_MODE_OATH;
    chest_round_plan(&r, &st, 0, &o, false, false, 1000, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 0,
                "the LIST left in flight does not hold the re-entry LIST for 2 s");
}

/* Review round 1, I3: one request in flight — a navigation key while a
 * LIST is outstanding does not send a second LIST; the new page is asked
 * when the answer arrives or the request times out. */
static void test_nav_during_a_list_in_flight_waits(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 5);
    chest_status_t st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, true, false, 500, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 500);
    chest_oath_nav(&o, 3);                                        /* cursor 8: another page */
    chest_round_plan(&r, &st, 0, &o, true, false, 750, &p);
    TEST_ASSERT(!p.send, "LIST(5) still in flight: no LIST(8) yet");
    for (uint32_t t = 1000; t < 2500; t += 250) {
        chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        TEST_ASSERT(!p.send, "nor on any round before the answer or the timeout");
    }
    chest_round_plan(&r, &st, 0, &o, false, false, 2500, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 8, "timeout: now LIST(8)");

    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 5);
    st = oath_status(1);
    chest_round_plan(&r, &st, 0, &o, true, false, 500, &p);
    chest_round_sent(&r, true, p.cmd, p.arg, st.instance, 500);
    chest_oath_nav(&o, 3);
    chest_round_plan(&r, &st, 0, &o, true, false, 750, &p);
    TEST_ASSERT(!p.send, "in flight");
    st.dma_seq = 2; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, 1000, &p);
    TEST_ASSERT(p.send && p.cmd == CHEST_REQ_LIST && p.arg == 8, "answer in: now LIST(8)");
}

/* Review round 1, M25 + M26: the same-first block is about the SAME first
 * only, and the retry budget is per segment. The one way the wanted first
 * changes without a key is chest_oath_on_list's clamp when the chest's
 * total shrinks: that LIST must go out even though retries were spent and
 * a LIST (for another first) is still remembered, and it gets its own
 * retries. */
static void test_a_shrinking_chest_is_followed_with_fresh_retries(void)
{
    chest_round_t r; chest_round_plan_t p; chest_oath_t o;
    round_in_oath_with_page(&r, &o, 0);
    chest_oath_nav(&o, 11);                                       /* cursor 11 of 12 */
    chest_status_t st = oath_status(1);
    uint32_t t = 500;
    int lists = 0;
    chest_round_plan(&r, &st, 0, &o, true, false, t, &p);
    for (; t < 9000; t += 250) {
        if (t > 500) chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        if (p.send) { lists++; chest_round_sent(&r, true, p.cmd, p.arg, st.instance, t); }
    }
    TEST_ASSERT_EQ(lists, 1 + CHEST_LIST_RETRIES, "LIST(11) and its retries, spent");

    chest_list_t shrunk; memset(&shrunk, 0, sizeof shrunk);      /* the chest now holds 5 */
    shrunk.total = 5; shrunk.count = 3; shrunk.first = 0;
    for (uint8_t i = 0; i < 3; i++) shrunk.e[i].index = i;
    st.dma_seq = 2; st.dma_kind = CHEST_DMA_LIST; st.dma_len = 34;
    chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
    TEST_ASSERT(p.read_segment, "a late page arrives");
    chest_round_segment_read(&r, true, 2);
    chest_oath_on_list(&o, &shrunk);                             /* cursor clamps to 4 */
    lists = 0;
    for (t += 250; t < 20000; t += 250) {
        chest_round_plan(&r, &st, 0, &o, false, false, t, &p);
        if (p.send) {
            TEST_ASSERT(p.cmd == CHEST_REQ_LIST && p.arg == 4, "the clamped cursor's page: LIST(4)");
            lists++; chest_round_sent(&r, true, p.cmd, p.arg, st.instance, t);
        }
    }
    TEST_ASSERT_EQ(lists, 1 + CHEST_LIST_RETRIES, "LIST(4) goes out, with retries of its own");
}

void test_chest_round(void)
{
    TEST_SUITE("chest link: DMA round plan");
    TEST_RUN(test_first_contact_takes_the_reference_reads_nothing);
    TEST_RUN(test_segment_read_only_on_a_change_and_only_when_readable);
    TEST_RUN(test_entering_oath_sends_one_list_not_one_per_tick);
    TEST_RUN(test_a_dead_channel_gets_two_retries_then_silence);
    TEST_RUN(test_a_reply_not_covering_the_cursor_does_not_loop);
    TEST_RUN(test_an_empty_chest_asks_nothing);
    TEST_RUN(test_nothing_is_sent_outside_oath_ready);
    TEST_RUN(test_code_press_without_time_sends_nothing);
    TEST_RUN(test_code_press_sends_code_for_the_entry_index);
    TEST_RUN(test_code_not_armed_within_2s_is_cancelled);
    TEST_RUN(test_armed_code_is_never_cancelled_by_the_wire_timeout);
    TEST_RUN(test_armed_then_confirmed_with_segment_keeps_the_request);
    TEST_RUN(test_armed_then_op_gone_without_segment_is_cancelled);
    TEST_RUN(test_armed_then_instance_changes_is_cancelled);
    TEST_RUN(test_leaving_oath_drops_the_browser_and_cancels);
    TEST_RUN(test_code_press_while_prompt_pending_or_busy_is_dropped);
    TEST_RUN(test_code_send_failure_gets_one_retry);
    TEST_RUN(test_doorbell_wraps_and_reset_forgets_the_chest);
    TEST_RUN(test_a_second_code_after_a_served_code);
    TEST_RUN(test_reentering_oath_lists_again);
    TEST_RUN(test_nav_during_a_list_in_flight_waits);
    TEST_RUN(test_a_shrinking_chest_is_followed_with_fresh_retries);
}
