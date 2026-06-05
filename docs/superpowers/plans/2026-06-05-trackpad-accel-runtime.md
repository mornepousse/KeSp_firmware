# Trackpad Accel Curve (runtime-configurable, approach B) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rendre le pointeur trackpad agréable (précis en lent, rapide en grand déplacement) via une courbe d'accélération qui tourne sur le **dongle**, réglable à chaud depuis le soft par CDC binaire.

**Architecture:** La moitié devient un capteur brut : elle envoie `{ge0,ge1,n_fingers,rel_x,rel_y}` par NRF. Le dongle décode, applique `trackpad_map()` (gestes + courbe d'accel paramétrée par `trackpad_cfg_t`) et émet le HID souris. La config vit en NVS dongle, réglée par CDC `0xB8` (get) / `0xB9` (set). La logique pure (`trackpad_map`, courbe, encode/decode cfg) est dans un fichier compilé dongle + tests host.

**Tech Stack:** C (ESP-IDF), protocole RF NRF24 (rf_packet.h, big-endian), protocole CDC binaire KS/KR (little-endian), NVS, CMake host tests.

**Référence spec:** `docs/superpowers/specs/2026-06-05-trackpad-accel-runtime-design.md`

**Conventions à suivre :** `ks_respond(cmd, KS_STATUS_OK, buf, len)`, `ks_respond_err(cmd, KS_STATUS_ERR_INVALID|ERR_RANGE)`, table `bin_cmd_table[]`, gating `#if CONFIG_KASE_HAS_RF_RX` (dongle) / `#if CONFIG_KASE_HAS_TRACKPAD` (half). NVS via `nvs_save_blob_with_total` / `nvs_load_blob_with_total` (lire `main/sys/nvs_utils.h` pour la signature exacte). Pas de malloc hot-path. Le fichier pur n'inclut que `<stdint.h>` (+ `rf_packet.h` pour le type wire).

---

## File Structure

- Modify `main/comm/rf/rf_packet.h` — `rf_trackpad_t` devient le payload **brut** ; `rf_encode_trackpad`/`rf_decode_trackpad` réécrits (9 octets).
- Create `main/periph/trackpad/trackpad_map.c` — partie **pure** : `trackpad_map()` (gestes + courbe), `clamp8()`, et (Lot 3) `trackpad_cfg_encode/decode`. Déplacée depuis `trackpad.c`.
- Modify `main/periph/trackpad/trackpad.h` — `trackpad_out_t` (sortie map), `trackpad_cfg_t`, nouvelles signatures.
- Modify `main/periph/trackpad/trackpad.c` — ne garde que le matériel half ; la tâche encode le **brut** et envoie ; n'appelle plus `trackpad_map`.
- Modify `main/comm/rf/rf_rx_task.c` — le dongle décode le brut, appelle `trackpad_map(..., &cfg, &state, &out)`, émet `hid_send_mouse`.
- Modify `main/comm/cdc/cdc_binary_protocol.h` — `KS_CMD_TRACKPAD_GET=0xB8`, `KS_CMD_TRACKPAD_SET=0xB9`.
- Modify `main/comm/cdc/cdc_binary_cmds.c` — handlers `bin_cmd_trackpad_get/set` + table.
- Create `main/periph/trackpad/trackpad_cfg.c` (Lot 3) — chargement/sauvegarde NVS + config active dongle (`trackpad_cfg_get/set/load`).
- Modify `main/CMakeLists.txt` — `trackpad_map.c` compilé dongle + half ; `trackpad_cfg.c` dongle.
- Modify `test/CMakeLists.txt`, `test/test_main.c` — câbler nouveaux tests ; remplacer `trackpad.c` par `trackpad_map.c` dans la liste.
- Modify `test/test_rf_packet.c`, `test/test_trackpad_map.c` ; Create `test/test_trackpad_cfg.c`.
- Modify `docs/CDC_BINARY_PROTOCOL.md`, `scripts/test_binary_protocol.py` (Lot 3).

---

## Task 1 : Refactor flux B (payload brut + map sur le dongle, comportement identique)

But : déplacer le traitement sans changer le ressenti. La courbe existe mais avec une config par défaut neutre (`base=100, accel=0, gain_max=100` → gain 1.0 = comportement actuel `SENS 3/3`).

**Files:** Modify `rf_packet.h`, `trackpad.h`; Create `trackpad_map.c`; Modify `trackpad.c`, `rf_rx_task.c`, `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`, `test/test_rf_packet.c`, `test/test_trackpad_map.c`.

- [ ] **Step 1 : Nouveaux types dans `trackpad.h`**

Remplacer la déclaration de `trackpad_map` et ajouter `trackpad_out_t` + `trackpad_cfg_t`. Garder `trackpad_state_t` tel quel. Nouveau contenu de la section types/proto :

```c
/* Sortie HID-ready du mapping (anciennement les champs de rf_trackpad_t). */
typedef struct {
    int8_t  dx, dy;
    uint8_t buttons;     /* bits: 0x01 L, 0x02 R, 0x04 M */
    int8_t  scroll_v, scroll_h;
} trackpad_out_t;

/* Config courbe d'accel (réglable runtime en Lot 3). Gains en centièmes (×100). */
#define TRACKPAD_CFG_FMT   0x01
typedef struct {
    uint8_t  fmt;        /* = TRACKPAD_CFG_FMT */
    uint16_t base;       /* gain bas-régime ×100 (100 = 1.00×) */
    uint16_t accel;      /* pente : ×100 de gain ajouté par unité de vitesse / TRACKPAD_ACCEL_DEN */
    uint16_t gain_max;   /* plafond de gain ×100 */
} trackpad_cfg_t;

#define TRACKPAD_ACCEL_DEN   100   /* échelle de la pente (constante) */

/* Pure : mappe les champs bruts IQS5xx → sortie HID, avec courbe d'accel (cfg).
 * Retourne true si un paquet doit être émis (gate d'activité). */
bool trackpad_map(uint8_t ge0, uint8_t ge1, uint8_t n_fingers,
                  int16_t rel_x, int16_t rel_y,
                  const trackpad_cfg_t *cfg,
                  trackpad_state_t *state, trackpad_out_t *out);
```

- [ ] **Step 2 : Payload brut dans `rf_packet.h`**

Remplacer la struct `rf_trackpad_t` (lignes 39-44) et les fonctions `rf_encode_trackpad`/`rf_decode_trackpad` par la version brute (9 octets, `rel_x/rel_y` big-endian) :

```c
typedef struct {
    uint8_t ge0, ge1;     /* GestureEvents0/1 */
    uint8_t n_fingers;
    int16_t rel_x, rel_y; /* RelativeX/Y bruts */
    uint8_t seq;
} rf_trackpad_t;
```

```c
static inline uint16_t rf_encode_trackpad(uint8_t *buf, const rf_trackpad_t *t)
{
    buf[0] = (PKT_TYPE_TRACKPAD << 4);
    buf[1] = t->ge0;
    buf[2] = t->ge1;
    buf[3] = t->n_fingers;
    buf[4] = (uint8_t)((uint16_t)t->rel_x >> 8);   /* big-endian */
    buf[5] = (uint8_t)((uint16_t)t->rel_x & 0xFF);
    buf[6] = (uint8_t)((uint16_t)t->rel_y >> 8);
    buf[7] = (uint8_t)((uint16_t)t->rel_y & 0xFF);
    buf[8] = t->seq;
    return 9;
}

static inline bool rf_decode_trackpad(const uint8_t *buf, uint16_t len, rf_trackpad_t *t)
{
    if (len < 9 || rf_packet_type(buf, len) != PKT_TYPE_TRACKPAD) return false;
    t->ge0       = buf[1];
    t->ge1       = buf[2];
    t->n_fingers = buf[3];
    t->rel_x = (int16_t)((uint16_t)(buf[4] << 8) | buf[5]);
    t->rel_y = (int16_t)((uint16_t)(buf[6] << 8) | buf[7]);
    t->seq   = buf[8];
    return true;
}
```

- [ ] **Step 3 : Mettre à jour `test/test_rf_packet.c` (rouge attendu)**

Remplacer `test_rf_trackpad_roundtrip` (lignes ~56-78) par la version brute :

```c
static void test_rf_trackpad_roundtrip(void)
{
    uint8_t buf[16];
    rf_trackpad_t t = { .ge0 = 0x01, .ge1 = 0x02, .n_fingers = 2,
                        .rel_x = -300, .rel_y = 1234, .seq = 9 };
    uint16_t n = rf_encode_trackpad(buf, &t);
    TEST_ASSERT_EQ(n, 9, "trackpad encode length");

    rf_trackpad_t td;
    TEST_ASSERT(rf_decode_trackpad(buf, n, &td), "tp decode ok");
    TEST_ASSERT_EQ(td.ge0, 0x01, "tp ge0");
    TEST_ASSERT_EQ(td.ge1, 0x02, "tp ge1");
    TEST_ASSERT_EQ(td.n_fingers, 2, "tp nfingers");
    TEST_ASSERT_EQ(td.rel_x, -300, "tp rel_x BE signed");
    TEST_ASSERT_EQ(td.rel_y, 1234, "tp rel_y BE");
    TEST_ASSERT_EQ(td.seq, 9, "tp seq");
    /* short packet rejected */
    TEST_ASSERT(!rf_decode_trackpad(buf, 8, &td), "tp short rejected");
}
```

- [ ] **Step 4 : Créer `main/periph/trackpad/trackpad_map.c` (pur)**

Déplacer `clamp8()` et `trackpad_map()` hors de `trackpad.c`. Adapter `trackpad_map` à la nouvelle signature (sortie `trackpad_out_t`, param `cfg`, courbe d'accel). Garder **à l'identique** toute la logique gestuelle existante (drag, tap 1/2/3, scroll, gate) — lire l'implémentation actuelle dans `trackpad.c` et la transcrire ; SEULE la partie scaling curseur change. Le fichier commence par :

```c
#include "trackpad.h"

/* clamp signed 16-bit to int8 range */
static inline int8_t clamp8(int16_t v)
{
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

/* abs for int16 (no stdlib in pure unit) */
static inline int16_t iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }
```

Le bloc scaling curseur (anciennement `sx = rel_x * SENS_NUM/SENS_DEN`) devient la courbe d'accel :

```c
    } else if (!state->drag_active || n_fingers > 0) {
        /* Courbe d'accel : gain croît avec la vitesse du report, borné [base,gain_max]. */
        int32_t speed = iabs16(rel_x) > iabs16(rel_y) ? iabs16(rel_x) : iabs16(rel_y);
        int32_t gain  = (int32_t)cfg->base + (int32_t)cfg->accel * speed / TRACKPAD_ACCEL_DEN;
        if (gain < cfg->base)     gain = cfg->base;
        if (gain > cfg->gain_max) gain = cfg->gain_max;
        out->dx = clamp8((int16_t)((int32_t)rel_x * gain / 100));
        out->dy = clamp8((int16_t)((int32_t)rel_y * gain / 100));
    }
```

Le reste (init `out` à zéro en tête, drag, scroll via `IQS5XX_SCROLL_DIV`, tap state machine, gate `should_send`) est transcrit **sans changement de comportement** depuis `trackpad.c`. NB : `IQS5XX_SCROLL_DIV` et les masques `IQS5XX_GEST*` doivent être redéfinis en tête de `trackpad_map.c` (les copier depuis `trackpad.c`) puisque la partie pure ne voit plus les défines de la partie matérielle. Les boutons `MOUSE_BTN_*` aussi.

- [ ] **Step 5 : Élaguer `trackpad.c` + encoder le brut**

Dans `trackpad.c` : supprimer `clamp8()` et `trackpad_map()` (désormais dans `trackpad_map.c`) et les défines uniquement utilisés par eux si plus référencés. Remplacer l'étape 5-7 de `trackpad_task` (l'appel `trackpad_map` + encode 7 octets) par un encode **brut** (9 octets) sans mapping :

```c
        /* ── Step 5 : encode brut + transmit (le mapping se fait sur le dongle) ── */
        rf_trackpad_t tp = {
            .ge0 = ge0, .ge1 = ge1, .n_fingers = n_fingers,
            .rel_x = rel_x, .rel_y = rel_y, .seq = s_seq++,
        };
        uint8_t buf[9];
        rf_encode_trackpad(buf, &tp);
        half_spi_lock();
        rf_driver_send(&s_radio, buf, 9);
        half_spi_unlock();
```

Supprimer `s_tp_state` (la state machine vit maintenant sur le dongle) et l'ancienne gate `should_send` (la moitié envoie chaque report non nul ; conserver un petit gate : si `ge0==0 && ge1==0 && n_fingers==0 && rel_x==0 && rel_y==0` → `continue;` pour ne pas spammer au repos).

- [ ] **Step 6 : Dongle applique le mapping (`rf_rx_task.c`)**

Remplacer le bloc `PKT_TYPE_TRACKPAD` (lignes ~216-224) :

```c
        } else if (type == PKT_TYPE_TRACKPAD) {
            rf_trackpad_t tp;
            if (rf_decode_trackpad(buf, n, &tp)) {
                static trackpad_state_t s_tp_state;           /* gesture state (dongle-side) */
                static const trackpad_cfg_t s_tp_cfg = {      /* Lot 1: neutre = 1.0 linéaire */
                    .fmt = TRACKPAD_CFG_FMT, .base = 100, .accel = 0, .gain_max = 100,
                };
                trackpad_out_t out;
                if (trackpad_map(tp.ge0, tp.ge1, tp.n_fingers, tp.rel_x, tp.rel_y,
                                 &s_tp_cfg, &s_tp_state, &out)) {
                    hid_send_mouse(out.buttons, out.dx, out.dy, out.scroll_v);
                }
            }
        }
```

Ajouter `#include "trackpad.h"` en tête de `rf_rx_task.c` si absent.

- [ ] **Step 7 : CMake — compiler `trackpad_map.c` dongle + half ; tests**

Dans `main/CMakeLists.txt` : sous `if(CONFIG_KASE_HAS_TRACKPAD)` garder `periph/trackpad/trackpad.c` et ajouter `periph/trackpad/trackpad_map.c`. Ajouter un bloc pour le dongle :

```cmake
# Pure trackpad mapping (accel curve) — needed on the dongle (applies map) + half
if(CONFIG_KASE_HAS_RF_RX)
    list(APPEND srcs "periph/trackpad/trackpad_map.c")
endif()
```

(Veiller à ne pas lister `trackpad_map.c` deux fois si un board a RF_RX **et** TRACKPAD ; utiliser un garde : ne l'ajouter sous `HAS_TRACKPAD` que si `NOT CONFIG_KASE_HAS_RF_RX`, comme le pattern `rf_driver.c` existant.)

Dans `test/CMakeLists.txt` : remplacer `../main/periph/trackpad/trackpad.c` par `../main/periph/trackpad/trackpad_map.c` (le test host n'a besoin que de la partie pure).

- [ ] **Step 8 : Mettre à jour `test/test_trackpad_map.c`**

Adapter aux nouvelles signatures : `g_out` devient `trackpad_out_t`, chaque appel `trackpad_map(...)` reçoit `&g_cfg` (config neutre `{TRACKPAD_CFG_FMT,100,0,100}` → 1.0). Les assertions de gestes (tap/scroll/drag/gate) restent identiques. Ajouter en tête un helper :

```c
static const trackpad_cfg_t g_cfg = { .fmt = TRACKPAD_CFG_FMT, .base = 100, .accel = 0, .gain_max = 100 };
```

et passer `&g_cfg` à tous les appels. Vérifier qu'un mouvement 1 doigt `rel=(10,-4)` donne `dx=10, dy=-4` (gain 1.0 préservé).

- [ ] **Step 9 : Build host rouge→vert + commit**

```bash
rm -rf test/build && cmake -S test -B test/build >/dev/null && cmake --build test/build 2>&1 | tail
./test/build/test_runner | tail -5     # 0 failed
```
Puis build firmware (dongle + half + un clavier) :
```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_kase_dongle    -DBOARD=kase_dongle    -DSDKCONFIG=build_kase_dongle/sdkconfig    build 2>&1 | tail -3
idf.py -B build_kase_half_right -DBOARD=kase_half_right -DSDKCONFIG=build_kase_half_right/sdkconfig build 2>&1 | tail -3
idf.py -B build_kase_v2        -DBOARD=kase_v2        -DSDKCONFIG=build_kase_v2/sdkconfig        build 2>&1 | tail -3
```
Commit (pas de Co-Authored-By ; `--no-verify`) :
```bash
git add main/comm/rf/rf_packet.h main/periph/trackpad/ main/comm/rf/rf_rx_task.c main/CMakeLists.txt test/
git commit --no-verify -m "refactor(trackpad): raw NRF payload + map on dongle (approach B, behaviour-preserving)"
```

---

## Task 2 : Courbe d'accélération (tuning + tests)

But : activer le vrai gain de confort. La formule existe (Task 1) ; on change les défauts et on couvre la courbe par des tests.

**Files:** Modify `main/comm/rf/rf_rx_task.c` (défaut cfg), `test/test_trackpad_map.c`.

- [ ] **Step 1 : Test de la courbe (rouge)**

Ajouter dans `test/test_trackpad_map.c` une fonction `test_trackpad_accel_curve` et l'appeler dans `test_trackpad_map()` (ou l'exposer ; suivre le pattern du fichier). Config de tuning :

```c
static const trackpad_cfg_t g_accel = { .fmt = TRACKPAD_CFG_FMT, .base = 90, .accel = 40, .gain_max = 300 };

static void test_trackpad_accel_curve(void)
{
    trackpad_state_t st = {0};
    trackpad_out_t o;

    /* lent : rel=(2,0) → gain≈base(0.90) → dx ≈ 2*90/100 = 1 (precision) */
    trackpad_map(0,0,1, 2,0, &g_accel, &st, &o);
    TEST_ASSERT_EQ(o.dx, 1, "slow ~base gain (precision)");

    /* rapide : rel=(60,0) → gain=90+40*60/100=90+24=114? non, /DEN — verifier */
    /* gain = base + accel*speed/ACCEL_DEN = 90 + 40*60/100 = 90+24 = 114 (1.14x) */
    /* dx attendu = clamp8(60*114/100)=clamp8(68)=68 */
    trackpad_map(0,0,1, 60,0, &g_accel, &st, &o);
    TEST_ASSERT_EQ(o.dx, 68, "fast → amplified by curve");

    /* tres rapide : rel=(200,0) → gain=90+40*200/100=90+80=170 mais clamp gain_max=300 → 170<300 */
    /* dx=clamp8(200*170/100)=clamp8(340)=127 (clamp8) */
    trackpad_map(0,0,1, 200,0, &g_accel, &st, &o);
    TEST_ASSERT_EQ(o.dx, 127, "very fast → clamp8 ceiling");

    /* monotonie : gain(slow) <= gain(fast) à rel identique scalé */
    trackpad_state_t s2 = {0}; trackpad_out_t a, b;
    trackpad_map(0,0,1, 5,0,  &g_accel, &s2, &a);
    trackpad_state_t s3 = {0};
    trackpad_map(0,0,1, 50,0, &g_accel, &s3, &b);
    TEST_ASSERT((int)b.dx * 10 >= (int)a.dx * 100, "gain monotone croissant");
}
```

NB IMPORTANT pour l'implémenteur : **recalculer les valeurs attendues exactes** à partir de la formule `gain = clamp(base + accel*speed/100, base, gain_max)` puis `dx = clamp8(rel*gain/100)` et ajuster les littéraux ci-dessus s'ils diffèrent (les commentaires montrent le calcul ; le test doit refléter la formule réelle, pas l'inverse).

- [ ] **Step 2 : Lancer le test → rouge** (les défauts du dongle sont encore neutres, mais ce test passe sa propre cfg `g_accel` ; il valide la formule). `cmake --build test/build && ./test/build/test_runner` → la suite accel doit échouer si la formule n'est pas correcte, sinon passer.

- [ ] **Step 3 : Changer les défauts du dongle** dans `rf_rx_task.c` (le `s_tp_cfg` statique du Step 1.6) :

```c
                static const trackpad_cfg_t s_tp_cfg = {
                    .fmt = TRACKPAD_CFG_FMT, .base = 90, .accel = 40, .gain_max = 300,
                };
```
(Ces défauts seront remplacés par la valeur NVS en Task 3.)

- [ ] **Step 4 : Vert + build + commit**

```bash
cmake --build test/build >/dev/null 2>&1 && ./test/build/test_runner | tail -5
source ~/esp/esp-idf/export.sh && idf.py -B build_kase_dongle -DBOARD=kase_dongle -DSDKCONFIG=build_kase_dongle/sdkconfig build 2>&1 | tail -3
git add main/comm/rf/rf_rx_task.c test/test_trackpad_map.c
git commit --no-verify -m "feat(trackpad): acceleration curve (base/accel/gain_max) + host tests"
```

---

## Task 3 : Config NVS dongle + CDC 0xB8/0xB9 + doc/exemples

But : régler `base/accel/gain_max` à chaud depuis le soft, persistant.

**Files:** Create `main/periph/trackpad/trackpad_cfg.c`; Modify `trackpad.h`, `main/comm/cdc/cdc_binary_protocol.h`, `main/comm/cdc/cdc_binary_cmds.c`, `main/comm/rf/rf_rx_task.c`, `main/CMakeLists.txt`, `docs/CDC_BINARY_PROTOCOL.md`, `scripts/test_binary_protocol.py`; Create `test/test_trackpad_cfg.c`; Modify `test/CMakeLists.txt`, `test/test_main.c`.

- [ ] **Step 1 : encode/decode cfg purs (test rouge)**

Déclarer dans `trackpad.h` :
```c
#define TRACKPAD_CFG_SIZE 7   /* fmt(1) + base(2) + accel(2) + gain_max(2), LE */
uint16_t trackpad_cfg_encode(uint8_t *buf, const trackpad_cfg_t *c);   /* → 7 */
bool     trackpad_cfg_decode(const uint8_t *buf, uint16_t len, trackpad_cfg_t *c); /* len>=6 (sans fmt) ou 7 */
```

Créer `test/test_trackpad_cfg.c` :
```c
#include "test_framework.h"
#include "../main/periph/trackpad/trackpad.h"

void test_trackpad_cfg(void)
{
    printf("\n--- trackpad_cfg ---\n");
    trackpad_cfg_t c = { .fmt = TRACKPAD_CFG_FMT, .base = 90, .accel = 40, .gain_max = 300 };
    uint8_t b[TRACKPAD_CFG_SIZE];
    uint16_t n = trackpad_cfg_encode(b, &c);
    TEST_ASSERT_EQ(n, TRACKPAD_CFG_SIZE, "cfg encode size 7");
    TEST_ASSERT_EQ(b[0], TRACKPAD_CFG_FMT, "fmt");
    TEST_ASSERT_EQ(b[1], 90,  "base lo"); TEST_ASSERT_EQ(b[2], 0, "base hi");
    TEST_ASSERT_EQ(b[3], 40,  "accel lo"); TEST_ASSERT_EQ(b[4], 0, "accel hi");
    TEST_ASSERT_EQ(b[5], 44,  "gmax lo (300=0x012C)"); TEST_ASSERT_EQ(b[6], 1, "gmax hi");

    trackpad_cfg_t d;
    TEST_ASSERT(trackpad_cfg_decode(b, n, &d), "decode ok");
    TEST_ASSERT_EQ(d.base, 90, "rt base"); TEST_ASSERT_EQ(d.accel, 40, "rt accel");
    TEST_ASSERT_EQ(d.gain_max, 300, "rt gain_max");

    /* SET payload sans fmt (6 octets) : base,accel,gain_max LE */
    uint8_t setp[6] = { 100,0, 50,0, 0x2C,1 };
    TEST_ASSERT(trackpad_cfg_decode(setp, 6, &d), "decode 6B (no fmt)");
    TEST_ASSERT_EQ(d.base, 100, "6B base"); TEST_ASSERT_EQ(d.gain_max, 300, "6B gmax");
}
```
Câbler dans `test/test_main.c` (`extern` + appel) et `test/CMakeLists.txt` (`test_trackpad_cfg.c`). Build → rouge (link).

- [ ] **Step 2 : implémenter encode/decode dans `trackpad_map.c`**

```c
static void put_u16le(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static uint16_t get_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }

uint16_t trackpad_cfg_encode(uint8_t *buf, const trackpad_cfg_t *c)
{
    buf[0] = c->fmt;
    put_u16le(&buf[1], c->base);
    put_u16le(&buf[3], c->accel);
    put_u16le(&buf[5], c->gain_max);
    return TRACKPAD_CFG_SIZE;
}

/* Accepte 7 octets (avec fmt en tête) OU 6 octets (payload SET sans fmt). */
bool trackpad_cfg_decode(const uint8_t *buf, uint16_t len, trackpad_cfg_t *c)
{
    const uint8_t *p;
    if (len >= TRACKPAD_CFG_SIZE) { c->fmt = buf[0]; p = &buf[1]; }
    else if (len >= 6)            { c->fmt = TRACKPAD_CFG_FMT; p = buf; }
    else return false;
    c->base     = get_u16le(&p[0]);
    c->accel    = get_u16le(&p[2]);
    c->gain_max = get_u16le(&p[4]);
    return true;
}
```
Build host → vert. Commit intermédiaire optionnel.

- [ ] **Step 3 : config active dongle + NVS (`trackpad_cfg.c`)**

Déclarer dans `trackpad.h` :
```c
void                 trackpad_cfg_load(void);                 /* boot: NVS → cfg active (ou défauts) */
const trackpad_cfg_t *trackpad_cfg_active(void);             /* pointeur vers la cfg courante */
bool                 trackpad_cfg_apply_and_save(const trackpad_cfg_t *c); /* valide, applique, NVS */
```
Créer `main/periph/trackpad/trackpad_cfg.c` (compilé dongle uniquement). Bornes : `base ≤ gain_max`, `gain_max ≤ 1000`, `accel ≤ 1000`. Défauts `{TRACKPAD_CFG_FMT,90,40,300}`. Lire `main/sys/nvs_utils.h` pour la signature exacte de `nvs_save_blob_with_total`/`nvs_load_blob_with_total` et l'utiliser (namespace `"storage"` = `STORAGE_NAMESPACE`, clé `"tp_cfg"`).

```c
#include "trackpad.h"
#include "nvs_utils.h"
#include "esp_log.h"
#include <string.h>

#define TP_NS   "storage"
#define TP_KEY  "tp_cfg"
static const char *TAG = "tp_cfg";

static trackpad_cfg_t s_cfg = { .fmt = TRACKPAD_CFG_FMT, .base = 90, .accel = 40, .gain_max = 300 };

static bool cfg_valid(const trackpad_cfg_t *c)
{
    return c->gain_max <= 1000 && c->accel <= 1000 &&
           c->base <= c->gain_max && c->base >= 1;
}

void trackpad_cfg_load(void)
{
    trackpad_cfg_t tmp;
    /* Adapter l'appel à la vraie signature de nvs_load_blob_with_total (lire le .h). */
    if (nvs_load_blob_with_total(TP_NS, TP_KEY, &tmp, sizeof(tmp)) == ESP_OK
        && tmp.fmt == TRACKPAD_CFG_FMT && cfg_valid(&tmp)) {
        s_cfg = tmp;
        ESP_LOGI(TAG, "loaded base=%u accel=%u gmax=%u", tmp.base, tmp.accel, tmp.gain_max);
    } else {
        ESP_LOGI(TAG, "defaults base=%u accel=%u gmax=%u", s_cfg.base, s_cfg.accel, s_cfg.gain_max);
    }
}

const trackpad_cfg_t *trackpad_cfg_active(void) { return &s_cfg; }

bool trackpad_cfg_apply_and_save(const trackpad_cfg_t *c)
{
    if (!cfg_valid(c)) return false;
    s_cfg = *c;
    s_cfg.fmt = TRACKPAD_CFG_FMT;
    nvs_save_blob_with_total(TP_NS, TP_KEY, &s_cfg, sizeof(s_cfg));  /* adapter la signature */
    return true;
}
```
Appeler `trackpad_cfg_load()` au boot du dongle (dans `main.c` sous `#if CONFIG_KASE_HAS_RF_RX`, près de `espnow_info_init()` / l'init dongle). Dans `rf_rx_task.c`, remplacer le `s_tp_cfg` statique par `trackpad_cfg_active()` :
```c
                trackpad_out_t out;
                if (trackpad_map(tp.ge0, tp.ge1, tp.n_fingers, tp.rel_x, tp.rel_y,
                                 trackpad_cfg_active(), &s_tp_state, &out)) {
                    hid_send_mouse(out.buttons, out.dx, out.dy, out.scroll_v);
                }
```
CMake : ajouter `periph/trackpad/trackpad_cfg.c` sous `if(CONFIG_KASE_HAS_RF_RX)`.

- [ ] **Step 4 : IDs CDC** dans `cdc_binary_protocol.h`, après `KS_CMD_MONITOR = 0xB7,` :
```c
    KS_CMD_TRACKPAD_GET     = 0xB8,  /* get trackpad accel cfg (trackpad_cfg_t, 7B) */
    KS_CMD_TRACKPAD_SET     = 0xB9,  /* set trackpad accel cfg (payload 6B: base,accel,gain_max LE) */
```

- [ ] **Step 5 : handlers CDC** dans `cdc_binary_cmds.c` (inclure `trackpad.h`). Placer près des autres handlers ; gater sous `#if CONFIG_KASE_HAS_RF_RX` car la cfg vit sur le dongle. **Hors dongle**, ne pas enregistrer ces commandes (ou répondre `KS_STATUS_ERR_*`). Approche : définir les handlers et l'entrée table sous `#if CONFIG_KASE_HAS_RF_RX`.

```c
#if CONFIG_KASE_HAS_RF_RX
static void bin_cmd_trackpad_get(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    (void)p; (void)l;
    uint8_t buf[TRACKPAD_CFG_SIZE];
    trackpad_cfg_encode(buf, trackpad_cfg_active());
    ks_respond(cmd, KS_STATUS_OK, buf, TRACKPAD_CFG_SIZE);
}

static void bin_cmd_trackpad_set(uint8_t cmd, const uint8_t *p, uint16_t l)
{
    if (l < 6) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    trackpad_cfg_t c;
    if (!trackpad_cfg_decode(p, l, &c)) { ks_respond_err(cmd, KS_STATUS_ERR_INVALID); return; }
    if (!trackpad_cfg_apply_and_save(&c)) { ks_respond_err(cmd, KS_STATUS_ERR_RANGE); return; }
    uint8_t buf[TRACKPAD_CFG_SIZE];
    trackpad_cfg_encode(buf, trackpad_cfg_active());
    ks_respond(cmd, KS_STATUS_OK, buf, TRACKPAD_CFG_SIZE);   /* echo applied */
}
#endif
```
Entrée table (dans `bin_cmd_table[]`), sous le même garde :
```c
#if CONFIG_KASE_HAS_RF_RX
    { KS_CMD_TRACKPAD_GET, bin_cmd_trackpad_get },
    { KS_CMD_TRACKPAD_SET, bin_cmd_trackpad_set },
#endif
```
Vérifier que `ks_respond_err` et les constantes `KS_STATUS_ERR_INVALID/ERR_RANGE` existent (utilisées par `bin_cmd_setkey`/`setlayer`). Vérifier que `trackpad.h` est incluable depuis `cdc_binary_cmds.c` (include dir `periph/trackpad` est déjà dans `INCLUDE_DIRS`).

- [ ] **Step 6 : build + host vert**
```bash
./scripts/check.sh --host-only 2>&1 | tail -3
source ~/esp/esp-idf/export.sh
idf.py -B build_kase_dongle -DBOARD=kase_dongle -DSDKCONFIG=build_kase_dongle/sdkconfig build 2>&1 | tail -3
idf.py -B build_kase_v2     -DBOARD=kase_v2     -DSDKCONFIG=build_kase_v2/sdkconfig     build 2>&1 | tail -3
```

- [ ] **Step 7 : doc + exemple Python**

Dans `docs/CDC_BINARY_PROTOCOL.md` (section Diagnostics, après MONITOR 0xB7) : documenter `TRACKPAD_GET (0xB8)` (réponse 7 o : fmt u8, base u16 LE, accel u16 LE, gain_max u16 LE ; gains en centièmes) et `TRACKPAD_SET (0xB9)` (payload 6 o : base, accel, gain_max u16 LE ; bornes `base≤gain_max≤1000`, `accel≤1000` ; réponse = écho de la cfg appliquée ; `KS_STATUS_ERR_RANGE` si hors bornes). Style sans diacritiques comme le reste du doc.

Dans `scripts/test_binary_protocol.py`, ajouter `test_trackpad(t)` (lecture seule pour le GET ; le SET est optionnel/commenté pour ne pas modifier la config en test) :
```python
def test_trackpad(t):
    print("\n=== Trackpad accel cfg (0xB8) ===")
    r = t.expect("TRACKPAD_GET", 0xB8, min_len=7, max_len=7)
    if r and len(r["payload"]) == 7:
        import struct
        p = r["payload"]
        fmt = p[0]
        base, accel, gmax = struct.unpack_from("<HHH", p, 1)
        print(f"         fmt={fmt} base={base/100:.2f}x accel={accel} gain_max={gmax/100:.2f}x")
```
L'appeler dans `main()` (après `test_monitor`). Adapter à la vraie API `t.expect` (déjà utilisée par `test_monitor`).

- [ ] **Step 8 : snippet C#** dans la doc (section TRACKPAD) : classe `TrackpadCfg {byte Fmt; ushort Base, Accel, GainMax;}` + `Parse(byte[] p)` (BitConverter.ToUInt16 LE aux offsets 1/3/5) + `byte[] BuildSet(ushort base_, ushort accel, ushort gmax)` (6 o LE) pour la commande 0xB9.

- [ ] **Step 9 : commit**
```bash
git add main/ test/ docs/CDC_BINARY_PROTOCOL.md scripts/test_binary_protocol.py
git commit --no-verify -m "feat(trackpad): runtime accel cfg via CDC 0xB8/0xB9 + NVS + doc/clients"
```

---

## Self-Review

**Couverture spec :** payload brut + map sur dongle → T1 ✓ ; courbe d'accel pure host-testée → T1 (formule) + T2 (tuning/tests) ✓ ; `trackpad_cfg_t` + NVS dongle → T3 ✓ ; CDC 0xB8/0xB9 get/set + bornes → T3 ✓ ; doc + Python/C# → T3 ✓ ; refactor pure/half/dongle + CMake → T1 ✓ ; gestes non régressés → T1.8/T2 (tests existants conservés) ✓ ; 6 boards compilent → T3.6 + check.sh ✓ ; reflash conjoint moitié+dongle (format wire) → noté T1.

**Placeholders :** les "adapter la signature de nvs_*" et "recalculer les valeurs attendues" pointent des fonctions/valeurs EXISTANTES à confirmer en lisant le code (pas des TODO de logique). Le code pur (courbe, encode/decode) est complet.

**Cohérence des types :** `rf_trackpad_t` (brut) ↔ encode/decode 9 o (T1) ; `trackpad_out_t` sortie de `trackpad_map` (T1) utilisé par le dongle (T1.6) ; `trackpad_cfg_t` {fmt,base,accel,gain_max} identique header (T1.1) / curve (T1.4) / encode (T3.2) / handlers (T3.5) ; `TRACKPAD_CFG_SIZE=7`, SET=6 o ; `trackpad_cfg_active()`/`apply_and_save()`/`load()` signatures cohérentes T3.3↔T3.5.

## Notes
- Changement de format wire RF → moitié **et** dongle reflashés ensemble (bump version à la release).
- Défauts de courbe `base=90, accel=40, gain_max=300` = point de départ ; à tuner au banc via 0xB9 (c'est tout l'intérêt du runtime).
- `trackpad_map.c` pur : n'inclure que `trackpad.h` (qui inclut `<stdint.h>`/`<stdbool.h>` + `rf_packet.h` n'est plus nécessaire pour la sortie puisque `trackpad_out_t` est dans `trackpad.h`).
