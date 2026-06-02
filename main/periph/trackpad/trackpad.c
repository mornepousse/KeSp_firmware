/*
 * trackpad.c — IQS5xx trackpad driver for KaSe half firmware.
 *
 * Skeleton status (after Plan IQS5xx-v1):
 *   REAL:  I2C init, RST pulse, RDY ISR + semaphore, I2C probe.
 *   REAL:  IQS5xx register read (9-byte block, GestureEvents0..RelY).
 *   REAL:  trackpad_map() — pure gesture-to-HID mapping (host-testable).
 *   REAL:  rf_encode_trackpad + half_spi_lock + rf_driver_send (send path).
 *
 * The PCB is reversible: both halves compile this module. Runtime probe
 * (I2C ACK check) determines whether the trackpad is mounted.
 *
 * s_radio is exposed as a non-static extern from half_scan_task.c.
 *
 * TEST_HOST guard: trackpad_map() and clamp8() are outside the guard
 * so they can be compiled and tested on the host without ESP-IDF.
 */

/* ── Host-safe includes (no ESP-IDF) ── */
#include "trackpad.h"
#include "rf_packet.h"      /* rf_trackpad_t — host-safe (stdint/stdbool only) */
#include <stdbool.h>
#include <stdint.h>

/* ── IQS5xx-B000 register addresses (16-bit, big-endian on wire) ── */
#define IQS5XX_REG_GESTURE_EVENTS_0   0x000F
#define IQS5XX_REG_GESTURE_EVENTS_1   0x0010
#define IQS5XX_REG_SYSTEM_INFO_0      0x0011
#define IQS5XX_REG_SYSTEM_INFO_1      0x0012
#define IQS5XX_REG_NUMBER_OF_FINGERS  0x0013
#define IQS5XX_REG_RELATIVE_X         0x0014   /* int16, big-endian */
#define IQS5XX_REG_RELATIVE_Y         0x0016   /* int16, big-endian */
#define IQS5XX_REG_SYSTEM_CONTROL_0   0x0431   /* ACK_RESET bit — VERIFY at bench */
#define IQS5XX_REG_END_WINDOW         0xEEEE   /* End Communication Window */

/* ── Gesture event bitmasks (IQS5xx-B000) ───────────────────────── */
#define IQS5XX_GEST0_SINGLE_TAP       0x01     /* GE0 bit 0 — single tap (any finger count) */
#define IQS5XX_GEST0_PRESS_HOLD       0x02     /* GE0 bit 1 — press-and-hold → start drag */
#define IQS5XX_GEST1_TWO_FINGER_TAP   0x01     /* GE1 bit 0 — 2-finger tap → right click */
#define IQS5XX_GEST1_SCROLL           0x02     /* GE1 bit 1 — 2-finger scroll (vertical + horizontal) */

/* ── SystemInfo0 bitmasks ── */
#define IQS5XX_SYSINFO0_SHOW_RESET    0x80     /* bit 7 — set after reset */

/* ── System Control 0 bitmasks ── */
#define IQS5XX_SYSCTRL0_ACK_RESET     0x02     /* bit 1 — VERIFY address at bench */

/* ── Read block ── */
/* IQS5xx-B000 map (confirmed vs QMK azoteq driver): GestureEvents0=0x000D,
 * GE1=0x000E, SystemInfo0=0x000F, SystemInfo1=0x0010, NumberOfFingers=0x0011,
 * RelativeX=0x0012 (2B), RelativeY=0x0014 (2B), AbsoluteX=0x0016.
 * Reading from 0x000D puts GE0 at data[0], RelX at data[5..6], RelY at data[7..8]. */
#define IQS5XX_READ_START_ADDR        0x000D   /* GestureEvents0 */
#define IQS5XX_READ_BYTE_COUNT        9        /* 0x000D..0x0015 → through RelY LSB */

/* ── Tunable constants (outside TEST_HOST guard — used by trackpad_map) ── */
#define IQS5XX_SCROLL_DIV             8        /* divide rel for scroll speed; tune at bench */
#define IQS5XX_EXPLICIT_END_WINDOW    1        /* 1 = write 0xEEEE after read */
#define IQS5XX_INVERT_X               0        /* 1 = negate dx — verify at bench */
#define IQS5XX_INVERT_Y               0        /* 1 = negate dy — verify at bench */

/* Cursor sensitivity scaling: dx,dy are multiplied by SENS_NUM/SENS_DEN.
 * Default 1.0 (3/3) preserves existing feel; bump SENS_NUM to e.g. 4 or 5 for
 * faster cursor without losing precision (clamp at int8 happens after). */
#define IQS5XX_SENS_NUM               3
#define IQS5XX_SENS_DEN               3

/* HID mouse button bits (matches USB HID standard button report) */
#define MOUSE_BTN_LEFT                0x01
#define MOUSE_BTN_RIGHT               0x02
#define MOUSE_BTN_MIDDLE              0x04

/* ── clamp8: pure, host-safe ─────────────────────────────────────────
 * Clamp a signed 16-bit value to the HID int8_t mouse delta range.
 * Upper bound is +127 (not +128) for symmetry with -127. */
static inline int8_t clamp8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -127) return -127;
    return (int8_t)v;
}

/* ── trackpad_map: pure, host-safe (v2) ─────────────────────────────
 *
 * Maps raw IQS5xx gesture fields to an rf_trackpad_t output.
 * No I/O, no FreeRTOS calls, no global state reads.
 *
 * Gesture precedence (most → least specific):
 *   1. Press-and-hold      → drag: left button held while fingers stay down,
 *                            released on n_fingers=0.
 *   2. Scroll gesture      → scroll_v / scroll_h (both axes), suppresses cursor.
 *   3. Tap (1F/2F/3F)      → button pulse press; next call emits release.
 *   4. Cursor movement     → dx, dy scaled by IQS5XX_SENS_NUM/DEN.
 *
 * 3-finger tap detection is synthesized: we track the peak number of fingers
 * seen during the current touch session (in state) and route a tap event to
 * MIDDLE if peak ≥ 3. The chip itself doesn't emit a 3F-tap event.
 *
 * Returns true if a packet should be sent (activity gate passed).
 */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  trackpad_state_t *state, rf_trackpad_t *out)
{
    /* Zero the output; caller sets seq after return */
    out->dx       = 0;
    out->dy       = 0;
    out->buttons  = 0;
    out->scroll_v = 0;
    out->scroll_h = 0;

    bool force_send = false;

    /* ── Axis inversion (compile-time flags; default off) ── */
    if (IQS5XX_INVERT_X) rel_x = (int16_t)(-rel_x);
    if (IQS5XX_INVERT_Y) rel_y = (int16_t)(-rel_y);

    /* ── Track peak fingers across the current touch session ──
     * Resets to 0 below when n_fingers returns to 0. */
    if (n_fingers > state->peak_fingers) state->peak_fingers = n_fingers;

    /* ── Drag: press-and-hold engages drag; releases when fingers leave ── */
    bool press_hold = (ge0 & IQS5XX_GEST0_PRESS_HOLD) != 0;
    bool just_ended_drag = false;
    if (press_hold && !state->drag_active) {
        state->drag_active = true;
        force_send         = true;
    }
    if (state->drag_active) {
        if (n_fingers == 0) {
            /* Fingers lifted → release the drag with one buttons=0 packet */
            state->drag_active = false;
            just_ended_drag    = true;
            force_send         = true;
            /* buttons stays 0x00 → that's the release */
        } else {
            /* Still dragging → hold left button down */
            out->buttons |= MOUSE_BTN_LEFT;
        }
    }

    /* ── Scroll vs cursor (scroll gesture takes priority over movement) ── */
    bool scroll_active = (ge1 & IQS5XX_GEST1_SCROLL) != 0;
    if (scroll_active) {
        /* 2-finger scroll, both axes */
        out->scroll_v = clamp8((int16_t)(rel_y / IQS5XX_SCROLL_DIV));
        out->scroll_h = clamp8((int16_t)(rel_x / IQS5XX_SCROLL_DIV));
        /* dx, dy remain 0 — scroll suppresses cursor */
    } else if (!state->drag_active || n_fingers > 0) {
        /* Cursor movement (also valid while dragging to drag-move).
         * Sensitivity scaling: multiply RelX/Y by SENS_NUM/SENS_DEN. */
        int32_t sx = (int32_t)rel_x * IQS5XX_SENS_NUM / IQS5XX_SENS_DEN;
        int32_t sy = (int32_t)rel_y * IQS5XX_SENS_NUM / IQS5XX_SENS_DEN;
        out->dx = clamp8((int16_t)sx);
        out->dy = clamp8((int16_t)sy);
    }

    /* ── Tap state machine (multi-finger aware) ──
     * - 2-finger tap event (GE1 bit 0) is dedicated → right click.
     * - SingleTap event (GE0 bit 0) is routed by peak_fingers:
     *     peak ≥ 3 → middle click; otherwise left click.
     * Drag is exclusive: don't synthesize taps in the same iteration as a drag
     * start or just-ended drag (the IQS5xx may emit a SingleTap on hold-release). */
    bool two_finger_tap = (ge1 & IQS5XX_GEST1_TWO_FINGER_TAP) != 0;
    bool single_tap     = (ge0 & IQS5XX_GEST0_SINGLE_TAP)     != 0;

    if (state->pending_release) {
        out->buttons          = 0x00;
        state->pending_release = false;
        force_send             = true;
    } else if (!state->drag_active && !just_ended_drag) {
        if (two_finger_tap) {
            out->buttons           = MOUSE_BTN_RIGHT;
            state->pending_release = true;
            force_send             = true;
        } else if (single_tap) {
            out->buttons           = (state->peak_fingers >= 3)
                                    ? MOUSE_BTN_MIDDLE
                                    : MOUSE_BTN_LEFT;
            state->pending_release = true;
            force_send             = true;
        }
    }

    /* ── Reset peak fingers when surface is empty (new touch starts fresh) ── */
    if (n_fingers == 0) state->peak_fingers = 0;

    /* ── Activity gate ── */
    bool send = force_send
             || (out->dx       != 0)
             || (out->dy       != 0)
             || (out->scroll_v != 0)
             || (out->scroll_h != 0)
             || (out->buttons  != 0x00);

    return send;
}

#ifndef TEST_HOST

/* ── ESP-IDF-dependent includes ── */
#include "board.h"           /* BOARD_TRACK_* GPIO + I2C defines */
#include "half_spi.h"        /* half_spi_lock / half_spi_unlock */
#include "rf_driver.h"       /* rf_driver_send, rf_radio_t */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "trackpad";

/* ── I2C bus + device handles ──────────────────────────────── */
static i2c_master_bus_handle_t s_i2c_bus    = NULL;
static i2c_master_dev_handle_t s_i2c_device = NULL;

/* ── RDY interrupt semaphore ───────────────────────────────── */
static SemaphoreHandle_t s_rdy_sem = NULL;

/* ── Packet sequence counter (shared trackpad seq space) ───── */
static volatile uint8_t s_seq = 0;

/* ── Reference to the half's NRF radio (owned by half_scan_task) ── */
/* half_scan_task.c defines s_radio as non-static (extern linkage). */
extern rf_radio_t s_radio;

/* ── ShowReset ACK state — cleared after first successful ACK write ── */
static bool s_need_reset_ack = true;

/* ── Gesture state — held across calls to trackpad_map (caller-owned). ── */
static trackpad_state_t s_tp_state = {0};

/* ── Trackpad task handle — needed for suspend/resume during light-sleep ── */
static TaskHandle_t s_tp_task = NULL;

/* ── RDY GPIO ISR handler — signals the trackpad task ──────── */
static void IRAM_ATTR rdy_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_rdy_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

bool trackpad_init(void)
{
    /* ── I2C master bus init ──────────────────────────────── */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .i2c_port          = BOARD_TRACK_I2C_PORT,
        .scl_io_num        = BOARD_TRACK_SCL_GPIO,
        .sda_io_num        = BOARD_TRACK_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,   /* external 4.7kΩ pull-ups on PCB */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", err);
        return false;
    }

    /* ── RST pulse: 10 ms low, then high, settle 50 ms ─────── */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(BOARD_TRACK_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TRACK_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   /* IQS5xx startup time after reset */

    /* ── RDY interrupt semaphore + ISR ─────────────────────── */
    s_rdy_sem = xSemaphoreCreateBinary();
    if (s_rdy_sem == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        return false;
    }

    gpio_config_t rdy_cfg = {
        .pin_bit_mask = (1ULL << BOARD_TRACK_RDY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* external pull-up on PCB */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,     /* IQS5xx-B000 RDY is ACTIVE-HIGH: rises
                                                * when a comm window opens. Read while high.
                                                * (NEGEDGE fired at window CLOSE → I2C timeout.) */
    };
    gpio_config(&rdy_cfg);

    esp_err_t isr_err = gpio_install_isr_service(0);
    /* ESP_ERR_INVALID_STATE means ISR service is already installed — that is OK */
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service returned %d (non-fatal)", isr_err);
    }
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);

    /* ── I2C probe: check device ACKs at BOARD_TRACK_I2C_ADDR ── */
    err = i2c_master_probe(s_i2c_bus, BOARD_TRACK_I2C_ADDR, 50 /* timeout ms */);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "I2C probe: no ACK at 0x%02X — trackpad not present on this half",
                 BOARD_TRACK_I2C_ADDR);
        /* Clean up ISR and bus */
        gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    /* ── Add I2C device handle for subsequent transactions ───── */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_TRACK_I2C_ADDR,
        .scl_speed_hz    = BOARD_TRACK_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", err);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return false;
    }

    ESP_LOGI(TAG, "trackpad present at I2C 0x%02X — init OK", BOARD_TRACK_I2C_ADDR);
    return true;
}

/* ── Trackpad FreeRTOS task ─────────────────────────────────── */
static void trackpad_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "trackpad_task started");

    for (;;) {
        /* Wait for RDY pin to go low (data-ready from IQS5xx) */
        xSemaphoreTake(s_rdy_sem, portMAX_DELAY);

        /* ── Step 1: Read 9-byte block from GestureEvents0 (0x000F) ── */
        /* IQS5xx window-transfer: write 2-byte reg addr, read 9 bytes.
         * Must complete while RDY is asserted. Timeout 50 ms. */
        uint8_t reg_addr[2] = {
            (IQS5XX_READ_START_ADDR >> 8) & 0xFF,   /* 0x00 */
            (IQS5XX_READ_START_ADDR)      & 0xFF,   /* 0x0F */
        };
        uint8_t data[IQS5XX_READ_BYTE_COUNT];
        esp_err_t err = i2c_master_transmit_receive(
            s_i2c_device,
            reg_addr, sizeof(reg_addr),
            data, sizeof(data),
            50   /* timeout ms */
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2C read failed: %d — skip event", err);
            /* Do not end window — bus is already free after failed transaction */
            continue;
        }

        /* ── Step 2: End Communication Window (best-effort) ─────────── */
        /* Some IQS5xx revisions require an explicit 0xEEEE write to close
         * the window. Others close on the I2C STOP from step 1.
         * IQS5XX_EXPLICIT_END_WINDOW=1 by default — disable at bench if
         * bus hangs (see bench checklist item 2). */
#if IQS5XX_EXPLICIT_END_WINDOW
        {
            uint8_t end_cmd[3] = {0xEE, 0xEE, 0x00};
            /* Best-effort: chip may already have closed the window — ignore error */
            i2c_master_transmit(s_i2c_device, end_cmd, sizeof(end_cmd), 10);
        }
#endif

        /* ── Step 3: Extract fields from read block ─────────────────── */
        uint8_t  ge0       = data[0];   /* GestureEvents0 */
        uint8_t  ge1       = data[1];   /* GestureEvents1 */
        uint8_t  sysinfo0  = data[2];   /* SystemInfo0 — ShowReset = bit7 */
        uint8_t  n_fingers = data[4];   /* NumberOfFingers */
        int16_t  rel_x     = (int16_t)((data[5] << 8) | data[6]);
        int16_t  rel_y     = (int16_t)((data[7] << 8) | data[8]);

        /* ── Step 4: ShowReset ACK (once after hardware reset) ───────── */
        /* On first read after RST pulse, ShowReset (sysinfo0 bit7) is set.
         * Acknowledge by writing ACK_RESET bit to System Control 0 (0x0431).
         * Skip the data from this event — RelX/RelY are garbage post-reset.
         * Register address and bit position: VERIFY against IQS5xx-B000
         * datasheet (spec §7 item 6). Expected: addr=0x0431, bit1=0x02. */
        if (s_need_reset_ack) {
            if (sysinfo0 & IQS5XX_SYSINFO0_SHOW_RESET) {
                uint8_t ack_cmd[3] = {
                    (IQS5XX_REG_SYSTEM_CONTROL_0 >> 8) & 0xFF,   /* 0x04 */
                    (IQS5XX_REG_SYSTEM_CONTROL_0)      & 0xFF,   /* 0x31 */
                    IQS5XX_SYSCTRL0_ACK_RESET                     /* 0x02 */
                };
                esp_err_t ack_err = i2c_master_transmit(s_i2c_device,
                                                         ack_cmd, sizeof(ack_cmd),
                                                         20);
                if (ack_err == ESP_OK) {
                    ESP_LOGI(TAG, "ShowReset ACK sent — entering normal operation");
                } else {
                    ESP_LOGW(TAG, "ShowReset ACK failed: %d", ack_err);
                }
                s_need_reset_ack = false;
                continue;   /* skip this event's movement data */
            } else {
                /* ShowReset not set on first read — chip was already running */
                s_need_reset_ack = false;
                ESP_LOGI(TAG, "ShowReset not set on first read — normal operation");
            }
        }

        /* ── Step 5: Map gesture fields → HID output ────────────────── */
        rf_trackpad_t tp;
        bool should_send = trackpad_map(ge0, ge1, n_fingers,
                                        rel_x, rel_y,
                                        &s_tp_state, &tp);

        /* ── Step 6: Activity gate ───────────────────────────────────── */
        if (!should_send) {
            /* All fields zero, no button event — drop silently.
             * No log here: this is the common idle case (hot path). */
            continue;
        }

        /* ── Step 7: Encode and transmit ────────────────────────────── */
        tp.seq = s_seq++;
        uint8_t buf[7];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 7);
        half_spi_unlock();
    }
}

void trackpad_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        trackpad_task, "trackpad",
        3072,   /* stack: 3 KB (I2C + encoding, no large buffers) */
        NULL, 6, &s_tp_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore failed: %d", (int)ret);
    }
}

void trackpad_suspend(void)
{
    gpio_isr_handler_remove(BOARD_TRACK_RDY_GPIO);   /* no RDY wake while asleep */
    if (s_tp_task) vTaskSuspend(s_tp_task);
}

void trackpad_resume(void)
{
    if (s_tp_task) vTaskResume(s_tp_task);
    gpio_isr_handler_add(BOARD_TRACK_RDY_GPIO, rdy_isr_handler, NULL);
}

#endif /* TEST_HOST */
