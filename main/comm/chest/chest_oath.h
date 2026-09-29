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
    bool         code_shown;
    chest_code_t code;
    uint32_t     code_deadline_ms;
} chest_oath_t;

void chest_oath_reset(chest_oath_t *o);

/* A decoded LIST page arrives: cache it — even when it does not cover the
 * cursor's current position (chest_oath_page_needed still reports what to
 * ask for next; see its own header for why `first` is always the cursor's
 * position) — and clamp the cursor to total-1 (0 when total == 0). Does
 * NOT touch a code on display: only navigation and expiry hide a code
 * (Review Focus, Task 3 — a LIST refresh is not a navigation key). */
void chest_oath_on_list(chest_oath_t *o, const chest_list_t *l);

/* A navigation key: move the cursor by delta, wrapping over the cached
 * page's `total` (0 when no page is cached yet, or its total is 0 — the
 * cursor then stays at 0). ALWAYS hides a shown code, whether or not the
 * cursor actually moves (IHM engagement: "the code disappears... at the
 * first navigation key" — the key itself is the trigger, not the motion;
 * delta == 0 or a single-account total still counts as a navigation key
 * pressed and still hides the code). */
void chest_oath_nav(chest_oath_t *o, int8_t delta);

/* True when the cursor's position is outside the cached page (or no page
 * is cached yet): *first = the position to request LIST from. Design
 * choice: `first` is always the cursor's OWN position, never the start of
 * some enclosing page boundary — the chest serves a page starting wherever
 * `first` says, so requesting from the cursor's position is the simplest
 * way to guarantee the page that comes back covers it. */
bool chest_oath_page_needed(const chest_oath_t *o, uint8_t *first);

/* The account under the cursor, from the cached page: name and chest
 * index; false if not cached (no page yet, or the cursor's position falls
 * outside the cached page's [first, first+count) range). */
bool chest_oath_cursor_entry(const chest_oath_t *o, const chest_list_entry_t **e);

/* A decoded CODE arrives (after a press): show it until now_ms +
 * seconds*1000 — UNLESS the code's index is not the account currently
 * under the cursor. That guards the case where the owner navigated away
 * between the press and the chest's answer: navigation already hid any
 * code being shown, and an answer that arrives afterwards for the account
 * that was under the cursor at press time must not reappear for a
 * different one. Also a no-op (no state change) when there is no cached
 * entry under the cursor at all. */
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
 * the chest's own ordering is not the cursor's 0-based one). */
bool chest_oath_may_request_code(const chest_oath_t *o, uint8_t chest_state, uint8_t *index);
