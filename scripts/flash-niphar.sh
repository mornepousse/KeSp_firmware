#!/usr/bin/env bash
# Flash une moitié du Niphargus EN VÉRIFIANT L'IDENTITÉ DE LA PUCE.
#
# Pourquoi ce script existe : les deux moitiés se programment par le MÊME
# adaptateur FTDI, qu'on déplace de l'une à l'autre. Rien, dans `idf.py flash`,
# ne dit sur laquelle il écrit — le port reste /dev/ttyUSB2 dans les deux cas.
# Le 2026-09-07, le firmware du MAÎTRE a été écrit sur le SCANNER : la moitié
# droite a cessé de scanner pour se mettre à écouter, et rien n'a protesté. Le
# clavier avait perdu une moitié, en silence.
#
# L'adresse MAC identifie la puce et ne dépend d'aucun câblage. On la lit avant
# d'écrire, et on refuse si elle ne correspond pas à la moitié demandée.
#
# Usage : ./scripts/flash-niphar.sh <left|right> [port]
set -euo pipefail

MAC_LEFT="d0:cf:13:21:92:60"    # relevée le 2026-09-07
MAC_RIGHT="80:b5:4e:eb:5e:08"   # relevée le 2026-09-07

moitie="${1:-}"
port="${2:-/dev/ttyUSB2}"

case "$moitie" in
  left)  attendue="$MAC_LEFT";  board=niphar_left  ;;
  right) attendue="$MAC_RIGHT"; board=niphar_right ;;
  *) echo "usage : $0 <left|right> [port]" >&2; exit 2 ;;
esac

if ! command -v esptool >/dev/null 2>&1; then
  echo "esptool absent du PATH — lancer depuis le devshell :" >&2
  echo "  nix develop ~/nixos-config#esp-idf" >&2
  exit 1
fi

lue="$(esptool --chip esp32s3 -p "$port" read_mac 2>/dev/null \
        | sed -n 's/^MAC:[[:space:]]*//p' | head -1)"

if [ -z "$lue" ]; then
  echo "aucune puce ne répond sur $port" >&2
  exit 1
fi

if [ "$lue" != "$attendue" ]; then
  echo "REFUS : la puce sur $port n'est pas la moitié « $moitie »." >&2
  echo "  attendue : $attendue" >&2
  echo "  lue      : $lue" >&2
  case "$lue" in
    "$MAC_LEFT")  echo "  → c'est la moitié GAUCHE qui est branchée." >&2 ;;
    "$MAC_RIGHT") echo "  → c'est la moitié DROITE qui est branchée." >&2 ;;
    *) echo "  → puce inconnue ; si la carte est neuve, ajouter sa MAC ici." >&2 ;;
  esac
  exit 1
fi

echo "puce confirmée : $moitie ($lue) — écriture de $board"
exec idf.py -B "build_$board" -p "$port" -b 460800 flash
