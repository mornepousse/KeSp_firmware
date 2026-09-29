#include "chest_view.h"
#include <string.h>

void chest_view_build(chest_view_t *v, chest_block_t blk, const chest_status_t *st,
                      uint8_t mode_wanted, uint8_t mode_state,
                      chest_oath_t *o, uint32_t now_ms)
{
    if (!v) return;
    memset(v, 0, sizeof *v);

    if (blk == CHEST_BLOCK_OK && st) {
        v->bits = CHEST_VIEW_PRESENT | (uint8_t)(st->state & 0x0F);   /* SD|USB|READY|TIME */
        v->op = st->pending_op;
        v->op_count = st->op_count;
        strncpy(v->label, st->label, CHEST_LABEL_MAX);
        v->label[CHEST_LABEL_MAX] = '\0';
        v->mode_active = st->active_mode;
        v->mode_wanted = mode_wanted;
        v->mode_state  = mode_state;

        if (st->active_mode == CHEST_MODE_OATH && o && o->have_page) {
            v->browsing = true;
            v->pos = o->cursor;
            v->total = o->page.total;
            const chest_list_entry_t *e;
            if (chest_oath_cursor_entry(o, &e)) {
                strncpy(v->name, e->name, CHEST_LABEL_MAX);
                v->name[CHEST_LABEL_MAX] = '\0';
            }
        }
    } else if (blk == CHEST_BLOCK_BAD_VERSION) {
        v->bits = CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER;
    }
    /* CHEST_BLOCK_ABSENT / CHEST_BLOCK_CORRUPT: v->bits stays 0 (no chest);
     * the code below still runs (a code already on screen must not vanish
     * on one busy/corrupt read — it disappears on its own deadline or on
     * navigation, never on a transport hiccup). */

    if (o) {
        uint8_t secs = 0;
        if (chest_oath_code_visible(o, now_ms, &secs)) {
            v->code_visible = true;
            strncpy(v->code, o->code.code, sizeof v->code - 1);
            v->code[sizeof v->code - 1] = '\0';
            v->code_secs = secs;
        }
    }
}
