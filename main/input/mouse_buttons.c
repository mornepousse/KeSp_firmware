/* See mouse_buttons.h for the reasoning. No ESP-IDF dependency here:
 * this file is compiled as-is by the host test harness. */
#include "mouse_buttons.h"

mouse_contact_t mouse_contact_decode(int no_level, int nc_level)
{
    if (no_level && !nc_level) return MOUSE_CONTACT_RELEASED;
    if (!no_level && nc_level) return MOUSE_CONTACT_PRESSED;
    if (no_level && nc_level)  return MOUSE_CONTACT_BOUNCING;
    return MOUSE_CONTACT_IMPOSSIBLE;
}

bool mouse_button_next(bool prev, mouse_contact_t contact)
{
    switch (contact) {
    case MOUSE_CONTACT_PRESSED:  return true;
    case MOUSE_CONTACT_RELEASED: return false;
    /* Both ambiguous cases keep the previous state. Keeping them distinct
     * rather than merging them into `default` has a point: IMPOSSIBLE signals
     * a hardware fault and deserves to be counted separately by the caller,
     * even though the decision is the same. */
    case MOUSE_CONTACT_BOUNCING:
    case MOUSE_CONTACT_IMPOSSIBLE:
    default:
        return prev;
    }
}
