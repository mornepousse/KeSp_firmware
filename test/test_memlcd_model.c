/* Sharp memory-LCD screen of the halves — pure logic.
 *
 * Three things a bug would make visible on screen without ever crashing:
 *  - rev8: the panel reads LSB-first, the ESP32 transmits MSB-first. A wrong
 *    inversion = ignored commands, silent screen, no error anywhere.
 *  - the layer name cut at 68 px: 4 characters per line, 3 lines,
 *    then "…" — the user preferred readable lines over rotated text.
 *  - the model diff: we only redraw if a displayed field has changed —
 *    every redraw is a transaction on the bus shared with the radio.
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include "../main/comm/chest/chest_view.h"
#include "chest_test_vectors.h"
#include <string.h>

#define V1  CHEST_TV_V1
#define V9  CHEST_TV_V9
#define V15 CHEST_TV_V15
#define V16 CHEST_TV_V16
#define L1  CHEST_TV_L1
#define C1  CHEST_TV_C1

static void test_rev8(void)
{
    TEST_ASSERT_EQ(memlcd_rev8(0x01), 0x80, "bit0 → bit7");
    TEST_ASSERT_EQ(memlcd_rev8(0x80), 0x01, "bit7 → bit0");
    TEST_ASSERT_EQ(memlcd_rev8(0xA5), 0xA5, "0xA5 is a binary palindrome");
    TEST_ASSERT_EQ(memlcd_rev8(0x0F), 0xF0, "nibbles swapped bit by bit");
    TEST_ASSERT_EQ(memlcd_rev8(0x86), 0x61, "0x86 (1000 0110) → 0x61 (0110 0001)");
    for (unsigned b = 0; b < 256; b++)
        TEST_ASSERT_EQ(memlcd_rev8(memlcd_rev8((uint8_t)b)), (uint8_t)b, "involution");
}

static void test_couper_nom(void)
{
    /* Full width under the icon column since 2026-09-26: 6 characters per
     * line (Montserrat 14 on 68 px — "LAYER 2" is ~56 px), 2 lines, cut at
     * the last space that fits so a word is not split when it can be avoided. */
    char l[MEMLCD_NOM_LIGNES][MEMLCD_NOM_BUF];
    TEST_ASSERT_EQ(MEMLCD_NOM_LIGNES, 2, "two name lines");
    TEST_ASSERT_EQ(MEMLCD_NOM_COLS, 6, "six characters per line");
    TEST_ASSERT_EQ(memlcd_couper_nom("DVORAK", l), 1, "6 letters → 1 line");
    TEST_ASSERT(strcmp(l[0], "DVORAK") == 0 && l[1][0] == '\0', "DVORAK, 2nd line empty");
    TEST_ASSERT_EQ(memlcd_couper_nom("NAV", l), 1, "3 letters → 1 line");
    TEST_ASSERT(strcmp(l[0], "NAV") == 0, "NAV");
    TEST_ASSERT_EQ(memlcd_couper_nom("", l), 1, "empty → 1 empty line (never 0)");
    TEST_ASSERT(l[0][0] == '\0', "empty line");
    TEST_ASSERT_EQ(memlcd_couper_nom(NULL, l), 1, "NULL → like empty, no crash");
    TEST_ASSERT_EQ(memlcd_couper_nom("LAYER 2", l), 2, "7 characters → 2 lines");
    TEST_ASSERT(strcmp(l[0], "LAYER") == 0 && strcmp(l[1], "2") == 0, "cut at the space: LAYER / 2");
    TEST_ASSERT_EQ(memlcd_couper_nom("GAMING", l), 1, "exactly 6 → 1 line");
    TEST_ASSERT_EQ(memlcd_couper_nom("SYMBOLS", l), 2, "7 letters, no space → hard cut");
    TEST_ASSERT(strcmp(l[0], "SYMBOL") == 0 && strcmp(l[1], "S") == 0, "SYMBOL / S");
    TEST_ASSERT_EQ(memlcd_couper_nom("NUM PAD", l), 2, "space at 3");
    TEST_ASSERT(strcmp(l[0], "NUM") == 0 && strcmp(l[1], "PAD") == 0, "NUM / PAD");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKL", l), 2, "12 letters → 2 full lines");
    TEST_ASSERT(strcmp(l[1], "GHIJKL") == 0, "2nd line full, no …");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKLMN", l), 2, "14 letters → truncated");
    TEST_ASSERT(strcmp(l[1], "GHIJK\xE2\x80\xA6") == 0, "2nd line = 5 letters + … (UTF-8)");
    TEST_ASSERT_EQ(memlcd_couper_nom("MEDIA KEYS 2", l), 2, "space cut, the rest fits exactly");
    TEST_ASSERT(strcmp(l[0], "MEDIA") == 0 && strcmp(l[1], "KEYS 2") == 0, "MEDIA / KEYS 2");
    TEST_ASSERT_EQ(memlcd_couper_nom("MEDIA KEYS ABC", l), 2, "space cut, then overflow");
    TEST_ASSERT(strcmp(l[1], "KEYS\xE2\x80\xA6") == 0, "the ellipsis never follows a space: KEYS…");
}

/* Displayed layer: the STABLE one (base, TO, Layer Lock) = the engine's
 * last_layer, never a held MO/LT/LM. In RF the left does not hear the right
 * (the dongle runs the engine), so a MO on the right was invisible and a mix
 * of thumbs left the screen on the wrong layer; a status screen at 1 s cannot
 * follow a momentary layer anyway. Mae, 2026-09-26: "if we can't be reactive,
 * we don't handle MOs". */
static void test_couche_affichee(void)
{
    TEST_ASSERT_EQ(memlcd_couche_affichee(0, 0), 0, "base layer");
    TEST_ASSERT_EQ(memlcd_couche_affichee(1, 0), 0, "MO(1) held from base → still base");
    TEST_ASSERT_EQ(memlcd_couche_affichee(3, 3), 3, "TO(3) / layer lock → 3");
    TEST_ASSERT_EQ(memlcd_couche_affichee(1, 3), 3, "MO(1) held over a locked 3 → 3");
}

/* Status lines, UNSCII 8 = 8 characters on 68 px. Line 1: what capitalises
 * (host Caps Lock, Caps Word). Line 2: armed one-shots — modifiers as letters
 * C S A G (left and right merged), then the one-shot layer. Empty when
 * nothing is armed: the screen says nothing when there is nothing to say. */
static void test_ligne_etat(void)
{
    char l1[MEMLCD_ETAT_BUF], l2[MEMLCD_ETAT_BUF];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(l1[0] == '\0' && l2[0] == '\0', "nothing armed → both lines empty");

    m.caps_lock = 1;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CAPS") == 0, "Caps Lock");
    m.caps_word = 1;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CAPS CW") == 0, "Caps Lock + Caps Word");
    m.caps_lock = 0;
    memlcd_ligne_etat(&m, l1, l2);
    TEST_ASSERT(strcmp(l1, "CW") == 0, "Caps Word alone");

    memlcd_model_t o = { .osl = MEMLCD_OSL_AUCUNE, .osm = 0x02 };   /* LShift */
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "S") == 0, "one-shot Shift");
    o.osm = 0x20 | 0x01;                                             /* RShift + LCtrl */
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CS") == 0, "right Shift merged, fixed order C S A G");
    o.osm = 0xFF;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CSAG") == 0, "all four");
    o.osl = 3;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "CSAG L3") == 0, "mods + one-shot layer, 7 characters");
    TEST_ASSERT(strlen(l2) <= 8, "fits the 8 UNSCII columns");
    o.osm = 0;
    memlcd_ligne_etat(&o, l1, l2);
    TEST_ASSERT(strcmp(l2, "L3") == 0, "one-shot layer alone, no leading space");
}

/* Chest status, 3 lines of 4 UNSCII characters under the logo: "P4" ready
 * ("P4.." booting, "P4?" unknown protocol version), "SD", then the USB
 * mode — ACTIVE in upper case once ARRIVED, WANTED in lower case while a
 * switch is PENDING, "ERR" on a persisting refusal (v2 rule, folded into
 * v3 unchanged, 2026-09-29: docs/superpowers/plans/2026-09-29-chest-link-v2.md
 * Task 3). */
static void test_lignes_coffre(void)
{
    char l[3][MEMLCD_COFFRE_BUF];
    memlcd_model_t m = { .osl = MEMLCD_OSL_AUCUNE };
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(!l[0][0] && !l[1][0] && !l[2][0], "absent: nothing");
    m.coffre = MEMLCD_COFFRE_PRESENT;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4..") == 0 && !l[1][0], "present, booting");

    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY | CHEST_STATE_SD | CHEST_STATE_USB;
    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_PGP; m.coffre_mode_wanted = CHEST_MODE_PGP;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4") == 0 && strcmp(l[1], "SD") == 0 && strcmp(l[2], "PGP") == 0, "P4 / SD / PGP");

    m.coffre = MEMLCD_COFFRE_PRESENT | CHEST_STATE_READY;
    m.coffre_mode_state = CHEST_MODE_PENDING; m.coffre_mode_active = CHEST_MODE_IN_FLIGHT; m.coffre_mode_wanted = CHEST_MODE_OATH;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[1], "oath") == 0 && l[2][0] == '\0', "no SD: pending mode moves up, lower case");

    m.coffre_mode_state = CHEST_MODE_FAULT;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[1], "ERR") == 0, "persisting refusal");

    m.coffre_mode_state = CHEST_MODE_ARRIVED; m.coffre_mode_active = CHEST_MODE_NONE; m.coffre_mode_wanted = CHEST_MODE_NONE;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(l[1][0] == '\0', "no mode: nothing");

    m.coffre = MEMLCD_COFFRE_PRESENT | MEMLCD_COFFRE_BADVER;
    memlcd_lignes_coffre(&m, l);
    TEST_ASSERT(strcmp(l[0], "P4?") == 0 && !l[1][0], "unknown protocol version");
}

/* memlcd_couper_8: hard cut at 8 characters, no space-awareness (untrusted
 * text, not prose) — '~' replaces the last line's last character when text
 * remains beyond the budget it was given. */
static void test_couper_8(void)
{
    char l[4][MEMLCD_ETAT_BUF];
    TEST_ASSERT_EQ(memlcd_couper_8("", 4, l), 1, "empty -> 1 empty line, never 0");
    TEST_ASSERT(l[0][0] == '\0', "empty line");
    TEST_ASSERT_EQ(memlcd_couper_8(NULL, 4, l), 1, "NULL -> like empty, no crash");
    TEST_ASSERT_EQ(memlcd_couper_8("GITHUB", 4, l), 1, "6 letters, fits in 1 line");
    TEST_ASSERT(strcmp(l[0], "GITHUB") == 0, "GITHUB, no cut, no marker");
    TEST_ASSERT_EQ(memlcd_couper_8("ABCDEFGH", 4, l), 1, "exactly 8 -> 1 full line, no marker");
    TEST_ASSERT(strcmp(l[0], "ABCDEFGH") == 0, "exactly 8, not truncated");
    TEST_ASSERT_EQ(memlcd_couper_8("ABCDEFGHI", 4, l), 2, "9 letters -> 2 lines");
    TEST_ASSERT(strcmp(l[0], "ABCDEFGH") == 0 && strcmp(l[1], "I") == 0, "8 + 1, no marker: the 2nd line fits it all");

    /* CHEST_LABEL_MAX (34 characters) into the 4 lines the prompt/browser
     * templates actually budget for the label/name body: 3 full lines (24
     * chars) then the last available line can only show 7 of the remaining
     * 10 characters, so it ends in '~' and 3 characters are dropped —
     * never silently: this is the bite proof for "the ~ cut marker
     * dropped" (plan Task 5). */
    const char *label34 = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH";   /* 34 chars, CHEST_LABEL_MAX */
    TEST_ASSERT_EQ(strlen(label34), 34u, "test string is exactly CHEST_LABEL_MAX");
    TEST_ASSERT_EQ(memlcd_couper_8(label34, 4, l), 4, "34 chars into 4 slots: all 4 used");
    TEST_ASSERT(strcmp(l[0], "ABCDEFGH") == 0, "line 0 full");
    TEST_ASSERT(strcmp(l[1], "IJKLMNOP") == 0, "line 1 full");
    TEST_ASSERT(strcmp(l[2], "QRSTUVWX") == 0, "line 2 full");
    TEST_ASSERT(strcmp(l[3], "YZABCDE~") == 0, "line 3: 7 real characters + '~' — the cut is never silent");
}

/* The bottom area's six lines: prompt > code > browser > nothing, priority
 * order and exact formatting (plan Task 5 / spec §5). */
static void test_bas_coffre(void)
{
    char b[MEMLCD_BAS_LIGNES][MEMLCD_ETAT_BUF];
    memlcd_model_t m = {0};
    TEST_ASSERT(!memlcd_bas_coffre(&m, b), "nothing pending, no code, not browsing: the caller falls back");

    /* Prompt: short label, one line, no N CPT (op_count <= 1). */
    m.coffre_op = 1; strcpy(m.coffre_label, "GITHUB"); m.coffre_op_count = 1;
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "prompt case applies");
    TEST_ASSERT(strcmp(b[0], "SIGN") == 0, "line 0: the op label (chest_op_label(1) == SIGN)");
    TEST_ASSERT(strcmp(b[1], "GITHUB") == 0, "line 1: the label, one line");
    TEST_ASSERT(b[2][0] == '\0' && b[3][0] == '\0' && b[4][0] == '\0', "unused label lines stay empty");
    TEST_ASSERT(strcmp(b[5], "OK ?") == 0, "line 5: OK ?");

    /* N CPT never shown when op_count <= 1 — bite proof. */
    m.coffre_op_count = 0;
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strstr(b[1], "CPT") == NULL && strstr(b[2], "CPT") == NULL, "op_count 0: no CPT anywhere");
    m.coffre_op_count = 1;
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strstr(b[1], "CPT") == NULL && strstr(b[2], "CPT") == NULL, "op_count 1: N CPT never shown (only N > 1)");

    /* N CPT appended after a short label that leaves room. */
    m.coffre_op_count = 12;
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strcmp(b[1], "GITHUB") == 0, "label line untouched");
    TEST_ASSERT(strcmp(b[2], "12 CPT") == 0, "N CPT appended right after the label");
    TEST_ASSERT(strcmp(b[5], "OK ?") == 0, "OK ? unaffected");

    /* N CPT replaces the last label line when all 4 are already used. */
    strcpy(m.coffre_label, "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH");   /* 34 chars -> fills all 4 label lines */
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strcmp(b[4], "12 CPT") == 0, "all 4 label lines used: N CPT replaces the last one");

    /* Code visible: 6 digits on one line. */
    memset(&m, 0, sizeof m);
    m.coffre_code_visible = 1;
    strcpy(m.coffre_nom, "WORK"); strcpy(m.coffre_code, "418902"); m.coffre_code_secs = 12;
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "code case applies");
    TEST_ASSERT(strcmp(b[0], "WORK") == 0, "line 0: the account name");
    TEST_ASSERT(b[1][0] == '\0', "line 1: empty");
    TEST_ASSERT(strcmp(b[2], "418902") == 0, "6 digits on one line");
    TEST_ASSERT(b[3][0] == '\0', "line 3 stays empty for a 6-digit code");
    TEST_ASSERT(strcmp(b[5], "  12 s") == 0, "countdown format");

    /* Code visible: 8 digits split 4+4. */
    strcpy(m.coffre_code, "12345678"); m.coffre_code_secs = 5;
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strcmp(b[2], "1234") == 0 && strcmp(b[3], "5678") == 0, "8 digits split 4+4");
    TEST_ASSERT(strcmp(b[5], "   5 s") == 0, "single-digit countdown right-justified");

    /* Code shown without on_code: not this function's job (it only reads
     * coffre_code_visible) — the gate is chest_oath's / chest_view's; here
     * we only prove the flag alone controls the case, not any of the
     * other coffre_ fields. */
    memset(&m, 0, sizeof m);
    strcpy(m.coffre_code, "123456"); m.coffre_code_secs = 12;   /* code text present */
    m.coffre_code_visible = 0;                                  /* but NOT visible */
    TEST_ASSERT(!memlcd_bas_coffre(&m, b), "code_visible false: never the code case, whatever coffre_code holds");

    /* Browsing: position, name, NO TIME. */
    memset(&m, 0, sizeof m);
    m.coffre_browsing = 1; m.coffre_pos = 2; m.coffre_total = 12; strcpy(m.coffre_nom, "OVH:PRO");
    m.coffre = CHEST_STATE_TIME;   /* time posed: no "NO TIME" */
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "browsing case applies");
    TEST_ASSERT(strcmp(b[0], "3/12") == 0, "1-based position / total");
    TEST_ASSERT(strcmp(b[1], "OVH:PRO") == 0, "the name, one line");
    TEST_ASSERT(b[5][0] == '\0', "time posed: line 5 empty, no NO TIME");

    m.coffre = 0;   /* TIME bit clear */
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strcmp(b[5], "NO TIME") == 0, "TIME bit clear: NO TIME shown");
}

/* chest_view_t -> memlcd_model_t, the SAME mapping memlcd_backend.c's
 * lire_modele() does under CONFIG_KASE_CHEST_LINK — duplicated here (a
 * host test cannot link the ESP-IDF backend) so this test chain proves the
 * mapping too, not just chest_view_build() and memlcd_bas_coffre() each on
 * their own. */
static void model_from_view(memlcd_model_t *m, const chest_view_t *v)
{
    memset(m, 0, sizeof *m);
    m->osl = MEMLCD_OSL_AUCUNE;
    m->coffre = v->bits; m->coffre_op = v->op; m->coffre_op_count = v->op_count;
    strncpy(m->coffre_label, v->label, sizeof m->coffre_label - 1);
    m->coffre_mode_active = v->mode_active; m->coffre_mode_wanted = v->mode_wanted; m->coffre_mode_state = v->mode_state;
    m->coffre_browsing = v->browsing; m->coffre_pos = v->pos; m->coffre_total = v->total;
    strncpy(m->coffre_nom, v->name, sizeof m->coffre_nom - 1);
    m->coffre_code_visible = v->code_visible;
    strncpy(m->coffre_code, v->code, sizeof m->coffre_code - 1);
    m->coffre_code_secs = v->code_secs;
}

/* "Bytes -> pixels, pinned end to end" (plan Task 5): the chest's own raw
 * register/DMA bytes, through chest_proto_parse -> chest_view_build -> the
 * memlcd model -> memlcd_bas_coffre, asserting the RENDERED lines — a
 * regression anywhere in that chain shows up as a wrong string on screen,
 * not just a wrong struct field (this note was added to the plan after the
 * chest found a RESET path where op_count 1 reached the screen while the
 * contract and V16 itself said 12: the vectors alone had proved the parser,
 * never that the screen showed it). */
static void test_bas_coffre_end_to_end(void)
{
    chest_status_t st;
    chest_view_t v;
    memlcd_model_t m;
    char b[MEMLCD_BAS_LIGNES][MEMLCD_ETAT_BUF];

    /* V16: a RESET pending — the label says "12 COMPTES" and 0x0F says 12
     * in one byte; both must reach the screen. */
    TEST_ASSERT_EQ(chest_proto_parse(V16, 64, &st), CHEST_BLOCK_OK, "V16 parses");
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, NULL, 0);
    model_from_view(&m, &v);
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "V16: prompt shown");
    TEST_ASSERT(strcmp(b[1], "12 COMPT") == 0 && strcmp(b[2], "ES") == 0, "V16: the label, 12 COMPTES, across the label lines");
    TEST_ASSERT(strcmp(b[3], "12 CPT") == 0, "V16: 12 CPT, from op_count == 12, not clamped to 1");

    /* V1: op pending, label GITHUB — an OATH cursor on a DIFFERENT, named
     * account must not leak into the prompt. Only V1 (and V16 above)
     * exercise this: a naive "always show the cursor" bug survives V9
     * below untouched (no op, no label, nothing to compare). */
    chest_list_t l;
    TEST_ASSERT(chest_list_decode(L1, sizeof L1, &l), "L1 decodes");
    chest_oath_t o;
    chest_oath_reset(&o);
    chest_oath_on_list(&o, &l);
    chest_oath_nav(&o, +2);   /* cursor -> "OVH:PRO", index 2 */
    TEST_ASSERT_EQ(chest_proto_parse(V1, 64, &st), CHEST_BLOCK_OK, "V1 parses");
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    model_from_view(&m, &v);
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "V1: prompt shown");
    TEST_ASSERT(strcmp(b[1], "GITHUB") == 0, "V1: the CHEST's label, not the cursor's OVH:PRO");

    /* V9 alone: present, not ready, nothing pending, active mode none —
     * even with the SAME OATH model (cursor on OVH:PRO) wired in, there is
     * no prompt and no name shown at all. This is the demonstration that
     * V9 CANNOT catch a "prompt uses the cursor" bug by itself: there is
     * simply no prompt line here to compare against, in either the correct
     * or the buggy implementation. */
    TEST_ASSERT_EQ(chest_proto_parse(V9, 64, &st), CHEST_BLOCK_OK, "V9 parses");
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    model_from_view(&m, &v);
    TEST_ASSERT(!memlcd_bas_coffre(&m, b), "V9: nothing to show — no prompt, no browsing, no code");
    for (int i = 0; i < MEMLCD_BAS_LIGNES; i++) TEST_ASSERT(b[i][0] == '\0', "V9: every line empty");

    /* V15: ready, TIME bit clear, browsing (same OATH model, active mode
     * oath): NO TIME. */
    TEST_ASSERT_EQ(chest_proto_parse(V15, 64, &st), CHEST_BLOCK_OK, "V15 parses");
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o, 0);
    model_from_view(&m, &v);
    /* V15's own op (9) is still pending, so the prompt still takes
     * priority — clear it to exercise the browsing case on its own, the
     * same way the transport would once the op is confirmed/cleared. */
    m.coffre_op = 0;
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "V15: browsing shown");
    TEST_ASSERT(strcmp(b[5], "NO TIME") == 0, "V15: NO TIME, bit 3 clear");

    /* A code, visible only after both the request and a matching answer,
     * gone at its deadline — through the full chain, not just chest_oath's
     * own unit tests. */
    chest_code_t c;
    TEST_ASSERT(chest_code_decode(C1, sizeof C1, &c), "C1 decodes");
    chest_list_t page; memset(&page, 0, sizeof page);
    page.total = 1; page.count = 1; page.e[0].index = 5; strcpy(page.e[0].name, "WORK");
    chest_oath_t o2; chest_oath_reset(&o2);
    chest_oath_on_list(&o2, &page);
    chest_oath_code_requested(&o2, 5);
    chest_oath_on_code(&o2, &c, 1000);
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o2, 1000);
    model_from_view(&m, &v);
    m.coffre_op = 0;
    TEST_ASSERT(memlcd_bas_coffre(&m, b), "code shown");
    TEST_ASSERT(strcmp(b[0], "WORK") == 0 && strcmp(b[2], "418902") == 0, "code lines");
    TEST_ASSERT(strcmp(b[5], "  12 s") == 0, "countdown at t0");
    chest_view_build(&v, CHEST_BLOCK_OK, &st, st.active_mode, CHEST_MODE_ARRIVED, &o2, 13000);
    model_from_view(&m, &v);
    m.coffre_op = 0;
    TEST_ASSERT(!m.coffre_code_visible, "code gone past its deadline, all the way to the model");
    memlcd_bas_coffre(&m, b);
    TEST_ASSERT(strcmp(b[2], "418902") != 0 && strcmp(b[0], "WORK") != 0, "the expired code no longer on screen");
}

static void test_model_diff(void)
{
    memlcd_model_t a = { .route_rf = 1, .dongle_vu = 1, .batt_local_dv = 40,
                         .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identical → no redraw");
    b.batt_local_dv = 39;  TEST_ASSERT(memlcd_model_diff(&a, &b), "local voltage changes → redraw");
    b = a; b.batt_local_chg = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "charge state changes → redraw");
    b = a; b.couche = 2;   TEST_ASSERT(memlcd_model_diff(&a, &b), "layer changes → redraw");
    b = a; strcpy(b.nom, "NAV"); TEST_ASSERT(memlcd_model_diff(&a, &b), "name changes → redraw");
    b = a; b.dongle_vu = 0; TEST_ASSERT(memlcd_model_diff(&a, &b), "dongle lost → redraw");
    b = a; b.is_left = 0;  TEST_ASSERT(!memlcd_model_diff(&a, &b), "is_left is not a displayed field that moves");
    /* TRRS link: the 5 V closing is displayed (bolt in the banner), so it
     * redraws. Without this, the handshake was invisible — the only witness
     * was the console, which you don't have while typing on battery. */
    b = a; b.lien_5v = 1;  TEST_ASSERT(memlcd_model_diff(&a, &b), "TRRS 5 V closes → redraw");
    b = a; b.caps_lock = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "Caps Lock → redraw");
    b = a; b.caps_word = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "Caps Word → redraw");
    b = a; b.osm = 0x02;   TEST_ASSERT(memlcd_model_diff(&a, &b), "one-shot mod armed → redraw");
    b = a; b.osl = 2;      TEST_ASSERT(memlcd_model_diff(&a, &b), "one-shot layer armed → redraw");
    b = a; b.veille = 1;   TEST_ASSERT(memlcd_model_diff(&a, &b), "going to sleep → redraw (zZ)");
    b = a; b.coffre = MEMLCD_COFFRE_PRESENT; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest appears → redraw");
    b = a; b.coffre_op = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest prompt → redraw");
    b = a; b.coffre_op_count = 2; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest op_count changes → redraw");
    b = a; strcpy(b.coffre_label, "GITHUB"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest label changes → redraw");
    b = a; b.coffre_mode_active = CHEST_MODE_PGP; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest active mode changes → redraw");
    b = a; b.coffre_mode_wanted = CHEST_MODE_OATH; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest wanted mode changes → redraw");
    b = a; b.coffre_mode_state = CHEST_MODE_FAULT; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest mode state changes → redraw");
    b = a; b.coffre_browsing = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest browsing starts → redraw");
    b = a; b.coffre_pos = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest cursor moves → redraw");
    b = a; b.coffre_total = 12; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest page total changes → redraw");
    b = a; strcpy(b.coffre_nom, "OVH:PRO"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest cursor name changes → redraw");
    b = a; b.coffre_code_visible = 1; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code appears → redraw");
    b = a; strcpy(b.coffre_code, "123456"); TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code text changes → redraw");
    /* The countdown changes every second while a code is visible (at most
     * 30 s, chest_oath's own window bound): this makes the model diff
     * return true on most 1 s ticks during that window, same cadence the
     * status task/LVGL already poll at (STATUS_DISP_PERIODE_MS /
     * LVGL_REFR_MS, cadence.h, both 1000 ms on the memlcd halves) — no new,
     * faster periodic wait is introduced, only more REAL redraws within an
     * already-scheduled 1 s tick, so the tickless-sleep rule (no wait below
     * CADENCE_REPOS_MIN_MS) is unaffected. */
    b = a; b.coffre_code_secs = 5; TEST_ASSERT(memlcd_model_diff(&a, &b), "chest code countdown changes → redraw");
}

/* The panel is PHYSICALLY 68 lines of 160 pixels (Sharp catalog, doc
 * lemia 6844 p. 5: "LS011B7DH03 160 × 68", H = direction of data); it is
 * mounted upright. The portrait framebuffer (68 × 160, 9 bytes per row, bit 7 =
 * x = 0, 1 = ink) therefore transposes into 68 lines of 20 bytes, bit 7 = D1,
 * 1 = WHITE (app note doc 6845 p. 10: D(n) = L → black). */
static void test_fb_to_panel(void)
{
    static uint8_t fb[MEMLCD_H * MEMLCD_LINE_BYTES];
    static uint8_t panel[MEMLCD_PANEL_LINES * MEMLCD_PANEL_LINE_BYTES];
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINES, 68, "68 grid lines");
    TEST_ASSERT_EQ(MEMLCD_PANEL_LINE_BYTES, 20, "160 pixels per line = 20 bytes");

    memset(fb, 0, sizeof fb);
    memlcd_fb_to_panel(fb, panel, false);
    bool blanc = true;
    for (size_t i = 0; i < sizeof panel; i++) if (panel[i] != 0xFF) blanc = false;
    TEST_ASSERT(blanc, "empty buffer → panel all white (1 = white for Sharp)");

    /* portrait pixel (x=0, y=0): top-left corner → line 0, column 159 (90° rotation) */
    fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFE, "(0,0) → line 0, D160 (bit 0 of last byte) black");
    TEST_ASSERT_EQ(panel[1 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "line 1 is untouched");

    /* pixel (x=67, y=159): bottom-right corner → line 67, column 0 (D1 = bit 7 of byte 0) */
    memset(fb, 0, sizeof fb);
    fb[159 * MEMLCD_LINE_BYTES + 8] = 0x10;   /* x = 67 = byte 8, bit (7 - 3) */
    memlcd_fb_to_panel(fb, panel, false);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "(67,159) → line 67, D1 black");

    /* 180° rotation: (0,0) → line 67, column 0 */
    memset(fb, 0, sizeof fb); fb[0] = 0x80;
    memlcd_fb_to_panel(fb, panel, true);
    TEST_ASSERT_EQ(panel[67 * MEMLCD_PANEL_LINE_BYTES + 0], 0x7F, "rot180: (0,0) → line 67, D1");
    TEST_ASSERT_EQ(panel[0 * MEMLCD_PANEL_LINE_BYTES + 19], 0xFF, "rot180: line 0 stays white");
}

/* The displayed voltage: the ADC oscillates between two neighboring dV values
 * (a panel rewrite each time), but a "±1 around the DISPLAYED value" hysteresis
 * froze 4.2 V for a whole night while the battery lost 0.1 V (bench 2026-09-15).
 * Rule: a value DIFFERENT from the displayed one is shown once it has HELD for
 * hold_ms straight — oscillation never holds, drift eventually holds. */
static void test_batt_affichee(void)
{
    memlcd_batt_aff_t b; memlcd_batt_aff_init(&b);
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 42, 0, 30000), 42, "first measurement: displayed right away");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 10000, 30000), 42, "41 since 0 s: not yet");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 42, 20000, 30000), 42, "back to 42: the count for 41 restarts from zero");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 30000, 30000), 42, "41 again, since 0 s");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 50000, 30000), 42, "41 since 20 s: not yet");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 41, 60000, 30000), 41, "41 since 30 s: the drift is displayed");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 40, 60001, 30000), 41, "40: new candidate, restarts from zero");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 40, 90001, 30000), 40, "40 since 30 s: still follows");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 0xFF, 90002, 30000), 0xFF, "unknown: displayed without delay (not an oscillation)");
    TEST_ASSERT_EQ(memlcd_batt_aff_step(&b, 39, 90003, 30000), 39, "return of a measurement after unknown: no delay");
}

void test_memlcd_model(void)
{
    TEST_SUITE("Memory-LCD screen: pure logic");
    test_rev8();
    test_couper_nom();
    test_couche_affichee();
    test_ligne_etat();
    test_lignes_coffre();
    test_couper_8();
    test_bas_coffre();
    test_bas_coffre_end_to_end();
    test_model_diff();
    test_fb_to_panel();
    test_batt_affichee();
}
