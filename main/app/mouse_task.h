/* Main task of the Conchodytes mouse — see mouse_task.c. */
#pragma once
#include "esp_err.h"

/* Initializes the inputs and the sensor, then starts the polling task.
 * Returns ESP_OK even if the sensor is missing: clicks and the wheel remain
 * readable, and the failure is logged. Only returns an error if the task
 * itself could not start. */
esp_err_t mouse_task_start(void);
