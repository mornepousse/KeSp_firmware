/* Main task of the Conchodytes mouse.
 *
 * Assembles the board's three inputs: the PMW3389 sensor, the three SPDT
 * clicks and the wheel encoder. The decoding logic lives elsewhere and is
 * tested on the host (input/mouse_buttons.c, input/mouse_wheel.c); this file
 * only reads GPIOs and paces.
 *
 * WARNING: STATE — this task does not yet produce ANY HID report. The relay
 * toward dongle slot 2 is the next milestone and will have its own spec — see
 * docs/superpowers/specs/2026-08-25-conchodytes-firmware-design.md §5 and §7,
 * the latter carrying the undecided question of saturation at ±127.
 * In the meantime it logs, which is enough to validate the hardware.
 */

#include "mouse_task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board.h"
#include "periph/pmw3389.h"
#include "input/mouse_buttons.h"
#include "input/mouse_wheel.h"
#include "comm/rf/mouse_relay_tx.h"

static const char *TAG = "mouse";

/* -- Polling ──────────────────────────────────────────────────────────────
 * 1 kHz — which REQUIRES CONFIG_FREERTOS_HZ=1000, set in
 * sdkconfig.defaults.conchodytes. With the ESP-IDF default of 100 Hz,
 * vTaskDelay(1) would be worth TEN milliseconds, enough to miss half the
 * short presses and the whole bounce window. That is exactly what
 * happened during the 2026-08-25 measurement campaign.
 *
 * WARNING: this rate suits CLICKS. It does NOT suit the wheel:
 * 60 quadrature slots make 240 transitions per turn, i.e. 2400/s on a
 * sharp flick, and it would take ~5 kHz to lose nothing. See wheel_note
 * below. */
#define SCAN_PERIOD_TICKS  1

/* The sensor is noticeably slower to poll than the GPIOs: each register
 * read costs 160 us of tSRAD, and there are eight per reading. It is
 * therefore polled less often than the buttons. */
/* WARNING: WAS AT 8 — i.e. one sensor read every 8 ms, 125 reports/s.
 *
 * Two flaws, both felt as imprecision:
 *
 * 1. MOVEMENT IN BATCHES. Seven empty reports then one carrying 8 ms of motion.
 *    The cursor advances in jerks instead of tracking smoothly.
 * 2. CLIPPING. `dx` travels on an int8, ±127 per report. At 125 Hz and
 *    BOARD_SNS_CPI = 1000 cpi, the ceiling is 127 x 125 = 15,875 counts/s,
 *    i.e. ~40 cm/s. Beyond that the excess is not lost (it carries over to
 *    the following frames) but it arrives LATE: the cursor lags then
 *    catches up. A brisk gesture exceeds a meter per second.
 *
 * WARNING: TRIED AT 1 (1 kHz), AND IT WAS WORSE. The clipping ceiling became
 * comfortable, but the CHAIN only delivers about 130 packets/s: sending a
 * thousand times a second to get a hundred thirty through saturates the link
 * and makes movement arrive in irregular bursts. Felt on the bench on
 * 2026-08-26: "jerky, in fits and starts". A REGULAR rate fully
 * delivered beats a high rate two-thirds lost.
 *
 * At 4, i.e. 250 Hz: under the chain's ceiling, so every frame arrives, and
 * clipping only bites past 127 x 250 = 31,750 counts/s ~= 80 cm/s — twice
 * the margin of the original 125 Hz.
 *
 * No frame is sent at rest (see mouse_task): the cost follows the
 * gesture, it is not permanent. */
#define SENSOR_EVERY_N_SCANS  4

typedef struct {
    gpio_num_t  no, nc;
    bool        pressed;
    const char *nom;
    uint32_t    fronts;
    uint32_t    ambigus;   /* BOUNCING or IMPOSSIBLE samples */
} bouton_t;

static bouton_t s_boutons[3] = {
    { BOARD_SW_LEFT_GPIO,  BOARD_SW_LEFT_NC_GPIO,  false, "left",   0, 0 },
    { BOARD_SW_RIGHT_GPIO, BOARD_SW_RIGHT_NC_GPIO, false, "right",  0, 0 },
    { BOARD_SW_MID_GPIO,   BOARD_SW_MID_NC_GPIO,   false, "middle", 0, 0 },
};

static int32_t s_wheel;
static uint32_t s_wheel_missed;

/* Anti-glitch: discards ISOLATED movement on a single sample.
 *
 * WARNING: THIS FILTER DOES NOT JUDGE ON AMPLITUDE, AND THAT IS THE WHOLE
 * POINT. A first attempt on 2026-08-26 had set a threshold on the amount of
 * movement: it ate fine gestures while letting tremor through, because
 * amplitude does not separate the two. DURATION does.
 *
 * Measured on the bench, mouse motionless, 63 phantom-movement bursts:
 * median length of 1 sample, maximum 3, and 42 out of 63 fitting on a
 * single sample. A real gesture, at 250 Hz, occupies TENS of consecutive
 * samples — even the briefest one. The criterion is therefore clear-cut: what
 * has no movement before or after is isolated.
 *
 * Cost: one sample of delay (4 ms), the time to see the next one before
 * deciding. And on a real gesture, at worst the very first sample is lost
 * if it is preceded and followed by nothing — a few counts, bounded, against
 * dozens of suppressed glitches.
 *
 * WARNING: THIS ONLY HANDLES THE "GLITCH" REGIME. The sensor also produces,
 * on some surfaces, COHERENT drifts of ~1000 counts over several
 * seconds (measured: dy=+996 in 2.5 s, mouse motionless). Those are
 * indistinguishable from a slow intentional gesture and NO filter can remove
 * them. The cause is optical — see NOTES-V2 §8, Conchodytes repo. */
static void anti_tache(int16_t *dx, int16_t *dy)
{
    static int16_t att_x, att_y;   /* sample awaiting a decision */
    static bool    avant_bouge;    /* the previous sample carried movement */

    bool ici_bouge = (*dx || *dy);
    bool att_bouge = (att_x || att_y);

    int16_t sort_x = att_x, sort_y = att_y;
    if (att_bouge && !avant_bouge && !ici_bouge) {
        sort_x = 0; sort_y = 0;    /* isolated glitch: discard it */
    }

    att_x = *dx; att_y = *dy;
    avant_bouge = att_bouge;

    *dx = sort_x; *dy = sort_y;
}

/* Adaptive smoothing of the displacement — dampens tremor without clipping gestures.
 *
 * WARNING: WHY NOT A GATE. A filter that DISCARDS movement under a threshold
 * was tried on the bench on 2026-08-26 then removed: it ate fine gestures AND
 * did not calm tremor. The reason is that, with a hand resting on the mouse,
 * the micro-movement crosses any threshold, opens the gate, and everything
 * goes through — while slow intentional displacements stayed
 * under it. The worst of both worlds.
 *
 * HERE NOTHING IS DISCARDED. The displacement is passed through a low-pass
 * filter whose DC gain is 1: TOTAL DISTANCE IS PRESERVED, only its
 * distribution over time changes. The worst case is therefore a slight delay,
 * never an erased gesture.
 *
 * The smoothing ADAPTS TO SPEED, like the "1 euro" filter (Casiez, Roussel,
 * Vogel, CHI 2012), designed for pointing noise:
 *   - a tremor is slow and oscillating -> heavily smoothed;
 *   - a gesture is fast and coherent   -> passes through as-is, no added delay.
 * It is this coupling that avoids the usual trade-off between stability at
 * rest and responsiveness in motion: there is no choice to make, speed decides.
 *
 * The fraction not yet emitted is PRESERVED from one sample to the next
 * (`reste_*`, in Q8). Without this, rounding to an integer would clip a few
 * counts on every report and the mouse would travel less than the hand — the
 * very defect that was just fixed on the radio side. */
static void lissage(int16_t *dx, int16_t *dy)
{
#if BOARD_SNS_LISSAGE_ALPHA_MIN < 256
    static int32_t etat_x, etat_y;     /* smoothed velocity, Q8 */
    static int32_t reste_x, reste_y;   /* fraction not yet emitted, Q8 */

    int32_t ax = *dx < 0 ? -*dx : *dx;
    int32_t ay = *dy < 0 ? -*dy : *dy;
    int32_t v  = ax + ay;
    if (v > BOARD_SNS_LISSAGE_VITESSE_MAX) v = BOARD_SNS_LISSAGE_VITESSE_MAX;

    /* alpha ranges from ALPHA_MIN (motionless) to 256 (fast): 256 = pass through. */
    int32_t alpha = BOARD_SNS_LISSAGE_ALPHA_MIN
                  + ((256 - BOARD_SNS_LISSAGE_ALPHA_MIN) * v)
                    / BOARD_SNS_LISSAGE_VITESSE_MAX;

    etat_x += ((((int32_t)*dx) * 256) - etat_x) * alpha / 256;
    etat_y += ((((int32_t)*dy) * 256) - etat_y) * alpha / 256;

    reste_x += etat_x;
    reste_y += etat_y;
    int32_t sx = reste_x / 256, sy = reste_y / 256;
    reste_x -= sx * 256;
    reste_y -= sy * 256;

    *dx = (int16_t)sx;
    *dy = (int16_t)sy;
#else
    (void)dx; (void)dy;
#endif
}

static void entrees_init(void)
{
    /* The pull resistors are EXTERNAL — 10 k on the six contacts (R105-R110) as
     * on the two encoder lines (R103/R104). The S3's internal ones are not
     * armed, since they would sit in parallel and skew the thresholds. */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << BOARD_SW_LEFT_GPIO)  | (1ULL << BOARD_SW_LEFT_NC_GPIO)  |
                        (1ULL << BOARD_SW_RIGHT_GPIO) | (1ULL << BOARD_SW_RIGHT_NC_GPIO) |
                        (1ULL << BOARD_SW_MID_GPIO)   | (1ULL << BOARD_SW_MID_NC_GPIO)   |
                        (1ULL << BOARD_ENC_A_GPIO)    | (1ULL << BOARD_ENC_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

static inline uint8_t lire_encodeur(void)
{
    return (uint8_t)((gpio_get_level(BOARD_ENC_A_GPIO) << 1) |
                      gpio_get_level(BOARD_ENC_B_GPIO));
}

static void mouse_task(void *arg)
{
    (void)arg;

    uint8_t enc_prev = lire_encodeur();
    unsigned n = 0;
    int32_t acc_dx = 0, acc_dy = 0;

    uint8_t last_squal = 0; uint16_t last_shutter = 0;
    int32_t rf_dx = 0, rf_dy = 0, rf_wheel = 0;   /* remainder still to send */
    uint32_t satur = 0;                            /* clipped reports */
    uint32_t perdues = 0;                          /* unacknowledged frames, counts refunded */
    int32_t wheel_env = 0;

    /* LQ1 was dead on the v1 and pinned both lines to 0.65 V
     * (NOTES-V2.md §1bis). Until it is replaced, `lire_encodeur` always
     * returns 0 and no step is counted. It is reported once at startup
     * rather than letting it look like a software fault.
     *
     * 00 at startup is not abnormal in itself — it is a valid quadrature
     * state. What is abnormal is that it never changes. */
    if (enc_prev == 0)
        ESP_LOGW(TAG, "wheel: ENC_A and ENC_B at 0 on startup — LQ1 saturated, "
                      "known on v1, see Conchodytes/NOTES-V2.md §1bis");

    while (1) {
        for (unsigned i = 0; i < 3; i++) {
            bouton_t *b = &s_boutons[i];
            mouse_contact_t c = mouse_contact_decode(gpio_get_level(b->no),
                                                     gpio_get_level(b->nc));
            if (c == MOUSE_CONTACT_BOUNCING || c == MOUSE_CONTACT_IMPOSSIBLE)
                b->ambigus++;

            bool suivant = mouse_button_next(b->pressed, c);
            if (suivant != b->pressed) {
                b->pressed = suivant;
                b->fronts++;
                ESP_LOGI(TAG, "click %s: %s", b->nom, suivant ? "pressed" : "released");
            }
        }

        /* WARNING: PROVISIONAL — this polling is structurally too slow.
         *
         * At 240 transitions per turn, one turn per second already produces
         * 240, against 1000 samples/s here: the margin disappears as soon
         * as it spins a bit fast, and the lost steps are counted in
         * s_wheel_missed then discarded — the wheel "sticks" without signaling anything.
         *
         * The right answer is the S3's PCNT peripheral, which decodes
         * quadrature in hardware: signed counter, anti-glitch filter, no
         * CPU wakeup, and no lost step whatever the speed. Not written
         * for now because the encoder is dead-shorted to ground
         * (Conchodytes/NOTES-V2.md §1bis) and neither approach can be
         * validated on the board before repair. */
        uint8_t enc = lire_encodeur();
        if (enc != enc_prev) {
            int8_t pas = mouse_wheel_step(enc_prev, enc);
            if (pas) s_wheel += pas;
            else     s_wheel_missed++;   /* both lines changed: missed step */
            enc_prev = enc;
        }

        if (++n % SENSOR_EVERY_N_SCANS == 0) {
            pmw3389_motion_t m;
            if (pmw3389_read_motion(&m) == ESP_OK) {
                anti_tache(&m.dx, &m.dy); /* discards the isolated, keeps the continuous */
                lissage(&m.dx, &m.dy);    /* then smooths what remains */

                rf_dx += m.dx;
                rf_dy += m.dy;
                /* The burst clears the counters on every read: it is
                 * accumulated here, otherwise each reading is only worth 8 ms
                 * of movement and the log fills up with crumbs. */
                acc_dx += m.dx;
                acc_dy += m.dy;

                last_squal = m.squal;
                last_shutter = m.shutter;
            }
        }

        /* -- HID transmission to dongle slot 2 ─────────────────────────────
         *
         * The frame carries x, y and wheel as int8_t: ±127 per report. The
         * remainder is ACCUMULATED rather than clipped outright — a brisk
         * movement ends up spread over the following reports instead of
         * being lost. The `satur` counter measures how many times the
         * bound is hit: that is the figure that will settle spec §7,
         * between clipping, accumulating, and extending the frame to int16_t.
         *
         * The middle button occupies bit 2 of the buttons byte, per the
         * standard mouse HID descriptor (left=0, right=1, middle=2). */
        if (mouse_relay_active()) {
            int32_t w = s_wheel, dw = w - wheel_env; wheel_env = w;
            rf_wheel += dw;

            int8_t px = (rf_dx >  127) ? 127 : (rf_dx < -127) ? -127 : (int8_t)rf_dx;
            int8_t py = (rf_dy >  127) ? 127 : (rf_dy < -127) ? -127 : (int8_t)rf_dy;
            int8_t pw = (rf_wheel >  127) ? 127 : (rf_wheel < -127) ? -127 : (int8_t)rf_wheel;
            if (px != rf_dx || py != rf_dy || pw != rf_wheel) satur++;
            rf_dx -= px; rf_dy -= py; rf_wheel -= pw;

            uint8_t btn = (uint8_t)((s_boutons[0].pressed ? 1 : 0) |
                                    (s_boutons[1].pressed ? 2 : 0) |
                                    (s_boutons[2].pressed ? 4 : 0));

            static uint8_t btn_env; static bool jamais;
            if (px || py || pw || btn != btn_env || !jamais) {
                if (mouse_relay_send(btn, px, py, pw)) {
                    btn_env = btn; jamais = true;
                } else {
                    /* WARNING: UNACKNOWLEDGED FRAME: THE COUNTS ARE REFUNDED.
                     * Without radio retransmission (ARC=0, see mouse_relay_init),
                     * this frame is lost for good. Since the displacement is
                     * RELATIVE, the accumulator had already been cleared
                     * above: without this refund, this bit of gesture
                     * vanishes and the cursor travels less than the hand.
                     *
                     * The frame is NOT resent — replaying a relative value
                     * would advance twice. The counts are put back, and the
                     * next frame carries the sum: a sum of displacements is a
                     * displacement, the operation is correct by construction.
                     *
                     * `btn_env` is NOT updated either: button state is
                     * absolute, it must be retried as-is. */
                    rf_dx += px; rf_dy += py; rf_wheel += pw;
                    perdues++;
                }
            }
        }

        /* Periodic summary — provisional, for the time it takes to validate
         * the hardware. Will disappear once the task produces real HID reports. */
        if (n % 500 == 0) {
            ESP_LOGI(TAG, "dx=%+6ld dy=%+6ld SQUAL=%3u Shutter=%5u | G%d D%d M%d"
                          " | wheel %+5ld (rates %lu)",
                     (long)acc_dx, (long)acc_dy, last_squal, last_shutter,
                     s_boutons[0].pressed, s_boutons[1].pressed, s_boutons[2].pressed,
                     (long)s_wheel, (unsigned long)s_wheel_missed);
            if (mouse_relay_active()) {
                uint32_t tx, ack;
                mouse_relay_stats(&tx, &ack);
                ESP_LOGI(TAG, "  radio: %lu frames, %lu acked (%lu%%) | "
                              "remaining dx=%ld dy=%ld | %lu clipped | %lu refunded",
                         (unsigned long)tx, (unsigned long)ack,
                         (unsigned long)(tx ? ack * 100 / tx : 0),
                         (long)rf_dx, (long)rf_dy, (unsigned long)satur,
                         (unsigned long)perdues);
            }
            acc_dx = acc_dy = 0;
        }

        vTaskDelay(SCAN_PERIOD_TICKS);
    }
}

esp_err_t mouse_task_start(void)
{
    /* NVS is now initialized by app_main() for all roles
     * (see main.c) — no longer needed here. */
    entrees_init();

    /* The sensor BEFORE the radio: it is the one that initializes the shared
     * SPI bus (mouse_relay_tx.h explains why the order matters). */
    esp_err_t err = pmw3389_init();
    if (err != ESP_OK) {
        /* The sensor can be missing without the mouse becoming unusable:
         * clicks and the wheel remain readable. It is logged loudly and it
         * starts anyway — a mouse that clicks beats a dead mouse, and the
         * message says what to look at. */
        ESP_LOGE(TAG, "PMW3389 sensor absent or mute (%s) — clicks and wheel "
                      "only", esp_err_to_name(err));
    }

    err = mouse_relay_init();
    if (err != ESP_OK) {
        /* A mouse without radio remains diagnosable over the serial port; it
         * is logged loudly and it starts anyway. */
        ESP_LOGE(TAG, "radio absent or mute (%s) — no link with the dongle",
                 esp_err_to_name(err));
    } else if (!mouse_relay_active()) {
        /* Not paired: one attempt at startup. The dongle's window must be
         * open (KS_CMD_RF_PAIR_START) — otherwise the attempt fails cleanly
         * after twenty tries and the mouse starts anyway.
         *
         * WARNING: provisional, for the bench. In real use, pairing will need
         * to be triggered by an explicit gesture — a long press on the three
         * clicks, for example — rather than on every startup: a mouse
         * constantly trying to pair transmits on the rendezvous for nothing,
         * and on battery that has a cost. */
        ESP_LOGW(TAG, "unpaired: attempting pairing at startup");
        mouse_relay_pair();
    }

    BaseType_t ok = xTaskCreate(mouse_task, "mouse", 4096, NULL, 10, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
