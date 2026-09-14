# Contrat matériel — Niphargus v2, moitiés S3

Source de vérité : netlist du schéma `Niphargus/rili/pcb/niphar.kicad_sch`, vérifiée
pin par pin lors de la revue du 2026-08-06 (commits `faf3e11`→`c1f9cf6`). En cas de
doute, la netlist prime sur ce document.

## MCU : ESP32-S3-WROOM-1(U)-N16R8 — un par moitié

> **Corrigé au bring-up du 2026-09-01.** Ce document annonçait un **N8R2**
> (8 MB flash / 2 MB PSRAM). La première moitié gauche mise sous tension répond
> `16 MB` de flash et `8 MB` de PSRAM embarquée à l'esptool (ESP32-S3 rev v0.2,
> MAC `d0:cf:13:21:92:60`) : le module posé est un **N16R8**. La moitié droite
> n'a pas encore été lue. La PSRAM reste désactivée côté firmware
> (aucun `CONFIG_SPIRAM=y`).

U6 = gauche (feuille `s3`), U5 = droite (feuille `right`). Nets de la droite suffixés `_d`/`_D`.

## Matrice — ⚠ DEUX tables distinctes (permutations de routage)

| ligne | GAUCHE (U6) | DROITE (U5) |
|---|---|---|
| row0 | GPIO1 | GPIO2 |
| row1 | GPIO2 | GPIO12 |
| row2 | GPIO8 | GPIO4 |
| row3 | GPIO6 | GPIO5 |
| col0 | GPIO4 | GPIO6 |
| col1 | GPIO5 | GPIO7 |
| col2 | GPIO7 | GPIO8 |
| col3 | GPIO9 | GPIO9 |
| col4 | GPIO10 | GPIO11 |
| col5 | GPIO11 | GPIO10 |
| col6 | GPIO12 | GPIO1 |

- Chaîne électrique : **COL → switch → anode-diode-cathode → ROW** (1N4148W).
  → le scan PILOTE les colonnes et LIT les rows.
- **Réveil sommeil profond : EXT1 sur les ROWS** (tous en GPIO RTC ✓).
- 100 Ω série sur chaque ligne côté MCU ; TVS SD05C côté switchs (transparent firmware).
- 26 touches par moitié (rangées de 7/7/6/6).

## Identifier les moitiés — adresse MAC

Les deux moitiés se programment par le **même adaptateur FTDI**, déplacé de
l'une à l'autre, et le port reste `/dev/ttyUSB2` dans les deux cas : rien dans
`idf.py flash` ne dit sur laquelle il écrit.

| Moitié | MAC (relevée le 2026-09-07) |
|---|---|
| gauche (maître) | `d0:cf:13:21:92:60` |
| droite (scanner) | `80:b5:4e:eb:5e:08` |

Le 2026-09-07, le firmware du maître a été écrit sur le scanner : la droite a
cessé de scanner pour se mettre à écouter, et **rien n'a protesté** — le clavier
avait perdu une moitié en silence. Flasher via `./scripts/flash-niphar.sh
<left|right>`, qui lit la MAC et refuse si elle ne correspond pas.

## Pins communs aux deux MCU

| Fonction | GPIO | Notes |
|---|---|---|
| VBAT_SENSE (jauge) | 13 | ADC2_CH2, diviseur 1M/1M + 100nF — **ADC2 interdit si WiFi actif** (nRF24-only : OK). Batterie pleine ≈ 4,15 V ÷ 2 |
| LCD_CS (`CS_DPL`) | 14 | écran Sharp, **actif HAUT** |
| nRF24 CE / CSN | 15 / 16 | |
| LINK_TX / LINK_RX (TRRS) | 17 / 18 | UART1. **Câble droit : TX arrive sur TX** → UNE moitié doit échanger TXD/RXD via la matrice GPIO. Ne jamais driver les deux TX sans ce swap |
| USB D− / D+ | 19 / 20 | natif |
| LINK_5V_EN | 21 | ON du SiP32431 (pull-down 100k = 5 V mort par défaut). Poignée de main : l'émetteur ET le récepteur doivent activer leur switch pour transférer du 5 V ; une moitié à batterie morte n'est pas réveillable par le TRRS (assumé) |
| SPI partagé SCK / MISO / MOSI | 38 / 39 / 40 | nRF24 + écran (écran write-only) + **ESP32-P4 (Niphar_chest)** — voir ci-dessous |
| nRF24 IRQ | 41 | |
| TP_RDY | 42 | trackpad, gauche seulement (labels en attente à droite) |
| I2C trackpad SDA / SCL | 47 / 48 | pull-ups 4,7k ; gauche seulement. NRST du trackpad = RC matériel, pas de GPIO |
| Prog | 0, 43 (TX0), 44 (RX0) | connecteur 6 pins type ESP-Prog par moitié (EN/3V3/TX/GND/RX/IO0) |
| Interdits | 3, 45, 46, 35-37 | strapping / PSRAM octale — non câblés |

## ⚠ Le bus SPI porte un troisième participant : l'ESP32-P4

> **Ajouté le 2026-09-05, après un diagnostic d'une heure.**

Une carte **ESP32-P4** (projet Niphar_chest) est raccordée au bus SPI partagé de
la moitié gauche — SCK/MISO/MOSI sur GPIO 38/39/40. Ce document ne la mentionnait
nulle part, et son absence a coûté cher : un P4 **non programmé** tient ces trois
lignes, la nRF24 devient muette, et le diagnostic accuse successivement le module
radio, un court-circuit mécanique, puis la PSRAM octale avant de trouver la cause.

Signature du défaut, si ça se reproduit : les **trois lignes partagées** lisent
`CLOUÉE À LA MASSE` au test de lignes (`CONFIG_KASE_NRF_PROBE`), pendant que
**CSN, CE et IRQ restent libres** — ces trois-là sont propres à la radio, le P4
n'y touche pas. Aucun pont entre elles : chacune est tirée séparément, par le
même composant à l'autre bout.

Conséquences de conception, non traitées à ce jour :

- **Trois esclaves sur un bus, chacun son CS.** Le P4 doit impérativement
  relâcher MISO hors sélection. Un esclave qui garde MISO en sortie tient le bus
  même en fonctionnant parfaitement.
- **Arbitrage.** Rien n'ordonne aujourd'hui les accès entre la nRF24, l'écran et
  le P4.
- Le P4 doit être flashé **avant** tout test radio sur la gauche.

## Périphériques

- **Écran (droite)** : module type nice!view (Sharp LS011B7DH03) sur J4 5 broches :
  MOSI/SCK/3V3/GND/CS. LSB-first, CS actif haut, VCOM logiciel à basculer (EXTCOMIN
  géré par le module). Connecteur miroir J12 côté gauche non peuplé (populate-per-half).
- **Trackpad (gauche)** : Azoteq TPS43 (IQS572) en I2C + RDY obligatoire (handshake).
- **nRF24L01+** : modules breakout 2×4, alim 3,3 V, 100 Ω série sur les 6 signaux.

## Alimentation (par moitié)

- Li-ion 16340 → DW01A+FS8205 → interrupteur (chemin batterie seul) → **HT7833** (500 mA,
  4 µA IQ) → 3,3 V. USB 5 V → SS14 → même nœud (l'USB contourne l'interrupteur).
- Charge TP4056 ~500 mA ; CHRG = LED ; STDBY non câblé → fin de charge par ADC.
  **Jauge implémentée le 2026-09-14** (`power/batt_sense`) : ADC2_CH2 calibré,
  8 lectures / 10 s + au réveil, 4,10 V lus contre 4,08 V au voltmètre. « Fin de
  charge par ADC » = plateau ≥ 4,15 V tenu 2 min (déduit, faute de STDBY/VBUS).
- Load-sharing AO3407 : USB présent = batterie isolée.
- Hub CH334R (gauche) : mode sans quartz, alimenté par le 5 V USB uniquement —
  **il n'existe pas sur batterie** (et le port 4 est non câblé).

## Firmware : exigences non négociables

1. **Swap TX/RX** d'une moitié avant toute UART TRRS.
2. **Watchdog** + récupération brownout : reset propre ≤ 200 ms (héritage : les v1
   plantaient à l'ESD — le matériel est durci, le firmware doit finir le travail).
3. Budget sommeil : viser < 50 µA par moitié, radio en power-down, scan RTC.
4. Deux profils de pins (tables ci-dessus) sélectionnés à la compilation ou par
   détection (ex. présence trackpad sur le bus I2C = moitié gauche).

---

## Intégration KeSp_firmware (à faire)

Créer les définitions de board `boards/niphargus_half_left` et `boards/niphargus_half_right`
sur le modèle de `kase_half_left`/`kase_half_right`, avec les deux tables de pins ci-dessus.
Différences notables vs KaSe : matrice 4×7 (26 touches) en domaine RTC avec réveil EXT1
sur les rows, radio nRF24L01+ sur SPI partagé avec l'écran (CS écran GPIO14 actif haut),
lien filaire TRRS UART1 (swap TX/RX sur une moitié), jauge ADC2_CH2.
