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
- [ ] Éveil oisif (DFS) : console au boot « DFS actif : 160 MHz en travail,
      40 MHz oisif » ; taper 1 min → « TX … acquittes » ≥ 98 % ; écran à jour ;
      console lisible au repos ; veille à 15 s et réveil sur touche inchangés ;
      multimètre en série : éveillée oisive ≈ 13-19 mA (contre 28-42 avant),
      endormie ≈ 250 µA ; balayage au repos arrêté : taper après une pause
      sort la première touche sans retard ni doublon, tenir une touche puis
      en presser une autre de la même rangée ne fantôme pas, une touche tenue
      au réveil ne se lit pas sur toute sa rangée ; dormir entre les touches :
      lien TRRS : gauche en USB + câble → console gauche « etat=2 5V=1 » et
      les ACK de la droite qui montent, « veille REFUSEE … vetos=usb+lien » ; câble
      retiré → « etat=0 5V=0 » en moins d'une seconde ; gauche en USB seule :
      « vetos=usb » et jamais de veille ; USB retiré : « vetos=- », veille à
      15 s, console « tache de veille : tick 1000 ms, 3 hook(s) » au boot ;
      au repos le HB de banc (CONFIG_PM_PROFILING=y) montre « light_sleep_counts » qui grimpe (~90 par
      10 s) et « light_sleep_reject_counts:0 », la frappe reste immédiate
- [ ] Une nuit sur batterie : une moitié perd de l'ordre du centième de volt ;
      0,2 V = elle n'a pas dormi. Console au matin : « HB … dormi=X s/n vetos=- » avec
      X ≈ la durée de la nuit, et « reveil apres N s de sommeil » cohérent ;
      après 4 h sans frappe : « sommeil profond » puis redémarrage à la
      première touche (réveil EXT1, ~700 ms)
- [ ] set_id survit à un erase_flash
- [ ] Fusion — moteur local dormant : gauche sur batterie (route RF), taper →
      le dongle tape et la console gauche ne montre AUCUN traitement local
      (pas de tap-hold/combo/HID) ; brancher un hôte USB sur la gauche → elle
      tape en local dès la frappe suivante, le dongle se tait ; débrancher →
      retour au brut (binaires issus de `build_niphar_left` /
      `build_niphar_right` / `build_kase_dongle` — pas d'un dossier `_fusion`)
- [ ] Fusion — Divergence de config signalée : keymap du dongle ≠ celle de la
      gauche → console dongle logue « DIVERGENCE de config » et
      KS_CMD_CONFIG_COHERENCE renvoie match=0 ; keymaps identiques → match=1
- [ ] Fusion — Canal retour ACK payload : le dongle charge une charge utile
      connue dans l'ACK (EN_ACK_PAY) ; la gauche, en sans-fil, la lit après
      chaque émission et la logue. Go/no-go des clones nRF24 : si RX_DR ne se
      lève jamais côté gauche, la sync auto par ACK est impossible → repli B
- [ ] Radio — un propriétaire : dongle DÉBRANCHÉ, taper sur la droite → console
      droite « repli : bascule TX -> GAUCHE KaSe.03 » (puis oscillation
      GAUCHE/DONGLE si personne n'écoute) ; dongle rebranché → les ACK
      reprennent sans reset (« TX n envois, m acquittes » ≥ 95 % cumulé) ;
      gauche en USB → « fusion USB : ecoute la droite reemise (PRX ch=0x4F
      KaSe.03) » et la droite tape par la gauche ; USB retiré → « retour
      emission PTX vers le dongle », les deux tapent par le dongle
- [ ] Fusion — Sync keymap sans câble : modifier une couche du dongle (SETLAYER)
      → gauche en sans-fil, RIEN branché d'autre : en < 15 s la console gauche
      logue « sync keymap : balise » puis « 40/40 recus … enregistree en NVS »,
      et KS_CMD_CONFIG_COHERENCE (0x17) repasse à match=1 sur la NOUVELLE
      empreinte ; ensuite plus aucun ACK payload au repos (balise coupée)

## Half (left / right)
- [ ] Batterie faible : builder une moitié avec `BATT_FAIBLE_DV`/`BATT_CRITIQUE_DV`
      décalés au-dessus de la tension réelle (banc, à ne pas commiter) → console
      « batterie : FAIBLE » puis, en critique, « light sleep » à ~5 s ; écran :
      tension / « BAT » en alternance 2 s, jauge lisible à bordure épaisse ;
      gauche en USB + TRRS → « etat=0 5V=0 », 0 sonde ; seuils réels reflashés
      → le lien remonte
- [ ] Jauge batterie : console au boot « batt: jauge : NN dV » avec NN plausible
      (36-42) et à ±0,1 V d'un voltmètre sur la batterie ; CDC BATTERY (dongle)
      donne les DEUX moitiés avec age frais (gauche ~1 s, droite ≤ 30 s) ; la
      droite s'endort toujours à 15 s malgré son STATUS lent ; une moitié éteinte
      repasse « inconnue » ; batterie en charge → après ≥ 2 min à ≥ 4,15 V,
      charging = 2 (PLEINE)
- [ ] Écrans memory-LCD (gauche ET droite) : au boot, console « panneau attache
      (bus radio pret), mire ecrite » APRÈS « radio PTX … init OK » ; mire
      nette sur tout le panneau (cadre 1 px, pavé plein 16×16 en HAUT-GAUCHE
      du portrait, damier 8×8 ailleurs), sans pixel parasite ; taper pendant un
      rafraîchissement ne perd aucune frappe ; en light sleep l'image reste
      figée et lisible, la carte dort toujours à 15 s ; au réveil l'écran revit
- [ ] Écrans memory-LCD UI : gauche → bandeau « RF ▲ » (ou « USB ») + jauge +
      tension, nom de couche en lignes de 4 (ex. « DVO / RAK »), « L0 » ;
      droite → même bandeau, logo Niphargus net et centré ; changer de couche
      (MO tenu) met le nom à jour en < 200 ms sans perdre de frappe ; droite
      servie à 1 s : aucune zone ne grise en 2 min (VCOM ~1 Hz), l'image revit
      dans la seconde qui suit la première touche après une veille
- [ ] Première touche après veille : laisser la moitié s'endormir (15 s sans
      toucher), taper UNE touche brève → le caractère sort (pas avalé) et rien
      ne reste collé ; console : « reveil : 1 touche(s) capturee(s) ». À faire
      sur la gauche ET la droite, en sans-fil (fusion)
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
