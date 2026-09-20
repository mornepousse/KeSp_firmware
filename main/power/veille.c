/* Niphargus sleep — brick B7. See veille.h for the power budget and
 * the reason the radio is turned off as early as the light stage. */
#include "veille.h"
#include "board.h"
#include "matrix_scan.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/gpio_reg.h"
#include "veille_task.h"   /* sleep/wake hooks: radio, display, gauge */
#include "esp_timer.h"
#include "tinyusb.h"
#include "driver/gpio.h"
#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"   /* uart_wait_tx_done: flush the console before sleep */
#endif
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "veille";

/* Sleep summary since boot, to read back a night: veille_bilan(). */
static uint32_t s_sommeils, s_dormi_ms;
static uint32_t s_dernier_reveil_ms;   /* for veille_en_grace (0 = never) */
void veille_bilan(uint32_t *sommeils, uint32_t *dormi_ms)
{
    if (sommeils) *sommeils = s_sommeils;
    if (dormi_ms) *dormi_ms = s_dormi_ms;
}

/* Thresholds adjustable on the bench: testing the EXT1 wake-up with the
 * default of 4 h would mean waiting four hours. The values in veille.h remain
 * the documentation and the anchor for the host test. */
#ifndef CONFIG_KASE_VEILLE_LEGERE_S
#define CONFIG_KASE_VEILLE_LEGERE_S   60
#endif
#ifndef CONFIG_KASE_VEILLE_PROFONDE_S
#define CONFIG_KASE_VEILLE_PROFONDE_S 14400
#endif

/* The pinout tables are sized by their initializer, not by
 * MATRIX_COLS/ROWS: the board.h files pad the tail with GPIO_NUM_NC for
 * smaller boards (Niphargus 7x4). The loops stop at the real dimensions,
 * so these entries exist without ever being read. Same reason as
 * in matrix_arm_key_wake(). */
static const int s_cols[] = { COLS0, COLS1, COLS2, COLS3, COLS4, COLS5,
                              COLS6, COLS7, COLS8, COLS9, COLS10, COLS11, COLS12 };
static const int s_rows[] = { ROWS0, ROWS1, ROWS2, ROWS3, ROWS4 };

/* To call at STARTUP, before matrix_setup().
 *
 * rtc_gpio_hold_en() deliberately survives deep sleep — that is what
 * keeps the columns high while the digital domain is powered off, and
 * hence what makes the EXT1 wake-up possible. But the hold ALSO survives a
 * restart: without this release the columns stay frozen, the scan driver
 * thinks it is driving them while they no longer move, and the whole matrix
 * reads garbage. gpio_reset_pin() is not enough — it does not undo a
 * hold set by the RTC multiplexer.
 *
 * Found on the bench on 2026-09-08: after an EXT1 wake-up, the keys were
 * completely wrong. Harmless if no hold is set, so called
 * unconditionally. */
void veille_liberer_gpio(void)
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        rtc_gpio_hold_dis(s_cols[i]);
        rtc_gpio_deinit(s_cols[i]);
    }
    for (int i = 0; i < MATRIX_ROWS; i++) {
        rtc_gpio_hold_dis(s_rows[i]);
        rtc_gpio_deinit(s_rows[i]);
    }
}

void veille_legere_entrer(void)
{
    ESP_LOGI(TAG, "light sleep");
    /* BENCH 2026-09-16: time the BLIND WINDOW. Between destroying the
     * driver and arming the wake-up, then between the wake-up and the capture,
     * a key can neither be scanned nor wake the board — 160 to 570 ms
     * of wakefulness around a sleep in the left half's log, and the first press
     * after a 15 s pause was lost. Four esp_timer markers (µs). */
#if CONFIG_KASE_VEILLE_DIAG
    int64_t t_entree = esp_timer_get_time(), t_radio, t_pilote, t_arme;
#endif

    /* Module hooks (veille_task.h): radio in power-down (900 nA instead
     * of 26 µA in standby-I, timer stopped, mutex held), display frozen. Sleep
     * no longer knows the modules by name — each one registered itself. */
    veille_hooks_dormir();
#if CONFIG_KASE_VEILLE_DIAG
    t_radio = esp_timer_get_time();
#endif
    rtc_matrix_deinit();          /* give the GPIOs back to the static wake-up */
#if CONFIG_KASE_VEILLE_DIAG
    t_pilote = esp_timer_get_time();
#endif
    matrix_arm_key_wake();
#if CONFIG_KASE_VEILLE_DIAG
    t_arme = esp_timer_get_time();
#endif

    /* Withdraw from the USB bus BEFORE sleeping. Light sleep cuts the PHY: from
     * the host's point of view this is an abrupt unplug, and on wake-up TinyUSB
     * finds an OTG controller in an undefined state — the right half would
     * no longer wake up while plugged in, only a reset would bring it back.
     * Found on the bench on 2026-09-11: 65 s of operation plugged in, then
     * total silence on the first sleep. On battery, the wake-up worked.
     *
     * tud_disconnect() releases the D+ pull-up cleanly: the host sees a
     * device withdrawing, not one disappearing. tud_connect() on wake-up
     * re-enumerates in ~1 s. A sleeping device has no business on the bus. */
    bool etait_connecte = tud_mounted();
    if (etait_connecte) tud_disconnect();

    /* DEEP sleep used to be unreachable: inactivity is only evaluated
     * while awake, and the board stays stuck here until a key — which resets
     * the counter to zero. A timer wake-up at the deep threshold (minus what
     * has already elapsed) switches to deep sleep without going through a
     * keystroke (the user, 2026-09-15: "it never goes into deep sleep"). */
    uint64_t reste_us = (uint64_t)(CONFIG_KASE_VEILLE_PROFONDE_S - CONFIG_KASE_VEILLE_LEGERE_S) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(reste_us);
    REG_WRITE(GPIO_STATUS_W1TC_REG, 0xFFFFFFFFu);   /* clean GPIO state: the wake-up mask will only report the sleep */
    uint64_t avant_us = (uint64_t)esp_timer_get_time();   /* esp_timer follows the RTC: the only reliable witness */
    /* Flush the console BEFORE sleeping: without this the "light sleep" line
     * and the entry timings stayed in the UART FIFO and only came out at
     * wake-up, stuck to the wake-up log — the silence after the last HB
     * was the only proof of a sleep (bench from Sept 13 to 19). ~2 ms
     * at 115200 baud for a few lines; with no console, no cost at all. */
#if CONFIG_ESP_CONSOLE_UART
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(20));
#endif
    esp_light_sleep_start();      /* blocks here until a key, or the timer */
#if CONFIG_KASE_VEILLE_DIAG
    int64_t t_sorti = esp_timer_get_time();
    /* BENCH 2026-09-16: VERY FIRST read after wake-up, before any
     * logging — are the lines still high? And which pin
     * triggered (mask remembered by the ESP)? A light tap on the left half did
     * not wake it up; a held press did. If the pin is in the mask
     * but already low here, the GPIO wake-up is SLOW; high here and invisible to
     * the capture, it's the capture. */
    uint8_t lignes_sortie = 0;
    for (int i = 0; i < MATRIX_ROWS; i++) if (gpio_get_level(s_rows[i])) lignes_sortie |= (uint8_t)(1u << i);
    /* No GPIO wake-up status API on the S3 (EXT1 only): we read the
     * GPIO interrupt status register, which remembers the pins whose level
     * condition occurred, INT_ENA or not. */
    uint64_t masque_reveil = REG_READ(GPIO_STATUS_REG);
#endif
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    uint32_t dormi_ms = (uint32_t)(((uint64_t)esp_timer_get_time() - avant_us) / 1000);
#if CONFIG_KASE_VEILLE_DIAG
    ESP_LOGW(TAG, "chrono entree : radio %lld us, pilote %lld us, armement %lld us, jusqu'au sommeil %lld us",
             (long long)(t_radio - t_entree), (long long)(t_pilote - t_radio),
             (long long)(t_arme - t_pilote), (long long)((int64_t)avant_us - t_arme));
#endif
    s_sommeils++; s_dormi_ms += dormi_ms;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGW(TAG, "%lu s de sommeil leger sans une touche : sommeil profond", (unsigned long)(dormi_ms / 1000));
        veille_profonde_entrer();   /* does not return; the radio is already off */
    }
    { uint32_t t = (uint32_t)(esp_timer_get_time() / 1000); s_dernier_reveil_ms = t ? t : 1; }
    /* BEFORE ANYTHING: prove the wake-up AND name it. cause=7 is ESP_SLEEP_WAKEUP_GPIO
     * and the mask says which row; any other cause is a wake-up we did not
     * ask for. And say HOW MUCH we slept: a night with 0.2 V lost
     * (= ~20 mA) is indistinguishable from a night at 244 µA without this figure. */
    ESP_LOGW(TAG, "reveil apres %lu s de sommeil (cause=%d) — cumul : %lu sommeils, %lu s dormies sur %lu s",
             (unsigned long)(dormi_ms / 1000), (int)esp_sleep_get_wakeup_cause(),
             (unsigned long)s_sommeils, (unsigned long)(s_dormi_ms / 1000),
             (unsigned long)(esp_timer_get_time() / 1000000));

    if (etait_connecte) tud_connect();

    /* The order is the heart of the fix, each step has its reason:
     *
     * 1. Radio first (~7 ms): everything that follows transmits, and a
     *    transmit on a powered-off chip is a lost report — the right half
     *    sends directly, the left half via excursion.
     * 2. Capture: the key that woke the board is pressed RIGHT NOW;
     *    recreating the driver takes tens of ms, a brief keystroke would be
     *    released before that. It also stamps activity, otherwise the
     *    keyboard loop would send the board back to sleep 10 ms later.
     * 3. Transmit the press RIGHT AWAY (left half): the report buffer is
     *    unique, a release published afterward would overwrite it before it is sent.
     * 4. Recreate the driver, then give it 10 ms for a first scan.
     * 5. Reconcile: if it said nothing, the key was released in the meantime
     *    and it will never say so — publish and transmit the release, otherwise
     *    it stays stuck until the next event from this half. */
    veille_hooks_reveiller();     /* reverse order: the radio (~5 ms) is up BEFORE the capture, which transmits */
#if CONFIG_KASE_VEILLE_DIAG
    ESP_LOGW(TAG, "chrono sortie : sommeil -> capture %lld us ; lignes a la sortie=0x%X ; broches du reveil=0x%llX",
             (long long)(esp_timer_get_time() - t_sorti), (unsigned)lignes_sortie, (unsigned long long)masque_reveil);
#endif
    matrix_wake_capture();
    /* First edge in the contact's BOUNCE: the 2-pass capture (1 ms) then
     * reads 0 keys, the board concludes "ghost", goes back to sleep for 10-50 ms and
     * wakes up again on the key still held — seen twice on the bench on
     * 2026-09-13. A brief tap released during this re-sleep is LOST (a
     * release does not wake the board). If the wake-up does come from a GPIO and the
     * capture is empty, we re-read ONCE 5 ms later before deciding: a
     * real glitch gives two empty captures (filter intact), a real press is
     * caught without a double wake-up. */
    if (!matrix_wake_had_keys() && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        esp_rom_delay_us(5000);
        matrix_wake_capture();
    }
#if CONFIG_KASE_VEILLE_DIAG
    /* BENCH 2026-09-16 (left half): the waking key is seen neither by the two
     * captures nor by the driver — 5 presses, 4 seen, the first is missing. Either the
     * wake-up arrives ~150 ms after the press (key already released), or the
     * line reading is wrong during the first ms after wake-up
     * (the right half reads 7 ms later, after its radio, and almost always
     * captures). We re-read every 10 ms for 150 ms and note AT WHAT
     * DELAY a key appears: "never" = late wake-up or glitch;
     * "at 10-20 ms" = wrong early reading; "at 100 ms+" = the next press. */
    if (!matrix_wake_had_keys() && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
        int trouve_ms = -1;
        for (int d = 10; d <= 150; d += 10) {
            esp_rom_delay_us(10000);
            matrix_wake_capture();
            if (matrix_wake_had_keys()) { trouve_ms = d + 6; break; }
        }
        ESP_LOGW(TAG, "capture vide au reveil : touche %s", trouve_ms < 0 ? "JAMAIS vue en 156 ms" : "vue plus tard");
        if (trouve_ms >= 0) ESP_LOGW(TAG, "  apparue a +%d ms apres le reveil", trouve_ms);
    }
#endif
    matrix_disarm_key_wake();
    matrix_setup();
    /* WARNING: no vTaskDelay(1) here: a bare tick waits until the NEXT
     * tick boundary, between ~0 and 10 ms — not "10 ms". When the phase
     * landed badly, the driver had not finished its debounce, its silence
     * passed for a release, and a HELD key was released to the dongle
     * 20 ms after wake-up (Super held -> Super tap -> launcher -> Super+F
     * lost, bench 2026-09-13). We wait for its first event, or the grace period
     * derived from its debounce (wake_grace.h). */
    matrix_wake_wait_first_scan();
    if (matrix_wake_reconcile()) {
    }
    ESP_LOGI(TAG, "reveil");
}

void veille_profonde_entrer(void)
{
    ESP_LOGW(TAG, "deep sleep — le reveil sera un redemarrage");

    veille_hooks_dormir();        /* idempotent: coming from light sleep, the radio is already asleep */
    rtc_matrix_deinit();

    /* Same electrical wiring as the light wake-up: COL -> switch ->
     * diode -> ROW, so we hold all columns HIGH and wake up on
     * whichever row rises. No scanning is needed, and
     * that's what makes the sub-50 µA target reachable — the ULP alone would cost
     * 170 µA.
     *
     * In deep sleep the digital domain is powered off: without an RTC hold the
     * columns would fall back down and no key could ever wake the
     * board. rtc_gpio_hold_en freezes the pin as it currently is. */
    for (int i = 0; i < MATRIX_COLS; i++) {
        rtc_gpio_init(s_cols[i]);
        rtc_gpio_set_direction(s_cols[i], RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(s_cols[i], 1);
        rtc_gpio_hold_en(s_cols[i]);
    }

    uint64_t masque = 0;
    for (int i = 0; i < MATRIX_ROWS; i++) {
        rtc_gpio_init(s_rows[i]);
        rtc_gpio_set_direction(s_rows[i], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(s_rows[i]);
        rtc_gpio_pulldown_en(s_rows[i]);
        masque |= 1ULL << s_rows[i];
    }

    esp_sleep_enable_ext1_wakeup_io(masque, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();       /* never returns: the wake-up will reboot */
}


static uint32_t s_seuil_legere_ms = (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u;
uint32_t veille_seuil_legere_ms(void) { return s_seuil_legere_ms; }
void     veille_seuil_legere_set(uint32_t ms) { s_seuil_legere_ms = ms; }

void veille_pas(uint32_t inactif_ms, bool bloque)
{
    /* Post-wake-up grace period (veille.h): the key that woke the board may
     * not have been seen yet (slow pre-contact, empty capture); we give
     * the recreated driver time to see it and transmit it before sleeping. */
    if (veille_en_grace((uint32_t)(esp_timer_get_time() / 1000), s_dernier_reveil_ms, VEILLE_GRACE_REVEIL_MS))
        return;
    veille_t niveau = veille_niveau(inactif_ms, bloque,
                                    veille_seuil_legere_ms(),
                                    (uint32_t)CONFIG_KASE_VEILLE_PROFONDE_S * 1000u);

#if CONFIG_KASE_HALF_LINK_TX
    /* Right half: if USB has been mounted, DEEP rather than LIGHT.
     *
     * In light sleep the USB PHY clocks are frozen; the ESP-IDF doc
     * (USB Serial/JTAG Console, "Sleep Mode Considerations") warns that
     * the host may declare the device in error, that it may not
     * re-enumerate it on exit, and that ESP-IDF does not refuse entering sleep
     * with the cable plugged in. Found on the bench on 2026-09-11: the right half plugged in
     * would fall asleep then never wake up again, only a reset would bring it back.
     *
     * There is no good presence signal without a VBUS bridge: tud_mounted()
     * stays true after a hot unplug, tud_ready() follows the host's
     * autosuspend (a CDC nobody opens is suspended within 2 s). So we
     * work around it: deep sleep turns off the PHY cleanly — normal disconnect and
     * reconnect, says the same doc — and its EXT1 wake-up is a
     * REBOOT, which resets tud_mounted() to false. After a hot
     * unplug, a single wake-up costs 700 ms, then the board returns to light sleep
     * on its own. Graceful, self-healing degradation, rather than a
     * dead keyboard or a drained battery.
     *
     * The left half is not affected: HID keyboard, the host does not suspend it,
     * it never sleeps while plugged in. */
    if (niveau == VEILLE_LEGERE && tud_mounted()) {
        ESP_LOGW(TAG, "USB monte : sommeil profond plutot que leger (PHY USB)");
        niveau = VEILLE_PROFONDE;
    }
#endif

    switch (niveau) {
    case VEILLE_PROFONDE: veille_profonde_entrer(); break;   /* ne revient pas */
    case VEILLE_LEGERE:   veille_legere_entrer();   break;
    case VEILLE_AUCUNE:   break;
    }
}
