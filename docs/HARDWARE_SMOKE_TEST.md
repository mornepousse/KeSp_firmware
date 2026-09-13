# Smoke-test hardware KaSe

À cocher AVANT chaque release / merge vers `main`. Couvre le runtime que les
tests host ne peuvent pas attraper. Flasher le board, cocher, garder une trace
dans la PR/release.

## Commun (tous boards)
- [ ] Boot sans boot loop (pas de Guru Meditation en boucle)
- [ ] USB HID : toutes les touches tapent le bon keycode (layer base)
- [ ] Layers MO/TO/LT/MT : bascule et retour OK
- [ ] Scan : aucune touche fantôme, aucune touche morte
- [ ] NVS préservée après reflash app-only (keymaps/macros intacts)

## V1 (round display + LED)
- [ ] Écran rond GC9A01 affiche sans corruption
- [ ] LED strip : animation par défaut OK
- [ ] Aucune trace de tamagotchi (retiré en v4.1.0) : pas de pet, pas de barres

## V2 / V2D (OLED I2C)
- [ ] OLED SSD1306 affiche sans artefact
- [ ] V2D : COLS7/8 (GPIO21/4) scannent correctement

## OLED refonte multi-écrans (V2 / V2D) — tamagotchi retiré
- [ ] Boot : splash « KaSe » + version ~2s, puis HOME
- [ ] Splash UNIQUEMENT au boot : éteindre/rallumer l'écran (veille→réveil) → PAS de splash, retour direct sur HOME
- [ ] HOME : barre de statut (connexion USB/BLE/RF + slot + CAP) ; **nom de couche en grand** (font_28) + « layer N » dessous
- [ ] Changement de couche → HOME met à jour le nom (PLUS d'écran LAYER plein écran)
- [ ] Rien ne déborde / ne se chevauche (nom de couche long → tronqué …)
- [ ] Touche K_DISP_NEXT (0x3F00, à mapper) : cycle HOME → STATS → HOME
- [ ] STATS : KPM/WPM bougent en tapant, sparkline se remplit, total croît
- [ ] V2D : écran s'éteint à ~30s d'inactivité ; réveil à la 1ʳᵉ frappe
- [ ] Plus AUCun tama : pas de pet, pas d'écran TAMA, pas de barres faim/joie nulle part (V1 round inclus)
- [ ] Pas de scintillement / rebuild en boucle entre HOME et STATS
- [ ] Presser une touche BT (switch/pair) pendant que HOME s'affiche → pas de crash, l'écran se reconstruit

## Dongle
- [ ] Lien RF s'établit avec une half (pairing < 120s)
- [ ] NRF ne se wedge pas après 5 min (watchdog OK)
- [ ] Une nuit sur batterie : la moitié droite tient, moins de 0,2 V perdus
- [ ] set_id survit à un erase_flash
- [ ] Fusion — Divergence de config signalée : keymap du dongle ≠ celle de la
      gauche → console dongle logue « DIVERGENCE de config » et
      KS_CMD_CONFIG_COHERENCE renvoie match=0 ; keymaps identiques → match=1

## Half (left / right)
- [ ] e-ink affiche le splash 'PAIRED' au pairing
- [ ] Dashboard e-ink : L/R/USB + batterie, sans corruption
- [ ] Trackpad (si présent) : curseur, clic G/D/M, scroll
- [ ] BLE pairing OK + reconnexion après deep sleep
- [ ] Power Phase 1 : frappe reste instantanée (scan/TX inchangés)
- [ ] Power Phase 1 : après ~3 s sans frappe, le heartbeat ralentit (console half : TX heartbeat espacée) sans perte de lien
- [ ] Power Phase 1 : reprise immédiate du heartbeat 100 ms à la première frappe
- [ ] Power Phase 2 : entre en light-sleep après ~15 s d'inactivité (console muette)
- [ ] Power Phase 2 : chute de courant mesurée (WiFi + NRF off) — noter la valeur
- [ ] Power Phase 2 : 1re frappe réveille + s'enregistre (latence acceptable), suivantes plein régime
- [ ] Power Phase 2 : e-ink lisible gelé pendant le sommeil, redevient live au réveil
- [ ] Power Phase 2 : aucune touche bloquée/fantôme au réveil ; relâchement géré
- [ ] Power Phase 2 : si le réveil ne déclenche pas sur touche, inverser la polarité GPIO colonnes (cf. half_scan_arm_key_wake BENCH-TUNE)
- [ ] Power Phase 2 : left + right indépendamment

## BLE (boards concernés)
- [ ] Appairage host OK, tape sans drop pendant 1 min
- [ ] Bascule de slot BT OK
