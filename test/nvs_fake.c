/* Implémentation RAM-backed du fake NVS.
 * Couvre exactement les primitives appelées par keymap.c :
 *   nvs_flash_init, nvs_flash_erase,
 *   nvs_open, nvs_close, nvs_commit,
 *   nvs_set_blob, nvs_get_blob,
 *   nvs_set_u32, nvs_get_u32.
 */
#include "nvs_fake.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdint.h>

/* ── Paramètres du store ─────────────────────────────────────── */
#define NVS_MAX_ENTRIES  64
#define NVS_NS_LEN       16
#define NVS_KEY_LEN      16
/* MAX_MACROS * sizeof(macro_t) ≤ 1500 ; keymaps ≤ 1300. 4096 est confortable. */
#define NVS_BLOB_MAX     4096
#define NVS_MAX_HANDLES  8

typedef struct {
    char     ns[NVS_NS_LEN];
    char     key[NVS_KEY_LEN];
    int      is_u32;
    uint32_t u32_val;
    uint8_t  blob[NVS_BLOB_MAX];
    size_t   blob_size;
} nvs_entry_t;

static nvs_entry_t s_store[NVS_MAX_ENTRIES];
static int         s_entry_count;
static int         s_fail_writes;   /* injection de faute pour nvs_set_blob */

static char s_handle_ns[NVS_MAX_HANDLES][NVS_NS_LEN];
static int  s_handle_used[NVS_MAX_HANDLES];

/* ── API publique ────────────────────────────────────────────── */

void nvs_fake_reset(void)
{
    memset(s_store,      0, sizeof(s_store));
    memset(s_handle_used, 0, sizeof(s_handle_used));
    s_entry_count = 0;
    s_fail_writes = 0;
}

void nvs_fake_fail_writes(int enable) { s_fail_writes = enable; }

/* ── Helpers internes ────────────────────────────────────────── */

static nvs_entry_t *find_entry(const char *ns, const char *key)
{
    for (int i = 0; i < s_entry_count; i++) {
        if (strncmp(s_store[i].ns,  ns,  NVS_NS_LEN  - 1) == 0 &&
            strncmp(s_store[i].key, key, NVS_KEY_LEN - 1) == 0)
            return &s_store[i];
    }
    return NULL;
}

static nvs_entry_t *get_or_create(const char *ns, const char *key)
{
    nvs_entry_t *e = find_entry(ns, key);
    if (e) return e;
    if (s_entry_count >= NVS_MAX_ENTRIES) return NULL;
    e = &s_store[s_entry_count++];
    strncpy(e->ns,  ns,  NVS_NS_LEN  - 1);
    strncpy(e->key, key, NVS_KEY_LEN - 1);
    return e;
}

/* ── Injection directe (pour tests de garde de taille) ──────── */

void nvs_fake_put_blob(const char *ns, const char *key,
                       const void *data, size_t size)
{
    nvs_entry_t *e = get_or_create(ns, key);
    if (!e || size > NVS_BLOB_MAX) return;
    e->is_u32    = 0;
    e->blob_size = size;
    memcpy(e->blob, data, size);
}

void nvs_fake_put_u32(const char *ns, const char *key, uint32_t value)
{
    nvs_entry_t *e = get_or_create(ns, key);
    if (!e) return;
    e->is_u32  = 1;
    e->u32_val = value;
}

/* ── Implémentations des primitives NVS ─────────────────────── */

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    nvs_fake_reset();
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    for (int i = 0; i < NVS_MAX_HANDLES; i++) {
        if (!s_handle_used[i]) {
            s_handle_used[i] = 1;
            strncpy(s_handle_ns[i], ns, NVS_NS_LEN - 1);
            *out = (nvs_handle_t)(i + 1);   /* handle 1-based */
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle)
{
    int idx = (int)handle - 1;
    if (idx >= 0 && idx < NVS_MAX_HANDLES)
        s_handle_used[idx] = 0;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                        const void *value, size_t length)
{
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= NVS_MAX_HANDLES || !s_handle_used[idx])
        return ESP_FAIL;
    if (s_fail_writes) return ESP_FAIL;   /* injection de faute (simule NVS pleine) */
    if (length > NVS_BLOB_MAX) return ESP_FAIL;
    nvs_entry_t *e = get_or_create(s_handle_ns[idx], key);
    if (!e) return ESP_FAIL;
    e->is_u32    = 0;
    e->blob_size = length;
    memcpy(e->blob, value, length);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                        void *out, size_t *length)
{
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= NVS_MAX_HANDLES || !s_handle_used[idx])
        return ESP_FAIL;
    nvs_entry_t *e = find_entry(s_handle_ns[idx], key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    /* out == NULL = requête de taille seulement */
    if (out == NULL) {
        *length = e->blob_size;
        return ESP_OK;
    }
    memcpy(out, e->blob, e->blob_size);
    *length = e->blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= NVS_MAX_HANDLES || !s_handle_used[idx])
        return ESP_FAIL;
    nvs_entry_t *e = get_or_create(s_handle_ns[idx], key);
    if (!e) return ESP_FAIL;
    e->is_u32  = 1;
    e->u32_val = value;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out)
{
    int idx = (int)handle - 1;
    if (idx < 0 || idx >= NVS_MAX_HANDLES || !s_handle_used[idx])
        return ESP_FAIL;
    nvs_entry_t *e = find_entry(s_handle_ns[idx], key);
    if (!e || !e->is_u32) return ESP_ERR_NVS_NOT_FOUND;
    *out = e->u32_val;
    return ESP_OK;
}
