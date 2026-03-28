/* Keyboard task: main coordinator */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Task handle for ISR notification */
extern TaskHandle_t keyboard_task_handle;

/* USB/BLE transport state (0=USB, 1=BLE) */
extern uint8_t usb_bl_state;

/* Main keyboard task entry point */
void vTaskKeyboard(void *pvParameters);

/* Initialize keyboard subsystem (HID queue, actions worker) */
void keyboard_manager_init(void);
