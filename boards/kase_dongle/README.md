# kase_dongle — KaSe Split Wireless Dongle

USB receiver for the KaSe split-wireless keyboard. Receives keystrokes from 2 halves
via NRF24L01+ and presents as USB HID composite + CDC binary to the host.

Hardware : `~/Documents/PCB-esp/dongle/dongle/` (KiCad 9 project, M.2 Key B 3042 form factor).

## Build & flash

```bash
source ~/esp/esp-idf/export.sh

# First build (or after switching boards) — sdkconfig is shared, may need rm
rm -f sdkconfig
idf.py -B build_dongle -DBOARD=kase_dongle -DIDF_TARGET=esp32s3 build

# Flash via CH340C on USB hub port 2
idf.py -B build_dongle -p /dev/ttyUSB0 flash
```

## Pinout (ESP32-S3-WROOM-2)

Extracted from `dongle.kicad_sch` netlist :

| Signal | GPIO | Notes |
|---|---|---|
| SPI MOSI | GPIO5 | shared, R15 100Ω série |
| SPI MISO | GPIO6 | shared, R16 100Ω série |
| SPI SCK | GPIO7 | shared, R17 100Ω série |
| NRF#1 (half_L) CSN / CE / IRQ | GPIO13 / GPIO14 / GPIO8 | |
| NRF#2 (half_R) CSN / CE / IRQ | GPIO1 / GPIO4 / GPIO2 | |
| USB D+ / D- | GPIO20 / GPIO19 | native OTG full-speed |
| Bootstrap IO0 | GPIO0 | flash mode trigger via CH340 RTS |

## Status

**Plan 1 (bring-up)** ✅ : board variant créé, USB CDC binaire actif au boot.
Hardware-vérifié : énumère `303a:4001`, CDC PING/FEATURES répondent.

**Plan 2 (NRF RX stack)** ✅ codé + bench-vérifié : les 2 radios NRF24 probent OK
sur le vrai dongle (SPI GPIO5/6/7, CSN 13/1, CE 14/4, IRQ 8/2), init canaux 76/82,
`rf_rx_task` démarre (L=1 R=1). Réception de paquets pas encore validée (besoin d'un
émetteur — TX tester sur le half, Task 8). `rf_packet` + `heartbeat` couverts par tests host.

**Plans suivants** :
- Plan 2 : NRF24L01+ stack + intégration engine (touche tester half → HID)
- Plan 3 : USB composite refinement (mouse, consumer, system reports)
- Plan 4 : CDC dongle commands (RF config, diagnostics)
- Plan 5 : ESP-NOW cold path (OTA halves, config push, telemetry)

Voir `docs/superpowers/plans/2026-05-11-dongle-plan-*.md`.

## Notes pour rebuilds entre boards

- `sdkconfig` est **partagé** entre tous les boards (V1/V2/V2D/dongle). Si tu switch de board,
  fais `rm sdkconfig` avant `idf.py reconfigure` pour repartir des bons defaults.
- L'IDF target doit être passé explicitement la 1ère fois après `rm sdkconfig` :
  `idf.py -DBOARD=... -DIDF_TARGET=esp32s3 build`
- Builds incrémentaux (sans `rm sdkconfig`) gardent les paramètres précédents.

## Issues pré-existantes (à fixer hors scope Plan 1)

- `sdkconfig.defaults` utilise des noms de symbols Kconfig BT obsolètes pour ESP-IDF 5.5.2
  (`BT_ENABLED`, `BT_BLUEDROID_ENABLED` non reconnus) → empêche un build clean V1/V2/V2D
  depuis un sdkconfig vide. Workaround : utiliser un sdkconfig préexistant qui a déjà
  les bonnes valeurs (via menuconfig précédent).
- Le dongle n'est pas affecté (pas de BT compilé).
