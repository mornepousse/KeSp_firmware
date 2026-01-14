/**
 * @file display_types.h
 * @brief Common types for display drivers
 */

#ifndef DISPLAY_TYPES_H
#define DISPLAY_TYPES_H

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Display bus type */
typedef enum {
    DISPLAY_BUS_I2C = 0,
    DISPLAY_BUS_SPI = 1,
} display_bus_type_t;

/** Hardware configuration for display */
typedef struct {
    display_bus_type_t bus_type;
    uint16_t width;
    uint16_t height;
    gpio_num_t reset_pin;
    uint32_t pixel_clock_hz;
    
    union {
        struct {
            int host;               /* I2C port number (I2C_NUM_0 or I2C_NUM_1) */
            gpio_num_t sda;
            gpio_num_t scl;
            uint8_t address;
            bool enable_internal_pullups;
        } i2c;
        
        struct {
            spi_host_device_t host;
            gpio_num_t sclk;
            gpio_num_t mosi;
            gpio_num_t cs;
            gpio_num_t dc;
            gpio_num_t backlight;
        } spi;
    };
} display_hw_config_t;

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_TYPES_H */
