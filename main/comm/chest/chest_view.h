#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "chest_proto.h"
#include "chest_oath.h"

/* Niphar_chest link, screen view — pure, no ESP-IDF: builds what the bottom
 * area of the halves' screen shows from a parsed status block plus the OATH
 * browser model. chest_link.h re-exports chest_view_t and chest_link_view();
 * the transport (chest_link.c) only calls chest_view_build() and copies the
 * result under its lock, it never fills a field itself.
 *
 * "Bytes -> pixels, pinned end to end" (added to the v3 plan 2026-09-29
 * after the chest found its RESET path publishing op_count 1 while the
 * contract and V16 said 12): the register/DMA vectors pin the PARSER, not
 * that the SCREEN shows what the block says. test_memlcd_model.c chains the
 * chest's own raw vector bytes through chest_proto_parse -> chest_view_build
 * -> the memlcd model -> memlcd_bas_coffre and asserts the rendered lines,
 * so a regression anywhere in that chain shows up as a wrong string, not
 * just a wrong struct field. */

#define CHEST_VIEW_PRESENT 0x80
#define CHEST_VIEW_BADVER  0x40

typedef struct {
    /* 0 = no chest; else CHEST_VIEW_PRESENT | CHEST_VIEW_BADVER |
     * CHEST_STATE_* (SD, USB, READY, and — new in v3 — TIME). */
    uint8_t  bits;
    uint16_t op;                         /* pending op awaiting confirmation, 0 = none */
    uint8_t  op_count;                   /* accounts targeted by that op: 0 none, 1, N (RESET) */
    char     label[CHEST_LABEL_MAX + 1]; /* the CHEST's OWN label for the pending op — never the browser's name */
    uint8_t  mode_active, mode_wanted, mode_state;
    bool     browsing;                   /* active mode is OATH AND a LIST page is cached */
    uint8_t  pos, total;                 /* cursor position (0-based) and the cached page's total */
    char     name[CHEST_LABEL_MAX + 1];  /* the entry under the cursor, "" if not cached */
    bool     code_visible;
    char     code[9];                    /* 6 or 8 digits, NUL-terminated */
    uint8_t  code_secs;                  /* whole seconds left, rounded up (chest_oath_code_visible) */
} chest_view_t;

/* Pure builder, called once per read round.
 *   blk, st  — the round's chest_proto_parse() result. `st` is read ONLY
 *              when blk == CHEST_BLOCK_OK: chest_proto_parse's own contract
 *              is "out written only on OK", so reading it otherwise would
 *              be reading stale or uninitialized bytes. May be a stack
 *              local the caller never bothered to zero on a non-OK round.
 *   mode_wanted, mode_state — the mode tracker's own output (chest_mode_track,
 *              chest_proto.h); this function does not compute them, it only
 *              carries them into the view next to `st->active_mode`.
 *   o        — the OATH browser model (chest_oath.h). May be NULL (a caller
 *              that has not wired it yet): browsing stays false and no code
 *              is ever shown. Non-NULL: chest_oath_code_visible(o, now_ms, …)
 *              is called, which HIDES an expired code as a side effect —
 *              call this once per read round, not more.
 *   now_ms   — drives that countdown. */
void chest_view_build(chest_view_t *v, chest_block_t blk, const chest_status_t *st,
                      uint8_t mode_wanted, uint8_t mode_state,
                      chest_oath_t *o, uint32_t now_ms);
