/* Handshake of the Niphargus inter-half link.
 *
 * LINK_5V_EN drives a SiP32431 load switch with a 100k pull-down: the 5 V is
 * DEAD by default. Both transmitter and receiver must close their switch
 * for current to flow, so a hot-plug cannot produce a spark.
 * A half with an empty battery cannot be woken by the TRRS —
 * assumed in the hardware design.
 *
 * -- The safety invariant --------------------------------------------------
 *
 * en_5v only becomes true after a VERIFIED EXCHANGE with the peer: either we
 * probed and received an ACK, or we were probed and answered. Time passing,
 * the mere presence of USB, or an unexpected event never raise it.
 *
 * Pure logic: the clock and the events are passed as arguments, no
 * GPIO is touched here. The caller translates the actions into gpio_set_level().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LINK_HS_PROBE_TIMEOUT_MS  200   /* wait for an ACK after the probe */
#define LINK_HS_PEER_TIMEOUT_MS   500   /* tolerated peer silence, link established */
/* Interval between two probes in IDLE as long as USB is present. USB_PRESENT
 * is an EDGE event (not a state re-read elsewhere): without a periodic
 * re-probe, a single probe lost at boot would condemn the link until the
 * cable is unplugged/replugged. Same order of magnitude as
 * LINK_HS_PROBE_TIMEOUT_MS so as not to spam the line. */
#define LINK_HS_REPROBE_INTERVAL_MS  300
/* Keepalive of the link while UP. Observed at the bench on 2026-09-11, on the
 * first 5 V closure: the link came up then dropped 500 ms later. While UP
 * the transmitter no longer probed, so the receiver had nothing left to ACK,
 * and LINK_HS_PEER_TIMEOUT_MS expired. The design relied on the MATRIX
 * frames of the wired path to keep the link alive — a path that does not
 * exist, the halves talk over radio. As long as we have current to give, we
 * say it again: two keepalives fit within the expiry delay, a lost probe
 * does not reopen the switch. */
#define LINK_HS_KEEPALIVE_MS         200

typedef enum {
    LINK_HS_IDLE = 0,   /* 5 V dead, nothing in progress */
    LINK_HS_PROBING,    /* probe sent, waiting for the ACK */
    LINK_HS_UP,         /* peer recognized, 5 V closed */
} link_hs_state_t;

typedef enum {
    LINK_HS_EV_TICK = 0,     /* time passing, nothing else */
    LINK_HS_EV_USB_PRESENT,  /* the host cable just appeared */
    LINK_HS_EV_USB_GONE,     /* the host cable has disappeared */
    LINK_HS_EV_PEER_ACK,     /* the half opposite has answered the probe */
    LINK_HS_EV_PEER_FRAME,   /* valid frame received from the peer (keeps the link alive) */
    LINK_HS_EV_PROBED,       /* the peer is probing US (PROBE frame received and verified) */
} link_hs_event_t;

typedef enum {
    LINK_HS_ACT_NONE = 0,
    LINK_HS_ACT_SEND_PROBE,   /* send the "are you really my half?" probe */
    LINK_HS_ACT_ENABLE_5V,    /* close the load switch */
    LINK_HS_ACT_DISABLE_5V,   /* reopen the load switch */
    /* Answer the probe AND close our switch. The two go together: the
     * hardware contract requires BOTH halves to close theirs for current
     * to flow, so answering without closing would be useless. Closing an
     * already-closed switch has no effect, the caller does not need to worry about it. */
    LINK_HS_ACT_ACK_AND_ENABLE_5V,
} link_hs_action_t;

typedef struct {
    link_hs_state_t state;
    uint32_t        since_ms;    /* entry into the current state */
    uint32_t        last_peer_ms;/* last sign of life from the peer */
    uint32_t        last_probe_ms;/* last probe sent (keepalive while UP) */
    bool            usb;         /* host cable present */
    bool            en_5v;       /* commanded state of the load switch */
} link_hs_t;

static inline void link_hs_init(link_hs_t *h)
{
    h->state = LINK_HS_IDLE;
    h->since_ms = 0;
    h->last_peer_ms = 0;
    h->last_probe_ms = 0;
    h->usb = false;
    h->en_5v = false;
}

static inline bool link_hs_5v_enabled(const link_hs_t *h) { return h->en_5v; }

static inline link_hs_action_t link_hs_step(link_hs_t *h, link_hs_event_t ev,
                                            uint32_t now_ms)
{
    /* USB lost: cut everything, immediately. */
    if (ev == LINK_HS_EV_USB_GONE) {
        h->usb = false;
        if (h->en_5v) {
            h->en_5v = false;
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
            return LINK_HS_ACT_DISABLE_5V;
        }
        h->state = LINK_HS_IDLE;
        h->since_ms = now_ms;
        return LINK_HS_ACT_NONE;
    }

    if (ev == LINK_HS_EV_USB_PRESENT) h->usb = true;
    if (ev == LINK_HS_EV_PEER_ACK || ev == LINK_HS_EV_PEER_FRAME ||
        ev == LINK_HS_EV_PROBED)
        h->last_peer_ms = now_ms;

    /* The peer is probing us: this is a verified exchange (the PROBE frame
     * passed its CRC before arriving here), so we answer and close our side.
     * Handled before the switch because it applies from any state — including
     * PROBING, when both halves probe each other at the same time and would
     * otherwise stay stuck waiting for each other.
     *
     * Decision made (2026-08-19): the probed half ALWAYS closes, even when it
     * has its own USB. The two 5 V rails then end up connected through the
     * jack when both cables are plugged in; the current limiting and the
     * soft-start of the load switch absorb it, but this is not the intended
     * use — to be checked at the bench. The alternative that was ruled out was
     * to not close when we are ourselves powered. */
    if (ev == LINK_HS_EV_PROBED) {
        h->state = LINK_HS_UP;
        h->since_ms = now_ms;
        h->en_5v = true;
        return LINK_HS_ACT_ACK_AND_ENABLE_5V;
    }

    switch (h->state) {
    case LINK_HS_IDLE:
        /* We only probe if we have current to give. */
        if (ev == LINK_HS_EV_USB_PRESENT) {
            h->state = LINK_HS_PROBING;
            h->since_ms = now_ms;
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        /* USB_PRESENT is an edge event: if a probe was lost
         * (a PROBING timeout brought us back here with h->usb still true),
         * nothing will ever raise it again on its own. Re-probe periodically
         * as long as USB stays there — without this, a single ACK lost at
         * boot condemns the link until the cable is unplugged. Do NOT raise
         * en_5v here: this only resends the probe, the safety property (5V
         * only after PEER_ACK) is unchanged. */
        if (ev == LINK_HS_EV_TICK && h->usb &&
            (uint32_t)(now_ms - h->since_ms) >= LINK_HS_REPROBE_INTERVAL_MS) {
            h->state = LINK_HS_PROBING;
            h->since_ms = now_ms;
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        return LINK_HS_ACT_NONE;

    case LINK_HS_PROBING:
        if (ev == LINK_HS_EV_PEER_ACK) {
            h->state = LINK_HS_UP;
            h->since_ms = now_ms;
            h->en_5v = true;
            return LINK_HS_ACT_ENABLE_5V;
        }
        if ((uint32_t)(now_ms - h->since_ms) >= LINK_HS_PROBE_TIMEOUT_MS) {
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
        }
        return LINK_HS_ACT_NONE;

    case LINK_HS_UP:
        if ((uint32_t)(now_ms - h->last_peer_ms) >= LINK_HS_PEER_TIMEOUT_MS) {
            h->en_5v = false;
            h->state = LINK_HS_IDLE;
            h->since_ms = now_ms;
            return LINK_HS_ACT_DISABLE_5V;
        }
        /* Keepalive: whoever has current to give says it again periodically.
         * The peer answers with an ACK, which refreshes last_peer_ms on both
         * sides (PROBED on its side, PEER_ACK on ours). The receiver, without
         * USB, never probes — it has nothing to give. */
        if (ev == LINK_HS_EV_TICK && h->usb &&
            (uint32_t)(now_ms - h->last_probe_ms) >= LINK_HS_KEEPALIVE_MS) {
            h->last_probe_ms = now_ms;
            return LINK_HS_ACT_SEND_PROBE;
        }
        return LINK_HS_ACT_NONE;

    default:
        /* h->state outside the enum: call contract violated (a struct on the
         * stack not passed through link_hs_init(), arbitrary memory). This
         * module is the last line of defense before the load switch's GPIO,
         * and link_hs_5v_enabled() is public — a caller querying it in this
         * state must never read a garbage `true` when no ACT_ENABLE_5V has
         * been issued. Here, the property "5 V off unless proven otherwise"
         * takes priority over diagnostics: we fall back to the safe side
         * rather than preserve an unknown value.
         */
        h->state = LINK_HS_IDLE;
        h->en_5v = false;
        return LINK_HS_ACT_NONE;
    }
    return LINK_HS_ACT_NONE;
}
