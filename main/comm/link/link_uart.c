/* Wired TRRS link transport — brick B2.
 *
 * Everything that is decided lives elsewhere: the format in link_frame.h, the
 * 5 V handshake in link_handshake.h — pure logic, host-tested. Here, only what
 * touches the hardware: UART1, the load switch pin, and USB presence. This
 * file translates events into link_hs_step() and actions into
 * gpio_set_level().
 *
 * ── The cable is straight ────────────────────────────────────────────────────
 * TX arrives on TX. ONE half therefore swaps TXD/RXD via the GPIO matrix
 * (BOARD_LINK_SWAP_TX_RX, set to 1 on the left). Driving both TX without this
 * swap would put two outputs in opposition on the same wire.
 *
 * ── 5 V safety ───────────────────────────────────────────────────────────────
 * LINK_5V_EN has a 100 k pull-down: dead by default, and this file drives it
 * LOW before anything else. It only goes high on an action from the state
 * machine, which only emits it after a verified exchange — see the invariant
 * at the top of link_handshake.h.
 *
 * ── A charging half stays awake ──────────────────────────────────────────────
 * The receiver must answer probes for the 5 V to pass. In light sleep its
 * UART is mute: the peer times out (LINK_HS_PEER_TIMEOUT_MS) and reopens its
 * switch. link_uart_active() therefore acts as a sleep lock, via the
 * `bloque` parameter of veille_pas(). A plugged-in device does not sleep. */
#include "link_uart.h"
#include "cadence.h"    /* LINK_TICK_MS / LINK_REPOS_MS */
#include "link_frame.h"
#include "link_handshake.h"
#include "board.h"
#include "usb_presence.h"     /* vbus_debounce_step, usb_presence_cable */
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"       /* low battery: no 5 V for the other half */
#endif
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"        /* esp_sleep_enable_uart_wakeup: the probe wakes a sleeping half */
#include "tinyusb.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* LINK veto: a half charging the other does not sleep */
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "link";

#define LINK_BAUD        115200
#define LINK_RX_BUF      64

static link_hs_t        s_hs;
static vbus_debounce_t  s_usb_db;
static bool             s_usb_prev;
static uint8_t          s_seq;
static uint8_t          s_rx[LINK_RX_BUF];
static uint16_t         s_rx_len;
static volatile bool    s_active;
static QueueHandle_t    s_uart_q;      /* UART driver events: wake on reception */

/* Bench counters: we can't see the wire, we have to count it. */
static uint32_t s_probes_tx, s_acks_tx, s_probes_rx, s_acks_rx, s_skips;

static void set_5v(bool on)
{
    gpio_set_level(BOARD_LINK_5V_EN, on ? 1 : 0);
    if (s_active == on) return;   /* keepalive re-closes an already-closed switch every 200 ms */
    s_active = on;
#if CONFIG_KASE_VEILLE
    veille_veto(VEILLE_VETO_LIEN, on);   /* asleep, it would stop answering and the peer would reopen its 5 V */
#endif
    ESP_LOGW(TAG, "5 V %s — GPIO%d readback = %d", on ? "CLOSED" : "open",
             BOARD_LINK_5V_EN, gpio_get_level(BOARD_LINK_5V_EN));
}

static void send_ctrl(uint8_t type)
{
    uint8_t buf[LINK_FRAME_MIN];
    uint16_t n = link_encode_ctrl(buf, type, s_seq++);
    uart_write_bytes(BOARD_LINK_UART_NUM, buf, n);
    if (type == LINK_TYPE_PROBE) s_probes_tx++; else s_acks_tx++;
}

static void apply(link_hs_action_t a)
{
    switch (a) {
    case LINK_HS_ACT_SEND_PROBE:        send_ctrl(LINK_TYPE_PROBE); break;
    case LINK_HS_ACT_ENABLE_5V:         set_5v(true); break;
    case LINK_HS_ACT_DISABLE_5V:        set_5v(false); break;
    case LINK_HS_ACT_ACK_AND_ENABLE_5V: send_ctrl(LINK_TYPE_ACK); set_5v(true); break;
    case LINK_HS_ACT_NONE:              break;
    }
}

/* Drain the UART and decode, with resynchronisation: the decoder says how
 * much to consume, we advance by that much, and a SKIP always consumes at
 * least one byte — no infinite loop on noise. */
static void drain_uart(uint32_t now)
{
    int n = uart_read_bytes(BOARD_LINK_UART_NUM, s_rx + s_rx_len,
                            LINK_RX_BUF - s_rx_len, 0);
    if (n > 0) s_rx_len += (uint16_t)n;

    uint16_t off = 0;
    for (;;) {
        link_frame_t f; uint16_t used = 0;
        link_decode_status_t st = link_decode(s_rx + off, s_rx_len - off, &f, &used);
        if (st == LINK_DECODE_NEED_MORE) break;
        off += used;
        if (st == LINK_DECODE_SKIP) { s_skips++; continue; }
        switch (f.type) {
        case LINK_TYPE_PROBE: s_probes_rx++; apply(link_hs_step(&s_hs, LINK_HS_EV_PROBED,   now)); break;
        case LINK_TYPE_ACK:   s_acks_rx++;   apply(link_hs_step(&s_hs, LINK_HS_EV_PEER_ACK, now)); break;
        default:                             apply(link_hs_step(&s_hs, LINK_HS_EV_PEER_FRAME, now)); break;
        }
    }
    if (off) { memmove(s_rx, s_rx + off, s_rx_len - off); s_rx_len -= off; }
    if (s_rx_len == LINK_RX_BUF) s_rx_len = 0;   /* buffer full without a frame: noise, start over */
}

static void link_task(void *arg)
{
    (void)arg;
    uint32_t dernier_bilan = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        /* USB presence, on EDGE: the state machine wants events, not a
         * state. tud_ready(), not tud_mounted() — on the ESP32-S3 mounted
         * stays true after a hot unplug. Same lesson as sleep. */
        /* Same rule as routing and sleep (usb_presence_brut): VBUS bridge if
         * populated — a wall charger does not enumerate and must still make
         * this half the source of the 5 V —, otherwise tud_ready(), bench
         * forcing included. */
        bool usb = vbus_debounce_step(&s_usb_db, usb_presence_cable(), now, 50);
#if CONFIG_KASE_BATT_SENSE
        /* LOW battery (< 3.5 V): this half no longer declares itself source
         * of the 5 V — we don't charge the other half from a flat cell. On
         * USB with a VBUS bridge the cable's 5 V is what feeds it; without a
         * bridge we can't know, so we stay cautious. */
        if (usb && batt_sense_niveau() != 0) usb = false;
#endif
        if (usb != s_usb_prev) {
            s_usb_prev = usb;
            apply(link_hs_step(&s_hs, usb ? LINK_HS_EV_USB_PRESENT : LINK_HS_EV_USB_GONE, now));
            ESP_LOGI(TAG, "USB %s", usb ? "present" : "absent");
        }

        drain_uart(now);
        apply(link_hs_step(&s_hs, LINK_HS_EV_TICK, now));

        if ((uint32_t)(now - dernier_bilan) >= 5000) {
            dernier_bilan = now;
            ESP_LOGI(TAG, "state=%d 5V=%d GPIO%d=%d | probes tx %u rx %u | acks tx %u rx %u | noise %u",
                     (int)s_hs.state, (int)s_hs.en_5v,
                     BOARD_LINK_5V_EN, gpio_get_level(BOARD_LINK_5V_EN),
                     (unsigned)s_probes_tx, (unsigned)s_probes_rx,
                     (unsigned)s_acks_tx, (unsigned)s_acks_rx, (unsigned)s_skips);
        }
        /* At rest (5 V dead, no USB): blocked on the UART event queue — a
         * byte from the peer wakes it immediately (its probe arrives every
         * 300 ms during the handshake); USB, a human event, is polled at
         * LINK_REPOS_MS. At 10 ms this task pulled the processor out of
         * idle 100 times a second for nothing; at 100 ms poll still ten
         * times. During handshake or with the link up: tick of LINK_TICK_MS
         * (200 ms keepalive, 200-500 ms timeouts). */
        bool repos = (s_hs.state == LINK_HS_IDLE) && !usb;
        uart_event_t ev;
        if (s_uart_q && xQueueReceive(s_uart_q, &ev, pdMS_TO_TICKS(repos ? LINK_REPOS_MS : LINK_TICK_MS)) == pdTRUE) {
            /* Overflow (floating TX of a sleeping peer = flood of fake bytes):
             * start clean rather than decode noise for seconds. */
            if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                uart_flush_input(BOARD_LINK_UART_NUM);
                xQueueReset(s_uart_q);
                s_rx_len = 0;
            }
        } else if (!s_uart_q) {
            vTaskDelay(pdMS_TO_TICKS(repos ? LINK_REPOS_MS : LINK_TICK_MS));
        }
    }
}

bool link_uart_active(void) { return s_active; }

void link_uart_start(void)
{
    /* 1. The switch, LOW, before anything else: it's the only pin that can
     *    do harm, and it must not depend on anything that follows. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LINK_5V_EN,
        .mode = GPIO_MODE_INPUT_OUTPUT,   /* readback possible: we want to SEE what we command */
    };
    gpio_config(&io);
    gpio_set_level(BOARD_LINK_5V_EN, 0);

    /* 2. The UART, with the swap on the half that declares it. */
    uart_config_t uc = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        /* XTAL, not APB: with DFS (CONFIG_PM_ENABLE) the APB drops to 40 MHz at
         * rest and a UART clocked off it loses its baud between two locks.
         * The XTAL never moves. */
        .source_clk = UART_SCLK_XTAL,
    };
    ESP_ERROR_CHECK(uart_driver_install(BOARD_LINK_UART_NUM, 256, 0, 8, &s_uart_q, 0));
    ESP_ERROR_CHECK(uart_param_config(BOARD_LINK_UART_NUM, &uc));
#if BOARD_LINK_SWAP_TX_RX
    const int link_rx_pin = BOARD_LINK_TX;   /* swap: the real RX is on TX */
    ESP_ERROR_CHECK(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_RX, BOARD_LINK_TX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d (SWAP, straight cable)",
             BOARD_LINK_UART_NUM, BOARD_LINK_RX, BOARD_LINK_TX);
#else
    const int link_rx_pin = BOARD_LINK_RX;
    ESP_ERROR_CHECK(uart_set_pin(BOARD_LINK_UART_NUM, BOARD_LINK_TX, BOARD_LINK_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d", BOARD_LINK_UART_NUM, BOARD_LINK_TX, BOARD_LINK_RX);
#endif

    /* Pull-up on RX. When the other half sleeps, its TX floats: at rest a
     * UART is in the HIGH state, a floating line drifts low and gets read as
     * a flood of fake bytes (91,682 "noise" counted on a capture from
     * 2026-09-12). The internal pull-up holds it high — line at rest, no
     * spurious decoding. Also a lead against TRRS cable coupling into the
     * matrix lines, suspected in the phantom wakeups. */
    gpio_set_pull_mode(link_rx_pin, GPIO_PULLUP_ONLY);

#if CONFIG_KASE_VEILLE
    /* ── A sleeping half must HEAR the probe ─────────────────────────────
     * Without this, the handshake only worked when both halves happened to
     * be awake: light sleep has no wake source but EXT1 (the matrix rows)
     * and the deep-sleep timer, so the probes of a half that has just been
     * plugged in arrived on a clock-gated UART. You had to type on BOTH
     * halves for the 5 V to pass — "it doesn't always work" (Mae, 2026-09-23).
     *
     * UART1 is a light-sleep wake source on the S3 (TRM v1.8, table 10.4-3
     * p. 580, WAKEUP_ENA 0x80, note 5: the wake fires when the number of RX
     * pulses exceeds the threshold register). Three edges is the documented
     * minimum; the RX line rests high (pull-up above), so only a real frame
     * produces them.
     *
     * The frame that wakes us is LOST — the chip only starts receiving after
     * the wake (ESP-IDF, Sleep Modes, § UART Wakeup). That costs nothing
     * here: whoever has current to give re-probes every
     * LINK_HS_REPROBE_INTERVAL_MS (300 ms), so the NEXT probe is the one
     * that gets decoded and answered, and that ACK is also the UART traffic
     * the same doc asks for to clear the internal wake indication.
     *
     * ⚠ What this does NOT fix: plugging the USB cable into a half that is
     * already asleep. The USB is not a wake source at all on the S3 (same
     * table) — it would take the VBUS bridge on GPIO33, which is not
     * populated. On a sleeping half, the cable is noticed at the first
     * keystroke. */
    ESP_ERROR_CHECK(uart_set_wakeup_threshold(BOARD_LINK_UART_NUM, 3));
    ESP_ERROR_CHECK(esp_sleep_enable_uart_wakeup(BOARD_LINK_UART_NUM));
    ESP_LOGI(TAG, "UART%d wakes the half from light sleep (3 RX edges)", BOARD_LINK_UART_NUM);
#endif

    link_hs_init(&s_hs);
    memset(&s_usb_db, 0, sizeof(s_usb_db));
    s_usb_prev = false;
    xTaskCreate(link_task, "link", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "wired link ready, 5 V open");
}
