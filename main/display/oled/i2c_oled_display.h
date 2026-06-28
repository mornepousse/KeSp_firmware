#pragma once
#include <stdbool.h>
#include "display_types.h"

extern bool display_available;

void display_set_hw_config(const display_hw_config_t *config);
const display_hw_config_t *display_get_hw_config(void);

void init_display(void);
void display_clear_screen(void);

/* Turn the SSD1306 panel on/off (true=on). Used by the OLED sleep/wake path so
 * the screen goes dark in light-sleep regardless of LVGL flushing. */
void i2c_oled_display_power(bool on);
