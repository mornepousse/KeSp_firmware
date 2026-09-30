#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Safe line-breaking for account names/labels — a bench incident (Mae,
 * 2026-09-29) found "TEST:RFC6238" cut by pixel width into "TEST:RFC" /
 * "6238", and the second line read as a truncated 4-digit CODE, not part
 * of a name. Two rules, enforced together:
 *
 *  1. Break priority, most to least meaningful: ':' (issuer/account) >
 *     '.' (domain) > '@' (user/host) > '-' (word joiner) > generic
 *     space/punctuation > mid-word — a normal greedy wrap, but always
 *     preferring the MOST meaningful separator available in the window
 *     over a bare pixel-width cut.
 *  2. SAFETY OVERRIDE: no resulting line may be non-empty and consist only
 *     of digits and spaces/punctuation — that reads as a code. If rule 1's
 *     break would produce one, the fix is, in order: (a) shift the break
 *     backward, mid-word if needed, pulling letters from the END of the
 *     PREVIOUS line into the front of the unsafe line, until it contains a
 *     letter and both lines still fit the budget — "BANQUE:4021" cannot
 *     fit the panel on one line at the text-size floor (12px), so its
 *     ':'-break ("BANQUE:" / "4021", unsafe) is walked back to a mid-word
 *     split inside the letters ("BANQU" / "E:4021", safe); (b) only if no
 *     such shift exists (the previous line has no letters to lend, or
 *     every shift still overflows), fall back to REMOVING the break
 *     entirely (the two lines rejoin into one, over-budget if it must) —
 *     never re-split elsewhere, since rejoining can only ever add non-digit
 *     content to the run.
 *
 * 2026-09-29 revision note: an earlier version of this file always did (b)
 * — collapse to one line — for EVERY unsafe break, on the theory that a
 * mid-word cut was itself confusing. That relied on dropping all the way to
 * an 8px fallback font to make the collapsed line narrow enough to fit,
 * and 8px is exactly the size where LVGL's antialiased strokes lose ':' and
 * '.' entirely under the panel's 1-bit threshold (measured in
 * check_glyph_ink.c) — trading one truncation-reads-as-a-code risk for a
 * punctuation-silently-vanishes risk. With a 12px text-size floor (see
 * cave_ui.c's k_label_ladder), (b) alone would routinely overflow the
 * label's width box and get clipped by LVGL — the exact hazard this file
 * exists to prevent. (a) is the fix: same "no digit-only line" guarantee,
 * without needing a font this file has no floor-aware way to bound.
 *
 * This is pure text-shaping (no font legibility trade-off beyond the
 * caller keeping the font at/above the floor): a caller that still
 * overflows the panel after this should drop to the next size UP the
 * ladder from the floor and re-run it, never below the floor. */

#define SAFE_WRAP_MAX_LINES 10
#define SAFE_WRAP_LINE_BUF  40

static inline bool safe_wrap_is_break_char(char c)
{
    return c == ' ' || c == ',' || c == '.' || c == ';' || c == '-' || c == '_' || c == '@';
}

/* Break priority tiers, most to least meaningful (see file header). Tier 4
 * is every remaining generic separator (space, comma, semicolon,
 * underscore) — '.', '@' and '-' each get their OWN tier above it because
 * a domain/user/word-joiner split is more legible than an arbitrary one,
 * even though all of them would also match `safe_wrap_is_break_char`. */
static inline bool safe_wrap_is_break_tier(char c, int tier)
{
    switch (tier) {
        case 0: return c == ':';
        case 1: return c == '.';
        case 2: return c == '@';
        case 3: return c == '-';
        case 4: return c == ' ' || c == ',' || c == ';' || c == '_';
        default: return false;
    }
}
#define SAFE_WRAP_TIER_COUNT 5

/* Unsafe = "could be mistaken for a code": at least one digit, and NO
 * letter anywhere on the line. A bare ":4021" is exactly as dangerous as
 * "4021" (found on the first pass at the real 66px budget: "BANQUE:4021"
 * split into "BANQUE" / ":4021" — the leading colon does not make it read
 * as a name, so it is not enough to only reject PURE digit/space lines). */
static inline bool safe_wrap_line_is_unsafe(const char *s, size_t n)
{
    bool any_digit = false, any_letter = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') any_digit = true;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) any_letter = true;
    }
    return any_digit && !any_letter;
}

/* Greedy pixel wrap, priority ':' > '.' > '@' > '-' > generic break-char >
 * hard cut (see safe_wrap_is_break_tier), into byte offsets `brk[0..n]`
 * (brk[0]=0, brk[n]=strlen(text)); n+1 offsets bound n lines, line i =
 * text[brk[i]..brk[i+1]). Returns n (line count). */
static inline int safe_wrap_positions(const char *text, const lv_font_t *font, lv_coord_t budget,
                                      size_t brk[], int max_lines)
{
    size_t len = strlen(text);
    int n = 0;
    size_t pos = 0;
    brk[0] = 0;
    if (len == 0) { brk[1] = 0; return 1; }
    while (pos < len && n < max_lines - 1) {
        size_t end = pos;
        while (end < len) {
            size_t trial = end + 1;
            if (lv_txt_get_width(text + pos, (uint32_t)(trial - pos), font, 0, LV_TEXT_FLAG_NONE) > budget) break;
            end = trial;
        }
        if (end == pos) end = pos + 1;              /* always progress */
        if (end >= len) break;                       /* the rest fits on this last line */
        size_t cut = 0;
        for (int tier = 0; tier < SAFE_WRAP_TIER_COUNT && !cut; tier++)
            for (size_t i = end; i > pos; i--)
                if (safe_wrap_is_break_tier(text[i - 1], tier)) { cut = i; break; }
        if (!cut) cut = end;                          /* last resort: mid-word */
        n++;
        brk[n] = cut;
        pos = cut;
    }
    n++;
    brk[n] = len;
    return n;
}

/* Phase 2 — fix any line that is digits/spaces-only (see file header for
 * the two-step strategy). `font`/`budget` are needed only for step (a),
 * the mid-word shift: it must know when the growing unsafe line would
 * stop fitting. Operates on the SAME offsets array in place; returns the
 * new line count. */
static inline int safe_wrap_fix(const char *text, const lv_font_t *font, lv_coord_t budget, size_t brk[], int n)
{
    bool changed = true;
    while (changed && n > 1) {
        changed = false;
        for (int i = 0; i < n; i++) {
            if (!safe_wrap_line_is_unsafe(text + brk[i], brk[i + 1] - brk[i])) continue;

            /* (a) shift the shared boundary with the PREVIOUS line
             * backward, one char at a time, pulling letters into this
             * line's front, as long as: there IS a previous line to pull
             * from, doing so doesn't empty it, and the growing line still
             * fits `budget`. Stops as soon as the line is safe. */
            if (i > 0) {
                size_t lo = brk[i - 1];
                size_t try_start = brk[i];
                while (try_start > lo) {
                    size_t cand = try_start - 1;
                    lv_coord_t w = lv_txt_get_width(text + cand, (uint32_t)(brk[i + 1] - cand), font, 0, LV_TEXT_FLAG_NONE);
                    if (w > budget) break;             /* would overflow the line — stop shifting */
                    try_start = cand;
                    if (!safe_wrap_line_is_unsafe(text + try_start, brk[i + 1] - try_start)) break;
                }
                if (try_start != brk[i] && !safe_wrap_line_is_unsafe(text + try_start, brk[i + 1] - try_start)) {
                    brk[i] = try_start;
                    changed = true;
                    break;
                }
            }

            /* (b) fallback: remove the break that starts this line (merge
             * into the previous line), or if this IS the first line,
             * remove the break that ENDS it (merge into the next) — one
             * fewer break, one fewer line; the merge can only ever add
             * non-digit content to the unsafe run (or, in the degenerate
             * all-digit-label case, terminate at n==1). */
            int remove_idx = (i > 0) ? i : 1;
            for (int j = remove_idx; j < n; j++) brk[j] = brk[j + 1];
            n--;
            changed = true;
            break;
        }
    }
    return n;
}

/* Convenience for a caller that keeps ONE lv_label in LV_LABEL_LONG_WRAP
 * mode (like dir_surface.c) rather than a pool of per-line widgets: returns
 * a single string with '\n' inserted at each safe break — each segment
 * between newlines already fits `budget`, so LVGL's own wrap has nothing
 * left to do and won't re-break inside a safety-checked segment. */
static inline void safe_wrap_join(const char *text, const lv_font_t *font, lv_coord_t budget,
                                  char *out, size_t out_size)
{
    size_t brk[SAFE_WRAP_MAX_LINES + 1];
    int n = safe_wrap_positions(text, font, budget, brk, SAFE_WRAP_MAX_LINES);
    n = safe_wrap_fix(text, font, budget, brk, n);
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < out_size; i++) {
        size_t a = brk[i], b = brk[i + 1];
        while (a < b && text[a] == ' ') a++;
        size_t len = b - a;
        if (o + len + 1 >= out_size) len = out_size - o - 1;
        memcpy(out + o, text + a, len);
        o += len;
        if (i + 1 < n && o + 1 < out_size) out[o++] = '\n';
    }
    out[o] = '\0';
}

/* Convenience: wrap + fix in one call, materializing line strings (each
 * left/right-untrimmed except a single leading space after a space-break,
 * which IS trimmed for display — trimming never removes a character that
 * safety accounting above looked at, since it only ever trims a space). */
static inline int safe_wrap_lines(const char *text, const lv_font_t *font, lv_coord_t budget,
                                  char out[][SAFE_WRAP_LINE_BUF], int max_lines)
{
    size_t brk[SAFE_WRAP_MAX_LINES + 1];
    int n = safe_wrap_positions(text, font, budget, brk, max_lines);
    n = safe_wrap_fix(text, font, budget, brk, n);
    if (n > max_lines) n = max_lines;   /* should not happen: fix only reduces n */
    for (int i = 0; i < n; i++) {
        size_t a = brk[i], b = brk[i + 1];
        while (a < b && text[a] == ' ') a++;         /* trim a leading space left by a space-break */
        size_t len = b - a;
        if (len >= SAFE_WRAP_LINE_BUF) len = SAFE_WRAP_LINE_BUF - 1;
        memcpy(out[i], text + a, len);
        out[i][len] = '\0';
    }
    return n;
}
