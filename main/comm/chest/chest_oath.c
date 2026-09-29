/* Niphar_chest link, OATH browser model — pure logic. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md §4; IHM
 * engagements: Niphar_chest docs/LINK_CONTRACT.md §13 (32e8257). See
 * chest_oath.h for the design choices behind each function (page_needed's
 * `first`, on_code's off-cursor guard, code_visible's wrap-safe, one-way
 * hide). */
#include "chest_oath.h"
#include <string.h>

void chest_oath_reset(chest_oath_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
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
}

void chest_oath_nav(chest_oath_t *o, int8_t delta)
{
    if (!o) return;
    o->code_shown = false;   /* IHM engagement: every navigation key hides the code, unconditionally */

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
    if (o->cursor < o->page.first) return false;

    uint16_t idx_in_page = (uint16_t)(o->cursor - o->page.first);
    if (idx_in_page >= o->page.count) return false;

    *e = &o->page.e[idx_in_page];
    return true;
}

void chest_oath_on_code(chest_oath_t *o, const chest_code_t *c, uint32_t now_ms)
{
    if (!o || !c) return;

    const chest_list_entry_t *e;
    if (!chest_oath_cursor_entry(o, &e)) return;      /* nothing cached under the cursor: ignore */
    if (e->index != c->index) return;                 /* the owner navigated away: ignore */

    o->code = *c;
    o->code_shown = true;
    o->code_deadline_ms = now_ms + (uint32_t)c->seconds * 1000u;
}

bool chest_oath_code_visible(chest_oath_t *o, uint32_t now_ms, uint8_t *secs_left)
{
    if (!o) return false;
    if (!o->code_shown) return false;

    int32_t remaining_ms = (int32_t)(o->code_deadline_ms - now_ms);
    if (remaining_ms <= 0) {
        o->code_shown = false;   /* hidden for good: no later call resurrects it */
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
