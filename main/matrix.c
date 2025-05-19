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
#define TAG_MATRIX "MATRIX"
/* Define pins, notice that:
 * GPIO6-11 are usually used for SPI flash
 * GPIO34-39 can only be set as input mode and do not have software pullup or pulldown functions.
 * GPIOS 0,2,4,12-15,25-27,32-39 Can be used as RTC GPIOS as well (please read about power management in ReadMe)
 */
//const gpio_num_t MATRIX_ROWS_PINS[] = { GPIO_NUM_37, GPIO_NUM_40, GPIO_NUM_39, GPIO_NUM_36, GPIO_NUM_3 };
const gpio_num_t MATRIX_ROWS_PINS[] = { GPIO_NUM_3, 
										GPIO_NUM_36, 
										GPIO_NUM_39, 
										GPIO_NUM_40, 
										GPIO_NUM_37 };
const gpio_num_t MATRIX_COLS_PINS[] = { GPIO_NUM_21, //0
										GPIO_NUM_47, //1
										GPIO_NUM_48, //2
										GPIO_NUM_45, //3
										GPIO_NUM_35, //4
                                        GPIO_NUM_38, //5
										
										GPIO_NUM_9,  //7
										GPIO_NUM_10, //8
										GPIO_NUM_11, //9
                                        GPIO_NUM_12, //10
                                        GPIO_NUM_13, //11
										GPIO_NUM_14,//12
										GPIO_NUM_46, };//6

// matrix states
uint8_t MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t PREV_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t SLAVE_MATRIX_STATE[MATRIX_ROWS][MATRIX_COLS] = { 0 };
uint8_t keycodes[6] = { 0,0,0,0,0,0 };
uint8_t last_layer = 0;
uint32_t lastDebounceTime = 0;

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
// Scanning the matrix for input
void scan_matrix(void) {
#ifdef COL2ROW
	// Setting column pin as low, and checking if the input of a row pin changes.

	for (uint8_t col = 0; col < MATRIX_COLS; col++) {
		gpio_set_level(MATRIX_COLS_PINS[col], 1);
		for (uint8_t row = 0; row < MATRIX_ROWS; row++) {

			curState = gpio_get_level(MATRIX_ROWS_PINS[row]);
			if (PREV_MATRIX_STATE[row][col] != curState) {
				DEBOUNCE_MATRIX[row][col] = millis();
			}
			PREV_MATRIX_STATE[row][col] = curState;
			if ((millis() - DEBOUNCE_MATRIX[row][col]) > DEBOUNCE) {

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
					
    				

					//ESP_LOGI(GPIO_TAG, " 0: %d - %d, 1: %d - %d, 2: %d - %d, 3: %d - %d, 4: %d- %d, 5: %d - %d", 
					//current_press_row[0], current_press_col[0], current_press_row[1], current_press_col[1], current_press_row[2], current_press_col[2], 
					//current_press_row[3], current_press_col[3], current_press_row[4], current_press_col[4], current_press_row[5], current_press_col[5]);
					//tud_hid_keyboard_report(1, 0, keycodes);
					stat_matrix_changed = 1;
					//ESP_LOGI(GPIO_TAG, "Row: %d, Col: %d, State: %d, K : %d %d %d %d %d %d ", row, col, curState, keycodes[0], keycodes[1], keycodes[2], keycodes[3], keycodes[4], keycodes[5]);
				}

			}
		}
		gpio_set_level(MATRIX_COLS_PINS[col], 0);
	}

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

}