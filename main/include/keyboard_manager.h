#pragma once
#include <stdlib.h>

#include <stdint.h>

// Structure d'une macro (liste de touches + longueur)
typedef struct {
  uint8_t keys[6]; // Jusqu'à 6 touches simultanées (HID limit)
  uint16_t key_definition;
} macro_t;

extern uint16_t extra_keycodes[6];
extern void send_hid_key();
extern void vTaskKeyboard(void *pvParameters);
void keyboard_manager_init();


extern macro_t macros_list[];
extern size_t macros_count;
