#pragma once

/*
 * espnow_info.h — ESP-NOW info-channel handler API.
 *
 * Shared between espnow_info.c (writer) and eink.c (reader) via g_half_state.
 * Roles:
 *   - Dongle: receives EN_INFO_BATTERY from a half; stub logs it.
 *   - Half:   receives EN_INFO_LAYER + EN_INFO_STATE from dongle; updates g_half_state.
 *
 * g_half_state and g_half_state_mutex are defined in espnow_info.c (half role only).
 * They are declared extern here for eink.c.
 */

#include "espnow_msg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <string.h>

/* ── Half state shared struct ────────────────────────────────── */
typedef struct {
    uint8_t layer_idx;       /* Current active layer index */
    char    layer_name[16];  /* Layer name, zero-padded */
    uint8_t modifiers;       /* HID modifier byte */
    uint8_t flags;           /* bit0=caps_word, bit1=bt_connected, bit2=usb_active */
} half_state_t;

/* Global half state (defined in espnow_info.c, HALF role only).
 * On the dongle, this symbol is not defined — eink.c is not compiled there either. */
extern half_state_t g_half_state;
extern SemaphoreHandle_t g_half_state_mutex;

/* Convenience lock/unlock wrappers */
static inline void half_state_lock(void)   { xSemaphoreTake(g_half_state_mutex, portMAX_DELAY); }
static inline void half_state_unlock(void) { xSemaphoreGive(g_half_state_mutex); }

/* Initialize the info-channel handler module.
 * Creates g_half_state_mutex. Must be called before espnow_link_init(). */
void espnow_info_init(void);

/* Dispatch callback called by espnow_link.c on message receipt.
 * mac: 6-byte sender MAC; buf: raw [type][payload] bytes; len: total length. */
void espnow_info_dispatch(const uint8_t mac[6], const uint8_t *buf, uint8_t len);
