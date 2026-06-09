#include "test_framework.h"
#include "openpgp_do.h"
#include <string.h>

/* ---- put / get round-trip + unset not-found (from plan) ---- */
static void test_put_get(void)
{
    openpgp_do_reset();

    uint8_t fp[20];
    memset(fp, 0xAB, 20);

    TEST_ASSERT(openpgp_do_put(0xC5, fp, 20), "put C5");

    const uint8_t *v;
    uint16_t n;
    TEST_ASSERT(openpgp_do_get(0xC5, &v, &n), "get C5");
    TEST_ASSERT_EQ(n, 20, "len");
    TEST_ASSERT(memcmp(v, fp, 20) == 0, "value matches");

    /* DO 0x5B was never written */
    TEST_ASSERT(!openpgp_do_get(0x5B, &v, &n), "unset DO not found");
}

/* ---- overwrite existing tag updates the value ---- */
static void test_overwrite(void)
{
    openpgp_do_reset();

    uint8_t a[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t b[4] = {0xAA, 0xBB, 0xCC, 0xDD};

    TEST_ASSERT(openpgp_do_put(0xD6, a, 4), "first put");
    TEST_ASSERT(openpgp_do_put(0xD6, b, 4), "overwrite same tag");

    const uint8_t *v;
    uint16_t n;
    TEST_ASSERT(openpgp_do_get(0xD6, &v, &n), "get after overwrite");
    TEST_ASSERT(memcmp(v, b, 4) == 0, "overwritten value visible");
}

/* ---- oversized payload must be rejected ---- */
static void test_oversized_rejected(void)
{
    openpgp_do_reset();

    /* Allocate a buffer larger than OPENPGP_DO_MAX_LEN */
    uint8_t big[OPENPGP_DO_MAX_LEN + 1];
    memset(big, 0xFF, sizeof(big));

    TEST_ASSERT(!openpgp_do_put(0xC5, big, OPENPGP_DO_MAX_LEN + 1),
                "oversized DO rejected");
}

/* ---- reset wipes all stored DOs ---- */
static void test_reset_clears(void)
{
    openpgp_do_reset();

    uint8_t data[2] = {0x01, 0x20};
    TEST_ASSERT(openpgp_do_put(0xD6, data, 2), "put before reset");

    openpgp_do_reset();

    const uint8_t *v;
    uint16_t n;
    TEST_ASSERT(!openpgp_do_get(0xD6, &v, &n), "DO gone after reset");
}

void test_openpgp_do(void)
{
    TEST_SUITE("openpgp DO store");
    TEST_RUN(test_put_get);
    TEST_RUN(test_overwrite);
    TEST_RUN(test_oversized_rejected);
    TEST_RUN(test_reset_clears);
}
