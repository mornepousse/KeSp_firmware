/* Fake NVS RAM-backed pour les tests hôte.
 * Permet de tester la logique de persistance de keymap.c sans matériel. */
#pragma once
#include <stddef.h>

/* Réinitialise entièrement le store RAM (appeler au début de chaque test). */
void nvs_fake_reset(void);

/* Injecte directement un blob dans le store, en contournant keymap.c.
 * Utile pour tester les gardes de taille (cas where stored_size != expected). */
void nvs_fake_put_blob(const char *ns, const char *key,
                       const void *data, size_t size);
