#pragma once
/* Sleep vetoes — pure logic, tested on host (test/test_veille_veto.c).
 *
 * A veto is a NAMED lock, on the model of esp_pm locks: the module that
 * has a reason to prevent sleep declares it, sleep does not ask anyone.
 * Until 2026-09-18 the decision was made in two tasks with two rules
 * (left: usb || link; right: link) — every new blocker required finding
 * both places.
 *
 * It is a STATE per name, not a counter: each module only sets its own
 * and clears it when its reason disappears; setting it twice then
 * clearing once = cleared. The FreeRTOS wiring (critical section, task)
 * is in veille_task.c; here nothing but the truth table. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    VEILLE_VETO_USB  = 1u << 0,   /* USB host ready (left: HID keyboard, the host is waiting) */
    VEILLE_VETO_LIEN = 1u << 1,   /* TRRS 5 V active: one half is charging the other */
    VEILLE_VETO_SYNC = 1u << 2,   /* keymap pull via ACK payload in progress */
    VEILLE_VETO_TEST = 1u << 3,   /* matrix test mode (CDC) */
    VEILLE_VETO_PAIR = 1u << 4,   /* pairing active: radio_pair_round holds the chip ~150 ms per round,
                                   * 30-40 s without anyone typing — asleep, radio_sleep was cutting
                                   * the chip out from under the pairing task (review 2026-09-20) */
    VEILLE_VETO_TOUCHE = 1u << 5, /* a key of THIS half is held: no matrix change, so inactivity grew
                                   * and the half slept key down, woke on its high row, slept again
                                   * (2026-09-25, needed once the threshold went from 15 s to 5 s) */
} veille_veto_t;

/* Room for every veto name joined by '+' in the heartbeat, NUL included:
 * "usb+link+sync+test+pair+key" = 27 characters. Sized once, here — the HB
 * and its "sleep REFUSED" line use it, the test checks all six fit. */
#define VEILLE_VETOS_STR_MAX 32

typedef struct { uint32_t actifs; } veille_vetos_t;

static inline void veille_veto_poser(veille_vetos_t *v, veille_veto_t quoi, bool on)
{
    if (on) v->actifs |=  (uint32_t)quoi;
    else    v->actifs &= ~(uint32_t)quoi;
}

static inline bool veille_bloquee(const veille_vetos_t *v) { return v->actifs != 0; }

/* Is any key of this half's matrix held? `etat` is the scanned matrix, one
 * byte per key, non-zero = pressed (MATRIX_STATE). Feeds VEILLE_VETO_TOUCHE. */
static inline bool veille_touche_tenue(const uint8_t *etat, size_t n)
{
    if (etat == NULL) return false;
    for (size_t i = 0; i < n; i++) if (etat[i]) return true;
    return false;
}

/* Names of active vetoes for the heartbeat: "usb+link", "-" if
 * none. Bounded to n bytes (n >= 2), cleanly truncated beyond that — the six
 * fit within VEILLE_VETOS_STR_MAX. */
static inline const char *veille_vetos_str(const veille_vetos_t *v, char *out, size_t n)
{
    static const struct { veille_veto_t q; const char *nom; } noms[] = {
        { VEILLE_VETO_USB, "usb" }, { VEILLE_VETO_LIEN, "link" },
        { VEILLE_VETO_SYNC, "sync" }, { VEILLE_VETO_TEST, "test" },
        { VEILLE_VETO_PAIR, "pair" }, { VEILLE_VETO_TOUCHE, "key" },
    };
    if (n == 0) return "";
    out[0] = '\0';
    size_t len = 0;
    for (size_t i = 0; i < sizeof noms / sizeof noms[0]; i++) {
        if (!(v->actifs & (uint32_t)noms[i].q)) continue;
        const char *sep = len ? "+" : "";
        size_t need = strlen(sep) + strlen(noms[i].nom);
        if (len + need + 1 > n) break;
        memcpy(out + len, sep, strlen(sep)); len += strlen(sep);
        memcpy(out + len, noms[i].nom, strlen(noms[i].nom)); len += strlen(noms[i].nom);
        out[len] = '\0';
    }
    if (len == 0 && n >= 2) { out[0] = '-'; out[1] = '\0'; }
    return out;
}
