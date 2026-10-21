/* The nRF24 bench knobs and their SHIPPED values.
 *
 * PURE (host-tested, test/test_rf_settle.c). Three Kconfig symbols exist for
 * rf-scope bench experiments only: KASE_RF_FEATURE, KASE_RF_SETUP_RETR,
 * KASE_RF_RX_SETTLE_US (dongle). A build_<board>/sdkconfig KEEPS a value set
 * there by menuconfig — a later change of the Kconfig default does not reach
 * it — so a bench value can silently ship (review I4). Hence:
 *  - the firmware flags any non-shipped value: ESP_LOGW once at boot
 *    (rf_driver.c), in the halves' HB suffix, and in the `bench` byte of every
 *    rf-scope periodic record (both ends — the dongle has no console);
 *  - scripts/build_release.sh refuses to package a board whose sdkconfig
 *    carries a non-shipped value (it reads the values below from this file).
 *
 * The values below ARE the Kconfig defaults; test_rf_bench_shipped_match_kconfig
 * pins them against main/Kconfig.projbuild. Never change one without an
 * `rf_scope stress` measurement on the bench (COMPORTEMENTS.md, RF stress).
 */
#ifndef RF_BENCH_H
#define RF_BENCH_H

#include <stdint.h>
#include <stddef.h>

#define RF_SHIPPED_FEATURE        0x06   /* EN_DPL | EN_ACK_PAY */
#define RF_SHIPPED_SETUP_RETR     0x1F   /* ARD 500 us, ARC 15 */
#define RF_SHIPPED_RX_SETTLE_US   500    /* dongle only (rf_settle.h) */

/* Bits of the flag byte (also the rf-scope periodic record's `bench`). */
#define RF_BENCH_FEATURE  0x01u
#define RF_BENCH_RETR     0x02u
#define RF_BENCH_SETTLE   0x04u

/* Which knobs differ from their shipped value. A role without the settle
 * knob passes settle_us == settle_shipped (e.g. 0, 0). */
uint8_t rf_bench_flags(uint8_t feature, uint8_t setup_retr,
                       uint32_t settle_us, uint32_t settle_shipped);

/* "" when flags == 0, else " BENCH-RF(feature=0x04 retr=0x2F settle=200us)"
 * naming only the knobs that differ — the leading space makes it an HB
 * suffix. Always NUL-terminated, truncated to cap. */
void rf_bench_describe(uint8_t flags, uint8_t feature, uint8_t setup_retr,
                       uint32_t settle_us, char *out, size_t cap);

#endif /* RF_BENCH_H */
