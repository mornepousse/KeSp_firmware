# Keycodes modifiés — « cette touche envoie Shift+1 »

Brief pour l'agent firmware. Rédigé le 2026-09-11 depuis le contrôleur, après
recherche dans ce dépôt. Cible : `KeSp_firmware`, branche courante.

## Le besoin

Sur un clavier sans rangée de chiffres, `!@#$%^&*()` ne sont accessibles que par
Shift + chiffre. On veut les poser **directement** sur des touches — une couche
symboles, un keycode par symbole, une pression. C'est le besoin de base de tout
clavier 40 % ; QMK l'appelle `KC_EXLM`, ZMK `EXCL`.

**HID ne l'offre pas.** Le protocole transmet des positions de touches, pas des
caractères : `1/!` est une seule touche (`0x1E`), et c'est l'OS qui produit `1`
ou `!` selon l'état de Shift. Il n'existe aucun code HID pour `!` seul. Le
firmware doit donc enfoncer Shift et la touche **ensemble dans le même rapport**.
C'est ce que fait `KC_EXLM` chez QMK : sous le capot c'est `LSFT(KC_1)`, un u16
qui encode le modificateur dans l'octet haut et la touche dans l'octet bas.

## Ce que ce firmware a — et n'a pas

Carte de l'espace u16, `docs/KEYCODE_MAP.md:6-33` et `main/input/key_definitions.h` :

| Plage | Type | Encodage | Source |
|---|---|---|---|
| `0x4000-0x4FFF` | Layer-Tap (LT) | `0x4000 \| (layer << 8) \| kc` | `key_definitions.h:431-433` |
| `0x5000-0x5FFF` | Mod-Tap (MT) | `0x5000 \| (mod << 8) \| kc` | `key_definitions.h:438-440` |
| `0x6000-0x6FFF` | Tap Dance (TD) | `0x6000 \| (index << 8)` | `key_definitions.h:445` |
| `0x7000-0x7FFF` | Layer-Mod (LM) | `0x7000 \| (mods << 4) \| layer` | `key_definitions.h:451-453` |
| **`0x8000-0x8FFF`** | **libre** | | |

Aucune plage ne dit « modificateur + touche en une action ». Les voisines ne
conviennent pas :

- **MT** : hold = mod, tap = touche. Un tap sur `MT(Shift, 1)` envoie `1`, pas `!`.
- **OSM** (`0x30xx`) : Shift collant puis `1` — deux pressions.
- **Macro** (`0x15xx-0x28xx`) : 20 slots pour tout le clavier, une macro par symbole.
- **Key Override** (`0x3Dxx`) : « quand Shift+X est pressé, envoie Y » — l'inverse.

**Trace d'intention** : `docs/superpowers/plans/2026-05-11-dongle-plan-1-bringup.md:316-322`
contient une keymap qui utilise `K_TILD, K_EXLM, K_AT, K_HASH, K_DLR, K_PERC,
K_CIRC, K_AMPR, K_ASTR, K_LPRN, K_RPRN, K_UNDS, K_PLUS, K_LCBR, K_RCBR, K_PIPE`
— seize noms de style QMK. **Aucun n'est défini** dans `key_definitions.h`
(vérifié le 2026-09-11). Quelqu'un a écrit une keymap en supposant qu'ils
existaient. Ils n'ont jamais existé dans ce dépôt (`git log -S'K_EXLM'` ne
remonte que ce plan ; `git log -S'K_LSFT('` est vide).

## L'architecture à respecter : les mods ne sont pas des keycodes

Commit `bffdf4ec` (audit M7), à lire en entier avant de coder :

> le report HID a un octet modifier SÉPARÉ des 6 keycodes. Or key_processor
> empilait les mods comme keycodes 0xE0-0xE7 dans le tableau partagé keycodes[6],
> où ils volaient une slot. Si les 6 slots étaient pleins de vraies touches, le mod
> ne trouvait pas de place → PERDU silencieusement.

Le chemin actuel, `main/input/key_processor.c` :

- `:217` `process_advanced_key()` — le dispatch par plage (`K_IS_LT/MT/OSM` →
  tap-hold, `K_IS_TD`, `K_IS_OSL`, `K_CAPS_WORD`, `K_REPEAT`…). **C'est là que
  `K_IS_MK` se branche.**
- `:430` Step 5 — `extra_mods = th_mods | osm_mods | lm_active_mods | macro_hold_mods`,
  puis overrides. **C'est là que le mod d'un MK tenu doit s'OR-er.**
- `:445` `report_mods = extra_mods` → `key_processor_report_mods()` (`:194`) →
  `hid_report.c:141` `modifier |= key_processor_report_mods()`.

Précédent direct : `key_features.c:57`, `caps_word_process` fait
`*modifier |= MOD_LSFT` sur les lettres — un Shift synthétique porté par l'octet
modifier, exactement le mécanisme voulu.

Masques : `MOD_LCTL 0x01`, `MOD_LSFT 0x02`, `MOD_LALT 0x04`, `MOD_LGUI 0x08`
(`key_definitions.h:372-375`).

## Design

### Encodage

```c
/* Modified Key (MK) — 0x8000-0x8FFF : une pression envoie mod + touche ensemble.
 * Pas un tap-hold : immédiat, sans timer. Le nibble mod est limité aux quatre
 * mods GAUCHES (LCTL/LSFT/LALT/LGUI), même contrainte que MT (0x5xxx). */
#define K_MK_BASE                    0x8000
#define K_MK(mod, kc)               (K_MK_BASE | (((mod) & 0x0F) << 8) | ((kc) & 0xFF))
#define K_IS_MK(kc)                 (((kc) & 0xF000) == K_MK_BASE)
#define K_MK_MOD(kc)                (((kc) >> 8) & 0x0F)
#define K_MK_KEY(kc)                ((kc) & 0xFF)
```

Même forme que MT — un lecteur qui connaît `K_MT` lit `K_MK` sans apprendre.

Référence QMK, pour comparaison (de source : `quantum/keycodes.h`, stable) :
`QK_LCTL 0x0100`, `QK_LSFT 0x0200`, `QK_LALT 0x0400`, `QK_LGUI 0x0800`,
`LSFT(kc) = QK_LSFT | kc`, `KC_EXLM = S(KC_1)`. QMK met les mods dans l'octet haut
de la plage basse ; ici cette plage est prise par MO/TO/macros, d'où `0x8xxx`.

### Alias

Les seize noms du plan de mai, plus le complément standard, tous en
`K_MK(MOD_LSFT, …)` et pour une disposition **US** :

```
K_EXLM  Shift+1   K_AT    Shift+2   K_HASH  Shift+3   K_DLR   Shift+4
K_PERC  Shift+5   K_CIRC  Shift+6   K_AMPR  Shift+7   K_ASTR  Shift+8
K_LPRN  Shift+9   K_RPRN  Shift+0
K_UNDS  Shift+-   K_PLUS  Shift+=   K_LCBR  Shift+[   K_RCBR  Shift+]
K_PIPE  Shift+\   K_TILD  Shift+`   K_COLN  Shift+;   K_DQUO  Shift+'
K_LT    Shift+,   K_GT    Shift+.   K_QUES  Shift+/
```

Attention : `K_LT` **collisionne** avec `K_LT(layer, kc)` (Layer-Tap). Nommer
`K_LABK` / `K_RABK` (angle brackets, comme QMK) — pas `K_LT` / `K_GT`.

### Sémantique

- **Pression** : la touche de base (`K_MK_KEY`) entre dans `keycodes[]` comme une
  touche normale ; le mod (`K_MK_MOD`) s'OR-e dans `extra_mods` **tant que la
  touche est tenue**.
- **Relâchement** : les deux disparaissent. Pas de mod collant.
- **Pas de timer, pas de tap-hold** : MK n'entre pas dans `tap_hold.c`. C'est ce
  qui le distingue de MT.

**Limite inhérente à HID, à documenter, pas à contourner** : l'octet modifier
est global au rapport. Tenir `K_EXLM` (Shift+1) et presser `a` produit
`Shift+1+A` = `!A`. QMK a le même comportement. Un MK n'est pas une touche à
tenir pour du rollover.

## Décisions à prendre — et à écrire dans le code

Chacune a une réponse recommandée ; l'agent tranche et **documente le choix dans
un commentaire**, il ne choisit pas en silence.

1. **Auto-shift** (`key_features.c`) : un MK déjà shifté ne doit pas être
   re-shifté ni retardé. Recommandé : auto-shift ignore les keycodes MK.
2. **Caps Word** : `K_EXLM` sous caps word → `caps_word_process` ajoute Shift
   sur les lettres seulement ; `0x1E` n'est pas une lettre → sans effet. OK tel
   quel, à confirmer par un test.
3. **Key Override** (`bf616538` : les overrides voient les mods physiques) : le
   Shift synthétique d'un MK doit-il déclencher un override « Shift+X → Y » ?
   Recommandé : **non** — un MK est une touche finale, pas une combinaison à
   réinterpréter. Sinon `K_EXLM` pourrait se faire réécrire par un override sur
   Shift+1, ce qui surprendrait.
4. **Repeat Key** (`repeat_key_record`, `key_processor.c:437`) : après `!`,
   Repeat doit reproduire `!`, pas `1`. Recommandé : enregistrer le keycode MK
   complet (u16), pas la touche de base. Aujourd'hui
   `repeat_key_record(uint8_t keycode)` (`key_features.h:45`) ne prend qu'un
   octet — il faudra élargir à `uint16_t` ou stocker le mod à part.
5. **Imbrication** : `K_MT(mod, K_MK(…))` est impossible — le champ kc de MT fait
   8 bits. Documenter la limite dans `KEYCODE_MAP.md`, ne pas tenter de l'étendre.
6. **OSM + MK** : OSM Shift armé puis `K_EXLM` → `osm_mods | mk_mods` = Shift,
   consommé. Sans effet visible, OK. Un test le fige.

## Protocole CDC : rien à changer

`SETKEY` porte un `u16` (`docs/CDC_BINARY_PROTOCOL.md`, section Keymap) ;
`0x821E` passe tel quel. Le contrôleur devra apprendre à **afficher** `0x821E`
comme `!` et à **lister** les symboles dans son sélecteur — travail séparé, côté
`KeSp_controller` (`src/protocol/keycode.rs::decode_keycode`, `src/key_selector.rs`).
Ne pas le faire ici.

## Tests — TDD obligatoire, et ils doivent mordre

Harnais hôte existant (`test/`, 54 fichiers, `TEST_ASSERT`). Test rouge d'abord. Pour chaque
test, l'implémenteur vérifie qu'il **échouerait** contre les implémentations
fausses ci-dessous — dix tests de ce projet-frère ont été livrés incapables
d'échouer, tous par des valeurs attendues qui coïncidaient avec l'erreur.

| Test | Ce qu'il fige |
|---|---|
| press `K_EXLM` → report `modifier & MOD_LSFT` **et** `keycodes[]` contient `0x1E` | la sémantique de base |
| release → `modifier` sans `MOD_LSFT`, `keycodes[]` sans `0x1E` | pas de mod collant |
| 6 touches normales + `K_EXLM` → Shift dans `modifier`, **aucun** `0xE1` dans `keycodes[]` | la régression M7 (`bffdf4ec`) — le mod ne vole pas de slot |
| `K_MK(MOD_LCTL, K_C)` → `modifier & MOD_LCTL`, `keycodes[]` contient `K_C` | la plage n'est pas que Shift |
| `K_EXLM` tenu + `K_A` pressé → `Shift` + `0x1E` + `0x04` dans le même report | la limite HID, documentée, pas cachée |
| auto-shift actif + `K_EXLM` → un seul Shift, aucun délai | décision 1 |
| `K_IS_MK(0x7FFF) == false`, `K_IS_MK(0x9000) == false`, `K_IS_MK(0x8000) == true` | les bornes de la plage |

**Implémentations fausses à éprouver** (nommer laquelle chaque test attrape) :

- une qui pousse `0xE1` (LShift) dans `keycodes[]` au lieu de l'octet modifier
  — c'est le bug M7, et c'est l'erreur la plus naturelle à commettre
- une qui laisse le mod dans `extra_mods` après relâchement (mod collant)
- une qui route MK vers `tap_hold_on_press` comme MT (le tap enverrait `1`)
- une dont `K_IS_MK` teste `& 0x8000` au lieu de `& 0xF000 == 0x8000` (attrape
  tout ce qui est ≥ 0x8000)

Le ratchet `.tripwire-testcount` monte et se committe avec.

## Documentation

- `docs/KEYCODE_MAP.md` : une ligne dans le tableau d'encodage, `0x8000-0x8FFF`,
  et un paragraphe « Modified Key » dans Feature Behaviors avec la limite HID.
- `key_definitions.h` : le bloc de macros avec son commentaire, les alias
  groupés sous un titre.

## Hors périmètre

- Le contrôleur (affichage et sélecteur). Séparé.
- Les mods droits (RSHIFT…) — le nibble de 4 bits ne les porte pas, comme MT.
- Une table de symboles non-US. Les alias sont US ; une autre disposition se
  traite par les mêmes `K_MK(MOD_LSFT, kc)` avec d'autres kc.
