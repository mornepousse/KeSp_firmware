/* Fake RAM-backed NVS for host tests.
 * Lets keymap.c's persistence logic be tested without hardware. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Fully resets the RAM store (call at the start of every test). */
void nvs_fake_reset(void);

/* Fault injection: when enable != 0, nvs_set_blob returns an error
 * (simulates a full NVS) → tests error propagation from save_*. */
void nvs_fake_fail_writes(int enable);

/* Injects a blob directly into the store, bypassing keymap.c.
 * Useful for testing size guards (case where stored_size != expected). */
void nvs_fake_put_blob(const char *ns, const char *key,
                       const void *data, size_t size);

/* Injects a u32 value (for testing version guards). */
void nvs_fake_put_u32(const char *ns, const char *key, uint32_t value);
