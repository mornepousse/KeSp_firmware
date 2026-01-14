#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/i2c.h"

typedef enum {
    DISPLAY_BUS_I2C = 0,
    DISPLAY_BUS_SPI = 1,
} display_bus_type_t;

typedef struct {
    display_bus_type_t bus_type;
    int width;
    int height;
    int pixel_clock_hz;
    gpio_num_t reset_pin;
    struct {
        i2c_port_t host;
        gpio_num_t sda;
        gpio_num_t scl;
        uint8_t address;
        bool enable_internal_pullups;
    } i2c;
    struct {
        int host;
        gpio_num_t sclk;
        gpio_num_t mosi;
        gpio_num_t cs;
        gpio_num_t dc; // Data/Command
        gpio_num_t backlight; // Backlight control
    } spi;
} display_hw_config_t;
