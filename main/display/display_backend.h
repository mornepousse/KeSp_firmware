/* Display backend interface — each display type implements these callbacks.
   status_display.c calls them without knowing which backend is active. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "display_types.h"

typedef struct {
    bool (*init)(const display_hw_config_t *cfg);   /* Initialize hardware + LVGL */
    void (*update_layer)(void);                     /* Update layer name display */
    void (*update)(void);                           /* Periodic UI refresh */
    void (*refresh_all)(void);                      /* Full UI rebuild */
    void (*sleep)(void);                            /* Enter sleep mode */
    void (*wake)(void);                             /* Exit sleep mode */
    void (*notify_mouse)(void);                     /* Mouse activity notification */
    void (*show_dfu)(void);                         /* Show DFU mode screen */
} display_backend_t;

/* Backend registration — called by board-specific init */
void display_set_backend(const display_backend_t *backend);

/* Get current backend (NULL if none set) */
const display_backend_t *display_get_backend(void);
