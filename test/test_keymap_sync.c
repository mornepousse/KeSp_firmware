/* Réassemblage de la keymap reçue par chunks (drip via ACK payload) — pur.
 *
 * Le pull est piloté par la gauche : elle ne demande JAMAIS que le prochain chunk
 * manquant, et le dongle le sert. L'ordre est donc garanti par construction, et
 * le réassembleur peut être séquentiel : il n'accepte que le chunk attendu et
 * ignore doublons / hors-séquence. Conséquence utile : un ACK payload rejoué,
 * ou une trame en retard, ne corrompt jamais la keymap en cours de réception.
 *
 * Design : docs/superpowers/specs/2026-09-13-keymap-sync-ack-payload-design.md
 */
#include "test_framework.h"
#include "../main/comm/rf/keymap_sync.h"
#include <string.h>

static void remplir(uint8_t *d, uint8_t seed)
{
    for (int i = 0; i < SYNC_CHUNK_BYTES; i++) d[i] = (uint8_t)(seed + i);
}

static void test_sequence_complete(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 0, "démarre au chunk 0");
    TEST_ASSERT(!keymap_rx_complete(&s), "pas complet au départ");
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(keymap_rx_chunk(&s, (uint8_t)k, d), "chunk en séquence accepté");
        TEST_ASSERT_EQ(keymap_rx_next(&s), (uint8_t)(k + 1), "next avance");
    }
    TEST_ASSERT(keymap_rx_complete(&s), "complet après 40 chunks");
    /* Le blob réassemblé est exact, chaque chunk à sa place. */
    for (int k = 0; k < SYNC_N_CHUNKS; k++) {
        uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, (uint8_t)k);
        TEST_ASSERT(memcmp(&s.buf[k * SYNC_CHUNK_BYTES], d, SYNC_CHUNK_BYTES) == 0,
                    "contenu du chunk à la bonne place");
    }
    /* Une fois complet, un chunk de plus est refusé (pas d'écriture hors buffer). */
    uint8_t extra[SYNC_CHUNK_BYTES]; remplir(extra, 99);
    TEST_ASSERT(!keymap_rx_chunk(&s, SYNC_N_CHUNKS, extra), "au-delà de 40 : refusé");
    TEST_ASSERT_EQ(keymap_rx_next(&s), SYNC_N_CHUNKS, "next reste à 40");
}

static void test_doublon_et_hors_sequence_ignores(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    uint8_t d0[SYNC_CHUNK_BYTES]; remplir(d0, 0);
    TEST_ASSERT(keymap_rx_chunk(&s, 0, d0), "chunk 0 accepté");
    /* Doublon de 0 (ACK rejoué) : ignoré, next inchangé. */
    TEST_ASSERT(!keymap_rx_chunk(&s, 0, d0), "doublon ignoré");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next reste 1 après doublon");
    /* Saut à 5 (hors séquence) : ignoré, et le contenu n'a pas été écrit. */
    uint8_t d5[SYNC_CHUNK_BYTES]; remplir(d5, 5);
    TEST_ASSERT(!keymap_rx_chunk(&s, 5, d5), "hors séquence ignoré");
    TEST_ASSERT_EQ(keymap_rx_next(&s), 1, "next reste 1 après hors-séquence");
    TEST_ASSERT(memcmp(&s.buf[5 * SYNC_CHUNK_BYTES], d5, SYNC_CHUNK_BYTES) != 0,
                "un chunk hors séquence n'écrit rien dans le buffer");
}

/* Un reset repart de zéro même après un pull partiel — c'est ce que fait la
 * gauche quand une nouvelle BEACON annonce une AUTRE empreinte en cours de
 * route (la keymap du dongle a rechangé) : on recommence proprement. */
static void test_reset_repart_de_zero(void)
{
    keymap_rx_t s;
    keymap_rx_reset(&s);
    uint8_t d[SYNC_CHUNK_BYTES]; remplir(d, 1);
    keymap_rx_chunk(&s, 0, d);
    keymap_rx_chunk(&s, 1, d);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 2, "deux chunks reçus");
    keymap_rx_reset(&s);
    TEST_ASSERT_EQ(keymap_rx_next(&s), 0, "reset : retour au chunk 0");
    TEST_ASSERT(!keymap_rx_complete(&s), "reset : plus complet");
}

void test_keymap_sync(void)
{
    TEST_SUITE("Réassembleur keymap (pull séquentiel)");
    test_sequence_complete();
    test_doublon_et_hors_sequence_ignores();
    test_reset_repart_de_zero();
}
