#pragma once
/* Transition queue of the dongle engine (pure logic, test/test_fusion_file.c).
 *
 * The engine used to play only the CURRENT state of both halves on every cycle
 * (10 ms, 20 when a tap is in progress): a press + release, or a
 * release + re-press, falling between two reads, were merged — a tap
 * that never comes out, two t's collapsed into one. The "overwritten
 * transitions" counter added on 2026-09-15 settled it: 536 in one evening on 2026-09-19.
 *
 * Now every RECEIVED merged state is queued and the engine replays them
 * all, in order. A state identical to the last one pushed (reaffirming a
 * hold) is not a transition. When full, the queue merges new states into
 * its last slot and counts it: the overwrite becomes the exception it
 * should always have been (FUSION_FILE_CAP states = 80 ms at a 10 ms cycle). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "half_link.h"   /* fusion_state_t */

#define FUSION_FILE_CAP 8

typedef struct {
    fusion_state_t etats[FUSION_FILE_CAP];
    uint8_t        tete, n;          /* index of the oldest, number pending */
    fusion_state_t dernier;          /* last state pushed (deduplication) */
    bool           dernier_valide;
    uint32_t       ecrasees;         /* pushes merged for lack of room */
} fusion_file_t;

static inline void fusion_file_init(fusion_file_t *f) { memset(f, 0, sizeof *f); }

static inline bool fusion_file_meme(const fusion_state_t *a, const fusion_state_t *b)
{
    return memcmp(a->left.bitmap,  b->left.bitmap,  sizeof a->left.bitmap)  == 0
        && memcmp(a->right.bitmap, b->right.bitmap, sizeof a->right.bitmap) == 0;
}

/* Pushes a state. false if identical to the last one pushed (nothing to replay). */
static inline bool fusion_file_push(fusion_file_t *f, const fusion_state_t *e)
{
    if (f->dernier_valide && fusion_file_meme(&f->dernier, e)) return false;
    f->dernier = *e; f->dernier_valide = true;
    if (f->n == FUSION_FILE_CAP) {
        f->etats[(f->tete + FUSION_FILE_CAP - 1) % FUSION_FILE_CAP] = *e;   /* merges into the most recent */
        f->ecrasees++;
        return true;
    }
    f->etats[(f->tete + f->n) % FUSION_FILE_CAP] = *e;
    f->n++;
    return true;
}

static inline bool fusion_file_pop(fusion_file_t *f, fusion_state_t *out)
{
    if (!f->n) return false;
    *out = f->etats[f->tete];
    f->tete = (f->tete + 1) % FUSION_FILE_CAP;
    f->n--;
    return true;
}

static inline uint8_t  fusion_file_en_attente(const fusion_file_t *f) { return f->n; }

/* Drops what is pending without replaying anything (dongle silent: the right
 * keeps feeding the queue). The overflow counter survives; the last state pushed
 * is forgotten so that the current state, pushed again on resume, passes
 * deduplication. */
static inline void fusion_file_vider(fusion_file_t *f)
{
    f->tete = f->n = 0;
    f->dernier_valide = false;
}
static inline uint32_t fusion_file_ecrasees(const fusion_file_t *f)   { return f->ecrasees; }
