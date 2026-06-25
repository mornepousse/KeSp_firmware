/* main/security/openpgp_do.c — OpenPGP Data-Object store */
#include "openpgp_do.h"
#include <string.h>

static bool openpgp_do_persist(void); /* defined per-target below */

typedef struct {
    uint16_t tag;
    uint16_t len;
    uint8_t  data[OPENPGP_DO_MAX_LEN];
    bool     set;
} do_entry_t;

static do_entry_t s_dos[OPENPGP_DO_MAX_ENTRIES];

static do_entry_t *find_entry(uint16_t tag)
{
    for (int i = 0; i < OPENPGP_DO_MAX_ENTRIES; i++)
        if (s_dos[i].set && s_dos[i].tag == tag)
            return &s_dos[i];
    return NULL;
}

static do_entry_t *find_or_alloc(uint16_t tag)
{
    do_entry_t *e = find_entry(tag);
    if (e) return e;
    for (int i = 0; i < OPENPGP_DO_MAX_ENTRIES; i++)
        if (!s_dos[i].set)
            return &s_dos[i];
    return NULL;
}

bool openpgp_do_put(uint16_t tag, const uint8_t *v, uint16_t n)
{
    if (n > OPENPGP_DO_MAX_LEN) return false;
    do_entry_t *e = find_or_alloc(tag);
    if (!e) return false;
    e->tag = tag;
    e->len = n;
    if (n > 0 && v) memcpy(e->data, v, n);
    else            memset(e->data, 0, n);
    e->set = true;
    return openpgp_do_persist();
}

bool openpgp_do_get(uint16_t tag, const uint8_t **v, uint16_t *n)
{
    do_entry_t *e = find_entry(tag);
    if (!e) return false;
    if (v) *v = e->data;
    if (n) *n = e->len;
    return true;
}

void openpgp_do_reset(void)
{
    memset(s_dos, 0, sizeof(s_dos));
}

#ifndef TEST_HOST
#include "esp_log.h"
#include "nvs_utils.h"
#include "keyboard_config.h"
static const char *TAG = "openpgp_do";
static bool openpgp_do_persist(void)
{
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "pgp_dos",
                                             s_dos, sizeof(s_dos),
                                             "pgp_dos_ver", 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
void openpgp_do_init(void)
{
    uint32_t ver = 0;
    esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, "pgp_dos",
                             s_dos, sizeof(s_dos), "pgp_dos_ver", &ver);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "pgp_dos absent/invalid (%s) — DOs reset to factory",
                 esp_err_to_name(err));
}
#else
static bool openpgp_do_persist(void) { return true; }
void openpgp_do_init(void) { openpgp_do_reset(); }
#endif
