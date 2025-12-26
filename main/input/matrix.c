#include "matrix.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "keyboard_config.h"
#include "keymap.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stddef.h>
/* Forward declaration to avoid depending on header include path during build */
int cpu_time_measure_period(unsigned period_ms, char *out, size_t len);
#include "esp_heap_caps.h"
#define TAG_MATRIX "MATRIX"
/* Define pins, notice that:
 * GPIO6-11 are usually used for SPI flash
 * GPIO34-39 can only be set as input mode and do not have software pullup or pulldown functions.
 * GPIOS 0,2,4,12-15,25-27,32-39 Can be used as RTC GPIOS as well (please read about power management in ReadMe)
 */

/* Diagnostic: threshold for triggering a diagnostic dump (us). Lowered to 1 ms for aggressive capture */
#ifndef MATRIX_COL_DUMP_THRESHOLD_US
#define MATRIX_COL_DUMP_THRESHOLD_US 1000
#endif

/* Rate limit diagnostic dumps to at most once every DIAG_DUMP_INTERVAL_MS */
#ifndef DIAG_DUMP_INTERVAL_MS
#define DIAG_DUMP_INTERVAL_MS 5000
#endif

/* Per-pin profiling and summary interval */
#ifndef ROW_SLOW_THRESHOLD_US
#define ROW_SLOW_THRESHOLD_US 200 /* per-row read threshold for slow detection (200 us) */
#endif
#ifndef SLOW_SUMMARY_INTERVAL_MS
#define SLOW_SUMMARY_INTERVAL_MS 10000 /* 10s summary for faster feedback */
#endif

/* Keep scan priority by default (no yields in column loop). Set to 1 to allow yielding between columns */
#ifndef MATRIX_SCAN_ALLOW_YIELD
#define MATRIX_SCAN_ALLOW_YIELD 0
#endif

/* Forward extern task handles for diagnostic reporting */
extern TaskHandle_t keyboard_task_handle;
extern TaskHandle_t nrf24_task_handle;
extern TaskHandle_t status_display_task_handle;

/* Profiling counters (accumulate over runtime) */
static uint32_t col_slow_count[MATRIX_COLS];
static uint32_t row_slow_count[MATRIX_ROWS];
static uint64_t col_total_time_us[MATRIX_COLS];
static uint32_t col_samples[MATRIX_COLS];
static TickType_t last_slow_summary_tick = 0;
const gpio_num_t MATRIX_ROWS_PINS[] = { ROWS0, 
										ROWS1, 
										ROWS2, 
										ROWS3, 
										ROWS4 };
const gpio_num_t MATRIX_COLS_PINS[] = { COLS0, //0
										COLS1, //1
										COLS2, //2
										COLS3, //3
										COLS4, //4
                                        COLS5, //5
										
										COLS6,  //6
										COLS7, //7
										COLS8, //8
                                        COLS9, //9
                                        COLS10, //10
										COLS11,//11
										COLS12, };//12

// matrix states
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t PREV_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t keycodes[6] = { 0,0,0,0,0,0 };
uint8_t last_layer = 0;
uint8_t current_layout = 0;
uint8_t is_layer_changed = 0;
uint32_t lastDebounceTime = 0;
uint32_t last_activity_time_ms = 0;

uint8_t (*matrix_states[])[MATRIX_ROWS][MATRIX_COLS] = { &MATRIX_STATE,
		&SLAVE_MATRIX_STATE, };

//used for debouncing - avoid expensive 64-bit divide in hot path by using RTOS tick count
static inline uint32_t millis() {
    /* xTaskGetTickCount returns TickType_t; multiply by portTICK_PERIOD_MS to get ms.
       This avoids doing a 64-bit division on esp_timer_get_time() inside scan loops. */
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// deinitializing rtc matrix pins on  deep sleep wake up
void rtc_matrix_deinit(void) {

	// Deinitializing columns
	for (uint8_t col = 0; col < MATRIX_COLS; col++) {

		if (rtc_gpio_is_valid_gpio(MATRIX_COLS_PINS[col]) == 1) {
			rtc_gpio_set_level(MATRIX_COLS_PINS[col], 0);
			rtc_gpio_set_direction(MATRIX_COLS_PINS[col],
					RTC_GPIO_MODE_DISABLED);
			gpio_reset_pin(MATRIX_COLS_PINS[col]);
		}
	}

	// Deinitializing rows
	for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

		if (rtc_gpio_is_valid_gpio(MATRIX_ROWS_PINS[row]) == 1) {
			rtc_gpio_set_level(MATRIX_ROWS_PINS[row], 0);
			rtc_gpio_set_direction(MATRIX_ROWS_PINS[row],
					RTC_GPIO_MODE_DISABLED);
			gpio_reset_pin(MATRIX_ROWS_PINS[row]);
		}
	}
}

// Initializing rtc matrix pins for deep sleep wake up
void rtc_matrix_setup(void) {
	uint64_t rtc_mask = 0;

	// Initializing columns
	for (uint8_t col = 0; col < MATRIX_COLS; col++) {

		if (rtc_gpio_is_valid_gpio(MATRIX_COLS_PINS[col]) == 1) {
			rtc_gpio_init((MATRIX_COLS_PINS[col]));
			rtc_gpio_set_direction(MATRIX_COLS_PINS[col],
					RTC_GPIO_MODE_INPUT_OUTPUT);
			rtc_gpio_set_level(MATRIX_COLS_PINS[col], 1);

			ESP_LOGI(TAG_MATRIX,"%d is level %d", MATRIX_COLS_PINS[col], gpio_get_level(MATRIX_COLS_PINS[col]));
		}
	}

	// Initializing rows
	for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

		if (rtc_gpio_is_valid_gpio(MATRIX_ROWS_PINS[row]) == 1) {
			rtc_gpio_init((MATRIX_ROWS_PINS[row]));
			rtc_gpio_set_direction(MATRIX_ROWS_PINS[row],
					RTC_GPIO_MODE_INPUT_OUTPUT);
			rtc_gpio_set_drive_capability(MATRIX_ROWS_PINS[row],
					GPIO_DRIVE_CAP_0);
			rtc_gpio_set_level(MATRIX_ROWS_PINS[row], 0);
			rtc_gpio_wakeup_enable(MATRIX_ROWS_PINS[row], GPIO_INTR_HIGH_LEVEL);
			SET_BIT(rtc_mask, MATRIX_ROWS_PINS[row]);

			ESP_LOGI(TAG_MATRIX,"%d is level %d", MATRIX_ROWS_PINS[row], gpio_get_level(MATRIX_ROWS_PINS[row]));
		}
		esp_sleep_enable_ext1_wakeup(rtc_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
	}
}

// Initializing matrix pins
void matrix_setup(void) {
	
	// Initializing columns
	for (uint8_t col = 0; col < MATRIX_COLS; col++) {

		esp_rom_gpio_pad_select_gpio(MATRIX_COLS_PINS[col]);
		gpio_set_direction(MATRIX_COLS_PINS[col], GPIO_MODE_INPUT_OUTPUT);
		gpio_set_level(MATRIX_COLS_PINS[col], 0);

		/* Warn if column pin is a special/reserved pin (UART/Flash) which may cause issues */
		if (MATRIX_COLS_PINS[col] == GPIO_NUM_1 || MATRIX_COLS_PINS[col] == GPIO_NUM_3 || (MATRIX_COLS_PINS[col] >= GPIO_NUM_6 && MATRIX_COLS_PINS[col] <= GPIO_NUM_11)) {
			ESP_LOGW(TAG_MATRIX, "Column %u uses potentially shared pin %d - consider remapping", (unsigned)col, (int)MATRIX_COLS_PINS[col]);
		}

		ESP_LOGI(TAG_MATRIX,"%d is level %d", MATRIX_COLS_PINS[col], gpio_get_level(MATRIX_COLS_PINS[col]));
	}

	// Initializing rows
	for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

		esp_rom_gpio_pad_select_gpio(MATRIX_ROWS_PINS[row]);
		gpio_set_direction(MATRIX_ROWS_PINS[row], GPIO_MODE_INPUT_OUTPUT);
		gpio_set_drive_capability(MATRIX_ROWS_PINS[row], GPIO_DRIVE_CAP_0);
		gpio_set_level(MATRIX_ROWS_PINS[row], 0);
		/* enable internal pull-down to avoid floating inputs when columns are not driven */
		gpio_set_pull_mode(MATRIX_ROWS_PINS[row], GPIO_PULLDOWN_ONLY);

		ESP_LOGI(TAG_MATRIX,"%d is level %d", MATRIX_ROWS_PINS[row], gpio_get_level(MATRIX_ROWS_PINS[row]));
	}

}

uint8_t curState = 0;
uint32_t DEBOUNCE_MATRIX[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t current_press_row[6] = { 255, 255, 255, 255, 255, 255 };
uint8_t current_press_col[6] = { 255, 255, 255, 255, 255, 255 };
uint8_t current_press_stat[6] = { 0, 0, 0, 0, 0, 0 };
uint8_t stat_matrix_changed = 0; // 1: matrix changed, 0: matrix not changed

// Number of columns to scan per invocation of scan_matrix()
#ifndef SCAN_COLS_PER_ITER
#define SCAN_COLS_PER_ITER 3
#endif
static uint8_t scan_col_index = 0;

// Scanning the matrix for input
void scan_matrix(void) {
    uint64_t t_scan_start = esp_timer_get_time();
#ifdef COL2ROW
	// Setting column pin as low, and checking if the input of a row pin changes.

	for (uint8_t sc = 0; sc < SCAN_COLS_PER_ITER; sc++) {
		uint8_t col = (scan_col_index + sc) % MATRIX_COLS;
		uint64_t tcol_start = esp_timer_get_time();

		gpio_set_level(MATRIX_COLS_PINS[col], 1);
		uint32_t now_col = millis();
		for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

			curState = gpio_get_level(MATRIX_ROWS_PINS[row]);
			if (PREV_MATRIX_STATE[row][col] != curState) {
				DEBOUNCE_MATRIX[row][col] = now_col;
			}
			PREV_MATRIX_STATE[row][col] = curState;
			if ((now_col - DEBOUNCE_MATRIX[row][col]) > DEBOUNCE) {

				if (MATRIX_STATE[row][col] != curState) {
					MATRIX_STATE[row][col] = curState;
					uint8_t index = 0;
					//uint8_t keycodeTMP = default_layouts[current_layout][row][col];
					//uint8_t keycodeTMP = keymaps[current_layout][row][col];

					for (uint8_t i = 0; i < 6; i++)
					{
						if (current_press_col[i] == 255)
						{
							index = i;
							break;
						}
					}
					
					for (uint8_t i = 0; i < 6; i++)
					{
						if (curState == 0 && current_press_col[i] == col && current_press_row[i] == row)
						{
							current_press_col[i] = 255;
							current_press_row[i] = 255;
							break;
						}
					}
					if (curState == 1)
					{
						current_press_col[index] = col;
						current_press_row[index] = row;
						current_press_stat[index] = curState;
					}
					
					stat_matrix_changed = 1;
					last_activity_time_ms = now_col;
					/* Reduced log level to avoid blocking serial output -- enable DEBUG for verbose traces */
					ESP_LOGD(TAG_MATRIX, "Row: %d, Col: %d, State: %d", row, col, curState);
				}

			}
		}
		gpio_set_level(MATRIX_COLS_PINS[col], 0);

		uint64_t tcol_end = esp_timer_get_time();
		uint64_t tcol_dur = tcol_end - tcol_start;
		if (tcol_dur > 2000) {
			ESP_LOGW(TAG_MATRIX, "col %d took %llu us", col, (unsigned long long)tcol_dur);
		}

		/* If a column read is very long, collect diagnostics (rate-limited) */
		if (tcol_dur > MATRIX_COL_DUMP_THRESHOLD_US) {
			static TickType_t last_diag_tick = 0;
			TickType_t now_tick = xTaskGetTickCount();
			if (last_diag_tick == 0 || (now_tick - last_diag_tick) > pdMS_TO_TICKS(DIAG_DUMP_INTERVAL_MS)) {
				last_diag_tick = now_tick;
				ESP_LOGW(TAG_MATRIX, "Long column read (col %d): %llu us - collecting diagnostics", col, (unsigned long long)tcol_dur);

				/* Attempt runtime CPU usage snapshot (may fail if trace facility not enabled) */
				char buf[512];
				int r = cpu_time_measure_period(200, buf, sizeof(buf));
				if (r == 0) {
					ESP_LOGW(TAG_MATRIX, "CPU usage snapshot:\n%s", buf);
				} else {
					ESP_LOGW(TAG_MATRIX, "cpu_time_measure_period failed (enable FreeRTOS trace): %d", r);
				}

				/* Stack high-water marks for key tasks */
				if (keyboard_task_handle) {
					UBaseType_t words_left = uxTaskGetStackHighWaterMark(keyboard_task_handle);
					size_t bytes_left = words_left * sizeof(StackType_t);
					ESP_LOGW(TAG_MATRIX, "STACK: keyboard approx %u bytes free", (unsigned)bytes_left);
				}
				if (nrf24_task_handle) {
					UBaseType_t words_left = uxTaskGetStackHighWaterMark(nrf24_task_handle);
					size_t bytes_left = words_left * sizeof(StackType_t);
					ESP_LOGW(TAG_MATRIX, "STACK: nrf24 approx %u bytes free", (unsigned)bytes_left);
				}
				if (status_display_task_handle) {
					UBaseType_t words_left = uxTaskGetStackHighWaterMark(status_display_task_handle);
					size_t bytes_left = words_left * sizeof(StackType_t);
					ESP_LOGW(TAG_MATRIX, "STACK: status_display approx %u bytes free", (unsigned)bytes_left);
				}

				/* Heap diagnostics */
				size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
				size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
				ESP_LOGW(TAG_MATRIX, "HEAP: free8=%u largest8=%u", (unsigned)free8, (unsigned)largest8);
			}
		}

		/* yield to let idle and other tasks run (prevents WDT if a column read takes long) */
		vTaskDelay(0);

		/* Give other tasks a chance to run; avoids hogging CPU in case of slow column reads or heavy logging */
		vTaskDelay(0);
	}

	/* advance the next column index for next invocation */
	scan_col_index = (scan_col_index + SCAN_COLS_PER_ITER) % MATRIX_COLS;

#endif
#ifdef ROW2COL
	// Setting row pin as low, and checking if the input of a column pin changes.aaa============================================================
	for(uint8_t row=0; row < MATRIX_ROWS; row++) {
		gpio_set_level(MATRIX_ROWS_PINS[row], 1);

		for(uint8_t col=0; col <MATRIX_COLS; col++) {

			curState = gpio_get_level(MATRIX_COLS_PINS[col]);
			if( PREV_MATRIX_STATE[row][col] != curState) {
				DEBOUNCE_MATRIX[row][col] = millis();
			}
			PREV_MATRIX_STATE[row][col] = curState;
			if( (millis() - DEBOUNCE_MATRIX[row][col]) > DEBOUNCE) {

				if( MATRIX_STATE[row][col] != curState) {
					MATRIX_STATE[row][col] = curState;
					ESP_LOGI(TAG_MATRIX, "Row: %d, Col: %d, State: %d", row, col, curState);
				}

			}
		}
		gpio_set_level(MATRIX_ROWS_PINS[row], 0);
	}
#endif

    uint64_t t_scan_end = esp_timer_get_time();
    uint64_t t_scan_dur = t_scan_end - t_scan_start;
    if (t_scan_dur > 2000) {
        ESP_LOGW(TAG_MATRIX, "scan_matrix duration %llu us", (unsigned long long)t_scan_dur);
    }
}
void layer_changed(void)
{
    is_layer_changed = 1;
}

uint32_t get_last_activity_time_ms(void)
{
	return last_activity_time_ms;
}


#if MATRIX_IRQ_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Task handle exported from keyboard manager -- used to notify keyboard task from ISR */
extern TaskHandle_t keyboard_task_handle;

/* last interrupt time (ms) for debounce in ISR */
volatile uint32_t last_row_int_time = 0;

/* Full scan: scan all columns immediately (used on IRQ wake) */
void scan_matrix_full_once(void)
{
    uint64_t t_scan_start = esp_timer_get_time();
#ifdef COL2ROW
    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        uint64_t tcol_start = esp_timer_get_time();

        gpio_set_level(MATRIX_COLS_PINS[col], 1);
        uint32_t now_col = millis();
        for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

            curState = gpio_get_level(MATRIX_ROWS_PINS[row]);
            if (PREV_MATRIX_STATE[row][col] != curState) {
                DEBOUNCE_MATRIX[row][col] = now_col;
            }
            PREV_MATRIX_STATE[row][col] = curState;
            if ((now_col - DEBOUNCE_MATRIX[row][col]) > DEBOUNCE) {

                if (MATRIX_STATE[row][col] != curState) {
                    MATRIX_STATE[row][col] = curState;
                    uint8_t index = 0;

                    for (uint8_t i = 0; i < 6; i++)
                    {
                        if (current_press_col[i] == 255)
                        {
                            index = i;
                            break;
                        }
                    }

                    for (uint8_t i = 0; i < 6; i++)
                    {
                        if (curState == 0 && current_press_col[i] == col && current_press_row[i] == row)
                        {
                            current_press_col[i] = 255;
                            current_press_row[i] = 255;
                            break;
                        }
                    }
                    if (curState == 1)
                    {
                        current_press_col[index] = col;
                        current_press_row[index] = row;
                        current_press_stat[index] = curState;
                    }

                    stat_matrix_changed = 1;
                    last_activity_time_ms = now_col;
                    ESP_LOGD(TAG_MATRIX, "Row: %d, Col: %d, State: %d", row, col, curState);
                }

            }
        }
        gpio_set_level(MATRIX_COLS_PINS[col], 0);

        uint64_t tcol_end = esp_timer_get_time();
        uint64_t tcol_dur = tcol_end - tcol_start;
        if (tcol_dur > 2000) {
            ESP_LOGW(TAG_MATRIX, "col %d took %llu us", col, (unsigned long long)tcol_dur);
        }

        /* Always record per-column stats */
        col_samples[col]++;
        col_total_time_us[col] += tcol_dur;

        /* If a column read exceeds threshold, collect diagnostics (rate-limited) and log stack/heap; also profile per-row reads */
        if (tcol_dur > MATRIX_COL_DUMP_THRESHOLD_US) {
            col_slow_count[col]++;
            static TickType_t last_diag_tick = 0;
            TickType_t now_tick = xTaskGetTickCount();
            if (last_diag_tick == 0 || (now_tick - last_diag_tick) > pdMS_TO_TICKS(DIAG_DUMP_INTERVAL_MS)) {
                last_diag_tick = now_tick;
                ESP_LOGW(TAG_MATRIX, "Long column read (col %d): %llu us - collecting diagnostics", col, (unsigned long long)tcol_dur);

                /* CPU snapshot */
                char buf[512];
                int r = cpu_time_measure_period(200, buf, sizeof(buf));
                if (r == 0) {
                    ESP_LOGW(TAG_MATRIX, "CPU usage snapshot:\n%s", buf);
                } else {
                    ESP_LOGW(TAG_MATRIX, "cpu_time_measure_period failed: %d", r);
                }

                /* Per-row profiling (run only during diagnostic) */
                for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
                    uint64_t t0 = esp_timer_get_time();
                    int level = gpio_get_level(MATRIX_ROWS_PINS[row]); (void)level;
                    uint64_t t1 = esp_timer_get_time();
                    uint64_t rdur = (t1 > t0) ? (t1 - t0) : 0;
                    if (rdur > ROW_SLOW_THRESHOLD_US) {
                        row_slow_count[row]++;
                        ESP_LOGW(TAG_MATRIX, "Slow row read: row=%d pin=%d dur=%llu us", row, (int)MATRIX_ROWS_PINS[row], (unsigned long long)rdur);
                    }
                }

                if (keyboard_task_handle) {
                    UBaseType_t words_left = uxTaskGetStackHighWaterMark(keyboard_task_handle);
                    ESP_LOGW(TAG_MATRIX, "STACK: keyboard approx %u bytes free", (unsigned)(words_left * sizeof(StackType_t)));
                }
                if (nrf24_task_handle) {
                    UBaseType_t words_left = uxTaskGetStackHighWaterMark(nrf24_task_handle);
                    ESP_LOGW(TAG_MATRIX, "STACK: nrf24 approx %u bytes free", (unsigned)(words_left * sizeof(StackType_t)));
                }
                if (status_display_task_handle) {
                    UBaseType_t words_left = uxTaskGetStackHighWaterMark(status_display_task_handle);
                    ESP_LOGW(TAG_MATRIX, "STACK: status_display approx %u bytes free", (unsigned)(words_left * sizeof(StackType_t)));
                }

                size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                size_t largest8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                ESP_LOGW(TAG_MATRIX, "HEAP: free8=%u largest8=%u", (unsigned)free8, (unsigned)largest8);
            }
        }

        /* Optionally yield between columns to let other tasks run; disabled by default to preserve scan priority */
#if MATRIX_SCAN_ALLOW_YIELD
        vTaskDelay(0);
#endif

        /* Periodic slow-summary log (rate-limited) */
        if (last_slow_summary_tick == 0 || (xTaskGetTickCount() - last_slow_summary_tick) > pdMS_TO_TICKS(SLOW_SUMMARY_INTERVAL_MS)) {
            last_slow_summary_tick = xTaskGetTickCount();
            /* compute top slow columns and rows */
            uint8_t best_col = 0; uint32_t best_col_count = 0;
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if (col_slow_count[c] > best_col_count) { best_col_count = col_slow_count[c]; best_col = c; }
            }
            uint8_t best_row = 0; uint32_t best_row_count = 0;
            for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
                if (row_slow_count[r] > best_row_count) { best_row_count = row_slow_count[r]; best_row = r; }
            }
// `            ESP_LOGI(TAG_MATRIX, "Slow summary: top_col=%u pin=%d count=%u top_row=%u pin=%d count=%u avg_col_us=%llu", best_col, (int)MATRIX_COLS_PINS[best_col], (unsigned)best_col_count, best_row, (int)MATRIX_ROWS_PINS[best_row], (unsigned)best_row_count, (unsigned long long)(col_samples[best_col]?col_total_time_us[best_col]/col_samples[best_col]:0));
        }
    }
#endif

#ifdef ROW2COL
    for(uint8_t row=0; row < MATRIX_ROWS; row++) {
        gpio_set_level(MATRIX_ROWS_PINS[row], 1);

        for(uint8_t col=0; col <MATRIX_COLS; col++) {

            curState = gpio_get_level(MATRIX_COLS_PINS[col]);
            if( PREV_MATRIX_STATE[row][col] != curState) {
                DEBOUNCE_MATRIX[row][col] = millis();
            }
            PREV_MATRIX_STATE[row][col] = curState;
            if( (millis() - DEBOUNCE_MATRIX[row][col]) > DEBOUNCE) {

                if( MATRIX_STATE[row][col] != curState) {
                    MATRIX_STATE[row][col] = curState;
                    ESP_LOGI(TAG_MATRIX, "Row: %d, Col: %d, State: %d", row, col, curState);
                }

            }
        }
        gpio_set_level(MATRIX_ROWS_PINS[row], 0);
    }
#endif

    uint64_t t_scan_end = esp_timer_get_time();
    uint64_t t_scan_dur = t_scan_end - t_scan_start;
    if (t_scan_dur > 2000) {
        ESP_LOGW(TAG_MATRIX, "scan_matrix_full_once duration %llu us", (unsigned long long)t_scan_dur);
    }
}

/* Lightweight ISR handler: only debounce using RTOS tick and notify keyboard task */
void IRAM_ATTR row_isr(void* arg)
{
    BaseType_t woke = pdFALSE;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((now_ms - last_row_int_time) < DEBOUNCE) {
        return;
    }
    last_row_int_time = now_ms;
    if (keyboard_task_handle != NULL) {
        vTaskNotifyGiveFromISR(keyboard_task_handle, &woke);
        portYIELD_FROM_ISR(woke);
    }
}

/* Setup GPIO ISR for matrix rows */
void matrix_irq_setup(void)
{
    ESP_LOGI(TAG_MATRIX, "Matrix IRQ setup");
    gpio_install_isr_service(0);
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        gpio_set_intr_type(MATRIX_ROWS_PINS[row], GPIO_INTR_ANYEDGE);
        gpio_isr_handler_add(MATRIX_ROWS_PINS[row], row_isr, (void*)(uintptr_t)row);
    }
}

/* Teardown ISR handlers */
void matrix_irq_deinit(void)
{
    ESP_LOGI(TAG_MATRIX, "Matrix IRQ deinit");
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        gpio_isr_handler_remove(MATRIX_ROWS_PINS[row]);
        gpio_set_intr_type(MATRIX_ROWS_PINS[row], GPIO_INTR_DISABLE);
    }
    gpio_uninstall_isr_service();
}
#endif // MATRIX_IRQ_ENABLED