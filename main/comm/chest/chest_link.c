/* Chest link, S3 master — protocol v3. Spec:
 * docs/superpowers/specs/2026-09-29-chest-link-v3-design.md (v2 mode rules:
 * docs/superpowers/specs/2026-09-29-chest-link-v2-design.md §3); contract:
 * Niphar_chest docs/LINK_CONTRACT.md (46499d6) §1, §5, §6, §13.
 * Presence = a USB host (the chest is powered by the left half's USB only).
 * Absent: no SPI device, GPIO3 an input (R48 pulls CS to the chest's rail —
 * driving it into a dead rail costs ~0.33 mA), IRQ off, the task blocked.
 * Present: device added, IRQ armed, a round every CHEST_POLL_MS, at once on
 * an IRQ edge or on a chest key (chest_gate_set_notify), every transaction
 * under the radio owner's bus lock ("one chip, one owner").
 *
 * This file is glue: every decision is a tested pure function — the block
 * (chest_proto), the confirm rule and the mode (chest_proto), the DMA round
 * (chest_round), the browser (chest_oath), the screen (chest_view).
 *
 * WIRE SHAPES. The device is configured like Espressif's own spi_slave_hd
 * master (examples/peripherals/spi_slave_hd/segment_mode/seg_master, ESP-IDF
 * v5.5.2: command 8, address 8, dummy 8 bits, SPI_DEVICE_HALFDUPLEX), and the
 * transactions mirror that example's helpers
 * (components/driver/test_apps/components/esp_serial_slave_link/essl_spi.c):
 *   RDBUF  0x02  addr = offset, 8 dummy, rxlength   (essl_spi_rdbuf)
 *   WRBUF  0x01  addr = offset, 8 dummy, length     (essl_spi_wrbuf)
 *   WRDMA  0x03  addr 0, 8 dummy, length 64 bits    (essl_spi_wrdma_seg)
 *   WR_END 0x07  addr 0, 8 dummy, no data phase     (essl_spi_wrdma_done: a plain
 *                spi_transaction_t with only .cmd — the address and dummy
 *                phases still go out per the device config)
 *   RDDMA  0x04  addr 0, 8 dummy, rxlength = dma_len * 8   (essl_spi_rddma_seg)
 *   INT0   0x08  addr 0, 8 dummy, no data phase     (essl_spi_rddma_done)
 * essl uses spi_transaction_ext_t + SPI_TRANS_VARIABLE_DUMMY with
 * dummy_bits = spi_ll_get_slave_hd_dummy_bits() = 8 in one-line mode — the
 * same 8 bits as the device default, so a plain spi_transaction_t puts the
 * identical bits on the wire. The one-line command byte equals the base
 * command (spi_ll_get_slave_hd_command: cmd_mod 0 for 1-bit data). A request
 * sequence (WRDMA, WR_END, doorbell) and a response sequence (RDDMA, INT0)
 * each run under ONE rf_bus_lock, so the radio never interleaves. */
#include "chest_link.h"
#include "chest_proto.h"
#include "chest_dma.h"
#include "chest_oath.h"
#include "chest_round.h"
#include "chest_gate.h"
#include "veille_task.h"
#include "rf_bus.h"
#include "hid_report.h"     /* hid_report_type_usages, keyboard_get_usb_bl_state */
#include "usb_presence.h"   /* kbd_active_route: typing only on the USB route */
#include "board.h"
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "chest";
#define CHEST_SPI_HZ   1000000
#define CHEST_POLL_MS  250u
#define CHEST_DEV_RETRY_MS 1000u   /* spi_bus_add_device failed: retry, don't block forever */
/* 3072 until v3. chest_list_decode() holds a chest_list_t (1156 B) on its
 * own stack while it validates a page (its `out` is left untouched on
 * failure); the caller's chest_list_t is static (consume_segment).
 * -fstack-usage on the left's build (xtensa-esp32s3 gcc, 2026-09-29):
 * chest_task 144 + round_ok 64 + consume_segment 48 + chest_list_decode
 * 1232 + its leaves ~64 = ~1.55 KB on the decode path, a frame the v2 task
 * never had; +1536 over v2 keeps the old margin for the ESP_LOG/vprintf
 * path on top. The bench diagnostic (CONFIG_KASE_CHEST_DIAG) logs the
 * high-water mark after the first LIST. */
#define CHEST_TASK_STACK 4608

#if CONFIG_KASE_CHEST_DIAG
#define DIAG(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define DIAG(...) do { } while (0)
#endif

static TaskHandle_t         s_task;
static spi_device_handle_t  s_dev;
static volatile bool        s_want;          /* presence asked by the sleep task */
/* Screen view snapshot: written by chest_task (below) after every round,
 * read by chest_link_view() from the display task. A whole-struct copy under
 * a critical section: a torn read across fields would show one frame's stale
 * label next to another's op. */
static portMUX_TYPE         s_view_mux = portMUX_INITIALIZER_UNLOCKED;
static chest_view_t         s_view;
static chest_oath_t         s_oath;          /* ~1.2 KB: static, never on a stack (chest_oath.h) */
static chest_round_t        s_round;
static chest_confirm_t      s_confirm;
static uint8_t              s_mode_wanted;   /* CHEST_MODE_*; none at every presence session */
static chest_mode_track_t   s_mode_track;
static uint8_t              s_mode_state = CHEST_MODE_ARRIVED;
static chest_block_t        s_blk = CHEST_BLOCK_ABSENT;   /* last parsed block, for the view */
static chest_status_t       s_st;                         /* valid when s_blk == OK */
static bool                 s_nav_pending;   /* a navigation key not yet seen by an OK round */
static bool                 s_code_pending;  /* a K_OATH_CODE press held over a bus-busy read */
static bool                 s_veto_code;
static uint8_t              s_badver_logged;  /* once per presence session, reset in go_absent */
static uint8_t              s_corrupt_logged; /* once per presence session, reset in go_absent */
static DMA_ATTR uint8_t     s_rx[CHEST_REG_SIZE];
static DMA_ATTR uint8_t     s_tx[4];
static DMA_ATTR uint8_t     s_req[CHEST_REQ_SIZE];
static DMA_ATTR uint8_t     s_dma[CHEST_DMA_MAX];
#if CONFIG_KASE_CHEST_DIAG
static bool                 s_first_list_logged;
#endif

static void cs_release(void)
{
    const gpio_config_t c = { .pin_bit_mask = 1ULL << BOARD_CHEST_CS, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&c);
}

static void IRAM_ATTR irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* chest_gate's wake-up hook: runs in the keyboard task after a chest key
 * stored its request — the round serves it now, not at the next poll. */
static void wake_from_key(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

static bool dev_add(void)
{
    const spi_device_interface_config_t d = {
        .command_bits = 8, .address_bits = 8, .dummy_bits = 8,   /* spi_slave_hd: cmd, addr, dummy */
        .mode = 0, .clock_speed_hz = CHEST_SPI_HZ,
        .spics_io_num = BOARD_CHEST_CS, .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    return spi_bus_add_device(rf_bus_host(), &d, &s_dev) == ESP_OK;
}

/* n transactions under ONE bus lock; false on a busy bus or any error. */
static bool xfer_seq(spi_transaction_t *t, size_t n)
{
    if (!s_dev || !rf_bus_lock(20)) return false;          /* radio busy: next round */
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) ok = spi_device_polling_transmit(s_dev, &t[i]) == ESP_OK;
    rf_bus_unlock();
    return ok;
}

static bool read_block(void)
{
    spi_transaction_t t = { .cmd = CHEST_CMD_RDBUF, .addr = 0,
                            .rxlength = CHEST_REG_SIZE * 8, .rx_buffer = s_rx };
    return xfer_seq(&t, 1);
}

/* Contract §5 step 3: {0x5A, instance} in ONE write at 0x38 — the instance
 * of the block that showed the op the owner confirmed (s_confirm). */
static void write_confirm(uint8_t instance)
{
    s_tx[0] = CHEST_CONFIRM_MAGIC;
    s_tx[1] = instance;
    spi_transaction_t t = { .cmd = CHEST_CMD_WRBUF, .addr = CHEST_REG_USER_CONFIRM,
                            .length = 16, .tx_buffer = s_tx };
    if (!xfer_seq(&t, 1)) ESP_LOGW(TAG, "confirmation not written (bus busy) — retried by the rule");
}

/* Contract §6.4: one byte at 0x3A, never in the same write as 0x38. */
static void write_mode(uint8_t mode)
{
    s_tx[0] = mode;
    spi_transaction_t t = { .cmd = CHEST_CMD_WRBUF, .addr = CHEST_REG_MODE_REQ,
                            .length = 8, .tx_buffer = s_tx };
    if (xfer_seq(&t, 1)) ESP_LOGI(TAG, "USB mode requested: 0x%02X", mode);
}

/* Contract §13: WRDMA (8 bytes), WR_END, THEN the doorbell at 0x3C. */
static bool send_request(uint8_t cmd, uint8_t arg, uint8_t bell)
{
    chest_req_pack(s_req, cmd, arg);
    s_tx[0] = bell;
    spi_transaction_t t[3] = {
        { .cmd = CHEST_CMD_WRDMA, .addr = 0, .length = CHEST_REQ_SIZE * 8, .tx_buffer = s_req },
        { .cmd = CHEST_CMD_WR_END },
        { .cmd = CHEST_CMD_WRBUF, .addr = CHEST_REG_REQ_SEQ, .length = 8, .tx_buffer = s_tx },
    };
    /* Partial failure (review m4): if WRDMA and WR_END went out but the
     * doorbell did not, the chest holds a completed, unannounced receive.
     * We report !ok, so the doorbell does not advance (chest_round_sent)
     * and the next attempt re-sends WRDMA + WR_END + the SAME doorbell
     * value. That second WRDMA finds no receive armed on the chest (its one
     * receive is the completed one) and is lost; the doorbell then makes the
     * chest serve the FIRST segment — the same request if it is a retry,
     * the old one otherwise (a CODE re-planned for another account after a
     * navigation, for instance). Harmless either way: a wrong LIST is just a
     * page, and a wrong CODE arms a prompt that names the account the chest
     * really targeted, which the owner does not confirm. */
    bool ok = xfer_seq(t, 3);
    DIAG("DMA WRDMA %s arg=%u bell=%u: %02X %02X %02X %02X %02X %02X %02X %02X -> %s",
         cmd == CHEST_REQ_LIST ? "LIST" : "CODE", arg, bell, s_req[0], s_req[1], s_req[2],
         s_req[3], s_req[4], s_req[5], s_req[6], s_req[7], ok ? "ok" : "FAILED");
    return ok;
}

/* Contract §13 step 5: RDDMA exactly the published length (the chest pads
 * its queue to a cache line; 0x12-0x13 is the TRUE length), then INT0. */
static bool read_segment(uint16_t len)
{
    spi_transaction_t t[2] = {
        { .cmd = CHEST_CMD_RDDMA, .addr = 0, .rxlength = (size_t)len * 8, .rx_buffer = s_dma },
        { .cmd = CHEST_CMD_INT0 },
    };
    return xfer_seq(t, 2);
}

static void consume_segment(const chest_status_t *st, uint32_t now)
{
    if (st->dma_kind == CHEST_DMA_LIST) {
        static chest_list_t l;   /* 1156 B: off the task stack */
        bool ok = chest_list_decode(s_dma, st->dma_len, &l);
#if CONFIG_KASE_CHEST_DIAG
        if (!s_first_list_logged) {   /* the chest session wants the raw first LIST, good or bad */
            ESP_LOGI(TAG, "DMA RDDMA LIST seq=%u len=%u, raw:", st->dma_seq, st->dma_len);
            ESP_LOG_BUFFER_HEX(TAG, s_dma, st->dma_len);
        } else {
            DIAG("DMA RDDMA LIST seq=%u len=%u: %02X %02X %02X %02X ...", st->dma_seq, st->dma_len,
                 s_dma[0], s_dma[1], s_dma[2], s_dma[3]);
        }
#endif
        if (ok) {
            DIAG("LIST decoded: total=%u count=%u first=%u more=%u%s%s", l.total, l.count, l.first,
                 l.more, l.count ? " e0=" : "", l.count ? l.e[0].name : "");
        } else {
            ESP_LOGW(TAG, "LIST segment refused (%u bytes): length/CRC/entries", st->dma_len);
        }
#if CONFIG_KASE_CHEST_DIAG
        if (!s_first_list_logged) {
            ESP_LOGI(TAG, "chest task stack high-water mark after the first LIST: %u bytes free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            s_first_list_logged = true;
        }
#endif
        if (ok && s_round.in_oath) chest_oath_on_list(&s_oath, &l);
    } else {
        chest_code_t c;
        bool ok = chest_code_decode(s_dma, st->dma_len, &c);
        /* The digits never reach the log, diagnostic or not: a code belongs
         * on the owner's screen only (contract §13, IHM engagements). */
        DIAG("DMA RDDMA CODE seq=%u len=%u: index=%02X digits=%02X code=******** secs=%02X",
             st->dma_seq, st->dma_len, s_dma[0], s_dma[1], st->dma_len > 10 ? s_dma[10] : 0);
        if (ok) {
            DIAG("CODE decoded: index=%u digits=%u seconds=%u", c.index, c.digits, c.seconds);
            if (s_round.in_oath) chest_oath_on_code(&s_oath, &c, now);
        } else {
            ESP_LOGW(TAG, "CODE segment refused (%u bytes): length/CRC/digits", st->dma_len);
        }
        memset(&c, 0, sizeof c);
    }
    memset(s_dma, 0, st->dma_len);   /* a code does not linger in the DMA buffer */
}

static void round_ok(const chest_status_t *st, uint32_t pressed, bool mode_next,
                     bool nav, bool code_key, uint32_t now)
{
    chest_gate_publish(st->pending_op, st->instance);

    /* Mode (v2 spec §3): K_CHEST_NEXT cycles the WANTED mode, only on a
     * READY chest; 0x3A is rewritten whenever its read-back differs
     * (self-heal after a chest reboot or the confirm reclaim's RMW). The
     * decision is taken EVERY round from 0x3A as re-read in THIS round's
     * RDBUF block (s_rx), never from a local "already written" latch: the
     * chest's reclaim of 0x38 rewrites the whole first master word
     * 0x38-0x3B, so a write that landed in those cycles is silently gone
     * and only the read-back shows it (chest session, 2026-09-29). */
    if (mode_next && (st->state & CHEST_STATE_READY)) s_mode_wanted = chest_mode_next(s_mode_wanted);
    if (chest_mode_needs_write(s_rx, s_mode_wanted)) write_mode(s_mode_wanted);
    s_mode_state = (uint8_t)chest_mode_track(&s_mode_track, st->active_mode, s_mode_wanted);

    /* DMA channel (chest_round.h). */
    chest_round_plan_t p;
    chest_round_plan(&s_round, st, s_rx[CHEST_REG_REQ_SEQ], &s_oath, nav, code_key, now, &p);
    if (p.leave_oath) chest_oath_reset(&s_oath);          /* also drops any request */
    else if (p.cancel_code) chest_oath_code_cancel(&s_oath);
    if (p.code_dropped) DIAG("K_OATH_CODE not served (mode/time/busy/prompt up)");
    if (p.send) {
        uint8_t bell = chest_round_next_doorbell(&s_round);
        bool ok = send_request(p.cmd, p.arg, bell);
        chest_round_sent(&s_round, ok, p.cmd, p.arg, st->instance, now);
        if (ok && p.cmd == CHEST_REQ_CODE) chest_oath_code_requested(&s_oath, p.arg);
    }

    /* Confirm (contract §5): the press must match THIS block's op AND instance. */
    if (chest_press_matches(pressed, CHEST_BLOCK_OK, st)) chest_confirm_request(&s_confirm, st, now);
    if (chest_confirm_step(&s_confirm, st, now)) write_confirm(s_confirm.instance);

    if (p.read_segment) {
        bool ok = read_segment(st->dma_len);
        chest_round_segment_read(&s_round, ok, st->dma_seq);
        if (ok) consume_segment(st, now);
        else {
            /* RDDMA may have landed before INT0 failed: whatever reached
             * s_dma (a code, possibly) is wiped now, not at the retry. */
            memset(s_dma, 0, st->dma_len);
            DIAG("DMA RDDMA seq=%u len=%u: bus busy, next round", st->dma_seq, st->dma_len);
        }
    }
}

/* A K_OATH_CODE press (chest_oath_code_key decides, pure and tested): true
 * when it is a code REQUEST for the round planner. TYPE goes through the
 * engine's own HID queue (hid_report_type_usages) — the left's route must be
 * USB: on the radio route the dongle types and hid_transport drops the
 * left's reports (fusion), so the code would vanish unseen. The chest only
 * exists while a host is there anyway; the guard makes it explicit. */
static bool type_or_request(uint16_t press_epoch, uint32_t now)
{
    uint8_t u[CHEST_OATH_TYPE_MAX];
    uint8_t n = 0;
    bool route_usb = kbd_active_route() == KBD_OUT_USB && keyboard_get_usb_bl_state() == 0;
    chest_oath_key_t k = chest_oath_code_key(&s_oath, press_epoch, now, route_usb, u, &n);
    if (k == CHEST_OATH_KEY_TYPE) {
        bool ok = hid_report_type_usages(u, n);
        memset(u, 0, sizeof u);          /* the digits do not outlive the queueing */
        if (ok) ESP_LOGI(TAG, "TOTP code typed (%u digits)", n);
        else    ESP_LOGW(TAG, "TOTP code not typed: HID queue full (code hidden)");
    } else if (k == CHEST_OATH_KEY_IGNORE) {
        DIAG("K_OATH_CODE on a code no longer typeable (expired, changed, or route %s)",
             route_usb ? "USB" : "not USB");
    }
    memset(u, 0, sizeof u);
    return k == CHEST_OATH_KEY_REQUEST;
}

/* The chest vanished while the host is still there (reboot, unpowered):
 * the next one is a new chest — its 0x11, doorbell and instances restart.
 * The WANTED mode stays: the self-heal re-requests it once it is back. */
static void chest_vanished(void)
{
    chest_round_reset(&s_round);
    chest_oath_reset(&s_oath);
    s_confirm.armed = false;
    s_mode_track.differ_reads = 0;
}

static void go_absent(void)
{
    gpio_intr_disable(BOARD_CHEST_IRQ);
    if (s_dev) {
        esp_err_t e = spi_bus_remove_device(s_dev);
        if (e != ESP_OK) ESP_LOGW(TAG, "spi_bus_remove_device: %s", esp_err_to_name(e));
        s_dev = NULL;
    }
    cs_release();
    chest_gate_publish(0, 0);
    chest_gate_publish_code(0);
    chest_vanished();
    s_mode_wanted = CHEST_MODE_NONE;               /* v2 spec §3: presence lost -> none */
    s_mode_state = CHEST_MODE_ARRIVED;
    s_blk = CHEST_BLOCK_ABSENT;
    s_nav_pending = false; s_code_pending = false;
    if (s_veto_code) { veille_veto(VEILLE_VETO_CODE, false); s_veto_code = false; }
    { chest_view_t v = {0};
      taskENTER_CRITICAL(&s_view_mux); s_view = v; taskEXIT_CRITICAL(&s_view_mux); }
    s_badver_logged = 0; s_corrupt_logged = 0;   /* once-per-presence-session logs, next session starts fresh */
    ESP_LOGI(TAG, "chest gone: CS released");
}

static void chest_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* s_dev: poll cadence. s_want && !s_dev: device add failed, retry
         * instead of blocking forever. Otherwise (battery, no chest): block
         * until the sleep task's presence hand-off wakes us. */
        TickType_t wait = portMAX_DELAY;
        if (s_dev) wait = pdMS_TO_TICKS(CHEST_POLL_MS);
        else if (s_want) wait = pdMS_TO_TICKS(CHEST_DEV_RETRY_MS);
        ulTaskNotifyTake(pdTRUE, wait);

        /* Taken on EVERY round, whatever happens below — a press queued for
         * an operation the owner saw must not survive a non-OK round (chest
         * reboot -> ABSENT, CORRUPT, BAD_VERSION, a bus-busy skipped read)
         * and confirm a different op at the next OK block (review Important,
         * 2026-09-29). Matched against the CURRENT chest state below. The
         * other keys are taken every round too, so none waits for a later
         * chest to act on it. */
        uint32_t pressed   = chest_gate_take_press();
        bool     mode_next = chest_gate_take_mode_next();
        int8_t   nav       = chest_gate_take_oath_nav();
        uint32_t code_rec  = chest_gate_take_oath_code();
        bool     code_key  = s_code_pending;
        s_code_pending = false;

        if (s_want && !s_dev) {
            if (!dev_add()) { ESP_LOGE(TAG, "spi_bus_add_device failed"); continue; }
            chest_round_reset(&s_round);
            chest_oath_reset(&s_oath);
            gpio_intr_enable(BOARD_CHEST_IRQ);
            ESP_LOGI(TAG, "USB host present: talking to the chest (CS GPIO%d, IRQ GPIO%d)",
                     BOARD_CHEST_CS, BOARD_CHEST_IRQ);
        } else if (!s_want && s_dev) {
            go_absent();
            continue;
        }
        if (!s_dev) continue;

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        /* Navigation is applied to the model even when the bus is busy
         * (review note, Task 5): the code disappears at the first
         * navigation key, whatever the wire does. The LIST decision it
         * implies waits for the next OK round. */
        if (nav) { chest_oath_nav(&s_oath, nav); s_nav_pending = true; }
        /* K_OATH_CODE (Mae, 2026-10-01): typed if the press was made on the
         * code still visible now, a request if no code was on screen. Decided
         * before this round's read, so a code arriving later in the round is
         * never typed by a press that did not see it (the record's epoch). */
        if (CHEST_GATE_CODE_PRESSED(code_rec)) {
            if (type_or_request(CHEST_GATE_CODE_EPOCH(code_rec), now)) code_key = true;
        }

        if (read_block()) {
            chest_status_t st = {0};   /* chest_proto_parse writes it only on OK (its own contract) */
            chest_block_t blk = chest_proto_parse(s_rx, CHEST_REG_SIZE, &st);
            switch (blk) {
            case CHEST_BLOCK_OK:
                round_ok(&st, pressed, mode_next, s_nav_pending, code_key, now);
                s_nav_pending = false;
                s_st = st;
                break;
            case CHEST_BLOCK_BAD_VERSION:
                if (!s_badver_logged) { ESP_LOGW(TAG, "chest speaks protocol %u, we speak %u: ignored", s_rx[4], CHEST_PROTO_VERSION); s_badver_logged = 1; }
                chest_gate_publish(0, 0);
                break;
            case CHEST_BLOCK_CORRUPT:
                if (!s_corrupt_logged) { ESP_LOGW(TAG, "chest block corrupt (magic/CRC/label/short read): ignored"); s_corrupt_logged = 1; }
                chest_gate_publish(0, 0);
                break;
            default:   /* CHEST_BLOCK_ABSENT: booting or unpowered, the ordinary case, never logged */
                chest_gate_publish(0, 0);
                if (s_blk != CHEST_BLOCK_ABSENT) chest_vanished();
                break;
            }
            s_blk = blk;
        } else {
            s_code_pending = code_key;   /* bus busy: the press gets the next round */
        }

        /* Screen view — pure builder, also on a bus-busy round (last block
         * seen), so a navigation or an expired code shows at once. */
        chest_view_t v;
        chest_view_build(&v, s_blk, &s_st, s_mode_wanted, s_mode_state, &s_oath, now);
        taskENTER_CRITICAL(&s_view_mux); s_view = v; taskEXIT_CRITICAL(&s_view_mux);
        /* The code on screen, for the engine to stamp a K_OATH_CODE press with. */
        chest_gate_publish_code(chest_oath_visible_epoch(&s_oath, now));
        /* Mae, 2026-09-29: no sleep while a code is on the screen (bounded
         * by its window, <= 30 s — chest_oath_code_visible hides it then). */
        if (v.code_visible != s_veto_code) {
            s_veto_code = v.code_visible;
            veille_veto(VEILLE_VETO_CODE, s_veto_code);
        }
    }
}

void chest_link_start(void)
{
    cs_release();
    const gpio_config_t irq = { .pin_bit_mask = 1ULL << BOARD_CHEST_IRQ, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                .intr_type = GPIO_INTR_POSEDGE };   /* R49 is the pull-down */
    gpio_config(&irq);
    gpio_intr_disable(BOARD_CHEST_IRQ);
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(e)); return; }
    gpio_isr_handler_add(BOARD_CHEST_IRQ, irq_isr, NULL);
    xTaskCreatePinnedToCore(chest_task, "chest", CHEST_TASK_STACK, NULL, 3, &s_task, 1);
    chest_gate_set_notify(wake_from_key);
}

void chest_link_presence(bool usb_host)
{
    if (usb_host == s_want || !s_task) return;
    s_want = usb_host;
    xTaskNotifyGive(s_task);
}

void chest_link_view(chest_view_t *v)
{
    if (!v) return;
    taskENTER_CRITICAL(&s_view_mux); *v = s_view; taskEXIT_CRITICAL(&s_view_mux);
}
