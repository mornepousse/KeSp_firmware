/* Fake NVS RAM-backed pour les tests hôte.
 * Permet de tester la logique de persistance de keymap.c sans matériel. */
#pragma once
#include <stddef.h>

/* Réinitialise entièrement le store RAM (appeler au début de chaque test). */
void nvs_fake_reset(void);

/* Injection de faute : quand enable != 0, nvs_set_blob renvoie une erreur
 * (simule NVS pleine) → teste la propagation d'erreur des save_*. */
void nvs_fake_fail_writes(int enable);

/* Injecte directement un blob dans le store, en contournant keymap.c.
 * Utile pour tester les gardes de taille (cas where stored_size != expected). */
void nvs_fake_put_blob(const char *ns, const char *key,
                       const void *data, size_t size);
