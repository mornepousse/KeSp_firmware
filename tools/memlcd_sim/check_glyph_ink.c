/* Glyph-ink floor check (2026-09-29, punctuation-vanishes bug) — a GATE.
 *
 * At the bottom of the Montserrat ladder (8/10 px), the panel's 1-bit
 * threshold (lv_color_brightness < 128, exactly memlcd_backend.c's
 * flush_cb) wipes out thin punctuation: "TEST:RFC6238" rendered
 * "TEST RFC6238", "GITHUB.COM" rendered "GITHUB COM", "4.2V" "4 2V".
 *
 * This program renders every printable ASCII glyph (0x20..0x7E) of each
 * font through that same threshold and reports, per font:
 *   - any non-space glyph with ZERO ink pixels after threshold (FAIL)
 *   - whether ':' and '.' produce the SAME thresholded bitmap (FAIL —
 *     "distinguishable" is the requirement, not just "non-empty")
 *   - the ink-pixel count of the security-relevant marks : . , - @ _
 *
 * Two lists: the fonts the screen USES (the firmware's memlcd_cave_fonts,
 * memlcd_cave.c — UNSCII 8 among them, the prompt label's last resort, a
 * bitmap font with nothing to threshold away) — any failure there exits
 * 1 and fails build.sh — and reference fonts below the floor (k_ref),
 * printed so the floor stays explained by numbers (Montserrat 8 loses 7
 * glyphs and ':' == '.', Montserrat 10 loses the apostrophe; Montserrat 12
 * is the smallest size where every glyph keeps ink).
 */
#include "lvgl.h"
#include "memlcd_cave.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_8);
LV_FONT_DECLARE(lv_font_montserrat_10);


/* This tool needs the raw per-pixel ink buffer (to compare glyph bitmaps
 * for the ':' vs '.' distinguishability check), which common.c's
 * panel_*() API doesn't expose (it only writes PNGs). Rather than change
 * the shared harness used by every other render_* program, this file
 * registers its OWN tiny thresholded display driver — identical rule to
 * panel_flush_cb (lv_color_brightness < 128) — on a private lv_init().
 * Self-contained, no shared state with common.c/sim_init(). */
#define GW 68
#define GH 48   /* Montserrat 32 has a 35 px line: the box must hold it whole */
static lv_color_t s_drawbuf[GW * GH];
static lv_disp_draw_buf_t s_db;
static lv_disp_drv_t s_drv;
static lv_disp_t *s_disp;
static uint8_t s_ink[GW * GH];

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    for (lv_coord_t y = area->y1; y <= area->y2; y++)
        for (lv_coord_t x = area->x1; x <= area->x2; x++, px++)
            if (x >= 0 && x < GW && y >= 0 && y < GH)
                s_ink[y * GW + x] = lv_color_brightness(*px) < 128 ? 1 : 0;
    lv_disp_flush_ready(drv);
}

static int text_ink(const lv_font_t *f, const char *s, uint8_t bits[GH][GW]);
static int glyph_ink(const lv_font_t *f, char c, uint8_t bits[GH][GW])
{
    char s[2] = { c, 0 };
    return text_ink(f, s, bits);
}
static int text_ink(const lv_font_t *f, const char *s, uint8_t bits[GH][GW])
{
    memset(s_ink, 0, sizeof s_ink);
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(scr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_size(scr, GW, GH);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, 2, 2);
    lv_obj_set_width(l, GW - 4);
    lv_label_set_text(l, s);
    lv_refr_now(s_disp);
    int n = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            bits[y][x] = s_ink[y * GW + x];
            n += s_ink[y * GW + x];
        }
    return n;
}

static bool bits_equal(uint8_t a[GH][GW], uint8_t b[GH][GW])
{
    return memcmp(a, b, GH * GW) == 0;
}

typedef struct { const char *name; const lv_font_t *f; } font_e;

int main(void)
{
    lv_init();
    lv_disp_draw_buf_init(&s_db, s_drawbuf, NULL, GW * GH);
    lv_disp_drv_init(&s_drv);
    s_drv.draw_buf = &s_db;
    s_drv.flush_cb = flush_cb;
    s_drv.hor_res = GW;
    s_drv.ver_res = GH;
    s_drv.full_refresh = 1;
    s_disp = lv_disp_drv_register(&s_drv);

    /* The fonts the screen draws with: the firmware engine's own table
     * (memlcd_cave.c, memlcd_cave_fonts) — a font added there is gated here. */
    extern const lv_font_t *const memlcd_cave_fonts[MEMLCD_F_N];
    static const char *const noms[MEMLCD_F_N] = {
        "montserrat_12", "montserrat_14", "montserrat_16", "montserrat_18",
        "montserrat_20", "montserrat_24", "montserrat_28", "montserrat_32", "unscii_8",
    };
    font_e used[MEMLCD_F_N];
    for (int i = 0; i < MEMLCD_F_N; i++) { used[i].name = noms[i]; used[i].f = memlcd_cave_fonts[i]; }
    /* Below the floor: reported, never gated. */
    font_e ref[] = {
        { "montserrat_8",  &lv_font_montserrat_8  },
        { "montserrat_10", &lv_font_montserrat_10 },
    };
    int nused = (int)(sizeof used / sizeof used[0]);
    int nfonts = nused + (int)(sizeof ref / sizeof ref[0]);
    int gate_failures = 0;

    const char *punct_watch = ":.,-@_";

    printf("Glyph-ink floor check — threshold lv_color_brightness<128, box %dx%d\n", GW, GH);
    printf("=================================================================\n");

    for (int fi = 0; fi < nfonts; fi++) {
        const font_e *fe = fi < nused ? &used[fi] : &ref[fi - nused];
        const lv_font_t *f = fe->f;
        int zero_ink = 0;
        char zero_list[256]; zero_list[0] = '\0';
        static uint8_t store[95][GH][GW];   /* indexed by (ascii - 0x20) */
        int ink_count[95];
        for (int c = 0x20; c <= 0x7E; c++) {
            int idx = c - 0x20;
            ink_count[idx] = glyph_ink(f, (char)c, store[idx]);
            if (c != ' ' && ink_count[idx] == 0) {
                zero_ink++;
                char buf[4]; snprintf(buf, sizeof buf, "%c ", (char)c);
                strncat(zero_list, buf, sizeof zero_list - strlen(zero_list) - 1);
            }
        }
        bool colon_eq_dot = bits_equal(store[':' - 0x20], store['.' - 0x20]);

        printf("\n-- %s (%s) --\n", fe->name, fi < nused ? "USED by the screen" : "reference, below the floor");
        printf("  zero-ink non-space glyphs: %d%s%s\n", zero_ink,
               zero_ink ? "  -> " : "", zero_ink ? zero_list : "");
        printf("  ':' vs '.' same bitmap: %s\n", colon_eq_dot ? "YES <<< FAIL (indistinguishable)" : "no (ok)");
        printf("  punctuation ink pixel counts:");
        for (const char *p = punct_watch; *p; p++) {
            printf("  '%c'=%d", *p, ink_count[*p - 0x20]);
        }
        printf("\n");
        bool pass = (zero_ink == 0) && !colon_eq_dot;
        printf("  VERDICT: %s\n", pass ? "PASS" : "FAIL (unfit for the panel)");
        if (fi < nused && !pass) gate_failures++;
    }
    /* The reading INSIDE the drop (memlcd_cave.h, memlcd_goutte_texte_ok)
     * assumes Montserrat 12's digits, '+' and '?' ink only rows
     * [MEMLCD_CAVE_M12_ENCRE_HAUT, MEMLCD_CAVE_M12_ENCRE_BAS) of their line
     * and only columns inside the measured width: checked here, rendered,
     * for every reading the drop can hold. */
    {
        static uint8_t bits[GH][GW];
        int hors = 0;
        char t[8];
        for (int p = -2; p <= MEMLCD_CAVE_PCT_BAS; p++) {
            if (p == -2) snprintf(t, sizeof t, "?");
            else if (p == -1) snprintf(t, sizeof t, "+");
            else snprintf(t, sizeof t, "%d", p);
            text_ink(memlcd_cave_fonts[MEMLCD_F_M12], t, bits);
            int w = memlcd_text_width(MEMLCD_F_M12, t);
            for (int y = 0; y < GH; y++)
                for (int x = 0; x < GW; x++)
                    if (bits[y][x] && (y < 2 + MEMLCD_CAVE_M12_ENCRE_HAUT || y >= 2 + MEMLCD_CAVE_M12_ENCRE_BAS
                                       || x < 2 || x >= 2 + w)) {
                        if (hors < 5) printf("  drop reading \"%s\": ink at (%d, %d) outside rows [3, 12) x [0, %d)\n", t, x - 2, y - 2, w);
                        hors++;
                    }
        }
        printf("\n-- drop readings (Montserrat 12: 0..%d, '+', '?') --\n  ink outside the assumed box: %d -- %s\n",
               MEMLCD_CAVE_PCT_BAS, hors, hors ? "FAIL" : "OK");
        if (hors) gate_failures++;
    }
    printf("\n%s\n", gate_failures ? "GLYPH INK: a font the screen uses loses ink -- FAIL" : "GLYPH INK: every font the screen uses keeps its ink -- OK");
    return gate_failures ? 1 : 0;
}
