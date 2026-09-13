#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Cohérence de config entre les deux moteurs keymap (dongle + gauche) — fusion.
 *
 * Deux moteurs ne doivent pas taper différemment. Plutôt qu'un numéro de version
 * tenu à la main (design initial, règle 2), on prend une EMPREINTE du contenu :
 * un CRC-32 du blob de config. Même contenu → même empreinte ; toute différence
 * la change. Le contrôleur lit l'empreinte des deux appareils (égales =
 * synchronisées) ; le dongle comparera l'empreinte annoncée par la gauche à la
 * sienne pour refuser de tourner sur une divergence (garde-fou à venir).
 *
 * Logique pure, testée host (test/test_config_sync.c).
 * Design : docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md
 */

/* CRC-32 IEEE (reflété, poly 0xEDB88320) — le CRC-32 « zlib/gzip » standard.
 * Bitwise : pas de table, quelques Ko de config à hacher une fois, le coût est
 * négligeable et le code reste identique sur hôte et cible. */
static inline uint32_t config_fp_crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Deux empreintes désignent-elles la même config ? Égales ET non nulles : 0 vaut
 * « inconnu / pas encore annoncé », jamais un accord. */
static inline bool config_fp_match(uint32_t a, uint32_t b)
{
    return a != 0u && a == b;
}
