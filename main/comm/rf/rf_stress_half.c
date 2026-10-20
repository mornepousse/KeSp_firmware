/* rf-scope stress generator, half side — see rf_stress_half.h. The decisions
 * (payload, frame, when the next frame is due, when the run ends) are pure
 * and host-tested in rf_stress.c; this file only wires them to a task, a
 * one-shot esp_timer, the radio owner (through the policy's sink) and the
 * sleep veto. */
#include "rf_stress_half.h"
#include "rf_stress.h"
#include "rf_trace_ctl.h"          /* rf_trace_ack_encode */
#include "rf_packet.h"             /* RF_HALF_LEFT / RF_HALF_RIGHT */
#include "cdc_binary_protocol.h"   /* KS_STATUS_* */
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"
#endif

static const char *TAG = "rfstress";

#if CONFIG_KASE_DEVICE_ROLE_SPLIT_SCANNER
#define HALF RF_HALF_RIGHT
#else
#define HALF RF_HALF_LEFT
#endif

static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static rf_stress_sched_t  s_sched;                  /* under s_mux */
static rf_stress_sink_t   s_sink;                   /* set once by the policy */
static TaskHandle_t       s_task;                   /* created at the first start */
static esp_timer_handle_t s_timer;                  /* one-shot: the scheduler's wait */

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

static void notify(void) { if (s_task) xTaskNotify(s_task, 1, eSetBits); }

static void timer_cb(void *arg) { (void)arg; notify(); }

static void veto(bool on)
{
#if CONFIG_KASE_VEILLE
    veille_veto(VEILLE_VETO_STRESS, on);
#else
    (void)on;
#endif
}

static rf_stress_tx_t verdict(radio_tx_t r)
{
    switch (r) {
    case RADIO_TX_ACK:  return RF_STRESS_TX_ACK;
    case RADIO_TX_REFUS: return RF_STRESS_TX_REFUSED;
    default:            return RF_STRESS_TX_UNAVAIL;   /* INDISPO, PERIME: nothing left the chip */
    }
}

static void stress_task(void *arg)
{
    (void)arg;
    uint32_t reported = 0;
    for (;;) {
        uint64_t wait = 0;
        rf_stress_sched_t snap;
        portENTER_CRITICAL(&s_mux);
        rf_stress_step_t st = rf_stress_step(&s_sched, now_us(), &wait);
        snap = s_sched;
        portEXIT_CRITICAL(&s_mux);

        /* The veto follows the run at every step, whatever stopped it — set
         * here as well as by the start command, so a start+stop pair handled
         * before this task ran can never leave it posted. One critical
         * section per frame: nothing next to the radio send. */
        veto(snap.on);
        if (!snap.on && snap.stopped_by != RF_STRESS_STOP_NONE && snap.session != reported) {
            reported = snap.session;
            ESP_LOGW(TAG, "session %lu stopped (%s): %lu sent (%lu acked, %lu MAX_RT), %lu unavailable",
                     (unsigned long)snap.session, rf_stress_stop_name(snap.stopped_by),
                     (unsigned long)snap.sent, (unsigned long)snap.acked,
                     (unsigned long)snap.refused, (unsigned long)snap.unavail);
        }

        if (st == RF_STRESS_SEND) {
            uint8_t f[RF_STRESS_FRAME_LEN];
            size_t n = rf_stress_frame(f, sizeof f, HALF, snap.seq);
            radio_tx_t r = s_sink ? s_sink(f, (uint8_t)n) : RADIO_TX_INDISPO;
            uint64_t t = now_us();
            portENTER_CRITICAL(&s_mux);
            rf_stress_tx_done(&s_sched, snap.session, t, verdict(r));
            portEXIT_CRITICAL(&s_mux);
            continue;                       /* the next step is a WAIT (≥ the minimum gap) */
        }
        if (st == RF_STRESS_WAIT) esp_timer_start_once(s_timer, wait);
        /* IDLE: forever, the timer stopped. WAIT: until the timer or a command. */
        xTaskNotifyWait(0, UINT32_MAX, NULL, portMAX_DELAY);
        esp_timer_stop(s_timer);            /* woken by a command: the wait is recomputed */
    }
}

static bool ensure_task(void)
{
    if (s_task) return true;
    if (!s_timer) {
        const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "rfstress" };
        if (esp_timer_create(&ta, &s_timer) != ESP_OK) return false;
    }
    /* Priority 1, like the trace's drain task: under every keyboard task, so
     * real typing preempts the generator (it still competes for the radio
     * lock — that is the load being measured). */
    return xTaskCreate(stress_task, "rfstress", 3072, NULL, 1, &s_task) == pdPASS;
}

void rf_stress_half_set_sink(rf_stress_sink_t sink)
{
    s_sink = sink;
}

static const char *pattern_name(uint8_t p)
{
    switch (p) {
    case RF_STRESS_RATE:  return "rate";
    case RF_STRESS_BURST: return "burst";
    case RF_STRESS_MAX:   return "max";
    case RF_STRESS_HELD:  return "held";
    }
    return "?";
}

uint8_t rf_stress_half_command(const uint8_t *payload, uint16_t len,
                               uint8_t *ack, size_t cap, size_t *ack_len)
{
    *ack_len = 0;
    rf_stress_cmd_t c;
    rf_stress_err_t e = rf_stress_parse(payload, len, &c);
    if (e == RF_STRESS_ERR_RANGE) return KS_STATUS_ERR_RANGE;
    if (e != RF_STRESS_OK)        return KS_STATUS_ERR_INVALID;

    uint32_t session;
    uint16_t remaining = 0;
    if (c.start) {
        if (s_sink == NULL || !ensure_task()) return KS_STATUS_ERR_UNKNOWN;
        veto(true);                         /* before the first frame: no sleep in between */
        portENTER_CRITICAL(&s_mux);
        uint64_t t = now_us();
        rf_stress_start(&s_sched, &c.p, t);
        session = s_sched.session;
        remaining = rf_stress_remaining_s(&s_sched, t);
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "session %lu: %s, rate %u Hz, count %lu, %u s, burst %u / %u ms",
                 (unsigned long)session, pattern_name(c.p.pattern), (unsigned)c.p.rate_hz,
                 (unsigned long)c.p.count, (unsigned)c.p.duration_s,
                 (unsigned)c.p.burst_len, (unsigned)c.p.burst_pause_ms);
    } else {
        portENTER_CRITICAL(&s_mux);
        session = s_sched.on ? s_sched.session : 0;
        rf_stress_stop(&s_sched, RF_STRESS_STOP_CMD);
        portEXIT_CRITICAL(&s_mux);
    }
    notify();                               /* the task applies it: frame, wait, veto, report */
    *ack_len = rf_trace_ack_encode(ack, cap, c.start ? RF_STRESS_SUB_START : RF_STRESS_SUB_STOP,
                                   session, remaining);
    return KS_STATUS_OK;
}

void rf_stress_half_sleep(void)
{
    portENTER_CRITICAL(&s_mux);
    rf_stress_stop(&s_sched, RF_STRESS_STOP_SLEEP);
    portEXIT_CRITICAL(&s_mux);
    if (s_timer) esp_timer_stop(s_timer);   /* nothing armed across the sleep */
    notify();                               /* at the wake: veto released, run reported */
}
