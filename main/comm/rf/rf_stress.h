/* rf-scope stress generator — the PURE part: request payload, the stress
 * frame, and the scheduler (when the next frame is due, when the run stops).
 *
 * Spec: docs/superpowers/specs/2026-10-20-rf-scope-design.md, "Stress tests".
 * No ESP-IDF include: host-tested in test/test_rf_stress.c. The glue (task,
 * timer, radio, veto, UART0 command) is rf_stress_half.c.
 *
 * ── HARD RULE: a stress frame never reaches the host as a keystroke ──────
 * The frame is of its own kind, PKT_TYPE_STRESS (0xA): [0xA0 | half][seq]
 * [padding to the size of a MATRIX frame]. The dongle drops it before every
 * handler, traced or not (rf_rx_dispatch, rf_trace_ctl.h); the left's local
 * engine never sees one (the generator hands it straight to the radio owner,
 * and the left's only receive path — the right re-emitted by the dongle —
 * decodes HEARTBEAT frames only, and drops stress frames first). It goes
 * through the radio owner like a real frame (same lock, same ESB retries,
 * the same PRX→PTX→PRX excursion on a left on USB) and gets the same TX
 * record; it carries its OWN 8-bit seq counter.
 *
 * ── Request payload (KS frame, cmd 0xBB, on the half's UART0) ────────────
 *   stop : [0x00]
 *   start: [0x01][pattern u8][rate_hz u16][count u32][duration_s u16]
 *          [burst_len u8][burst_pause_ms u16]                 13 bytes, LE
 *   pattern  RATE  (0) one frame every 1/rate_hz s, on a grid (1..2000 Hz);
 *            BURST (1) burst_len frames back to back (≥ 1), then a pause of
 *                      burst_pause_ms counted from the end of the last TX;
 *            MAX   (2) the next frame as soon as the previous TX completed;
 *            HELD  (3) one frame every 100 ms — the halves' re-assertion
 *                      of held keys (rate ignored).
 *   count    frames that LEFT the chip (ACK or MAX_RT), 0 = no count;
 *   duration 0 = RF_STRESS_DEFAULT_S without a count, RF_STRESS_COUNT_CAP_S
 *            with one: a run ALWAYS ends on its own, whatever the radio does.
 * The reply is the 7-byte ack of rf_trace_ctl.h ([sub][session u32]
 * [remaining_s u16]); ERR_INVALID (length, sub-command, pattern),
 * ERR_RANGE (rate, burst length), ERR_UNKNOWN (no radio / not paired).
 *
 * ── Scheduler: times are esp_timer µs (u64, no wrap) ─────────────────────
 * A frame that cannot go out NOW is never sent in a hurry later: a late RATE
 * frame is rebased (no catch-up storm). Every frame is followed by at least
 * RF_STRESS_MIN_GAP_US during which the generator BLOCKS — the radio send
 * busy-polls the chip, so MAX without a gap would starve IDLE (task
 * watchdog). An unavailable radio (lock held, asleep) consumes nothing and
 * backs off RF_STRESS_BACKOFF_US — never a spin. */
#ifndef RF_STRESS_H
#define RF_STRESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define RF_STRESS_CMD            0xBBu   /* = KS_CMD_RF_STRESS (static-asserted in rf_stress.c) */
#define RF_STRESS_SUB_STOP       0x00u
#define RF_STRESS_SUB_START      0x01u
#define RF_STRESS_START_LEN      13u

#define RF_STRESS_RATE           0u
#define RF_STRESS_BURST          1u
#define RF_STRESS_MAX            2u
#define RF_STRESS_HELD           3u

#define RF_STRESS_HELD_PERIOD_MS 100u    /* KBD_RELAY_REAFFIRM_MS / HALF_TX_REFRESH_MS */
#define RF_STRESS_RATE_MAX_HZ    2000u
#define RF_STRESS_MIN_GAP_US     200u    /* the task blocks at least this long between frames */
#define RF_STRESS_BACKOFF_US     10000u  /* radio unavailable: try again later, never spin */
#define RF_STRESS_DEFAULT_S      30u     /* neither count nor duration */
#define RF_STRESS_COUNT_CAP_S    600u    /* a count alone still ends in time */
#define RF_STRESS_FRAME_LEN      6u      /* = a MATRIX frame: 2 + RF_HALF_BITMAP_BYTES */

typedef struct {
    uint8_t  pattern;
    uint16_t rate_hz;
    uint32_t count;            /* 0 = none */
    uint16_t duration_s;       /* never 0 after parse */
    uint8_t  burst_len;
    uint16_t burst_pause_ms;
} rf_stress_params_t;

typedef struct {
    bool start;                /* false = stop */
    rf_stress_params_t p;      /* start only */
} rf_stress_cmd_t;

typedef enum {
    RF_STRESS_OK = 0,
    RF_STRESS_ERR_LEN,         /* wrong length for the sub-command */
    RF_STRESS_ERR_SUB,         /* unknown sub-command */
    RF_STRESS_ERR_PATTERN,     /* unknown pattern */
    RF_STRESS_ERR_RANGE,       /* rate 0 / above the max (RATE), burst of 0 frames (BURST) */
} rf_stress_err_t;

rf_stress_err_t rf_stress_parse(const uint8_t *p, uint16_t len, rf_stress_cmd_t *out);

/* [0xA0 | half][seq][0 × 4]. Returns RF_STRESS_FRAME_LEN, 0 if cap is too small. */
size_t rf_stress_frame(uint8_t *out, size_t cap, uint8_t half, uint8_t seq);

typedef enum {
    RF_STRESS_TX_ACK = 0,      /* went out, acknowledged */
    RF_STRESS_TX_REFUSED,      /* went out, MAX_RT */
    RF_STRESS_TX_UNAVAIL,      /* did NOT go out: lock held, asleep, wrong mode */
} rf_stress_tx_t;

typedef enum {
    RF_STRESS_STOP_NONE = 0,
    RF_STRESS_STOP_COUNT,
    RF_STRESS_STOP_DURATION,
    RF_STRESS_STOP_CMD,
    RF_STRESS_STOP_SLEEP,
} rf_stress_stop_t;

typedef struct {
    rf_stress_params_t p;
    bool     on;
    uint32_t session;          /* the running (or last) run's number, never 0 once started */
    uint32_t last_session;     /* numbering */
    uint64_t start_us, end_us;
    uint64_t next_us;          /* the next frame's due time */
    uint32_t sent, acked, refused, unavail;
    uint8_t  in_burst;         /* frames of the current burst already sent */
    uint8_t  seq;              /* the NEXT frame's seq */
    rf_stress_stop_t stopped_by;
} rf_stress_sched_t;

void rf_stress_init(rf_stress_sched_t *s);
/* A new run (new session, seq 0, counters 0), the first frame due now. A
 * start while running replaces the run. */
void rf_stress_start(rf_stress_sched_t *s, const rf_stress_params_t *p, uint64_t now_us);
/* Ends a running run (no-op when off: the first reason stays). */
void rf_stress_stop(rf_stress_sched_t *s, rf_stress_stop_t why);

typedef enum {
    RF_STRESS_IDLE = 0,        /* off (or just ended): block until a command */
    RF_STRESS_SEND,            /* build a frame with s->seq and send it now */
    RF_STRESS_WAIT,            /* block *wait_us (> 0), then step again */
} rf_stress_step_t;

/* Ends the run first when its count or duration is reached. */
rf_stress_step_t rf_stress_step(rf_stress_sched_t *s, uint64_t now_us, uint64_t *wait_us);
/* After a SEND: `session` is the one read with the step (a TX that completes
 * after a stop or a restart changes nothing), now_us the end of the TX. */
void rf_stress_tx_done(rf_stress_sched_t *s, uint32_t session, uint64_t now_us, rf_stress_tx_t r);
/* Whole seconds left, rounded up; 0 when off. */
uint16_t rf_stress_remaining_s(const rf_stress_sched_t *s, uint64_t now_us);

const char *rf_stress_stop_name(rf_stress_stop_t why);

#endif /* RF_STRESS_H */
