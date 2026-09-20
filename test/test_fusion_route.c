/* Fusion routing — which engine is active based on USB (design rule 3).
 *
 * Two engines (left over direct USB, dongle over wireless), ONLY ONE active
 * at a time. The choice is based on whether a USB host is present on the left:
 *   - USB present on the left → the left types locally, the dongle stays silent;
 *   - otherwise → the left emits its raw data to the dongle, the dongle types.
 *
 * The invariant to never violate: left and dongle NEVER type at the same
 * time (otherwise double typing / keyboard split across two devices).
 *
 * Pure logic, tested on host. Design:
 * docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md (rule 3).
 */
#include "test_framework.h"
#include "../main/comm/rf/fusion_route.h"

static void test_gauche_par_usb(void)
{
    /* USB present: the left types locally and does NOT feed the dongle. */
    TEST_ASSERT(fusion_left_types_local(true),  "USB → left types locally");
    TEST_ASSERT(!fusion_left_emits_raw(true),   "USB → left does not emit raw");

    /* No USB: the left emits its raw data to the dongle and does not type locally. */
    TEST_ASSERT(!fusion_left_types_local(false), "no USB → left does not type locally");
    TEST_ASSERT(fusion_left_emits_raw(false),    "no USB → left emits its raw data");
}

static void test_dongle_selon_gauche(void)
{
    /* The left is on USB → the dongle stays silent but re-emits the right to the left. */
    TEST_ASSERT(!fusion_dongle_types(true),    "left USB → dongle stays silent");
    TEST_ASSERT(fusion_dongle_reemits(true),   "left USB → dongle re-emits the right");

    /* The left is wireless → the dongle types, no re-emission. */
    TEST_ASSERT(fusion_dongle_types(false),    "left wireless → dongle types");
    TEST_ASSERT(!fusion_dongle_reemits(false), "left wireless → no re-emission");
}

/* The central invariant: never two engines typing at the same time, whatever
 * the coherent view of the left's USB state. */
static void test_jamais_double_frappe(void)
{
    for (int usb = 0; usb <= 1; usb++) {
        bool left  = fusion_left_types_local((bool)usb);
        bool dongle = fusion_dongle_types((bool)usb);   /* the dongle knows the left's mode */
        TEST_ASSERT(!(left && dongle), "never left AND dongle typing");
        TEST_ASSERT(left || dongle,    "always at least one engine typing");
    }
}

void test_fusion_route(void)
{
    TEST_SUITE("Fusion routing (rule 3)");
    test_gauche_par_usb();
    test_dongle_selon_gauche();
    test_jamais_double_frappe();
}
