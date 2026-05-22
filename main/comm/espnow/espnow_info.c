/*
 * espnow_info.c — ESP-NOW info-channel handler stubs.
 *
 * Skeleton:  dispatch logic, g_half_state update, mutex.
 * Stub:      all handler bodies (log only or no-op).
 *
 * Both dongle and half compile this file. Role-specific code is guarded with
 * CONFIG_KASE_DEVICE_ROLE_DONGLE / CONFIG_KASE_DEVICE_ROLE_HALF.
 */

#include "espnow_info.h"
#include "espnow_msg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "espnow_info";

/* ── g_half_state + mutex — defined on HALF only ─────────────── */
#if CONFIG_KASE_DEVICE_ROLE_HALF
half_state_t     g_half_state       = { 0 };
SemaphoreHandle_t g_half_state_mutex = NULL;
#endif

void espnow_info_init(void)
{
#if CONFIG_KASE_DEVICE_ROLE_HALF
    g_half_state_mutex = xSemaphoreCreateMutex();
    if (g_half_state_mutex == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed for g_half_state_mutex");
    }
    memset(&g_half_state, 0, sizeof(g_half_state));
    ESP_LOGI(TAG, "espnow_info_init OK (half role)");
#else
    ESP_LOGI(TAG, "espnow_info_init OK (dongle role — no half_state)");
#endif
}

/* ── Dongle: receives EN_INFO_BATTERY from a half ─────────────── */
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
static void on_battery(const uint8_t mac[6], const en_battery_t *b)
{
    ESP_LOGI(TAG, "battery from %02X:%02X:%02X:%02X:%02X:%02X: %u.%uV soc=%u%% chg=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             b->batt_dV / 10, b->batt_dV % 10,
             b->soc_pct, b->charging);
    /* TODO STUB: Forward battery level to controller via CDC push frame.
     *   Suggested: new KR unsolicited frame (KS_CMD_RF_LINK_STATUS extended, or new cmd).
     *   Or: store in a global and return via KS_CMD_DONGLE_STATS.
     *   For now: log only. */
}
#endif /* CONFIG_KASE_DEVICE_ROLE_DONGLE */

/* ── Half: receives EN_INFO_LAYER from dongle ─────────────────── */
#if CONFIG_KASE_DEVICE_ROLE_HALF
static void on_layer(const en_layer_t *l)
{
    ESP_LOGI(TAG, "layer update: idx=%u name='%.16s'", l->layer_idx, l->name);
    /* TODO STUB: Update g_half_state and wake eink_task.
     *   half_state_lock();
     *   g_half_state.layer_idx = l->layer_idx;
     *   memcpy(g_half_state.layer_name, l->name, 16);
     *   half_state_unlock();
     *   xTaskNotify(s_eink_task_handle, 0x01, eSetBits);   // wake eink to refresh
     *   Requires: expose eink task handle (eink_get_task_handle() in eink.h). */
    (void)l;
}

/* ── Half: receives EN_INFO_STATE from dongle ─────────────────── */
static void on_state(const en_state_t *s)
{
    ESP_LOGD(TAG, "state update: mod=0x%02x flags=0x%02x", s->modifiers, s->flags);
    /* TODO STUB: Update g_half_state modifiers/flags and wake eink_task. */
    (void)s;
}
#endif /* CONFIG_KASE_DEVICE_ROLE_HALF */

/* ── Dispatch: called by espnow_link.c recv callback ─────────── */
void espnow_info_dispatch(const uint8_t mac[6], const uint8_t *buf, uint8_t len)
{
    if (len < 1) return;
    uint8_t type = buf[0];

    switch (type) {
#if CONFIG_KASE_DEVICE_ROLE_DONGLE
    case EN_INFO_BATTERY: {
        en_battery_t b;
        if (en_decode_battery(buf, len, &b)) {
            on_battery(mac, &b);
        }
        break;
    }
#endif
#if CONFIG_KASE_DEVICE_ROLE_HALF
    case EN_INFO_LAYER: {
        en_layer_t l;
        if (en_decode_layer(buf, len, &l)) {
            on_layer(&l);
        }
        break;
    }
    case EN_INFO_STATE: {
        en_state_t s;
        if (en_decode_state(buf, len, &s)) {
            on_state(&s);
        }
        break;
    }
#endif
    default:
        ESP_LOGW(TAG, "unknown ESP-NOW type 0x%02x (len=%u) — dropped", type, len);
        break;
    }
}
