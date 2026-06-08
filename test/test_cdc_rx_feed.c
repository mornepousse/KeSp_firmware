/*
 * test_cdc_rx_feed.c — Tests de CARACTÉRISATION pour ks_rx_feed() et ks_crc8().
 *
 * But : épingler le comportement ACTUEL du parser de frames KS binaire
 * tel qu'il est dans cdc_binary_protocol.c. Aucune correction de bug ici.
 *
 * Format frame KS (request) :
 *   [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
 *
 * Stubs requis (dans test/) :
 *   esp_log.h, tinyusb_cdc_acm.h, tusb.h, freertos/FreeRTOS.h, task.h
 * Ces stubs sont résolus via le chemin d'inclusion CMake (test/ est le premier
 * répertoire), donc transparent pour cdc_binary_protocol.c.
 */
#include "test_framework.h"
#include "tinyusb_cdc_acm.h"               /* stub : esp_err_t, tinyusb_cdcacm_itf_t */
#include "../main/comm/cdc/cdc_binary_protocol.h"
#include <string.h>

/* ── Stubs requis par cdc_binary_protocol.c au link ─────────────────────── */

/* TAG_CDC déclaré extern dans cdc_internal.h, défini ici pour ce TU de test. */
const char *TAG_CDC = "test_cdc";

/* Capture des écritures CDC — réinitialisée avant chaque sous-test. */
static uint8_t  fake_cdc_buf[256];
static size_t   fake_cdc_pos;
static int      fake_write_count;

esp_err_t tinyusb_cdcacm_write_queue(tinyusb_cdcacm_itf_t itf,
                                      const uint8_t *buf, size_t size)
{
    (void)itf;
    if (size > 0 && fake_cdc_pos + size <= sizeof(fake_cdc_buf))
        memcpy(fake_cdc_buf + fake_cdc_pos, buf, size);
    fake_cdc_pos += size;
    fake_write_count++;
    return 0;
}

esp_err_t tinyusb_cdcacm_write_flush(tinyusb_cdcacm_itf_t itf,
                                      uint32_t timeout_ticks)
{
    (void)itf; (void)timeout_ticks;
    return 0;
}

/* ── Handler de test enregistré via ks_register_binary_commands ─────────── */

static bool     fake_rx_called;
static uint8_t  fake_rx_cmd;
static uint16_t fake_rx_len;
static uint8_t  fake_rx_payload[64]; /* suffisant pour les payloads de test */

static void test_cmd_handler(uint8_t cmd_id, const uint8_t *payload, uint16_t len)
{
    fake_rx_called = true;
    fake_rx_cmd    = cmd_id;
    fake_rx_len    = len;
    if (len > 0 && len <= sizeof(fake_rx_payload))
        memcpy(fake_rx_payload, payload, len);
}

static const ks_bin_cmd_entry_t test_cmd_table[] = {
    { KS_CMD_PING,    test_cmd_handler },
    { KS_CMD_VERSION, test_cmd_handler },
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Reset l'état du parser ET l'état de capture entre chaque cas. */
static void reset_state(void)
{
    ks_rx_reset();
    fake_cdc_pos      = 0;
    fake_write_count  = 0;
    memset(fake_cdc_buf, 0, sizeof(fake_cdc_buf));
    fake_rx_called    = false;
    fake_rx_cmd       = 0;
    fake_rx_len       = 0;
    memset(fake_rx_payload, 0, sizeof(fake_rx_payload));
}

/*
 * Construit un frame KS valide dans buf.
 * - payload peut être NULL si len==0 (ks_crc8 ne déréférence pas avec len=0).
 * - Retourne la taille totale de la frame (6 + len).
 */
static size_t build_ks_frame(uint8_t *buf, uint8_t cmd,
                              const uint8_t *payload, uint16_t len)
{
    buf[0] = KS_MAGIC_0;
    buf[1] = KS_MAGIC_1;
    buf[2] = cmd;
    buf[3] = (uint8_t)(len & 0xFF);
    buf[4] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0 && payload)
        memcpy(buf + 5, payload, len);
    buf[5 + len] = ks_crc8(payload, len); /* NULL+0 → retourne 0x00, sûr */
    return (size_t)(6 + len);
}

/* ══ Suite CRC-8 ═════════════════════════════════════════════════════════ */

/* Vecteur 1 : payload vide → CRC = 0x00 (init seul, boucle ne tourne pas) */
static void test_crc8_empty_payload(void)
{
    TEST_ASSERT_EQ(ks_crc8(NULL, 0), 0x00,
                   "crc8(NULL,0) = 0x00 — init 0x00, boucle ignorée");
}

/* Vecteur 2 : octet 0x00 → CRC = 0x00
 * (XOR puis 8 shifts sans activer le polynôme : 0 reste 0) */
static void test_crc8_single_zero_byte(void)
{
    uint8_t d[] = {0x00};
    TEST_ASSERT_EQ(ks_crc8(d, 1), 0x00, "crc8([0x00]) = 0x00");
}

/* Vecteur 3 : octet 0x01 → CRC = 0x31 (calculé à la main, poly 0x31 MSB-first)
 * 0x01 → shift ×7 → 0x80 → MSB=1 → (0x80<<1)^0x31 = 0x00^0x31 = 0x31 */
static void test_crc8_single_nonzero_byte(void)
{
    uint8_t d[] = {0x01};
    TEST_ASSERT_EQ(ks_crc8(d, 1), 0x31, "crc8([0x01]) = 0x31");
}

/* Vecteur 4 : multi-octets — adosse le vecteur publié dans
 * docs/CDC_BINARY_PROTOCOL.md ([0x4B,0x53] → 0xBE) */
static void test_crc8_multibyte_doc_vector(void)
{
    uint8_t d[] = {0x4B, 0x53};
    TEST_ASSERT_EQ(ks_crc8(d, 2), 0xBE, "crc8([0x4B,0x53]) = 0xBE (doc)");
}

/* Vecteur 5 : déterminisme — même entrée donne toujours la même sortie */
static void test_crc8_deterministic(void)
{
    uint8_t d[] = {0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_EQ(ks_crc8(d, 4), ks_crc8(d, 4),
                   "crc8 déterministe sur la même donnée");
}

/* ══ Suite parser ks_rx_feed ════════════════════════════════════════════ */

/* Cas 1 : frame valide passée d'un coup */
static void test_valid_frame_all_at_once(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "frame entière : tous les octets consommés");
    TEST_ASSERT(ks_process_one(),
                "ks_process_one retourne true après frame valide");
    TEST_ASSERT(fake_rx_called,       "handler appelé");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING,  "cmd_id = PING");
    TEST_ASSERT_EQ(fake_rx_len, 0,    "payload_len = 0");
}

/* Cas 2 : même frame livrée octet par octet */
static void test_valid_frame_byte_by_byte(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);

    for (size_t i = 0; i < flen; i++) {
        uint16_t c = ks_rx_feed((const char *)(frame + i), 1);
        TEST_ASSERT_EQ(c, 1, "chaque octet individuel est consommé (retour 1)");
    }
    TEST_ASSERT(ks_process_one(),
                "frame octet par octet : ks_process_one true");
    TEST_ASSERT(fake_rx_called, "handler appelé après feed octet par octet");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING, "cmd_id correct après feed incrémental");
}

/* Cas 3 : frame avec payload non-vide */
static void test_frame_with_nonempty_payload(void)
{
    reset_state();
    uint8_t payload[] = {0xAB, 0xCD};
    uint8_t frame[32];
    size_t flen = build_ks_frame(frame, KS_CMD_VERSION,
                                 payload, sizeof(payload));

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "frame avec payload : tous octets consommés");
    TEST_ASSERT(ks_process_one(), "frame avec payload : ks_process_one true");
    TEST_ASSERT_EQ(fake_rx_len, 2, "payload_len = 2");
    TEST_ASSERT_EQ(fake_rx_payload[0], 0xAB, "payload[0] = 0xAB");
    TEST_ASSERT_EQ(fake_rx_payload[1], 0xCD, "payload[1] = 0xCD");
}

/* Cas 4 : bruit avant le magic → retour 0 immédiat (pas d'avance dans le stream) */
static void test_noise_before_magic_returns_zero(void)
{
    reset_state();

    /* En état IDLE, tout octet ≠ 0x4B provoque un return 0 immédiat.
     * Le parser ne scanne PAS le buffer à la recherche du magic — c'est au
     * caller d'avancer octet par octet. */
    uint8_t noise[] = {0xFF};
    uint16_t c = ks_rx_feed((const char *)noise, 1);
    TEST_ASSERT_EQ(c, 0,
                   "octet de bruit en IDLE : retour 0 (pas de consommation)");

    /* Après le bruit, l'état est encore IDLE : une frame valide passe. */
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    c = ks_rx_feed((const char *)frame, (uint16_t)flen);
    TEST_ASSERT_EQ(c, (int)flen,
                   "frame valide après bruit : consommée normalement");
    TEST_ASSERT(ks_process_one(),
                "frame valide après bruit : ks_process_one true");
}

/* Cas 5 : magic1 correct (0x4B) mais magic2 incorrect → return 0, état reset IDLE
 *
 * Comportement réel : la fonction a déjà mis consumed=1 (pour le 0x4B),
 * mais retourne 0 (pas consumed). Le caller interprète ça comme "zéro octets
 * consommés en binaire" et traite 0x4B comme texte. */
static void test_bad_magic2_returns_zero_and_resets(void)
{
    reset_state();

    /* 0x4B=magic0 correct, 0x52=KR_MAGIC_1 ≠ KS_MAGIC_1 */
    uint8_t bad[] = {KS_MAGIC_0, KR_MAGIC_1};
    uint16_t c = ks_rx_feed((const char *)bad, 2);
    TEST_ASSERT_EQ(c, 0,
                   "magic2 invalide : retour 0 (pas consumed)");

    /* L'état est revenu à IDLE : une frame valide s'en suit sans problème. */
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    c = ks_rx_feed((const char *)frame, (uint16_t)flen);
    TEST_ASSERT_EQ(c, (int)flen, "frame après bad magic2 : consommée");
    TEST_ASSERT(ks_process_one(),  "frame après bad magic2 : ok");
}

/* Cas 6 : CRC incorrect → frame rejetée, réponse KR ERR_CRC envoyée */
static void test_bad_crc_frame_rejected(void)
{
    reset_state();
    uint8_t frame[16];
    size_t flen = build_ks_frame(frame, KS_CMD_PING, NULL, 0);
    /* Corrompt le dernier octet (le CRC) */
    frame[flen - 1] ^= 0xFF;

    uint16_t consumed = ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT_EQ(consumed, (int)flen,
                   "frame bad-CRC : tous les octets sont quand même consommés");
    TEST_ASSERT(!ks_process_one(),
                "ks_process_one retourne false (frame rejetée, ready=false)");

    /* Une réponse d'erreur KR doit avoir été envoyée via CDC. */
    TEST_ASSERT(fake_cdc_pos > 0, "réponse KR envoyée sur la sortie CDC");
    TEST_ASSERT_EQ(fake_cdc_buf[0], KS_MAGIC_0,     "réponse KR : magic0=0x4B");
    TEST_ASSERT_EQ(fake_cdc_buf[1], KR_MAGIC_1,     "réponse KR : magic1=0x52 (R)");
    TEST_ASSERT_EQ(fake_cdc_buf[2], KS_CMD_PING,    "réponse KR : cmd_id=PING");
    TEST_ASSERT_EQ(fake_cdc_buf[3], KS_STATUS_ERR_CRC,
                   "réponse KR : status=ERR_CRC (0x02)");
}

/* Cas 7 : CRC correct → frame acceptée (complément du cas 6) */
static void test_correct_crc_frame_accepted(void)
{
    reset_state();
    uint8_t payload[] = {0xCA, 0xFE};
    uint8_t frame[32];
    size_t flen = build_ks_frame(frame, KS_CMD_VERSION,
                                 payload, sizeof(payload));

    ks_rx_feed((const char *)frame, (uint16_t)flen);

    TEST_ASSERT(ks_process_one(),  "CRC correct : ks_process_one true");
    TEST_ASSERT(fake_rx_called,    "CRC correct : handler appelé");
    TEST_ASSERT_EQ(fake_rx_len, 2, "CRC correct : payload_len = 2");
    /* Aucune réponse d'erreur ne doit avoir été envoyée. */
    TEST_ASSERT_EQ(fake_cdc_pos, 0,
                   "CRC correct : aucune réponse d'erreur envoyée");
}

/* Cas 8 : payload_len > KS_PAYLOAD_MAX → rejet immédiat, réponse ERR_OVERFLOW
 *
 * Comportement réel :
 *  - ks_respond_err(cmd, ERR_OVERFLOW) est appelé dès la fin du header
 *  - L'état reset à IDLE (les octets "payload" dans le buffer continuent
 *    d'être lus par la boucle, mais en état IDLE chaque octet ≠ 0x4B
 *    provoquerait un return 0 prématuré — voir note dans le rapport)
 *  - Ici on n'envoie PAS de bytes payload après le header pour éviter
 *    l'ambiguïté (voir comportement noté dans le rapport).
 */
static void test_oversized_payload_rejected(void)
{
    reset_state();

    /* payload_len = 0x1001 = 4097 > KS_PAYLOAD_MAX (4096) */
    uint8_t frame[] = {
        KS_MAGIC_0, KS_MAGIC_1,
        KS_CMD_PING,
        0x01, 0x10  /* len_lo=0x01, len_hi=0x10 → 0x1001 = 4097 */
    };

    uint16_t consumed = ks_rx_feed((const char *)frame, sizeof(frame));

    /* Tous les octets du header (5) sont consommés. */
    TEST_ASSERT_EQ(consumed, 5,
                   "overflow : les 5 octets du header sont consommés");

    /* Réponse d'erreur ERR_OVERFLOW envoyée. */
    TEST_ASSERT(fake_cdc_pos > 0,
                "overflow : réponse KR envoyée");
    TEST_ASSERT_EQ(fake_cdc_buf[0], KS_MAGIC_0,          "overflow : magic0");
    TEST_ASSERT_EQ(fake_cdc_buf[3], KS_STATUS_ERR_OVERFLOW,
                   "overflow : status=ERR_OVERFLOW (0x06)");

    /* Pas de frame prête. */
    TEST_ASSERT(!ks_process_one(),
                "overflow : ks_process_one false (aucune frame prête)");
}

/* Cas 9 : overflow + octet non-magic suivant → consumed = header (pas 0)
 *
 * Régression : après un overflow l'état revient à IDLE ; si un octet
 * non-magic suit dans le même buffer, le parser doit refléter les octets
 * déjà consommés (le header), pas retourner 0 (ce qui ferait croire au
 * caller que rien n'a été consommé alors que l'erreur a été envoyée). */
static void test_oversized_then_garbage_consumes_header(void)
{
    reset_state();
    uint8_t buf[] = {
        KS_MAGIC_0, KS_MAGIC_1, KS_CMD_PING,
        0x01, 0x10,   /* len = 0x1001 = 4097 > KS_PAYLOAD_MAX */
        0xFF          /* octet non-magic après l'overflow */
    };
    uint16_t c = ks_rx_feed((const char *)buf, sizeof(buf));
    TEST_ASSERT_EQ(c, 5,
                   "overflow + octet non-magic : consumed = 5 (header), pas 0");
}

/* Cas 10 : overflow + octet parasite + frame valide → la frame n'est PAS perdue
 *
 * Régression : un return 0 prématuré en IDLE après l'overflow stoppait le
 * scan et faisait perdre une frame valide située plus loin dans le buffer. */
static void test_oversized_then_garbage_then_valid_frame(void)
{
    reset_state();
    uint8_t valid[16];
    size_t vlen = build_ks_frame(valid, KS_CMD_PING, NULL, 0);

    uint8_t buf[6 + 16];
    buf[0] = KS_MAGIC_0; buf[1] = KS_MAGIC_1; buf[2] = KS_CMD_VERSION;
    buf[3] = 0x01; buf[4] = 0x10;   /* header oversized */
    buf[5] = 0xFF;                  /* octet parasite */
    memcpy(buf + 6, valid, vlen);

    ks_rx_feed((const char *)buf, (uint16_t)(6 + vlen));
    TEST_ASSERT(ks_process_one(),
                "frame valide après overflow+parasite : parsée (non perdue)");
    TEST_ASSERT(fake_rx_called,
                "handler appelé pour la frame qui suit l'overflow");
    TEST_ASSERT_EQ(fake_rx_cmd, KS_CMD_PING, "cmd de la frame rescapée = PING");
}

/* ══ Point d'entrée de la suite ════════════════════════════════════════ */

void test_cdc_rx_feed(void)
{
    printf("\n--- cdc_rx_feed / ks_crc8 ---\n");

    /*
     * Les tables de commandes sont statiques dans cdc_binary_protocol.c.
     * On enregistre une seule fois pour toute la suite — ks_rx_reset()
     * ne touche pas ces tables.
     */
    ks_register_binary_commands(
        test_cmd_table,
        sizeof(test_cmd_table) / sizeof(test_cmd_table[0]));

    /* CRC-8 vectors */
    TEST_RUN(test_crc8_empty_payload);
    TEST_RUN(test_crc8_single_zero_byte);
    TEST_RUN(test_crc8_single_nonzero_byte);
    TEST_RUN(test_crc8_multibyte_doc_vector);
    TEST_RUN(test_crc8_deterministic);

    /* Parser ks_rx_feed */
    TEST_RUN(test_valid_frame_all_at_once);
    TEST_RUN(test_valid_frame_byte_by_byte);
    TEST_RUN(test_frame_with_nonempty_payload);
    TEST_RUN(test_noise_before_magic_returns_zero);
    TEST_RUN(test_bad_magic2_returns_zero_and_resets);
    TEST_RUN(test_bad_crc_frame_rejected);
    TEST_RUN(test_correct_crc_frame_accepted);
    TEST_RUN(test_oversized_payload_rejected);
    TEST_RUN(test_oversized_then_garbage_consumes_header);
    TEST_RUN(test_oversized_then_garbage_then_valid_frame);
}
