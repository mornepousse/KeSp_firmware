/* rf-scope, half side — ring, drain task, UART0 out and in, sleep hook.
 * The decisions are pure and host-tested (rf_trace_half.c); this file only
 * moves bytes. Spec: docs/superpowers/specs/2026-10-20-rf-scope-design.md.
 *
 * ── Threads ──────────────────────────────────────────────────────────────
 *  - PRODUCERS (never block, never log): the radio owner after each
 *    transmission (lock held, any task: relay, half_link, refresh), and the
 *    drain task / sleep hook for the periodic records. Pushes go through a
 *    spinlock (a 24-byte copy) because the ring is single-producer.
 *  - CONSUMER = whoever holds s_out (a mutex): the drain task, or the sleep
 *    hook flushing the SLEEPING record before the half sleeps.
 *  - The drain task (prio 1) blocks on its notification: data pushed, bytes
 *    received, or the next periodic record (1 s, and only while tracing —
 *    tracing off, it blocks forever: no poller, no cadence added).
 *
 * ── UART0 out: the console's own lock, never a wait under it ─────────────
 * The bytes are exactly rf_trace_frame_encode()'s (a KR of 0xBA), written
 * straight into the TX FIFO — not through stdout, whose CRLF translation
 * would corrupt any 0x0A. The console's FILE lock is held around the write:
 * every printf/ESP_LOG takes it for each vprintf, so no console write() can
 * land INSIDE a frame (text may be split around one: the host resyncs).
 * ROM prints (esp_rom_printf, panic) bypass it — a frame cut by one fails its
 * CRC and is lost, nothing worse.
 * The lock is held for a FIFO copy only (≤ 128 bytes, microseconds), never
 * across a wait: a batch is sized to the TX FIFO's free room at that instant
 * (rf_trace_batch_fit) and popped only then, so a whole frame always goes in
 * at once. When the FIFO is full the records stay in the ring, the task
 * retries a tick later WITHOUT any lock held, and a ring that fills up in the
 * meantime counts its drops (reported by the next periodic record). A
 * higher-priority task's ESP_LOG therefore never waits behind this prio-1
 * task for more than one FIFO copy.
 *
 * ── UART0 in: an RX interrupt, no driver ─────────────────────────────────
 * The console UART0 is the ROM's: configured at boot on the XTAL clock, no
 * driver installed. The reader installs NO driver either: one interrupt
 * (RX FIFO threshold + RX timeout) copies the bytes into a 64-byte ring and
 * notifies the drain task, which assembles KS requests (rf_trace_ks_rx_feed).
 * Why not uart_driver_install: on UART1 an installed driver kept the 40 MHz
 * crystal alive through light sleep — 3.4 mA measured (CLAUDE.md, link_uart.c)
 * — while the console's bare UART0 is part of the 0.67 mA sleep baseline.
 * The reader changes nothing of that: same clock source, same pins and their
 * sleep configuration, no UART wake source, no PM lock, no veto. An enabled
 * interrupt costs nothing asleep.
 * The price: bytes that arrive while the half sleeps (automatic light sleep
 * between keystrokes, or the 5 s veille) are not received. The host therefore
 * repeats the start request until the KR ack comes back — at most a key
 * press away.
 * RX0 (GPIO44) with the ESP-Prog unplugged: the pad's reset state is input +
 * weak pull-up (ESP32-S3 TRM v1.8, Table 6.12-1 "IO MUX Pin Functions",
 * p. 494: GPIO44/U0RXD, RST = 3 → IE = 1, WPU = 1), so it idles high, not
 * floating. rx_init() re-asserts the pull-up anyway (same IO_MUX function,
 * U0RXD untouched) in case anything cleared it, and the drain task returns
 * to sleep at once on an RX notification that completed no request.
 *
 * ── Timing: tracing perturbs it slightly ─────────────────────────────────
 * While tracing, the drain task wakes for every frame pushed and, while a
 * backlog waits for FIFO room, every tick; the sleep hook spends up to the
 * transmission time of what the TX FIFO holds (≤ 128 chars, ~11 ms at
 * 115200 baud, ~3 ms for the SLEEPING frame alone) so that the record leaves
 * BEFORE the sleep. Tracing must be OFF for current measurements — off, none
 * of this runs (COMPORTEMENTS.md, smoke "RF trace"). */
#include "rf_trace_uart.h"
#include "rf_trace_half.h"
#include "radio_owner.h"
#include "cdc_binary_protocol.h"   /* KS_CMD_RF_TRACE, KS_STATUS_* */
#include "cadence.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "soc/interrupts.h"
#include "soc/uart_pins.h"
#include "hal/uart_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#if CONFIG_KASE_VEILLE
#include "veille_task.h"
#endif
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#endif

_Static_assert(CONFIG_ESP_CONSOLE_UART_NUM == 0, "rf-scope frames ride the console UART0 (the ESP-Prog)");
CADENCE_REPOS_OK(RF_TRACE_PERIODIC_MS);

static const char *TAG = "rftrace";

#if CONFIG_KASE_DEVICE_ROLE_SPLIT_SCANNER
#define EMITTER RF_TRACE_EMIT_RIGHT
#else
#define EMITTER RF_TRACE_EMIT_LEFT
#endif

#define RING_CAP   128u          /* ~3 KB; a full batch of 16 TX records is 23 + 15×13 B */
#define RXB_CAP    64u
#define NOTE_DATA  0x1u
#define NOTE_RX    0x2u
#define NOTE_WAKE  0x4u
#define UART0_US_PER_CHAR ((10u * 1000000u + CONFIG_ESP_CONSOLE_UART_BAUDRATE - 1u) / CONFIG_ESP_CONSOLE_UART_BAUDRATE)
#define REPLY_TRIES 3            /* a full FIFO empties in ~11 ms: three ticks are plenty */

volatile uint32_t g_rf_trace_session;

static rf_trace_rec_t    s_slots[RING_CAP];
static rf_trace_ring_t   s_ring;
static portMUX_TYPE      s_push_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t          s_frames, s_fails;          /* since the last periodic record, under s_push_mux */
static portMUX_TYPE      s_ctl_mux = portMUX_INITIALIZER_UNLOCKED;
static rf_trace_sess_t   s_ctl;                      /* under s_ctl_mux */
static bool              s_woke_pending;             /* under s_ctl_mux */
static SemaphoreHandle_t s_out;                      /* the consumer: ring pop + UART0 writes */
static TaskHandle_t      s_task;
static uint8_t           s_frame[RF_TRACE_FRAME_MAX];
static rf_trace_rec_t    s_batch[RF_TRACE_BATCH_MAX];
static volatile uint8_t  s_rxb[RXB_CAP];
static volatile uint32_t s_rx_head, s_rx_tail;       /* ISR writes head, the task tail */
static rf_trace_ks_rx_t  s_ks;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void notify(uint32_t bits) { if (s_task) xTaskNotify(s_task, bits, eSetBits); }

static void push(const rf_trace_rec_t *rec)
{
    portENTER_CRITICAL_SAFE(&s_push_mux);
    rf_trace_ring_push(&s_ring, rec);
    portEXIT_CRITICAL_SAFE(&s_push_mux);
}

/* ── Producers ── */

void rf_trace_uart_tx(const rf_trace_tx_t *r)
{
    rf_trace_rec_t rec = { .kind = RF_TRACE_KIND_TX };
    rec.u.tx = *r;
    rec.u.tx.t_us = (uint32_t)esp_timer_get_time();
    rec.u.tx.emitter = EMITTER;
    portENTER_CRITICAL_SAFE(&s_push_mux);
    s_frames++;
    if (r->result != RF_TRACE_TX_ACK) s_fails++;
    rf_trace_ring_push(&s_ring, &rec);
    portEXIT_CRITICAL_SAFE(&s_push_mux);
    notify(NOTE_DATA);
}

static void periodic(uint8_t state)   /* s_out held: drops_take is the consumer's */
{
    rf_trace_rec_t rec = { .kind = RF_TRACE_KIND_PERIODIC };
    rf_trace_periodic_t *p = &rec.u.periodic;
    int64_t t = esp_timer_get_time();
    p->t_us = (uint32_t)t;
    p->uptime_s = (uint32_t)(t / 1000000);
    p->source = EMITTER;
    p->state = state;
#if CONFIG_KASE_BATT_SENSE
    p->batt_dV = batt_sense_dv();
#endif
    p->channel = radio_cible()->channel;   /* diagnostic read, like the HB */
    uint32_t d = rf_trace_ring_drops_take(&s_ring);
    p->drops = d > 0xFFFFu ? 0xFFFFu : (uint16_t)d;
    portENTER_CRITICAL(&s_push_mux);
    p->frames = s_frames; p->fails = s_fails;
    s_frames = s_fails = 0;
    portEXIT_CRITICAL(&s_push_mux);
    push(&rec);
}

/* ── UART0 out ── */

/* Writes b[0..n) WHOLE if the TX FIFO has room for it now; false otherwise.
 * The console lock is held for the copy only. */
static bool uart0_try_write(const uint8_t *b, size_t n)
{
    uart_dev_t *u = UART_LL_GET_HW(0);
    flockfile(stdout);                    /* no console write() inside the frame */
    bool ok = uart_ll_get_txfifo_len(u) >= n;
    if (ok) uart_ll_write_txfifo(u, b, (uint32_t)n);
    funlockfile(stdout);
    return ok;
}

typedef enum { OUT_EMPTY, OUT_FULL, OUT_SENT } out_t;

/* One frame, as many records as the TX FIFO takes at this instant.
 * `blocking`: false in the sleep hook — gives up instead of waiting for the
 * console lock held by a task in the middle of a print. */
static out_t drain_one(bool blocking)   /* s_out held */
{
    uart_dev_t *u = UART_LL_GET_HW(0);
    if (blocking) flockfile(stdout);
    else if (ftrylockfile(stdout) != 0) return OUT_FULL;
    out_t res;
    uint8_t kind = rf_trace_ring_peek_kind(&s_ring);
    size_t k = kind ? rf_trace_batch_fit(uart_ll_get_txfifo_len(u), kind) : 0;
    if (kind == 0) {
        res = OUT_EMPTY;
    } else if (k == 0) {
        res = OUT_FULL;                   /* the records wait in the ring, no lock held */
    } else {
        size_t n = rf_trace_ring_pop_batch(&s_ring, s_batch, k);
        size_t len = rf_trace_frame_encode(s_frame, sizeof s_frame, s_batch[0].kind, s_batch, n);
        if (len) uart_ll_write_txfifo(u, s_frame, (uint32_t)len);
        res = OUT_SENT;
    }
    funlockfile(stdout);
    return res;
}

/* True when records are left behind a full TX FIFO (retry a tick later). */
static bool drain(bool blocking)   /* s_out held */
{
    for (unsigned i = 0; i < RING_CAP; i++) {
        out_t r = drain_one(blocking);
        if (r == OUT_EMPTY) return false;
        if (r == OUT_FULL)  return true;
    }
    return rf_trace_ring_count(&s_ring) != 0;
}

/* The KR ack: its own 7 bytes, no ring, no s_out. The FIFO may be full of
 * trace frames: retry a few ticks with no lock held, then give up (the host
 * repeats its request until the ack comes). */
static void reply(uint8_t cmd, uint8_t status)
{
    uint8_t f[8];
    size_t n = rf_trace_kr_encode(f, sizeof f, cmd, status, NULL, 0);
    for (int i = 0; n && !uart0_try_write(f, n); i++) {
        if (i + 1 >= REPLY_TRIES) return;
        vTaskDelay(1);
    }
}

/* ── UART0 in ── */

static void uart0_rx_isr(void *arg)
{
    (void)arg;
    uart_dev_t *u = UART_LL_GET_HW(0);
    uint32_t st = uart_ll_get_intsts_mask(u);
    uint32_t n = uart_ll_get_rxfifo_len(u);
    while (n--) {
        uint8_t b;
        uart_ll_read_rxfifo(u, &b, 1);
        uint32_t h = s_rx_head;
        if (h - s_rx_tail < RXB_CAP) { s_rxb[h % RXB_CAP] = b; s_rx_head = h + 1; }   /* full: the host repeats */
    }
    uart_ll_clr_intsts_mask(u, st & (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT));
    BaseType_t hp = pdFALSE;
    if (s_task) xTaskNotifyFromISR(s_task, NOTE_RX, eSetBits, &hp);
    portYIELD_FROM_ISR(hp);
}

static void rx_init(void)
{
    uart_dev_t *u = UART_LL_GET_HW(0);
    /* Already the pad's reset state (TRM Table 6.12-1 p. 494): a floating RX0
     * with the ESP-Prog unplugged would raise noise interrupts. Pull bit
     * only — the IO_MUX function stays U0RXD. */
    gpio_pullup_en((gpio_num_t)U0RXD_GPIO_NUM);
    uart_ll_disable_intr_mask(u, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    uart_ll_rxfifo_rst(u);                       /* whatever the boot left there */
    uart_ll_set_rxfifo_full_thr(u, 32);
    uart_ll_set_rx_tout(u, 30);                  /* 3 symbols of silence: a request has ended */
    uart_ll_clr_intsts_mask(u, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
    intr_handle_t h;
    if (esp_intr_alloc(ETS_UART0_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, uart0_rx_isr, NULL, &h) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 RX interrupt unavailable: the trace cannot be started");
        return;
    }
    uart_ll_ena_intr_mask(u, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT);
}

static void command(const rf_trace_ks_rx_t *rx)   /* drain task */
{
    rf_trace_ctl_cmd_t c = { 0 };
    uint8_t status = KS_STATUS_OK;
    if (rx->cmd != KS_CMD_RF_TRACE)                                          status = KS_STATUS_ERR_UNKNOWN;
    else if (rf_trace_ctl_parse(rx->payload, rx->len, &c) != RF_TRACE_CTL_OK) status = KS_STATUS_ERR_INVALID;
    else {
        portENTER_CRITICAL(&s_ctl_mux);
        if (c.start) rf_trace_sess_start(&s_ctl, now_ms(), c.duration_s);
        else                           rf_trace_sess_stop(&s_ctl);
        g_rf_trace_session = s_ctl.session;
        portEXIT_CRITICAL(&s_ctl_mux);
    }
    reply(rx->cmd, status);
    if (status == KS_STATUS_OK) {
        if (c.start)
            ESP_LOGW(TAG, "ON for %u s (session %lu)", (unsigned)c.duration_s, (unsigned long)g_rf_trace_session);
        else
            ESP_LOGW(TAG, "OFF (stop)");
    }
}

/* True when a complete request was handled. */
static bool rx_bytes(void)
{
    bool handled = false;
    while (s_rx_tail != s_rx_head) {
        uint8_t b = s_rxb[s_rx_tail % RXB_CAP];
        s_rx_tail = s_rx_tail + 1;
        if (rf_trace_ks_rx_feed(&s_ks, b)) { command(&s_ks); handled = true; }
    }
    return handled;
}

/* ── The drain task ── */

static void rf_trace_task(void *arg)
{
    (void)arg;
    uint32_t bits = 0;
    for (;;) {
        bool handled = rx_bytes();
        if (bits == NOTE_RX && !handled && g_rf_trace_session == 0) {
            /* Bytes only (noise on RX0, a request still incomplete) and
             * tracing off: nothing to do — straight back to sleep. */
            bits = 0;
            xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
            continue;
        }
        uint32_t wait_ms;
        portENTER_CRITICAL(&s_ctl_mux);
        rf_trace_act_t act = rf_trace_sess_tick(&s_ctl, now_ms(), &wait_ms);
        bool woke = s_woke_pending;
        s_woke_pending = false;
        g_rf_trace_session = s_ctl.session;
        portEXIT_CRITICAL(&s_ctl_mux);

        xSemaphoreTake(s_out, portMAX_DELAY);    /* held for µs: never across a wait */
        if (woke)                         periodic(RF_TRACE_ST_WOKE);
        if (act == RF_TRACE_ACT_PERIODIC) periodic(RF_TRACE_ST_AWAKE);
        bool backlog = drain(true);
        xSemaphoreGive(s_out);
        if (act == RF_TRACE_ACT_EXPIRED) ESP_LOGW(TAG, "OFF (duration over)");

        /* Off: forever. On: until the next periodic record or the deadline
         * (>= 1 s apart, CADENCE_REPOS_OK above) — or a push / a byte. A
         * backlog behind a full TX FIFO: one tick (the FIFO empties in
         * ~11 ms) — only while tracing, and only while records wait. */
        TickType_t t = (wait_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms) + 1;
        if (backlog && t > 1) t = 1;
        bits = 0;                                    /* 0 after a timeout: the full pass */
        xTaskNotifyWait(0, UINT32_MAX, &bits, t);
    }
}

/* ── Sleep: one record on the way in, one on the way out, none between ── */

#if CONFIG_KASE_VEILLE
static void hook_sleep(void)   /* veille task, after the radio's power-down */
{
    portENTER_CRITICAL(&s_ctl_mux);
    bool emit = rf_trace_sess_sleep(&s_ctl);
    portEXIT_CRITICAL(&s_ctl_mux);
    /* Never wait for a lock here: the drain task holds s_out for µs only, a
     * printing task may hold the console for ms — then the SLEEPING record is
     * skipped (the WOKE record after the sleep still says it slept). */
    if (!emit || xSemaphoreTake(s_out, 0) != pdTRUE) return;
    periodic(RF_TRACE_ST_SLEEPING);
    (void)drain(false);                             /* what fits the TX FIFO now; the rest waits for the wake */
    xSemaphoreGive(s_out);
    /* The light sleep SUSPENDS UART0 (xoff) with its FIFO — the SLEEPING
     * record would only leave at the wake, and the host would not know the
     * half sleeps. Let the FIFO empty first: bounded by what it holds (≤ 128
     * chars, ~11 ms at 115200; ~3 ms for the SLEEPING frame alone), busy,
     * tracing on only. */
    uart_dev_t *u = UART_LL_GET_HW(0);
    uint32_t chars = UART_LL_FIFO_DEF_LEN - uart_ll_get_txfifo_len(u) + 1u;
    int64_t until = esp_timer_get_time() + (int64_t)chars * UART0_US_PER_CHAR;
    while (!uart_ll_is_tx_idle(u) && esp_timer_get_time() < until) esp_rom_delay_us(50);
}

static void hook_wake(void)    /* veille task; touches neither the SPI bus nor the UART */
{
    portENTER_CRITICAL(&s_ctl_mux);
    if (rf_trace_sess_wake(&s_ctl, now_ms())) s_woke_pending = true;
    g_rf_trace_session = s_ctl.session;            /* a duration over during the sleep: off now */
    portEXIT_CRITICAL(&s_ctl_mux);
    notify(NOTE_WAKE);
}
#endif

void rf_trace_uart_init(void)
{
    if (s_task) return;
    rf_trace_ring_init(&s_ring, s_slots, RING_CAP);
    rf_trace_sess_init(&s_ctl);
    rf_trace_ks_rx_reset(&s_ks);
    g_rf_trace_session = 0;                         /* off at every boot */
    s_out = xSemaphoreCreateMutex();
    if (!s_out || xTaskCreate(rf_trace_task, "rftrace", 3072, NULL, 1, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no drain task: rf-scope disabled");
        return;
    }
    rx_init();
#if CONFIG_KASE_VEILLE
    static const veille_hook_t hook = { "rftrace", hook_sleep, hook_wake, NULL };
    veille_hook_enregistrer(&hook);
#endif
}
