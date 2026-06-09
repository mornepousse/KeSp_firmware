#include "test_framework.h"
#include "cr_crc16.h"

static void test_crc16_empty(void)
{
    /* CRC-16 of zero bytes = init value 0xFFFF (no bytes processed) */
    TEST_ASSERT_EQ(cr_crc16(NULL, 0), 0xFFFF, "crc16 of empty = 0xFFFF");
}

static void test_crc16_residual(void)
{
    /* YubiKey frames store the COMPLEMENT (~crc) of the raw CRC-16, not the
     * raw value directly (ykpers: frame.crc = ~yubikey_crc16(data, 65)).
     * When the complemented CRC bytes are fed back into cr_crc16 together
     * with the data, the register yields the constant residual 0xF0B8. */
    uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};
    uint16_t crc = cr_crc16(data, 4);
    /* Append ~crc LSB-first (as ykpers stores it in the frame) */
    uint8_t framed[6] = {0x01, 0x02, 0x03, 0x04,
                         (uint8_t)((crc ^ 0xFFFF) & 0xFF),
                         (uint8_t)((crc ^ 0xFFFF) >> 8)};
    TEST_ASSERT_EQ(cr_crc16(framed, 6), 0xF0B8, "crc16 residual over data+~crc = 0xF0B8");
}

void test_cr_crc16(void)
{
    TEST_SUITE("cr_crc16 (YubiKey CRC-16)");
    TEST_RUN(test_crc16_empty);
    TEST_RUN(test_crc16_residual);
}
