#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_proto.h"
#include "chest_oath.h"

/* Niphar_chest link, DMA channel bookkeeping — pure, no ESP-IDF. Every
 * decision the transport (chest_link.c) takes about the DMA channel lives
 * here, host-tested (test/test_chest_round.c): when to send LIST or CODE,
 * when to read a segment, when a code request is abandoned
 * (chest_oath_code_cancel), when the OATH browser is dropped. chest_link.c
 * only executes the plan on the wire. Contract: Niphar_chest
 * docs/LINK_CONTRACT.md §13 (46499d6; the 0x11-before-op guarantee below at
 * c9ee3ad/6219516); spec
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §3-4.
 *
 * The rules, each one pinned by a test:
 *  - SEGMENT: read only when 0x11 CHANGED since the last segment consumed
 *    AND chest_dma_segment_ok(). Never on 0x10 alone (it stays non-zero
 *    after a read). First contact takes 0x11 as a reference, reads nothing
 *    (contract §13, "wait for 0x11 to change"). A change with an unreadable
 *    announcement (kind unknown, length 0 or > 512) is consumed without a
 *    read, so it is not retried every round.
 *  - ONE REQUEST IN FLIGHT: a LIST is in flight until the next segment (any
 *    0x11 change), a CODE until the chest arms it (pending op non-zero with a
 *    NEW instance). Either one is dropped after CHEST_REQ_TIMEOUT_MS.
 *  - LIST RATE LIMIT (review ruling 4, 2026-09-29: at most one LIST per 0x11
 *    change or per decision, never one per tick): besides the one-in-flight
 *    rule, a LIST is never sent again for the SAME first index as the last
 *    LIST sent until a decision forgets it — entering OATH, a navigation
 *    key, or a timeout retry (at most CHEST_LIST_RETRIES in a row with no
 *    segment). A chest answering with a page that does not cover the cursor
 *    therefore cannot drive a loop, and a dead channel costs 1 +
 *    CHEST_LIST_RETRIES requests, then silence until a key.
 *  - CODE: only from a K_OATH_CODE press, in OATH mode and READY, when
 *    chest_oath_may_request_code() agrees (TIME_VALID, entry cached), no
 *    prompt pending on the chest (it would refuse to arm) and nothing in
 *    flight. A press that cannot be served is DROPPED, never queued for
 *    later (a code comes from a press, not from a press remembered) — the
 *    one exception is a send the bus refused (chest_round_sent).
 *  - CANCEL (chest_oath_code_cancel): no arming within CHEST_REQ_TIMEOUT_MS
 *    (the chest refused: a CCID confirmation in flight, a slot that is not
 *    OATH); armed, then the instance changes (another arming replaced ours);
 *    armed, then the pending op returns to 0 while 0x11 has not moved
 *    since the last segment consumed — contract §13 ("The segment number
 *    moves before the pending operation clears", c9ee3ad/6219516): the
 *    chest bumps 0x11 in the tick that authorises and serves, and clears
 *    the op one tick LATER, also when it fails to serve, so op 0 with no
 *    new 0x11 means refused, expired or failed; leaving OATH. The
 *    exemption (op 0 but a CODE segment announced and not yet consumed)
 *    covers the rounds where the segment is still unread when the op is
 *    already 0: our 250 ms read seeing both changes in one block (the
 *    chest ticks every 20 ms — cancelling there would retract the request
 *    before the same round's read, and the code would be refused), and a
 *    round after an RDDMA the busy bus refused (0x11 not consumed). NEVER
 *    on the wire timeout of a request that WAS armed: the answer then waits
 *    for a human press, seconds away (chest_oath.h). */

#define CHEST_REQ_TIMEOUT_MS  2000u  /* a request unanswered (LIST) or unarmed (CODE) is dropped */
#define CHEST_LIST_RETRIES    2u     /* automatic LIST retries after a timeout, then wait for a decision */

typedef struct {
    bool     seq_known;        /* 0x11 reference taken this session */
    uint8_t  last_seq;         /* the 0x11 value last consumed (read or skipped) */
    bool     doorbell_known;
    uint8_t  doorbell;         /* 0x3C: the last value written (seeded from the read-back) */
    bool     req_busy;         /* a request is in flight */
    uint8_t  req_cmd, req_arg;
    uint32_t req_t_ms;
    bool     code_wait;        /* a CODE request was sent; watching the chest's arming */
    bool     code_armed;       /* ...and the chest armed it */
    uint8_t  code_inst0;       /* instance at send time */
    uint8_t  code_inst;        /* instance of the arming */
    bool     code_key_retry;   /* a CODE send failed on the bus: tried again next round */
    bool     in_oath;          /* previous OK round was OATH and READY */
    uint8_t  list_retries;
    bool     list_sent_known;
    uint8_t  list_sent_first;  /* first index of the last LIST sent */
} chest_round_t;

typedef struct {
    bool    leave_oath;        /* chest_oath_reset() */
    bool    cancel_code;       /* chest_oath_code_cancel() */
    bool    read_segment;      /* RDDMA dma_len + INT0, then chest_round_segment_read() */
    bool    send;              /* WRDMA + WR_END + doorbell, then chest_round_sent() */
    uint8_t cmd, arg;          /* CHEST_REQ_LIST/CODE and its argument */
    bool    code_dropped;      /* a K_OATH_CODE press not served (diagnostic only) */
} chest_round_plan_t;

/* Session start, presence lost, chest gone (ABSENT): forget everything —
 * the next chest seen is a new one (its 0x11 and doorbell restart). */
void chest_round_reset(chest_round_t *r);

/* One OK round. `st` is this round's parsed block (blk == OK), `o` the OATH
 * model AFTER this round's navigation delta was applied (read only here),
 * `doorbell_readback` the master byte 0x3C of the same read (seeds the
 * doorbell at first contact, so a keyboard reboot under a powered chest
 * never re-sends the value the chest already served), `nav` true when a
 * navigation key was taken this round, `code_key` true when a K_OATH_CODE
 * press was taken this round. Fills `p`; commits only the decisions that do
 * not depend on the wire (timeouts, arming, LIST retries). */
void chest_round_plan(chest_round_t *r, const chest_status_t *st, uint8_t doorbell_readback,
                      const chest_oath_t *o, bool nav, bool code_key, uint32_t now_ms,
                      chest_round_plan_t *p);

/* The doorbell value to write at 0x3C after the planned request's WR_END:
 * the last one written plus one (contract §13, step 2). */
uint8_t chest_round_next_doorbell(const chest_round_t *r);
/* The transport sent (ok: WRDMA, WR_END and the doorbell all went out) or
 * failed to send (!ok) the planned request. ok: the request is in flight,
 * the doorbell advances, a CODE starts the arming watch (the caller also
 * calls chest_oath_code_requested). !ok: nothing is committed — a LIST is
 * decided again next round; a CODE press is tried again next round (still
 * through chest_oath_may_request_code, so for whatever is under the cursor
 * then) until it goes out, a navigation key drops it (the press was for
 * the account the owner just left), or OATH is left. */
void    chest_round_sent(chest_round_t *r, bool ok, uint8_t cmd, uint8_t arg,
                         uint8_t instance, uint32_t now_ms);

/* A planned segment was read off the wire (ok) — consume its 0x11. A failed
 * read (!ok) leaves it for the next round. */
void chest_round_segment_read(chest_round_t *r, bool ok, uint8_t seq);
