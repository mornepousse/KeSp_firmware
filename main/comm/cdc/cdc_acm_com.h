/* CDC ACM command framework — binary protocol only.
   Provides: binary send helpers, processing task, byte packing. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Configuration ───────────────────────────────────────────────── */

#define OTA_CHUNK_SIZE 4096
#define OTA_TIMEOUT_MS 30000

/* ── Send helpers ────────────────────────────────────────────────── */

void cdc_send_binary(const uint8_t *data, size_t len);

/* Byte-packing helpers for binary protocol */
static inline void pack_u16_le(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}
static inline void pack_u32_le(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* ── USB callbacks ───────────────────────────────────────────────── */

/* Called by TinyUSB ISR/callback with arbitrary chunks */
void receive_data(const char *data, uint16_t len);

/* Initialize binary protocol processing task */
void init_cdc_commands(void);
