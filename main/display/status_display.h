#pragma once
#include <stdint.h>
#include <stdbool.h>

void status_display_start(void);
void status_display_refresh_all(void);
void status_display_sleep(void);
void status_display_wake(void);
void status_display_update(void);
void status_display_update_layer_name(void);
void draw_separator_line(void); 
void status_display_show_DFU_prog(void);
void status_display_notify_mouse_activity(void);

/* Force-disables the display subsystem (used when heap integrity fails) */
void status_display_force_disable(void);
