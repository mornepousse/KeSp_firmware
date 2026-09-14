# Écrans Sharp memory-LCD des moitiés — Plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Faire vivre l'écran Sharp LS011B7DH03 (portrait 68 × 160, 1 bit) de chaque moitié : bandeau (route, dongle, sa batterie), centre (gauche = couche statique ; droite = logo Niphargus centré), pied (batterie de l'autre moitié), image gelée en veille — sans jamais perturber la radio qui partage le bus SPI.

**Architecture:** Un pilote panneau minimal (`memlcd_panel`, write-line LSB-first, CS actif haut, VCOM) qui n'accède au bus que sous le **verrou radio** ; un backend `display_backend_t` LVGL 8 (`memlcd_backend`) qui redessine à la demande depuis un modèle pur (`memlcd_model`) et pousse les lignes modifiées ; une trame `PKT_TYPE_DISPLAY` que le dongle glisse dans l'ACK payload pour descendre couche statique + batterie de l'autre moitié. Le logo est une image LVGL 1 bit **générée par script** depuis `Niphargus/images/niphargus_logo.svg`.

**Tech Stack:** C, ESP-IDF 5.5 (`driver/spi_master`), LVGL 8 (`esp_lvgl_port` déjà lié), nRF24 ACK payload, Inkscape (génération du logo), tests host CMake.

**Spec:** `docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md`

## Global Constraints

- Panneau **portrait** : LVGL travaille en **68 × 160** (tampon 9 octets par rangée). **Tranché à la datasheet le 2026-09-14** (Task 3) : le panneau physique est 68 lignes × 160 px (20 octets) ; `memlcd_fb_to_panel` (memlcd_model.h, testée) transpose le portrait en lignes physiques, `memlcd_panel_show(fb)` l'écrit. Le mot de commande part brut (M0 = bit 7), seule l'adresse passe par rev8.
- SPI : mode 0, **1 MHz**, **LSB-first émulé** (table d'inversion de bits pour les octets de commande et d'adresse ; les octets de pixels sont écrits de sorte que le pixel 0 soit le bit de poids faible), `spics_io_num = -1`, **CS actif HAUT** piloté à la main (`BOARD_LCD_CS_GPIO` = 14 des deux côtés), **tenu BAS dès le boot** des deux moitiés même sans écran.
- Bus partagé avec le nRF24 : **toute** transaction écran s'exécute sous `rf_bus_lock(5 ms)` / `rf_bus_unlock()` exposés par le propriétaire de la puce (gauche `kbd_relay`, droite `half_link`) ; si le verrou n'est pas obtenu, le rafraîchissement **cède** et réessaie au tick suivant. Jamais de transaction écran depuis un contexte ISR.
- Énergie : `sleep()` = arrêt des timers écran, **image gelée** (pas d'effacement) ; `wake()` = reprise + redessin complet. Aucun timer écran ne doit exister pendant le light sleep.
- Contenu : couche **statique** = `current_layout` (TO / base), jamais un MO/LT tenu.
- Trame DISPLAY : `PKT_TYPE_DISPLAY = 0xA`, ≤ 6 o, servie dans l'ACK **seulement si `!dongle_sync_active()`** (la sync keymap reste prioritaire) et seulement après une trame de la moitié destinataire.
- Logo : `main/display/assets/img_niphargus_60.c`, `LV_IMG_CF_ALPHA_1BIT`, **60 px de large, centré** dans la zone centrale ; généré par `scripts/gen_logo_memlcd.sh`, jamais édité à la main.
- Kconfig : `KASE_DISPLAY_MEMLCD` (bool, default n) ; `KASE_HAS_DISPLAY` passe à `y` pour les deux moitiés **uniquement** via ce symbole. Les 7 boards par défaut restent verts ; la droite (`NIPHAR_SLAVE`) gagne son propre démarrage écran (aujourd'hui tout est sous `DEVICE_ROLE_KEYBOARD`).
- Tout nouveau code pur : test host rouge avant, mordant à la mutation. Toute source touchée adossée à `COMPORTEMENTS.md`.

---

### Task 1 : Logique pure — inversion de bits, coupure du nom, modèle, trame DISPLAY

**Files:**
- Create: `main/display/memlcd/memlcd_model.h`
- Modify: `main/comm/rf/rf_packet.h` (trame DISPLAY)
- Test: `test/test_memlcd_model.c` (create), `test/test_rf_packet.c` (étendre), `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces:**
- Produces (`memlcd_model.h`) :
  - `uint8_t memlcd_rev8(uint8_t b);` — inversion des 8 bits (LSB-first).
  - `#define MEMLCD_W 68`, `#define MEMLCD_H 160`, `#define MEMLCD_LINE_BYTES 9`
  - `#define MEMLCD_NOM_COLS 4`, `#define MEMLCD_NOM_LIGNES 3`
  - `uint8_t memlcd_couper_nom(const char *nom, char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_COLS + 1]);` — remplit jusqu'à 3 lignes de 4 caractères, renvoie le nombre de lignes ; si le nom dépasse 12 caractères, la 3e ligne finit par `…` (UTF-8, 3 octets — on réserve : la 3e ligne fait 3 caractères + `…`).
  - `typedef struct { uint8_t route_rf, dongle_vu; uint8_t batt_local_dv, batt_local_chg; uint8_t batt_autre_dv, batt_autre_chg; uint8_t couche; char nom[16]; uint8_t is_left; } memlcd_model_t;`
  - `bool memlcd_model_diff(const memlcd_model_t *a, const memlcd_model_t *b);` — true si un champ affiché diffère.
- Produces (`rf_packet.h`) :
  - `#define PKT_TYPE_DISPLAY 0xA`, `#define PKT_DISPLAY_FLAG_TO_RIGHT 0x1`
  - `typedef struct { uint8_t to_right; uint8_t couche; uint8_t batt_autre_dv; uint8_t batt_autre_chg; uint8_t dongle_ok; } rf_display_t;`
  - `uint16_t rf_encode_display(uint8_t *buf, const rf_display_t *d);` → 4 o : `[type<<4 | flags][couche][batt_autre_dv][(chg & 0x3) | (dongle_ok ? 0x4 : 0)]`
  - `bool rf_decode_display(const uint8_t *buf, uint16_t len, rf_display_t *o);`

- [ ] **Step 1: Write the failing tests** — `test/test_memlcd_model.c` :

```c
#include "test_framework.h"
#include "../main/display/memlcd/memlcd_model.h"
#include <string.h>

static void test_rev8(void)
{
    TEST_ASSERT_EQ(memlcd_rev8(0x01), 0x80, "bit0 → bit7");
    TEST_ASSERT_EQ(memlcd_rev8(0x80), 0x01, "bit7 → bit0");
    TEST_ASSERT_EQ(memlcd_rev8(0xA5), 0xA5, "0xA5 est un palindrome binaire");
    TEST_ASSERT_EQ(memlcd_rev8(0x0F), 0xF0, "nibbles échangés bit à bit");
    for (unsigned b = 0; b < 256; b++)
        TEST_ASSERT_EQ(memlcd_rev8(memlcd_rev8((uint8_t)b)), (uint8_t)b, "involution");
}

static void test_couper_nom(void)
{
    char l[MEMLCD_NOM_LIGNES][MEMLCD_NOM_COLS + 1];
    TEST_ASSERT_EQ(memlcd_couper_nom("DVORAK", l), 2, "6 lettres → 2 lignes");
    TEST_ASSERT(strcmp(l[0], "DVOR") == 0 && strcmp(l[1], "AK") == 0, "DVOR / AK");
    TEST_ASSERT_EQ(memlcd_couper_nom("NAV", l), 1, "3 lettres → 1 ligne");
    TEST_ASSERT(strcmp(l[0], "NAV") == 0, "NAV");
    TEST_ASSERT_EQ(memlcd_couper_nom("", l), 1, "vide → 1 ligne vide (jamais 0)");
    TEST_ASSERT(l[0][0] == '\0', "ligne vide");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKL", l), 3, "12 lettres → 3 lignes pleines");
    TEST_ASSERT(strcmp(l[2], "IJKL") == 0, "3e ligne pleine sans …");
    TEST_ASSERT_EQ(memlcd_couper_nom("ABCDEFGHIJKLMNOP", l), 3, "16 lettres → 3 lignes, tronqué");
    TEST_ASSERT(strcmp(l[2], "IJK\xE2\x80\xA6") == 0, "3e ligne = 3 lettres + … (UTF-8)");
}

static void test_model_diff(void)
{
    memlcd_model_t a = { .route_rf = 1, .dongle_vu = 1, .batt_local_dv = 40, .couche = 1, .nom = "DVORAK", .is_left = 1 };
    memlcd_model_t b = a;
    TEST_ASSERT(!memlcd_model_diff(&a, &b), "identiques → pas de redessin");
    b.batt_local_dv = 39;
    TEST_ASSERT(memlcd_model_diff(&a, &b), "tension change → redessin");
    b = a; b.couche = 2;
    TEST_ASSERT(memlcd_model_diff(&a, &b), "couche change → redessin");
    b = a; strcpy(b.nom, "NAV");
    TEST_ASSERT(memlcd_model_diff(&a, &b), "nom change → redessin");
}

void test_memlcd_model(void)
{
    TEST_SUITE("Écran memory-LCD : logique pure");
    test_rev8();
    test_couper_nom();
    test_model_diff();
}
```
Et dans `test/test_rf_packet.c` (suite existante) :
```c
static void test_rf_display_roundtrip(void)
{
    rf_display_t in = { .to_right = 1, .couche = 3, .batt_autre_dv = 41, .batt_autre_chg = 2, .dongle_ok = 1 };
    uint8_t buf[8];
    uint16_t n = rf_encode_display(buf, &in);
    TEST_ASSERT_EQ(n, 4, "DISPLAY = 4 octets (tient dans un ACK)");
    TEST_ASSERT_EQ(rf_packet_type(buf, n), PKT_TYPE_DISPLAY, "type DISPLAY");
    rf_display_t out = {0};
    TEST_ASSERT(rf_decode_display(buf, n, &out), "decode");
    TEST_ASSERT(out.to_right == 1 && out.couche == 3 && out.batt_autre_dv == 41 &&
                out.batt_autre_chg == 2 && out.dongle_ok == 1, "champs round-trip");
    TEST_ASSERT(!rf_decode_display(buf, 3, &out), "rejette trop court");
    uint8_t bad[4]; memcpy(bad, buf, 4); bad[0] = (PKT_TYPE_STATUS << 4);
    TEST_ASSERT(!rf_decode_display(bad, 4, &out), "rejette autre type");
}
```
(+ appel dans `test_rf_packet()`). Enregistrer `test_memlcd_model.c` dans `test/CMakeLists.txt` et `test/test_main.c`.

- [ ] **Step 2: Run to verify failure** — `cd test && cmake --build build -j` → erreur (header/fonctions absents).

- [ ] **Step 3: Implement** — `main/display/memlcd/memlcd_model.h` :
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Écran Sharp memory-LCD des moitiés — logique pure (test/test_memlcd_model.c).
 * Panneau LS011B7DH03 monté en PORTRAIT : 68 px de large × 160 de haut.
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
#define MEMLCD_W          68
#define MEMLCD_H          160
#define MEMLCD_LINE_BYTES 9     /* ceil(68/8) — 1 bit par pixel */

/* Le panneau lit LSB-first ; l'ESP32 émet MSB-first. On inverse les bits des
 * octets de commande et d'adresse (les pixels sont déjà posés bit 0 = pixel 0). */
static inline uint8_t memlcd_rev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* Nom de couche sur 68 px en Montserrat 14 : 4 caractères par ligne, 3 lignes,
 * puis « … » (l'utilisateur a préféré des lignes lisibles à un texte tourné). */
#define MEMLCD_NOM_COLS   4
#define MEMLCD_NOM_LIGNES 3

static inline uint8_t memlcd_couper_nom(const char *nom, char lignes[MEMLCD_NOM_LIGNES][MEMLCD_NOM_COLS + 1])
{
    size_t n = nom ? strlen(nom) : 0;
    for (int i = 0; i < MEMLCD_NOM_LIGNES; i++) lignes[i][0] = '\0';
    if (n == 0) return 1;
    size_t max = (size_t)MEMLCD_NOM_COLS * MEMLCD_NOM_LIGNES;
    uint8_t nl = 0;
    for (size_t off = 0; off < n && nl < MEMLCD_NOM_LIGNES; off += MEMLCD_NOM_COLS, nl++) {
        size_t len = n - off; if (len > MEMLCD_NOM_COLS) len = MEMLCD_NOM_COLS;
        memcpy(lignes[nl], nom + off, len); lignes[nl][len] = '\0';
    }
    if (n > max) {   /* tronqué : 3 lettres + … sur la dernière ligne */
        memcpy(lignes[MEMLCD_NOM_LIGNES - 1] + 3, "\xE2\x80\xA6", 4);
    }
    return nl;
}

typedef struct {
    uint8_t route_rf, dongle_vu;
    uint8_t batt_local_dv, batt_local_chg;
    uint8_t batt_autre_dv, batt_autre_chg;
    uint8_t couche;
    char    nom[16];
    uint8_t is_left;
} memlcd_model_t;

static inline bool memlcd_model_diff(const memlcd_model_t *a, const memlcd_model_t *b)
{
    return a->route_rf != b->route_rf || a->dongle_vu != b->dongle_vu ||
           a->batt_local_dv != b->batt_local_dv || a->batt_local_chg != b->batt_local_chg ||
           a->batt_autre_dv != b->batt_autre_dv || a->batt_autre_chg != b->batt_autre_chg ||
           a->couche != b->couche || strcmp(a->nom, b->nom) != 0;
}
```
⚠ `lignes[i]` fait `MEMLCD_NOM_COLS + 1` = 5 octets : « 3 lettres + … (3 octets) + NUL » = 7 octets — **agrandir** le tableau à `MEMLCD_NOM_COLS + 4` dans la signature (et le test) pour loger l'ellipse UTF-8. Le test rouge le révélera : ajuster la signature à `[MEMLCD_NOM_LIGNES][MEMLCD_NOM_COLS + 4]` partout.

`rf_packet.h` (à la suite des trames de sync) :
```c
/* Trame d'affichage dongle→moitié, glissée dans l'ACK payload (spec écrans §4).
 * 4 octets : [type<<4 | flags][couche statique][batt de l'AUTRE moitié dV]
 * [chg & 0x3 | dongle_ok<<2]. flags bit0 = destinée à la droite. */
#define PKT_TYPE_DISPLAY          0xA
#define PKT_DISPLAY_FLAG_TO_RIGHT 0x1
typedef struct { uint8_t to_right, couche, batt_autre_dv, batt_autre_chg, dongle_ok; } rf_display_t;
static inline uint16_t rf_encode_display(uint8_t *buf, const rf_display_t *d)
{
    if (!buf || !d) return 0;
    buf[0] = (uint8_t)((PKT_TYPE_DISPLAY << 4) | (d->to_right ? PKT_DISPLAY_FLAG_TO_RIGHT : 0));
    buf[1] = d->couche; buf[2] = d->batt_autre_dv;
    buf[3] = (uint8_t)((d->batt_autre_chg & 0x3) | (d->dongle_ok ? 0x4 : 0));
    return 4;
}
static inline bool rf_decode_display(const uint8_t *buf, uint16_t len, rf_display_t *o)
{
    if (!buf || !o || len < 4 || rf_packet_type(buf, len) != PKT_TYPE_DISPLAY) return false;
    o->to_right = (buf[0] & PKT_DISPLAY_FLAG_TO_RIGHT) != 0;
    o->couche = buf[1]; o->batt_autre_dv = buf[2];
    o->batt_autre_chg = buf[3] & 0x3; o->dongle_ok = (buf[3] & 0x4) != 0;
    return true;
}
```

- [ ] **Step 4: Run green + mutation** (rev8 : retirer la 3e permutation → involution rouge ; couper_nom : oublier l'ellipse → rouge). Fichiers neufs : sauvegarde scratchpad pour restaurer.

- [ ] **Step 5: Commit**
```bash
git add main/display/memlcd/memlcd_model.h main/comm/rf/rf_packet.h test/test_memlcd_model.c test/test_rf_packet.c test/CMakeLists.txt test/test_main.c .tripwire-testcount
git commit -m "feat(memlcd): logique pure — rev8 LSB-first, coupure du nom, modèle, trame DISPLAY (testé)"
```

---

### Task 2 : Verrou du bus partagé exposé par le propriétaire de la radio

**Files:**
- Modify: `main/comm/rf/kbd_relay_tx.c`/`.h` (gauche), `main/comm/rf/half_link.c`/`.h` (droite)
- Create: `main/comm/rf/rf_bus.h` (déclarations communes)

**Interfaces:**
- Produces: `bool rf_bus_lock(uint32_t timeout_ms);` / `void rf_bus_unlock(void);` — implémentés **une fois par moitié** : gauche → `s_tx_mutex` de kbd_relay ; droite → `s_tx_radio_mux` de half_link. `rf_bus_host()` renvoie `spi_host_device_t` du bus (depuis `s_radio.cfg.spi_host` ou `BOARD_NRF_SPI_HOST`).

- [ ] **Step 1:** `rf_bus.h` :
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
/* Le bus SPI d'une moitié appartient à la RADIO (« une puce, un propriétaire »,
 * CLAUDE.md). Un autre esclave (l'écran) n'y touche que sous le verrou du
 * propriétaire — jamais pendant un envoi nRF. Implémenté par kbd_relay (gauche)
 * ou half_link (droite). */
bool rf_bus_lock(uint32_t timeout_ms);
void rf_bus_unlock(void);
spi_host_device_t rf_bus_host(void);
```
- [ ] **Step 2:** kbd_relay_tx.c (sous `CONFIG_KASE_KBD_WIRELESS && !CONFIG_KASE_HALF_LINK_RX`) :
```c
#include "rf_bus.h"
bool rf_bus_lock(uint32_t timeout_ms)
{ return s_tx_mutex && xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
void rf_bus_unlock(void) { if (s_tx_mutex) xSemaphoreGive(s_tx_mutex); }
spi_host_device_t rf_bus_host(void) { return BOARD_NRF_SPI_HOST; }
```
half_link.c (sous `CONFIG_KASE_HALF_LINK_TX`) : idem avec `s_tx_radio_mux` et `BOARD_NRF_SPI_HOST`.
⚠ `kbd_tx_locked`/`half_link_tx_frame` prennent déjà ce mutex : le pilote écran ne doit **jamais** être appelé depuis leur contexte (il l'est depuis la tâche display uniquement).
- [ ] **Step 3:** Build gauche/droite fusion + défaut `rc=0`. Commit :
```bash
git add main/comm/rf/rf_bus.h main/comm/rf/kbd_relay_tx.c main/comm/rf/half_link.c
git commit -m "feat(rf): rf_bus_lock/unlock — le propriétaire de la radio prête son bus SPI à l'écran"
```

---

### Task 3 : Pilote panneau `memlcd_panel` + bring-up damier (banc, go/no-go)

**Files:**
- Create: `main/display/memlcd/memlcd_panel.h`, `main/display/memlcd/memlcd_panel.c`
- Modify: `main/Kconfig.projbuild` (`KASE_DISPLAY_MEMLCD`, `KASE_HAS_DISPLAY` default y if), `main/CMakeLists.txt`, `boards/niphar_left/board.h` (macros LCD, commentaire J12 corrigé), `boards/niphar_right/board.h` (rotation), `sdkconfig.defaults.niphar_*`, `main/main.c` (CS bas au boot + test damier temporaire)

**Interfaces:**
- Produces:
  - `esp_err_t memlcd_panel_init(void);` — ajoute le device SPI (mode 0, 1 MHz, `spics_io_num=-1`) sur `rf_bus_host()`, configure `BOARD_LCD_CS_GPIO` en sortie **basse**.
  - `void memlcd_cs_idle(void);` — CS bas ; appelable **très tôt** au boot (avant la radio), pour que l'écran n'écoute jamais.
  - `bool memlcd_panel_clear(void);` — commande M2 (sous verrou).
  - `bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *bits /* count × MEMLCD_LINE_BYTES */);` — write-line M0, adresses 1..160, sous verrou, VCOM basculé.
  - `bool memlcd_panel_vcom_tick(void);` — trame VCOM seule (M1 basculé), sous verrou.
  - `void memlcd_panel_test_pattern(void);` — damier 8 px (bring-up).

- [ ] **Step 1: Kconfig / CMake / board.h**
Kconfig (après `KASE_BATT_SENSE`) :
```
config KASE_DISPLAY_MEMLCD
    bool "Ecran Sharp memory-LCD LS011B7DH03 (moities Niphargus, portrait 68x160)"
    default n
    help
        Pilote + backend LVGL de l'ecran nice!view des moities. SPI partage avec
        le nRF24 : toute transaction ecran passe sous le verrou radio. Image
        gelee en light sleep, aucun reveil pour l'ecran.
```
et dans `KASE_HAS_DISPLAY` : ajouter **en tête** `default y if KASE_DISPLAY_MEMLCD` (avant `default n if KASE_NIPHAR_MASTER`).
CMake : dans le bloc `if(CONFIG_KASE_HAS_DISPLAY)`, ajouter une branche `elseif(DISPLAY_BACKEND STREQUAL "memlcd")` avec `display/memlcd/memlcd_panel.c`, `display/memlcd/memlcd_backend.c` (Task 4), `display/assets/img_niphargus_60.c` (Task 5) ; racine `CMakeLists.txt` : `if(_board_h_content MATCHES "BOARD_DISPLAY_BACKEND_MEMLCD") set(DISPLAY_BACKEND "memlcd" …)` **avant** le test ROUND.
board.h gauche : remplacer le bloc « Pas d'écran sur la gauche » par `#define BOARD_DISPLAY_BACKEND_MEMLCD`, `BOARD_LCD_CS_GPIO GPIO_NUM_14`, `BOARD_LCD_CS_ACTIVE_HIGH 1`, `BOARD_LCD_ROTATE_180 0`, `BOARD_DISPLAY_WIDTH 68`, `BOARD_DISPLAY_HEIGHT 160`, `BOARD_DISPLAY_SLEEP_MS 60000` ; board.h droite : ajouter `BOARD_DISPLAY_BACKEND_MEMLCD`, `BOARD_LCD_ROTATE_180 0`, `BOARD_DISPLAY_WIDTH/HEIGHT`, `BOARD_DISPLAY_SLEEP_MS`. `sdkconfig.defaults.niphar_left/right` : `CONFIG_KASE_DISPLAY_MEMLCD=y` (+ dans les sdkconfig des 4 builds).

- [ ] **Step 2: `memlcd_panel.c`** (extrait central — le reste est plomberie SPI standard) :
```c
#define CMD_WRITE  0x80   /* M0 */
#define CMD_VCOM   0x40   /* M1 */
#define CMD_CLEAR  0x20   /* M2 */
static spi_device_handle_t s_dev; static uint8_t s_vcom;
static void cs(bool on) { gpio_set_level(BOARD_LCD_CS_GPIO, on ? 1 : 0); }   /* actif HAUT */
static bool xfer(const uint8_t *tx, size_t n) {
    spi_transaction_t t = { .length = n * 8, .tx_buffer = tx };
    return spi_device_polling_transmit(s_dev, &t) == ESP_OK;
}
bool memlcd_panel_write_lines(uint16_t first, uint16_t count, const uint8_t *bits)
{
    if (!s_dev || !rf_bus_lock(5)) return false;
    s_vcom ^= CMD_VCOM;
    static uint8_t buf[2 + (MEMLCD_LINE_BYTES + 2) * 16];   /* 16 lignes max par transaction */
    bool ok = true;
    for (uint16_t done = 0; done < count && ok; done += 16) {
        uint16_t n = (count - done > 16) ? 16 : (count - done);
        size_t p = 0;
        buf[p++] = memlcd_rev8((uint8_t)(CMD_WRITE | s_vcom));
        for (uint16_t i = 0; i < n; i++) {
            buf[p++] = memlcd_rev8((uint8_t)(first + done + i + 1));      /* adresse 1..160 */
            memcpy(&buf[p], bits + (size_t)(done + i) * MEMLCD_LINE_BYTES, MEMLCD_LINE_BYTES);
            p += MEMLCD_LINE_BYTES;
            buf[p++] = 0x00;                                             /* dummy fin de ligne */
        }
        buf[p++] = 0x00;                                                 /* dummy fin de trame */
        cs(true); esp_rom_delay_us(6); ok = xfer(buf, p); esp_rom_delay_us(2); cs(false);
    }
    rf_bus_unlock();
    return ok;
}
```
(`clear` = `[rev8(CMD_CLEAR|vcom)][0x00]` ; `vcom_tick` = `[rev8(vcom)][0x00]` — sans M0.) **Ordre des pixels dans `bits`** : bit 0 de l'octet 0 = pixel x=0 ; la Task 4 remplit dans cet ordre.
`memlcd_cs_idle()` : `gpio_reset_pin` + sortie + niveau 0 — appelée dans `main.c` **juste après `veille_liberer_gpio()`** des deux côtés, avant toute init radio.

- [ ] **Step 3: Damier (banc, go/no-go)** — dans `main.c`, après `batt_sense_init()`, temporairement : `memlcd_panel_init(); memlcd_panel_clear(); memlcd_panel_test_pattern();` (damier 8 px : lignes alternées `0xAA`/`0x55` par blocs de 8). Build + flash la moitié qui porte le FTDI. **Critère** : un damier **net** sur tout le panneau. Décalé/brouillé → inverser l'ordre des bits pixel (ou `MEMLCD_LINE_BYTES`/orientation) — un seul paramètre à la fois, refaire. Noter le résultat dans le commit. Retirer l'appel temporaire ensuite (le backend prend le relais).

- [ ] **Step 4: Commit**
```bash
git add main/display/memlcd/memlcd_panel.c main/display/memlcd/memlcd_panel.h main/Kconfig.projbuild main/CMakeLists.txt CMakeLists.txt boards/niphar_left/board.h boards/niphar_right/board.h sdkconfig.defaults.niphar_left sdkconfig.defaults.niphar_right main/main.c
git commit -m "feat(memlcd): pilote panneau LS011B7DH03 — write-line LSB-first sous verrou radio, CS bas au boot ; damier prouvé au banc"
```

---

### Task 4 : Backend LVGL `memlcd_backend` — bandeau, centre, pied

**Files:**
- Create: `main/display/memlcd/memlcd_backend.c`
- Modify: `main/main.c` (démarrage écran pour la **droite**, rôle `NIPHAR_SLAVE`), `main/display/status_display.c` si besoin (rien de prévu)

**Interfaces:**
- Consumes: `memlcd_panel_*` (Task 3), `memlcd_model_*` (Task 1), `batt_sense_dv/charging` (jauge), `kbd_active_route()` (gauche), `current_layout` + `layout_names` (gauche), `display_backend_t`.
- Produces: `const display_backend_t memlcd_display_backend;` ; `void memlcd_backend_set_remote(uint8_t couche, uint8_t batt_autre_dv, uint8_t chg, bool dongle_ok);` (alimentée par la trame DISPLAY, Task 6).

- [ ] **Step 1: LVGL** — `init()` : `lvgl_port_init` (si pas déjà), `lv_disp_draw_buf_init` sur un buffer **68 × 160 / 8** (1 bit, 1360 o, ×2 pour le double buffer LVGL en 16 bits : on garde le rendu LVGL 16 bits par défaut du projet et on **seuille dans le flush** — comme l'OLED — vers un tampon 1 bit local `s_fb[160][9]`) ; `lv_disp_drv_t` : `hor_res = 68`, `ver_res = 160`, `flush_cb = memlcd_flush`. `flush_cb` : pour chaque ligne `y` de l'aire, écrire les bits (`px.full < seuil` → noir → bit à 1 selon la polarité du panneau ; à confirmer au damier : sur Sharp, **1 = blanc**), marquer la ligne « sale » ; en fin d'aire, `memlcd_panel_write_lines()` des lignes sales contiguës, `lv_disp_flush_ready`.
- [ ] **Step 2: UI** — objets LVGL : bandeau (label route `RF`/`USB` + `▲` si dongle vu, UNSCII 8 ; jauge batterie locale = `lv_bar` vertical 10 × 22 + label tension), ligne, zone centrale (**gauche** : 1-3 labels Montserrat 14 depuis `memlcd_couper_nom(layout_names[couche])` + label `L<n>` ; **droite** : `lv_img` `img_niphargus_60` **centré** : `lv_obj_align(img, LV_ALIGN_CENTER, 0, 0)` dans la zone entre bandeau et pied), ligne, pied (label `DROITE`/`GAUCHE` + tension de l'autre + `■` si `chg == 2`, `?` si dV inconnu). `update()` : recalcule un `memlcd_model_t` depuis les sources ; si `memlcd_model_diff` → met à jour les labels ; **VCOM** : toutes les 1 s (compteur dans `update`, appelé toutes les 100 ms par la tâche) `memlcd_panel_vcom_tick()` si aucune écriture ce tour.
- [ ] **Step 3: sleep/wake** — `sleep()` : `lvgl_port_lock`, stop des timers LVGL (`lv_timer_pause`), `s_sleeping = true` (plus aucun VCOM ni flush) ; **pas** de clear (image gelée). `wake()` : `s_sleeping = false`, `lv_timer_resume`, `lv_obj_invalidate(lv_scr_act())` → redessin complet.
- [ ] **Step 4: Démarrage droite** — `main.c` : le bloc display est sous `DEVICE_ROLE_KEYBOARD` ; ajouter, sous `#if CONFIG_KASE_DEVICE_ROLE_NIPHAR_SLAVE && CONFIG_KASE_DISPLAY_MEMLCD`, le même démarrage (`display_set_backend(&memlcd_display_backend); status_display_start(); xTaskCreatePinnedToCore(status_display_task…)`). La `status_display_task` lit `current_layout`/`is_layer_changed` : sur la droite ces symboles viennent de… **rien** (pas de moteur) → fournir dans `memlcd_backend.c` sous `NIPHAR_SLAVE` : `uint8_t current_layout; volatile uint8_t is_layer_changed;` alimentés par `memlcd_backend_set_remote`. Sur la gauche, le sélecteur de backend de `main.c` devient : `#if defined(BOARD_DISPLAY_BACKEND_MEMLCD) memlcd_display_backend #elif ROUND … #else oled`.
- [ ] **Step 5: Banc** — flash la moitié FTDI : splash (`KaSe`/version, déjà géré par status_display) puis bandeau/pied avec **ses** données ; gauche : le nom de couche ; droite : le logo (Task 5 — d'ici là un label « NIPHARGUS »). Frapper pendant l'affichage : aucune perte (verrou). Commit :
```bash
git add main/display/memlcd/memlcd_backend.c main/main.c
git commit -m "feat(memlcd): backend LVGL — bandeau, couche statique / logo, pied ; gel en veille ; démarrage écran de la droite"
```

---

### Task 5 : Le logo Niphargus (généré) centré sur la droite

**Files:**
- Create: `scripts/gen_logo_memlcd.sh`, `main/display/assets/img_niphargus_60.c`
- Modify: `main/display/memlcd/memlcd_backend.c` (remplacer le label par `lv_img`)

- [ ] **Step 1:** `scripts/gen_logo_memlcd.sh` :
```bash
#!/usr/bin/env bash
# Génère main/display/assets/img_niphargus_60.c depuis le SVG du dépôt Niphargus.
# Reproductible : Inkscape → PNG 60 px (fond blanc) → seuil 128 → LV_IMG_CF_ALPHA_1BIT.
set -euo pipefail
SVG="${1:-$HOME/Documents/GitHub/Niphargus/images/niphargus_logo.svg}"
OUT="$(dirname "$0")/../main/display/assets/img_niphargus_60.c"
TMP="$(mktemp -d)"
inkscape "$SVG" --export-type=png --export-width=60 --export-background=white \
         --export-background-opacity=1 --export-filename="$TMP/logo.png" >/dev/null 2>&1
python3 - "$TMP/logo.png" "$OUT" <<'PY'
import sys, struct, zlib
png, out = sys.argv[1], sys.argv[2]
# décodeur PNG minimal (pas de PIL sur la machine) : RGBA/RGB/L 8 bits, non entrelacé
d = open(png, "rb").read(); assert d[:8] == b"\x89PNG\r\n\x1a\n"
pos = 8; idat = b""; w = h = ct = 0
while pos < len(d):
    ln = struct.unpack(">I", d[pos:pos+4])[0]; typ = d[pos+4:pos+8]; body = d[pos+8:pos+8+ln]; pos += 12 + ln
    if typ == b"IHDR": w, h, bd, ct = struct.unpack(">IIBB", body[:10])
    elif typ == b"IDAT": idat += body
raw = zlib.decompress(idat); bpp = {0:1, 2:3, 4:2, 6:4}[ct]; stride = w * bpp
rows = []; prev = bytearray(stride); p = 0
def paeth(a,b,c):
    pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
for y in range(h):
    f = raw[p]; line = bytearray(raw[p+1:p+1+stride]); p += 1 + stride
    for i in range(stride):
        a = line[i-bpp] if i >= bpp else 0; b = prev[i]; c = prev[i-bpp] if i >= bpp else 0
        if f == 1: line[i] = (line[i] + a) & 255
        elif f == 2: line[i] = (line[i] + b) & 255
        elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
        elif f == 4: line[i] = (line[i] + paeth(a,b,c)) & 255
    rows.append(bytes(line)); prev = line
bits = bytearray(); rowbytes = (w + 7) // 8
for y in range(h):
    rb = bytearray(rowbytes)
    for x in range(w):
        px = rows[y][x*bpp:(x+1)*bpp]; lum = px[0] if bpp in (1,2) else int(0.299*px[0]+0.587*px[1]+0.114*px[2])
        if lum < 128: rb[x >> 3] |= 0x80 >> (x & 7)     # 1 = pixel dessiné (alpha)
    bits += rb
with open(out, "w") as f:
    f.write("/* GÉNÉRÉ par scripts/gen_logo_memlcd.sh — ne pas éditer à la main. */\n")
    f.write('#include "lvgl.h"\n')
    f.write(f"static const uint8_t img_niphargus_60_map[] = {{\n  0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff,\n")  # palette alpha-1bit: transparent, opaque
    for i in range(0, len(bits), 16): f.write("  " + ",".join(f"0x{b:02x}" for b in bits[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"const lv_img_dsc_t img_niphargus_60 = {{ .header.cf = LV_IMG_CF_ALPHA_1BIT, .header.always_zero = 0, .header.w = {w}, .header.h = {h}, .data_size = {len(bits)+8}, .data = img_niphargus_60_map }};\n")
print(f"{out}: {w}x{h}, {len(bits)} octets de bits")
PY
```
- [ ] **Step 2:** `chmod +x`, lancer, vérifier que le `.c` compile ; `lv_img_set_src(img, &img_niphargus_60)` + `lv_obj_set_style_img_recolor` noir si nécessaire (ALPHA_1BIT prend la couleur de style). Centrage : `lv_obj_align(img, LV_ALIGN_CENTER, 0, 0)` dans un conteneur qui couvre exactement la zone entre les deux lignes.
- [ ] **Step 3:** Banc droite : logo net, centré, aucun pixel parasite ; comparer au rendu du compagnon visuel (identique attendu). Commit :
```bash
git add scripts/gen_logo_memlcd.sh main/display/assets/img_niphargus_60.c main/display/memlcd/memlcd_backend.c
git commit -m "feat(memlcd): logo Niphargus 60 px généré depuis le SVG, centré sur la droite"
```

---

### Task 6 : Trame DISPLAY dans l'ACK — couche et batterie de l'autre moitié

> **Faite puis RETIRÉE le 2026-09-14** (commits 04531a3e, 5e805e41, puis retrait) : l'utilisateur ne veut pas de la batterie de l'autre moitié. Voir la spec §4 pour ce que le banc a appris (pipe partagé, FIFO vidée par l'excursion). Le reste de cette section est conservé comme archive.

**Files:**
- Modify: `main/comm/rf/rf_rx_task.c` (dongle : charger DISPLAY quand pas de sync), `main/comm/rf/dongle_engine.c/.h` (`dongle_display_ack_for(half, out)`), `main/comm/rf/half_link.c` (droite : `send_ap` + décodage), `main/comm/rf/kbd_relay_tx.c` (gauche : décodage), `main/display/memlcd/memlcd_backend.c` (`set_remote`)

- [ ] **Step 1: Dongle** — `dongle_display_ack_for(uint8_t half, uint8_t *out)` : `rf_display_t d = { .to_right = (half==RIGHT), .couche = current_layout, .batt_autre_dv/chg = cache de l'AUTRE moitié (dongle_cache_get_battery(1-idx), 0xFF→0), .dongle_ok = 1 }`. Dans `drain_radio`, après le bloc sync : `else if (slot == RF_SLOT_KBD && de_la_moitie_connue) { rf_driver_load_ack_payload(radio, 0, dispbuf, dispn); }` où `de_la_moitie_connue` = STATUS (half du flag) ou MATRIX (half). Priorité : le bloc sync charge d'abord ; DISPLAY seulement si `!dongle_sync_active()`.
- [ ] **Step 2: Droite** — `half_link_tx_frame` : `rf_driver_send_ap(&s_radio, buf, n, ack, &ack_n)` ; si `rf_decode_display(ack, ack_n, &d) && d.to_right` → `memlcd_backend_set_remote(d.couche, d.batt_autre_dv, d.batt_autre_chg, d.dongle_ok)` (sous `CONFIG_KASE_DISPLAY_MEMLCD`). Les trames de sync (BEACON/CHUNK) reçues par la droite sont ignorées (elle n'a pas de keymap).
- [ ] **Step 3: Gauche** — dans `kbd_tx_locked`, après le décodage sync : `else if (rf_decode_display(ack, ack_n, &d) && !d.to_right) memlcd_backend_set_remote(...)`. En mode USB (dongle muet côté typing), la couche locale prime : `set_remote` n'écrase la couche que si `kbd_active_route() != KBD_OUT_USB`.
- [ ] **Step 4: Banc** — les deux flashées : la droite affiche la couche courante et la batterie de la gauche ; changer de couche par `TO` → l'écran droite suit (≤ 1 s si on tape, ≤ 30 s au repos) ; un `MO` tenu → **rien ne bouge**. La gauche affiche la batterie de la droite. Commit :
```bash
git add main/comm/rf/rf_rx_task.c main/comm/rf/dongle_engine.c main/comm/rf/dongle_engine.h main/comm/rf/half_link.c main/comm/rf/kbd_relay_tx.c main/display/memlcd/memlcd_backend.c
git commit -m "feat(memlcd): trame DISPLAY dans l'ACK — couche statique et batterie de l'autre moitié sur les deux écrans"
```

---

### Task 7 : Veille, contrat, smoke, docs, push

**Files:**
- Modify: `main/power/veille.c` (appeler `status_display_sleep()`/`wake()` autour du light sleep si la tâche display ne le fait pas déjà pour ce rôle), `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`, `docs/NIPHARGUS_V2_HARDWARE.md`, `boards/niphar_left/board.h` (commentaire J12), mémoire.

- [ ] **Step 1:** Vérifier au banc : light sleep → image **gelée** lisible ; aucun réveil dû à l'écran (la carte dort toujours à 60 s) ; réveil → écran vivant, VCOM repart. Si `status_display_task` n'existe pas sur la droite, `veille.c` appelle directement `memlcd_display_backend.sleep()/wake()` sous `CONFIG_KASE_DISPLAY_MEMLCD`.
- [ ] **Step 2:** `COMPORTEMENTS.md`, section « Écrans memory-LCD » : `[test:test_memlcd_model]` (rev8 involutif ; nom coupé 4/3/… ; diff = redessin seulement au changement), `[test:test_rf_display_roundtrip]` (trame DISPLAY 4 o, round-trip), `[smoke:Écrans memory-LCD]` (damier net ; bandeau/pied ; couche suit TO pas MO ; logo centré ; batterie de l'autre ≤ 30 s ; frappe sans perte pendant le rafraîchissement ; image gelée en veille, aucun réveil dû à l'écran). Item smoke correspondant dans `HARDWARE_SMOKE_TEST.md` (section Half).
- [ ] **Step 3:** `NIPHARGUS_V2_HARDWARE.md` : J12 **peuplé** (les deux moitiés ont l'écran, 2026-09-14), orientation portrait, CS tenu bas au boot. `board.h` gauche : commentaire J12 corrigé.
- [ ] **Step 4:** `TRIPWIRE_CONTRAT_STRICT=1 ./scripts/check.sh --fast` vert ; builds gauche/droite fusion + défaut, dongle fusion + défaut, `kase_v1`/`kase_v2` (les backends existants intacts) `rc=0`.
- [ ] **Step 5: Commit + push**
```bash
git add main/power/veille.c COMPORTEMENTS.md docs/HARDWARE_SMOKE_TEST.md docs/NIPHARGUS_V2_HARDWARE.md boards/niphar_left/board.h
git commit -m "feat(memlcd): veille (image gelée), contrat, smoke, doc matériel — écrans livrés (v1)"
git push origin main   # devshell : pre-push = 7 boards
```

---

## Notes d'exécution

- **Ordre et go/no-go** : Task 1 (pur) → 2 (verrou) → **3 (damier = le go/no-go du protocole : LSB-first, ordre des lignes, polarité 1 = blanc)** → 4 → 5 → 6 → 7. Ne pas écrire de UI avant que le damier soit net.
- **Un seul paramètre à la fois** au bring-up : polarité des pixels, ordre des bits, `MEMLCD_LINE_BYTES`, rotation. Chaque essai = un flash + une photo mentale du panneau.
- **Le FTDI ne sert qu'une moitié** : bring-up sur celle qui le porte, puis déplacer pour l'autre. Flash app-only, MAC vérifié (gauche `d0:cf:13:21:92:60`, droite `80:b5:4e:eb:5e:08`, dongle `ac:a7:04:18:82:24`).
- **Jamais** de transaction écran hors `rf_bus_lock` — si une frappe se perd pendant un rafraîchissement, c'est la première chose à vérifier.
