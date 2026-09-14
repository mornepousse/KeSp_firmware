# Jauge batterie des moitiés — Plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mesurer la tension batterie sur chaque moitié, la remonter au dongle par STATUS avec l'identité de moitié, l'exposer par la CDC `BATTERY` existante, et déduire « pleine » d'un plateau de tension.

**Architecture:** Un module `power/batt_sense` par moitié lit ADC2 (GPIO13, pont 1M/1M) toutes les 10 s et au réveil, via une logique pure testée (`power/batt_calc.h` : conversion, moyenne/rejet, table SoC, machine à états pleine/en charge). `PKT_TYPE_STATUS` gagne dans son nibble de flags l'identité de moitié (bit 1) et l'état de charge (bits 2-3) — rétrocompatible, taille inchangée. La gauche remplit son STATUS à 1/s ; la droite émet un STATUS lent (30 s) au repos et au réveil. Le dongle indexe son cache par moitié ; la CDC ne change pas de format.

**Tech Stack:** C, ESP-IDF 5.5 (`esp_adc` : `adc_oneshot`, `adc_cali_curve_fitting`), nRF24 ESB, tests host CMake.

**Spec:** `docs/superpowers/specs/2026-09-14-batterie-jauge-design.md`

## Global Constraints

- `VBAT_SENSE` = `BOARD_VBAT_SENSE_GPIO` (GPIO13, ADC2_CH2) sur les deux moitiés ; pont **1 MΩ / 1 MΩ + 100 nF** → `V_batt = 2 × V_adc`. ADC2 libre (pas de WiFi).
- Unité de transport : **dV** (`uint8`, 42 = 4,2 V), **0 = inconnu** (convention existante de `batt_dV`).
- Rejet : `< 2,5 V` ou `> 4,5 V` ⇒ inconnu. Moyenne de **8** échantillons.
- Cadence mesure : **10 s** éveillée + une au réveil ; jamais en light sleep.
- STATUS : nibble bas de l'octet 0 = `bit0 mode_usb` (existant), **`bit1 HALF_RIGHT`**, **`bits2-3 charging`** (0 inconnu, 1 en charge probable, 2 pleine). Trame toujours **8 octets** (`RF_STATUS_LEN`).
- Cadence droite : **`RF_BATT_PERIOD_MS = 30000`** dans `rf_slot.h` (contrat entre firmwares). Le STATUS de la droite **n'est pas une activité** (pas de tampon de veille).
- Pleine : plateau **≥ 4,15 V pendant ≥ 120 s** ; en charge probable : **hausse ≥ 0,1 V en < 300 s** ; hystérésis 0,05 V.
- Seuils faibles (v2, table seulement en v1) : 3,5 V avertissement, 3,3 V critique ; SoC : 4,15→100, 3,9→70, 3,7→40, 3,5→15, 3,3→0 (interpolation linéaire).
- CDC `KS_CMD_BATTERY` : format inchangé ; `charging` prend 0xFF inconnu / 1 en charge probable / 2 pleine (doc à mettre à jour).
- Tout nouveau code pur : test host rouge avant, mordant à la mutation. Toute source touchée adossée à `COMPORTEMENTS.md`.
- Gardes : le module est sous `CONFIG_KASE_BATT_SENSE` (défaut n), activé dans `sdkconfig.defaults.niphar_left` et `.niphar_right`. Les 7 boards par défaut restent verts.

---

### Task 1 : Logique pure `batt_calc.h` (conversion, moyenne/rejet, SoC, états)

**Files:**
- Create: `main/power/batt_calc.h`
- Test: `test/test_batt_calc.c` (create), `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces:**
- Produces:
  - `uint8_t batt_mv_to_dv(uint32_t mv_adc);` — `mv_adc × 2` (pont) → dV arrondi ; renvoie **0** si hors `[2500, 4500] mV` batterie.
  - `uint8_t batt_dv_from_samples(const uint32_t *mv_adc, unsigned n);` — moyenne des `n` échantillons ADC (mV côté ADC) puis `batt_mv_to_dv` ; 0 si `n == 0`.
  - `uint8_t batt_soc_pct(uint8_t dv);` — table linéaire par morceaux (41.5→100, 39→70, 37→40, 35→15, 33→0), bornée 0..100 ; 0xFF si `dv == 0`.
  - `typedef enum { BATT_CHG_UNKNOWN = 0, BATT_CHG_PROBABLE = 1, BATT_CHG_FULL = 2 } batt_chg_t;`
  - `typedef struct { uint8_t ref_dv; uint32_t ref_ms; uint32_t plateau_since_ms; bool plateau; batt_chg_t etat; } batt_state_t;`
  - `batt_chg_t batt_state_step(batt_state_t *s, uint8_t dv, uint32_t now_ms);`
  - Constantes : `BATT_FULL_DV 41` (≥ 4,15 V arrondi : on compare en dixièmes → seuil 41 signifie ≥ 4,1 ; pour 4,15 exact on travaille en **centivolts** dans la FSM : `BATT_FULL_CV 415`, `BATT_RISE_CV 10`, `BATT_HYST_CV 5`, `BATT_FULL_HOLD_MS 120000`, `BATT_RISE_WINDOW_MS 300000`). La FSM prend `dv` mais convertit en cV (`dv × 10`) — un dV = 0,1 V ne distingue pas 4,15 de 4,1 : la FSM reçoit donc plutôt les **mV** : signature finale `batt_state_step(batt_state_t *s, uint32_t mv_batt, uint32_t now_ms)` avec `BATT_FULL_MV 4150`, `BATT_RISE_MV 100`, `BATT_HYST_MV 50`.
  - Et donc aussi `uint32_t batt_mv_from_samples(const uint32_t *mv_adc, unsigned n);` (mV batterie moyennés, 0 si rejet) — `batt_dv_from_samples` = `batt_mv_from_samples / 100` arrondi.

- [x] **Step 1: Write the failing test** — `test/test_batt_calc.c` :

```c
#include "test_framework.h"
#include "../main/power/batt_calc.h"

static void test_conversion_et_rejet(void)
{
    TEST_ASSERT_EQ(batt_mv_to_dv(2075), 42, "2075 mV ADC × 2 = 4,15 V → 42 dV (arrondi)");
    TEST_ASSERT_EQ(batt_mv_to_dv(1850), 37, "1850 × 2 = 3,70 V → 37");
    TEST_ASSERT_EQ(batt_mv_to_dv(1200), 0,  "2,4 V batterie : sous 2,5 V → inconnu");
    TEST_ASSERT_EQ(batt_mv_to_dv(2300), 0,  "4,6 V batterie : au-dessus de 4,5 V → inconnu");
    TEST_ASSERT_EQ(batt_mv_to_dv(0), 0,     "ADC à 0 (pont ouvert) → inconnu");
}

static void test_moyenne(void)
{
    uint32_t s[8] = { 1850, 1852, 1848, 1851, 1849, 1850, 1850, 1850 };
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 8), 3700, "moyenne × 2 = 3700 mV");
    TEST_ASSERT_EQ(batt_dv_from_samples(s, 8), 37, "→ 37 dV");
    TEST_ASSERT_EQ(batt_mv_from_samples(s, 0), 0, "aucun échantillon → inconnu");
    uint32_t bad[8] = { 0,0,0,0,0,0,0,0 };
    TEST_ASSERT_EQ(batt_dv_from_samples(bad, 8), 0, "tout à 0 → inconnu");
}

static void test_soc(void)
{
    TEST_ASSERT_EQ(batt_soc_pct(0), 0xFF, "inconnu → 0xFF");
    TEST_ASSERT_EQ(batt_soc_pct(42), 100, "4,2 V → 100 %");
    TEST_ASSERT_EQ(batt_soc_pct(39), 70,  "3,9 V → 70 %");
    TEST_ASSERT_EQ(batt_soc_pct(37), 40,  "3,7 V → 40 %");
    TEST_ASSERT_EQ(batt_soc_pct(35), 15,  "3,5 V → 15 %");
    TEST_ASSERT_EQ(batt_soc_pct(33), 0,   "3,3 V → 0 %");
    TEST_ASSERT_EQ(batt_soc_pct(30), 0,   "sous 3,3 V : 0, jamais négatif");
    TEST_ASSERT(batt_soc_pct(38) > 40 && batt_soc_pct(38) < 70, "3,8 V interpolé entre 40 et 70");
}

static void test_pleine_apres_plateau(void)
{
    batt_state_t st = {0};
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 0), BATT_CHG_UNKNOWN, "première mesure haute : pas encore pleine");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 60000), BATT_CHG_UNKNOWN, "60 s de plateau : pas encore");
    TEST_ASSERT_EQ(batt_state_step(&st, 4165, 121000), BATT_CHG_FULL, "≥ 120 s ≥ 4,15 V → PLEINE");
    TEST_ASSERT_EQ(batt_state_step(&st, 4120, 130000), BATT_CHG_FULL, "hystérésis : 4,12 V reste pleine");
    TEST_ASSERT_EQ(batt_state_step(&st, 4090, 140000), BATT_CHG_UNKNOWN, "sous 4,10 V : plus pleine");
}

static void test_en_charge_probable_si_ca_monte(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 3700, 0);
    TEST_ASSERT_EQ(batt_state_step(&st, 3750, 60000), BATT_CHG_UNKNOWN, "+50 mV : pas assez");
    TEST_ASSERT_EQ(batt_state_step(&st, 3810, 120000), BATT_CHG_PROBABLE, "+110 mV en 2 min → en charge probable");
    /* Une décharge ne fait jamais « monter » : rester/retomber inconnu. */
    batt_state_t d = {0};
    batt_state_step(&d, 3900, 0);
    TEST_ASSERT_EQ(batt_state_step(&d, 3850, 100000), BATT_CHG_UNKNOWN, "ça baisse : inconnu");
}

static void test_inconnu_reset(void)
{
    batt_state_t st = {0};
    batt_state_step(&st, 4160, 0);
    batt_state_step(&st, 4160, 130000);   /* pleine */
    TEST_ASSERT_EQ(batt_state_step(&st, 0, 140000), BATT_CHG_UNKNOWN, "mesure inconnue → état inconnu, plateau oublié");
    TEST_ASSERT_EQ(batt_state_step(&st, 4160, 150000), BATT_CHG_UNKNOWN, "il faut refaire 120 s de plateau");
}

void test_batt_calc(void)
{
    TEST_SUITE("Jauge batterie : calculs purs");
    test_conversion_et_rejet();
    test_moyenne();
    test_soc();
    test_pleine_apres_plateau();
    test_en_charge_probable_si_ca_monte();
    test_inconnu_reset();
}
```
Enregistrer `test_batt_calc.c` dans `test/CMakeLists.txt` (après `test_wake_grace.c`) et dans `test/test_main.c` (`extern void test_batt_calc(void);` + appel).

- [x] **Step 2: Run test to verify it fails**

Run: `cd test && cmake --build build -j 2>&1 | grep -iE "error" | head; ./build/test_runner | grep -iE "Jauge|FAIL|Results"`
Expected : erreur de compilation (header absent).

- [x] **Step 3: Write minimal implementation** — `main/power/batt_calc.h` :

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Jauge batterie — logique pure (test/test_batt_calc.c).
 * Matériel : VBAT_SENSE = pont 1 MΩ/1 MΩ + 100 nF → V_batt = 2 × V_adc.
 * Unité de transport : dV (42 = 4,2 V), 0 = inconnu (convention batt_dV).
 * Rejet : hors [2,5 V ; 4,5 V] = capteur absent / pont ouvert / erreur ADC. */
#define BATT_DIVIDER_NUM   2u
#define BATT_MIN_MV        2500u
#define BATT_MAX_MV        4500u

#define BATT_FULL_MV       4150u   /* plateau de fin de charge (TP4056 sans STDBY) */
#define BATT_FULL_HOLD_MS  120000u /* tenir ≥ 2 min au-dessus pour dire « pleine » */
#define BATT_RISE_MV       100u    /* hausse qui trahit une charge en cours */
#define BATT_RISE_WINDOW_MS 300000u
#define BATT_HYST_MV       50u

static inline uint32_t batt_mv_from_adc(uint32_t mv_adc) { return mv_adc * BATT_DIVIDER_NUM; }

static inline bool batt_mv_plausible(uint32_t mv_batt)
{ return mv_batt >= BATT_MIN_MV && mv_batt <= BATT_MAX_MV; }

static inline uint8_t batt_mv_to_dv_batt(uint32_t mv_batt)
{ return batt_mv_plausible(mv_batt) ? (uint8_t)((mv_batt + 50u) / 100u) : 0u; }

static inline uint8_t batt_mv_to_dv(uint32_t mv_adc)
{ return batt_mv_to_dv_batt(batt_mv_from_adc(mv_adc)); }

static inline uint32_t batt_mv_from_samples(const uint32_t *mv_adc, unsigned n)
{
    if (n == 0) return 0;
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) sum += mv_adc[i];
    uint32_t mv = batt_mv_from_adc((uint32_t)(sum / n));
    return batt_mv_plausible(mv) ? mv : 0;
}

static inline uint8_t batt_dv_from_samples(const uint32_t *mv_adc, unsigned n)
{ return batt_mv_to_dv_batt(batt_mv_from_samples(mv_adc, n)); }

/* SoC approché, Li-ion 16340 au repos : 4,15→100, 3,9→70, 3,7→40, 3,5→15, 3,3→0. */
static inline uint8_t batt_soc_pct(uint8_t dv)
{
    if (dv == 0) return 0xFF;
    static const struct { uint8_t dv; uint8_t pct; } t[] =
        { {33, 0}, {35, 15}, {37, 40}, {39, 70}, {42, 100} };
    if (dv <= t[0].dv) return 0;
    for (unsigned i = 1; i < sizeof t / sizeof t[0]; i++)
        if (dv <= t[i].dv) {
            uint32_t span = t[i].dv - t[i-1].dv, off = dv - t[i-1].dv;
            return (uint8_t)(t[i-1].pct + (t[i].pct - t[i-1].pct) * off / span);
        }
    return 100;
}

typedef enum { BATT_CHG_UNKNOWN = 0, BATT_CHG_PROBABLE = 1, BATT_CHG_FULL = 2 } batt_chg_t;

typedef struct {
    uint32_t   ref_mv, ref_ms;          /* point de référence pour la hausse */
    uint32_t   plateau_since_ms;
    bool       plateau;
    batt_chg_t etat;
} batt_state_t;

/* Une mesure (mV batterie, 0 = inconnue) à l'instant now_ms. « En charge » n'est
 * pas mesurable (pas de VBUS) : on DÉDUIT. Pleine = plateau ≥ BATT_FULL_MV tenu
 * BATT_FULL_HOLD_MS, gardée avec hystérésis ; en charge probable = hausse ≥
 * BATT_RISE_MV en moins de BATT_RISE_WINDOW_MS (une décharge ne monte jamais). */
static inline batt_chg_t batt_state_step(batt_state_t *s, uint32_t mv, uint32_t now_ms)
{
    if (mv == 0) { *s = (batt_state_t){0}; return BATT_CHG_UNKNOWN; }
    /* plateau haut */
    if (mv >= BATT_FULL_MV) {
        if (!s->plateau) { s->plateau = true; s->plateau_since_ms = now_ms; }
        if ((uint32_t)(now_ms - s->plateau_since_ms) >= BATT_FULL_HOLD_MS) s->etat = BATT_CHG_FULL;
    } else if (mv < BATT_FULL_MV - BATT_HYST_MV) {
        s->plateau = false;
        if (s->etat == BATT_CHG_FULL) s->etat = BATT_CHG_UNKNOWN;
    }
    if (s->etat == BATT_CHG_FULL) return s->etat;
    /* hausse = charge probable */
    if (s->ref_ms == 0 && s->ref_mv == 0) { s->ref_mv = mv; s->ref_ms = now_ms; }
    if ((uint32_t)(now_ms - s->ref_ms) > BATT_RISE_WINDOW_MS || mv < s->ref_mv) {
        s->ref_mv = mv; s->ref_ms = now_ms;          /* fenêtre glissante, repart du bas */
        if (s->etat == BATT_CHG_PROBABLE && mv < s->ref_mv) s->etat = BATT_CHG_UNKNOWN;
    } else if (mv >= s->ref_mv + BATT_RISE_MV) {
        s->etat = BATT_CHG_PROBABLE;
    }
    return s->etat;
}
```
⚠ Dans le test `test_en_charge_probable_si_ca_monte`, la séquence 3700 → 3750 → 3810 doit donner PROBABLE au 3e pas (référence 3700 gardée car la fenêtre n'a pas expiré et ça ne baisse pas). Si le test rouge révèle une différence, ajuster **le code**, pas le test.

- [x] **Step 4: Run test to verify it passes** ; puis **mutation** : mettre `BATT_FULL_HOLD_MS` à `0` (pleine immédiate) → `test_pleine_apres_plateau` rouge ; restaurer depuis une copie scratchpad (fichier neuf, **pas** `git checkout`).

Run: `cd test && cmake --build build -j && ./build/test_runner | grep Results`
Expected : PASS.

- [x] **Step 5: Commit**

```bash
git add main/power/batt_calc.h test/test_batt_calc.c test/CMakeLists.txt test/test_main.c .tripwire-testcount
git commit -m "feat(batt): logique pure de la jauge — conversion, moyenne/rejet, SoC, états pleine/en charge (testé)"
```

---

### Task 2 : STATUS porte l'identité de moitié et l'état de charge (rétrocompatible)

**Files:**
- Modify: `main/comm/rf/rf_packet.h` (struct `rf_status_t`, flags, `rf_encode_status`, `rf_decode_status`)
- Test: `test/test_rf_packet.c` (étendre)

**Interfaces:**
- Produces:
  - `#define PKT_STATUS_FLAG_HALF_RIGHT 0x2`, `#define PKT_STATUS_CHG_SHIFT 2`, `#define PKT_STATUS_CHG_MASK 0x3`
  - `rf_status_t` gagne `uint8_t half;` (`RF_HALF_LEFT`/`RF_HALF_RIGHT`) et `uint8_t charging;` (0..2)
  - encode : `buf[0] = (PKT_TYPE_STATUS<<4) | (mode_usb?0x1:0) | (half==RF_HALF_RIGHT?0x2:0) | ((charging & 0x3) << 2)`
  - decode : `half = (buf[0]&0x2) ? RF_HALF_RIGHT : RF_HALF_LEFT ; charging = (buf[0]>>2)&0x3`
  - Taille inchangée : 8 octets.

- [x] **Step 1: Write the failing test** — ajouter dans `test/test_rf_packet.c` (près de `test_rf_status_config_fp`) et l'appeler dans la suite :

```c
static void test_rf_status_half_et_charge(void)
{
    rf_status_t in = { .batt_dV = 41, .link_q = 0, .seq = 3, .mode_usb = false,
                       .half = RF_HALF_RIGHT, .charging = 2 };
    uint8_t buf[RF_STATUS_LEN];
    uint16_t n = rf_encode_status(buf, &in);
    TEST_ASSERT_EQ(n, RF_STATUS_LEN, "toujours 8 octets");
    rf_status_t out = {0};
    TEST_ASSERT(rf_decode_status(buf, n, &out), "decode");
    TEST_ASSERT_EQ(out.half, RF_HALF_RIGHT, "identité droite round-trip");
    TEST_ASSERT_EQ(out.charging, 2, "état de charge round-trip");
    TEST_ASSERT_EQ(out.batt_dV, 41, "tension intacte");
    /* Rétrocompatibilité : une trame ancienne (nibble = mode_usb seul) = gauche, inconnu. */
    rf_status_t old_in = { .batt_dV = 37, .seq = 1, .mode_usb = true };
    rf_encode_status(buf, &old_in);
    TEST_ASSERT(rf_decode_status(buf, RF_STATUS_LEN, &out), "decode ancienne");
    TEST_ASSERT_EQ(out.half, RF_HALF_LEFT, "bit1 à 0 → gauche");
    TEST_ASSERT_EQ(out.charging, 0, "bits 2-3 à 0 → inconnu");
    TEST_ASSERT(out.mode_usb, "mode_usb préservé");
}
```

- [x] **Step 2: Run test to verify it fails** — `.half`/`.charging` inexistants → erreur de compilation.

- [x] **Step 3: Write minimal implementation** — dans `rf_packet.h` : ajouter à `rf_status_t` (après `mode_usb`) :
```c
    uint8_t half;      /* RF_HALF_LEFT/RF_HALF_RIGHT — bit1 du nibble, 0 = gauche (rétrocompat) */
    uint8_t charging;  /* 0 inconnu, 1 en charge probable, 2 pleine — bits 2-3 du nibble */
```
Après `#define PKT_STATUS_FLAG_MODE_USB 0x1` :
```c
#define PKT_STATUS_FLAG_HALF_RIGHT 0x2
#define PKT_STATUS_CHG_SHIFT       2
#define PKT_STATUS_CHG_MASK        0x3
```
Encode, remplacer la ligne `buf[0] = ...` par :
```c
    buf[0] = (uint8_t)((PKT_TYPE_STATUS << 4) |
                       (s->mode_usb ? PKT_STATUS_FLAG_MODE_USB : 0) |
                       (s->half == RF_HALF_RIGHT ? PKT_STATUS_FLAG_HALF_RIGHT : 0) |
                       ((s->charging & PKT_STATUS_CHG_MASK) << PKT_STATUS_CHG_SHIFT));
```
Decode, après `out->mode_usb = ...` :
```c
    out->half     = (buf[0] & PKT_STATUS_FLAG_HALF_RIGHT) ? RF_HALF_RIGHT : RF_HALF_LEFT;
    out->charging = (uint8_t)((buf[0] >> PKT_STATUS_CHG_SHIFT) & PKT_STATUS_CHG_MASK);
```
(`RF_HALF_LEFT/RIGHT` sont définis plus haut dans le même header.)

- [x] **Step 4: Run test to verify it passes** — toute la suite doit rester verte (les autres tests STATUS initialisent `half=0` = gauche par défaut).

- [x] **Step 5: Commit**
```bash
git add main/comm/rf/rf_packet.h test/test_rf_packet.c .tripwire-testcount
git commit -m "feat(rf): STATUS porte l'identité de moitié et l'état de charge (nibble, rétrocompatible)"
```

---

### Task 3 : Module `batt_sense` (ADC2, 10 s, réveil) sur les deux moitiés

**Files:**
- Create: `main/power/batt_sense.h`, `main/power/batt_sense.c`
- Modify: `main/CMakeLists.txt` (près de `list(APPEND srcs "power/veille.c")`), `main/Kconfig.projbuild` (après le bloc `KASE_VEILLE_PROFONDE_S`), `sdkconfig.defaults.niphar_left`, `sdkconfig.defaults.niphar_right`, `main/main.c` (init), `main/power/veille.c` (mesure au réveil)

**Interfaces:**
- Consumes: `batt_calc.h` (Task 1).
- Produces:
  - `void batt_sense_init(void);` — configure ADC2 via `adc_oneshot_io_to_channel(BOARD_VBAT_SENSE_GPIO, …)`, calibration courbe, lance un `esp_timer` périodique 10 s ; fait une première mesure.
  - `void batt_sense_sample_now(void);` — une mesure immédiate (réveil).
  - `uint8_t batt_sense_dv(void);` — dernière tension valide en dV, 0 = inconnue.
  - `uint8_t batt_sense_charging(void);` — `batt_chg_t` courant (0/1/2).
  - `uint32_t batt_sense_age_ms(void);` — ancienneté de la dernière mesure valide.

- [x] **Step 1: Kconfig + CMake + defaults**

`main/Kconfig.projbuild`, après le bloc `KASE_VEILLE_PROFONDE_S` :
```
config KASE_BATT_SENSE
    bool "Jauge batterie : lecture ADC de VBAT_SENSE (moities Niphargus)"
    default n
    help
        Mesure la tension batterie toutes les 10 s et au reveil sur
        BOARD_VBAT_SENSE_GPIO (pont 1M/1M, ADC2). Remontee au dongle par STATUS.
        ADC2 exige l'absence de WiFi — vrai sur le Niphargus (nRF24 seul).
```
`main/CMakeLists.txt`, à côté de veille.c :
```cmake
if(CONFIG_KASE_BATT_SENSE)
    list(APPEND srcs "power/batt_sense.c")
endif()
```
(Adopter la forme exacte utilisée pour `power/veille.c` — si veille est ajouté sans garde, ajouter batt_sense de la même manière **avec** la garde `if(CONFIG_KASE_BATT_SENSE)`.) Vérifier que le composant dépend de `esp_adc` (`REQUIRES`/`PRIV_REQUIRES` dans le même CMakeLists ; l'ajouter s'il manque).

`sdkconfig.defaults.niphar_left` et `sdkconfig.defaults.niphar_right` : ajouter `CONFIG_KASE_BATT_SENSE=y` (les dossiers `build_niphar_*` existants ont un sdkconfig généré : y ajouter la même ligne, ou `idf.py reconfigure` après suppression du sdkconfig du build).

- [x] **Step 2: `batt_sense.h`**
```c
#pragma once
#include <stdint.h>
/* Jauge batterie d'une moitié — voir batt_calc.h pour la logique pure.
 * Mesure toutes les 10 s éveillée et au réveil ; jamais en light sleep
 * (le timer esp_timer est gelé par le sommeil, batt_sense_sample_now() est
 * appelé par veille.c au retour). 0 = inconnu. */
void     batt_sense_init(void);
void     batt_sense_sample_now(void);
uint8_t  batt_sense_dv(void);
uint8_t  batt_sense_charging(void);   /* batt_chg_t : 0 inconnu, 1 en charge probable, 2 pleine */
uint32_t batt_sense_age_ms(void);
```

- [x] **Step 3: `batt_sense.c`**
```c
#include "batt_sense.h"
#include "batt_calc.h"
#include "board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "batt";
#define BATT_PERIOD_US   (10ULL * 1000 * 1000)
#define BATT_SAMPLES     8

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t         s_cali;      /* NULL = pas de calibration */
static adc_channel_t             s_chan;
static adc_unit_t                s_unit_id;
static volatile uint8_t          s_dv;        /* 0 = inconnu */
static volatile uint8_t          s_chg;
static volatile uint32_t         s_last_ms;
static batt_state_t              s_state;
static esp_timer_handle_t        s_timer;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t read_mv_once(void)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_unit, s_chan, &raw) != ESP_OK) return 0;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return (uint32_t)mv;
    /* Sans calibration : 12 bits, 12 dB ≈ 0..3100 mV, approximation documentée. */
    return (uint32_t)raw * 3100u / 4095u;
}

void batt_sense_sample_now(void)
{
    if (!s_unit) return;
    uint32_t s[BATT_SAMPLES];
    esp_rom_delay_us(200);                     /* le 100 nF du pont se stabilise */
    for (int i = 0; i < BATT_SAMPLES; i++) { s[i] = read_mv_once(); esp_rom_delay_us(100); }
    uint32_t mv = batt_mv_from_samples(s, BATT_SAMPLES);
    uint8_t  dv = batt_mv_to_dv_batt(mv);
    uint32_t t  = now_ms();
    s_chg = (uint8_t)batt_state_step(&s_state, mv, t);
    if (dv) { s_dv = dv; s_last_ms = t; }
    else    { s_dv = 0; }
    ESP_LOGD(TAG, "batt %u mV -> %u dV etat=%u", (unsigned)mv, dv, s_chg);
}

static void timer_cb(void *arg) { (void)arg; batt_sense_sample_now(); }

void batt_sense_init(void)
{
    if (adc_oneshot_io_to_channel(BOARD_VBAT_SENSE_GPIO, &s_unit_id, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d n'est pas une entree ADC", BOARD_VBAT_SENSE_GPIO); return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = s_unit_id, .ulp_mode = ADC_ULP_MODE_DISABLE };
    if (adc_oneshot_new_unit(&ucfg, &s_unit) != ESP_OK) { ESP_LOGE(TAG, "ADC init KO"); s_unit = NULL; return; }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_unit, s_chan, &ccfg);
    adc_cali_curve_fitting_config_t cal = { .unit_id = s_unit_id, .chan = s_chan,
                                            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL; ESP_LOGW(TAG, "pas de calibration ADC : lecture approchee");
    }
    batt_sense_sample_now();
    const esp_timer_create_args_t a = { .callback = timer_cb, .name = "batt" };
    if (esp_timer_create(&a, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, BATT_PERIOD_US);
    ESP_LOGI(TAG, "jauge : %u dV (etat %u), mesure toutes les 10 s", s_dv, s_chg);
}

uint8_t  batt_sense_dv(void)       { return s_dv; }
uint8_t  batt_sense_charging(void) { return s_chg; }
uint32_t batt_sense_age_ms(void)   { return s_last_ms ? (uint32_t)(now_ms() - s_last_ms) : 0xFFFFFFFFu; }
```
Si `ADC_ATTEN_DB_12` n'existe pas dans cette version d'IDF, utiliser `ADC_ATTEN_DB_11` (même plage, ancien nom).

- [x] **Step 4: Init + réveil**

`main/main.c` : après `veille_liberer_gpio();` (ligne ~194) ajouter :
```c
#if CONFIG_KASE_BATT_SENSE
  batt_sense_init();
#endif
```
avec `#include "batt_sense.h"` sous la même garde en tête de fichier.

`main/power/veille.c` : au réveil, juste après `matrix_wake_capture();` (et sa relecture anti-rebond) :
```c
#if CONFIG_KASE_BATT_SENSE
    batt_sense_sample_now();   /* une mesure au réveil : le timer était gelé */
#endif
```
avec l'include gardé.

- [x] **Step 5: Build + flash + preuve console**

Build gauche fusion + droite fusion (+ défauts) : `rc=0`. Flasher la moitié qui porte le FTDI, capturer le boot : la ligne `batt: jauge : NN dV` doit afficher une tension **plausible (36–42)** ; sinon investiguer avant d'aller plus loin (rejet → 0 = pont ou canal faux).

- [x] **Step 6: Commit**
```bash
git add main/power/batt_sense.c main/power/batt_sense.h main/CMakeLists.txt main/Kconfig.projbuild sdkconfig.defaults.niphar_left sdkconfig.defaults.niphar_right main/main.c main/power/veille.c
git commit -m "feat(batt): module batt_sense — ADC2 sur VBAT_SENSE, 8 échantillons, 10 s + réveil"
```

---

### Task 4 : La gauche remplit son STATUS

**Files:**
- Modify: `main/comm/rf/kbd_relay_tx.c` (deux `rf_status_t st = {...}` : RF ~ligne 396 et USB ~ligne 261)

**Interfaces:**
- Consumes: `batt_sense_dv()`, `batt_sense_charging()` (Task 3), champs `half/charging` (Task 2).

- [x] **Step 1:** En tête de `kbd_relay_tx.c`, sous garde :
```c
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
#define KBD_BATT_DV()  batt_sense_dv()
#define KBD_BATT_CHG() batt_sense_charging()
#else
#define KBD_BATT_DV()  0
#define KBD_BATT_CHG() 0
#endif
```
- [x] **Step 2:** Dans les deux initialisations de `rf_status_t st`, remplacer `.batt_dV = 0` par `.batt_dV = KBD_BATT_DV()` et ajouter `.half = RF_HALF_LEFT, .charging = KBD_BATT_CHG()`.
- [x] **Step 3:** Build gauche fusion + défaut `rc=0`. Flash (si FTDI sur la gauche) ; sinon la preuve viendra du dongle (Task 6).
- [x] **Step 4: Commit**
```bash
git add main/comm/rf/kbd_relay_tx.c
git commit -m "feat(batt): la gauche annonce sa tension et son état de charge dans STATUS"
```

---

### Task 5 : La droite émet un STATUS lent (30 s) et au réveil

**Files:**
- Modify: `main/comm/rf/rf_slot.h` (constante), `main/comm/rf/half_link.c` (factorisation de l'envoi, STATUS lent), `main/comm/rf/half_link.h` (déclaration)

**Interfaces:**
- Consumes: `batt_sense_*` (Task 3), `rf_encode_status` avec `half/charging` (Task 2), `RF_STATUS_LEN`.
- Produces: `#define RF_BATT_PERIOD_MS 30000u` (rf_slot.h) ; `bool half_link_tx_status(void);` ; interne `static bool half_link_tx_frame(const uint8_t *buf, uint8_t n);`.

- [x] **Step 1:** `rf_slot.h`, après `RF_STATUS_PERIOD_MS` :
```c
#define RF_BATT_PERIOD_MS    30000u  /* STATUS lent de la droite au repos (jauge) — contrat avec le dongle */
```
- [x] **Step 2:** Factoriser dans `half_link.c` : extraire de `half_link_tx_matrix()` la partie « prise du mutex → `rf_driver_send` → chien de garde/bascule → rendu du mutex → instrument » en `static bool half_link_tx_frame(const uint8_t *buf, uint8_t n)` ; `half_link_tx_matrix` ne garde que l'encodage (MATRIX ou HEARTBEAT selon la cible) puis appelle `half_link_tx_frame`. Comportement strictement identique (mêmes compteurs, même FSM).
- [x] **Step 3:** Ajouter :
```c
#if CONFIG_KASE_BATT_SENSE
#include "batt_sense.h"
static uint32_t s_dernier_status_ms;   /* 0 = forcer au prochain tick (boot, réveil) */

bool half_link_tx_status(void)
{
    rf_status_t st = { .batt_dV = batt_sense_dv(), .link_q = 0, .seq = s_seq,
                       .mode_usb = false, .half = RF_HALF_RIGHT,
                       .charging = batt_sense_charging(), .config_fp = 0 };
    uint8_t buf[RF_STATUS_LEN];
    uint16_t n = rf_encode_status(buf, &st);
    return half_link_tx_frame(buf, (uint8_t)n);
}
#endif
```
et dans `half_link_tx_refresh_task`, juste après `half_link_tx_update(NULL, false);` :
```c
#if CONFIG_KASE_BATT_SENSE
        {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_dernier_status_ms == 0 || (uint32_t)(now - s_dernier_status_ms) >= RF_BATT_PERIOD_MS) {
                s_dernier_status_ms = now ? now : 1;
                half_link_tx_status();   /* PAS une activité : ne touche pas last_activity_time_ms */
            }
        }
#endif
```
et dans `half_link_radio_wake()` : `#if CONFIG_KASE_BATT_SENSE s_dernier_status_ms = 0; #endif` (un STATUS au réveil).
Déclarer `bool half_link_tx_status(void);` dans `half_link.h` sous `#if CONFIG_KASE_BATT_SENSE`.
⚠ Le STATUS de la droite part vers sa **cible courante** (FSM) : vers le dongle normalement ; s'il part vers la gauche (repli), la gauche l'ignore — acceptable (spec §2).
- [x] **Step 4:** Build droite fusion + défaut `rc=0`. Flash droite (FTDI dessus) : console → toutes les 30 s une trame de plus dans `TX N envois` au repos, et **le compteur d'activité ne bouge pas** (la droite doit toujours s'endormir à 60 s).
- [x] **Step 5: Commit**
```bash
git add main/comm/rf/rf_slot.h main/comm/rf/half_link.c main/comm/rf/half_link.h
git commit -m "feat(batt): la droite émet un STATUS lent (30 s) avec sa tension, et un au réveil"
```

---

### Task 6 : Dongle — cache par moitié, SoC, CDC inchangé

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c` (`cache_battery`, appels), `main/comm/rf/dongle_state.c` (commentaire d'indexation), `docs/CDC_BINARY_PROTOCOL.md` (sémantique `charging`)

**Interfaces:**
- Consumes: `rf_status_t.half/.charging` (Task 2), `batt_soc_pct` (Task 1, pur — le dongle l'inclut : `#include "batt_calc.h"`).

- [x] **Step 1:** Dans `rf_rx_task.c`, remplacer `cache_battery` :
```c
#include "batt_calc.h"   /* batt_soc_pct — SoC calculé côté dongle, pas transporté */
/* Indexé par MOITIÉ (0 = gauche, 1 = droite), pas par slot : en fusion les deux
 * moitiés partagent le slot clavier. Le SoC est dérivé de la tension ici. */
static void cache_battery_half(uint8_t half, uint8_t batt_dV, uint8_t charging)
{
    uint8_t chg = (batt_dV == 0) ? 0xFF : charging;   /* 0xFF inconnu (doc CDC) */
    dongle_cache_set_battery(half == RF_HALF_RIGHT ? 1 : 0, batt_dV, batt_soc_pct(batt_dV), chg);
}
```
Appel STATUS (ligne ~190) : `cache_battery_half(st.half, st.batt_dV, st.charging);`.
Appel HEARTBEAT (ligne ~207, ancien format) : `cache_battery_half(RF_HALF_LEFT, h.batt_dV, 0);` — le heartbeat ne porte pas d'identité ; hors fusion il ne vient que de la gauche.
- [x] **Step 2:** `dongle_state.c` : commentaire de `s_batt[2]` → « index = moitié (0 gauche, 1 droite) ».
- [x] **Step 3:** `docs/CDC_BINARY_PROTOCOL.md`, ligne `charging` de BATTERY : `0xFF = inconnu (pas de VBUS : l'état est DÉDUIT), 1 = en charge probable (tension qui monte), 2 = pleine (plateau ≥ 4,15 V ≥ 2 min), 0 = décharge/inconnu`. Et `soc_pct` : « dérivé de la tension par le dongle (table Li-ion 16340), 0xFF si tension inconnue ».
- [x] **Step 4:** Build dongle fusion + dongle défaut `rc=0`. Flash dongle (CH340 ttyUSB0). **Preuve banc** : script CDC `KS_CMD_BATTERY` (même patron que `coh.py`, id dans `cdc_binary_protocol.h`) → slot 0 (gauche) et slot 1 (droite) affichent des dV plausibles (36–42), `soc` cohérent, `age` frais (gauche ~1 s, droite ≤ 30 s).
- [x] **Step 5: Commit**
```bash
git add main/comm/rf/rf_rx_task.c main/comm/rf/dongle_state.c docs/CDC_BINARY_PROTOCOL.md
git commit -m "feat(batt): le dongle indexe la batterie par moitié et dérive le SoC ; CDC BATTERY documentée"
```

---

### Task 7 : Contrat, smoke, plateau « pleine » au banc, push

**Files:**
- Modify: `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`, `docs/NIPHARGUS_V2_HARDWARE.md` (note : jauge implémentée), mémoire de session.

- [x] **Step 1:** `COMPORTEMENTS.md`, nouvelle section « Batterie » :
  - `[test:test_batt_calc] La tension batterie est convertie depuis le pont 1M/1M, moyennée, et rejetée hors [2,5 V ; 4,5 V] (0 = inconnu) ; le SoC est une table Li-ion bornée ; « pleine » exige un plateau ≥ 4,15 V tenu 2 min avec hystérésis, « en charge probable » une hausse ≥ 0,1 V — une décharge ne l'est jamais.`
  - `[test:test_rf_status_half_et_charge] STATUS porte l'identité de moitié et l'état de charge dans son nibble de flags ; une trame ancienne se lit gauche/inconnu (rétrocompatible).`
  - `[smoke:Jauge batterie] Les deux moitiés remontent une tension plausible au dongle (CDC BATTERY, slots gauche/droite), la droite toutes les 30 s sans s'empêcher de dormir ; une moitié éteinte repasse « inconnu » ; en charge, PLEINE apparaît après le plateau.`
- [x] **Step 2:** `docs/HARDWARE_SMOKE_TEST.md`, section Half : item « Jauge batterie : CDC BATTERY donne 36–42 dV pour chaque moitié avec age frais ; la droite s'endort toujours à 60 s malgré son STATUS lent ; brancher la charge → après ≥ 2 min à ≥ 4,15 V, charging = 2 ».
- [x] **Step 3:** `TRIPWIRE_CONTRAT_STRICT=1 ./scripts/check.sh --fast` vert ; builds : gauche/droite fusion + défaut, dongle fusion + défaut, `kase_v2` `rc=0`.
- [x] **Step 4 (banc) :** dérouler le smoke ; noter les tensions lues et l'écart éventuel avec un voltmètre (±0,1 V acceptable).
- [x] **Step 5: Commit + push**
```bash
git add COMPORTEMENTS.md docs/HARDWARE_SMOKE_TEST.md docs/NIPHARGUS_V2_HARDWARE.md
git commit -m "feat(batt): contrat, smoke et doc — jauge batterie livrée (v1)"
git push origin main   # devshell : pre-push = 7 boards
```

---

## Notes d'exécution

- **Ordre** : Tasks 1-2 (pur, host) → 3 (ADC, à prouver à la console : une tension plausible AVANT toute remontée) → 4-5 (émetteurs) → 6 (dongle, preuve CDC) → 7.
- **Flash** : app-only `write-flash 0x20000`, MAC vérifié (gauche `d0:cf:13:21:92:60`, droite `80:b5:4e:eb:5e:08`, dongle `ac:a7:04:18:82:24`). Le FTDI ne sert qu'une moitié à la fois : flasher l'autre quand il est déplacé.
- **v2 (hors plan)** : effets de batterie faible (écran/LED/refus) — la table SoC et les seuils sont déjà là.
