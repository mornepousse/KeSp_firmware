#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_dma.h"

/* Niphar_chest link, OATH browser model — pure. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §4 (flows). The
 * IHM engagements it implements (never a code without a preceding press;
 * the code disappears at the end of its window or at the first navigation
 * key; never refreshed automatically; the name list browses freely) are
 * Niphar_chest docs/LINK_CONTRACT.md §13 (32e8257) — contract, not a local
 * choice. Cursor position is 0-based over `total` accounts and independent
 * from the cached page's own `first`/`count`: the cache follows the
 * cursor, never the other way round.
 *
 * chest_oath_t embeds a chest_list_t (~1.1 KB: CHEST_LIST_MAX_ENTRIES == 32
 * names of up to CHEST_LABEL_MAX == 34 bytes). The owner (chest_link.c,
 * Task 6) MUST hold this struct static, or in a task's own static/BSS
 * storage — never on a stack frame that can be reused between calls. */
typedef struct {
    bool         have_page;
    chest_list_t page;              /* the cached LIST page */
    uint8_t      cursor;            /* position 0..total-1 */
    bool         code_requested;    /* a CODE request is outstanding for requested_index */
    uint8_t      requested_index;   /* the CHEST index chest_oath_code_requested was called with */
    bool         code_shown;
    chest_code_t code;
    uint32_t     code_deadline_ms;
} chest_oath_t;

void chest_oath_reset(chest_oath_t *o);

/* A decoded LIST page arrives: cache it — even when it does not cover the
 * cursor's current position (chest_oath_page_needed still reports what to
 * ask for next; see its own header for why `first` is always the cursor's
 * position) — and clamp the cursor to total-1 (0 when total == 0).
 *
 * Does NOT hide a code just because a LIST arrived — a refresh is not a
 * navigation key. But a LIST can silently move accounts around (added or
 * removed between two pages): if a code is currently shown, it is kept
 * ONLY when the cursor, after this refresh, still lands on the SAME chest
 * account (same entry.index) the code was issued for; if that account is
 * no longer under the cursor at all (gone, or some other account slid
 * there), the code is hidden — a code must never be shown next to an
 * account it was not issued for. */
void chest_oath_on_list(chest_oath_t *o, const chest_list_t *l);

/* A navigation key: move the cursor by delta, wrapping over the cached
 * page's `total` (0 when no page is cached yet, or its total is 0 — the
 * cursor then stays at 0). ALWAYS hides a shown code and retracts any
 * pending code request (chest_oath_code_requested), whether or not the
 * cursor actually moves — including delta == 0 (IHM engagement: "the code
 * disappears... at the first navigation key" — the key itself is the
 * trigger, not the motion). Retracting the request on every nav, even one
 * that returns to the same position, is deliberate: it is what stops a
 * late CODE answer from being accepted just because the owner happened to
 * navigate away and back before it arrived (chest_oath_on_code below). */
void chest_oath_nav(chest_oath_t *o, int8_t delta);

/* True when the cursor's position is outside the cached page AND the
 * cached page might still have more to offer: no page cached yet, or the
 * cursor sits outside [first, first+count). False once a REAL empty chest
 * (page.total == 0) has been cached — the chest publishes an empty LIST on
 * purpose (contract §13, "an empty list is still published") and there is
 * nothing further to fetch; without this, an empty page always reads as
 * "outside" the trivial range [0,0) and the transport would re-request
 * LIST(0) forever. *first = the position to request LIST from, set only
 * when the return value is true. Design choice: `first` is always the
 * cursor's OWN position, never the start of some enclosing page boundary —
 * the chest serves a page starting wherever `first` says, so requesting
 * from the cursor's position is the simplest way to guarantee the page
 * that comes back covers it. */
bool chest_oath_page_needed(const chest_oath_t *o, uint8_t *first);

/* The account under the cursor, from the cached page: name and chest
 * index; false if not cached — no page yet; the cached page's total is 0
 * (a malformed page could still carry non-zero count/entries despite
 * total == 0: total 0 always means NO account, whatever else the bytes
 * say); the cursor's position is >= total (same malformed-page guard,
 * independent of count); or the cursor's position falls outside the
 * cached page's [first, first+count) range. */
bool chest_oath_cursor_entry(const chest_oath_t *o, const chest_list_entry_t **e);

/* The owner calls this the moment it actually SENDS a CODE(chest_index)
 * request over the wire — arming the one lock chest_oath_on_code checks.
 * Overwrites any previous pending request (a second press before an
 * answer arrives simply re-arms the lock for whichever index it names). */
void chest_oath_code_requested(chest_oath_t *o, uint8_t chest_index);

/* A decoded CODE arrives: shown until now_ms + seconds*1000, but ONLY when
 * a request is currently pending (chest_oath_code_requested) for EXACTLY
 * c->index. Consumed on acceptance — code_requested clears — so a second
 * answer for the same index (a duplicate packet, or the chest re-sending)
 * is ignored: one code per request, never refreshed. This is also what
 * makes navigating away and back a real block, not just a cursor check:
 * chest_oath_nav retracts the request on EVERY call, so returning to the
 * same account does not restore eligibility for an answer that was
 * in flight before the navigation. A no-op (no state change) in every
 * other case: no request pending, or the answer's index does not match
 * the one requested. */
void chest_oath_on_code(chest_oath_t *o, const chest_code_t *c, uint32_t now_ms);

/* Visible? Hides it FOR GOOD once `now_ms` reaches or passes the deadline —
 * a later call, even with an EARLIER now_ms, never resurrects it: once
 * code_shown flips to false it stays false until the next chest_oath_on_code.
 * *secs_left = the remaining whole seconds, rounded UP (so at on_code time
 * with a 12 s window it reads 12, and 1 during the last second — never 0
 * while still visible). Wrap-safe: the remaining time is computed as
 * (int32_t)(code_deadline_ms - now_ms), which stays correct across the
 * uint32 millisecond wrap as long as the true difference fits an int32 (it
 * always does here: the window is at most 30 s). */
bool chest_oath_code_visible(chest_oath_t *o, uint32_t now_ms, uint8_t *secs_left);

/* The code request decision: true only with BOTH CHEST_STATE_READY and
 * CHEST_STATE_TIME set in `chest_state` (the raw register 0x05 state byte)
 * AND a cached entry under the cursor; *index = that entry's CHEST index
 * (entry.index — the account's real position in the chest, NOT the
 * browser's cursor position, which can differ once a page has scrolled or
 * the chest's own ordering is not the cursor's 0-based one). Does not
 * itself arm chest_oath_code_requested — the owner calls that separately,
 * once it actually sends the request built from this index. */
bool chest_oath_may_request_code(const chest_oath_t *o, uint8_t chest_state, uint8_t *index);
