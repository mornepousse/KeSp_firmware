#pragma once
#include "memlcd_model.h"

/* Safe line breaking for account names and chest labels — pure, tested on
 * host (test/test_memlcd_safe_wrap.c), measured with the width oracle of
 * memlcd_model.h (kerning included).
 *
 * Bench incident (Mae, 2026-09-29): "TEST:RFC6238" cut by pixel width into
 * "TEST:RFC" / "6238", and the second line read as a truncated 4-digit
 * CODE, not as part of a name. The rules:
 *
 *  1. SAFETY: no line may hold a digit without holding a letter — that
 *     reads as a code (":4021" is as bad as "4021"). Only a text that has
 *     no letter at all is exempt (it IS digits: nothing to protect).
 *  2. Every line within the budget.
 *  3. Break priority, most to least meaningful: ':' (issuer/account) >
 *     '.' (domain) > '@' (user/host) > '-' > space , ; _ > mid-word, the
 *     last one only when nothing else works; then as few lines as possible;
 *     ties go to the longer first lines.
 *
 * The breaks are CHOSEN, not patched: a small dynamic programme over the
 * break positions (a label is at most 34 bytes) finds the cheapest set of
 * lines satisfying 1 and 2 — "TEST:RFC6238" -> "TEST:" / "RFC6238";
 * "BANQUE:4021" -> "BANQU" / "E:4021" (the ':' break would strand the
 * digits: safe, not pretty); "AB:123456789:CD" -> "AB:123456" /
 * "789:CD" where a greedy wrap would have stranded a run of digits.
 * When NO split satisfies 1 and 2 (a run of digits and punctuation wider
 * than two lines' worth, "AWS:123456789012" at 12 px), memlcd_safe_wrap
 * returns the greedy wrap with its unsafe breaks REMOVED — lines rejoin,
 * over budget if they must — and the caller sees it (a line wider than its
 * budget): a browsed name is then cut with '~', its unsafe lines dropped.
 *
 * A prompt's label is never cut: memlcd_safe_wrap_marque wraps it for lines
 * that continue the previous one to start with a drawn continuation mark
 * (spec "Continuation mark", Mae 2026-09-30 — the mark takes `retrait` px of
 * every line after the first). A safe split is still preferred; when none
 * exists, a marked line may hold digits alone — it reads as the rest of a
 * name, never as a code. The first line has no mark: it holds a letter
 * whenever any first line could (only a label that STARTS with a digit run
 * wider than a line cannot).
 *
 * The text is untrusted (chest_proto sanitizes it to 0x20..0x7E): every scan
 * is bounded by MEMLCD_VC_TXT - 1 = CHEST_LABEL_MAX bytes. */

#define MEMLCD_SW_MAX_LINES 10
#define MEMLCD_SW_LINE_BUF  MEMLCD_VC_TXT

/* Cost of breaking AFTER character c (lower is better), and of a line. */
static inline int memlcd_sw_cout_coupure(char c)
{
    switch (c) {
    case ':': return 0;
    case '.': return 5;
    case '@': return 10;
    case '-': return 15;
    case ' ': case ',': case ';': case '_': return 20;
    default:  return 150;                         /* mid-word: worse than one more line */
    }
}
#define MEMLCD_SW_COUT_LIGNE 100

/* Unsafe = could be mistaken for a code: at least one digit and NO letter. */
static inline bool memlcd_sw_line_is_unsafe(const char *s, size_t n)
{
    bool digit = false, letter = false;
    for (size_t i = 0; i < n && s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') digit = true;
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) letter = true;
    }
    return digit && !letter;
}

/* The line text[a..b) as displayed: its leading spaces trimmed. */
static inline size_t memlcd_sw_debut(const char *text, size_t a, size_t b)
{
    while (a < b && text[a] == ' ') a++;
    return a;
}

/* What a line may hold: anything (the text has no letter at all), only
 * safe lines, or — marked wrap — unsafe CONTINUATION lines at a penalty
 * (the fewer the better) and an unsafe first line only at a penalty no
 * other choice can outweigh (it happens only when it cannot be avoided). */
enum { MEMLCD_SW_LIBRE, MEMLCD_SW_STRICT, MEMLCD_SW_MARQUE };
#define MEMLCD_SW_COUT_NUE      50       /* a marked line of digits alone: less than a mid-word break */
#define MEMLCD_SW_COUT_TETE_NUE 30000    /* an unmarked first line of digits alone */

/* Breaks of the cheapest wrap under `regle` into brk[0..n] (brk[0] = 0,
 * brk[n] = len); the first line within `budget`, every later one within
 * `budget - retrait`. Returns n, or 0 when no wrap satisfies the rule and
 * the widths, or the cheapest one needs more than max_lines. */
static inline int memlcd_sw_optimal(const char *text, size_t len, uint8_t font, uint16_t budget,
                                    uint16_t retrait, int regle, size_t brk[], int max_lines)
{
    enum { N = MEMLCD_VC_TXT };
    int32_t cout[N + 1];                              /* cost of wrapping text[i..len) */
    uint8_t suite[N + 1];                             /* where the line starting at i ends */
    uint8_t lignes[N + 1];
    const uint16_t budget_suite = retrait < budget ? (uint16_t)(budget - retrait) : 0;
    cout[len] = 0; lignes[len] = 0; suite[len] = (uint8_t)len;
    for (size_t i = len; i-- > 0;) {
        cout[i] = -1;
        const uint16_t bud = i == 0 ? budget : budget_suite;
        for (size_t j = len; j > i; j--) {            /* longest line first: ties keep it */
            if (cout[j] < 0) continue;
            size_t a = memlcd_sw_debut(text, i, j);
            if (memlcd_text_width_n(font, text + a, j - a) > bud) continue;
            int32_t pen = 0;
            if (regle != MEMLCD_SW_LIBRE && memlcd_sw_line_is_unsafe(text + a, j - a)) {
                if (regle == MEMLCD_SW_STRICT) continue;
                pen = i == 0 ? MEMLCD_SW_COUT_TETE_NUE : MEMLCD_SW_COUT_NUE;
            }
            int32_t c = MEMLCD_SW_COUT_LIGNE + pen + cout[j] + (j < len ? memlcd_sw_cout_coupure(text[j - 1]) : 0);
            if (cout[i] < 0 || c < cout[i]) { cout[i] = c; suite[i] = (uint8_t)j; lignes[i] = (uint8_t)(lignes[j] + 1); }
        }
    }
    if (cout[0] < 0 || lignes[0] > max_lines) return 0;
    int n = 0;
    brk[0] = 0;
    for (size_t i = 0; i < len; i = suite[i]) brk[++n] = suite[i];
    return n;
}

/* The fewest lines: each as long as its budget allows (the first within
 * `budget`, the others within `budget - retrait`, leading spaces not
 * counted). Returns n, or 0 when a glyph is wider than a line or the text
 * needs more than max_lines. Safety is not its business — the marked wrap
 * uses it last, when the cheapest wrap does not fit max_lines, and every
 * line after its first carries the mark; its first line is the longest
 * possible, so it holds a letter whenever any first line could. */
static inline int memlcd_sw_glouton(const char *text, size_t len, uint8_t font, uint16_t budget,
                                    uint16_t retrait, size_t brk[], int max_lines)
{
    const uint16_t budget_suite = retrait < budget ? (uint16_t)(budget - retrait) : 0;
    int n = 0;
    size_t pos = 0;
    brk[0] = 0;
    while (pos < len) {
        if (n >= max_lines) return 0;
        const uint16_t bud = n == 0 ? budget : budget_suite;
        size_t a = memlcd_sw_debut(text, pos, len), end = a;
        if (a >= len) { brk[n] = len; break; }      /* only spaces left: they end the last line */
        while (end < len && memlcd_text_width_n(font, text + a, end + 1 - a) <= bud) end++;
        if (end == a) return 0;                      /* one glyph wider than the line */
        brk[++n] = end;
        pos = end;
    }
    return n;
}

/* The fallback when no safe wrap exists: greedy lines, then every break
 * that leaves a line unsafe is removed (the lines rejoin). */
static inline int memlcd_sw_repli(const char *text, size_t len, uint8_t font, uint16_t budget,
                                  bool exempt, size_t brk[], int max_lines)
{
    int n = 0;
    size_t pos = 0;
    brk[0] = 0;
    while (pos < len && n < max_lines - 1) {
        size_t end = pos;
        while (end < len && memlcd_text_width_n(font, text + pos, end + 1 - pos) <= budget) end++;
        if (end == pos) end = pos + 1;
        if (end >= len) break;
        brk[++n] = end;
        pos = end;
    }
    brk[++n] = len;
    for (int i = 0; i < n && n > 1 && !exempt;) {
        size_t a = memlcd_sw_debut(text, brk[i], brk[i + 1]);
        if (!memlcd_sw_line_is_unsafe(text + a, brk[i + 1] - a)) { i++; continue; }
        int rm = (i > 0) ? i : 1;                     /* rejoin with a neighbour */
        for (int j = rm; j < n; j++) brk[j] = brk[j + 1];
        n--;
        if (i > 0) i--;
    }
    return n;
}

/* The lines text[brk[i]..brk[i+1]) into out, leading spaces trimmed. */
static inline void memlcd_sw_lignes(const char *text, const size_t brk[], int n, char out[][MEMLCD_SW_LINE_BUF])
{
    for (int i = 0; i < n; i++) {
        size_t a = memlcd_sw_debut(text, brk[i], brk[i + 1]);
        size_t l = brk[i + 1] - a;
        if (l >= MEMLCD_SW_LINE_BUF) l = MEMLCD_SW_LINE_BUF - 1;
        memcpy(out[i], text + a, l);
        out[i][l] = '\0';
    }
}

/* Wrap, materialising the lines (leading spaces trimmed). Returns the
 * number of lines (1..max_lines; an empty text is one empty line). */
static inline int memlcd_safe_wrap(const char *text, uint8_t font, uint16_t budget,
                                   char out[][MEMLCD_SW_LINE_BUF], int max_lines)
{
    size_t brk[MEMLCD_SW_MAX_LINES + 1];
    if (!text) text = "";
    size_t len = strnlen(text, MEMLCD_VC_TXT - 1);
    if (max_lines > MEMLCD_SW_MAX_LINES) max_lines = MEMLCD_SW_MAX_LINES;
    int n;
    if (len == 0) { brk[0] = brk[1] = 0; n = 1; }
    else {
        bool exempt = memlcd_sw_line_is_unsafe(text, len);   /* no letter at all */
        n = memlcd_sw_optimal(text, len, font, budget, 0, exempt ? MEMLCD_SW_LIBRE : MEMLCD_SW_STRICT,
                              brk, max_lines);
        if (n == 0) n = memlcd_sw_repli(text, len, font, budget, exempt, brk, max_lines);
    }
    memlcd_sw_lignes(text, brk, n, out);
    return n;
}

/* The marked wrap of a prompt's label: every line after the first starts
 * with the continuation mark, `retrait` px wide (mark + gap), so it has
 * `budget - retrait` px. In order: the cheapest SAFE wrap; if none, the
 * cheapest wrap whose continuation lines may hold digits alone (marked);
 * if the cheapest needs more than max_lines, the fewest lines. Returns the
 * number of lines (an empty text is one empty line), or 0 when the text
 * does not fit max_lines at all in this font — the caller then takes a
 * narrower one. The lines concatenated are the text (spaces at a break
 * trimmed, and the text's trailing spaces): never cut. */
static inline int memlcd_safe_wrap_marque(const char *text, uint8_t font, uint16_t budget, uint16_t retrait,
                                          char out[][MEMLCD_SW_LINE_BUF], int max_lines)
{
    size_t brk[MEMLCD_SW_MAX_LINES + 1];
    if (!text) text = "";
    size_t len = strnlen(text, MEMLCD_VC_TXT - 1);
    while (len > 0 && text[len - 1] == ' ') len--;   /* trailing spaces: no ink, no line of their own */
    if (max_lines > MEMLCD_SW_MAX_LINES) max_lines = MEMLCD_SW_MAX_LINES;
    int n;
    if (len == 0) { brk[0] = brk[1] = 0; n = 1; }
    else {
        bool exempt = memlcd_sw_line_is_unsafe(text, len);   /* no letter at all */
        n = memlcd_sw_optimal(text, len, font, budget, retrait, exempt ? MEMLCD_SW_LIBRE : MEMLCD_SW_STRICT,
                              brk, max_lines);
        if (n == 0 && !exempt)
            n = memlcd_sw_optimal(text, len, font, budget, retrait, MEMLCD_SW_MARQUE, brk, max_lines);
        if (n == 0) n = memlcd_sw_glouton(text, len, font, budget, retrait, brk, max_lines);
        if (n == 0) return 0;
    }
    memlcd_sw_lignes(text, brk, n, out);
    return n;
}
