/**
 * @file round_ui.h
 * @brief Header for modern circular UI for GC9A01 240x240 round display
 */

#ifndef ROUND_UI_H
#define ROUND_UI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the round UI
 * 
 * Creates all UI elements with dark theme for round display
 */
void round_ui_init(void);

/**
 * @brief Update the layer name display
 */
void round_ui_update_layer(void);

/**
 * @brief Periodic UI update (connection status, mouse indicator)
 */
void round_ui_update(void);

/**
 * @brief Put UI to sleep (blank screen)
 */
void round_ui_sleep(void);

/**
 * @brief Wake up UI from sleep
 */
void round_ui_wake(void);

/**
 * @brief Force refresh all UI elements
 */
void round_ui_refresh_all(void);

/**
 * @brief Notify UI of mouse activity
 */
void round_ui_notify_mouse(void);

/**
 * @brief Notify UI of a keypress (for KPM calculation)
 */
void round_ui_notify_keypress(void);

/**
 * @brief Update NRF debug info display
 */
void round_ui_update_nrf_debug(uint32_t pps, uint8_t status, bool spi_ok, uint8_t rpd, uint8_t last_byte, uint8_t mode);

/**
 * @brief Show splash screen with text
 */
void round_ui_show_splash(const char *text);

/**
 * @brief Show DFU mode screen
 */
void round_ui_show_dfu(void);

/**
 * @brief Check if UI is initialized
 */
bool round_ui_is_initialized(void);

/**
 * @brief Check if UI is sleeping
 */
bool round_ui_is_sleeping(void);

#ifdef __cplusplus
}
#endif

#endif /* ROUND_UI_H */
