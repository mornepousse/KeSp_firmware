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
#define TAG_MATRIX "MATRIX"
/* Define pins, notice that:
 * GPIO6-11 are usually used for SPI flash
 * GPIO34-39 can only be set as input mode and do not have software pullup or pulldown functions.
 * GPIOS 0,2,4,12-15,25-27,32-39 Can be used as RTC GPIOS as well (please read about power management in ReadMe)
 */
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

//used for debouncing
static uint32_t millis() {
	return esp_timer_get_time() / 1000;
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

		ESP_LOGI(TAG_MATRIX,"%d is level %d", MATRIX_COLS_PINS[col], gpio_get_level(MATRIX_COLS_PINS[col]));
	}

	// Initializing rows
	for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

		esp_rom_gpio_pad_select_gpio(MATRIX_ROWS_PINS[row]);
		gpio_set_direction(MATRIX_ROWS_PINS[row], GPIO_MODE_INPUT_OUTPUT);
		gpio_set_drive_capability(MATRIX_ROWS_PINS[row], GPIO_DRIVE_CAP_0);
		gpio_set_level(MATRIX_ROWS_PINS[row], 0);

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

		/* Give other tasks a chance to run; avoids hogging CPU in case of slow column reads or heavy logging */
		//vTaskDelay(0);
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