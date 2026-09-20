/*
 * Keymap engine embedded in the dongle — FUSION mode (KASE_DONGLE_FUSION).
 *
 * See dongle_engine.h and the design 2026-09-12-dongle-fusion. The dongle
 * receives both RAW half-matrices, merges them (fusion_state) and runs the
 * SAME keymap engine as left, then outputs HID over its own USB.
 *
 * ── Engine state ──
 * On a keyboard, matrix_scan.c provides MATRIX_STATE, keycodes[], current_press_*
 * and the layer/activity flags. The dongle has no local matrix: this file
 * supplies those globals in its place. build_keycode_report() reads them;
 * as long as nobody called it, the linker used to eliminate them
 * (gc-sections) — this very call is what finally makes them necessary.
 *
 * ── Concurrency ──
 * rf_rx_task drops frames via dongle_engine_on_matrix(); the engine task
 * consumes them. ONLY the fusion state (s_fusion + the s_file queue) is
 * shared between the two — protected by s_mux. current_press_* and the whole
 * engine cycle are touched ONLY by the engine task, so no lock needed.
 */
#include "dongle_engine.h"
#if CONFIG_KASE_DONGLE_FUSION

#include "half_link.h"          /* fusion_state_t, fusion_apply/timeout/collect */
#include "fusion_file.h"        /* each received transition is replayed, in order */
#include "fusion_route.h"       /* fusion_dongle_types (rule 3) */
#include "key_processor.h"      /* build_keycode_report, process_matrix_changes, taps */
#include "hid_report.h"         /* send_hid_key */
#include "tap_hold.h"
#include "tap_dance.h"
#include "combo.h"
#include "leader.h"
#include "key_features.h"       /* key_override_init */
#include "keymap.h"             /* macros_list, MACRO_*, keymaps[], KEYMAP_BLOB_BYTES */
#include "config_sync.h"        /* sync guard rail: fingerprint + coherence */
#include "matrix_scan.h"        /* extern of the globals defined here */
#include "hid_transport.h"      /* hid_send_keyboard (send_tap) */
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

#ifndef BOARD_REMOTE_COLS_MIRRORED
#define BOARD_REMOTE_COLS_MIRRORED 0
#endif

static const char *TAG = "dongle_eng";

/* ── Engine state globals (provided by matrix_scan.c on a keyboard) ────────── */
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS];
uint8_t keycodes[6];
uint8_t current_press_row[6];
uint8_t current_press_col[6];
uint8_t current_press_stat[6];
volatile uint8_t stat_matrix_changed = 0;
uint8_t last_layer = 0;
volatile uint8_t is_layer_changed = 0;
volatile uint32_t last_activity_time_ms = 0;

uint32_t get_last_activity_time_ms(void) { return last_activity_time_ms; }

/* ── Fusion state shared between rf_rx_task ↔ engine task ─────────────────── */
static fusion_state_t   s_fusion;
static fusion_file_t    s_file;      /* received transitions, to replay in order (under s_mux) */
static SemaphoreHandle_t s_mux;

/* Left mode (phase 2). true = left is driven by a USB host: the dongle goes
 * quiet (it types locally) and re-sends right → left. Simple flag: written
 * by rf_rx_task (STATUS/MATRIX), read by the engine task. */
static volatile bool s_left_usb = false;

void dongle_engine_set_left_usb(bool usb)
{
    if (usb != s_left_usb) {
        s_left_usb = usb;
        ESP_LOGW(TAG, "mode gauche : %s", usb ? "USB (dongle se tait, réémet la droite)"
                                              : "sans-fil (dongle tape)");
    }
}

bool dongle_engine_left_usb(void) { return s_left_usb; }

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ── Config sync guard rail: left announces its fingerprint, the dongle
 * compares it to its own ─────────────────────────────────────────────────────
 *
 * Over the air it is the dongle that types, with ITS OWN keymap. If its
 * keymap diverges from what the user set on left (dongle not yet
 * reprovisioned, cf. phase 3), it would silently type something else. So we
 * expose the coherence to the controller (CDC) and log it on change.
 * s_coh: written by rf_rx_task (one task), read by CDC (another) — aligned
 * u32 fields, atomic read on Xtensa, purely diagnostic values. */
static config_coherence_t s_coh;

static inline uint32_t dongle_own_fp(void)
{
    return config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
}

void dongle_engine_note_left_fp(uint32_t fp)
{
    if (!config_coherence_note(&s_coh, fp, now_ms())) return;  /* nothing new */
    uint32_t own = dongle_own_fp();
    if (config_fp_match(own, fp))
        ESP_LOGI(TAG, "config cohérente gauche↔dongle : empreinte 0x%08X", (unsigned)fp);
    else
        ESP_LOGW(TAG, "DIVERGENCE de config : gauche=0x%08X dongle=0x%08X — keymaps "
                      "désynchronisées, le dongle tape peut-être autre chose", (unsigned)fp,
                 (unsigned)own);
}

void dongle_engine_get_coherence(uint32_t *own_fp, uint32_t *left_fp,
                                 uint32_t *age_ms, bool *match)
{
    uint32_t own = dongle_own_fp();
    if (own_fp)  *own_fp  = own;
    if (left_fp) *left_fp = s_coh.left_fp;
    if (age_ms)  *age_ms  = s_coh.vue ? (now_ms() - s_coh.left_ms) : 0xFFFFFFFFu;
    if (match)   *match   = config_fp_match(own, s_coh.left_fp);
}

/* ── Auto-sync via ACK payload: what the dongle slips into the ACK ──────────
 *
 * The dongle distills ONLY on a known divergence (left announced a
 * fingerprint ≠ 0 and ≠ ours). It keeps no transfer state: left drives (REQ
 * next-chunk), we serve whatever is asked, and it's the fingerprint it will
 * announce at the end that cuts off the beacon. An ACK payload can be lost
 * (the ACK itself isn't acknowledged): left asks again, we re-serve — idempotent. */
bool dongle_sync_active(void)
{
    if (!s_coh.vue || s_coh.left_fp == 0u) return false;   /* nothing announced: unknown */
    return !config_fp_match(dongle_own_fp(), s_coh.left_fp);
}

uint16_t dongle_sync_ack_for(uint8_t req_next, uint8_t *out)
{
    if (out == NULL) return 0;
    if (req_next < SYNC_N_CHUNKS) {
        rf_sync_chunk_t c;
        c.idx = req_next;
        memcpy(c.data, (const uint8_t *)keymaps + (size_t)req_next * SYNC_CHUNK_BYTES,
               SYNC_CHUNK_BYTES);
        return rf_encode_sync_chunk(out, &c);
    }
    rf_sync_beacon_t b = { .fp_target = dongle_own_fp(), .n_chunks = SYNC_N_CHUNKS };
    return rf_encode_sync_beacon(out, &b);
}

/* Sends a brief keystroke (press+release), for tap-dance / leader / macros. */
static void send_tap(uint8_t kc, uint8_t mod)
{
    uint8_t buf[6] = { kc };
    hid_send_keyboard(mod, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(buf, 0, 6);
    hid_send_keyboard(0, buf);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* Copies the merged state into current_press_* (the format the engine reads).
 * Called under s_mux (reads s_fusion). Left in direct columns, right in high
 * columns via the PCB mirror — exactly matrix_apply_remote() but for TWO
 * remote halves. */
static void fill_current_press(const fusion_state_t *fs)
{
    uint8_t rows[6], cols[6];
    uint8_t n = fusion_collect(fs, MATRIX_COLS, BOARD_REMOTE_COLS_MIRRORED,
                               rows, cols, 6);
    for (uint8_t i = 0; i < 6; i++) {
        if (i < n) {
            current_press_row[i]  = rows[i];
            current_press_col[i]  = cols[i];
            current_press_stat[i] = 1;
        } else {
            current_press_row[i]  = INVALID_KEY_POS;
            current_press_col[i]  = INVALID_KEY_POS;
            current_press_stat[i] = 0;
        }
    }
}

/* OVERWRITTEN transitions (CDC RF_STATUS[27..30]): since the transition queue
 * (fusion_file.h, 2026-09-19), this is no longer "a frame arrived before the
 * engine could read" (536 in one evening) but only a queue OVERFLOW (8 states,
 * 80 ms of engine delay) — the exception it should always have been. */
uint32_t dongle_engine_transitions_ecrasees(void) { return fusion_file_ecrasees(&s_file); }
/* Maximum gap between two engine cycles since the last read (ms): a 70 ms
 * tap is only overwritten if the engine hasn't run for 70 ms. Tells what is
 * blocking it, not just that it happened. Reset to zero on read. */
static uint32_t s_gap_max_ms, s_gap_dernier_ms;
uint32_t dongle_engine_gap_max_ms(void) { uint32_t g = s_gap_max_ms; s_gap_max_ms = 0; return g; }

/* RE-PRESS detector: a key released then pressed again in less than
 * DONGLE_REAPPUI_MS. Two known causes, both invisible before the transition
 * queue (the engine used to sample at 10 ms): a stale repeat sent by a half
 * after release (fixed by radio_emettre PERIME, 2026-09-20) and mechanical
 * bounce longer than the halves' debounce (3 → 5 ms the same day). Counted
 * and logged: on the bench, whatever remains after these two fixes is the
 * switch. */
#define DONGLE_REAPPUI_MS 30u
static uint32_t s_relache_ms[2][RF_HALF_ROWS * RF_HALF_COLS];
static uint32_t s_reappuis;
static uint8_t  s_reappui_half, s_reappui_key; static uint16_t s_reappui_ms;   /* the latest one: for reporting without a console */
uint32_t dongle_engine_reappuis(void) { return s_reappuis; }
void dongle_engine_dernier_reappui(uint8_t *half, uint8_t *key, uint16_t *delta_ms)
{ *half = s_reappui_half; *key = s_reappui_key; *delta_ms = s_reappui_ms; }
static void detecter_reappui(const half_state_t *avant, const rf_matrix_t *m, uint32_t now)
{
    unsigned h = (m->half == RF_HALF_RIGHT) ? 1 : 0;
    for (uint8_t r = 0; r < RF_HALF_ROWS; r++)
        for (uint8_t c = 0; c < RF_HALF_COLS; c++) {
            bool etait = rf_bitmap_get(avant->bitmap, r, c), est = rf_bitmap_get(m->bitmap, r, c);
            unsigned k = (unsigned)r * RF_HALF_COLS + c;
            if (etait && !est) s_relache_ms[h][k] = now ? now : 1;
            else if (!etait && est && s_relache_ms[h][k] && (uint32_t)(now - s_relache_ms[h][k]) < DONGLE_REAPPUI_MS) {
                s_reappuis++;
                s_reappui_half = (uint8_t)m->half; s_reappui_key = (uint8_t)k;
                s_reappui_ms = (uint16_t)(now - s_relache_ms[h][k]);
                ESP_LOGW(TAG, "re-appui #%lu : moitie %u (%u,%u) %lu ms apres son relachement",
                         (unsigned long)s_reappuis, (unsigned)m->half, (unsigned)r, (unsigned)c,
                         (unsigned long)(now - s_relache_ms[h][k]));
            }
        }
}

void dongle_engine_on_matrix(const rf_matrix_t *m)
{
    if (!s_mux) return;
    const half_state_t *hs = (m->half == RF_HALF_RIGHT) ? &s_fusion.right
                           : (m->half == RF_HALF_LEFT)  ? &s_fusion.left : NULL;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = hs && memcmp(hs->bitmap, m->bitmap, RF_HALF_BITMAP_BYTES) != 0;
    if (changed) detecter_reappui(hs, m, now_ms());
    if (fusion_apply(&s_fusion, m, now_ms()) && changed) {
        uint32_t avant = fusion_file_ecrasees(&s_file);
        fusion_file_push(&s_file, &s_fusion);   /* replayed by the engine, in order */
        if (fusion_file_ecrasees(&s_file) != avant)
            ESP_LOGW(TAG, "transition ecrasee (#%lu) : file pleine, moitie %u",
                     (unsigned long)fusion_file_ecrasees(&s_file), (unsigned)m->half);
    }
    xSemaphoreGive(s_mux);
}

/* One full engine cycle: fusion→current_press already done, decode and send.
 * Modeled on the "matrix changed" branch of keyboard_task.c. */
static void run_cycle(void)
{
    build_keycode_report();
    process_matrix_changes();
    if (key_processor_has_tap()) {
        send_hid_key();
        vTaskDelay(pdMS_TO_TICKS(10));
        key_processor_clear_taps();
        send_hid_key();
    } else {
        send_hid_key();
    }
    last_activity_time_ms = now_ms();
}

static void dongle_engine_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 10 ms: tap-hold / tap-dance timer cadence, same as the keyboard. */
        vTaskDelay(pdMS_TO_TICKS(10));
        {
            uint32_t t = now_ms();
            if (s_gap_dernier_ms && (uint32_t)(t - s_gap_dernier_ms) > s_gap_max_ms)
                s_gap_max_ms = (uint32_t)(t - s_gap_dernier_ms);
            s_gap_dernier_ms = t ? t : 1;
        }
        tap_hold_tick();
        tap_dance_tick();

        /* Phase 2: if left is driven by USB, the dongle GOES QUIET (it's
         * left's engine that types). We release whatever we held at the
         * host on the transition, then skip the cycle. drain_radio keeps
         * re-sending right → left meanwhile. */
        static bool prev_types = true;
        bool types = fusion_dongle_types(s_left_usb);
        if (!types) {
            if (prev_types) {
                uint8_t none[6] = {0};
                hid_send_keyboard(0, none);   /* release the dongle's held keys */
            }
            prev_types = false;
            /* Silent, but right keeps feeding the queue: without draining it,
             * up to 7 stale transitions (keys typed during USB) used to be
             * REPLAYED on returning wireless — phantom keystrokes on
             * unplug (reviewed 2026-09-20). We drop them, s_fusion stays correct. */
            xSemaphoreTake(s_mux, portMAX_DELAY);
            fusion_file_vider(&s_file);
            xSemaphoreGive(s_mux);
            continue;
        }
        if (!prev_types) {
            /* Resuming: apply the CURRENT state once (a key held at the
             * moment of unplugging must be typed), then replay as it comes. */
            xSemaphoreTake(s_mux, portMAX_DELAY);
            fusion_file_push(&s_file, &s_fusion);
            xSemaphoreGive(s_mux);
        }
        prev_types = true;

        /* Minimal critical section: timeout, then take ONE pending
         * transition. Each received transition is played by a full cycle,
         * in order — no more "last state wins". If more are left, we chain
         * through without waiting for the tick (10 ms per state only when a
         * tap is in progress in run_cycle). */
        fusion_state_t etat; bool do_cycle;
        do {
            do_cycle = false;
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (fusion_timeout(&s_fusion, now_ms(), HALF_LINK_TIMEOUT_MS))
                fusion_file_push(&s_file, &s_fusion);   /* release on silence */
            if (fusion_file_pop(&s_file, &etat)) do_cycle = true;
            xSemaphoreGive(s_mux);
            if (do_cycle) { fill_current_press(&etat); run_cycle(); }
        } while (do_cycle && fusion_file_en_attente(&s_file));

        /* A hold that just switched to "hold" → re-send. */
        if (tap_hold_hold_just_activated()) {
            build_keycode_report();
            send_hid_key();
        }

        /* Tap-dance resolved → tap. */
        if (tap_dance_just_resolved()) {
            uint8_t kc = tap_dance_consume();
            if (kc != 0) send_tap(kc, 0);
        }

        /* Leader: timeout + result. */
        if (leader_tick()) {
            uint8_t mod = 0;
            uint8_t kc = leader_consume(&mod);
            if (kc != 0) send_tap(kc, mod);
        }

        /* Macro pending → play the sequence (same rules as keyboard_task). */
        if (key_processor_has_pending_macro()) {
            int16_t idx = key_processor_consume_macro();
            if (idx >= 0 && idx < MAX_MACROS) {
                const macro_step_t *steps = macros_list[idx].steps;
                uint8_t pending_mod = 0;
                for (int s = 0; s < MACRO_MAX_STEPS && steps[s].keycode != 0; s++) {
                    uint8_t kc  = steps[s].keycode;
                    uint8_t mod = steps[s].modifier;
                    if (kc == MACRO_DELAY_MARKER) { vTaskDelay(pdMS_TO_TICKS(mod * 10)); continue; }
                    if (kc >= 0xE0 && kc <= 0xE7) { pending_mod |= (1 << (kc - 0xE0)); continue; }
                    send_tap(kc, mod | pending_mod);
                    pending_mod = 0;
                }
            }
        }
    }
}

void dongle_engine_start(void)
{
    if (s_mux) return;   /* already started */
    s_mux = xSemaphoreCreateMutex();
    memset(&s_fusion, 0, sizeof(s_fusion));
    fusion_file_init(&s_file);

    /* Same init as keyboard_manager_init(), minus the local scan worker. */
    tap_hold_init();
    tap_dance_init();
    combo_init();
    leader_init();
    key_override_init();
    hid_report_init();

    xTaskCreatePinnedToCore(dongle_engine_task, "dongle_eng", 6144, NULL, 9, NULL, 0);
    ESP_LOGI(TAG, "moteur keymap du dongle démarré (fusion)");
}

#endif /* CONFIG_KASE_DONGLE_FUSION */
