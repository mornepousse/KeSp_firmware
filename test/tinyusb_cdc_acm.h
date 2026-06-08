/* stub: tinyusb_cdc_acm.h — utilisé uniquement dans les builds hôte (TEST_HOST).
 * Déclare le minimum nécessaire pour compiler cdc_binary_protocol.c.
 * Les implémentations (fake write_queue / write_flush) sont définies
 * dans test_cdc_rx_feed.c. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int  esp_err_t;
typedef int  tinyusb_cdcacm_itf_t;
#define TINYUSB_CDC_ACM_0 ((tinyusb_cdcacm_itf_t)0)

esp_err_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf,
                                      const uint8_t *buf, size_t size);
esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf,
                                      uint32_t timeout_ticks);
