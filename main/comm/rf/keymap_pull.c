/* Tirage de la keymap du dongle par ACK payload — voir keymap_pull.h.
 * Déplacé tel quel de kbd_relay_tx.c (2026-09-19), pas réécrit. */
#include "keymap_pull.h"
#include "radio_owner.h"
#include "rf_packet.h"
#include "keymap.h"         /* keymaps[], KEYMAP_BLOB_BYTES, save_keymaps */
#include "config_sync.h"    /* config_fp_crc32 */
#include "keymap_sync.h"    /* keymap_rx_* */
#if CONFIG_KASE_VEILLE
#include "veille_task.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "keymap_pull";

static keymap_rx_t   s_krx;
static bool          s_syncing;
static uint32_t      s_sync_target_fp;
static volatile bool s_sync_done;   /* 40/40 reçus : à enregistrer (tick) */

bool keymap_pull_en_cours(void) { return s_syncing || s_sync_done; }

void keymap_pull_on_ack(const uint8_t *ack, uint8_t ack_n)
{
    if (!ack_n || s_sync_done) return;
    rf_sync_beacon_t b;
    rf_sync_chunk_t  c;
    if (rf_decode_sync_beacon(ack, ack_n, &b)) {
        /* Une keymap nous attend. On (re)part de zéro si c'est une autre
         * empreinte que celle qu'on tirait déjà — le dongle a rechangé. */
        uint32_t own = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
        if (b.fp_target != own && (!s_syncing || b.fp_target != s_sync_target_fp)) {
            keymap_rx_reset(&s_krx);
            s_syncing = true;
#if CONFIG_KASE_VEILLE
            veille_veto(VEILLE_VETO_SYNC, true);   /* pas de veille en plein tirage */
#endif
            s_sync_target_fp = b.fp_target;
            ESP_LOGW(TAG, "sync keymap : balise fp=0x%08X (la nôtre 0x%08X), %u chunks — pull",
                     (unsigned)b.fp_target, (unsigned)own, (unsigned)b.n_chunks);
        }
    } else if (s_syncing && rf_decode_sync_chunk(ack, ack_n, &c)) {
        if (keymap_rx_chunk(&s_krx, c.idx, c.data) && keymap_rx_complete(&s_krx)) {
            s_syncing  = false;
            s_sync_done = true;   /* le tick enregistre, hors du chemin TX */
        }
    }
}

bool keymap_pull_tick(void (*emettre)(const uint8_t *, uint8_t))
{
    /* 40/40 reçus : la keymap complète est dans s_krx.buf. Le drapeau et le
     * buffer sont ÉCRITS pendant une émission (on_ack, sous le verrou du
     * propriétaire, depuis la tâche de scan) : on les CONSOMME sous ce même
     * verrou (radio_lock), sinon la copie pourrait lire un buffer pas encore
     * entièrement visible (revue 2026-09-13). La NVS (ms) se fait ensuite HORS
     * verrou. Le STATUS suivant annoncera la nouvelle empreinte → le dongle
     * coupera la balise. */
    bool a_enregistrer = false;
    uint32_t cible = 0;
    if (s_sync_done && radio_lock(20)) {
        if (s_sync_done) {
            memcpy((uint8_t *)keymaps, s_krx.buf, KEYMAP_BLOB_BYTES);
            cible = s_sync_target_fp;
            s_sync_done = false;
            a_enregistrer = true;
        }
        radio_unlock();
    }
    if (a_enregistrer) {
        bool saved = save_keymaps((uint16_t *)keymaps, KEYMAP_BLOB_BYTES);
        uint32_t fp = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
        ESP_LOGW(TAG, "sync keymap : 40/40 recus, fp=0x%08X %s (cible 0x%08X) — %s",
                 (unsigned)fp, fp == cible ? "= cible" : "!= CIBLE", (unsigned)cible,
                 saved ? "enregistree en NVS" : "ECHEC NVS");
#if CONFIG_KASE_VEILLE
        veille_veto(VEILLE_VETO_SYNC, false);   /* tirage fini et enregistré */
#endif
    }
    if (!s_syncing) return false;
    /* Un REQ toutes les 100 ms (gate par horodatage : le timer tourne à
     * KBD_RELAY_REFRESH_MS = 10 ms, il ne faut PAS un REQ par tick — la
     * revue du 2026-09-13 a relevé 100 REQ/s réels contre 10 documentés).
     * Chaque ACK rapporte le chunk demandé → ~10 chunks/s, 40 en ~4 s.
     * Trafic borné, seulement tant qu'on diverge. */
    static uint32_t s_dernier_req_ms;
    uint32_t maintenant = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(maintenant - s_dernier_req_ms) >= 100u) {
        s_dernier_req_ms = maintenant;
        rf_sync_req_t q = { .next = keymap_rx_next(&s_krx) };
        uint8_t rb[4];
        uint16_t rn = rf_encode_sync_req(rb, &q);
        emettre(rb, (uint8_t)rn);
    }
    return true;
}
