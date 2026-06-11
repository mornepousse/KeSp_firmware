/* main/security/otp_hid.c — OTP HID transport: real hooks (dongle only).
 *
 * Wires otp_proto to:
 *   - sec_store: slot secret retrieval
 *   - sec_confirm: physical keypress gate
 *   - cr_hmac: HMAC-SHA1 computation
 *
 * Slot mapping (fixed, per plan):
 *   OTP_SLOT_CHAL_HMAC1 (0x30) -> sec_store slot 0
 *   OTP_SLOT_CHAL_HMAC2 (0x38) -> sec_store slot 1
 */
#include "otp_hid.h"
#include "otp_proto.h"
#include "cr_hmac.h"
#include "sec_store.h"
#include "sec_confirm.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "otp_hid";

/* ---------- helpers --------------------------------------------------- */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** Map YubiKey OTP slot byte to sec_store index; -1 if unknown. */
static int otp_slot_to_store_idx(uint8_t otp_slot)
{
    if (otp_slot == OTP_SLOT_CHAL_HMAC1) return 0;
    if (otp_slot == OTP_SLOT_CHAL_HMAC2) return 1;
    return -1;
}

/* ---------- real hooks ------------------------------------------------ */

/**
 * Compute HMAC-SHA1 for the given challenge.
 *   slot     : OTP_SLOT_CHAL_HMAC1 or OTP_SLOT_CHAL_HMAC2
 *   challenge: 64-byte challenge (padded by KeePassXC)
 *   out20    : 20-byte HMAC digest output
 * Returns false if the slot is empty or the HMAC fails.
 */
static bool hook_compute_hmac(uint8_t slot,
                               const uint8_t challenge[64],
                               uint8_t out20[20])
{
    int idx = otp_slot_to_store_idx(slot);
    if (idx < 0) {
        ESP_LOGW(TAG, "compute_hmac: unknown slot 0x%02x", slot);
        return false;
    }

    uint8_t secret[SEC_SECRET_MAX];
    uint8_t secret_len = 0;
    if (!sec_store_get_secret((uint8_t)idx, secret, &secret_len) || secret_len == 0) {
        ESP_LOGW(TAG, "compute_hmac: slot %d empty", idx);
        return false;
    }

    bool ok = cr_hmac_sha1(secret, secret_len, challenge, 64, out20);
    /* Scrub key material from stack before returning */
    memset(secret, 0, sizeof(secret));
    return ok;
}

/**
 * Arm the physical confirmation gate for the given slot.
 * Called by otp_proto when a valid challenge frame arrives.
 */
/* Store-slot this OTP transaction armed sec_confirm for; -1 = none.
 * hook_confirm_state() checks the granted slot against this so a touch armed
 * for one slot can never authorise a different slot's HMAC (mirrors the CCID
 * `slot == CCID_CONFIRM_SLOT` check in ccid.c). */
static int s_armed_idx = -1;

static void hook_confirm_arm(uint8_t slot)
{
    int idx = otp_slot_to_store_idx(slot);
    if (idx < 0) return;
    s_armed_idx = idx;
    sec_confirm_arm((uint8_t)idx, now_ms());
    ESP_LOGI(TAG, "confirm armed for store slot %d", idx);
}

/**
 * Poll the confirmation gate state.
 * Returns: 0 = still waiting, 1 = authorized, 2 = denied/timeout.
 */
static int hook_confirm_state(void)
{
    uint8_t out_slot = 0;
    sec_confirm_state_t st = sec_confirm_poll(now_ms(), &out_slot);
    switch (st) {
        case SEC_CONFIRM_AUTHORIZED:
            /* Only accept a grant for the slot THIS transaction armed —
             * reject a touch armed for another slot (defence-in-depth). */
            return (s_armed_idx >= 0 && out_slot == (uint8_t)s_armed_idx) ? 1 : 2;
        case SEC_CONFIRM_TIMEDOUT:   return 2;
        default:                     return 0;
    }
}

/* ---------- init ------------------------------------------------------ */

static const otp_proto_hooks_t s_real_hooks = {
    .compute_hmac  = hook_compute_hmac,
    .confirm_state = hook_confirm_state,
    .confirm_arm   = hook_confirm_arm,
};

void otp_hid_init(void)
{
    otp_proto_init(&s_real_hooks);
    ESP_LOGI(TAG, "OTP HID initialized (slots: 0x30->slot0, 0x38->slot1)");
}
