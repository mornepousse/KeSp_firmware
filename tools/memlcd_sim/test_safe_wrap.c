#include "lvgl.h"
#include "safe_wrap.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);

static int g_fail = 0;

static void run(const char *label, const lv_font_t *font, const char *fname, lv_coord_t budget)
{
    char lines[SAFE_WRAP_MAX_LINES][SAFE_WRAP_LINE_BUF];
    int n = safe_wrap_lines(label, font, budget, lines, SAFE_WRAP_MAX_LINES);
    printf("\"%s\" @ %s budget=%dpx -> %d line(s):\n", label, fname, budget, n);
    bool any_unsafe = false, any_overflow = false;
    for (int i = 0; i < n; i++) {
        bool unsafe = safe_wrap_line_is_unsafe(lines[i], strlen(lines[i]));
        if (unsafe) any_unsafe = true;
        lv_coord_t w = lv_txt_get_width(lines[i], strlen(lines[i]), font, 0, LV_TEXT_FLAG_NONE);
        bool overflow = w > budget;
        if (overflow) any_overflow = true;
        printf("  [%d] \"%s\" (%dpx)%s%s\n", i, lines[i], w,
               unsafe ? "  <<< UNSAFE (digits/spaces only)" : "",
               overflow ? "  <<< OVERFLOW (> budget)" : "");
    }
    bool fail = any_unsafe;   /* overflow is reported but only fails if ALSO unsafe-caused (last-resort merge) */
    if (fail) g_fail++;
    printf("  %s%s\n\n", any_unsafe ? "FAIL: unsafe line present" : "OK: no digit-only line",
           any_overflow ? " (note: last-resort over-budget merge — expected only when no safe split exists)" : "");
}

int main(void)
{
    lv_init();
    /* The real floor (see check_glyph_ink.c) is Montserrat 12px — every
     * printable ASCII glyph keeps ink after the panel's 1-bit threshold,
     * and ':' is distinguishable from '.'. The real UI budget at this size
     * is 62px (label width MEMLCD_W-4=64, minus the 2px margin label_set
     * subtracts — see cave_ui.c). Test at both the floor and the next rung
     * (14px, budget shrinks accordingly isn't modeled here — same 62px
     * budget, tighter fit) since cave_ui.c's label ladder is now {14, 12}. */
    run("TEST:RFC6238", &lv_font_montserrat_12, "12px", 62);
    run("BANQUE:4021", &lv_font_montserrat_12, "12px", 62);
    run("TEST:RFC6238", &lv_font_montserrat_14, "14px", 62);
    run("BANQUE:4021", &lv_font_montserrat_14, "14px", 62);
    /* narrower stress tests (a label sharing its line with other content) */
    run("TEST:RFC6238", &lv_font_montserrat_12, "12px", 50);
    run("BANQUE:4021", &lv_font_montserrat_12, "12px", 50);
    run("BANQUE:4021", &lv_font_montserrat_12, "12px", 30);   /* even narrower: still must not isolate "4021" */
    /* known-good ones from the rest of the brief, must still behave sanely */
    run("GITHUB", &lv_font_montserrat_12, "12px", 62);
    run("OVH:PERSO", &lv_font_montserrat_12, "12px", 62);
    run("GITHUB.COM:ALICE.MARTIN@WORK-2FA01", &lv_font_montserrat_12, "12px", 62);
    run("NIPHAR_CHEST", &lv_font_montserrat_12, "12px", 62);

    printf("=================================================\n");
    printf(g_fail ? "TEST_SAFE_WRAP: %d FAILURE(S)\n" : "TEST_SAFE_WRAP: all OK\n", g_fail);
    return g_fail ? 1 : 0;
}
