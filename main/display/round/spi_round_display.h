/**
 * @file spi_round_display.h
 * @brief Driver for GC9A01 round SPI display (240x240)
 */

#ifndef SPI_ROUND_DISPLAY_H
#define SPI_ROUND_DISPLAY_H

#include "display_types.h"
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SPI round display (GC9A01)
 * @param cfg Display hardware configuration
 * @return true if initialization successful, false otherwise
 */
bool spi_display_init(const display_hw_config_t *cfg);

/**
 * @brief Get the LVGL display handle
 * @return Pointer to LVGL display, or NULL if not initialized
 */
lv_disp_t *spi_display_get_disp(void);

/**
 * @brief Check if display is available
 * @return true if display is initialized and working
 */
bool spi_display_is_available(void);

/**
 * @brief Clear the screen
 */
void spi_display_clear(void);

/**
 * @brief Display test text at center
 * @param text Text to display
 */
void spi_display_test_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* SPI_ROUND_DISPLAY_H */
