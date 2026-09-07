# KaSe firmware — Claude Code instructions

Firmware ESP32-S3 pour clavier split-ergo custom (KaSe V1/V2/V2D).
Distribué via binaires GitLab Releases. Build via ESP-IDF 5.5.

## Repo
- **Référence** : https://github.com/mornepousse/KeSp_firmware (remote `github`)
  — c'est le dépôt qui fait foi.
- **Miroir** : https://gitlab.com/harrael/KeSp_firmware (remote `origin`).
  GitLab pousse vers GitHub par un miroir côté serveur, vérifié le 2026-09-05 :
  un `git push origin` suffit, GitHub suit en quelques secondes. Inutile de
  pousser sur `github` en plus — le second push perd la course contre le miroir
  et se fait rejeter.
  ⚠ Le miroir ne peut pas rembobiner GitHub. Si `github/<branche>` prend de
  l'avance par un push direct, le miroir se bloque **en silence** sur cette
  branche : c'est arrivé sur `main`, resté deux commits en arrière côté GitLab.
  Réalignement par avance rapide, pas par force.
- **Local** : `~/Documents/GitHub/KeSp_firmware-gitlab/`
- **Related** : https://gitlab.com/harrael/KeSp_controller (remapping software)

## Versioning

Source de vérité : tag git `vX.Y.Z`. Lu par ESP-IDF via `git describe --tags`
au build. Pas de fichier VERSION.

Pour cut une release :
1. `git commit` les changements
2. `git tag vX.Y.Z`
3. `git push && git push --tags`
4. Build les 7 boards + merge full binaries
5. `glab release create vX.Y.Z <files...>`

Entre deux releases : `cheni vX.Y.Z-N-gHASH-dirty` via `git describe`.

## Périmètre — anciennes halves retirées, Niphargus à venir

Les moitiés split de première génération (`kase_half_left` / `kase_half_right`,
e-ink SSD1681, ESP-NOW) ont été retirées au commit `c107df77` : elles visaient un
matériel qui n'existe plus.

Le clavier split est redessiné sous le nom **Niphargus** — matériel dans
`~/Documents/GitHub/rili` (KiCad), **firmware ici**. Deux moitiés ESP32-S3 +
nRF24L01+, matrice 4×7, trackpad Azoteq TPS43 à gauche, Sharp Memory LCD à
droite, lien filaire TRRS. Pas de WiFi ni de BLE : config et mises à jour par USB.

Architecture actée : la **moitié gauche est le maître en toutes circonstances**
(elle porte le seul moteur keymap et le trackpad) ; la droite est un scanner. Le
dongle garde sa radio 1 pour le clavier — la radio 2 appartient à la souris
**Conchodytes** (`~/Documents/GitHub/Conchodytes`).

Design complet : `docs/superpowers/specs/2026-08-19-niphargus-firmware-design.md`.

**État au 2026-09-06** — les deux moitiés fonctionnent et tapent ensemble :
brochage vérifié à la netlist sur les deux, lien radio inter-moitiés prouvé
(canal 0x4F, adresse KaSe.03), fusion des keymaps, relais vers le dongle. Le
risque R1 du design est levé (0 perte, 0,4 retransmission/paquet).
**Alimentation batterie réparée le 2026-09-07** : les deux moitiés fonctionnent
en autonomie, sans aucun câble, et tapent ensemble par radio. C'est le mode
nominal du clavier.
**Full RF le 2026-09-07** : la gauche écoute la droite en PRX sur le canal du
lien ET relaie le HID fini au dongle, par excursion PRX→PTX→PRX. Le routage
reste USB-first — USB branché → HID par USB, sur batterie → radio.

⚠ **Une puce, un propriétaire.** La radio de chaque moitié appartient à
`half_link` seul ; `kbd_relay_tx` la lui emprunte via `half_link_excursion_tx`.
Deux modules qui l'initialisaient chacun de leur côté ont fait écouter la gauche
sur le mauvais canal **trois fois**, toujours en silence. `rf_driver` refuse
désormais de le taire (`rf_claim_chip`, revendication par broche CSN), et un
mutex sérialise la tâche d'écoute et l'excursion — un propriétaire unique ne
suffit pas s'il a deux bouches.

Restent ouverts : B7 l'énergie (sommeil < 50 µA, scan RTC, réveil EXT1) et le
driver du trackpad.
Brochage : `docs/NIPHARGUS_V2_HARDWARE.md` (source de vérité, vérifié à la netlist).

## Board variants

- **V1** : round SPI display (GC9A01), LED strip, pinout historique
- **V2** : OLED I2C (SSD1306), pinout production
- **V2D** : V2 + overrides GPIO pour prototype (COLS7/8 sur GPIO21/4 au lieu de UART0)
- **dongle** : récepteur USB, deux radios nRF24 (slot 1 clavier, slot 2 souris),
  ni matrice ni moteur keymap — il relaie du HID déjà fini
- **niphar_left** : moitié GAUCHE du Niphargus, le maître. Matrice 4×7, seul
  moteur keymap du clavier, `KEYMAP_COLS = 14` pour couvrir les deux moitiés,
  trackpad (driver à écrire), relais vers le dongle
- **niphar_right** : moitié DROITE, un scanner. Matrice 4×7 avec une table de
  brochage DIFFÉRENTE de la gauche (permutations de routage), émission de sa
  demi-matrice par radio, ni keymap ni HID. Ses colonnes sont **en miroir** de
  celles de la gauche (même PCB retourné) : la conversion est au maître, via
  `BOARD_REMOTE_COLS_MIRRORED` et `half_col_to_keymap()`. Elle émet sur
  changement, et **réaffirme les maintiens toutes les 100 ms** — le callback de
  scan ne se déclenchant que sur changement, une touche tenue ne produirait plus
  rien et la gauche la relâcherait au bout de 250 ms
- **conchodytes** : souris (PMW3389), slot 2 du dongle

Chaque variant sous `boards/<name>/` avec `board.h`, `board_keymap.c`,
`board_layout.c`. V2D inherit de V2 via `#include "../kase_v2/board.h"`.

## Build system

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_kase_v1       -DBOARD=kase_v1       -DSDKCONFIG=build_kase_v1/sdkconfig       build
idf.py -B build_kase_v2       -DBOARD=kase_v2       -DSDKCONFIG=build_kase_v2/sdkconfig       build
idf.py -B build_kase_v2_debug -DBOARD=kase_v2_debug -DSDKCONFIG=build_kase_v2_debug/sdkconfig build
```

Paramètre CMake : `-DBOARD=<name>` (pas `-DBOARD_VARIANT`). Chaque board a son
propre dossier build (`build_kase_<name>/`) **et son propre `sdkconfig`** via
`-DSDKCONFIG=build_kase_<name>/sdkconfig` — c'est ce qui évite la fuite de
config entre boards (cf. Workflow anti-régression). 7 boards au total : V1, V2,
V2D, dongle, niphar_left, niphar_right, conchodytes. Pour tout vérifier d'un coup :
`./scripts/check.sh`.

**ccache** : `check.sh` exporte `IDF_CCACHE_ENABLE=1` — les 7 boards partagent
la plupart des composants, donc après le 1er board les suivants réutilisent les
objets compilés (gros gain sur le build full + pre-push). Pour tes builds
interactifs, ajoute `export IDF_CCACHE_ENABLE=1` à ton shell (ou source-le avant
`idf.py`). Stats : `ccache -s`.

**Important** : avec `-DSDKCONFIG=build_kase_<name>/sdkconfig`, chaque board a
son sdkconfig isolé dans son dossier build — plus de fuite de config entre
boards. Le `sdkconfig` historique à la racine reste celui d'un build legacy
sans `-DSDKCONFIG` ; ne pas mélanger les deux modes sur un même board.

## Flash

**App only** (NVS préservée) :
```bash
idf.py -B build_v<N> -p /dev/ttyUSB0 flash
# ou: esptool.py write_flash 0x20000 build_v<N>/KeSp.bin
```

**Full flash** (erase + bootloader + partition table + app + storage) :
```bash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 erase_flash
esptool.py --chip esp32s3 -p /dev/ttyUSB0 write_flash 0x0 kase_<board>_full.bin
```

Requis après changement de partition table (ex: NVS resize).

## Partition table

`partitions.csv` — 16MB flash :
- `nvs`      : 0x9000  + 0x10000 (64KB) — config, keymaps, stats
- `otadata`  : 0x19000 + 0x2000
- `phy_init` : 0x1B000 + 0x1000
- `factory`  : 0x20000 + 0x200000 (2MB)
- `ota_0`    : 0x220000 + 0x200000 (2MB)
- `storage`  : 0x420000 + 0xF0000 (LittleFS)

NVS DOIT rester à 64KB — stocke ~21KB de bigrams + keymaps + macros + etc.
Ne pas réduire sans retirer les bigrams d'abord.

## Architecture

```
main/
├── main.c                # app_main, safe boot, task orchestration
├── comm/
│   ├── cdc/              # Binary protocol only (KS/KR frames, CRC-8)
│   │   ├── cdc_acm_com.c        # USB CDC dispatch
│   │   ├── cdc_binary_protocol.c # Frame parser, CRC
│   │   ├── cdc_binary_cmds.c    # All command handlers
│   │   └── cdc_ota.c            # OTA binary helpers
│   ├── rf/               # nRF24 — relais dongle, lien inter-moitiés
│   │   ├── rf_driver.c          # SPI + ESB, registres nRF24
│   │   ├── rf_packet.h          # trames + géométrie de demi-matrice (4×7)
│   │   ├── rf_slot.h            # slots dongle + PLAN DE CANAUX 2,4 GHz
│   │   ├── kbd_relay_tx.c       # HID gauche → dongle (KBD_WIRELESS)
│   │   ├── half_link.c          # lien droite → gauche (B3) + fusion
│   │   └── rf_probe.c           # diagnostic de banc (NRF_PROBE), test de lignes
│   ├── ble/              # Bluetooth LE HID
│   │   └── hid_bluetooth_manager.c
│   ├── usb/              # USB HID + CDC TinyUSB init
│   │   └── usb_hid.c
│   └── hid_transport.c   # USB/BLE routing (usb_bl_state)
├── input/
│   ├── matrix_scan.c     # keyboard_button driver wrapper
│   ├── keyboard_task.c   # Main scan loop
│   ├── key_processor.c   # Keycode decoding, layers, advanced
│   ├── key_features.c    # OSM, OSL, caps_word, repeat, leader, etc.
│   ├── tap_hold.c tap_dance.c combo.c leader.c
│   ├── hid_report.c      # HID queue + sender task
│   └── keymap.c key_stats.c
├── display/
│   ├── display_backend.h # vtable for OLED/round
│   ├── status_display.c  # Coordinator
│   ├── oled/             # I2C OLED (V2/V2D)
│   └── round/            # SPI GC9A01 (V1)
└── led/                  # WS2812 strip anim (V1 only)

boards/
├── kase_v1/   kase_v2/   kase_v2_debug/   kase_dongle/
├── niphar_left/   niphar_right/   # Niphargus split, phase 1 (pas de carte fabriquée)
└── kase_layout.inc  # Layout JSON shared V2/V2D
```

## CDC protocol — binary only, no ASCII

Frame format KS (request) / KR (response) avec CRC-8 :
```
KS: [0x4B][0x53][cmd:u8][len:u16 LE][payload...][crc8]
KR: [0x4B][0x52][cmd:u8][status:u8][len:u16 LE][payload...][crc8]
```

Voir `docs/CDC_BINARY_PROTOCOL.md` pour la doc complète et
`main/comm/cdc/cdc_binary_protocol.h` pour les IDs (KS_CMD_*).

**Jamais d'ajout de commande ASCII** — le protocole texte a été retiré en v3.7.

## Keycodes (16-bit)

Encoding dans `main/input/key_definitions.h`. Ranges :
- `0x00-0xFF` : HID standard
- `0x0100-0x0A00` : MO(layer)
- `0x0B00-0x1400` : TO(layer)
- `0x1500-0x2800` : Macros
- `0x2900-0x2F00` : Bluetooth actions
- `0x3000-0x3DFF` : OSM, OSL, CapsWord, Repeat, Leader, Tama, GESC, etc.
- `0x4000-0x4FFF` : LT(layer, kc)
- `0x5000-0x5FFF` : MT(mod, kc)
- `0x6000-0x6FFF` : TD(index)
- `0x7000-0x7FFF` : LM(layer, mods)

## NVS — données persistées

Namespace : `"storage"` (défini `STORAGE_NAMESPACE`).
Clés :
- `keymaps`, `layout_names` — ⚠ la taille du blob suit `KEYMAP_COLS`, qui vaut
  14 sur la moitié gauche du Niphargus contre 7 ailleurs. `load_keymaps` refuse
  un blob de taille différente et garde les défauts compile-time : un changement
  de dimension invalide donc les keymaps stockées, avec un avertissement.
- `macros`
- `key_stats`, `key_stats_tot`, `bigram_stats`, `bigram_total`
- `td_configs`, `td_count`
- `combo_cfg`, `combo_cnt`
- `leader_cfg`, `leader_cnt`
- `ko_cfg`, `ko_cnt`
- `bt_slots`, `bt_active`, `bt_enabled`

**Jamais** d'erase NVS au boot sans raison explicite (safe mode préserve
les données depuis v3.7.8).

## Safe boot

RTC memory tracking du nombre de boots consécutifs (`BOOT_CRASH_MAGIC`).
Si > 3 → `safe_mode = true` : skip display/BLE/NVS config loads. USB HID
basique reste fonctionnel. NVS NON effacée.

## Conventions C

- **Pas de `malloc` dans les hot paths** (scan, HID send, callbacks ISR).
  Buffers statiques ou pile.
- **`IRAM_ATTR`** pour les ISR / callbacks gptimer.
- **Pas de `ESP_LOGI`** dans les callbacks de scan matrice (trop lent).
  Utiliser `ESP_LOGD` avec niveau défini à NONE en prod.
- **Mutex LVGL** : tout accès LVGL doit être entouré de
  `lvgl_port_lock()` / `lvgl_port_unlock()`.
- **`lv_obj_is_valid()`** avant d'accéder à un objet LVGL après un
  potentiel `display_clear_screen()`.
- **NVS writes** via `nvs_save_blob_with_total()` pour éviter les
  corruptions si la struct change.

## Tests

Tests host-side dans `test/` (CMake standalone). Pas de tests embedded
sur le target. Exécution : `cd test/build && ./test_runner`.

Les tests doivent être parallel-safe : pas d'état global muté, pas de
chemins temp partagés. Mocks NVS via fake implementations dans le test.

## Workflow anti-régression (OBLIGATOIRE)

Source unique de vérité : `scripts/check.sh` (scaffold tripwire v0.13.0 ;
`--host-only`/`--board` sont des alias conservés de `--fast`/`--variant`,
déclarés dans `.tripwire-divergences`).
- `./scripts/check.sh --fast` — tests host CMake (~secondes)
- `./scripts/check.sh --variant <name>` — fast + build d'un board
- `./scripts/check.sh` — fast + les 7 boards (sdkconfig isolé par board)
- Skip-si-déjà-vert : état inchangé depuis le dernier vert → sortie immédiate ;
  `--force` pour relancer quand même.
- Sur rouge : le détail de la commande fautive est dans
  `.git/tripwire/last-fail.log` — le lire au lieu de relancer le build.
- **Sans `idf.py` dans le PATH** (hors devshell Nix), la phase de build se
  **saute en l'annonçant** au lieu de rendre rouge : un rouge qui veut dire
  « toolchain absente » est indiscernable d'un rouge qui veut dire « code
  cassé », et finit par ne plus être lu. Pour un check complet, lancer
  `./scripts/check.sh` dans le devshell.

**Activation des hooks git (une fois par clone)** :
```bash
./scripts/install-hooks.sh   # ou: git config core.hooksPath scripts/hooks
```
`pre-push` lance le check complet et bloque le push si rouge. WIP : `git push --no-verify`.

**Hooks Claude Code** (`.claude/settings.json`, automatiques). Échelle de
gravité : **pendant → informe, à la conclusion → bloque, au push → bloque.**
- `PostToolUse` sur édition de `.c/.h` dans `main/`, `boards/`, `test/` →
  `check.sh --fast`, en **avis non bloquant**. Il signale le rouge sans
  interrompre : la norme TDD impose d'écrire l'assertion rouge AVANT
  l'implémentation, et bloquer là ferait sonner l'alarme à chaque pas correct.
  Un avis n'est pas à ignorer pour autant.
- `Stop` → `check.sh --fast` et il **bloque** : on ne conclut pas un tour sur du
  rouge. Le build des 7 boards n'est PAS relancé à chaque fin de tour, il reste
  garanti au pre-push.
- `pre-push` → check complet, **bloquant**.

Un rouge de Stop ou de pre-push ne s'ignore jamais et ne se contourne pas par
`--no-verify` sans raison écrite.

Board courant (lu par `cc_session_start.sh`) : `echo kase_v1 > .kase-board`.

**Divergences déclarées** : `.tripwire-divergences` (committé) liste les écarts
assumés au scaffold standard. Une ligne `fichier<TAB>motif<TAB>pourquoi` ;
`check.sh` rend rouge la disparition d'un motif déclaré. Le fichier hôte doit
être **suivi par git** : un fichier gitignoré ne change pas l'empreinte du
skip-si-déjà-vert, donc sa perte peut passer sous un « déjà vert — skip ».
**Limite** : un écart non déclaré n'est protégé par rien et le prochain
re-scaffold l'effacera — toute divergence délibérée se déclare au moment où on
l'introduit.

**Jamais** builder deux boards dans le même `build/` avec le `sdkconfig` racine
(fuite de config). Toujours `-B build_<board> -DSDKCONFIG=build_<board>/sdkconfig`.

### Norme TDD — nouvelle logique pure
Toute nouvelle fonction de logique pure (keymap, layers, combo, tap-hold,
parsing CDC, encoding keycodes…) : test host écrit **d'abord**, ajouté à
`test/CMakeLists.txt` + déclaré dans `test/test_main.c`. Le test doit être rouge
avant l'implémentation, vert après, et parallel-safe. Invoquer l'agent
`kase-test-author`.

### Économie de modèles (subagents)
Le pipeline check.sh permet de descendre en gamme SANS risque d'hallucination,
mais seulement là où un oracle rattrape l'erreur :
- **Modèle économique (haiku) OK** : transcription de code déjà spécifié,
  refactors mécaniques, extraction citée (`fichier:ligne` obligatoire) — le
  check, la compilation ou le recoupement des citations attrapent la dérive.
- **Jamais en dessous de sonnet** : review, audit, debug, **et l'écriture
  d'assertions de test** — une assertion tautologique ou un verdict halluciné
  passent l'oracle mécanique au vert. Le jugement ne descend pas en gamme.
- Toute tâche économique DOIT finir par `./scripts/check.sh --fast` vert, et
  un test rewiré/écrit DOIT prouver qu'il mord (bug transitoire → rouge → revert).

### Quand invoquer les agents kase-*
- `kase-firmware-debugger` → backtrace / boot loop / crash.
- `kase-test-author` → ajout de logique pure (cf. norme TDD).
- `kase-code-reviewer` → avant un merge / release.
Les autres (`cdc-protocol`, `board-variant`, `release-manager`, `maintainer`,
`security-auditor`) : à la demande ponctuelle.

### Avant un merge vers main / release
Dérouler `docs/HARDWARE_SMOKE_TEST.md` sur les boards concernés.

## Dépendances ESP-IDF

Managed via `main/idf_component.yml` :
- `espressif/esp_tinyusb`
- `lvgl/lvgl: ^8`
- `espressif/esp_lvgl_port`
- `espressif/esp_lcd_gc9a01`
- `joltwallet/littlefs`
- `espressif/keyboard_button` (local dans `components/`)
- `espressif/led_strip`

Lock : `dependencies.lock` (tracké git). Pour mettre à jour :
```bash
rm dependencies.lock && rm -rf managed_components/
idf.py reconfigure
```

## Hardware specifics

**USB** : ESP32-S3 OTG Full-Speed only (12 Mbps, max packet 64 bytes).
Pas de High-Speed possible.

**Console UART désactivée** (`CONFIG_ESP_CONSOLE_NONE=y`) — libère
GPIO43/44/16 pour le scan matrice sur V2 (qui utilise UART0).

**GPIO reset** : `matrix_setup()` fait `gpio_reset_pin()` sur toutes les
pins matrice pour détacher les fonctions bootloader (UART0, SPI flash
secondaire).

## Release workflow

1. Bump version via tag git `vX.Y.Z`
2. `./scripts/check.sh` doit être vert (les 7 boards build)
3. Merge binaries avec `esptool.py merge_bin` pour les `_full.bin`
4. `glab release create vX.Y.Z <files...>` (app + full)

Voir `docs/` pour protocoles et keycodes détaillés.
