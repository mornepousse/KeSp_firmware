#include "sec_store.h"
#include <string.h>

static bool sec_store_persist(void);   /* defined per-target below */

static sec_slot_t s_slots[SEC_N_SLOTS];

bool sec_store_set_slot(uint8_t idx, uint8_t type, const char *label,
                        const uint8_t *secret, uint8_t secret_len)
{
    if (idx >= SEC_N_SLOTS) return false;
    if (secret_len > SEC_SECRET_MAX) return false;
    if (secret_len > 0 && secret == NULL) return false;
    sec_slot_t *s = &s_slots[idx];
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->flags = 0x01;                 /* require_keypress forced on */
    s->secret_len = secret_len;
    if (label) {
        strncpy(s->label, label, SEC_LABEL_LEN - 1);
        s->label[SEC_LABEL_LEN - 1] = '\0';
    }
    if (secret && secret_len) memcpy(s->secret, secret, secret_len);
    return sec_store_persist();
}

bool sec_store_clear_slot(uint8_t idx)
{
    if (idx >= SEC_N_SLOTS) return false;
    memset(&s_slots[idx], 0, sizeof(s_slots[idx]));
    return sec_store_persist();
}

uint8_t sec_store_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++)
        if (s_slots[i].type != SEC_SLOT_EMPTY) n++;
    return n;
}

uint8_t sec_store_type(uint8_t idx)
{
    return (idx < SEC_N_SLOTS) ? s_slots[idx].type : SEC_SLOT_EMPTY;
}

const char *sec_store_label(uint8_t idx)
{
    if (idx >= SEC_N_SLOTS || s_slots[idx].type == SEC_SLOT_EMPTY) return NULL;
    return s_slots[idx].label;
}

bool sec_store_get_secret(uint8_t idx, uint8_t *out, uint8_t *out_len)
{
    if (idx >= SEC_N_SLOTS || s_slots[idx].type == SEC_SLOT_EMPTY) return false;
    if (out)     memcpy(out, s_slots[idx].secret, s_slots[idx].secret_len);
    if (out_len) *out_len = s_slots[idx].secret_len;
    return true;
}

#ifndef TEST_HOST
#include "esp_log.h"
#include "nvs_utils.h"
#include "keyboard_config.h"   /* STORAGE_NAMESPACE */
static const char *TAG = "sec_store";
static bool sec_store_persist(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                                             sizeof(s_slots), "sec_slots_ver", 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
void sec_store_init(void)
{
    uint32_t ver = 0;
    nvs_load_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                             sizeof(s_slots), "sec_slots_ver", &ver);
}
#else
static bool sec_store_persist(void) { return true; }
void sec_store_init(void) { memset(s_slots, 0, sizeof(s_slots)); }
#endif
