/*
 * espnow_info.c — ESP-NOW info-channel handler.
 *
 * Roles:
 *   - Dongle: receives EN_INFO_BATTERY from a half; stub logs it.
 *   - Half:   receives EN_INFO_LAYER + EN_INFO_STATE from dongle;
 *             updates g_half_state (under mutex) + notifies eink_lvgl_task.
 *
 * Both dongle and half compile this file. Role-specific code is guarded with
 * CONFIG_KASE_DEVICE_ROLE_DONGLE / CONFIG_KASE_DEVICE_ROLE_HALF.
 */

#include "espnow_info.h"
#include "espnow_msg.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#include "eink_lvgl.h"    /* eink_get_task_handle() */

static void on_layer(const en_layer_t *l)
{
    ESP_LOGI(TAG, "layer update: idx=%u name='%.16s'", l->layer_idx, l->name);

    /* Update g_half_state under mutex — ESP-NOW recv task context.
     * Timeout 10 ms: if the mutex is held by eink_lvgl_task during label update,
     * we skip this event (next layer change will retry). Acceptable at low rate. */
    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_half_state.layer_idx = l->layer_idx;
        memcpy(g_half_state.layer_name, l->name, 16);
        xSemaphoreGive(g_half_state_mutex);
    } else {
        ESP_LOGW(TAG, "on_layer: mutex timeout — layer update skipped");
        return;
    }

    /* Wake eink_lvgl_task — bit 0 = layer/state event.
     * Non-blocking (eSetBits): safe from any task context (not ISR). */
    TaskHandle_t h = eink_get_task_handle();
    if (h != NULL) {
        xTaskNotify(h, 0x01, eSetBits);
    }
}

/* ── Half: receives EN_INFO_STATE from dongle ─────────────────── */
static void on_state(const en_state_t *s)
{
    ESP_LOGD(TAG, "state update: mod=0x%02x flags=0x%02x", s->modifiers, s->flags);

    if (xSemaphoreTake(g_half_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_half_state.modifiers = s->modifiers;
        g_half_state.flags     = s->flags;
        xSemaphoreGive(g_half_state_mutex);
    } else {
        ESP_LOGW(TAG, "on_state: mutex timeout — state update skipped");
        return;
    }

    /* Wake eink_lvgl_task — same bit 0; the task reads both layer_name and modifiers. */
    TaskHandle_t h = eink_get_task_handle();
    if (h != NULL) {
        xTaskNotify(h, 0x01, eSetBits);
    }
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
