/* Niphar_chest link, OATH browser model — pure logic. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §4; IHM
 * engagements: Niphar_chest docs/LINK_CONTRACT.md §13 (32e8257). See
 * chest_oath.h for the design choices behind each function (page_needed's
 * `first` and its empty-chest early exit, on_list's cursor-moved hide,
 * on_code's request lock, code_visible's wrap-safe one-way hide). */
#include "chest_oath.h"
#include <string.h>

void chest_oath_reset(chest_oath_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
}

/* Hides the code AND wipes its digits (Task 6 review m3): a code that is no
 * longer on the screen has no reason to stay in RAM until the next one
 * overwrites it. The only place code_shown goes false. */
static void hide_code(chest_oath_t *o)
{
    o->code_shown = false;
    memset(&o->code, 0, sizeof o->code);
}

void chest_oath_on_list(chest_oath_t *o, const chest_list_t *l)
{
    if (!o || !l) return;
    o->page = *l;
    o->have_page = true;
    if (l->total == 0) {
        o->cursor = 0;
    } else if (o->cursor >= l->total) {
        o->cursor = (uint8_t)(l->total - 1u);
    }

    /* Review round 1, item 3: a LIST refresh can move accounts around. Keep
     * a shown code only if the cursor, after this refresh, still lands on
     * the SAME chest account (entry.index) the code was issued for. */
    if (o->code_shown) {
        const chest_list_entry_t *e;
        if (!chest_oath_cursor_entry(o, &e) || e->index != o->code.index)
            hide_code(o);
    }

    /* Review round 2, item 1: same treatment for a PENDING (not yet
     * answered) request — retract it if the cursor's entry, after this
     * refresh, no longer matches requested_index.
     *
     * Bite-proof note (round 2): under the CURRENT call surface this block
     * is behaviorally redundant with chest_oath_on_code's own cursor-entry
     * check (below) — on_code always re-validates the cursor at answer
     * time regardless of what happened here, so no test in this suite can
     * tell this block's presence from its absence (confirmed: removing it
     * did not turn anything red). Kept anyway, per the reported fix, as an
     * early/proactive retraction: a caller that inspects code_requested
     * directly between an on_list and the eventual on_code (Task 6's
     * transport — e.g. to stop showing "waiting for confirmation", or to
     * avoid re-sending a request that can no longer be answered
     * meaningfully) sees it cleared right away rather than only once a
     * (possibly late, possibly never-arriving) answer shows up. Same shape
     * as chest_dma.c's documented "equivalent mutant" guard: a real,
     * deliberate piece of behavior that this pure model's own tests cannot
     * distinguish from a no-op, because the two checks it duplicates
     * happen to always compose correctly here. */
    if (o->code_requested) {
        const chest_list_entry_t *e;
        if (!chest_oath_cursor_entry(o, &e) || e->index != o->requested_index)
            o->code_requested = false;
    }
}

void chest_oath_nav(chest_oath_t *o, int8_t delta)
{
    if (!o) return;
    hide_code(o);               /* IHM engagement: every navigation key hides the code, unconditionally */
    o->code_requested = false;  /* review round 1, item 4: a nav key also retracts any pending request */

    uint8_t total = o->have_page ? o->page.total : 0;
    if (total == 0) {
        o->cursor = 0;
        return;
    }

    int32_t pos = (int32_t)o->cursor + (int32_t)delta;
    pos %= (int32_t)total;
    if (pos < 0) pos += (int32_t)total;
    o->cursor = (uint8_t)pos;
}

bool chest_oath_page_needed(const chest_oath_t *o, uint8_t *first)
{
    if (!o) return false;

    /* Review round 1, item 1: a REAL empty chest (total == 0) needs no
     * further fetch — nothing more will ever come back. Without this, an
     * empty cached page always reads as "outside" its own trivial range
     * [0,0) and the transport would re-request LIST(0) forever. */
    if (o->have_page && o->page.total == 0) return false;

    bool needed;
    if (!o->have_page) {
        needed = true;
    } else {
        uint16_t page_end = (uint16_t)o->page.first + o->page.count;
        needed = (o->cursor < o->page.first) || (o->cursor >= page_end);
    }
    if (needed && first) *first = o->cursor;
    return needed;
}

bool chest_oath_cursor_entry(const chest_oath_t *o, const chest_list_entry_t **e)
{
    if (!o || !e) return false;
    if (!o->have_page) return false;
    /* Review round 1, item 5: total == 0 means NO account, whatever a
     * malformed page's count/entries otherwise carry; same for a cursor at
     * or past total — both independent of page.first/page.count. */
    if (o->page.total == 0 || o->cursor >= o->page.total) return false;
    if (o->cursor < o->page.first) return false;

    uint16_t idx_in_page = (uint16_t)(o->cursor - o->page.first);
    if (idx_in_page >= o->page.count) return false;

    *e = &o->page.e[idx_in_page];
    return true;
}

void chest_oath_code_requested(chest_oath_t *o, uint8_t chest_index)
{
    if (!o) return;
    o->code_requested = true;
    o->requested_index = chest_index;
}

void chest_oath_on_code(chest_oath_t *o, const chest_code_t *c, uint32_t now_ms)
{
    if (!o || !c) return;
    /* Review round 1, item 4: accepted only against a pending request for
     * EXACTLY this index; consumed right away so a duplicate answer, or a
     * late one after nav retracted the request, is ignored. */
    if (!o->code_requested || o->requested_index != c->index) return;

    /* Review round 2, item 1: ALSO require the account currently under the
     * cursor to be that same index — a request alone is not enough. This
     * IS the actual enforcement (confirmed by bite proof: removing this
     * check alone turns test_chest_oath_code_requested_index_not_under_
     * cursor_never_visible red); chest_oath_on_list's own request-
     * retraction is a proactive, but behaviorally redundant, early exit
     * (see its comment there). A refusal here does NOT touch
     * code_requested — the request stays armed for whatever answer comes
     * next (item 3). */
    const chest_list_entry_t *e;
    if (!chest_oath_cursor_entry(o, &e) || e->index != c->index) return;

    o->code = *c;
    o->code_shown = true;
    o->code_deadline_ms = now_ms + (uint32_t)c->seconds * 1000u;
    o->code_requested = false;
}

void chest_oath_code_cancel(chest_oath_t *o)
{
    if (!o) return;
    o->code_requested = false;
}

bool chest_oath_code_visible(chest_oath_t *o, uint32_t now_ms, uint8_t *secs_left)
{
    if (!o) return false;
    if (!o->code_shown) return false;

    int32_t remaining_ms = (int32_t)(o->code_deadline_ms - now_ms);
    if (remaining_ms <= 0) {
        hide_code(o);            /* hidden for good: no later call resurrects it */
        return false;
    }

    if (secs_left) {
        uint32_t secs = ((uint32_t)remaining_ms + 999u) / 1000u;   /* whole seconds, rounded up */
        *secs_left = (uint8_t)secs;
    }
    return true;
}

bool chest_oath_may_request_code(const chest_oath_t *o, uint8_t chest_state, uint8_t *index)
{
    if (!o) return false;

    const uint8_t need = CHEST_STATE_READY | CHEST_STATE_TIME;
    if ((chest_state & need) != need) return false;

    const chest_list_entry_t *e;
    if (!chest_oath_cursor_entry(o, &e)) return false;

    if (index) *index = e->index;
    return true;
}
