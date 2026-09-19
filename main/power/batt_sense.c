/* Jauge batterie — lecture ADC de VBAT_SENSE. Voir batt_sense.h / batt_calc.h.
 *
 * Matériel : GPIO13 = ADC2_CH2, pont 1 MΩ / 1 MΩ + 100 nF. ADC2 est utilisable
 * parce qu'il n'y a pas de WiFi sur le Niphargus (nRF24 seul) — c'est la seule
 * raison, et elle est structurelle. Le pont à 1 MΩ est haute impédance : c'est
 * le 100 nF qui fournit la charge à l'échantillonneur, d'où un petit délai avant
 * la rafale et une moyenne de 8 lectures.
 *
 * Calibration : la courbe d'usine de l'ESP32-S3 (adc_cali_curve_fitting) quand
 * elle est disponible ; sinon une conversion linéaire approchée, annoncée au
 * journal — une jauge à ±0,1 V reste utile, une jauge muette non. */
#include "batt_sense.h"
#include "batt_calc.h"
#include "board.h"
#if CONFIG_KASE_VEILLE
#include "veille_task.h"   /* hook jauge : une mesure au réveil */
#endif
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "batt";

#define BATT_PERIOD_US  (10ULL * 1000 * 1000)
#define BATT_SAMPLES    8

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t         s_cali;      /* NULL = pas de calibration */
static adc_unit_t                s_unit_id;
static adc_channel_t             s_chan;
static esp_timer_handle_t        s_timer;

static volatile uint8_t  s_dv;        /* dernière tension valide, 0 = inconnue */
static volatile uint8_t  s_chg;       /* batt_chg_t */
static volatile uint32_t s_last_ms;   /* horodatage de la dernière mesure valide */
static batt_state_t      s_state;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t read_mv_once(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_unit, s_chan, &raw) != ESP_OK) return 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return (uint32_t)mv;
    /* Sans calibration : 12 bits, atténuation 12 dB ≈ 0..3100 mV. Approché. */
    return (uint32_t)raw * 3100u / 4095u;
}

void batt_sense_sample_now(void)
{
    if (!s_unit) return;
    uint32_t s[BATT_SAMPLES];
    esp_rom_delay_us(200);                          /* le 100 nF se stabilise */
    for (int i = 0; i < BATT_SAMPLES; i++) {
        s[i] = read_mv_once();
        esp_rom_delay_us(100);
    }
    uint32_t mv = batt_mv_from_samples(s, BATT_SAMPLES);   /* 0 = rejeté */
    uint8_t  dv = batt_mv_to_dv_batt(mv);
    uint32_t t  = now_ms();
    s_chg = (uint8_t)batt_state_step(&s_state, mv, t);
    if (dv) { s_dv = dv; s_last_ms = t; }
    else    { s_dv = 0; }
    ESP_LOGD(TAG, "%u mV (adc %u) -> %u dV, etat %u", (unsigned)mv, (unsigned)s[0], dv, s_chg);
}

static void timer_cb(void *arg) { (void)arg; batt_sense_sample_now(); }

void batt_sense_init(void)
{
#if CONFIG_KASE_VEILLE
    /* Une mesure au réveil : le timer était gelé pendant le sommeil. */
    static const veille_hook_t hook = { "jauge", NULL, batt_sense_sample_now };
    veille_hook_enregistrer(&hook);
#endif
    if (adc_oneshot_io_to_channel(BOARD_VBAT_SENSE_GPIO, &s_unit_id, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d n'est pas une entree ADC — jauge desactivee", BOARD_VBAT_SENSE_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = s_unit_id, .ulp_mode = ADC_ULP_MODE_DISABLE };
    if (adc_oneshot_new_unit(&ucfg, &s_unit) != ESP_OK) {
        ESP_LOGE(TAG, "ADC%d init KO — jauge desactivee", (int)s_unit_id + 1);
        s_unit = NULL;
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_unit, s_chan, &ccfg);

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = s_unit_id, .chan = s_chan,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "pas de calibration ADC : lecture approchee (+/-0,1 V)");
    }

    batt_sense_sample_now();
    const esp_timer_create_args_t a = { .callback = timer_cb, .name = "batt" };
    if (esp_timer_create(&a, &s_timer) == ESP_OK)
        esp_timer_start_periodic(s_timer, BATT_PERIOD_US);
    ESP_LOGI(TAG, "jauge : %u dV (etat %u), ADC%d ch%d, mesure toutes les 10 s",
             s_dv, s_chg, (int)s_unit_id + 1, (int)s_chan);
}

uint8_t  batt_sense_dv(void)       { return s_dv; }
uint8_t  batt_sense_charging(void) { return s_chg; }
uint32_t batt_sense_age_ms(void)   { return s_last_ms ? (uint32_t)(now_ms() - s_last_ms) : 0xFFFFFFFFu; }
