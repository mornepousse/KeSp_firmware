/* See mouse_wheel.h. No ESP-IDF dependency: compiled as-is on the host. */
#include "mouse_wheel.h"

/* Table indexed by (previous << 2) | current.
 *
 *   row prev=00: 00->00 = 0, 00->01 = -1, 00->10 = +1, 00->11 = impossible
 *   row prev=01: 01->00 = +1, 01->01 = 0, 01->10 = impossible, 01->11 = -1
 *   row prev=10: 10->00 = -1, 10->01 = impossible, 10->10 = 0, 10->11 = +1
 *   row prev=11: 11->00 = impossible, 11->01 = +1, 11->10 = -1, 11->11 = 0
 *
 * The diagonal is zero (no change) and the table is antisymmetric: a full
 * cycle in one direction then the other brings the counter back to zero,
 * which test_quadrature_round_trip_is_neutral verifies. */
static const int8_t QUAD[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

int8_t mouse_wheel_step(uint8_t prev_ab, uint8_t cur_ab)
{
    return QUAD[((prev_ab & 0x3) << 2) | (cur_ab & 0x3)];
}
