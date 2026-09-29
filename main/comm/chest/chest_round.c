/* Niphar_chest link, DMA channel bookkeeping — pure. See chest_round.h for
 * the rules; test/test_chest_round.c pins each one. */
#include "chest_round.h"
#include "chest_dma.h"
#include <string.h>

void chest_round_reset(chest_round_t *r)
{
    if (r) memset(r, 0, sizeof *r);
}

static void list_decision(chest_round_t *r)
{
    /* A decision (OATH entry, navigation key): a LIST may go out again for
     * any first index, and the retry counter starts over. */
    r->list_retries = 0;
    r->list_sent_known = false;
}

void chest_round_plan(chest_round_t *r, const chest_status_t *st, uint8_t doorbell_readback,
                      const chest_oath_t *o, bool nav, bool code_key, uint32_t now_ms,
                      chest_round_plan_t *p)
{
    memset(p, 0, sizeof *p);
    if (!r || !st) return;

    /* A press retried after a failed send is dropped by a navigation key:
     * it was for the account the owner just left. A fresh press taken in
     * the same round as the navigation stands (the model already moved). */
    code_key = code_key || (r->code_key_retry && !nav);
    r->code_key_retry = false;

    if (!r->doorbell_known) { r->doorbell = doorbell_readback; r->doorbell_known = true; }

    /* OATH membership: active mode OATH AND READY. */
    bool oath = st->active_mode == CHEST_MODE_OATH && (st->state & CHEST_STATE_READY);
    if (r->in_oath && !oath) {
        p->leave_oath = true;
        p->cancel_code = true;
        r->req_busy = false;
        r->code_wait = false;
    } else if (!r->in_oath && oath) {
        list_decision(r);
    }
    r->in_oath = oath;

    /* Segment: 0x11 must CHANGE; first contact only takes the reference. */
    bool seq_changed = false;
    if (!r->seq_known) {
        r->seq_known = true;
        r->last_seq = st->dma_seq;
    } else if (st->dma_seq != r->last_seq) {
        seq_changed = true;
        if (chest_dma_segment_ok(st)) p->read_segment = true;
        else r->last_seq = st->dma_seq;          /* unreadable: consumed, never retried */
        if (r->req_busy && r->req_cmd == CHEST_REQ_LIST) r->req_busy = false;   /* answered */
        r->list_retries = 0;
    }

    /* CODE arming watch — before the timeout, so an arming seen on the
     * boundary round counts. */
    if (r->code_wait) {
        if (!r->code_armed) {
            if (st->pending_op != 0 && st->instance != r->code_inst0) {
                r->code_armed = true;
                r->code_inst = st->instance;
                if (r->req_busy && r->req_cmd == CHEST_REQ_CODE) r->req_busy = false;   /* wire request answered */
            }
        } else if (st->instance != r->code_inst) {
            p->cancel_code = true;               /* another arming replaced ours */
            r->code_wait = false;
        } else if (st->pending_op == 0) {
            if (!(seq_changed && st->dma_kind == CHEST_DMA_CODE)) p->cancel_code = true;   /* refused / expired */
            r->code_wait = false;                /* either way the prompt is over */
        }
    }

    /* Request timeout. */
    if (r->req_busy && (uint32_t)(now_ms - r->req_t_ms) >= CHEST_REQ_TIMEOUT_MS) {
        r->req_busy = false;
        if (r->req_cmd == CHEST_REQ_CODE) {
            /* Never armed (an armed CODE is no longer req_busy): the chest
             * refused. Cancel — the armed case is NOT this branch. */
            p->cancel_code = true;
            r->code_wait = false;
        } else if (r->list_retries < CHEST_LIST_RETRIES) {
            r->list_retries++;
            r->list_sent_known = false;          /* the same first may be asked again */
        }
    }

    if (nav) list_decision(r);

    if (!oath) {
        p->code_dropped = code_key;
        return;
    }

    uint8_t first = 0;
    bool page = o && chest_oath_page_needed(o, &first);
    if (!r->req_busy && page
        && !(r->list_sent_known && r->list_sent_first == first)) {
        p->send = true; p->cmd = CHEST_REQ_LIST; p->arg = first;
    }

    if (code_key) {
        uint8_t idx = 0;
        if (!p->send && !r->req_busy && !r->code_wait && st->pending_op == 0
            && o && chest_oath_may_request_code(o, st->state, &idx)) {
            p->send = true; p->cmd = CHEST_REQ_CODE; p->arg = idx;
        } else {
            p->code_dropped = true;
        }
    }
}

uint8_t chest_round_next_doorbell(const chest_round_t *r)
{
    return r ? (uint8_t)(r->doorbell + 1u) : 1u;
}

void chest_round_sent(chest_round_t *r, bool ok, uint8_t cmd, uint8_t arg,
                      uint8_t instance, uint32_t now_ms)
{
    if (!r) return;
    if (!ok) {
        if (cmd == CHEST_REQ_CODE) r->code_key_retry = true;
        return;
    }
    r->doorbell = (uint8_t)(r->doorbell + 1u);
    r->req_busy = true;
    r->req_cmd = cmd;
    r->req_arg = arg;
    r->req_t_ms = now_ms;
    if (cmd == CHEST_REQ_CODE) {
        r->code_wait = true;
        r->code_armed = false;
        r->code_inst0 = instance;
    } else {
        r->list_sent_known = true;
        r->list_sent_first = arg;
    }
}

void chest_round_segment_read(chest_round_t *r, bool ok, uint8_t seq)
{
    if (r && ok) r->last_seq = seq;
}
