#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "rf_packet.h"   /* SYNC_CHUNK_BYTES, SYNC_N_CHUNKS */

/* Réassemblage de la keymap reçue par chunks — sync auto dongle→gauche par ACK
 * payload (fusion, phase 3). Côté GAUCHE.
 *
 * Le pull est piloté par la gauche : elle ne demande jamais que le PROCHAIN
 * chunk manquant (rf_sync_req_t.next), et le dongle le sert dans l'ACK. L'ordre
 * est donc garanti par construction, et le réassembleur peut rester séquentiel :
 * il n'accepte que le chunk attendu et ignore doublons / hors-séquence. Un ACK
 * payload rejoué ou une trame en retard ne corrompt jamais la keymap en cours.
 *
 * Pas de bitmap de chunks reçus : un simple compteur suffit, et il est aussi le
 * « prochain chunk voulu » que la gauche annonce au dongle. Quand il atteint
 * SYNC_N_CHUNKS, le buffer contient exactement KEYMAP_BLOB_BYTES octets, prêts
 * pour save_keymaps ; l'empreinte annoncée au STATUS suivant en est l'accusé.
 *
 * Pur, testé host (test/test_keymap_sync.c). Design :
 * docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md */
typedef struct {
    uint8_t buf[SYNC_N_CHUNKS * SYNC_CHUNK_BYTES];   /* 1120 o = KEYMAP_BLOB_BYTES */
    uint8_t next;                                    /* prochain chunk attendu, 0..40 */
} keymap_rx_t;

static inline void keymap_rx_reset(keymap_rx_t *s) { s->next = 0; }

/* Un chunk arrive. Accepté (copié, next avance) SEULEMENT si c'est celui attendu
 * et qu'il reste de la place ; sinon ignoré sans rien écrire. Retourne true si
 * accepté. */
static inline bool keymap_rx_chunk(keymap_rx_t *s, uint8_t idx, const uint8_t *data)
{
    if (s->next >= SYNC_N_CHUNKS || idx != s->next) return false;
    memcpy(&s->buf[(size_t)idx * SYNC_CHUNK_BYTES], data, SYNC_CHUNK_BYTES);
    s->next++;
    return true;
}

/* Prochain chunk voulu — ce que la gauche met dans rf_sync_req_t.next. */
static inline uint8_t keymap_rx_next(const keymap_rx_t *s) { return s->next; }

/* Tous les chunks sont là : buf est une keymap complète à enregistrer. */
static inline bool keymap_rx_complete(const keymap_rx_t *s) { return s->next >= SYNC_N_CHUNKS; }
