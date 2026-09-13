/* Routage de la fusion — quel moteur est actif selon l'USB (règle 3 du design).
 *
 * Deux moteurs (gauche en USB direct, dongle en sans-fil), UN SEUL actif à la
 * fois. Le choix se fait sur la présence d'un hôte USB à la gauche :
 *   - USB présent à la gauche → la gauche tape en local, le dongle se tait ;
 *   - sinon → la gauche émet son brut au dongle, le dongle tape.
 *
 * L'invariant à ne jamais violer : gauche et dongle ne tapent JAMAIS en même
 * temps (sinon double frappe / clavier éclaté sur deux périphériques).
 *
 * Logique pure, testée host. Design :
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md (règle 3).
 */
#include "test_framework.h"
#include "../main/comm/rf/fusion_route.h"

static void test_gauche_par_usb(void)
{
    /* USB présent : la gauche tape en local et n'alimente PAS le dongle. */
    TEST_ASSERT(fusion_left_types_local(true),  "USB → gauche tape en local");
    TEST_ASSERT(!fusion_left_emits_raw(true),   "USB → gauche n'émet pas de brut");

    /* Pas d'USB : la gauche émet son brut au dongle et ne tape pas en local. */
    TEST_ASSERT(!fusion_left_types_local(false), "pas d'USB → gauche ne tape pas en local");
    TEST_ASSERT(fusion_left_emits_raw(false),    "pas d'USB → gauche émet son brut");
}

static void test_dongle_selon_gauche(void)
{
    /* La gauche est en USB → le dongle se tait mais réémet la droite à la gauche. */
    TEST_ASSERT(!fusion_dongle_types(true),    "gauche USB → dongle se tait");
    TEST_ASSERT(fusion_dongle_reemits(true),   "gauche USB → dongle réémet la droite");

    /* La gauche est en sans-fil → le dongle tape, pas de réémission. */
    TEST_ASSERT(fusion_dongle_types(false),    "gauche sans-fil → dongle tape");
    TEST_ASSERT(!fusion_dongle_reemits(false), "gauche sans-fil → pas de réémission");
}

/* L'invariant central : jamais deux moteurs qui tapent en même temps, quelle que
 * soit la vue cohérente de l'état USB de la gauche. */
static void test_jamais_double_frappe(void)
{
    for (int usb = 0; usb <= 1; usb++) {
        bool left  = fusion_left_types_local((bool)usb);
        bool dongle = fusion_dongle_types((bool)usb);   /* le dongle connaît le mode de la gauche */
        TEST_ASSERT(!(left && dongle), "jamais gauche ET dongle qui tapent");
        TEST_ASSERT(left || dongle,    "toujours au moins un moteur qui tape");
    }
}

void test_fusion_route(void)
{
    TEST_SUITE("Routage de la fusion (règle 3)");
    test_gauche_par_usb();
    test_dongle_selon_gauche();
    test_jamais_double_frappe();
}
