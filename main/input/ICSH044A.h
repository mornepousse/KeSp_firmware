#pragma once
#include <stdint.h>
#include <stdlib.h>

void initISH044A();
void getLvltrackball(uint8_t CURSOR_Dir, uint8_t key,
                     uint8_t cursor_Selected_state);
void vTaskTrackBall(void *pvParameters);
void example_ledc_init(void);
void vTaskLED_Animation(void *pvParameters);
