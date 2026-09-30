/* Width-oracle gate: the layout of the left screen is decided on host by
 * memlcd_text_width() (memlcd_model.h + the generated memlcd_font_widths.h,
 * kerning included); the panel draws with LVGL. This program measures the
 * same strings both ways — every printable ASCII pair, the reference
 * labels, and a deterministic fuzz — for every font of the ladder and
 * fails on the first disagreement: the tests prove what the panel shows
 * only while the two agree. */
#include "lvgl.h"
#include "memlcd_model.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);

static const lv_font_t *const k_fonts[] = {
    [MEMLCD_F_M12] = &lv_font_montserrat_12, [MEMLCD_F_M14] = &lv_font_montserrat_14,
    [MEMLCD_F_M16] = &lv_font_montserrat_16, [MEMLCD_F_M18] = &lv_font_montserrat_18,
    [MEMLCD_F_M20] = &lv_font_montserrat_20, [MEMLCD_F_M24] = &lv_font_montserrat_24,
    [MEMLCD_F_M28] = &lv_font_montserrat_28, [MEMLCD_F_M32] = &lv_font_montserrat_32,
};

static int s_fail, s_n;

static void one(uint8_t f, const char *s)
{
    /* LVGL reads the kerning partner past `length` — measure a copy that
     * ends where the line ends, as a label holding just that line does. */
    char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    int lv = lv_txt_get_width(buf, (uint32_t)strlen(buf), k_fonts[f], 0, LV_TEXT_FLAG_NONE);
    int or = memlcd_text_width(f, buf);
    s_n++;
    if (lv != or) {
        if (s_fail < 20) printf("  MISMATCH font %d \"%s\": LVGL %d, oracle %d\n", f, buf, lv, or);
        s_fail++;
    }
}

int main(void)
{
    lv_init();
    static const char *const refs[] = {
        "BASE", "NAVIGATION", "RADIO", "SEEN", "USB", "100%", "TOTP", "RESET!", "PRESS", "12 ACCTS",
        "TEST:RFC6238", "BANQUE:4021", "GITHUB.COM:ALICE.MARTIN@WORK-2FA01",
        "github.com:alice.martin@work-2fa01", "OVH:PERSO", "NO CARD", "CAPS CW", "CSAG L3", "418902",
        "AV", "To", "Ty", "LT", "WA", "r.", "f\"", "Yo",
    };
    for (uint8_t f = MEMLCD_F_M12; f <= MEMLCD_F_M32; f++) {
        for (size_t i = 0; i < sizeof refs / sizeof refs[0]; i++) one(f, refs[i]);
        for (int a = 0x20; a <= 0x7E; a++)
            for (int b = 0x20; b <= 0x7E; b++) { char s[3] = { (char)a, (char)b, 0 }; one(f, s); }
        uint32_t x = 12345u + f;
        for (int k = 0; k < 2000; k++) {
            char s[35]; int len = 1 + (int)((x >> 8) % 34);
            for (int i = 0; i < len; i++) { x = x * 1103515245u + 12345u; s[i] = (char)(0x20 + (x >> 16) % 95); }
            s[len] = 0;
            one(f, s);
        }
    }
    printf("WIDTH ORACLE: %d strings, %d mismatch(es) -- %s\n", s_n, s_fail, s_fail ? "FAIL" : "OK");
    return s_fail ? 1 : 0;
}
