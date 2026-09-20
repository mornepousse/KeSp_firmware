/* Conchodytes mouse input: SPDT click decoding and quadrature.
 *
 * Both pieces of logic are pure — input levels, an output state — and
 * that's deliberate: they're the ones carrying the reasoning, not the GPIO.
 * Testing them on the host lets us cover cases the bench doesn't produce on
 * demand, in particular the bounce window and impossible quadrature
 * transitions.
 *
 * The expected behavior comes from an observation on a real board on
 * 2026-08-25: over 24 transitions of the three clicks, no spurious edge was
 * produced. The DURATION of the ambiguous state, however, was not measured —
 * the campaign believed it was sampling at 1 kHz while CONFIG_FREERTOS_HZ
 * defaults to 100. These tests do not depend on any duration: they reason in
 * number of samples, which stays valid whatever the cadence.
 */
#include "test_framework.h"
#include "../main/input/mouse_buttons.h"
#include "../main/input/mouse_wheel.h"

/* ── Decoding an SPDT contact ───────────────────────────────────────────
 *
 * COM to ground, NO and NC each pulled to 3.3 V by 10 k.
 *   idle    : NC stuck to COM -> low ; NO open -> high
 *   pressed : NO stuck to COM -> low ; NC open -> high
 *   bounce  : the moving contact is stuck to NEITHER -> both high
 *   both low: electrically impossible (both contacts closed)
 */
static void test_contact_decode(void)
{
    TEST_ASSERT_EQ(mouse_contact_decode(1, 0), MOUSE_CONTACT_RELEASED,
                   "NO high + NC low = idle");
    TEST_ASSERT_EQ(mouse_contact_decode(0, 1), MOUSE_CONTACT_PRESSED,
                   "NO low + NC high = pressed");
    TEST_ASSERT_EQ(mouse_contact_decode(1, 1), MOUSE_CONTACT_BOUNCING,
                   "both high = contact in mid-air, bounce");
    TEST_ASSERT_EQ(mouse_contact_decode(0, 0), MOUSE_CONTACT_IMPOSSIBLE,
                   "both low = physically impossible");
}

/* The core of the debounce: during the ambiguous window, we keep the
 * previous state. This is what suppresses the double-click with no time
 * filtering at all — no counter, no constant to tune. */
static void test_bounce_keeps_previous_state(void)
{
    TEST_ASSERT(mouse_button_next(false, MOUSE_CONTACT_BOUNCING) == false,
                "bounce from released: stays released");
    TEST_ASSERT(mouse_button_next(true, MOUSE_CONTACT_BOUNCING) == true,
                "bounce from pressed: stays pressed");

    /* Same for the impossible state: we conclude nothing rather than invent one. */
    TEST_ASSERT(mouse_button_next(false, MOUSE_CONTACT_IMPOSSIBLE) == false,
                "impossible state from released: we conclude nothing");
    TEST_ASSERT(mouse_button_next(true, MOUSE_CONTACT_IMPOSSIBLE) == true,
                "impossible state from pressed: we conclude nothing");
}

static void test_button_transitions(void)
{
    TEST_ASSERT(mouse_button_next(false, MOUSE_CONTACT_PRESSED)  == true,  "released -> pressed");
    TEST_ASSERT(mouse_button_next(true,  MOUSE_CONTACT_RELEASED) == false, "pressed -> released");
    TEST_ASSERT(mouse_button_next(true,  MOUSE_CONTACT_PRESSED)  == true,  "pressed held");
    TEST_ASSERT(mouse_button_next(false, MOUSE_CONTACT_RELEASED) == false, "released held");
}

/* A real press as the board produces it: idle, a few bounce samples, a
 * clean press, bounce on release, idle.
 * Exactly one falling edge and one rising edge must come out of it. */
static void test_realistic_press_produces_exactly_two_edges(void)
{
    const mouse_contact_t sequence[] = {
        MOUSE_CONTACT_RELEASED, MOUSE_CONTACT_RELEASED,
        MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING,
        MOUSE_CONTACT_PRESSED,  MOUSE_CONTACT_PRESSED,   MOUSE_CONTACT_PRESSED,
        MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING,
        MOUSE_CONTACT_RELEASED, MOUSE_CONTACT_RELEASED,
    };
    bool etat = false;
    unsigned fronts = 0;
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        bool suivant = mouse_button_next(etat, sequence[i]);
        if (suivant != etat) fronts++;
        etat = suivant;
    }
    TEST_ASSERT_EQ(fronts, 2, "one press = exactly two edges, never four");
    TEST_ASSERT(etat == false, "ends up released");
}

/* The worst case: a bounce that crosses the pressed state without settling
 * there. Without the state hold, each back-and-forth would produce a pair of edges. */
static void test_chattering_does_not_multiply_clicks(void)
{
    const mouse_contact_t sequence[] = {
        MOUSE_CONTACT_RELEASED,
        MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING,
        MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING,
        MOUSE_CONTACT_BOUNCING, MOUSE_CONTACT_BOUNCING,
        MOUSE_CONTACT_PRESSED,
    };
    bool etat = false;
    unsigned fronts = 0;
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        bool suivant = mouse_button_next(etat, sequence[i]);
        if (suivant != etat) fronts++;
        etat = suivant;
    }
    TEST_ASSERT_EQ(fronts, 1, "six bounce samples = always a single edge");
}

/* ── Wheel quadrature ─────────────────────────────────────────────
 * State is (A << 1) | B. A valid step changes only one channel at a time. */
static void test_quadrature_forward(void)
{
    /* 00 -> 10 -> 11 -> 01 -> 00: one full direction. */
    TEST_ASSERT_EQ(mouse_wheel_step(0b00, 0b10), 1, "00->10 = +1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b10, 0b11), 1, "10->11 = +1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b11, 0b01), 1, "11->01 = +1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b01, 0b00), 1, "01->00 = +1");
}

static void test_quadrature_backward(void)
{
    TEST_ASSERT_EQ(mouse_wheel_step(0b00, 0b01), -1, "00->01 = -1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b01, 0b11), -1, "01->11 = -1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b11, 0b10), -1, "11->10 = -1");
    TEST_ASSERT_EQ(mouse_wheel_step(0b10, 0b00), -1, "10->00 = -1");
}

static void test_quadrature_no_change_is_zero(void)
{
    for (uint8_t v = 0; v < 4; v++)
        TEST_ASSERT_EQ(mouse_wheel_step(v, v), 0, "no change = no step");
}

/* Both channels changing within the same interval: we missed a step. Return
 * 0 rather than an invented direction — getting the direction wrong is worse
 * than losing a notch, because it scrolls the page backward instead of doing nothing. */
static void test_quadrature_impossible_transitions(void)
{
    TEST_ASSERT_EQ(mouse_wheel_step(0b00, 0b11), 0, "00->11 impossible = 0");
    TEST_ASSERT_EQ(mouse_wheel_step(0b11, 0b00), 0, "11->00 impossible = 0");
    TEST_ASSERT_EQ(mouse_wheel_step(0b01, 0b10), 0, "01->10 impossible = 0");
    TEST_ASSERT_EQ(mouse_wheel_step(0b10, 0b01), 0, "10->01 impossible = 0");
}

/* A full turn in one direction then the other must bring the counter back to
 * zero: the table must not be asymmetric. */
static void test_quadrature_round_trip_is_neutral(void)
{
    const uint8_t avant[]  = { 0b00, 0b10, 0b11, 0b01, 0b00 };
    const uint8_t arriere[] = { 0b00, 0b01, 0b11, 0b10, 0b00 };
    int total = 0;
    for (unsigned i = 0; i + 1 < sizeof(avant) / sizeof(avant[0]); i++)
        total += mouse_wheel_step(avant[i], avant[i + 1]);
    TEST_ASSERT_EQ(total, 4, "one full forward cycle = +4 quadrature steps");
    for (unsigned i = 0; i + 1 < sizeof(arriere) / sizeof(arriere[0]); i++)
        total += mouse_wheel_step(arriere[i], arriere[i + 1]);
    TEST_ASSERT_EQ(total, 0, "then a backward cycle brings it back to zero");
}

void test_mouse_input(void)
{
    printf("\n-- entrees souris Conchodytes : clics SPDT et quadrature --\n");
    test_contact_decode();
    test_bounce_keeps_previous_state();
    test_button_transitions();
    test_realistic_press_produces_exactly_two_edges();
    test_chattering_does_not_multiply_clicks();
    test_quadrature_forward();
    test_quadrature_backward();
    test_quadrature_no_change_is_zero();
    test_quadrature_impossible_transitions();
    test_quadrature_round_trip_is_neutral();
}
