# Design — Filet anti-régression KaSe

Date : 2026-06-02
Statut : approuvé (brainstorming) → à transformer en plan d'implémentation

## Problème

Sur le projet KaSe, « chaque ajout casse un truc ». Diagnostic affiné avec
l'utilisateur :

- **Régressions silencieuses** : une feature qui marchait remarche plus, et on
  s'en aperçoit des jours après. Aucun moyen de savoir *quel commit* a cassé.
- Nature double : **logique pure** (testable host mais non couverte) **et**
  **hardware/runtime** (RF, eink, BLE — intestable host, faut la board).
- Workflow actuel : tests + build lancés **à la main, quand on y pense**. Pas
  systématique, pas de garde-fou automatique.
- 8 agents `kase-*` existent mais **ne sont jamais utilisés**.

### Constat clé

Le problème n'est **pas** un manque d'outils : tests host (1047 passent), CI,
agents existent déjà. Le problème est que **rien ne se déclenche
automatiquement** — tout repose sur la discipline au bon moment, qui est
justement ce qui lâche en plein rush.

Corollaire : ajouter des agents ne réglerait rien (un agent est un outil
*manuel*, même mode de défaillance). Ce qui transforme une régression
*silencieuse* en régression *bruyante au commit qui la cause*, c'est
**l'automatisation**.

### Pièges identifiés dans l'existant

- **`sdkconfig` partagé** à la racine entre les 6 builds. Le commentaire CMake
  lui-même avertit : *« rm sdkconfig first or the previous board's settings
  will leak in »*. Construire les 6 boards à la chaîne sans isolation produit
  des configs corrompues → source directe de régressions silencieuses
  config/build.
- **CI** : 3 boards seulement (V1/V2/V2D), déclenchée sur `main`/`develop`/PR.
  Zéro couverture sur la branche de travail (`dongle-firmware`) et sur les
  boards `dongle`/`half_left`/`half_right`.

## Boards (état réel)

`kase_v1`, `kase_v2`, `kase_v2_debug`, `kase_dongle`, `kase_half_left`,
`kase_half_right` — 6 au total. Sélection via `idf.py -DBOARD=<name>`.
Per-board defaults : `sdkconfig.defaults.<short>` (déjà présents pour
dongle/half).

## Solution retenue — Hybride (automatisation d'abord)

### §1 — Tripwire local : `scripts/check.sh`

Une seule commande. Deux phases, *fail-fast*, sortie non-zéro au premier rouge,
résumé coloré.

- **Phase rapide (~secondes)** : build + run des tests host (`test/`).
  Couvre la logique pure.
- **Phase lente (~minutes)** : build des **6 boards**, chacun avec son
  **`sdkconfig` isolé** :
  ```
  idf.py -B build_<board> -DBOARD=<board> -DSDKCONFIG=build_<board>/sdkconfig build
  ```
  Chaque board a son propre `sdkconfig` dans son dossier build → **supprime
  définitivement le piège du sdkconfig partagé**. Couvre compile + config
  cross-board.

Le script est la brique réutilisée par le hook git et la CI (source unique de
vérité du « quoi vérifier »).

### §2 — Hook git `pre-push`

- Hooks **versionnés** dans `scripts/hooks/`, activés en une ligne :
  `git config core.hooksPath scripts/hooks` (documenté dans CLAUDE.md).
- `pre-push` → lance `scripts/check.sh`. Rouge = push bloqué. C'est ce qui
  rend la régression bruyante au commit qui la cause, indépendamment de l'outil
  (humain ou Claude Code).
- Échappatoire WIP documentée : `git push --no-verify`.

### §2b — Hooks Claude Code (settings.json)

Garde-fou *pendant* la session, exécuté par la harness (ne dépend ni de la
discipline de l'utilisateur ni de celle de Claude). Étagement
fréquence × coût :

| Quand | Quoi | Coût | Attrape |
|---|---|---|---|
| `PostToolUse` (Edit/Write sur `main/**/*.c`, `boards/**`, `test/**`) | tests host seulement | ~secondes | régressions logique pure, instantané |
| `Stop` (fin de tâche) | tests host + build du **board courant** | ~minutes, 1× par tâche | casses compile/config avant « c'est fait » |

- **Board courant** : détecté via un fichier marqueur `.kase-board` à la racine
  (ou variable d'env `KASE_BOARD`), défaut `kase_v2_debug` (le défaut CMake).
  Mis à jour quand on change de board.
- Le build 6 boards complet reste sur `pre-push` (git) et la CI — pas dans la
  boucle de session.
- Décision explicite : **pas** de build à chaque Edit (coût minutes × N
  éditions → garde-fou abandonné par exaspération ; le hook Stop attrape les
  mêmes casses « assez vite »).

### §3 — CI étendue

- Matrice passée à **6 boards** (+ dongle/half_left/half_right), même isolation
  `sdkconfig` qu'en local.
- Déclenchée sur **toutes les branches** (plus seulement `main`/`develop`) →
  passage vert→rouge visible par commit sur la branche de travail réelle.
- Les entrées dongle/half n'ont de sens que sur les branches qui ont ces
  boards : on étend la matrice **dans le même commit/PR** qui mergera ces
  boards vers `main`, pour garder `main` vert.

### §4 — TDD léger sur la nouvelle logique pure

- Pas de backfill massif. **Norme** : toute nouvelle fonction de logique pure
  (keymap, layers, combo, tap-hold, parsing CDC, encoding keycodes…) → un test
  host **écrit d'abord**, ajouté à `test/CMakeLists.txt`.
- Déclencheur défini de l'agent `kase-test-author`.

### §5 — Checklist smoke-test hardware : `docs/HARDWARE_SMOKE_TEST.md`

- **Une page**, cases à cocher par sous-système que les tests host **ne peuvent
  pas** attraper : lien RF s'établit, eink affiche le dashboard sans
  corruption, BLE pair, USB HID tape, scan de toutes les touches, LED (V1).
- Organisée par board (les 6).
- Lancée **avant chaque release / merge vers `main`** — pas à chaque commit.

### §6 — Élagage + déclencheurs des agents

Le tripwire **automatique** s'occupe du gardiennage routinier ; les agents
servent au **non-routinier** (on ne les force pas dans la boucle, même faille
que « lancer les tests quand on y pense »). On définit *quand* invoquer les 3
qui comptent, documenté dans CLAUDE.md :

- `kase-firmware-debugger` → quand on colle un backtrace / boot loop.
- `kase-test-author` → quand on ajoute de la logique pure (cf. §4).
- `kase-code-reviewer` → avant un merge / release.

Les 5 autres (`cdc-protocol`, `board-variant`, `release-manager`, `maintainer`,
`security-auditor`) → à la demande ponctuelle, hors flux quotidien.

## Ordre d'implémentation

L'automatique d'abord (ce qui sauve), puis les compléments humains :

1. §1 `scripts/check.sh` (brique de base, isolation sdkconfig)
2. §2 hook git `pre-push` + `core.hooksPath`
3. §2b hooks Claude Code (`PostToolUse`, `Stop`)
4. §3 CI 6 boards / toutes branches
5. §5 checklist hardware
6. §4 norme TDD + §6 déclencheurs agents (doc CLAUDE.md)

## Critères de succès

- `scripts/check.sh` build les 6 boards sans fuite sdkconfig et fait passer les
  tests host, en une commande, sortie non-zéro si rouge.
- Un `git push` avec un board cassé ou un test rouge est **bloqué** localement.
- La CI passe au rouge sur le commit qui casse, sur la branche de travail.
- Une régression de logique pure est signalée **dans la session**, au moment de
  l'édition (PostToolUse), pas des jours après.
- Les agents ont des déclencheurs écrits dans CLAUDE.md.

## Hors scope (YAGNI)

- Pas de hardware-in-the-loop / test automatisé sur board réelle.
- Pas de backfill exhaustif de la couverture de tests existante.
- Pas de nouveaux agents.
- Pas de refactor des modules couplés (traité au cas par cas si une régression
  le justifie).
