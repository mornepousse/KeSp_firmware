/* main/security/apdu.c — ISO 7816-4 command-APDU parser */
#include "apdu.h"
#include <string.h>

bool apdu_parse(const uint8_t *buf, uint16_t len, apdu_t *out)
{
    if (!buf || !out || len < 4) return false;

    memset(out, 0, sizeof(*out));
    out->cla = buf[0];
    out->ins = buf[1];
    out->p1  = buf[2];
    out->p2  = buf[3];

    /* Case 1: header only */
    if (len == 4) return true;

    /*
     * Extended-length indicator: fifth byte == 0x00 AND total length >= 7.
     * A 5-byte APDU with fifth byte 0x00 is Case 2S (Le=256), not extended.
     */
    if (buf[4] == 0x00 && len >= 7) {
        uint16_t ext = ((uint16_t)buf[5] << 8) | buf[6];

        if (len == 7) {
            /* Case 2E: extended Le only (0x0000 means 65536 per ISO) */
            out->le         = ext;
            out->le_present = true;
            return true;
        }

        /* Case 3E / 4E: ext encodes Lc */
        {
            uint16_t    lc       = ext;
            uint32_t    data_end = (uint32_t)7 + lc;

            if ((uint32_t)len < data_end) return false; /* truncated */

            out->lc   = lc;
            out->data = buf + 7;

            if ((uint32_t)len == data_end) return true; /* Case 3E */

            if ((uint32_t)len == data_end + 2) {
                /* Case 4E: two-byte Le follows the data */
                out->le         = ((uint16_t)buf[data_end] << 8)
                                  | buf[data_end + 1];
                out->le_present = true;
                return true;
            }
        }
        return false; /* unexpected length */
    }

    /* buf[4]==0x00 and len==5: Case 2S, Le = 0 (means 256 in ISO short form) */
    if (len == 5) {
        out->le         = buf[4];
        out->le_present = true;
        return true;
    }

    /* buf[4]==0x00 and len==6: ambiguous extended start — reject */
    if (buf[4] == 0x00) return false;

    /* Short form with non-zero Lc (Case 3S / 4S) */
    {
        uint8_t  lc       = buf[4];
        uint16_t data_end = (uint16_t)(5 + lc);

        if (len < data_end) return false; /* truncated */

        out->lc   = lc;
        out->data = buf + 5;

        if (len == data_end) return true; /* Case 3S */

        if (len == (uint16_t)(data_end + 1)) {
            /* Case 4S: one-byte Le */
            out->le         = buf[data_end];
            out->le_present = true;
            return true;
        }
    }

    return false; /* unexpected length */
}
