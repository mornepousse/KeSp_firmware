/* Safe-wrap gate, measured by LVGL itself: the firmware's memlcd_safe_wrap
 * (main/display/memlcd/memlcd_safe_wrap.h) decides with the width oracle;
 * this program re-measures every line it produces with lv_txt_get_width in
 * the real font and fails if one is wider than the budget, or holds digits
 * without a letter (bench incident 2026-09-29: "TEST:RFC6238" pixel-cut to
 * "TEST:RFC" / "6238" read as a code). The host tests pin the expected
 * lines; this gate proves the panel draws them within their width. */
#include "lvgl.h"
#include "memlcd_safe_wrap.h"
#include "memlcd_cave.h"      /* MEMLCD_CAVE_MARQUE_PAS */
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);

static int g_fail;

static bool lines_ok(uint8_t font, uint16_t budget, char lines[][MEMLCD_SW_LINE_BUF], int n)
{
    for (int i = 0; i < n; i++) if (memlcd_text_width(font, lines[i]) > budget) return false;
    return true;
}

static void run(const char *label, uint8_t font, const lv_font_t *lf, const char *fname, uint16_t budget)
{
    char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    int n = memlcd_safe_wrap(label, font, budget, lines, MEMLCD_SW_MAX_LINES);
    bool ok_all = lines_ok(font, budget, lines, n);
    printf("\"%s\" @ %s budget=%upx -> %d line(s)%s:\n", label, fname, budget, n, ok_all ? "" : " (no safe split: rejoined)");
    for (int i = 0; i < n; i++) {
        bool unsafe = memlcd_sw_line_is_unsafe(lines[i], strlen(lines[i]));
        lv_coord_t w = lv_txt_get_width(lines[i], (uint32_t)strlen(lines[i]), lf, 0, LV_TEXT_FLAG_NONE);
        bool over = w > budget;
        printf("  [%d] \"%s\" (%dpx)%s%s\n", i, lines[i], (int)w, unsafe ? "  <<< UNSAFE" : "", over ? "  <<< OVER" : "");
        if (unsafe && n > 1) g_fail++;
        if (over && ok_all) g_fail++;              /* the oracle said it fits: LVGL must agree */
    }
}

/* The marked wrap of a prompt's label (memlcd_safe_wrap_marque): line 0
 * within the budget, every later line within budget - the mark's 8 px
 * (MEMLCD_CAVE_MARQUE_PAS), re-measured by LVGL; a line with digits and no
 * letter is allowed only after a mark (i > 0) — or as the first line when
 * no first line can reach a letter, which this gate reports, not fails. */
#define MARQUE_PAS MEMLCD_CAVE_MARQUE_PAS
static void run_marque(const char *label, uint8_t font, const lv_font_t *lf, const char *fname, uint16_t budget)
{
    char lines[MEMLCD_SW_MAX_LINES][MEMLCD_SW_LINE_BUF];
    int n = memlcd_safe_wrap_marque(label, font, budget, MARQUE_PAS, lines, MEMLCD_SW_MAX_LINES);
    printf("marked \"%s\" @ %s budget=%u/%upx -> %d line(s):\n", label, fname, budget, budget - MARQUE_PAS, n);
    if (n == 0) { printf("  (does not fit: the prompt takes UNSCII 8)\n"); return; }
    for (int i = 0; i < n; i++) {
        bool unsafe = memlcd_sw_line_is_unsafe(lines[i], strlen(lines[i]));
        lv_coord_t w = lv_txt_get_width(lines[i], (uint32_t)strlen(lines[i]), lf, 0, LV_TEXT_FLAG_NONE);
        bool over = w > (i ? budget - MARQUE_PAS : budget);
        printf("  [%d] %s\"%s\" (%dpx)%s%s\n", i, i ? "(mark) " : "", lines[i], (int)w,
               unsafe && i == 0 ? "  <<< UNSAFE FIRST LINE" : "", over ? "  <<< OVER" : "");
        if (over) g_fail++;
        if (unsafe && i == 0 && n > 1) g_fail++;
    }
}

int main(void)
{
    lv_init();
    /* 62 px: the label box (64) minus 2 px of air, memlcd_cave.h */
    run("TEST:RFC6238", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("BANQUE:4021", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("4021:BANQUE", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("AB:123456789:CD", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("TEST:RFC6238", MEMLCD_F_M14, &lv_font_montserrat_14, "14px", 62);
    run("GITHUB", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("OVH:PERSO", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("GITHUB.COM:ALICE.MARTIN@WORK-2FA01", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("github.com:alice.martin@work-2fa01", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("NIPHAR_CHEST", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run("AWS:123456789012", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("AWS:123456789012", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("AWS:123456789012345678901234567890", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("TEST:RFC6238", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("BANQUE:4021", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("GITHUB.COM:ALICE.MARTIN@WORK-2FA01", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    run_marque("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", MEMLCD_F_M12, &lv_font_montserrat_12, "12px", 62);
    printf("=================================================\n");
    printf(g_fail ? "TEST_SAFE_WRAP: %d FAILURE(S)\n" : "TEST_SAFE_WRAP: all OK\n", g_fail);
    return g_fail ? 1 : 0;
}
