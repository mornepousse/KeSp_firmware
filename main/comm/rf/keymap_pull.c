/* Pulling the dongle's keymap via ACK payload — see keymap_pull.h.
 * Moved as-is from kbd_relay_tx.c (2026-09-19), not rewritten. */
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
static volatile bool s_sync_done;   /* 40/40 received: to be saved (tick) */

bool keymap_pull_en_cours(void) { return s_syncing || s_sync_done; }

void keymap_pull_on_ack(const uint8_t *ack, uint8_t ack_n)
{
    if (!ack_n || s_sync_done) return;
    rf_sync_beacon_t b;
    rf_sync_chunk_t  c;
    if (rf_decode_sync_beacon(ack, ack_n, &b)) {
        /* A keymap is waiting for us. We (re)start from zero if this is a different
         * fingerprint than the one we were already pulling — the dongle changed again. */
        uint32_t own = config_fp_crc32((const uint8_t *)keymaps, KEYMAP_BLOB_BYTES);
        if (b.fp_target != own && (!s_syncing || b.fp_target != s_sync_target_fp)) {
            keymap_rx_reset(&s_krx);
            s_syncing = true;
#if CONFIG_KASE_VEILLE
            veille_veto(VEILLE_VETO_SYNC, true);   /* no sleep while pulling */
#endif
            s_sync_target_fp = b.fp_target;
            ESP_LOGW(TAG, "sync keymap: beacon fp=0x%08X (ours 0x%08X), %u chunks — pull",
                     (unsigned)b.fp_target, (unsigned)own, (unsigned)b.n_chunks);
        }
    } else if (s_syncing && rf_decode_sync_chunk(ack, ack_n, &c)) {
        if (keymap_rx_chunk(&s_krx, c.idx, c.data) && keymap_rx_complete(&s_krx)) {
            s_syncing  = false;
            s_sync_done = true;   /* the tick saves it, off the TX path */
        }
    }
}

bool keymap_pull_tick(void (*emettre)(const uint8_t *, uint8_t))
{
    /* 40/40 received: the complete keymap is in s_krx.buf. The flag and the
     * buffer are WRITTEN during a transmit (on_ack, under the owner's
     * lock, from the scan task): we CONSUME them under that same
     * lock (radio_lock), otherwise the copy could read a buffer not yet
     * fully visible (2026-09-13 review). The NVS write (ms) then happens
     * OUTSIDE the lock. The next STATUS will announce the new fingerprint ->
     * the dongle will stop the beacon. */
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
        ESP_LOGW(TAG, "sync keymap: 40/40 received, fp=0x%08X %s (target 0x%08X) — %s",
                 (unsigned)fp, fp == cible ? "= target" : "!= TARGET", (unsigned)cible,
                 saved ? "saved to NVS" : "NVS FAILURE");
#if CONFIG_KASE_VEILLE
        veille_veto(VEILLE_VETO_SYNC, false);   /* pull done and saved */
#endif
    }
    if (!s_syncing) return false;
    /* One REQ every 100 ms (gated by timestamp: the timer runs at
     * KBD_RELAY_REFRESH_MS = 10 ms, we must NOT send one REQ per tick — the
     * 2026-09-13 review found 100 REQ/s actual against 10 documented).
     * Each ACK reports the requested chunk -> ~10 chunks/s, 40 in ~4 s.
     * Bounded traffic, only while we diverge. */
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
