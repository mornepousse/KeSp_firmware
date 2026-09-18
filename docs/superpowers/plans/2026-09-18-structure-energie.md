# Structure énergie des moitiés Niphargus — plan d'implémentation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** donner un domicile à la politique d'énergie des moitiés (cadences, vetos de veille, séquence sommeil/réveil) pour que les régressions du 2026-09-16 (tick de 10 ms qui tuait le tickless, relais à 100 ms qui perdait la droite en USB) ne puissent plus se produire en silence — et fermer le trou du filet : le pre-push garde aujourd'hui une configuration qui n'est plus flashée.

**Architecture:** (1) la fusion devient la configuration par défaut et le chemin pré-fusion `HALF_LINK_RX` disparaît ; (2) toutes les cadences vivent dans `power/cadence.h`, gardées par `_Static_assert` contre la règle du tickless (repos ≥ 30 ms) ; (3) une tâche `power/veille_task.c` unique aux deux moitiés possède l'horloge d'inactivité, le battement de cœur et un registre de **vetos** (comme les verrous esp_pm) plus des **hooks** sommeil/réveil enregistrés par les modules ; (4) les réveilleurs périodiques restants passent en événementiel ou à 1 s.

**Tech Stack:** ESP-IDF 5.5.2 (esp_pm, tickless idle, FreeRTOS 100 Hz), nRF24L01+, tests host CMake (`test/`), `scripts/check.sh`.

**Spec:** revue du 2026-09-18 (conversation) — constats ancrés : `scripts/check.sh:30` (7 boards par défaut, non-fusion), `keyboard_task.c:217` et `half_link.c:592` (deux évaluateurs de veille à règles différentes), `main.c:109` et `half_link.c:582` (deux HB), `veille.c:95-103`/`186-191` (échelle d'`#if` dupliquée), `main.c:134` (écran droite 100 ms fixes), `link_uart.c:149` (poll 100 ms au repos).

## Global Constraints

- Workflow anti-régression : `./scripts/check.sh --fast` vert avant chaque commit ; toute source modifiée sans test → ligne au `COMPORTEMENTS.md` (sinon le Stop hook bloque). Pre-push = 7 boards.
- TDD pour toute logique pure : test rouge d'abord, prouver qu'il **mord** (bug transitoire → rouge → revert).
- Règle tickless (ESP-IDF, `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3`, tick 10 ms) : **aucune attente périodique au repos < 30 ms** sur les moitiés. Un tick à 10 ms = zéro sommeil automatique, sans erreur.
- Ne jamais ralentir un tick sans lister ce qu'il **consomme** (ex. : le tick du relais gauche vide la FIFO RX du nRF24 en mode USB).
- Flash : app-only `esptool write_flash 0x20000`, **MAC-vérifié** avant chaque écriture (gauche `d0:cf:13:21:92:60`, droite `80:b5:4e:eb:5e:08`) ; console par FTDI `/dev/ttyUSB2` avec `capture.py` (pyserial dtr=rts=False, détaché) — **jamais** `cat` sur le port (tient la carte en reset).
- Toolchain : `nix develop /home/mae/nixos-config#esp-idf --command …` ; `export IDF_CCACHE_ENABLE=1`.
- Build par board : `idf.py -B build_<board> -DBOARD=<board> -DSDKCONFIG=build_<board>/sdkconfig build`. ⚠ Un `build_<board>/sdkconfig` existant **gagne** sur `sdkconfig.defaults.<board>` : après un changement de defaults, supprimer ce fichier pour le régénérer.
- Commits : message en français, `Co-Authored-By` selon la session ; `git push origin main` (le miroir GitHub suit).
- Preuve au banc obligatoire avant de déclarer une tâche finie quand elle touche un binaire flashé (frappe, ACK, écran, veille à 15 s, réveil, `light_sleep_counts` qui grimpe dans le HB de banc).

---

## Ordre et dépendances

| Tâche | Sujet | Dépend de | Taille |
|---|---|---|---|
| 1 | La fusion devient le défaut ; le check garde ce qui tourne | — | 1 h |
| 2 | Suppression du chemin pré-fusion `HALF_LINK_RX` | 1 | 2 h (mécanique) |
| 3 | `power/cadence.h` : un contrat, une garde | — | 1 h |
| 4 | Écran de la droite : 100 ms → 1 s, VCOM par horodatage | 3 | 30 min |
| 5 | Lien TRRS événementiel (file d'événements UART) | 3 | 1 h |
| 6 | Vetos de veille (logique pure) | — | 1 h |
| 7 | `power/veille_task.c` : une tâche, deux moitiés, hooks | 2, 6 | 1 j |
| 8 | Diagnostic de banc derrière `KASE_VEILLE_DIAG` | 7 + bug « première touche » réglé | 1 h |
| 9 | Radio : `rf/radio_owner.c` — **plan séparé** | 2 | — |

Les tâches 1-2 et 3-5 sont indépendantes ; 7 est le gros morceau.

---

### Task 1 : La fusion devient la configuration par défaut

**Constat.** `scripts/check.sh:30` construit `niphar_left`, `niphar_right`, `kase_dongle` avec leurs `sdkconfig.defaults` : `niphar_left` a `CONFIG_KASE_HALF_LINK_RX=y` et **pas** `CONFIG_KASE_DONGLE_FUSION`. Les cartes en service tournent `build_niphar_left_fusion`, `build_niphar_right_fusion`, `build_dongle_fusion` (fusion=1). Diff `CONFIG_KASE_*` entre défaut et fusion :
- gauche : `+DONGLE_FUSION=y`, `-HALF_LINK_RX=y`, `+MATRIX_LOG_CONSOLE=y` (ce dernier = banc, **ne pas** le mettre en défaut) ;
- droite : `+DONGLE_FUSION=y` ;
- dongle : `+DONGLE_FUSION=y` (entraîne `NO_KEYMAP_ENGINE=n` et `KEY_STATS=y` par Kconfig — attendu).

**Files:**
- Modify: `sdkconfig.defaults.niphar_left:51-52`
- Modify: `sdkconfig.defaults.niphar_right` (après la ligne 44 `CONFIG_KASE_HALF_LINK_TX=y`)
- Modify: `sdkconfig.defaults.dongle` (après la ligne 4)
- Modify: `main/Kconfig.projbuild:89-91` (`default n` → `default y if NIPHAR_MASTER || NIPHAR_SLAVE || DONGLE` **n'est pas** retenu : un défaut explicite dans chaque fichier est plus lisible et n'affecte pas V1/V2/conchodytes)
- Modify: `COMPORTEMENTS.md`, `CLAUDE.md` (section Périmètre), `docs/HARDWARE_SMOKE_TEST.md`

- [x] **Step 1 : gauche — fusion par défaut, RX retiré**

Dans `sdkconfig.defaults.niphar_left`, remplacer :
```
CONFIG_KASE_KBD_WIRELESS=y
CONFIG_KASE_HALF_LINK_RX=y
```
par :
```
CONFIG_KASE_KBD_WIRELESS=y
# Fusion au dongle (design 2026-09-12, en service depuis le 2026-09-13) : la
# gauche emet sa demi-matrice BRUTE, le dongle fusionne et tape. C'est le mode
# nominal du clavier ; le chemin pre-fusion (HALF_LINK_RX) est retire.
CONFIG_KASE_DONGLE_FUSION=y
```

- [x] **Step 2 : droite et dongle — fusion par défaut**

`sdkconfig.defaults.niphar_right`, après `CONFIG_KASE_HALF_LINK_TX=y` :
```
# Fusion au dongle (2026-09-13) : la droite emet sa demi-matrice brute au dongle
# (KaSe.01), repli vers la gauche si le dongle se tait (half_tx_target_step).
CONFIG_KASE_DONGLE_FUSION=y
```
`sdkconfig.defaults.dongle`, après `CONFIG_KASE_DEVICE_ROLE_DONGLE=y` :
```
# Fusion (2026-09-13) : le dongle recoit deux demi-matrices brutes, fusionne et
# fait tourner le moteur keymap (NO_KEYMAP_ENGINE retombe a n par Kconfig).
CONFIG_KASE_DONGLE_FUSION=y
```

- [x] **Step 3 : régénérer les sdkconfig des trois boards et vérifier**

```bash
rm -f build_niphar_left/sdkconfig build_niphar_right/sdkconfig build_kase_dongle/sdkconfig
nix develop /home/mae/nixos-config#esp-idf --command bash -c '
  export IDF_CCACHE_ENABLE=1
  for b in niphar_left niphar_right kase_dongle; do
    idf.py -B build_$b -DBOARD=$b -DSDKCONFIG=build_$b/sdkconfig build 2>&1 | grep -E " error|Project build complete"
  done'
for b in niphar_left niphar_right kase_dongle; do
  echo "$b fusion=$(grep -c '^CONFIG_KASE_DONGLE_FUSION=y' build_$b/sdkconfig) rx=$(grep -c '^CONFIG_KASE_HALF_LINK_RX=y' build_$b/sdkconfig)"
done
```
Attendu : `fusion=1 rx=0` pour les trois, trois « Project build complete ».

- [x] **Step 4 : preuve au banc — le binaire par défaut se comporte comme le binaire fusion**

Flasher la moitié qui porte le FTDI depuis `build_niphar_<côté>/KeSp.bin` (MAC-check), relancer la capture, taper 1 min : ACK ≥ 98 % (« HID->dongle : N remis, 0 refuses » côté gauche ; « TX n envois, m acquittes » côté droite), écran à jour, veille à 15 s, réveil sur touche. Faire l'autre moitié quand le FTDI change de côté. Le dongle : `esptool --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x20000 build_kase_dongle/KeSp.bin` (MAC `ac:a7:04:18:82:24`), puis frappe depuis les deux moitiés.

- [x] **Step 5 : les dossiers `build_*_fusion` deviennent obsolètes**

```bash
rm -rf build_niphar_left_fusion build_niphar_right_fusion build_dongle_fusion
```
(Non suivis par git.) Les options de banc qu'ils portaient — `CONFIG_PM_PROFILING=y`, `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`, `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`, `CONFIG_KASE_MATRIX_LOG_CONSOLE=y` — se posent à la main dans `build_<board>/sdkconfig` quand on en a besoin (ce sont des options de banc, pas des défauts).

- [x] **Step 6 : contrat et docs**

`COMPORTEMENTS.md`, à la suite de la ligne `[smoke:Fusion — moteur local dormant]` la plus proche du bloc fusion, ajouter :
```
- [smoke:Fusion — moteur local dormant] La FUSION est la configuration PAR
  DÉFAUT des trois cartes du Niphargus (sdkconfig.defaults.niphar_left/right,
  dongle) : ce que `scripts/check.sh` construit au pre-push est ce qui est
  flashé. Jusqu'au 2026-09-18 le check gardait la gauche pré-fusion
  (HALF_LINK_RX) pendant que les cartes tournaient des builds `*_fusion` non
  gardés.
```
`CLAUDE.md`, section « Périmètre », après le paragraphe « Full RF le 2026-09-07 » : une phrase « **Fusion par défaut depuis le 2026-09-18** : `KASE_DONGLE_FUSION=y` dans les defaults des trois cartes ; le chemin pré-fusion `HALF_LINK_RX` est retiré (Task 2). Les dossiers `build_*_fusion` n'existent plus. »
`docs/HARDWARE_SMOKE_TEST.md` : dans l'item « Fusion — moteur local dormant », ajouter « (binaires issus de `build_niphar_left`/`build_niphar_right`/`build_kase_dongle`, pas d'un dossier `_fusion`) ».

- [x] **Step 7 : check et commit**

```bash
./scripts/check.sh --fast
git add sdkconfig.defaults.niphar_left sdkconfig.defaults.niphar_right sdkconfig.defaults.dongle COMPORTEMENTS.md CLAUDE.md docs/HARDWARE_SMOKE_TEST.md
git commit -m "build(fusion): la fusion devient la configuration par défaut des trois cartes du Niphargus — le pre-push garde désormais ce qui est flashé"
```

---

### Task 2 : Suppression du chemin pré-fusion `HALF_LINK_RX`

**Constat.** Une fois la fusion par défaut, `CONFIG_KASE_HALF_LINK_RX` (gauche qui écoute la droite en direct, `depends on !KASE_DONGLE_FUSION`) n'est plus compilé par aucun board. Il pèse ~300 lignes dans `half_link.c` (tâche RX, ISR, `half_link_remote_*`, `half_link_rx_start`, branches RX de `radio_sleep/wake`), 6 des 16 `#if` de `veille.c`, 3 blocs de `matrix_scan.c`, un bloc de `keyboard_task.c`, et toutes les conditions `!CONFIG_KASE_HALF_LINK_RX` de `kbd_relay_tx.c`. Refactor **mécanique** : le compilateur et les 7 boards sont l'oracle (éligible modèle économique, cf. CLAUDE.md).

**Files:**
- Modify: `main/Kconfig.projbuild:213-240` (retirer `KASE_HALF_LINK_RX` et `KASE_HALF_LINK_R1`)
- Modify: `main/comm/rf/half_link.c` (retirer tout ce qui est sous `#if CONFIG_KASE_HALF_LINK_RX` ; simplifier `#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX` → `#if CONFIG_KASE_HALF_LINK_TX`)
- Modify: `main/comm/rf/half_link.h` (prototypes RX : `half_link_rx_start`, `half_link_remote_pressed`, `half_link_remote_changed`, `half_link_note_wake`, `half_link_excursion_tx` si plus d'appelant)
- Modify: `main/comm/rf/kbd_relay_tx.c` (`#if !CONFIG_KASE_HALF_LINK_RX` → code inconditionnel ; `#if CONFIG_KASE_DONGLE_FUSION && !CONFIG_KASE_HALF_LINK_RX` → `#if CONFIG_KASE_DONGLE_FUSION`)
- Modify: `main/power/veille.c:7-10, 95-103, 165-167, 186-191, 228-233, 245-249, 258-260`
- Modify: `main/input/matrix_scan.c:54, 65-69, 301, 580, 682` (conditions `CONFIG_KASE_HALF_LINK_RX || (…)` → `(…)`)
- Modify: `main/input/keyboard_task.c:75-84` (bloc `#if CONFIG_KASE_HALF_LINK_RX … #elif` → garder la branche fusion seule)
- Modify: `main/main.c` (bloc `CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX` ligne 66 ; appel `half_link_rx_start` s'il existe)
- Modify: `main/CMakeLists.txt` si une source n'est incluse que sous RX
- Modify: `sdkconfig.defaults.niphar_left:26-35, 53-57` (commentaires sur RX/R1/NRF_PROBE : réécrire en trois lignes)
- Modify: `COMPORTEMENTS.md:50` (le `[NON GARDÉ]` « Au réveil, la réception RF est réarmée » décrit le chemin RX → supprimer), `.tripwire-nongardes` 2 → 1
- Test: aucun nouveau ; `test/test_half_state.c`, `test_fuse_halves.c`, `test_matrix_bitmap.c` sont purs et restent

**Interfaces:**
- Produces : `half_link.c` ne contient plus que la DROITE (TX) : `half_link_tx_init`, `half_link_tx_update`, `half_link_tx_matrix`, `half_link_tx_status`, `half_link_tx_refresh_start`, `half_link_tx_dongle_vu`, `half_link_radio_sleep/wake`, `rf_bus_lock`. `kbd_relay_tx.c` ne contient plus que la GAUCHE.

- [x] **Step 1 : inventaire exact avant de couper**

```bash
grep -rn "HALF_LINK_RX\|HALF_LINK_R1\|half_link_rx_\|half_link_remote_\|half_link_note_wake\|half_link_excursion_tx" main boards test --include=*.c --include=*.h --include=Kconfig* --include=CMakeLists.txt | cut -c1-120
```
Chaque ligne listée est à traiter dans les steps suivants ; relancer la commande à la fin : elle doit être vide (sauf commentaires historiques que l'on garde volontairement — les reformuler sans le nom de l'option).

- [x] **Step 2 : Kconfig**

Supprimer les blocs `config KASE_HALF_LINK_RX` (l. 213-~233) et `config KASE_HALF_LINK_R1` (l. 235-~245). Vérifier qu'aucun autre symbole n'a `depends on KASE_HALF_LINK_RX` (`grep -n "HALF_LINK_RX" main/Kconfig.projbuild` → vide).

- [x] **Step 3 : half_link.c / .h**

Supprimer : `half_link_irq_isr` (l. 72), la tâche `half_link_rx_task` (l. 699-~780) et son `rf_bus_lock` RX (l. 784), `half_link_remote_pressed/changed` (l. 830-840), `half_link_rx_start` (l. 842-~890), les branches `#if CONFIG_KASE_HALF_LINK_RX` de `half_link_radio_sleep/wake` (l. 892-921), `half_link_cfg` si seul le RX l'utilise, `s_radio_mux`, `s_distant*`. Toute condition `#if CONFIG_KASE_HALF_LINK_TX || CONFIG_KASE_HALF_LINK_RX` devient `#if CONFIG_KASE_HALF_LINK_TX`. Le commentaire d'en-tête du fichier décrit désormais « lien droite → dongle (fusion), repli vers la gauche ».

- [x] **Step 4 : kbd_relay_tx.c, veille.c, matrix_scan.c, keyboard_task.c, main.c**

Appliquer les remplacements listés dans **Files**. Dans `veille.c`, la séquence d'entrée devient :
```c
#if CONFIG_KASE_HALF_LINK_TX
    half_link_radio_sleep();      /* droite */
#endif
#if CONFIG_KASE_KBD_WIRELESS
    kbd_relay_sleep_prepare();    /* gauche */
#endif
```
et symétriquement au réveil (`half_link_radio_wake` / `kbd_relay_wake_restore`). Retirer les deux blocs `build_keycode_report(); send_hid_key();` sous RX (l. 228-233, 245-249) et les includes devenus inutiles (`key_processor.h`, `hid_report.h`, `matrix_flag.h` si plus utilisés dans `veille.c`).

- [x] **Step 5 : contrat**

Supprimer la ligne 50 de `COMPORTEMENTS.md` (`[NON GARDÉ] Au réveil, la réception RF est réarmée…`) ; écrire `1` dans `.tripwire-nongardes`. Ajouter sous le bloc fusion :
```
- [smoke:Fusion — moteur local dormant] Le chemin pré-fusion « la gauche écoute
  la droite en direct » (HALF_LINK_RX, B3 première version) est RETIRÉ le
  2026-09-18 : il n'était plus compilé par aucune carte. `half_link.c` ne
  contient plus que la droite, `kbd_relay_tx.c` que la gauche.
```

- [x] **Step 6 : compiler les 7 boards, preuve au banc, commit**

```bash
nix develop /home/mae/nixos-config#esp-idf --command ./scripts/check.sh --force
```
Attendu : vert. Puis flasher les deux moitiés (au fil du FTDI) et rejouer le smoke « Première touche après veille » + frappe 1 min + veille/réveil. Commit :
```bash
git commit -am "refactor(rf): retrait du chemin pré-fusion HALF_LINK_RX — half_link.c = la droite, kbd_relay_tx.c = la gauche, veille.c sans échelle d'#if par rôle"
```

---

### Task 3 : `power/cadence.h` — un contrat, une garde

**Constat.** Les cadences vivent dans 6 en-têtes ; la règle « repos ≥ 30 ms » n'existe qu'en commentaire. Une `_Static_assert` par constante de repos rend la violation impossible à compiler.

**Files:**
- Create: `main/power/cadence.h`
- Modify: `main/input/keyboard_cadence.h:21-25` (les `#define` partent dans cadence.h, l'en-tête l'inclut)
- Modify: `main/comm/rf/kbd_relay_tx.h:56-57` (idem)
- Modify: `main/comm/rf/half_link.c:602` (`tenu ? 20 : 100` → `tenu ? HALF_TX_TENU_MS : HALF_TX_REPOS_MS`)
- Modify: `main/comm/link/link_uart.c:42, 149` (`LINK_TICK_MS`, littéral `100` → `LINK_REPOS_MS`)
- Modify: `main/main.c:134` (`100` → `MEMLCD_DROITE_PERIODE_MS`), `main/main.c:122` (`10000` → `HB_PERIODE_MS`)
- Modify: `main/display/memlcd/memlcd_backend.c:224, 237` (`500`, `50`, `200` → `LVGL_TASK_MAX_SLEEP_MS`, `LVGL_TICK_MS`, `LVGL_REFR_MS`)
- Test: `test/test_cadence.c` (+ `test/CMakeLists.txt`, `test/test_main.c`)

**Interfaces:**
- Produces :
```c
#define CADENCE_TICK_MS        10u   /* CONFIG_FREERTOS_HZ = 100 */
#define CADENCE_REPOS_MIN_MS   30u   /* 3 ticks : CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP */
#define KBD_CADENCE_ACTIF_MS   10u
#define KBD_CADENCE_REPOS_MS   100u
#define KBD_CADENCE_FENETRE_MS 1500u
#define KBD_RELAY_REFRESH_MS   10u
#define KBD_RELAY_REPOS_MS     100u
#define HALF_TX_TENU_MS        20u
#define HALF_TX_REPOS_MS       100u
#define LINK_TICK_MS           10u
#define LINK_REPOS_MS          100u   /* Task 5 : 1000 */
#define MEMLCD_DROITE_PERIODE_MS 100u /* Task 4 : 1000 */
#define LVGL_TICK_MS           50u
#define LVGL_REFR_MS           200u
#define LVGL_TASK_MAX_SLEEP_MS 500u
#define HB_PERIODE_MS          10000u
#define CADENCE_REPOS_OK(ms) _Static_assert((ms) >= CADENCE_REPOS_MIN_MS, #ms " < 3 ticks : tue le light sleep automatique")
```

- [ ] **Step 1 : test rouge**

`test/test_cadence.c` :
```c
/* Cadences des moitiés : au repos, aucune attente périodique sous 3 ticks
 * (CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP) — sinon le light sleep automatique
 * n'arrive jamais, sans erreur. Les _Static_assert de cadence.h sont la vraie
 * garde ; ce test dit POURQUOI et vérifie les valeurs actives. */
#include "test_framework.h"
#include "../main/power/cadence.h"

static void test_le_repos_laisse_trois_ticks(void)
{
    TEST_ASSERT_EQ(CADENCE_REPOS_MIN_MS, 3 * CADENCE_TICK_MS, "3 ticks a 100 Hz");
    TEST_ASSERT(KBD_CADENCE_REPOS_MS >= CADENCE_REPOS_MIN_MS, "tache clavier");
    TEST_ASSERT(KBD_RELAY_REPOS_MS   >= CADENCE_REPOS_MIN_MS, "relais gauche");
    TEST_ASSERT(HALF_TX_REPOS_MS     >= CADENCE_REPOS_MIN_MS, "rafraichissement droite");
    TEST_ASSERT(LINK_REPOS_MS        >= CADENCE_REPOS_MIN_MS, "lien TRRS");
    TEST_ASSERT(MEMLCD_DROITE_PERIODE_MS >= CADENCE_REPOS_MIN_MS, "ecran droite");
    TEST_ASSERT(LVGL_TICK_MS         >= CADENCE_REPOS_MIN_MS, "tick LVGL");
    TEST_ASSERT(LVGL_REFR_MS         >= CADENCE_REPOS_MIN_MS, "rafraichissement LVGL");
}

static void test_l_actif_reste_reactif(void)
{
    /* Touche tenue / réparation : sous 20 ms, sinon les réaffirmations à 100 ms
     * et la réparation bornée (5 × 10 ms) perdent leur sens. */
    TEST_ASSERT(KBD_CADENCE_ACTIF_MS <= 20, "tache clavier active");
    TEST_ASSERT(KBD_RELAY_REFRESH_MS <= 20, "relais actif");
    TEST_ASSERT(HALF_TX_TENU_MS      <= 20, "droite touche tenue");
    TEST_ASSERT(LINK_TICK_MS         <= 20, "lien en poignee de main");
}

void test_cadence(void)
{
    TEST_SUITE("cadences des moities (tickless)");
    TEST_RUN(test_le_repos_laisse_trois_ticks);
    TEST_RUN(test_l_actif_reste_reactif);
}
```
Ajouter `test_cadence.c` dans `test/CMakeLists.txt` (après `test_keyboard_cadence.c`) et `extern void test_cadence(void);` + appel dans `test/test_main.c`.

- [ ] **Step 2 : rouge**

`./scripts/check.sh --fast` → rouge (`cadence.h` absent).

- [ ] **Step 3 : cadence.h**

Créer `main/power/cadence.h` avec les constantes de **Interfaces**, un commentaire d'en-tête (règle des 3 ticks, leçon du 2026-09-16 : « mode SLEEP 92 % et 0 sommeil ») et, après chaque constante `*_REPOS_MS`/`*_PERIODE_MS`/LVGL, la garde :
```c
CADENCE_REPOS_OK(KBD_CADENCE_REPOS_MS);
CADENCE_REPOS_OK(KBD_RELAY_REPOS_MS);
CADENCE_REPOS_OK(HALF_TX_REPOS_MS);
CADENCE_REPOS_OK(LINK_REPOS_MS);
CADENCE_REPOS_OK(MEMLCD_DROITE_PERIODE_MS);
CADENCE_REPOS_OK(LVGL_TICK_MS);
CADENCE_REPOS_OK(LVGL_REFR_MS);
```
Puis retirer les `#define` doublons de `keyboard_cadence.h` et `kbd_relay_tx.h` (remplacés par `#include "cadence.h"`), et substituer les littéraux listés dans **Files**. `main/CMakeLists.txt` : `power/` est-il dans les `INCLUDE_DIRS` ? (`pm_dfs.h` est inclus par `main.c`, donc oui.)

- [ ] **Step 4 : vert, puis prouver que la garde mord**

`./scripts/check.sh --fast` → vert. Puis, transitoirement, `#define KBD_RELAY_REPOS_MS 10u` → `idf.py -B build_niphar_left … build` doit **échouer** sur la `_Static_assert` ; rétablir 100u.

- [ ] **Step 5 : contrat, commit**

`COMPORTEMENTS.md`, à côté de la ligne `[test:test_keyboard_cadence]` :
```
- [test:test_cadence] Toutes les cadences des moitiés vivent dans
  `power/cadence.h` ; chaque cadence de REPOS est gardée par une
  `_Static_assert` ≥ 30 ms (3 ticks à 100 Hz, seuil du light sleep
  automatique) — une attente périodique plus courte ne compile pas. Les
  cadences ACTIVES restent ≤ 20 ms.
```
```bash
git add main/power/cadence.h main/input/keyboard_cadence.h main/comm/rf/kbd_relay_tx.h main/comm/rf/half_link.c main/comm/link/link_uart.c main/main.c main/display/memlcd/memlcd_backend.c test/ COMPORTEMENTS.md .tripwire-testcount
git commit -m "refactor(power): cadence.h — toutes les cadences des moitiés en un lieu, gardées par _Static_assert contre la règle des 3 ticks du tickless"
```

---

### Task 4 : Écran de la droite — 100 ms → 1 s, VCOM par horodatage

**Constat.** `main.c:134` : `for (;;) { be->update(); vTaskDelay(100 ms); }` pour un écran dont le modèle change une fois par 30 s. `memlcd_update` (`memlcd_backend.c:296`) compte 10 appels pour basculer le VCOM (~1 Hz) : il suppose la cadence de l'appelant.

**Files:**
- Modify: `main/display/memlcd/memlcd_backend.c:296`
- Modify: `main/power/cadence.h` (`MEMLCD_DROITE_PERIODE_MS 1000u`)
- Modify: `main/display/memlcd/memlcd_backend.c` `memlcd_wake` (l. ~300) : pousser l'image tout de suite au réveil, sans attendre le tick

- [ ] **Step 1 : VCOM indépendant de la cadence d'appel**

Remplacer la ligne 296 :
```c
    /* Entretien VCOM ~1 Hz quand rien ne s'écrit, quelle que soit la cadence
     * d'appel (100 ms à gauche, 1 s à droite). */
    static uint32_t s_vcom_ms;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now_ms - s_vcom_ms) >= 1000u) { s_vcom_ms = now_ms; memlcd_panel_vcom_tick(); }
```

- [ ] **Step 2 : le réveil ne laisse pas l'écran figé une seconde**

Dans `memlcd_wake`, après `s_dirty = true;`, ajouter un appel direct : `memlcd_update();` (déclarer le prototype `static void memlcd_update(void);` plus haut si nécessaire). L'appel est fait depuis `veille.c` au réveil, hors de la tâche écran ; `memlcd_update` prend déjà `s_fb_mux` et `lvgl_port_lock`, il est réentrant vis-à-vis de la tâche.

- [ ] **Step 3 : cadence**

`cadence.h` : `MEMLCD_DROITE_PERIODE_MS 1000u` (la `_Static_assert` reste vraie).

- [ ] **Step 4 : preuve au banc (droite)**

Flasher la droite, `light_sleep_counts` dans le HB doit monter d'environ la même valeur ; l'image ne clignote pas (VCOM toujours ~1 Hz : vérifier à l'œil sur 2 min qu'aucune zone ne grise) ; après une veille B7, l'écran revit à la première touche. Contrat :
```
- [smoke:Écrans] L'écran de la droite est servi à 1 s (modèle : jauge 30 s,
  dongle vu) ; l'entretien VCOM est horodaté (~1 Hz), indépendant de la
  cadence de la tâche ; au réveil l'image est repoussée immédiatement.
```
Commit : `perf(ecran droite): tâche à 1 s, VCOM horodaté, image repoussée au réveil`.

---

### Task 5 : Lien TRRS événementiel

**Constat.** `link_uart.c:149` : au repos (`LINK_HS_IDLE && !usb`) la tâche se réveille toutes les 100 ms pour lire un UART vide et sonder `tud_ready()`. L'UART sait réveiller sur réception (file d'événements du driver) ; l'USB est un événement humain (1 s suffit).

**Files:**
- Modify: `main/comm/link/link_uart.c:178` (`uart_driver_install(…, NULL, 0)` → file d'événements), `:149` (attente), déclaration `static QueueHandle_t s_uart_q;`
- Modify: `main/power/cadence.h` (`LINK_REPOS_MS 1000u`)

- [ ] **Step 1 : file d'événements**

```c
static QueueHandle_t s_uart_q;
…
ESP_ERROR_CHECK(uart_driver_install(BOARD_LINK_UART_NUM, 256, 0, 8, &s_uart_q, 0));
```

- [ ] **Step 2 : la tâche bloque sur la file**

Remplacer `vTaskDelay(repos ? pdMS_TO_TICKS(100) : 1);` par :
```c
        /* Au repos : bloqué sur la file UART — un octet du pair réveille la
         * tâche aussitôt (sa sonde arrive toutes les 300 ms en poignée de
         * main), l'USB est sondé à 1 s (événement humain). En poignée de main
         * ou lien établi : tick de 10 ms (keepalive 200 ms, timeouts 200-500 ms). */
        bool repos = (s_hs.state == LINK_HS_IDLE) && !usb;
        uart_event_t ev;
        (void)xQueueReceive(s_uart_q, &ev, pdMS_TO_TICKS(repos ? LINK_REPOS_MS : LINK_TICK_MS));
```
La lecture (`uart_read_bytes`, l. 89) reste en tête de boucle avec timeout 0 : elle draine ce que l'événement annonce. Si `ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL` : `uart_flush_input` + `xQueueReset(s_uart_q)` (bruit d'une TX flottante).

- [ ] **Step 3 : cadence et banc**

`cadence.h` : `LINK_REPOS_MS 1000u`. Banc : brancher le TRRS entre les deux moitiés, gauche en USB (source) → « lien monte », les deux switches ferment (`etat=2 5V=1` dans le bilan `link:`), il tient 2 min ; débrancher → repli. Sans TRRS, HB : `light_sleep_counts` inchangé ou meilleur. Contrat :
```
- [smoke:Lien TRRS] La tâche du lien est ÉVÉNEMENTIELLE au repos : bloquée sur
  la file UART (un octet du pair la réveille), USB sondé à 1 s ; 10 ms
  seulement en poignée de main ou lien établi.
```
Commit : `perf(lien): tâche TRRS bloquée sur la file d'événements UART au repos, USB sondé à 1 s`.

---

### Task 6 : Vetos de veille — logique pure

**Constat.** Deux évaluateurs (`keyboard_task.c:217` : `usb || lien` ; `half_link.c:592` : `lien`) avec des règles ad hoc. Un veto est un verrou nommé, comme `esp_pm_lock` : le module qui a une raison d'empêcher la veille la déclare ; la veille n'interroge personne.

**Files:**
- Create: `main/power/veille_veto.h`
- Test: `test/test_veille_veto.c` (+ CMake, test_main)

**Interfaces:**
```c
typedef enum {
    VEILLE_VETO_USB   = 1u << 0,   /* hôte USB prêt (gauche : clavier HID) */
    VEILLE_VETO_LIEN  = 1u << 1,   /* 5 V du TRRS actif (une moitié charge l'autre) */
    VEILLE_VETO_SYNC  = 1u << 2,   /* tirage de keymap par ACK en cours */
    VEILLE_VETO_TEST  = 1u << 3,   /* mode test matrice */
} veille_veto_t;
typedef struct { uint32_t actifs; } veille_vetos_t;
static inline void veille_veto_poser(veille_vetos_t *v, veille_veto_t q, bool on)
{ if (on) v->actifs |= (uint32_t)q; else v->actifs &= ~(uint32_t)q; }
static inline bool veille_bloquee(const veille_vetos_t *v) { return v->actifs != 0; }
/* Nom court des vetos actifs pour le HB : "usb+lien", "-" si aucun. n ≥ 24. */
static inline const char *veille_vetos_str(const veille_vetos_t *v, char *out, size_t n);
```

- [ ] **Step 1 : test rouge**

`test/test_veille_veto.c` :
```c
/* Vetos de veille : un registre nommé, comme les verrous esp_pm. Un veto posé
 * bloque toute veille ; deux poses du même veto puis une levée = levé (c'est
 * un état, pas un compteur — chaque module ne pose que le sien). */
#include "test_framework.h"
#include "../main/power/veille_veto.h"

static void test_sans_veto_rien_ne_bloque(void)
{
    veille_vetos_t v = {0};
    TEST_ASSERT(!veille_bloquee(&v), "vide");
}
static void test_un_veto_bloque_jusqu_a_sa_levee(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    TEST_ASSERT(veille_bloquee(&v), "usb pose");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    veille_veto_poser(&v, VEILLE_VETO_USB, false);
    TEST_ASSERT(veille_bloquee(&v), "lien tient encore");
    veille_veto_poser(&v, VEILLE_VETO_LIEN, false);
    TEST_ASSERT(!veille_bloquee(&v), "tout leve");
}
static void test_lever_un_veto_absent_est_sans_effet(void)
{
    veille_vetos_t v = {0};
    veille_veto_poser(&v, VEILLE_VETO_SYNC, false);
    TEST_ASSERT(!veille_bloquee(&v), "toujours vide");
}
static void test_noms_pour_le_hb(void)
{
    char buf[24]; veille_vetos_t v = {0};
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "-") == 0, "aucun -> -");
    veille_veto_poser(&v, VEILLE_VETO_USB, true);
    veille_veto_poser(&v, VEILLE_VETO_LIEN, true);
    TEST_ASSERT(strcmp(veille_vetos_str(&v, buf, sizeof buf), "usb+lien") == 0, "usb+lien");
}
void test_veille_veto(void)
{
    TEST_SUITE("vetos de veille");
    TEST_RUN(test_sans_veto_rien_ne_bloque);
    TEST_RUN(test_un_veto_bloque_jusqu_a_sa_levee);
    TEST_RUN(test_lever_un_veto_absent_est_sans_effet);
    TEST_RUN(test_noms_pour_le_hb);
}
```
Câbler dans `test/CMakeLists.txt` et `test/test_main.c`. `./scripts/check.sh --fast` → rouge.

- [ ] **Step 2 : implémentation**

`main/power/veille_veto.h` : les types de **Interfaces** ; `veille_vetos_str` concatène dans l'ordre usb, lien, sync, test avec `+`, écrit `-` si vide (snprintf borné). Vert.

- [ ] **Step 3 : mord**

Transitoirement `return v->actifs == 0;` dans `veille_bloquee` → rouge → rétablir. Commit : `feat(veille): registre de vetos pur (usb, lien, sync, test) — logique testée, pas encore câblée`.

---

### Task 7 : `power/veille_task.c` — une tâche, deux moitiés, hooks

**Constat.** Après Task 2, la veille est encore évaluée dans deux tâches (gauche : `keyboard_task.c:196-218` ; droite : `half_link.c:520-593`), avec deux HB (`main.c:85-125` `cpu_time_logger_task` ; `half_link.c:565-590`), et `veille_legere_entrer` appelle nommément radio/écran/jauge sous `#if`. Cible : **une** tâche identique sur les deux moitiés ; les modules posent des vetos et enregistrent des hooks.

**Files:**
- Create: `main/power/veille_task.c`, `main/power/veille_task.h`
- Modify: `main/power/veille.c` (retirer `veille_diag`, `veille_pas` devient interne à la tâche ; `veille_legere_entrer` appelle les hooks au lieu des modules)
- Modify: `main/power/veille.h` (prototypes)
- Modify: `main/main.c:85-125` (supprimer `cpu_time_logger_task` et sa création l. ~500 ; appeler `veille_task_start()` dans les deux rôles après l'init des modules)
- Modify: `main/input/keyboard_task.c:196-218` (supprimer le bloc veille ; poser `VEILLE_VETO_TEST` sur `matrix_test_mode` là où il change : `matrix_scan.c:219-233`)
- Modify: `main/comm/rf/half_link.c:520-593` (supprimer le bloc veille + HB ; enregistrer le hook radio dans `half_link_tx_init`)
- Modify: `main/comm/rf/kbd_relay_tx.c` (hook radio dans `kbd_relay_init` ; `VEILLE_VETO_SYNC` aux lignes 193/200-201/383)
- Modify: `main/comm/link/link_uart.c:57-62` (`set_5v` pose/lève `VEILLE_VETO_LIEN`)
- Modify: `main/power/pm_dfs.c` (`pm_dfs_usb_event` pose aussi `VEILLE_VETO_USB` — **gauche seulement**, voir Step 2)
- Modify: `main/display/memlcd/memlcd_backend.c` (hook écran : `sleep`/`wake` du backend — déjà appelés par veille.c ? vérifier `grep -n "display.*sleep\|->sleep\|->wake" main/power/veille.c` ; si absents, c'est le hook qui les apporte)
- Modify: `main/CMakeLists.txt:263` (`power/veille_task.c` à côté de `power/veille.c`)
- Modify: `COMPORTEMENTS.md`, `docs/HARDWARE_SMOKE_TEST.md`, `CLAUDE.md`
- Test: `test_veille.c` existant (inchangé : `veille_niveau`, `veille_en_grace`)

**Interfaces:**
```c
/* veille_task.h */
typedef struct {
    const char *nom;              /* "radio", "ecran", "jauge" — pour le journal */
    void (*dormir)(void);         /* appelé AVANT le sommeil, dans l'ordre d'enregistrement */
    void (*reveiller)(void);      /* appelé APRÈS le réveil, dans l'ordre INVERSE */
} veille_hook_t;
#define VEILLE_HOOKS_MAX 4
void veille_hook_enregistrer(const veille_hook_t *h);   /* avant veille_task_start */
void veille_veto(veille_veto_t quoi, bool on);           /* thread-safe (portENTER_CRITICAL) */
void veille_task_start(void);                            /* une tâche "veille", prio 3, 4096, cœur 0 */
```
- Ordre des hooks au **sommeil** : radio (power-down) → écran (gel) ; au **réveil** : radio (power-up, ~5 ms, AVANT la capture qui émet) → écran. `veille_legere_entrer` conserve en dur la séquence matrice (deinit → armement → sleep → capture → recréation → réconciliation) : c'est le cœur, son ordre est documenté ligne à ligne et ne doit pas dépendre d'un enregistrement.
- La règle « droite + `tud_mounted()` → profond plutôt que léger » (`veille.c:315-341`) reste dans `veille_pas`, sous `#if CONFIG_KASE_HALF_LINK_TX`.

- [ ] **Step 1 : la tâche**

`main/power/veille_task.c` :
```c
/* Une seule tâche de veille pour les deux moitiés. Elle possède : l'horloge
 * d'inactivité (matrix_scan), le registre de vetos, les hooks sommeil/réveil,
 * le battement de coeur. Les modules n'ont plus à connaître la veille : ils
 * posent un veto (usb, lien, sync, test) et enregistrent au plus un hook. */
#include "veille_task.h"
#include "veille.h"
#include "veille_veto.h"
#include "cadence.h"
#include "matrix_scan.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_PM_PROFILING
#include "esp_pm.h"
#endif

static const char *TAG = "veille";
static veille_vetos_t s_vetos;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static veille_hook_t s_hooks[VEILLE_HOOKS_MAX];
static int s_n_hooks;

void veille_hook_enregistrer(const veille_hook_t *h)
{
    if (s_n_hooks < VEILLE_HOOKS_MAX) s_hooks[s_n_hooks++] = *h;
    else ESP_LOGE(TAG, "trop de hooks (%s)", h->nom);
}
void veille_hooks_dormir(void)    { for (int i = 0; i < s_n_hooks; i++) if (s_hooks[i].dormir) s_hooks[i].dormir(); }
void veille_hooks_reveiller(void) { for (int i = s_n_hooks - 1; i >= 0; i--) if (s_hooks[i].reveiller) s_hooks[i].reveiller(); }

void veille_veto(veille_veto_t quoi, bool on)
{
    portENTER_CRITICAL(&s_mux);
    veille_veto_poser(&s_vetos, quoi, on);
    portEXIT_CRITICAL(&s_mux);
}
static veille_vetos_t vetos_lire(void)
{
    portENTER_CRITICAL(&s_mux); veille_vetos_t v = s_vetos; portEXIT_CRITICAL(&s_mux); return v;
}

static void hb(uint32_t inactif_ms, const veille_vetos_t *v)
{
    uint32_t dodo_n = 0, dodo_ms = 0; char vb[24];
    veille_bilan(&dodo_n, &dodo_ms);
#if CONFIG_PM_PROFILING
    esp_pm_dump_locks(stdout);
#endif
    ESP_LOGW(TAG, "HB up=%lus inactif=%lus dormi=%lus/%lu vetos=%s%s",
             (unsigned long)(esp_timer_get_time() / 1000000), (unsigned long)(inactif_ms / 1000),
             (unsigned long)(dodo_ms / 1000), (unsigned long)dodo_n,
             veille_vetos_str(v, vb, sizeof vb), veille_hb_suffixe());
}

static void veille_task(void *arg)
{
    (void)arg;
    uint32_t dernier_hb = 0, dernier_refus = 0;
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t inactif = now - get_last_activity_time_ms();
        veille_vetos_t v = vetos_lire();
        if ((uint32_t)(now - dernier_hb) >= HB_PERIODE_MS) { dernier_hb = now; hb(inactif, &v); }
        if (veille_bloquee(&v) && inactif >= (uint32_t)CONFIG_KASE_VEILLE_LEGERE_S * 1000u
            && (uint32_t)(now - dernier_refus) >= 30000u) {
            char vb[24]; dernier_refus = now;
            ESP_LOGW(TAG, "veille REFUSEE depuis %lu s : vetos=%s", (unsigned long)(inactif / 1000), veille_vetos_str(&v, vb, sizeof vb));
        }
        veille_pas(inactif, veille_bloquee(&v));   /* peut bloquer des heures (light sleep) */
        vTaskDelay(pdMS_TO_TICKS(VEILLE_TICK_MS));
    }
}

void veille_task_start(void)
{
    xTaskCreatePinnedToCore(veille_task, "veille", 4096, NULL, 3, NULL, 0);
}
```
`cadence.h` : `#define VEILLE_TICK_MS 1000u` + `CADENCE_REPOS_OK(VEILLE_TICK_MS);`. `veille_hb_suffixe()` : fonction faible par rôle — gauche (`kbd_relay_tx.c`) renvoie `" route=RF relais=actif"`, droite (`half_link.c`) renvoie `" lien=0 batt=39 dV"` ; déclaration `const char *veille_hb_suffixe(void);` dans `veille_task.h`, implémentation `__attribute__((weak))` renvoyant `""` dans `veille_task.c`. **Latence** : le veto USB posé après une frappe ne retarde rien (la veille n'arrive qu'à 15 s) ; la seule chose qui attend le tick de 1 s est le HB.

- [ ] **Step 2 : poser les vetos aux sources**

- `pm_dfs.c` `usb_hote(bool monte)` : ajouter `#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD veille_veto(VEILLE_VETO_USB, monte); #endif`. ⚠ La gauche utilisait `tud_ready()` (retombe à l'autosuspend), pas `tud_mounted()`. `TINYUSB_EVENT_DETACHED` n'arrive pas toujours au débranchement à chaud sur S3 : garder en plus, dans `veille_task`, un rattrapage `#if CONFIG_KASE_DEVICE_ROLE_KEYBOARD veille_veto(VEILLE_VETO_USB, tud_ready()); #endif` en tête de boucle (1 s). La droite ne pose **pas** de veto USB (règle existante : elle dort branchée, en profond).
- `link_uart.c` `set_5v` : après `s_active = on;` → `veille_veto(VEILLE_VETO_LIEN, on);`.
- `kbd_relay_tx.c` : l. 193 `s_syncing = true;` → `+ veille_veto(VEILLE_VETO_SYNC, true);` ; l. 200 `s_syncing = false;` → `+ veille_veto(VEILLE_VETO_SYNC, false);`.
- `matrix_scan.c` : là où `matrix_test_mode` passe à vrai/faux (l. 219-233 et `keyboard_task.c:99-104`) → `veille_veto(VEILLE_VETO_TEST, matrix_test_mode);`.

- [ ] **Step 3 : enregistrer les hooks**

- `half_link_tx_init` (droite), après « TX pret » : `static const veille_hook_t h = { "radio", half_link_radio_sleep, half_link_radio_wake }; veille_hook_enregistrer(&h);`
- `kbd_relay_init` (gauche), fin : `static const veille_hook_t h = { "radio", kbd_relay_sleep_prepare, kbd_relay_wake_restore }; veille_hook_enregistrer(&h);`
- Écran (les deux) : dans `memlcd_backend.c` `memlcd_init` : `static const veille_hook_t h = { "ecran", memlcd_sleep, memlcd_wake }; veille_hook_enregistrer(&h);` — et retirer l'appel équivalent de `veille.c` s'il existe.
- Jauge : `batt_sense_init` : `{ "jauge", NULL, batt_sense_sample_now }`.
- `veille_legere_entrer` : remplacer les blocs `#if … half_link_radio_sleep / kbd_relay_sleep_prepare` par `veille_hooks_dormir();` et les blocs de réveil (radio + `batt_sense_sample_now`) par `veille_hooks_reveiller();` **avant** `matrix_wake_capture()` (la radio doit être debout avant la capture qui émet — l'ordre inverse des hooks le garantit si radio est enregistré avant écran ; la jauge en dernier n'a pas d'ordre).

- [ ] **Step 4 : supprimer les anciens évaluateurs et HB**

- `keyboard_task.c:196-218` : bloc `#if CONFIG_KASE_VEILLE { … veille_diag … veille_pas … }` → supprimer ; retirer les includes `veille.h`, `link_uart.h` devenus inutiles.
- `half_link.c:520-593` : supprimer le bloc `#if CONFIG_KASE_VEILLE` de la tâche de rafraîchissement (veille + HB + dumps PM). Il reste : `half_link_tx_update`, `half_link_batt_tick`, l'attente `tenu ? HALF_TX_TENU_MS : HALF_TX_REPOS_MS`.
- `main.c:85-125` : supprimer `cpu_time_logger_task` et `main/sys/cpu_time.c` s'il n'a plus d'appelant ; l. ~500 remplacer la création par rien. Dans les deux rôles, après l'init des modules (gauche : après `kbd_relay_init()` ; droite : après `matrix_setup()`) : `#if CONFIG_KASE_VEILLE veille_task_start(); #endif`.
- `veille.c` : supprimer `veille_diag` ; `veille_pas` reste publique (appelée par la tâche).

- [ ] **Step 5 : compiler, banc des deux moitiés**

7 boards verts. Banc, par moitié : HB `vetos=-` au repos, veille à 15 s, réveil sur touche avec capture, radio réveillée (frappe relayée juste après le réveil), écran gelé/revit ; gauche en USB → `vetos=usb`, jamais de veille ; TRRS 5 V → `vetos=lien` ; sync forcée (diverge.py) → `vetos=sync` pendant ~7 s. `light_sleep_counts` grimpe comme avant.

- [ ] **Step 6 : contrat, docs, commit**

`COMPORTEMENTS.md` : remplacer les lignes qui décrivent « la boucle clavier évalue la veille » / « la tâche de rafraîchissement de la droite porte la veille » par :
```
- [smoke:Éveil oisif] UNE tâche de veille (power/veille_task.c), identique sur
  les deux moitiés, possède l'inactivité, les vetos et le battement de coeur
  (« HB up= inactif= dormi= vetos=… » + suffixe de rôle). Un module qui a une
  raison d'empêcher la veille POSE UN VETO (usb — gauche seulement —, lien,
  sync, test) ; un module qui a quelque chose à endormir ENREGISTRE UN HOOK
  (radio, écran, jauge), appelés dans l'ordre au sommeil et en ordre inverse
  au réveil (radio debout AVANT la capture). Plus aucun module n'appelle la
  veille, la veille n'appelle plus aucun module par son nom.
- [test:test_veille_veto] Registre de vetos : état par nom, un veto posé
  bloque toute veille, levée sans effet s'il n'était pas posé ; noms pour le HB.
```
`docs/HARDWARE_SMOKE_TEST.md` : item « Une nuit sur batterie » → le HB se lit « `HB … dormi=X s/n vetos=-` » ; ajouter « gauche USB : `vetos=usb` et jamais de veille ». `CLAUDE.md` : dans le bloc « Éveil oisif dompté », une phrase : « la veille est une TÂCHE unique (`power/veille_task.c`) à vetos et hooks — ne jamais ré-évaluer la veille dans une tâche de module ».
Commit : `refactor(veille): une tâche unique aux deux moitiés — vetos déclarés (usb, lien, sync, test), hooks sommeil/réveil (radio, écran, jauge), un seul battement de coeur`.

---

### Task 8 : Diagnostic de banc derrière `KASE_VEILLE_DIAG`

**Gate.** Ne pas exécuter tant que la « première touche légère perdue sur la gauche après une longue pause » n'est pas expliquée : l'instrumentation sert encore. Quand c'est réglé :

**Files:**
- Modify: `main/Kconfig.projbuild` (nouvelle option `KASE_VEILLE_DIAG`, `default n`, `depends on KASE_VEILLE`)
- Modify: `main/power/veille.c` (chronos d'entrée/sortie, `lignes a la sortie`, `broches du reveil`, échelle de relecture 150 ms → sous `#if CONFIG_KASE_VEILLE_DIAG`)
- Modify: `main/input/matrix_scan.c:519-657` (dump des passes brutes sur capture vide → idem)
- Modify: `COMPORTEMENTS.md` (ligne `[smoke:Première touche après veille]` : ajouter « instrumentation sous `KASE_VEILLE_DIAG` »)

- [ ] Step 1 : Kconfig + `#if` autour des blocs listés (garder la relecture unique à 5 ms de `veille.c` : c'est un correctif, pas un diagnostic).
- [ ] Step 2 : 7 boards verts, banc gauche sans l'option (réveil propre) puis avec (mêmes journaux qu'avant).
- [ ] Step 3 : commit `chore(veille): instrumentation de banc sous KASE_VEILLE_DIAG`.

---

### Task 9 : Radio — `rf/radio_owner.c` (plan séparé)

Hors du périmètre de ce plan : c'est une session banc RF-critique (les trois pannes « mauvais canal en silence » sont là). À écrire comme plan dédié quand la prochaine intervention radio se présente, ou pour la REV2. Cible, pour que ce plan ne se perde pas :

- `rf/radio_owner.c` = **la puce et rien d'autre** : un `rf_radio_t`, un mutex, trois modes (`RADIO_PTX_DONGLE`, `RADIO_PRX_LIEN`, `RADIO_DORMANTE`), `radio_mode_set()`, le prêt du bus à l'écran (`rf_bus_lock`, aujourd'hui dupliqué `half_link.c:291/784` et `kbd_relay_tx.c:152`), la séquence power-down/power-up + réarmement CE, l'excursion `radio_excursion_tx(canal, adresse, trame)` unique.
- Politiques minces au-dessus : `half_tx.c` (droite : demi-matrice, STATUS, repli de cible `half_tx_target_step`), `left_relay.c` (gauche : brut au dongle, écoute USB de la droite réémise, réparation bornée), `keymap_pull.c` (sync par ACK payload — aujourd'hui 200 lignes dans `kbd_relay_tx.c:190-400`), `rf_pairing` (déjà séparé).
- Invariants à tester host : « un seul mode à la fois », « une excursion restaure le mode d'avant », « la FIFO est vidée AVANT une émission, jamais après ».
- Après Task 2, `kbd_relay_tx.c` fait ~650 lignes et `half_link.c` ~600 : le découpage part de là.

---

## Self-review

- **Couverture** : les cinq propositions de la revue sont couvertes (1 → Tasks 1-2 ; 2 → Tasks 3-5 ; 3 → Tasks 6-7 ; 5 → Task 8 ; 4 → Task 9 déclarée hors périmètre avec sa cible).
- **Placeholders** : Task 9 est explicitement un plan séparé, pas un TBD ; les steps de code sont donnés en clair partout ailleurs.
- **Cohérence des noms** : `veille_veto()` (fonction, Task 7) vs `veille_veto_poser()` (pure, Task 6) ; `veille_hooks_dormir/reveiller` déclarés dans `veille_task.h` et appelés depuis `veille.c` ; `VEILLE_TICK_MS`, `HB_PERIODE_MS`, `LINK_REPOS_MS`, `MEMLCD_DROITE_PERIODE_MS` définis en Task 3 et modifiés en Tasks 4-5-7.
- **Risque connu** : Task 7 change la source du veto USB de la gauche (`tud_ready()` sondé → événement TinyUSB + rattrapage 1 s) ; le smoke « gauche en USB ne dort jamais » le vérifie. Une touche TENUE plus de 15 s sans autre changement n'est pas un veto aujourd'hui non plus (la carte s'endort et se réveille aussitôt sur la ligne tenue) — comportement existant, non traité ici, à noter si observé.
