#pragma once
/* File de transitions du moteur du dongle (logique pure, test/test_fusion_file.c).
 *
 * Le moteur ne jouait que l'état COURANT des deux moitiés à chaque cycle
 * (10 ms, 20 quand un tap est en cours) : un appui + relâchement, ou un
 * relâchement + ré-appui, tombés entre deux lectures étaient fondus — un tap
 * qui ne sort pas, deux t qui n'en font qu'un. Le compteur « transitions
 * écrasées » posé le 2026-09-15 a tranché : 536 en une soirée le 2026-09-19.
 *
 * Désormais chaque état fusionné REÇU est mis en file et le moteur les rejoue
 * tous, dans l'ordre. Un état identique au dernier poussé (réaffirmation de
 * maintien) n'est pas une transition. Pleine, la file fond les nouveaux dans
 * son dernier slot et le compte : l'écrasement redevient l'exception qu'il
 * aurait toujours dû être (FUSION_FILE_CAP états = 80 ms à 10 ms de cycle). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "half_link.h"   /* fusion_state_t */

#define FUSION_FILE_CAP 8

typedef struct {
    fusion_state_t etats[FUSION_FILE_CAP];
    uint8_t        tete, n;          /* index du plus ancien, nombre en attente */
    fusion_state_t dernier;          /* dernier état poussé (dédoublonnage) */
    bool           dernier_valide;
    uint32_t       ecrasees;         /* poussées fondues faute de place */
} fusion_file_t;

static inline void fusion_file_init(fusion_file_t *f) { memset(f, 0, sizeof *f); }

static inline bool fusion_file_meme(const fusion_state_t *a, const fusion_state_t *b)
{
    return memcmp(a->left.bitmap,  b->left.bitmap,  sizeof a->left.bitmap)  == 0
        && memcmp(a->right.bitmap, b->right.bitmap, sizeof a->right.bitmap) == 0;
}

/* Pousse un état. false si identique au dernier poussé (rien à rejouer). */
static inline bool fusion_file_push(fusion_file_t *f, const fusion_state_t *e)
{
    if (f->dernier_valide && fusion_file_meme(&f->dernier, e)) return false;
    f->dernier = *e; f->dernier_valide = true;
    if (f->n == FUSION_FILE_CAP) {
        f->etats[(f->tete + FUSION_FILE_CAP - 1) % FUSION_FILE_CAP] = *e;   /* fond dans le plus récent */
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

/* Jeter ce qui attend sans rien rejouer (dongle muet : la droite continue
 * d'alimenter la file). Le compteur de débordements survit ; le dernier poussé
 * est oublié pour que l'état courant, repoussé à la reprise, passe le
 * dédoublonnage. */
static inline void fusion_file_vider(fusion_file_t *f)
{
    f->tete = f->n = 0;
    f->dernier_valide = false;
}
static inline uint32_t fusion_file_ecrasees(const fusion_file_t *f)   { return f->ecrasees; }
