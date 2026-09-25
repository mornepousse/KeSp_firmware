/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gptimer.h"

// 1MHz, 1 tick = 1us
/* KaSe: clocked from the XTAL, not the default APB (2026-09-25). The IDF
 * gptimer driver takes an ESP_PM_APB_FREQ_MAX lock for an APB source (APB
 * moves under DFS) and only ESP_PM_NO_LIGHT_SLEEP for any other. The scan
 * timer runs while a key is held: with APB, the chip waited between two 1 ms
 * scans at 80 MHz — 36 mA measured on the Niphargus left, the ESP32-S3
 * datasheet v2.2 table 5-9 gives 22.0-36.1 mA for 80 MHz WAITI; with the XTAL
 * DFS can drop to 40 MHz (13.2-18.8 mA). Light sleep is impossible at a 1 ms
 * scan anyway, so the NO_LIGHT_SLEEP lock costs nothing. Boards without
 * CONFIG_PM_ENABLE take no lock at all. */
#define GPTIMER_CONFIG_DEFAULT()        \
{                                       \
    .clk_src = GPTIMER_CLK_SRC_XTAL,    \
    .direction = GPTIMER_COUNT_UP,      \
    .resolution_hz = 1 * 1000 * 1000,   \
}

#define GPTIMER_ALARM_CONFIG_DEFAULT(count) \
{                                       \
    .reload_count = 0,                  \
    .flags.auto_reload_on_alarm = true, \
    .alarm_count = count,               \
}

typedef struct {
    gptimer_handle_t *gptimer;      /*!< Pointer to gptimer handle */
    gptimer_event_callbacks_t cbs;  /*!< gptimer event callbacks */
    void *user_data;                /*!< User data */
    uint32_t alarm_count_us;        /*!< Timer interrupt period */
} kbd_gptimer_config_t;

/**
 * @brief  Initialize gptime
 *
 * @param config Configuration for the gptimer
 * @return
 *     ESP_OK on success
 *     ESP_ERR_INVALID_ARG if the parameter is invalid
 */
esp_err_t kbd_gptimer_init(kbd_gptimer_config_t *config);

/**
 * @brief deinitialize gptimer
 *
 * @param gptimer gptimer handle
 * @return
 *      ESP_ERR_INVALID_ARG if parameter is invalid
 *      ESP_OK if success
 */
esp_err_t kbd_gptimer_deinit(gptimer_handle_t gptimer);

/**
 * @brief Stop the gptimer.
 *
 * @param gptimer gptimer handle
 * @return ESP_OK if success
 */
esp_err_t kbd_gptimer_stop(gptimer_handle_t gptimer);

/**
 * @brief Start the gptimer.
 *
 * @param gptimer gptimer handle
 * @return ESP_OK if success
 */
esp_err_t kbd_gptimer_start(gptimer_handle_t gptimer);

#ifdef __cplusplus
}
#endif
