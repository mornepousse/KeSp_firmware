# Plan — fusion au dongle et deux moteurs

Design : `docs/superpowers/specs/2026-09-12-dongle-fusion-deux-moteurs-design.md`.
Chantier de plusieurs sessions. Règle d'or : **le clavier actuel doit marcher à
chaque étape** — tout derrière `KASE_DONGLE_FUSION` (défaut off), `check.sh` vert
en permanence.

## Fork tranché (2026-09-12)
La droite émet **toujours** vers le dongle (hub unique, ESB point à point
conservé). En mode USB-gauche, le dongle **réémet** la demi-matrice de la droite
à la gauche, et la gauche **annonce son mode USB au dongle** par radio (le dongle
ne voit pas l'énumération USB de la gauche autrement).

## Phase 0 — échafaudage non destructif  ✅ démarré
- Drapeau `KASE_DONGLE_FUSION` (défaut off). Rien ne change tant qu'il est off.
- Logique pure de fusion à deux demi-matrices : `fuse_halves()` (half_link.h),
  testée host (`test_fuse_halves.c`). Réutilise `half_col_to_keymap` (miroir PCB).

## Phase 1 — cœur, sans fil (le gain d'autonomie + fin du 10 s)
- La droite émet sa demi-matrice brute au dongle (slot dédié).
- La gauche, sur batterie, émet SA demi-matrice brute au dongle au lieu de
  fusionner et d'envoyer du HID fini — elle n'écoute plus.
- Le dongle : `drain_radio` alimente `fuse_halves` → `build_keycode_report` →
  HID local. Le moteur keymap est compilé dans le dongle.
- Test : frappe des deux moitiés via le moteur du dongle.

## Phase 2 — gauche autonome en USB
- Signal de mode gauche→dongle (USB présent).
- Dongle : en mode USB-gauche, réémet la matrice de la droite à la gauche et se
  tait (pas de HID). La gauche écoute (sur secteur, coût nul), moteur local, HID
  par son USB.
- Un seul moteur actif par route (réutilise usb_presence / kbd_active_route).

## Phase 3 — config et trackpad
- Config répliquée + **versionnée** dans les deux NVS ; le dongle refuse de
  tourner sur une version incohérente. Le contrôleur écrit les deux.
- Trackpad : la gauche envoie les gestes bruts, le moteur actif applique
  `trackpad_map` (réintroduit le parseur côté dongle).

## À réviser
`docs/superpowers/specs/2026-08-19-dongle-role-niphargus-design.md` (dongle « bête »).
